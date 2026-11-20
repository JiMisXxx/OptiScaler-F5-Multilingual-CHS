#include "pch.h"
#include "F5Low_Vk.h"

#include "F5Low.h"
#include "F5LowVkRules.h"
#include "f5low_frames.h"

#include <State.h>
#include <Config.h>
#include <hooks/Vulkan_Hooks.h>
#include <native/NativeDriverVk.h>
#include <native/SharedFrameVk.h>

#include <magic_enum.hpp>

#include <array>
#include <mutex>

using Microsoft::WRL::ComPtr;
using namespace F5LowVk;

namespace
{
constexpr VkImageUsageFlags kCopyUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr uint64_t kWaitFenceNs = 1'000'000'000ull;
constexpr uint64_t kAcquireNs = 100'000'000ull;

// The driver's swapchain calls, past our hooks (a hook would see them as the game's)
template <typename PFN> PFN SwapchainFunction(const char* name, PFN fallback)
{
    if (auto function = reinterpret_cast<PFN>(VulkanHooks::OriginalSwapchainFunction(name)))
        return function;

    return fallback;
}

// A CPU wait until a time (Util::MillisecondsNow's clock): the timer for most of it, a spin for the rest
void WaitUntil(double targetMs)
{
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);

    for (;;)
    {
        const double remaining = targetMs - Util::MillisecondsNow();

        if (remaining <= 0.0)
            return;

        if (remaining > 1.5 && timer != nullptr)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((remaining - 1.0) * 10000.0);

            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(timer, 50);
                continue;
            }
        }

        YieldProcessor();
    }
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags src,
                             VkAccessFlags dst, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return barrier;
}

// A generated frame, as the D3D12 side left it, into an image the swapchain handed out; ready to present after.
void RecordOutput(VkCommandBuffer cmd, const native::SharedImageVk& shared, VkImage target, uint32_t family)
{
    VkImageMemoryBarrier before[2] = {
        Barrier(shared.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, family),
        Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, before);

    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { shared.Width(), shared.Height(), 1 };
    vkCmdCopyImage(cmd, shared.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier after[2] = {
        Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0),
        Barrier(shared.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, 0, family, VK_QUEUE_FAMILY_EXTERNAL),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, after);
}

bool UsableLayout(VkImageLayout layout)
{
    return layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED;
}

// Motion vector formats that cross as they are (a copy cannot convert)
bool MotionFormat(VkFormat format, DXGI_FORMAT* dxgi)
{
    switch (format)
    {
    case VK_FORMAT_R16G16_SFLOAT:
        *dxgi = DXGI_FORMAT_R16G16_FLOAT;
        return true;
    case VK_FORMAT_R32G32_SFLOAT:
        *dxgi = DXGI_FORMAT_R32G32_FLOAT;
        return true;
    default:
        return false;
    }
}

constexpr uint32_t kBatches = 1 + kMaxGenerated; // the copy in, one copy out per generated frame

bool WaitFence12(ID3D12Fence* fence, uint64_t value, DWORD timeoutMs)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (event == nullptr || FAILED(fence->SetEventOnCompletion(value, event)))
    {
        if (event != nullptr)
            CloseHandle(event);

        return false;
    }

    const bool done = WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

// A fence value nobody will signal (f5low refused the call): the GPU waits on it must not hang
void SignalIfBehind(ID3D12Fence* fence, uint64_t value)
{
    if (fence != nullptr && fence->GetCompletedValue() < value)
        fence->Signal(value);
}
} // namespace

struct F5Low_Vk::Impl
{
    // f5low.dll's frames-only API
    HMODULE module = nullptr;
    decltype(&f5lowFramesCreate) create = nullptr;
    decltype(&f5lowFramesBuildStatus) buildStatus = nullptr;
    decltype(&f5lowFramesGenerate) generate = nullptr;
    decltype(&f5lowFramesResize) resize = nullptr;
    decltype(&f5lowFramesDestroy) destroy = nullptr;
    decltype(&f5lowFramesLastError) lastError = nullptr;

    F5LowFramesContext context = nullptr;
    DXGI_FORMAT contextFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t contextFlags = 0;
    uint32_t contextWidth = 0, contextHeight = 0;
    bool d3dReady = false;

    // The private D3D12 device (the frame source's) and what the depth needs on it
    native::VkInterop interop;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> allocators[kSlots];
    uint64_t allocatorValue[kSlots] {};
    ComPtr<ID3D12GraphicsCommandList> list;

    // ready: the host's copies done (Vulkan signals it, the depth stage on D3D12 after it); consumed and done are
    // f5low's. One fence each so every one only ever goes up.
    native::SharedFenceVk ready, consumed, done;
    uint64_t consumedPrevious = 0;

    native::SharedImageVk color, hudless, motion, depthImage;
    native::SharedImageVk out[kMaxGenerated];
    native::SharedBufferVk depthBuffer;
    ComPtr<ID3D12Resource> depthTexture;
    native::VkDepthCopyFormat depthFormat {};
    bool depthViaBuffer = false;
    VkFormat depthSource = VK_FORMAT_UNDEFINED;
    uint32_t depthWidth = 0, depthHeight = 0;

    // The swapchain's images, in the driver's order
    std::vector<VkImage> images;

    // Per frame in flight
    struct Slot
    {
        VkCommandBuffer commands[kBatches] {};
        VkFence fences[kBatches] {};
        bool used[kBatches] {};
        VkSemaphore inDone = VK_NULL_HANDLE;
        VkSemaphore acquired[kMaxGenerated] {};
        VkSemaphore outDone[kMaxGenerated] {};
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    Slot slots[kSlots];
    uint32_t cursor = 0;
    bool slotsReady = false;
    bool deviceGone = false;
    bool warnedQueue = false;
    bool warnedNotReady = false;

    ~Impl() = default;
};

F5Low_Vk::F5Low_Vk() : IFGFeature_Vk(), IFGFeature(), _impl(std::make_unique<Impl>())
{
    _maxInterpolationCount =
        F5Low::MaxInterpolations(F5LOW_FRAMES_MAX_GENERATED, Config::Instance()->FGXeFGMaxInterpolations.value_or_default());
    UpdateCount();

    if (F5Low::LatchedGenerator() != F5Low::Generator::XefgTensor)
    {
        _blocked = "On the Vulkan native swapchain only XeFG Tensor is available";
        LOG_WARN("F5Low (Vulkan): {}", _blocked);
        return;
    }

    auto& s = *_impl;
    s.module = F5Low::LoadLibraryForOutput();

    if (s.module == nullptr)
    {
        _blocked = std::string("F5Low is not available: ") + F5Low::Available().reason;
        LOG_WARN("F5Low (Vulkan): {}", _blocked);
        return;
    }

    s.create = reinterpret_cast<decltype(s.create)>(GetProcAddress(s.module, "f5lowFramesCreate"));
    s.buildStatus = reinterpret_cast<decltype(s.buildStatus)>(GetProcAddress(s.module, "f5lowFramesBuildStatus"));
    s.generate = reinterpret_cast<decltype(s.generate)>(GetProcAddress(s.module, "f5lowFramesGenerate"));
    s.resize = reinterpret_cast<decltype(s.resize)>(GetProcAddress(s.module, "f5lowFramesResize"));
    s.destroy = reinterpret_cast<decltype(s.destroy)>(GetProcAddress(s.module, "f5lowFramesDestroy"));
    s.lastError = reinterpret_cast<decltype(s.lastError)>(GetProcAddress(s.module, "f5lowFramesLastError"));

    if (s.create == nullptr || s.buildStatus == nullptr || s.generate == nullptr || s.resize == nullptr ||
        s.destroy == nullptr || s.lastError == nullptr)
    {
        _blocked = "This f5low.dll has no frames-only API (f5lowFramesCreate and friends), it needs a newer f5low.dll";
        LOG_WARN("F5Low (Vulkan): {}", _blocked);
        s.module = nullptr;
        return;
    }

    LOG_INFO("F5Low (Vulkan): frames-only API found, up to {} generated frames per game frame", _maxInterpolationCount);
}

F5Low_Vk::~F5Low_Vk() { Shutdown(); }

feature_version F5Low_Vk::Version()
{
    feature_version version;
    version.major = F5LOW_FRAMES_API_VERSION;
    return version;
}

const char* F5Low_Vk::Name()
{
    static std::string name;
    name = _framesToInterpolate > 0 ? "F5Low " + std::to_string(_framesToInterpolate + 1) + "x" : "F5Low";
    return name.c_str();
}

void F5Low_Vk::SetReason(const std::string& reason)
{
    std::lock_guard lock(_reasonMutex);

    if (_reason != reason)
    {
        _reason = reason;

        if (!reason.empty())
            LOG_WARN("F5Low (Vulkan): {}", reason);
    }
}

std::string F5Low_Vk::StatusLine() const
{
    std::lock_guard lock(_reasonMutex);

    if (!_blocked.empty())
        return _blocked;

    return _reason;
}

void F5Low_Vk::UpdateCount()
{
    const int target = Config::Instance()->FGInterpolationCount(Config::Instance()->FGXeFGInterpolationCount.value_or_default(),
                                                                 _maxInterpolationCount);

    if (target != _framesToInterpolate && target >= 1)
    {
        LOG_INFO("F5Low (Vulkan): interpolation count {} -> {}", _framesToInterpolate, target);
        _framesToInterpolate = target;
    }
}

void F5Low_Vk::Activate()
{
    if (_isActive || !_blocked.empty() || _swapchain == VK_NULL_HANDLE)
        return;

    UpdateTarget();
    _isActive = true;
    LOG_DEBUG("");
}

void F5Low_Vk::Deactivate()
{
    if (_isActive)
        LOG_DEBUG("");

    _isActive = false;
    _pending = {};
}

void F5Low_Vk::EvaluateState(FG_Constants& fgConstants)
{
    OwnedLockGuard lock(Mutex, 555);

    auto& state = State::Instance();
    _constants = fgConstants;

    if (_swapchain == VK_NULL_HANDLE || state.isShuttingDown)
        return;

    if (!Config::Instance()->IsFGEnabled() || state.fgChanged)
        Deactivate();

    if (state.fgChanged)
    {
        state.fgChanged = false;
        UpdateTarget();
    }

    state.scChanged = false;
}

bool F5Low_Vk::CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    if (!_blocked.empty())
        return false;

    if (device != _device || pCreateInfo == nullptr || pSwapchain == nullptr)
        return false;

    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    VkFormat sharedFormat = VK_FORMAT_UNDEFINED;

    if (!native::VkSharedPictureFormat(pCreateInfo->imageFormat, &dxgi, &sharedFormat) ||
        dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        SetReason(std::format("The swapchain format {} is not supported by F5Low (8-bit RGBA/BGRA and HDR10 are)",
                              static_cast<int>(pCreateInfo->imageFormat)));
        return false;
    }

    if ((pCreateInfo->imageUsage & kCopyUsage) != kCopyUsage)
    {
        SetReason("The surface does not allow copies of the swapchain's images");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps {};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, pCreateInfo->surface, &caps) != VK_SUCCESS)
        caps.maxImageCount = 0;

    const auto createFn = SwapchainFunction<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR", vkCreateSwapchainKHR);
    const auto imagesFn =
        SwapchainFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR);

    // Spare images for the frames we generate. The game's pNext chain (the latency mode among it) stays as it came.
    VkSwapchainCreateInfoKHR info = *pCreateInfo;
    info.minImageCount = RequestedMinImageCount(pCreateInfo->minImageCount, static_cast<uint32_t>(_maxInterpolationCount),
                                                caps.maxImageCount);

    auto result = createFn(device, &info, pAllocator, pSwapchain);

    if (result != VK_SUCCESS && info.minImageCount != pCreateInfo->minImageCount)
    {
        LOG_WARN("F5Low (Vulkan): {} swapchain images were refused ({}), trying the game's own count",
                 info.minImageCount, magic_enum::enum_name(result));
        info.minImageCount = pCreateInfo->minImageCount;
        result = createFn(device, &info, pAllocator, pSwapchain);
    }

    if (result != VK_SUCCESS)
    {
        LOG_ERROR("F5Low (Vulkan): vkCreateSwapchainKHR error: {}", magic_enum::enum_name(result));
        return false;
    }

    auto& s = *_impl;
    s.deviceGone = false;
    uint32_t count = 0;
    imagesFn(device, *pSwapchain, &count, nullptr);
    s.images.assign(count, VK_NULL_HANDLE);

    if (count == 0 || imagesFn(device, *pSwapchain, &count, s.images.data()) != VK_SUCCESS)
        s.images.clear();

    _headroom = ImageHeadroom(static_cast<uint32_t>(s.images.size()), pCreateInfo->minImageCount);
    _swapchain = *pSwapchain;
    _swapchainFormat = pCreateInfo->imageFormat;
    _swapchainExtent = pCreateInfo->imageExtent;
    _colorSpace = pCreateInfo->imageColorSpace;
    _hdr10 = pCreateInfo->imageFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
             pCreateInfo->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    _failures = 0;
    _intervalMs = 0.0;
    _lastEntryMs = 0.0;
    _pending = {};

    if (_headroom == 0)
        SetReason("The swapchain has no spare images to generate into");
    else
        SetReason("");

    LOG_INFO("F5Low (Vulkan): swapchain {}x{}, format {}, {} images ({} spare), HDR10: {}", _swapchainExtent.width,
             _swapchainExtent.height, magic_enum::enum_name(_swapchainFormat), s.images.size(), _headroom, _hdr10);

    return true;
}

bool F5Low_Vk::DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    // The handle is the driver's own, the hook destroys it after this
    if (swapchain == VK_NULL_HANDLE || swapchain != _swapchain)
        return false;

    Deactivate();

    // Our presents wait on our semaphores and show its images
    if (device == _device && !_impl->deviceGone)
        vkDeviceWaitIdle(device);

    _impl->images.clear();
    _swapchain = VK_NULL_HANDLE;
    _headroom = 0;
    return false;
}

VkResult F5Low_Vk::GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                      VkImage* pSwapchainImages)
{
    return SwapchainFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR)(
        device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VkResult F5Low_Vk::AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                    VkFence fence, uint32_t* pImageIndex)
{
    return SwapchainFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR", vkAcquireNextImageKHR)(
        device, swapchain, timeout, semaphore, fence, pImageIndex);
}

void F5Low_Vk::SetHdrMetadata(VkDevice device, VkSwapchainKHR swapchain, const VkHdrMetadataEXT* pMetadata)
{
    // The hook set _inFGCall around this, so the call goes straight to the driver
    const auto setHdr =
        reinterpret_cast<PFN_vkSetHdrMetadataEXT>(VulkanHooks::GetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));

    if (setHdr != nullptr)
        setHdr(device, 1, &swapchain, pMetadata);
}

bool F5Low_Vk::Present()
{
    OwnedLockGuard lock(Mutex, 2);

    _pending = {};

    // Game stopped sending new frames, menus and such
    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    UpdateCount();

    UINT64 willDispatchFrame = 0;
    const auto index = GetDispatchIndex(willDispatchFrame);

    if (index < 0 || !IsActive() || IsPaused() || !_blocked.empty())
        return false;

    auto velocity = GetResource(FG_ResourceType::Velocity, index);
    auto depth = GetResource(FG_ResourceType::Depth, index);

    if (!velocity || !depth)
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    _pending.valid = true;
    _pending.index = index;
    _pending.frame = willDispatchFrame;
    _pending.velocity = velocity;
    _pending.depth = depth;
    _pending.hudless = GetResource(FG_ResourceType::HudlessColor, index);
    return true;
}

VkResult F5Low_Vk::QueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    const auto presentFn = SwapchainFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR", vkQueuePresentKHR);

    // The time between two game presents, our own waits in it included: with them in, the output settles at the
    // same cadence, the game's thread is just held less
    const double entryMs = Util::MillisecondsNow();

    if (_lastEntryMs > 0.0)
        _intervalMs = NextInterval(_intervalMs, entryMs - _lastEntryMs);

    _lastEntryMs = entryMs;

    Pending pending = std::move(_pending);
    _pending = {};

    if (!pending.valid || !_blocked.empty() || pPresentInfo->swapchainCount != 1 ||
        pPresentInfo->pSwapchains[0] != _swapchain || _failures >= 3)
    {
        return presentFn(queue, pPresentInfo);
    }

    if (queue != _gameQueue)
    {
        if (!_impl->warnedQueue)
        {
            _impl->warnedQueue = true;
            SetReason("The game presents from a different queue than its first graphics queue");
        }

        return presentFn(queue, pPresentInfo);
    }

    VkPresentInfoKHR finalInfo = *pPresentInfo;
    Generate(queue, pPresentInfo, pending, finalInfo);
    return presentFn(queue, &finalInfo);
}

void F5Low_Vk::WaitIdle()
{
    auto& s = *_impl;

    if (_device != VK_NULL_HANDLE && !s.deviceGone)
        vkDeviceWaitIdle(_device);

    WaitFence12(s.consumed.Fence12(), s.consumedPrevious, 5000);
    WaitFence12(s.done.Fence12(), s.done.Value(), 5000);
    WaitFence12(s.ready.Fence12(), s.ready.Value(), 5000);
}

bool F5Low_Vk::EnsureSlots()
{
    auto& s = *_impl;

    if (s.slotsReady)
        return true;

    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = _gameQueueFamily;

    if (vkCreateCommandPool(_device, &poolInfo, nullptr, &s.pool) != VK_SUCCESS)
    {
        SetReason("Could not make a command pool on the game's queue family");
        return false;
    }

    VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    bool ok = true;

    for (auto& slot : s.slots)
    {
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = s.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = kBatches;
        ok = ok && vkAllocateCommandBuffers(_device, &allocate, slot.commands) == VK_SUCCESS;

        for (auto& fence : slot.fences)
            ok = ok && vkCreateFence(_device, &fenceInfo, nullptr, &fence) == VK_SUCCESS;

        ok = ok && vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &slot.inDone) == VK_SUCCESS;

        for (uint32_t k = 0; k < kMaxGenerated; k++)
        {
            ok = ok && vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &slot.acquired[k]) == VK_SUCCESS;
            ok = ok && vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &slot.outDone[k]) == VK_SUCCESS;
        }
    }

    // Whatever was made is released with the rest (ReleaseAll)
    s.slotsReady = true;

    if (!ok)
    {
        SetReason("Could not make the command buffers or semaphores for the frames");
        return false;
    }

    return true;
}

bool F5Low_Vk::Prepare(const Pending& p, uint32_t count, bool& useHudless)
{
    auto& s = *_impl;
    useHudless = false;

    DXGI_FORMAT dxgiColor = DXGI_FORMAT_UNKNOWN;
    VkFormat vkColor = VK_FORMAT_UNDEFINED;

    if (!native::VkSharedPictureFormat(_swapchainFormat, &dxgiColor, &vkColor))
    {
        SetReason("The swapchain's format cannot be shared with the generator");
        return false;
    }

    const uint32_t width = _swapchainExtent.width;
    const uint32_t height = _swapchainExtent.height;
    std::string why;

    // The D3D12 device the frames are made on: the Vulkan GPU's, the frame source's private one
    if (!s.d3dReady)
    {
        ID3D12Device* device = NativeMotionVk::ShareBridgeDevice(_physicalDevice, why);

        if (device == nullptr)
        {
            SetReason("No D3D12 device on the game's GPU: " + why);
            return false;
        }

        s.device12.Attach(device);

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        bool ok = s.interop.Load(_physicalDevice, _device, why) &&
                  SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&s.queue12)));

        for (auto& allocator : s.allocators)
            ok = ok &&
                 SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));

        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(), nullptr,
                                                       IID_PPV_ARGS(&s.list)));

        if (ok)
            s.list->Close();

        ok = ok && s.ready.Create(device, s.interop) && s.consumed.Create(device, s.interop) &&
             s.done.Create(device, s.interop);

        if (!ok)
        {
            if (why.empty())
                why = !s.ready.Error().empty() ? s.ready.Error() : "creating the D3D12 queue, list or fences failed";

            SetReason("Sharing with the D3D12 device failed: " + why);
            s.ready.Reset();
            s.consumed.Reset();
            s.done.Reset();
            s.list.Reset();
            s.queue12.Reset();
            s.device12.Reset();
            return false;
        }

        s.d3dReady = true;
        LOG_INFO("F5Low (Vulkan): sharing with a D3D12 device on the game's GPU");
    }

    // The picture and its generated frames
    // Depth and motion flags pick f5low's build variant up front; a later change makes one Generate rebuild
    const uint32_t flags = (_hdr10 ? F5LOW_FRAMES_FLAG_HDR10 : 0) |
                           (IsInvertedDepth() ? F5LOW_FRAMES_FLAG_DEPTH_INVERTED : 0) |
                           (IsJitteredMVs() ? F5LOW_FRAMES_FLAG_MOTION_JITTERED : 0);

    if (!s.color.Matches(width, height, vkColor))
    {
        WaitIdle();
        s.hudless.Reset();

        for (auto& image : s.out)
            image.Reset();

        if (!s.color.Create(s.device12.Get(), s.interop, width, height, dxgiColor, vkColor, false))
        {
            SetReason("Sharing the picture failed: " + s.color.Error());
            return false;
        }
    }

    if (s.context != nullptr && (s.contextWidth != width || s.contextHeight != height ||
                                 s.contextFormat != dxgiColor ||
                                 ((s.contextFlags ^ flags) & F5LOW_FRAMES_FLAG_HDR10) != 0))
    {
        WaitIdle();
        const auto result = s.resize(s.context, width, height, dxgiColor, flags);

        if (result != F5LOW_FRAMES_OK)
        {
            SetReason(std::string("f5low could not resize its frames: ") + s.lastError(s.context));
            s.destroy(s.context);
            s.context = nullptr;
            return false;
        }

        s.contextWidth = width;
        s.contextHeight = height;
        s.contextFormat = dxgiColor;
        s.contextFlags = flags;
        LOG_INFO("F5Low (Vulkan): frames resized to {}x{}", width, height);
    }

    if (s.context == nullptr)
    {
        F5LowFramesCreateInfo info {};
        info.apiVersion = F5LOW_FRAMES_API_VERSION;
        info.device = s.device12.Get();
        info.width = width;
        info.height = height;
        info.colorFormat = dxgiColor;
        info.maxGenerated = static_cast<uint32_t>(std::min<int>(_maxInterpolationCount, F5LOW_FRAMES_MAX_GENERATED));
        info.flags = flags;
        info.renderWidth = p.velocity->width;
        info.renderHeight = p.velocity->height;

        const auto result = s.create(&info, &s.context);

        if (result != F5LOW_FRAMES_OK || s.context == nullptr)
        {
            SetReason(std::string("f5low could not make its frames: ") + s.lastError(nullptr));
            s.context = nullptr;
            return false;
        }

        s.contextWidth = width;
        s.contextHeight = height;
        s.contextFormat = dxgiColor;
        s.contextFlags = flags;
        LOG_INFO("F5Low (Vulkan): f5low frames made, {}x{}, format {}, up to {} generated", width, height,
                 magic_enum::enum_name(dxgiColor), info.maxGenerated);
    }

    for (uint32_t k = 0; k < count; k++)
    {
        if (s.out[k].Matches(width, height, vkColor))
            continue;

        if (!s.out[k].Create(s.device12.Get(), s.interop, width, height, dxgiColor, vkColor, true))
        {
            SetReason("Sharing a generated frame failed: " + s.out[k].Error());
            return false;
        }
    }

    // The game's HUD-less picture, when it is the swapchain's size and format
    if (const auto& hudless = p.hudless; hudless && hudless->image != VK_NULL_HANDLE &&
                                         UsableLayout(hudless->layout) && hudless->imageWidth == width &&
                                         hudless->imageHeight == height)
    {
        DXGI_FORMAT hudlessDxgi = DXGI_FORMAT_UNKNOWN;
        VkFormat hudlessShared = VK_FORMAT_UNDEFINED;

        if (native::VkSharedPictureFormat(hudless->format, &hudlessDxgi, &hudlessShared) && hudlessShared == vkColor)
        {
            if (!s.hudless.Matches(width, height, vkColor))
            {
                WaitIdle();

                if (!s.hudless.Create(s.device12.Get(), s.interop, width, height, dxgiColor, vkColor, false))
                {
                    SetReason("Sharing the HUD-less picture failed: " + s.hudless.Error());
                    return false;
                }
            }

            useHudless = true;
        }
    }

    // Motion vectors cross as they are
    DXGI_FORMAT motionDxgi = DXGI_FORMAT_UNKNOWN;
    const auto& velocity = *p.velocity;

    if (!MotionFormat(velocity.format, &motionDxgi))
    {
        SetReason(std::format("The motion vectors' format {} is not supported (R16G16 and R32G32 float are)",
                              static_cast<int>(velocity.format)));
        return false;
    }

    if (!UsableLayout(velocity.layout) || velocity.image == VK_NULL_HANDLE)
    {
        SetReason("The motion vectors are not in a readable state");
        return false;
    }

    if (!s.motion.Matches(velocity.imageWidth, velocity.imageHeight, velocity.format))
    {
        WaitIdle();

        if (!s.motion.Create(s.device12.Get(), s.interop, velocity.imageWidth, velocity.imageHeight, motionDxgi,
                             velocity.format, false))
        {
            SetReason("Sharing the motion vectors failed: " + s.motion.Error());
            return false;
        }
    }

    // Depth: a colour format crosses as an image, a depth format through a buffer and a copy on the D3D12 side
    const auto& depth = *p.depth;

    if (!UsableLayout(depth.layout) || depth.image == VK_NULL_HANDLE)
    {
        SetReason("The depth is not in a readable state");
        return false;
    }

    if (depth.format == VK_FORMAT_R32_SFLOAT)
    {
        s.depthViaBuffer = false;

        if (!s.depthImage.Matches(depth.imageWidth, depth.imageHeight, depth.format))
        {
            WaitIdle();

            if (!s.depthImage.Create(s.device12.Get(), s.interop, depth.imageWidth, depth.imageHeight,
                                     DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, false))
            {
                SetReason("Sharing the depth failed: " + s.depthImage.Error());
                return false;
            }
        }
    }
    else
    {
        native::VkDepthCopyFormat format {};

        if (!native::VkDepthCopyFormatOf(depth.format, &format))
        {
            SetReason(std::format("The depth format {} is not supported", static_cast<int>(depth.format)));
            return false;
        }

        s.depthViaBuffer = true;

        if (s.depthSource != depth.format || s.depthWidth != depth.imageWidth || s.depthHeight != depth.imageHeight ||
            s.depthTexture == nullptr)
        {
            WaitIdle();
            s.depthTexture.Reset();

            const uint64_t size =
                static_cast<uint64_t>(native::DepthRowPitch(depth.imageWidth, format.bytes)) * depth.imageHeight;

            if (!s.depthBuffer.Create(s.device12.Get(), s.interop, size))
            {
                SetReason("Sharing the depth failed: " + s.depthBuffer.Error());
                return false;
            }

            // Read by f5low's own queue in the common state: simultaneous access lets it be promoted there
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = depth.imageWidth;
            desc.Height = depth.imageHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format.texture;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

            if (FAILED(s.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&s.depthTexture))))
            {
                SetReason("Could not make the depth texture on the D3D12 device");
                return false;
            }

            s.depthFormat = format;
            s.depthSource = depth.format;
            s.depthWidth = depth.imageWidth;
            s.depthHeight = depth.imageHeight;
            LOG_INFO("F5Low (Vulkan): sharing the depth at {}x{}, Vulkan format {}", s.depthWidth, s.depthHeight,
                     magic_enum::enum_name(depth.format));
        }
    }

    return true;
}

bool F5Low_Vk::Generate(VkQueue queue, const VkPresentInfoKHR* present, Pending& p, VkPresentInfoKHR& finalInfo)
{
    auto& s = *_impl;

    if (!p.depth || !p.velocity || s.images.empty() || present->pImageIndices == nullptr)
        return false;

    const uint32_t imageIndex = present->pImageIndices[0];

    if (imageIndex >= s.images.size())
        return false;

    const VkImage source = s.images[imageIndex];

    uint32_t count = GeneratedCount(_framesToInterpolate, static_cast<uint32_t>(_maxInterpolationCount), _headroom);

    // Until the frame interval is known the generator only takes the frames as history
    if (!CanPace(_intervalMs, count))
        count = 0;

    bool useHudless = false;

    if (!Prepare(p, count, useHudless) || !EnsureSlots())
    {
        _failures++;
        return false;
    }

    const uint32_t slotIndex = s.cursor;
    auto& slot = s.slots[slotIndex];
    s.cursor = (s.cursor + 1) % kSlots;

    // The slot's last use is over: its command buffers and semaphores are ours again
    {
        VkFence waits[kBatches];
        uint32_t waitCount = 0;

        for (uint32_t i = 0; i < kBatches; i++)
        {
            if (slot.used[i])
                waits[waitCount++] = slot.fences[i];
        }

        if (waitCount > 0)
        {
            if (vkWaitForFences(_device, waitCount, waits, VK_TRUE, kWaitFenceNs) != VK_SUCCESS)
            {
                SetReason("The GPU did not finish a frame's copies in time");
                _failures++;
                return false;
            }

            vkResetFences(_device, waitCount, waits);

            for (auto& used : slot.used)
                used = false;
        }
    }

    const auto& velocity = *p.velocity;
    const auto& depth = *p.depth;

    // The copies in: the presented picture, the HUD-less one, motion and depth, into the shared images
    VkCommandBuffer cmd = slot.commands[0];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    native::RecordCopyToShared(cmd, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, s.color, _gameQueueFamily);

    if (useHudless)
        native::RecordCopyToShared(cmd, p.hudless->image, p.hudless->layout, s.hudless, _gameQueueFamily);

    native::RecordCopyToShared(cmd, velocity.image, velocity.layout, s.motion, _gameQueueFamily);

    if (s.depthViaBuffer)
        native::RecordDepthToBuffer(cmd, depth.image, depth.format, depth.layout, depth.imageWidth, depth.imageHeight,
                                    s.depthBuffer.Buffer());
    else
        native::RecordCopyToShared(cmd, depth.image, depth.layout, s.depthImage, _gameQueueFamily);

    vkEndCommandBuffer(cmd);

    // Waits: what the game's present waited on (this submit takes it over), and f5low's last read of the shared images
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;

    for (uint32_t i = 0; i < present->waitSemaphoreCount; i++)
    {
        waitSemaphores.push_back(present->pWaitSemaphores[i]);
        waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        waitValues.push_back(0);
    }

    waitSemaphores.push_back(s.consumed.Semaphore());
    waitStages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);
    waitValues.push_back(s.consumedPrevious);

    const uint64_t copiedValue = s.ready.Next();
    const VkSemaphore signalSemaphores[2] = { s.ready.Semaphore(), slot.inDone };
    const uint64_t signalValues[2] = { copiedValue, 0 };

    VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timeline.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
    timeline.pWaitSemaphoreValues = waitValues.data();
    timeline.signalSemaphoreValueCount = 2;
    timeline.pSignalSemaphoreValues = signalValues;

    VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
    submit.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submit.pWaitSemaphores = waitSemaphores.data();
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 2;
    submit.pSignalSemaphores = signalSemaphores;

    if (const auto result = vkQueueSubmit(queue, 1, &submit, slot.fences[0]); result != VK_SUCCESS)
    {
        SetReason(std::string("vkQueueSubmit failed: ") + std::string(magic_enum::enum_name(result)));
        _failures++;
        return false;
    }

    slot.used[0] = true;

    // From here the game's present waits on our copy in, whatever else happens
    finalInfo.waitSemaphoreCount = 1;
    finalInfo.pWaitSemaphores = &slot.inDone;

    uint64_t readyValue = copiedValue;

    // The depth buffer into its texture on the D3D12 side, after the copy in
    if (s.depthViaBuffer)
    {
        auto* allocator = s.allocators[slotIndex].Get();

        if (!WaitFence12(s.ready.Fence12(), s.allocatorValue[slotIndex], 5000) || FAILED(allocator->Reset()) ||
            FAILED(s.list->Reset(allocator, nullptr)))
        {
            SetReason("The D3D12 depth copy could not be recorded");
            _failures++;
            return true;
        }

        const auto transition = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = s.depthTexture.Get();
            barrier.Transition.StateBefore = from;
            barrier.Transition.StateAfter = to;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s.list->ResourceBarrier(1, &barrier);
        };

        transition(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        native::RecordBufferToDepthTexture(s.list.Get(), s.depthBuffer.Res12(), s.depthTexture.Get(), s.depthFormat,
                                           s.depthWidth, s.depthHeight);
        transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        s.list->Close();

        s.queue12->Wait(s.ready.Fence12(), copiedValue);
        ID3D12CommandList* lists[] = { s.list.Get() };
        s.queue12->ExecuteCommandLists(1, lists);

        readyValue = s.ready.Next();
        s.queue12->Signal(s.ready.Fence12(), readyValue);
        s.allocatorValue[slotIndex] = readyValue;
    }

    // f5low's call: the frames between the previous pair and this one
    F5LowFramesInput input {};
    input.color = s.color.Res12();
    input.hudless = useHudless ? s.hudless.Res12() : nullptr;
    input.motion = s.motion.Res12();
    input.motionWidth = std::min(velocity.width, velocity.imageWidth);
    input.motionHeight = std::min(velocity.height, velocity.imageHeight);
    input.depth = s.depthViaBuffer ? s.depthTexture.Get() : s.depthImage.Res12();
    input.depthWidth = std::min(depth.width, depth.imageWidth);
    input.depthHeight = std::min(depth.height, depth.imageHeight);

    const int index = p.index;
    input.motionScale[0] = _mvScaleX[index] != 0.0f ? _mvScaleX[index] : 1.0f;
    input.motionScale[1] = _mvScaleY[index] != 0.0f ? _mvScaleY[index] : 1.0f;
    input.jitter[0] = _jitterX[index];
    input.jitter[1] = _jitterY[index];
    input.cameraNear = _cameraNear[index];
    input.cameraFar = _cameraFar[index];
    input.cameraFovY = _cameraVFov[index];
    input.depthInverted = IsInvertedDepth() ? 1 : 0;
    input.motionJittered = IsJitteredMVs() ? 1 : 0;
    input.reset = _reset[index] != 0 ? 1 : 0;
    input.frameId = p.frame;
    input.readyFence = s.ready.Fence12();
    input.readyValue = readyValue;
    input.consumedFence = s.consumed.Fence12();
    input.consumedValue = s.consumed.Next();

    F5LowFramesOutput output {};
    output.count = count;

    for (uint32_t k = 0; k < count; k++)
        output.targets[k] = s.out[k].Res12();

    output.doneFence = s.done.Fence12();
    output.doneValue = s.done.Next();

    const auto result = s.generate(s.context, &input, &output);

    if (result != F5LOW_FRAMES_OK)
    {
        // Nothing may wait on values f5low will not signal
        SignalIfBehind(s.consumed.Fence12(), input.consumedValue);
        SignalIfBehind(s.done.Fence12(), output.doneValue);
        s.consumedPrevious = input.consumedValue;

        if (result == F5LOW_FRAMES_NOT_READY)
        {
            if (!s.warnedNotReady)
            {
                s.warnedNotReady = true;
                LOG_INFO("F5Low (Vulkan): the generator is still being built, presenting the game's frames");
            }
        }
        else
        {
            _failures++;
            SetReason(std::format("f5low refused a frame ({}): {}", static_cast<int>(result), s.lastError(s.context)));
        }

        return true;
    }

    s.consumedPrevious = input.consumedValue;
    _failures = 0;
    SetReason("");

    const uint32_t generated = std::min(output.generated, count);

    if (generated == 0)
        return true;

    // The generated frames, one by one, then the game's own: spaced by the game's frame interval
    const auto acquireFn =
        SwapchainFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR", vkAcquireNextImageKHR);
    const auto presentFn = SwapchainFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR", vkQueuePresentKHR);
    const double spacingMs = SpacingMs(_intervalMs, generated);
    const double baseMs = Util::MillisecondsNow();
    bool stopped = false;

    for (uint32_t k = 0; k < generated; k++)
    {
        if (k > 0)
            WaitUntil(baseMs + k * spacingMs);

        uint32_t shownIndex = 0;
        const auto acquired =
            acquireFn(_device, _swapchain, kAcquireNs, slot.acquired[k], VK_NULL_HANDLE, &shownIndex);

        if ((acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) || shownIndex >= s.images.size())
        {
            LOG_WARN("F5Low (Vulkan): no swapchain image for generated frame {} ({})", k,
                     magic_enum::enum_name(acquired));
            stopped = true;
            break;
        }

        VkCommandBuffer outCmd = slot.commands[1 + k];
        vkResetCommandBuffer(outCmd, 0);
        vkBeginCommandBuffer(outCmd, &begin);
        RecordOutput(outCmd, s.out[k], s.images[shownIndex], _gameQueueFamily);
        vkEndCommandBuffer(outCmd);

        const VkSemaphore outWaits[2] = { slot.acquired[k], s.done.Semaphore() };
        const VkPipelineStageFlags outStages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
        const uint64_t outWaitValues[2] = { 0, output.doneValue };
        const uint64_t outSignalValue = 0;

        VkTimelineSemaphoreSubmitInfo outTimeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        outTimeline.waitSemaphoreValueCount = 2;
        outTimeline.pWaitSemaphoreValues = outWaitValues;
        outTimeline.signalSemaphoreValueCount = 1;
        outTimeline.pSignalSemaphoreValues = &outSignalValue;

        VkSubmitInfo outSubmit { VK_STRUCTURE_TYPE_SUBMIT_INFO, &outTimeline };
        outSubmit.waitSemaphoreCount = 2;
        outSubmit.pWaitSemaphores = outWaits;
        outSubmit.pWaitDstStageMask = outStages;
        outSubmit.commandBufferCount = 1;
        outSubmit.pCommandBuffers = &outCmd;
        outSubmit.signalSemaphoreCount = 1;
        outSubmit.pSignalSemaphores = &slot.outDone[k];

        if (const auto submitted = vkQueueSubmit(queue, 1, &outSubmit, slot.fences[1 + k]); submitted != VK_SUCCESS)
        {
            LOG_ERROR("F5Low (Vulkan): vkQueueSubmit for generated frame {} failed: {}", k,
                      magic_enum::enum_name(submitted));
            stopped = true;
            break;
        }

        slot.used[1 + k] = true;

        VkPresentInfoKHR shown { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        shown.waitSemaphoreCount = 1;
        shown.pWaitSemaphores = &slot.outDone[k];
        shown.swapchainCount = 1;
        shown.pSwapchains = &_swapchain;
        shown.pImageIndices = &shownIndex;

        if (const auto presented = presentFn(queue, &shown);
            presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            LOG_WARN("F5Low (Vulkan): presenting generated frame {} returned {}", k, magic_enum::enum_name(presented));
            stopped = true;
            break;
        }
    }

    // The game's frame after the last generated one
    if (!stopped)
        WaitUntil(baseMs + generated * spacingMs);

    if (_generatedFrames == 0)
        LOG_INFO("F5Low (Vulkan): generating {} frame(s) per game frame, frame interval {:.2f} ms", generated,
                 _intervalMs);

    _generatedFrames += generated;
    return true;
}

void F5Low_Vk::ReleaseAll(const char* why)
{
    auto& s = *_impl;

    // At exit nothing is waited for or destroyed
    if (State::Instance().isShuttingDown)
        return;

    if (s.context == nullptr && !s.slotsReady && !s.d3dReady)
        return;

    LOG_INFO("F5Low (Vulkan): releasing ({})", why);

    if (!s.deviceGone && _device != VK_NULL_HANDLE)
        WaitIdle();

    if (s.context != nullptr && s.destroy != nullptr)
        s.destroy(s.context);

    s.context = nullptr;
    s.consumedPrevious = 0;

    if (!s.deviceGone && _device != VK_NULL_HANDLE)
    {
        for (auto& slot : s.slots)
        {
            for (auto fence : slot.fences)
            {
                if (fence != VK_NULL_HANDLE)
                    vkDestroyFence(_device, fence, nullptr);
            }

            if (slot.inDone != VK_NULL_HANDLE)
                vkDestroySemaphore(_device, slot.inDone, nullptr);

            for (uint32_t k = 0; k < kMaxGenerated; k++)
            {
                if (slot.acquired[k] != VK_NULL_HANDLE)
                    vkDestroySemaphore(_device, slot.acquired[k], nullptr);

                if (slot.outDone[k] != VK_NULL_HANDLE)
                    vkDestroySemaphore(_device, slot.outDone[k], nullptr);
            }

            slot = {};
        }

        if (s.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, s.pool, nullptr);
    }

    s.pool = VK_NULL_HANDLE;
    s.slotsReady = false;
    s.cursor = 0;

    s.color.Reset();
    s.hudless.Reset();
    s.motion.Reset();
    s.depthImage.Reset();

    for (auto& image : s.out)
        image.Reset();

    s.depthBuffer.Reset();
    s.depthTexture.Reset();
    s.depthSource = VK_FORMAT_UNDEFINED;
    s.depthWidth = s.depthHeight = 0;
    s.ready.Reset();
    s.consumed.Reset();
    s.done.Reset();
    s.list.Reset();

    for (auto& allocator : s.allocators)
        allocator.Reset();

    for (auto& value : s.allocatorValue)
        value = 0;

    s.queue12.Reset();
    s.device12.Reset();
    s.d3dReady = false;
    s.warnedNotReady = false;
}

void F5Low_Vk::DestroyFGContext()
{
    Deactivate();
    ReleaseAll("frame generation context destroyed");
}

// The swapchain goes with the game's vkDestroySwapchainKHR
bool F5Low_Vk::ReleaseSwapchain(HWND hwnd)
{
    Deactivate();
    return true;
}

bool F5Low_Vk::Shutdown()
{
    Deactivate();
    ReleaseAll("shutdown");
    return true;
}

void F5Low_Vk::OnDeviceDestroying(VkDevice device)
{
    if (device == VK_NULL_HANDLE || device != _device)
        return;

    Deactivate();
    ReleaseAll("the game is destroying its device");
    _impl->deviceGone = true;
    _impl->images.clear();
    _swapchain = VK_NULL_HANDLE;
    _headroom = 0;
}
