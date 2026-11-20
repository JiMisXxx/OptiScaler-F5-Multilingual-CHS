// Superior Scaling's private DLSS on native Vulkan (CNN D/E and Transformer K), the Vulkan side of
// shaders/dlssnr/superior/Pipeline.inl (Stabilize, CreateTemporal), PrivateEffects.inl (the ghost field) and the call
// sites in DlssNr_Dx12.cpp. Included inside DlssNrFeature_Vk.cpp's anonymous namespace after DlssNr_Superior_Vk.inl;
// SuperiorVk::Compose is what EvaluateAtSeamVk calls, all under g_vkMutex.
//
// The private DLSS is DLAA at the model's raster (in = out = work size), fed a carrier (0.5 + answer - proxy) and the
// guides Superior matched to that raster, and its output decoded back to a signed residual that the resolve composes.
// While it is being made, and whenever it cannot run, the raw answer goes through the No DLSS composition as before.
//
// What Vulkan does differently from D3D12, and why:
//   - Creation is recorded on the NR command buffer and a VkEvent set behind it tells when the GPU has run it (D3D12:
//     the submission epoch); the feature is evaluated only after that.
//   - No GPU lifetime tracker: a preset change, a new raster or the end of the pass drains the device and releases the
//     feature at once (D3D12 waits for the old one to retire).
//   - The NGX Vulkan runtime shuts down AFTER us (ShutdownVkForDevice runs before the core goes), so the feature is
//     released there with the core alive; on a lost device the handles are only forgotten. NGX is never called when its
//     core is gone.
//   - The CNN's resident E and its motion probe (D3D12 swaps D for E while the picture is still) are not ported: D runs
//     alone, which is what D3D12 runs while the probe has not seen a still picture.
//   - The ghost field's state images are RGBA32 UINT / RGBA32F instead of R32G32 (not a storage format every device
//     offers), declared by the shaders as they were written.
namespace SuperiorVk
{
namespace Private
{
namespace Pol = DlssNr::SuperiorPrivate;
namespace Sup = DlssNr::SuperiorScaling;
using Constants = DlssNr::Superior::DlssNrConstants;
namespace Fields = DlssNr::Superior::Fields;
using Blob = DlssNrSuperior_Vk::Blob;
using Reads = DlssNrSuperior_Vk::Reads;

// What EvaluateAtSeamVk knows about the frame and the game's side of it.
struct Frame
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    const DlssNr::GuideRegions* guides = nullptr;
    float mvX = 1.f, mvY = 1.f; // the matched guides' vector scale (model pixels)
    float frameMs = 0.f;        // the game's FrameTimeDeltaInMsec, 0 when it gave none
    bool reset = false;         // the model's history was reset this frame
    bool depthInverted = false;
    bool haveDepth = false;
};

// Frames to wait for the creation to have run before giving up on it.
constexpr unsigned kCreationWaitFrames = 600;
// Evaluation failures in a row before this generation stops trying.
constexpr unsigned kFailuresAllowed = 3;

enum class Phase
{
    None,
    Creating,
    Ready,
    Failed // this generation runs without a private DLSS (the next raster or selection tries again)
};

Phase phase = Phase::None;
NVSDK_NGX_Parameter* params = nullptr;
NVSDK_NGX_Handle* feature = nullptr;
VkEvent created = VK_NULL_HANDLE;
unsigned waited = 0, failures = 0;
unsigned wantedPreset = 0, actualPreset = 0;
uint32_t builtW = 0, builtH = 0;
bool builtInverted = false;
bool srUnavailable = false; // no usable NGX DLSS SR here: not tried again until Superior is selected again
bool resetNext = true;      // this generation's history starts over (the DLSS Reset flag, the ghost history)
Sup::PrivatePresetChoice choice;
VkDevice ownerDevice = VK_NULL_HANDLE;

OwnedImage carrier, temporalOutput, temporalMotion, unit;

struct Ghost
{
    OwnedImage reconstructed, state[2], raw[2], reference[2], field, expanded, rings, bias;
    unsigned index = 0;
    bool valid = false; // the last built frame's state is there to compare with
    uint32_t madeFor[2] = {};
    bool Holds() const { return reconstructed.Valid() || field.Valid() || bias.Valid(); }
};
Ghost ghost;
bool ghostFormatsChecked = false, ghostFormatsOk = false;

void Say(const std::string& text)
{
    std::lock_guard<std::mutex> lock(statusLock);
    privateStatus = text;
}

bool NgxUsable()
{
    return NVNGXProxy::IsVulkanInited() && NVNGXProxy::VULKAN_AllocateParameters() != nullptr &&
           NVNGXProxy::VULKAN_CreateFeature() != nullptr && NVNGXProxy::VULKAN_EvaluateFeature() != nullptr &&
           NVNGXProxy::VULKAN_ReleaseFeature() != nullptr && NVNGXProxy::VULKAN_DestroyParameters() != nullptr;
}

Reads::value_type Rd(const OwnedImage& img) { return { img.view, img.layout }; }

void DestroyGhost()
{
    for (OwnedImage* img : { &ghost.reconstructed, &ghost.state[0], &ghost.state[1], &ghost.raw[0], &ghost.raw[1],
                             &ghost.reference[0], &ghost.reference[1], &ghost.field, &ghost.expanded, &ghost.rings,
                             &ghost.bias })
        DestroyImage(*img);
    ghost.valid = false;
    ghost.index = 0;
    ghost.madeFor[0] = ghost.madeFor[1] = 0;
}

void DestroyImages()
{
    for (OwnedImage* img : { &carrier, &temporalOutput, &temporalMotion, &unit })
        DestroyImage(*img);
    DestroyGhost();
    builtW = builtH = 0;
}

// Releases the NGX objects. The device must be idle. Only when the NGX Vulkan core is still up (never after it).
void ReleaseNgx()
{
    if (feature != nullptr && NVNGXProxy::IsVulkanInited() && NVNGXProxy::VULKAN_ReleaseFeature() != nullptr)
        NVNGXProxy::VULKAN_ReleaseFeature()(feature);
    if (params != nullptr && NVNGXProxy::IsVulkanInited() && NVNGXProxy::VULKAN_DestroyParameters() != nullptr)
        NVNGXProxy::VULKAN_DestroyParameters()(params);
    feature = nullptr;
    params = nullptr;
}

bool Holds() { return feature != nullptr || params != nullptr || carrier.Valid() || ghost.Holds(); }

// A preset change, a new raster or the end of the pass: the device is drained, then the feature and the images go.
void Rebuild(VkDevice device)
{
    if (Holds() && device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);

    ReleaseNgx();
    DestroyImages();
    if (created != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
    {
        vkDestroyEvent(device, created, nullptr);
        created = VK_NULL_HANDLE;
    }
    phase = Phase::None;
    waited = failures = 0;
    resetNext = true;
}

// Superior Scaling selected again: no DLSS SR here, or a creation that failed, is tried afresh.
void Reselected(VkDevice device)
{
    srUnavailable = false;
    if (phase == Phase::Failed)
        Rebuild(device);
}

// Shutdown (ReleaseNrObjects / ShutdownVk): the device is idle and the NGX core still up, or the device is gone.
void Release(bool deviceAlive)
{
    if (deviceAlive)
    {
        Rebuild(g_vk.device);
    }
    else
    {
        // The device took its images, its event and every NGX handle with it.
        feature = nullptr;
        params = nullptr;
        for (OwnedImage* img : { &carrier, &temporalOutput, &temporalMotion, &unit, &ghost.reconstructed,
                                 &ghost.state[0], &ghost.state[1], &ghost.raw[0], &ghost.raw[1], &ghost.reference[0],
                                 &ghost.reference[1], &ghost.field, &ghost.expanded, &ghost.rings, &ghost.bias })
            *img = OwnedImage {};
        ghost.valid = false;
        ghost.madeFor[0] = ghost.madeFor[1] = 0;
        builtW = builtH = 0;
        created = VK_NULL_HANDLE;
        phase = Phase::None;
        waited = failures = 0;
        resetNext = true;
    }

    choice = {};
    srUnavailable = false;
    Say("");
}

// D3D12's creation parameters (Pipeline.inl SetCreationParameters + CreateFeature).
void SetCreationParameters(NVSDK_NGX_Parameter* p, uint32_t w, uint32_t h, bool inverted)
{
    p->Set(NVSDK_NGX_Parameter_Width, w);
    p->Set(NVSDK_NGX_Parameter_Height, h);
    p->Set(NVSDK_NGX_Parameter_OutWidth, w);
    p->Set(NVSDK_NGX_Parameter_OutHeight, h);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                           NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                           (inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
}

NVSDK_NGX_Result CreateFeature(VkCommandBuffer cmd, unsigned preset)
{
    for (const char* key :
         { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        params->Set(key, preset);
    feature = nullptr;
    return NVNGXProxy::VULKAN_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, params, &feature);
}

// Whether the images this needs can be storage images on this device (the field and the bias mask are R16F).
bool GhostFormatsOk(VkPhysicalDevice physicalDevice)
{
    if (!ghostFormatsChecked)
    {
        ghostFormatsChecked = true;
        const auto storage = [&](VkFormat format, bool filtered)
        {
            VkFormatProperties props {};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              (filtered ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0u);
            return (props.optimalTilingFeatures & need) == need;
        };
        // The field is read with the linear sampler (the decode); the integer images are only loaded.
        ghostFormatsOk = storage(VK_FORMAT_R16_SFLOAT, true) && storage(VK_FORMAT_R32_UINT, false) &&
                         storage(VK_FORMAT_R32G32B32A32_UINT, false);
        if (!ghostFormatsOk)
            LOG_WARN("DLSS-NR Vulkan Superior Scaling: this GPU cannot use the Transformer's disocclusion field formats; "
                     "the Transformer runs without it");
    }
    return ghostFormatsOk;
}

// 1x1 R32F unit exposure for the DLSS (Pipeline.inl:495-501: Mode 7 of the generic v59 pass writes 1.0).
bool UnitExposure(VkCommandBuffer cmd)
{
    Constants c {};
    c.Mode = DlssNr::Superior::DlssNrMode_UnitExposure;
    c.Width = c.Height = 1u;
    Transition(cmd, unit, VK_IMAGE_LAYOUT_GENERAL);
    Reads reads {};
    if (!pass->Dispatch(cmd, Blob::Generic, &c, sizeof(c), 1, 1, reads, unit.view))
        return false;
    Transition(cmd, unit, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Makes the images and the feature on this command buffer and sets the event behind them. False: this generation runs
// without a private DLSS (the reason is said and logged); srUnavailable is set where no DLSS SR exists here at all.
bool Create(VkCommandBuffer cmd, const Frame& in, unsigned preset)
{
    const uint32_t w = depth.width, h = depth.height;

    if (!NVNGXProxy::IsVulkanInited())
    {
        // The game's own DLSS may not be the upscaler in use; the proxy's loader does what DLSSFeatureVk does.
        NVNGXProxy::InitVulkan(in.instance, in.physicalDevice, in.device, ::vkGetInstanceProcAddr,
                               ::vkGetDeviceProcAddr);
    }

    if (!NgxUsable())
    {
        srUnavailable = true;
        phase = Phase::Failed;
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: no usable NVIDIA DLSS runtime for the private DLSS; running No DLSS "
                 "(select Superior Scaling again to retry)");
        Say("NVIDIA DLSS is not available here. Using the current NR edit.");
        return false;
    }

    if (!CreateImage(carrier, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, true) ||
        !CreateImage(temporalOutput, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, true) ||
        !CreateImage(temporalMotion, w, h, VK_FORMAT_R32G32B32A32_SFLOAT, true) ||
        !CreateImage(unit, 1, 1, VK_FORMAT_R32_SFLOAT, true))
    {
        DestroyImages();
        phase = Phase::Failed;
        Say("Private DLSS could not be created. Using the current NR edit.");
        return false;
    }
    builtW = w;
    builtH = h;

    if (created == VK_NULL_HANDLE)
    {
        VkEventCreateInfo info { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
        if (vkCreateEvent(in.device, &info, nullptr, &created) != VK_SUCCESS)
        {
            created = VK_NULL_HANDLE;
            phase = Phase::Failed;
            Say("Private DLSS could not be created. Using the current NR edit.");
            return false;
        }
    }
    else
        vkResetEvent(in.device, created);

    if (!UnitExposure(cmd))
    {
        phase = Phase::Failed;
        Say("Private DLSS could not be created. Using the current NR edit.");
        return false;
    }

    NVSDK_NGX_Result result = NVNGXProxy::VULKAN_AllocateParameters()(&params);
    if (result != NVSDK_NGX_Result_Success || params == nullptr)
    {
        params = nullptr;
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: the private DLSS got no NGX parameters ({:X})", (unsigned) result);
        phase = Phase::Failed;
        Say("Private DLSS could not be created. Using the current NR edit.");
        return false;
    }

    SetCreationParameters(params, w, h, in.depthInverted);
    result = CreateFeature(cmd, preset);
    actualPreset = preset;

    if (result != NVSDK_NGX_Result_Success || feature == nullptr)
    {
        // A runtime without the requested preset (newer 310.x DLLs dropped the CNN presets A-E): its default.
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: private DLSS preset {} unavailable ({:X}); using the runtime's default "
                 "preset", preset, (unsigned) result);
        actualPreset = 0;
        result = CreateFeature(cmd, NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    }

    switch (Pol::ClassifyCreate((unsigned) result, feature != nullptr))
    {
    case Pol::CreateVerdict::Ok:
        break;
    case Pol::CreateVerdict::Session:
        srUnavailable = true;
        [[fallthrough]];
    default:
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: private DLSS creation failed (NGX result {:X}){}", (unsigned) result,
                 srUnavailable ? "; not tried again this session" : "");
        ReleaseNgx();
        phase = Phase::Failed;
        Say(srUnavailable ? "NVIDIA DLSS is not available here. Using the current NR edit."
                          : "Private DLSS could not be created. Using the current NR edit.");
        return false;
    }

    // The event says the GPU has run the creation (D3D12: a later submission epoch).
    vkCmdSetEvent(cmd, created, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    wantedPreset = preset;
    waited = 0;
    phase = Phase::Creating;
    LOG_INFO("DLSS-NR Vulkan Superior Scaling: private DLSS preset {} created ({}x{}), waiting for the GPU to run it",
             actualPreset, w, h);
    Say("Creating the private DLSS. Using the current NR edit.");
    return true;
}

// ---------------------------------------------------------------------------------------------
// The Transformer's disocclusion field (PrivateEffects.inl UpdateGhostHistory, GhostBiasMask, RejectTransformerGhosts)
// ---------------------------------------------------------------------------------------------

bool EnsureGhost(unsigned workW, unsigned workH)
{
    if (ghost.reconstructed.Valid() && ghost.madeFor[0] == workW && ghost.madeFor[1] == workH)
        return true;

    DestroyGhost();
    const unsigned qw = (workW + 3) / 4, qh = (workH + 3) / 4;
    bool ok = CreateImage(ghost.reconstructed, qw, qh, VK_FORMAT_R32_UINT, true) &&
              CreateImage(ghost.field, qw, qh, VK_FORMAT_R16_SFLOAT, true) &&
              CreateImage(ghost.expanded, qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, true) &&
              CreateImage(ghost.rings, qw, qh, VK_FORMAT_R32G32B32A32_SFLOAT, true) &&
              CreateImage(ghost.bias, workW, workH, VK_FORMAT_R16_SFLOAT, true);
    for (unsigned i = 0; i < 2 && ok; ++i)
        ok = CreateImage(ghost.state[i], qw, qh, VK_FORMAT_R32G32B32A32_UINT, true) &&
             CreateImage(ghost.raw[i], qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, true) &&
             CreateImage(ghost.reference[i], qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, true);

    if (!ok)
    {
        DestroyGhost();
        return false;
    }
    ghost.madeFor[0] = workW;
    ghost.madeFor[1] = workH;
    return true;
}

uint32_t Groups(uint32_t n, uint32_t per) { return (n + per - 1) / per; }

// Builds this frame's field from the matched depth and the private motion (it needs `previous` frame's state, which sits
// on the GPU in order; a reset frame starts from scratch). False: no field this frame (the DLSS then runs without a mask
// and without the repair).
bool UpdateGhost(VkCommandBuffer cmd, const Frame& in, OwnedImage& proxy, OwnedImage& answer, unsigned workW,
                 unsigned workH, bool reset)
{
    if (!GhostFormatsOk(in.physicalDevice) || !EnsureGhost(workW, workH))
        return false;

    Constants c {};
    const bool previous = ghost.valid && !reset;
    Fields::SetPrivateGhost(c, workW, workH, in.depthInverted, previous, depthPrecision);
    const uint32_t qw = c.Width, qh = c.Height;
    const uint32_t gx = Groups(qw, 8), gy = Groups(qh, 8);

    for (OwnedImage* img : { &ghost.reconstructed, &ghost.state[0], &ghost.state[1], &ghost.raw[0], &ghost.raw[1],
                             &ghost.reference[0], &ghost.reference[1], &ghost.field, &ghost.expanded })
        Transition(cmd, *img, VK_IMAGE_LAYOUT_GENERAL);

    const unsigned next = previous ? ghost.index ^ 1u : 0u;
    OwnedImage& old = ghost.state[previous ? ghost.index : 1];
    ghost.valid = false;

    // New images hold nothing (D3D12 resources start zeroed, Vulkan ones do not), so the depth scratch is cleared every
    // frame, not only after a previous one.
    Reads reads {};
    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostClear;
    reads[0] = Rd(depth);
    if (!pass->Dispatch(cmd, Blob::GhostClear, &c, sizeof(c), gx, gy, reads, ghost.reconstructed.view))
        return false;

    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostScatter;
    reads = {};
    reads[0] = Rd(depth);
    reads[1] = Rd(temporalMotion);
    reads[2] = Rd(old);
    if (!pass->Dispatch(cmd, Blob::GhostCoarse1, &c, sizeof(c), gx, gy, reads, ghost.reconstructed.view,
                        ghost.state[next].view))
        return false;

    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostAge;
    reads[3] = Rd(ghost.reconstructed);
    if (!pass->Dispatch(cmd, Blob::GhostCoarse2, &c, sizeof(c), gx, gy, reads, ghost.field.view,
                        ghost.state[next].view))
        return false;

    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostReference;
    reads = {};
    reads[0] = Rd(ghost.state[next]);
    reads[1] = previous ? Rd(ghost.raw[ghost.index]) : Rd(proxy);
    reads[2] = previous ? Rd(ghost.reference[ghost.index]) : Rd(proxy);
    reads[3] = Rd(proxy);
    reads[4] = Rd(answer);
    reads[5] = Rd(ghost.field);
    if (!pass->Dispatch(cmd, Blob::GhostReference, &c, sizeof(c), gx, gy, reads, ghost.raw[next].view,
                        ghost.reference[next].view))
        return false;

    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostDilate;
    reads = {};
    reads[0] = Rd(ghost.state[next]);
    reads[1] = Rd(ghost.reference[next]);
    if (!pass->Dispatch(cmd, Blob::GhostDilate, &c, sizeof(c), gx, gy, reads, ghost.field.view, ghost.expanded.view))
        return false;

    ghost.index = next;
    ghost.valid = true;
    return true;
}

// The field at the working raster, for the DLSS's bias-current-colour input. Null when there is no field this frame.
OwnedImage* BiasMask(VkCommandBuffer cmd, const Frame& in, OwnedImage& proxy, OwnedImage& answer, unsigned workW,
                     unsigned workH, bool reset)
{
    if (!UpdateGhost(cmd, in, proxy, answer, workW, workH, reset))
        return nullptr;

    Constants c {};
    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostBias;
    c.Width = workW;
    c.Height = workH;
    c.WhitePoint = 1.f; // mask strength
    Transition(cmd, ghost.bias, VK_IMAGE_LAYOUT_GENERAL);
    Reads reads {};
    reads[0] = Rd(ghost.field);
    if (!pass->Dispatch(cmd, Blob::GhostBias, &c, sizeof(c), Groups(workW, 8), Groups(workH, 8), reads,
                        ghost.bias.view))
        return nullptr;
    return &ghost.bias;
}

// The Transformer decode: the raw edit where the output still shows the occluder, seeded rings, an outward feather in
// place. Leaves the decoded residual in `carrier`.
bool RepairTransformer(VkCommandBuffer cmd, const Frame& in, OwnedImage& proxy, OwnedImage& answer, unsigned workW,
                       unsigned workH)
{
    Transition(cmd, carrier, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, ghost.rings, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, temporalOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    Constants decode {};
    decode.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostDecodeSeed;
    decode.Width = workW;
    decode.Height = workH;
    decode.Passthrough = in.depthInverted;
    Reads reads {};
    reads[0] = Rd(temporalOutput);
    reads[1] = Rd(ghost.expanded);
    reads[2] = Rd(proxy);
    reads[3] = Rd(depth);
    reads[4] = Rd(answer);
    reads[5] = Rd(ghost.field);
    // 16x8 groups (PrivateCarrier.hlsl)
    if (!pass->Dispatch(cmd, Blob::GhostDecodeSeed, &decode, sizeof(decode), Groups(workW, 16), Groups(workH, 8), reads,
                        carrier.view, ghost.rings.view))
        return false;

    Constants c {};
    Fields::SetPrivateRings(c, workW, workH, in.depthInverted, depthPrecision, true);
    c.Mode = DlssNr::Superior::DlssNrMode_PrivateGhostFeatherInPlace;
    reads = {};
    reads[0] = Rd(proxy);
    reads[1] = Rd(ghost.rings);
    reads[2] = Rd(proxy);
    reads[3] = Rd(depth);
    reads[4] = Rd(answer);
    // 16x16 groups covering 32x16 pixels
    return pass->Dispatch(cmd, Blob::GhostRingsInPlace, &c, sizeof(c), Groups(workW, 32), Groups(workH, 16), reads,
                          carrier.view);
}

// ---------------------------------------------------------------------------------------------
// One frame (Pipeline.inl Stabilize)
// ---------------------------------------------------------------------------------------------

void FrameFailed(const char* what)
{
    resetNext = true;
    if (++failures >= kFailuresAllowed)
    {
        phase = Phase::Failed;
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: {} {} times in a row; the private DLSS is off until Superior Scaling is "
                 "selected again or the raster changes",
                 what, failures);
    }
    Say("Private DLSS evaluation failed; using the current NR edit.");
}

// Runs the private DLSS on this frame's pair. Returns the signed residual (a carrier at the model's raster, readable) or
// null when this frame composes the raw answer (the private DLSS is being made, off, or failed).
OwnedImage* Step(VkCommandBuffer cmd, const Config& cfg, const Frame& in, OwnedImage& proxy, OwnedImage& answer,
                 float workScale)
{
    const unsigned workW = proxy.width, workH = proxy.height;
    const unsigned preset = choice.Select(cfg.DlssNrSuperiorScalingPreset.value_or_default(),
                                          cfg.DlssNrModelResolutionAuto.value_or_default(),
                                          cfg.DlssNrWorkingScale.value_or_default(), workScale);

    // The NGX core went before we could release (a shutdown from inside the NR pass): its objects are gone with it, so
    // they are forgotten, never released or evaluated, and the next frame makes them again.
    if ((feature != nullptr || params != nullptr) && !NVNGXProxy::IsVulkanInited())
    {
        feature = nullptr;
        params = nullptr;
        phase = Phase::None;
        waited = failures = 0;
        resetNext = true;
        LOG_INFO("DLSS-NR Vulkan Superior Scaling: the NGX runtime shut down under the private DLSS; it is made again");
    }

    if (preset == Sup::NoPrivateDlss)
    {
        // Nothing wanted (or no longer): what was made goes, with the device drained once.
        if (Holds())
            Rebuild(in.device);
        Say("");
        return nullptr;
    }

    if (srUnavailable)
    {
        Say("NVIDIA DLSS is not available here. Using the current NR edit.");
        return nullptr;
    }

    // Another preset, raster or depth convention: a new feature (the D3D12 generation change).
    if ((phase != Phase::None && (wantedPreset != preset || builtW != workW || builtH != workH ||
                                  builtInverted != in.depthInverted)) ||
        ownerDevice != in.device)
    {
        Rebuild(in.device);
        if (ownerDevice != in.device && ownerDevice != VK_NULL_HANDLE)
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: new device, the private DLSS starts over");
        ownerDevice = in.device;
    }

    if (!in.haveDepth)
    {
        // No guides to give it: this frame composes the current edit, the history starts over after it.
        resetNext = true;
        return nullptr;
    }

    if (phase == Phase::Failed)
        return nullptr;

    if (phase == Phase::None)
    {
        builtInverted = in.depthInverted;
        wantedPreset = preset;
        Create(cmd, in, preset);
        return nullptr; // created or not, this frame composes the raw answer
    }

    if (phase == Phase::Creating)
    {
        if (vkGetEventStatus(in.device, created) != VK_EVENT_SET)
        {
            if (++waited > kCreationWaitFrames)
            {
                LOG_WARN("DLSS-NR Vulkan Superior Scaling: the private DLSS creation was never run; giving it up for this "
                         "raster");
                phase = Phase::Failed;
                Say("Private DLSS creation was not submitted. Using the current NR edit.");
            }
            return nullptr;
        }
        phase = Phase::Ready;
        resetNext = true;
        LOG_INFO("DLSS-NR Vulkan Superior Scaling: private DLSS preset {} ready", actualPreset);
        Say(actualPreset == wantedPreset ? ""
                                         : "This DLSS version does not have the selected model; it runs its own default "
                                           "model instead.");
    }

    // ---- Ready: encode, (field), evaluate, decode ----
    const bool reset = resetNext || in.reset;
    const bool repair = Sup::NeedsGhostField(actualPreset);

    Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, carrier, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, temporalMotion, VK_IMAGE_LAYOUT_GENERAL);

    // v50: fixed carrier + motion, from the matched guides (D3D12: encode, Mode 39, with guidesMatched).
    Constants encode {};
    encode.Mode = DlssNr::Superior::DlssNrMode_SuperiorCarrierMotion;
    encode.Width = workW;
    encode.Height = workH;
    Fields::SetDepthRegion(encode, in.guides->depth.x, in.guides->depth.y, in.guides->depth.width,
                           in.guides->depth.height);
    Fields::SetMotionRegion(encode, 0, 0, workW, workH);
    encode.MvScaleX = in.mvX;
    encode.MvScaleY = in.mvY;
    Reads reads {};
    reads[0] = Rd(proxy);
    reads[1] = Rd(answer);
    reads[2] = Rd(motion);
    if (!pass->Dispatch(cmd, Blob::PrivateEncode, &encode, sizeof(encode), Groups(workW, 8), Groups(workH, 8), reads,
                        carrier.view, temporalMotion.view))
    {
        FrameFailed("the private encode failed");
        return nullptr;
    }

    // Transformer: the disocclusion field for the bias before the DLSS and the repair after it.
    OwnedImage* bias = repair ? BiasMask(cmd, in, proxy, answer, workW, workH, reset) : nullptr;
    const bool haveField = repair && bias != nullptr;

    Transition(cmd, carrier, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, temporalMotion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, temporalOutput, VK_IMAGE_LAYOUT_GENERAL);
    if (haveField)
        Transition(cmd, *bias, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // D3D12's evaluate parameters (Pipeline.inl Stabilize).
    auto* p = params;
    if (repair)
    {
        p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, haveField ? &bias->ngx : nullptr);
        p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0u);
        p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0u);
    }
    p->Set(NVSDK_NGX_Parameter_Color, &carrier.ngx);
    p->Set(NVSDK_NGX_Parameter_Output, &temporalOutput.ngx);
    p->Set(NVSDK_NGX_Parameter_Depth, &depth.ngx);
    p->Set(NVSDK_NGX_Parameter_MotionVectors, &temporalMotion.ngx);
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, &unit.ngx);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, workW);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, workH);
    p->Set(NVSDK_NGX_Parameter_Reset, (unsigned) reset);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, std::isfinite(in.frameMs) && in.frameMs > 0 ? in.frameMs : 16.67f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

    const NVSDK_NGX_Result result = NVNGXProxy::VULKAN_EvaluateFeature()(cmd, feature, p, nullptr);
    if (result != NVSDK_NGX_Result_Success)
    {
        LOG_WARN("DLSS-NR Vulkan Superior Scaling: private DLSS evaluate failed (NGX result {:X})", (unsigned) result);
        ghost.valid = false;
        FrameFailed("the private DLSS evaluation failed");
        return nullptr;
    }

    Transition(cmd, temporalOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, carrier, VK_IMAGE_LAYOUT_GENERAL);

    bool decoded = false;
    if (haveField)
        decoded = RepairTransformer(cmd, in, proxy, answer, workW, workH);
    else
    {
        // CNN (and a Transformer without a field): the plain decode, Mode 38 (D3D12's read-only decode, no preset fade).
        Constants decode {};
        decode.Mode = DlssNr::Superior::DlssNrMode_SuperiorCarrierDecode;
        decode.Width = workW;
        decode.Height = workH;
        reads = {};
        reads[0] = Rd(temporalOutput);
        decoded = pass->Dispatch(cmd, Blob::CarrierDecode, &decode, sizeof(decode), Groups(workW, 8), Groups(workH, 8),
                                 reads, carrier.view);
    }

    if (!decoded)
    {
        ghost.valid = false;
        FrameFailed("the private DLSS decode failed");
        return nullptr;
    }

    failures = 0;
    resetNext = false;
    Transition(cmd, carrier, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &carrier;
}
} // namespace Private

// What EvaluateAtSeamVk calls where it used to call Finish alone: the private DLSS when it is wanted and ready, else
// the raw answer through the No DLSS composition.
bool Compose(VkCommandBuffer cmd, const Config& cfg, const DlssNrConstants& resolve, float workScale,
             unsigned int width, unsigned int height, OwnedImage& proxy, OwnedImage& answer, OwnedImage& keep,
             VkImageView exposureView, VkImageView target, const Private::Frame& frame)
{
    OwnedImage* residual = Private::Step(cmd, cfg, frame, proxy, answer, workScale);
    return Finish(cmd, cfg, resolve, workScale, width, height, proxy, residual ? *residual : answer, residual != nullptr,
                  keep, exposureView, target);
}
} // namespace SuperiorVk
