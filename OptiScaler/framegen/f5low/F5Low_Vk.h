#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Vk.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// FG Output F5Low (XeFG Tensor) on a native Vulkan game's own swapchain: no bridge, no D3D12 swapchain. The game keeps
// the driver's VkSwapchain (made with spare images and copy usage); on each present the game's picture, motion and
// depth are copied into images shared with a private D3D12 device, f5low.dll's "frames only" API (f5low_frames.h) makes
// the generated frames there, and they are copied back into spare swapchain images and presented before the game's own
// image. The copy in and f5low's call run on the game thread (game queue); the presents run on a worker thread on one
// extra present-capable queue (reserved in the constructor), paced by the measured game frame interval. Without that
// queue the presents and their pacing run on the game thread as before.
class F5Low_Vk : public virtual IFGFeature_Vk
{
  public:
    struct Impl;
    struct Job;

  private:
    std::unique_ptr<Impl> _impl;

    // Why the output cannot run at all (f5low.dll too old, another generator): fixed after the constructor
    std::string _blocked;
    // Why it is not generating now (the last thing that stopped it), shown in the menu; empty when it is fine
    std::string _reason;
    mutable std::mutex _reasonMutex;

    uint32_t _headroom = 0; // swapchain images beyond the game's own
    bool _hdr10 = false;
    VkColorSpaceKHR _colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    // What Present() decided for the game's present that follows
    struct Pending
    {
        bool valid = false;
        int index = -1;
        UINT64 frame = 0;
        std::optional<VkFGResource> depth, velocity, hudless;
    } _pending;

    double _intervalMs = 0.0;
    double _lastEntryMs = 0.0;
    uint64_t _generatedFrames = 0;
    int _failures = 0;
    int _wanted = 1;          // generated frames per game frame the multiplier asks for
    int _countLimit = 5;      // lowered when f5low refuses a larger count
    std::string _nameCache;   // built once in the constructor
    bool _removed = false;    // the D3D12 device was removed: stays so across swapchains
    bool _retiredOld = false; // a failed create retired the game's old swapchain
    bool _loggedBehind = false;

    void SetReason(const std::string& reason);
    void UpdateCount();

    bool Prepare(const Pending& pending, uint32_t count, bool& useHudless, uint32_t set);
    bool EnsureSlots();

    // Our own GPU work and the worker, so a size change or a teardown can go on: the worker drained, every fence of
    // ours waited
    void WaitOwnWork();
    bool DrainWorker(DWORD timeoutMs);
    bool QuiesceWorker(DWORD timeoutMs);
    void LatchOff(const std::string& reason);
    bool CarryPresentStructs(const VkPresentInfoKHR* present, Job& job);
    void StartWorker();
    void StopWorker(bool join);
    void WorkerLoop();

    // The generated frames of a job: acquire, copy, present, spaced. On the worker, or on the game thread without a
    // present queue. Returns after the last generated frame was presented.
    void RunGenerated(const Job& job, bool paced);
    bool AcquireImage(VkSemaphore semaphore, uint32_t* index, double timeoutMs);
    VkResult PresentOn(VkQueue queue, const VkPresentInfoKHR& info, DWORD lockTimeoutMs);
    VkResult DirectPresent(VkQueue queue, const VkPresentInfoKHR* info);

    enum class Outcome
    {
        Direct, // the real present goes out on the game thread (finalInfo)
        Queued  // the worker presents everything
    };
    Outcome Generate(VkQueue queue, const VkPresentInfoKHR* present, Pending& pending, VkPresentInfoKHR& finalInfo);
    void ReleaseAll(const char* why);

  public:
    // IFGFeature
    feature_version Version() override final;
    const char* Name() override final;
    bool Present() override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool ReleaseSwapchain(HWND hwnd) override final;
    bool Shutdown() override final;
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override final { return true; }

    // IFGFeature_Vk: the game's handle is the driver's, VK_NV_low_latency2 calls with it reach the driver
    bool HasDriverSwapchain() const override final { return true; }
    bool WantsLatencyMode() const override final { return true; }
    void OnDeviceDestroying(VkDevice device) override final;
    bool RetiredOldSwapchain() const override final { return _retiredOld; }
    VkResult WaitForPresent(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId,
                            uint64_t timeout) override final;

    bool CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                         const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) override final;
    bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                          const VkAllocationCallbacks* pAllocator) override final;
    VkResult GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                VkImage* pSwapchainImages) override final;
    VkResult AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                              VkFence fence, uint32_t* pImageIndex) override final;
    VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) override final;
    void SetHdrMetadata(VkDevice device, VkSwapchainKHR swapchain, const VkHdrMetadataEXT* pMetadata) override final;

    void EvaluateState(FG_Constants& fgConstants) override final;

    std::string StatusLine() const override final;

    F5Low_Vk();
    ~F5Low_Vk();
};
