// Superior Scaling, Vulkan (menu: NR Options, Upscale Mode "Superior Scaling"; ini [DlssNr] Transfer=11).
//
// The No DLSS path of the D3D12 side (shaders/dlssnr/superior/Pipeline.inl, DlssNr_Dx12.cpp), step for step and from the
// same HLSL (tools/compile_superior_vk.ps1 builds it to SPIR-V; the D3D12 blobs are unchanged):
//   1. Area downsample of the encoded proxy to the model's raster, instead of the ordinary box downsample (v59's Area,
//      dlssnr_superior_area).
//   2. The game's depth and motion matched to the model's raster (SuperiorRun::MatchGuides: the uniform joint guides), so
//      the model is handed guides at its own size, with the vector scale taken from the matched grid.
//   3. The model, one pass.
//   4. The 1/8-size low field of the answer minus the model's input (host_low_raw), when the band gains are not both 1.
//   5. The resolve: the live composition (host_resolve_guided_raw) with the radius-one guided estimator reading the model's
//      raster, shaped by the low field.
// There is no private DLSS yet (a later story), so the saved CNN / Transformer preset runs as No DLSS here, and says so.
//
// Runs below 100% Model resolution, unpacked, in one pass and without Reuse detail (DlssNr_SuperiorVkRule.h decides, and
// words each case it does not); otherwise the Matched residual composition the Vulkan path always had runs, as D3D12 runs
// it at 100% and above.
//
// Vulkan differences from D3D12, and why:
//   - Layouts instead of resource states: our own images are moved with Transition; the game's depth and motion are read
//     in SHADER_READ_ONLY_OPTIMAL and never moved (through a depth-only view when the game's depth view covers stencil too,
//     EdgeCompressionVk::DepthRead). The matched motion is RGBA32F: R32G32 is not a storage format every device offers.
//   - No GPU lifetime tracker: the images are made and freed with the device drained, as the rest of the Vulkan path does
//     (a first frame, a size change, or after the pass has not run for a few seconds).
//   - Compute and transfer commands only, so it is legal on the async-compute queue a game may present from.
//   - The live exposure travels in the motion slot (t3), where the Vulkan composition has always read it, not in t4.
//
// Included inside DlssNrFeature_Vk.cpp's anonymous namespace, after DlssNr_Spatial_Vk.inl. EvaluateAtSeamVk calls Begin
// where it decides the frame, and the rest around the model, all under g_vkMutex. The menu reads Status().
namespace SuperiorVk
{
namespace Rule = DlssNr::SuperiorScaling::VkRule;
namespace Sp = DlssNr::Spatial;

std::unique_ptr<DlssNrSuperior_Vk> pass;
bool passFailed = false;

OwnedImage depth;  // the game's depth matched to the model's raster, R32F
OwnedImage motion; // the game's motion matched the same way, RGBA32F (the model reads xy)
OwnedImage low;    // the 1/8-size low field, RGBA16F
unsigned int madeFor[2] = {};

// When the pass last ran, for letting go of its images and passes once it has not for a while.
std::chrono::steady_clock::time_point lastRan {};
constexpr auto kIdleRelease = std::chrono::seconds(5);

std::mutex statusLock;
std::string status;
Rule::Outcome lastOutcome = Rule::Outcome::NotSelected;
bool lastPrivateWanted = false;
bool outcomeSaid = false;

std::string Status()
{
    std::lock_guard<std::mutex> lock(statusLock);
    return status;
}

void Publish(const std::string& text)
{
    std::lock_guard<std::mutex> lock(statusLock);
    status = text;
}

bool HoldsImages() { return depth.Valid() || motion.Valid() || low.Valid(); }

void DestroyImages()
{
    for (OwnedImage* img : { &depth, &motion, &low })
        DestroyImage(*img);

    madeFor[0] = madeFor[1] = 0;
}

// The images for a model raster. Earlier frames may still read the old ones, so the device is drained first; that happens
// on the first frame, on a size change (the resize block has just drained for the same reason) and after an idle release.
bool EnsureImages(VkDevice device, unsigned int workW, unsigned int workH)
{
    if (depth.Valid() && motion.Valid() && low.Valid() && madeFor[0] == workW && madeFor[1] == workH)
        return true;

    if (HoldsImages() && device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);

    DestroyImages();
    const bool ok = CreateImage(depth, workW, workH, VK_FORMAT_R32_SFLOAT, true) &&
                    CreateImage(motion, workW, workH, VK_FORMAT_R32G32B32A32_SFLOAT, true) &&
                    CreateImage(low, (workW + 7) / 8, (workH + 7) / 8, VK_FORMAT_R16G16B16A16_SFLOAT, true);

    if (!ok)
    {
        DestroyImages();
        return false;
    }

    madeFor[0] = workW;
    madeFor[1] = workH;
    return true;
}

// The pass, made on first use; not retried once it failed.
bool PassReady(VkDevice device, VkPhysicalDevice physicalDevice)
{
    if (!pass && !passFailed)
    {
        pass = std::make_unique<DlssNrSuperior_Vk>("Superior Scaling", device, physicalDevice);

        if (!pass->Ready())
        {
            pass.reset();
            passFailed = true;
            LOG_WARN("DLSS-NR Vulkan: the Superior Scaling passes could not be built; Matched residual is used");
        }
    }

    return pass != nullptr && pass->Ready();
}

// The 310.8 runtime has one main network only, which changes the band gain tables (DlssNr_SuperiorScaling.h). Read from the
// model's file version once; D3D12 does the same when it makes its pipeline.
bool MainOnlyGains()
{
    static std::optional<bool> mainOnly;

    if (!mainOnly.has_value())
    {
        mainOnly = false;
        version_t version {};
        const auto snippet = FindSnippet();

        if (snippet.has_value() && Util::GetFileVersion(snippet->wstring(), &version))
            mainOnly = DlssNr::SuperiorScaling::MainOnlyRuntime(version.major, version.minor);
        else
            LOG_WARN("DLSS-NR Vulkan Superior Scaling: the model's version is unavailable; using the 310.9 preset "
                     "semantics for the frequency gains");
    }

    return *mainOnly;
}

// What a 16- or 24-bit integer depth cannot tell apart, in the units the guide shaders compare (the D3D12 table).
float DepthPrecision(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return 2.f / 65535.f;
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 2.f / 16777215.f;
    default:
        return 0.f;
    }
}

// Decides this frame (and publishes and logs what changed). reduced: the model works on fewer pixels than the frame.
Rule::Outcome Begin(const Config& cfg, VkDevice device, VkPhysicalDevice physicalDevice, bool reduced, bool packed,
                    unsigned int passes, unsigned int workW, unsigned int workH)
{
    Rule::Frame frame;
    frame.selected = DlssNr::SuperiorScaling::Enabled(cfg);
    frame.reduced = reduced;
    frame.packed = packed;
    frame.passes = passes;
    frame.detailReuse = cfg.DlssNrDetailReuse.value_or_default();
    if (frame.selected)
    {
        // The device's limits do not change; asked once per device handle.
        static VkPhysicalDevice asked = VK_NULL_HANDLE;
        static bool canRun = true;
        if (asked != physicalDevice)
        {
            asked = physicalDevice;
            canRun = DlssNrSuperior_Vk::DeviceCanRun(physicalDevice);
        }
        frame.deviceCanRun = canRun;
    }
    Rule::Outcome outcome = Rule::Decide(frame);

    // Only a run that could go ahead builds anything; the images are the last thing that can fail.
    if (outcome == Rule::Outcome::Runs && (!PassReady(device, physicalDevice) || !EnsureImages(device, workW, workH)))
        outcome = Rule::Outcome::PassFailed;

    const bool privateWanted =
        DlssNr::SuperiorScaling::Preset(cfg.DlssNrSuperiorScalingPreset.value_or_default()) !=
        DlssNr::SuperiorScaling::NoPrivateDlss;
    Publish(Rule::Describe(outcome, privateWanted));

    if (!outcomeSaid || lastOutcome != outcome || lastPrivateWanted != privateWanted)
    {
        outcomeSaid = true;
        lastOutcome = outcome;
        lastPrivateWanted = privateWanted;

        if (outcome == Rule::Outcome::Runs)
        {
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: running (No DLSS), the model works on {}x{}", workW, workH);

            if (privateWanted)
                LOG_INFO("DLSS-NR Vulkan Superior Scaling: CNN/Transformer private DLSS on Vulkan: not yet, running "
                         "No DLSS");
        }
        else if (outcome != Rule::Outcome::NotSelected)
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: {}", Rule::Describe(outcome, privateWanted));
        else
            LOG_INFO("DLSS-NR Vulkan Superior Scaling: not selected");
    }

    const auto now = std::chrono::steady_clock::now();

    if (outcome == Rule::Outcome::Runs)
        lastRan = now;
    else if ((pass || HoldsImages()) && lastRan != std::chrono::steady_clock::time_point {} &&
             now - lastRan >= kIdleRelease && device != VK_NULL_HANDLE)
    {
        // Not run for a while (another Upscale Mode, 100%, a fallback): the images and passes go, with the device drained
        // once. A quick switch back finds them still here.
        vkDeviceWaitIdle(device);
        DestroyImages();
        pass.reset();
        lastRan = {};
    }

    return outcome;
}

// 1. The Area downsample of the encoded proxy (native size) to the model's raster, into proxySmall.
bool Area(VkCommandBuffer cmd, OwnedImage& proxy, OwnedImage& proxySmall, unsigned int workW, unsigned int workH)
{
    DlssNr::Superior::DlssNrConstants c {};
    c.Mode = DlssNr::Superior::DlssNrMode_Downsample;
    c.Width = workW;
    c.Height = workH;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;

    Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, proxySmall, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return pass->Dispatch(cmd, DlssNrSuperior_Vk::Blob::Area, &c, sizeof(c), (workW + 7) / 8, (workH + 7) / 8, reads,
                          proxySmall.view);
}

// 2. The game's depth and motion matched to the model's raster (D3D12: SuperiorRun::MatchGuides, the uniform joint guides
// with the bounded-footprint correction where the guides shrink). Leaves `depth` and `motion` readable.
bool MatchGuides(VkCommandBuffer cmd, VkDevice device, const NVSDK_NGX_Resource_VK* gameDepth,
                 const NVSDK_NGX_Resource_VK* gameMotion, const DlssNr::GuideRegions& guides, unsigned int width,
                 unsigned int height, unsigned int workW, unsigned int workH, bool depthInverted, float gameMvX,
                 float gameMvY, DlssNr::GuideExtent reference)
{
    const auto depthRead = EdgeCompressionVk::DepthRead(device, gameDepth);
    const bool haveDepth = depthRead.view != VK_NULL_HANDLE && guides.depth.valid();

    Sp::Layout layout {};
    layout.nativeW = width;
    layout.nativeH = height;
    layout.modelW = workW;
    layout.modelH = workH;
    layout.warp.x = pw::BuildAxis(width, width, workW, 1, 0, 0);
    layout.warp.y = pw::BuildAxis(height, height, workH, 1, 0, 0);

    auto c = Sp::MakeGuideConstants(layout, guides, reference, gameMvX, gameMvY);
    c.mode = 104u; // uniform: the model's raster is a plain scaling of the frame
    c.padding[0] = haveDepth ? 1.f : 0.f;
    if (!haveDepth)
    {
        c.depthRect[0] = c.depthRect[1] = 0.f;
        c.depthRect[2] = c.depthRect[3] = 1.f;
    }
    c.unused = depthInverted ? 1.f : 0.f;
    Sp::SetDepthPrecision(c, haveDepth ? DepthPrecision(gameDepth->Resource.ImageViewInfo.Format) : 0.f);

    using Blob = DlssNrSuperior_Vk::Blob;
    const bool quantized = c.warp.reserved != 0;
    const bool joint = haveDepth; // padding[0] > .5
    const Sp::MinifiedMotionDispatch minified = joint ? Sp::PrepareJointMinification(c) : Sp::MinifiedMotionDispatch {};
    const Blob primary = joint ? (quantized ? Blob::JointGuidesQuantized : Blob::JointGuides) : Blob::SurfaceGuides;
    const Blob correction = joint && minified.pixels ? (quantized ? Blob::JointMinifiedGuidesQuantized
                                                                   : Blob::JointMinifiedGuides)
                                                     : Blob::Count;

    Transition(cmd, depth, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, motion, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[1] = { haveDepth ? depthRead.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[2] = { gameMotion->Resource.ImageViewInfo.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

    if (!pass->DispatchGuides(cmd, primary, correction, &c, sizeof(c), (workW + 7) / 8, (workH + 7) / 8,
                              minified.groupsX, minified.groupsY, reads, depth.view, motion.view))
        return false;

    Transition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// 4 and 5. The low field (unless both gains are 1) and the resolve. `proxy` and `answer` are the model's input and answer
// at the model's raster, readable. `resolve` is the composition's constants, as the ordinary resolve would take them.
bool Compose(VkCommandBuffer cmd, const Config& cfg, const DlssNrConstants& resolveIn, float workScale,
             unsigned int width, unsigned int height, OwnedImage& proxy, OwnedImage& answer, OwnedImage& keep,
             VkImageView exposureView, VkImageView target)
{
    DlssNrConstants resolve = resolveIn; // the band gains and flags below are Superior's, not the ordinary resolve's
    // The same gains D3D12 reads for one pass over the frame's native size: no pass has run before this one.
    const auto scales = DlssNr::SuperiorScaling::MakeFrequencyScales(width, height, proxy.width, proxy.height,
                                                                     proxy.width, proxy.height);
    const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, 0u, 1u, scales, MainOnlyGains());
    const bool wantLow = gains.high != 1.0f || gains.low != 1.0f;

    if (wantLow)
    {
        DlssNr::Superior::DlssNrConstants c {};
        c.Mode = DlssNr::Superior::DlssNrMode_GuidedResidualLow;
        c.Transfer = 0u; // the model's answer is raw, not a decoded signed residual
        c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
        c.Width = (proxy.width + 7) / 8;
        c.Height = (proxy.height + 7) / 8;

        Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, low, VK_IMAGE_LAYOUT_GENERAL);

        DlssNrSuperior_Vk::Reads reads {};
        reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        reads[1] = { answer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        if (!pass->Dispatch(cmd, DlssNrSuperior_Vk::Blob::LowRaw, &c, sizeof(c), (c.Width + 7) / 8,
                            (c.Height + DlssNr::SuperiorScaling::LowGroupHeight - 1) /
                                DlssNr::SuperiorScaling::LowGroupHeight,
                            reads, low.view))
            return false;

        Transition(cmd, low, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    DlssNr::Superior::Fields::SetBandGains(resolve, gains.high, gains.low);
    DlssNr::SuperiorScaling::SetGuidedRange(resolve, workScale);
    DlssNr::Superior::Fields::SetResolveFlags(resolve, wantLow, true);

    Transition(cmd, proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, answer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    DlssNrSuperior_Vk::Reads reads {};
    reads[0] = { proxy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[1] = { answer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[2] = { keep.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[3] = { exposureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    reads[4] = { wantLow ? low.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return pass->Dispatch(cmd, DlssNrSuperior_Vk::Blob::ResolveGuidedRaw, &resolve, sizeof(resolve), (width + 7) / 8,
                          (height + 7) / 8, reads, target);
}

// Shutdown. deviceAlive: the device is idle and everything is destroyed; otherwise it went with the device and took every
// handle, so they are only forgotten.
void Release(bool deviceAlive)
{
    if (deviceAlive)
    {
        DestroyImages();
        pass.reset();
    }
    else
    {
        for (OwnedImage* img : { &depth, &motion, &low })
            *img = OwnedImage {};
        madeFor[0] = madeFor[1] = 0;
        pass.release(); // its pipelines and descriptors went with the device
    }

    passFailed = false;
    lastRan = {};
    outcomeSaid = false;
    Publish("");
}
} // namespace SuperiorVk
