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

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace F5LowVk;

namespace
{
constexpr VkImageUsageFlags kCopyUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr uint64_t kWaitFenceNs = 1'000'000'000ull;
constexpr double kAcquireMs = 100.0;
constexpr DWORD kWorkerLockMs = kWorkerLockMsRule;
constexpr double kBlockMs = kBlockMsRule;

// The driver's swapchain calls, past our hooks (a hook would see them as the game's)
template <typename PFN> PFN SwapchainFunction(const char* name, PFN fallback)
{
    if (auto function = reinterpret_cast<PFN>(VulkanHooks::OriginalSwapchainFunction(name)))
        return function;

    return fallback;
}

// A per-thread high-resolution waitable timer (the worker and the game thread both wait)
HANDLE ThreadTimer()
{
    struct Holder
    {
        HANDLE handle =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Holder()
        {
            if (handle != nullptr)
                CloseHandle(handle);
        }
    };
    thread_local Holder holder;
    return holder.handle;
}

// A short sleep that is not a 15 ms scheduler tick
void SleepHighRes(double ms)
{
    if (HANDLE timer = ThreadTimer(); timer != nullptr)
    {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);

        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, 50);
            return;
        }
    }

    std::this_thread::yield();
}

// A CPU wait until a time (Util::MillisecondsNow's clock): the timer for most of it, a spin for the rest
void WaitUntil(double targetMs)
{
    for (;;)
    {
        const double remaining = targetMs - Util::MillisecondsNow();

        if (remaining <= 0.0)
            return;

        if (remaining > 1.5)
            SleepHighRes(remaining - 1.0);
        else
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

// Motion vector formats that cross as they are (a copy cannot convert); f5low converts the float ones itself
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
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        *dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
} // namespace

// One game frame's generated frames, for whoever presents them
struct F5Low_Vk::Job
{
    uint32_t slot = 0;
    uint32_t generated = 0;
    uint64_t doneValue = 0;
    uint32_t realIndex = 0;
    VkSemaphore realWait = VK_NULL_HANDLE;
    double intervalMs = 0.0;
    uint32_t set = 0;
    bool hasPresentId = false;
    uint64_t presentId = 0;
    bool hasFence = false;
    VkFence presentFence = VK_NULL_HANDLE;
    bool hasMode = false;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    bool hasGroup = false; // a single-device VkDeviceGroupPresentInfoKHR
    uint32_t groupCount = 0;
    uint32_t groupMask = 0;
    uint32_t groupMode = 0;
};

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

    // The private D3D12 device (the frame source's) and the queue and list of the depth stage
    native::VkInterop interop;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> allocators[kSlots];
    ComPtr<ID3D12GraphicsCommandList> list;

    // ready: Vulkan signals it when the copies in are done. depthDone: the D3D12 depth stage's own fence (one producer
    // each). consumed and done are f5low's. Every value only ever goes up.
    native::SharedFenceVk ready, consumed, done;
    ComPtr<ID3D12Fence> depthDone;
    FenceCounter depthCounter;
    uint64_t allocatorValue[kSlots] {};

    native::SharedImageVk color, hudless, motion, depthImage;
    native::SharedImageVk out[2][kMaxGenerated]; // two sets, written by f5low on alternate frames
    native::SharedBufferVk depthBuffer;
    ComPtr<ID3D12Resource> depthTexture;
    native::VkDepthCopyFormat depthFormat {};
    bool depthViaBuffer = false;
    VkFormat depthSource = VK_FORMAT_UNDEFINED;
    uint32_t depthWidth = 0, depthHeight = 0;

    // The swapchain's images, in the driver's order
    std::vector<VkImage> images;

    // Per frame in flight. The worker (or the game thread without one) uses the copy out parts of a slot while it is
    // busy; the game thread the rest.
    struct Slot
    {
        VkCommandBuffer commands[kBatches] {};
        VkFence fences[kBatches] {};
        bool used[kBatches] {};
        VkSemaphore inDone = VK_NULL_HANDLE;
        VkSemaphore acquired[kMaxGenerated] {};
        VkSemaphore outDone[kMaxGenerated] {};
    };
    VkCommandPool poolIn = VK_NULL_HANDLE;  // on the game's queue family
    VkCommandPool poolOut = VK_NULL_HANDLE; // on the present queue's family
    Slot slots[kSlots];
    uint32_t cursor = 0;
    uint32_t nextSet = 0;
    int setSlot[2] = { -1, -1 }; // the slot of the job that last used each set of output images
    bool slotsReady = false;
    bool deviceGone = false;
    bool warnedQueue = false;
    bool warnedNotReady = false;
    bool warnedPNext = false;

    // The present queue: our extra one when the device had a free queue, else the game's
    VkQueue outQueue = VK_NULL_HANDLE;
    uint32_t outFamily = UINT32_MAX;
    bool ownQueue = false;

    // The worker thread and its jobs. The swapchain is externally synchronised: swapchainMutex is held around every
    // acquire and present on it (the game's too), never across a wait.
    std::timed_mutex swapchainMutex;
    bool detached = false; // the worker was left behind at exit: this object is never destroyed
    std::set<int> unknownPNext;
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::deque<Job> jobs;
    bool busy = false;
    bool stop = false;
    bool slotBusy[kSlots] {};
    std::thread worker;
    std::atomic<int> lastResult { VK_SUCCESS };

    ~Impl() = default;
};

F5Low_Vk::F5Low_Vk() : IFGFeature_Vk(), IFGFeature(), _impl(std::make_unique<Impl>())
{
    _maxInterpolationCount = F5Low::MaxInterpolations(F5LOW_FRAMES_MAX_GENERATED,
                                                      Config::Instance()->FGXeFGMaxInterpolations.value_or_default());
    _nameCache = "F5Low";
    _generationOff = true; // until a frame really was generated
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

    // The presents of the generated frames run on a queue of their own (PrepareDevice reserves it with the device)
    _extraQueues.resize(1);
    _extraQueues[0].flags = VK_QUEUE_GRAPHICS_BIT;
    _extraQueues[0].present = true;

    LOG_INFO("F5Low (Vulkan): frames-only API found, up to {} generated frames per game frame", _maxInterpolationCount);
}

F5Low_Vk::~F5Low_Vk()
{
    Shutdown();

    // A worker left behind at exit may still use everything in it
    if (_impl->detached)
        (void) _impl.release();
}

feature_version F5Low_Vk::Version()
{
    feature_version version;
    version.major = F5LOW_FRAMES_API_VERSION;
    return version;
}

const char* F5Low_Vk::Name() { return _nameCache.c_str(); }

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
    const int target = Config::Instance()->FGInterpolationCount(
        Config::Instance()->FGXeFGInterpolationCount.value_or_default(), _maxInterpolationCount);

    if (target >= 1 && target != _wanted)
    {
        LOG_INFO("F5Low (Vulkan): interpolation count {} -> {}", _wanted, target);
        _wanted = target;
    }

    // What the multiplier combos, the cap and the status read: the chosen count, as far as the generator and the spare
    // images allow it (what a frame really did is _generationOff and the status line)
    _framesToInterpolate = static_cast<int>(std::max(
        1u, GeneratedCount(_wanted, static_cast<uint32_t>(std::min(_maxInterpolationCount, _countLimit)), _headroom)));
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

// ---- the worker and the swapchain lock ----

bool F5Low_Vk::DrainWorker(DWORD timeoutMs)
{
    auto& s = *_impl;

    if (!s.worker.joinable())
        return true;

    std::unique_lock lock(s.jobMutex);
    return s.jobCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return s.jobs.empty() && !s.busy; });
}

// The worker idle and stopped, for whatever replaces or destroys what it uses. When it does not go idle in time it is
// left alone (running, with its state untouched) and the output latches off.
bool F5Low_Vk::QuiesceWorker(DWORD timeoutMs)
{
    if (!_impl->worker.joinable())
        return true;

    if (!DrainWorker(timeoutMs))
    {
        LatchOff("The present worker did not go idle in time, frame generation is off");
        return false;
    }

    StopWorker(true);
    return true;
}

void F5Low_Vk::LatchOff(const std::string& reason)
{
    _failures = 99;
    _generationOff = true;
    SetReason(reason);
}

void F5Low_Vk::StartWorker()
{
    auto& s = *_impl;

    if (!s.ownQueue || s.worker.joinable() || s.detached)
        return;

    {
        std::lock_guard lock(s.jobMutex);
        s.stop = false;
    }

    s.worker = std::thread([this] { WorkerLoop(); });
}

// join: the caller drained it (QuiesceWorker); without: process exit, the thread is left behind and everything it may
// touch is leaked on purpose (the destructor)
void F5Low_Vk::StopWorker(bool join)
{
    auto& s = *_impl;

    if (!s.worker.joinable())
        return;

    {
        std::lock_guard lock(s.jobMutex);
        s.stop = true;
    }

    s.jobCv.notify_all();

    if (!join)
    {
        s.worker.detach();
        s.detached = true;
        return;
    }

    s.worker.join();

    std::lock_guard lock(s.jobMutex);
    s.busy = false;

    for (auto& busy : s.slotBusy)
        busy = false;
}

void F5Low_Vk::WorkerLoop()
{
    auto& s = *_impl;

    for (;;)
    {
        Job job;

        {
            std::unique_lock lock(s.jobMutex);
            s.jobCv.wait(lock, [&] { return s.stop || !s.jobs.empty(); });

            // Jobs still queued when it is told to stop are presented first
            if (s.jobs.empty())
                return;

            job = s.jobs.front();
            s.jobs.pop_front();
            s.busy = true;
        }

        RunGenerated(job, true);

        // The game's own image after the generated ones, with what the game chained to its present
        VkPresentIdKHR presentId { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &job.presentId;

        VkSwapchainPresentFenceInfoEXT fenceInfo { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT };
        fenceInfo.swapchainCount = 1;
        fenceInfo.pFences = &job.presentFence;

        VkSwapchainPresentModeInfoEXT modeInfo { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT };
        modeInfo.swapchainCount = 1;
        modeInfo.pPresentModes = &job.presentMode;

        VkDeviceGroupPresentInfoKHR groupInfo { VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR };
        groupInfo.swapchainCount = job.groupCount;
        groupInfo.pDeviceMasks = &job.groupMask;
        groupInfo.mode = static_cast<VkDeviceGroupPresentModeFlagBitsKHR>(job.groupMode);

        const void* chain = nullptr;

        if (job.hasGroup)
        {
            groupInfo.pNext = chain;
            chain = &groupInfo;
        }

        if (job.hasPresentId)
        {
            presentId.pNext = chain;
            chain = &presentId;
        }

        if (job.hasFence)
        {
            fenceInfo.pNext = chain;
            chain = &fenceInfo;
        }

        if (job.hasMode)
        {
            modeInfo.pNext = chain;
            chain = &modeInfo;
        }

        VkPresentInfoKHR real { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        real.pNext = chain;
        real.waitSemaphoreCount = 1;
        real.pWaitSemaphores = &job.realWait;
        real.swapchainCount = 1;
        real.pSwapchains = &_swapchain;
        real.pImageIndices = &job.realIndex;

        if (const auto presented = PresentOn(s.outQueue, real, kWorkerLockMs); presented != VK_TIMEOUT)
            s.lastResult = presented;

        {
            std::lock_guard lock(s.jobMutex);
            s.busy = false;
            s.slotBusy[job.slot] = false;
        }

        s.jobCv.notify_all();
    }
}

// VK_TIMEOUT: the swapchain lock was not free in time (nothing was presented)
VkResult F5Low_Vk::PresentOn(VkQueue queue, const VkPresentInfoKHR& info, DWORD lockTimeoutMs)
{
    const auto presentFn = SwapchainFunction<PFN_vkQueuePresentKHR>("vkQueuePresentKHR", vkQueuePresentKHR);
    std::unique_lock lock(_impl->swapchainMutex, std::defer_lock);

    if (!lock.try_lock_for(std::chrono::milliseconds(lockTimeoutMs)))
    {
        LOG_WARN("F5Low (Vulkan): the swapchain lock was not free for {} ms, a present is dropped", lockTimeoutMs);
        return VK_TIMEOUT;
    }

    return presentFn(queue, &info);
}

// The real present on the game's thread: after the worker's jobs (a short wait), so the order of the images holds.
// Never longer than the game thread's budget in all.
VkResult F5Low_Vk::DirectPresent(VkQueue queue, const VkPresentInfoKHR* info)
{
    const double startMs = Util::MillisecondsNow();

    if (!DrainWorker(kBlockMs))
        LOG_DEBUG("F5Low (Vulkan): the worker was still busy, presenting the game's image anyway");

    const auto result = PresentOn(queue, *info, RemainingMs(kBlockMs, Util::MillisecondsNow() - startMs));

    if (result == VK_TIMEOUT)
    {
        // Something holds the swapchain: the frame is dropped rather than the game held, generation stops
        LatchOff("The swapchain lock was held too long (a stuck present), frame generation is off");
        return VK_SUCCESS;
    }

    return result;
}

// The swapchain is externally synchronised and the worker presents to it while the game acquires: an acquire never
// waits inside the lock (a game's blocking acquire would hold it while the images only come back through the worker's
// presents), it polls.
VkResult F5Low_Vk::AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                    VkFence fence, uint32_t* pImageIndex)
{
    const auto acquireFn = SwapchainFunction<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR", vkAcquireNextImageKHR);
    const double startMs = Util::MillisecondsNow();

    for (;;)
    {
        VkResult result = VK_NOT_READY;

        {
            std::unique_lock lock(_impl->swapchainMutex, std::defer_lock);

            if (lock.try_lock_for(std::chrono::milliseconds(2)))
                result = acquireFn(device, swapchain, 0, semaphore, fence, pImageIndex);
        }

        if (result != VK_NOT_READY && result != VK_TIMEOUT)
            return result;

        if (timeout == 0)
            return VK_NOT_READY;

        if (timeout != UINT64_MAX && (Util::MillisecondsNow() - startMs) * 1e6 >= static_cast<double>(timeout))
            return VK_TIMEOUT;

        SleepHighRes(0.3);
    }
}

// vkWaitForPresentKHR on the swapchain the worker presents to: a poll under the lock, the wait outside it
VkResult F5Low_Vk::WaitForPresent(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout)
{
    const auto waitFn = SwapchainFunction<PFN_vkWaitForPresentKHR>("vkWaitForPresentKHR", nullptr);

    if (waitFn == nullptr)
        return VK_SUCCESS;

    const double startMs = Util::MillisecondsNow();

    for (;;)
    {
        VkResult result = VK_TIMEOUT;

        {
            std::unique_lock lock(_impl->swapchainMutex, std::defer_lock);

            if (lock.try_lock_for(std::chrono::milliseconds(2)))
                result = waitFn(device, swapchain, presentId, 0);
        }

        if (result != VK_TIMEOUT)
            return result;

        if (timeout != UINT64_MAX && (Util::MillisecondsNow() - startMs) * 1e6 >= static_cast<double>(timeout))
            return VK_TIMEOUT;

        SleepHighRes(0.5);
    }
}

bool F5Low_Vk::AcquireImage(VkSemaphore semaphore, uint32_t* index, double timeoutMs)
{
    const auto result =
        AcquireNextImage(_device, _swapchain, static_cast<uint64_t>(timeoutMs * 1e6), semaphore, VK_NULL_HANDLE, index);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

// What the game chained to its present that the real present of a generated frame carries over: the present id, the
// present fence and the present mode (one swapchain). Any other struct makes this frame pass through.
bool F5Low_Vk::CarryPresentStructs(const VkPresentInfoKHR* present, Job& job)
{
    for (auto* node = static_cast<const VkBaseInStructure*>(present->pNext); node != nullptr; node = node->pNext)
    {
        switch (node->sType)
        {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
        {
            const auto* id = reinterpret_cast<const VkPresentIdKHR*>(node);

            if (id->swapchainCount >= 1 && id->pPresentIds != nullptr)
            {
                job.hasPresentId = true;
                job.presentId = id->pPresentIds[0];
            }
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:
        {
            const auto* fence = reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(node);

            if (fence->swapchainCount >= 1 && fence->pFences != nullptr)
            {
                job.hasFence = true;
                job.presentFence = fence->pFences[0];
            }
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT:
        {
            const auto* mode = reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(node);

            if (mode->swapchainCount >= 1 && mode->pPresentModes != nullptr)
            {
                job.hasMode = true;
                job.presentMode = mode->pPresentModes[0];
            }
            break;
        }
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR:
        {
            const auto* group = reinterpret_cast<const VkDeviceGroupPresentInfoKHR*>(node);
            const uint32_t mask =
                group->swapchainCount >= 1 && group->pDeviceMasks != nullptr ? group->pDeviceMasks[0] : 0;

            // One device only: its mask goes to every present of the frame
            if (!DeviceGroupCarryable(group->swapchainCount, static_cast<uint32_t>(group->mode), mask))
            {
                if (_impl->unknownPNext.insert(static_cast<int>(node->sType)).second)
                    LOG_INFO("F5Low (Vulkan): the game's device group present info is not a single device (swapchains "
                             "{}, mode {}, mask {}), those frames pass through",
                             group->swapchainCount, static_cast<uint32_t>(group->mode), mask);
                return false;
            }

            job.hasGroup = true;
            job.groupCount = group->swapchainCount;
            job.groupMask = mask;
            job.groupMode = static_cast<uint32_t>(group->mode);
            break;
        }
        default:
            if (_impl->unknownPNext.insert(static_cast<int>(node->sType)).second)
                LOG_INFO("F5Low (Vulkan): the game chains a present struct of type {} that cannot be carried to the "
                         "real present of a generated frame, those frames pass through",
                         static_cast<int>(node->sType));
            return false;
        }
    }

    return true;
}

// ---- the swapchain ----

bool F5Low_Vk::CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    _retiredOld = false;

    if (!_blocked.empty())
        return false;

    if (device != _device || pCreateInfo == nullptr || pSwapchain == nullptr)
        return false;

    auto& s = *_impl;

    // The old swapchain and the surface are externally synchronised: nothing of ours presents to them while the driver
    // replaces them (this create, and the hook's own one if ours fails). The worker comes back with the next frame.
    if (!QuiesceWorker(2000))
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

    UpdateCount();

    // Spare images for the frames we generate (one per generated frame of the multiplier). The game's pNext chain (the
    // latency mode among it) stays as it came.
    VkSwapchainCreateInfoKHR info = *pCreateInfo;
    info.minImageCount =
        RequestedMinImageCount(pCreateInfo->minImageCount, static_cast<uint32_t>(_wanted), caps.maxImageCount);

    VkResult result;

    {
        std::lock_guard lock(s.swapchainMutex);
        result = createFn(device, &info, pAllocator, pSwapchain);

        // A failed create retires the old swapchain (VUID-VkSwapchainCreateInfoKHR-oldSwapchain-01933): the second try,
        // and the hook's if this one fails, are made without it
        if (result != VK_SUCCESS && pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
            _retiredOld = true;

        if (result != VK_SUCCESS && info.minImageCount != pCreateInfo->minImageCount)
        {
            LOG_WARN("F5Low (Vulkan): {} swapchain images were refused ({}), trying the game's own count",
                     info.minImageCount, magic_enum::enum_name(result));
            info.minImageCount = pCreateInfo->minImageCount;
            info.oldSwapchain = VK_NULL_HANDLE;
            result = createFn(device, &info, pAllocator, pSwapchain);
        }
    }

    if (result != VK_SUCCESS)
    {
        LOG_ERROR("F5Low (Vulkan): vkCreateSwapchainKHR error: {}", magic_enum::enum_name(result));
        return false;
    }

    s.deviceGone = false;
    s.lastResult = VK_SUCCESS;
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
    _intervalMs = 0.0;
    _lastEntryMs = 0.0;
    _generationOff = true;
    _loggedBehind = false;
    _loggedGenerating = false;
    _createdWanted = _wanted;
    _askedWanted = 0;
    s.nextSet = 0;
    s.setSlot[0] = s.setSlot[1] = -1;

    // A removed device stays removed; the other latches are re-armed with the new swapchain
    _failures = _removed ? 3 : 0;

    {
        OwnedLockGuard lock(Mutex, 3);
        _pending = {};
    }

    UpdateCount();

    if (_removed)
        SetReason("The D3D12 device was removed");
    else if (_headroom == 0)
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
    auto& s = *_impl;
    const bool quiet = QuiesceWorker(2000);

    if (quiet)
    {
        if (device == _device && !s.deviceGone)
        {
            WaitOwnWork();

            if (s.outQueue != VK_NULL_HANDLE && s.outQueue != _gameQueue)
                vkQueueWaitIdle(s.outQueue);
        }

        // A worker that did not stop still reads them
        s.images.clear();
    }

    _swapchain = VK_NULL_HANDLE;
    _headroom = 0;
    _generationOff = true;
    return false;
}

VkResult F5Low_Vk::GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                      VkImage* pSwapchainImages)
{
    return SwapchainFunction<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR)(
        device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

void F5Low_Vk::SetHdrMetadata(VkDevice device, VkSwapchainKHR swapchain, const VkHdrMetadataEXT* pMetadata)
{
    // The hook set _inFGCall around this, so the call goes straight to the driver
    const auto setHdr =
        reinterpret_cast<PFN_vkSetHdrMetadataEXT>(VulkanHooks::GetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));

    if (setHdr != nullptr)
        setHdr(device, 1, &swapchain, pMetadata);
}

// ---- per frame ----

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
    // The time between two game presents. The game thread is not held by our pacing (the worker paces), so this is the
    // game's own cadence.
    const double entryMs = Util::MillisecondsNow();

    if (_lastEntryMs > 0.0)
        _intervalMs = NextInterval(_intervalMs, entryMs - _lastEntryMs);

    _lastEntryMs = entryMs;

    Pending pending;

    {
        OwnedLockGuard lock(Mutex, 3);
        pending = std::move(_pending);
        _pending = {};
    }

    // No spare image: nothing to copy, nothing to ask of f5low
    if (!pending.valid || !_blocked.empty() || pPresentInfo->swapchainCount != 1 ||
        pPresentInfo->pSwapchains[0] != _swapchain || _failures >= 3 || _headroom == 0)
    {
        _generationOff = true;
        return AskRecreateIfRaised(DirectPresent(queue, pPresentInfo));
    }

    if (queue != _gameQueue)
    {
        if (!_impl->warnedQueue)
        {
            _impl->warnedQueue = true;
            SetReason("The game presents from a different queue than its first graphics queue");
        }

        _generationOff = true;
        return AskRecreateIfRaised(DirectPresent(queue, pPresentInfo));
    }

    VkPresentInfoKHR finalInfo = *pPresentInfo;

    if (Generate(queue, pPresentInfo, pending, finalInfo) == Outcome::Queued)
        return AskRecreateIfRaised(static_cast<VkResult>(_impl->lastResult.load()));

    return AskRecreateIfRaised(DirectPresent(queue, &finalInfo));
}

// The multiplier was raised past the spare images of this swapchain: the game is told its swapchain is suboptimal once,
// so it makes a new one (CreateSwapchain then asks the driver for the new count). Only on top of a present that went
// through; the result is not kept anywhere else, so it is never taken for the driver's.
VkResult F5Low_Vk::AskRecreateIfRaised(VkResult result)
{
    if (result != VK_SUCCESS || !AskRecreate(_wanted, _createdWanted, _headroom, _askedWanted))
        return result;

    _askedWanted = _wanted;
    LOG_INFO("F5Low (Vulkan): multiplier {}x needs more spare swapchain images ({} now), asking the game to make a new "
             "swapchain",
             _wanted + 1, _headroom);
    return VK_SUBOPTIMAL_KHR;
}

void F5Low_Vk::WaitOwnWork()
{
    auto& s = *_impl;

    DrainWorker(2000);

    if (_device != VK_NULL_HANDLE && !s.deviceGone)
    {
        for (auto& slot : s.slots)
        {
            VkFence waits[kBatches];
            uint32_t count = 0;

            for (uint32_t i = 0; i < kBatches; i++)
            {
                if (slot.used[i] && slot.fences[i] != VK_NULL_HANDLE)
                    waits[count++] = slot.fences[i];
            }

            if (count > 0)
                vkWaitForFences(_device, count, waits, VK_TRUE, kWaitFenceNs);
        }
    }

    WaitFence12(s.consumed.Fence12(), s.consumed.Value(), 5000);
    WaitFence12(s.done.Fence12(), s.done.Value(), 5000);
    WaitFence12(s.ready.Fence12(), s.ready.Value(), 5000);
    WaitFence12(s.depthDone.Get(), s.depthCounter.issued, 5000);
}

bool F5Low_Vk::EnsureSlots()
{
    auto& s = *_impl;

    if (s.slotsReady)
        return true;

    // The present queue: the extra one PrepareDevice reserved, else the game's (the presents then run on the game
    // thread)
    VkQueue extra = _extraQueues.empty() ? VK_NULL_HANDLE : _extraQueues[0].queue;
    // Swapchain images owned by another queue family would need an ownership transfer: the game's family or none
    const bool sameFamily = extra != VK_NULL_HANDLE && _extraQueues[0].family == _gameQueueFamily;

    if (extra != VK_NULL_HANDLE && !sameFamily)
        LOG_INFO("F5Low (Vulkan): the reserved queue is in family {}, not the game's {}: presents on the game thread",
                 _extraQueues[0].family, _gameQueueFamily);

    s.ownQueue = sameFamily;
    s.outQueue = s.ownQueue ? extra : _gameQueue;
    s.outFamily = _gameQueueFamily;

    if (s.ownQueue)
        LOG_INFO("F5Low (Vulkan): presents on a worker thread, queue family {} index {}", _extraQueues[0].family,
                 _extraQueues[0].index);
    else
        LOG_WARN("F5Low (Vulkan): no extra queue could be reserved, the presents and their pacing run on the game "
                 "thread");

    VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = _gameQueueFamily;
    bool ok = vkCreateCommandPool(_device, &poolInfo, nullptr, &s.poolIn) == VK_SUCCESS;
    poolInfo.queueFamilyIndex = s.outFamily;
    ok = ok && vkCreateCommandPool(_device, &poolInfo, nullptr, &s.poolOut) == VK_SUCCESS;

    VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

    for (auto& slot : s.slots)
    {
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        allocate.commandPool = s.poolIn;
        ok = ok && vkAllocateCommandBuffers(_device, &allocate, &slot.commands[0]) == VK_SUCCESS;
        allocate.commandPool = s.poolOut;
        allocate.commandBufferCount = kMaxGenerated;
        ok = ok && vkAllocateCommandBuffers(_device, &allocate, &slot.commands[1]) == VK_SUCCESS;

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

    StartWorker();
    return true;
}

bool F5Low_Vk::Prepare(const Pending& p, uint32_t count, bool& useHudless, uint32_t set)
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

        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocators[0].Get(),
                                                       nullptr, IID_PPV_ARGS(&s.list)));

        if (ok)
            s.list->Close();

        ok = ok && s.ready.Create(device, s.interop) && s.consumed.Create(device, s.interop) &&
             s.done.Create(device, s.interop) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.depthDone)));
        s.depthCounter = {};

        if (!ok)
        {
            if (why.empty())
                why = !s.ready.Error().empty() ? s.ready.Error() : "creating the D3D12 queue, list or fences failed";

            SetReason("Sharing with the D3D12 device failed: " + why);
            s.ready.Reset();
            s.consumed.Reset();
            s.done.Reset();
            s.depthDone.Reset();
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
        WaitOwnWork();
        s.hudless.Reset();

        for (auto& outSet : s.out)
        {
            for (auto& image : outSet)
                image.Reset();
        }

        if (!s.color.Create(s.device12.Get(), s.interop, width, height, dxgiColor, vkColor, false))
        {
            SetReason("Sharing the picture failed: " + s.color.Error());
            return false;
        }
    }

    if (s.context != nullptr && (s.contextWidth != width || s.contextHeight != height || s.contextFormat != dxgiColor ||
                                 ((s.contextFlags ^ flags) & F5LOW_FRAMES_FLAG_HDR10) != 0))
    {
        WaitOwnWork();
        const auto result = s.resize(s.context, width, height, dxgiColor, flags);

        // A failed resize is permanent: the context is made again with the next frame
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
        info.renderWidth = std::min(p.velocity->width, width);
        info.renderHeight = std::min(p.velocity->height, height);

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
        if (s.out[set][k].Matches(width, height, vkColor))
            continue;

        if (!s.out[set][k].Create(s.device12.Get(), s.interop, width, height, dxgiColor, vkColor, true))
        {
            SetReason("Sharing a generated frame failed: " + s.out[set][k].Error());
            return false;
        }
    }

    // The game's HUD-less picture, when it is the swapchain's size and format
    if (const auto& hudless = p.hudless; hudless && hudless->image != VK_NULL_HANDLE && UsableLayout(hudless->layout) &&
                                         hudless->imageWidth == width && hudless->imageHeight == height)
    {
        DXGI_FORMAT hudlessDxgi = DXGI_FORMAT_UNKNOWN;
        VkFormat hudlessShared = VK_FORMAT_UNDEFINED;

        if (native::VkSharedPictureFormat(hudless->format, &hudlessDxgi, &hudlessShared) && hudlessShared == vkColor)
        {
            if (!s.hudless.Matches(width, height, vkColor))
            {
                WaitOwnWork();

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
        WaitOwnWork();

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
            WaitOwnWork();

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
            WaitOwnWork();
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
            LOG_INFO("F5Low (Vulkan): sharing the depth at {}x{}, Vulkan format {} ({})", s.depthWidth, s.depthHeight,
                     static_cast<int>(depth.format), magic_enum::enum_name(depth.format));
        }
    }

    return true;
}

// The generated frames of one job, spaced by the game's frame interval: for each, acquire an image, copy the shared
// output into it, present. The pacing waits are in here and nowhere that holds the swapchain lock.
void F5Low_Vk::RunGenerated(const Job& job, bool paced)
{
    auto& s = *_impl;
    auto& slot = s.slots[job.slot];
    const double spacingMs = SpacingMs(job.intervalMs, job.generated);
    const double baseMs = Util::MillisecondsNow();
    bool complete = true;

    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    for (uint32_t k = 0; k < job.generated; k++)
    {
        if (paced && k > 0)
            WaitUntil(baseMs + k * spacingMs);

        uint32_t shownIndex = 0;

        if (!AcquireImage(slot.acquired[k], &shownIndex, kAcquireMs) || shownIndex >= s.images.size())
        {
            LOG_WARN("F5Low (Vulkan): no swapchain image for generated frame {}", k);
            complete = false;
            break;
        }

        VkCommandBuffer cmd = slot.commands[1 + k];
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        RecordOutput(cmd, s.out[job.set][k], s.images[shownIndex], s.outFamily);
        vkEndCommandBuffer(cmd);

        const VkSemaphore waits[2] = { slot.acquired[k], s.done.Semaphore() };
        const VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
        const uint64_t waitValues[2] = { 0, job.doneValue };
        const uint64_t signalValue = 0;

        VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timeline.waitSemaphoreValueCount = 2;
        timeline.pWaitSemaphoreValues = waitValues;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &signalValue;

        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
        submit.waitSemaphoreCount = 2;
        submit.pWaitSemaphores = waits;
        submit.pWaitDstStageMask = stages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &slot.outDone[k];

        const auto submitted = vkQueueSubmit(s.outQueue, 1, &submit, slot.fences[1 + k]);
        slot.used[1 + k] = submitted == VK_SUCCESS;

        // The game's device group present info (one device) goes with every present of the frame
        VkDeviceGroupPresentInfoKHR groupInfo { VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR };
        groupInfo.swapchainCount = job.groupCount;
        groupInfo.pDeviceMasks = &job.groupMask;
        groupInfo.mode = static_cast<VkDeviceGroupPresentModeFlagBitsKHR>(job.groupMode);

        VkPresentInfoKHR shown { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        shown.pNext = job.hasGroup ? &groupInfo : nullptr;
        shown.waitSemaphoreCount = 1;
        shown.swapchainCount = 1;
        shown.pSwapchains = &_swapchain;
        shown.pImageIndices = &shownIndex;

        if (submitted != VK_SUCCESS)
        {
            // The acquired image goes back through a present (waiting for its acquire semaphore) so it is not lost
            LOG_ERROR("F5Low (Vulkan): vkQueueSubmit for generated frame {} failed: {}", k,
                      magic_enum::enum_name(submitted));
            shown.pWaitSemaphores = &slot.acquired[k];
            PresentOn(s.outQueue, shown, kWorkerLockMs);
            complete = false;
            break;
        }

        shown.pWaitSemaphores = &slot.outDone[k];

        if (const auto presented = PresentOn(s.outQueue, shown, kWorkerLockMs);
            presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        {
            LOG_WARN("F5Low (Vulkan): presenting generated frame {} returned {}", k, magic_enum::enum_name(presented));
            s.lastResult = presented;
            complete = false;
            break;
        }
    }

    // The game's frame follows after the last generated one's slot
    if (paced && complete)
        WaitUntil(baseMs + job.generated * spacingMs);
}

F5Low_Vk::Outcome F5Low_Vk::Generate(VkQueue queue, const VkPresentInfoKHR* present, Pending& p,
                                     VkPresentInfoKHR& finalInfo)
{
    auto& s = *_impl;
    const double startMs = Util::MillisecondsNow();

    // What this thread may wait for in one present, in all
    const auto leftMs = [&] { return RemainingMs(kBlockMs, Util::MillisecondsNow() - startMs); };

    // Assumed off until a frame really was generated: the cap and the status follow what happens
    _generationOff = true;

    if (!p.depth || !p.velocity || s.images.empty() || present->pImageIndices == nullptr)
        return Outcome::Direct;

    const uint32_t imageIndex = present->pImageIndices[0];

    if (imageIndex >= s.images.size())
        return Outcome::Direct;

    // What the game chained to its present has to be carried by the real present of a generated frame
    Job job;

    if (!CarryPresentStructs(present, job))
        return Outcome::Direct;

    const VkImage source = s.images[imageIndex];

    uint32_t count =
        GeneratedCount(_wanted, static_cast<uint32_t>(std::min(_maxInterpolationCount, _countLimit)), _headroom);

    // Until the frame interval is known the generator only takes the frames as history
    if (!CanPace(_intervalMs, count))
        count = 0;

    // Two sets of output images: the one this frame writes was last used by the job before the previous one
    const uint32_t set = s.nextSet;
    bool useHudless = false;

    if (!Prepare(p, count, useHudless, set) || !EnsureSlots())
    {
        _failures++;
        return Outcome::Direct;
    }

    StartWorker();

    // f5low's fences: the game queue never waits on a value that will not arrive (a removed device, a stuck generator)
    {
        const auto consumedDone = s.consumed.Fence12()->GetCompletedValue();
        const auto doneDone = s.done.Fence12()->GetCompletedValue();

        if (FenceBehind(consumedDone, s.consumed.Value()) || FenceBehind(doneDone, s.done.Value()))
        {
            if (consumedDone == UINT64_MAX || doneDone == UINT64_MAX)
            {
                _removed = true;
                LatchOff("The D3D12 device was removed");
            }
            else if (!_loggedBehind)
            {
                _loggedBehind = true;
                LOG_WARN("F5Low (Vulkan): f5low is more than a frame behind, passing frames through until it "
                         "catches up");
            }

            return Outcome::Direct;
        }
    }

    const uint32_t slotIndex = s.cursor;
    auto& slot = s.slots[slotIndex];

    // The slot's last use is over: its command buffers and semaphores are ours again
    {
        std::unique_lock lock(s.jobMutex);

        if (!s.jobCv.wait_for(lock, std::chrono::milliseconds(leftMs()), [&] { return !s.slotBusy[slotIndex]; }))
        {
            LatchOff("The present worker did not finish a frame in time, frame generation is off");
            return Outcome::Direct;
        }
    }

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
            if (vkWaitForFences(_device, waitCount, waits, VK_TRUE, static_cast<uint64_t>(leftMs()) * 1'000'000ull) !=
                VK_SUCCESS)
            {
                LatchOff("The GPU did not finish a frame's copies in time, frame generation is off");
                return Outcome::Direct;
            }

            vkResetFences(_device, waitCount, waits);

            for (auto& used : slot.used)
                used = false;
        }
    }

    s.cursor = (s.cursor + 1) % kSlots;

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
    waitValues.push_back(s.consumed.Value());

    // Taken once the submit went through
    const uint64_t copiedValue = s.ready.Value() + 1;
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
        return Outcome::Direct;
    }

    s.ready.Next();
    slot.used[0] = true;

    // From here the game's present waits on our copy in, whatever else happens
    finalInfo.waitSemaphoreCount = 1;
    finalInfo.pWaitSemaphores = &slot.inDone;

    uint64_t readyValue = copiedValue;
    ID3D12Fence* readyFence = s.ready.Fence12();

    // The depth buffer into its texture on the D3D12 side, after the copy in; its own fence tells f5low it is there
    if (s.depthViaBuffer)
    {
        auto* allocator = s.allocators[slotIndex].Get();

        if (!WaitFence12(s.depthDone.Get(), s.allocatorValue[slotIndex], leftMs()) || FAILED(allocator->Reset()) ||
            FAILED(s.list->Reset(allocator, nullptr)))
        {
            SetReason("The D3D12 depth copy could not be recorded");
            _failures++;
            return Outcome::Direct;
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

        readyValue = s.depthCounter.Take();
        s.queue12->Signal(s.depthDone.Get(), readyValue);
        s.allocatorValue[slotIndex] = readyValue;
        readyFence = s.depthDone.Get();
    }

    // The job two frames back (the last one to write this set of output images) is done with them before f5low writes
    // them again
    if (count > 0 && s.setSlot[set] >= 0)
    {
        const uint32_t previous = static_cast<uint32_t>(s.setSlot[set]);
        {
            std::unique_lock lock(s.jobMutex);

            if (!s.jobCv.wait_for(lock, std::chrono::milliseconds(leftMs()), [&] { return !s.slotBusy[previous]; }))
            {
                LatchOff("The present worker did not finish a frame in time, frame generation is off");
                return Outcome::Direct;
            }
        }

        VkFence waits[kMaxGenerated];
        uint32_t waitCount = 0;

        for (uint32_t i = 0; i < kMaxGenerated; i++)
        {
            if (s.slots[previous].used[1 + i])
                waits[waitCount++] = s.slots[previous].fences[1 + i];
        }

        if (waitCount > 0 && vkWaitForFences(_device, waitCount, waits, VK_TRUE,
                                             static_cast<uint64_t>(leftMs()) * 1'000'000ull) != VK_SUCCESS)
        {
            LatchOff("The GPU did not finish the last generated frames in time, frame generation is off");
            return Outcome::Direct;
        }
    }

    // f5low's call: the frames between the previous pair and this one
    F5LowFramesInput input {};
    input.color = s.color.Res12();
    input.hudless = useHudless ? s.hudless.Res12() : nullptr;
    input.motion = s.motion.Res12();
    input.motionWidth = std::min({ velocity.width, velocity.imageWidth, _swapchainExtent.width });
    input.motionHeight = std::min({ velocity.height, velocity.imageHeight, _swapchainExtent.height });
    input.depth = s.depthViaBuffer ? s.depthTexture.Get() : s.depthImage.Res12();
    input.depthWidth = std::min({ depth.width, depth.imageWidth, _swapchainExtent.width });
    input.depthHeight = std::min({ depth.height, depth.imageHeight, _swapchainExtent.height });

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
    input.readyFence = readyFence;
    input.readyValue = readyValue;
    input.consumedFence = s.consumed.Fence12();
    input.consumedValue = s.consumed.Value() + 1;

    F5LowFramesOutput output {};
    output.count = count;

    for (uint32_t k = 0; k < count; k++)
        output.targets[k] = s.out[set][k].Res12();

    output.doneFence = s.done.Fence12();
    output.doneValue = s.done.Value() + 1;

    const auto result = s.generate(s.context, &input, &output);

    // f5low signals both fences for every answer (also NOT_READY and errors), in order on its queue
    s.consumed.Next();
    s.done.Next();

    if (result != F5LOW_FRAMES_OK)
    {
        if (result == F5LOW_FRAMES_NOT_READY)
        {
            if (!s.warnedNotReady)
            {
                s.warnedNotReady = true;
                LOG_INFO("F5Low (Vulkan): the generator is still being built or busy, presenting the game's frames");
            }
        }
        else if (result == F5LOW_FRAMES_ERROR_UNSUPPORTED && count > 1)
        {
            _countLimit = 1;
            LOG_WARN("F5Low (Vulkan): this generator only makes one frame per game frame, limiting to 2x");
        }
        else
        {
            _failures++;
            SetReason(std::format("f5low refused a frame ({}): {}", static_cast<int>(result), s.lastError(s.context)));
        }
    }
    else
    {
        _failures = 0;
        SetReason(static_cast<uint32_t>(_wanted) > _headroom
                      ? std::format("Only {} spare swapchain images: generating fewer frames than the multiplier asks "
                                    "for until the game makes a new swapchain",
                                    _headroom)
                      : std::string());
    }

    const uint32_t generated = GeneratedAfter(result, output.generated, count);

    if (generated == 0)
        return Outcome::Direct;

    _generationOff = false;

    if (!_loggedGenerating)
    {
        _loggedGenerating = true;
        LOG_INFO("F5Low (Vulkan): generating {} frame(s) per game frame, frame interval {:.2f} ms", generated,
                 _intervalMs);
    }

    job.slot = slotIndex;
    job.set = set;
    job.generated = generated;
    job.doneValue = output.doneValue;
    job.realIndex = imageIndex;
    job.realWait = slot.inDone;
    job.intervalMs = _intervalMs;

    s.setSlot[set] = static_cast<int>(slotIndex);
    s.nextSet ^= 1;

    if (s.worker.joinable())
    {
        {
            std::lock_guard lock(s.jobMutex);
            s.jobs.push_back(job);
            s.slotBusy[slotIndex] = true;
        }

        s.jobCv.notify_all();
        return Outcome::Queued;
    }

    // No present queue of our own: the presents and their pacing run here, the real image goes out after
    RunGenerated(job, true);
    return Outcome::Direct;
}

void F5Low_Vk::ReleaseAll(const char* why)
{
    auto& s = *_impl;

    // At exit nothing is waited for or destroyed
    if (State::Instance().isShuttingDown)
    {
        StopWorker(false);
        return;
    }

    // A worker that does not go idle still uses all of it: left running, nothing is destroyed
    if (!QuiesceWorker(2000))
    {
        StopWorker(false);
        return;
    }

    if (s.context == nullptr && !s.slotsReady && !s.d3dReady)
        return;

    LOG_INFO("F5Low (Vulkan): releasing ({})", why);

    if (!s.deviceGone && _device != VK_NULL_HANDLE)
    {
        WaitOwnWork();

        if (s.outQueue != VK_NULL_HANDLE && s.outQueue != _gameQueue)
            vkQueueWaitIdle(s.outQueue);
    }

    if (s.context != nullptr && s.destroy != nullptr)
        s.destroy(s.context);

    s.context = nullptr;

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

        if (s.poolIn != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, s.poolIn, nullptr);

        if (s.poolOut != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, s.poolOut, nullptr);
    }

    s.poolIn = VK_NULL_HANDLE;
    s.poolOut = VK_NULL_HANDLE;
    s.slotsReady = false;
    s.cursor = 0;
    s.nextSet = 0;
    s.setSlot[0] = s.setSlot[1] = -1;
    s.outQueue = VK_NULL_HANDLE;
    s.ownQueue = false;

    s.color.Reset();
    s.hudless.Reset();
    s.motion.Reset();
    s.depthImage.Reset();

    for (auto& outSet : s.out)
    {
        for (auto& image : outSet)
            image.Reset();
    }

    s.depthBuffer.Reset();
    s.depthTexture.Reset();
    s.depthSource = VK_FORMAT_UNDEFINED;
    s.depthWidth = s.depthHeight = 0;
    s.ready.Reset();
    s.consumed.Reset();
    s.done.Reset();
    s.depthDone.Reset();
    s.depthCounter = {};
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
