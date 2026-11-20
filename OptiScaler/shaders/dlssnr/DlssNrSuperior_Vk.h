#pragma once

// Superior Scaling on native Vulkan: the dispatches of its No DLSS composition (SPIR-V built by
// tools/compile_superior_vk.ps1 from the same sources as the D3D12 blobs). What the frame does with them is
// dlssnr/DlssNr_Superior_Vk.inl.
//
// One descriptor layout serves every pass, the one of the composition pass (DlssNr_Vk): binding 0 the constants, 1-4 four
// sampled reads (t0-t3), 5-6 two storage images (u0-u1), 7 the linear sampler, 8 a fifth read (t5: the low field of the
// resolve). Each blob uses the bindings it declares, as numbered in its source under VK_MODE, and leaves the rest.
//
// As the other Vulkan passes: every binding is written every dispatch (a slot a pass does not use gets a 1x1
// placeholder), constants and descriptor sets come from a ring of slots so the dispatches of one frame and of the frames
// still in flight never share one, the layouts of the images are the caller's to state, and each dispatch is followed by a
// compute-to-compute barrier. Compute and nothing else: no blits, no clears, no draws, so it is legal on the async-compute
// queue a game may present from.

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

#include <array>

class DlssNrSuperior_Vk : public Shader_Vk
{
  public:
    enum class Blob : uint32_t
    {
        Area,                         // v59 Area downsample of the encoded proxy to the model's raster
        SurfaceGuides,                // depth and motion matched to the model's raster, no depth
        JointGuides,                  // ... with depth
        JointMinifiedGuides,          // the correction pass after JointGuides where the guides shrink
        JointGuidesQuantized,         // JointGuides for a 16/24-bit integer depth
        JointMinifiedGuidesQuantized, // its correction pass
        LowRaw,                       // the 1/8-size low field of the model's answer minus its input
        ResolveGuidedRaw,             // the final composition of a raw answer, shaped by the low field
        Count
    };

    // A sampled read: the view, and the layout its image is in when the dispatch runs. A null view binds the placeholder.
    struct Read
    {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    // t0, t1, t2, t3 (bindings 1-4) and the fifth read (binding 8).
    static constexpr uint32_t kReads = 5;
    using Reads = std::array<Read, kReads>;

    // The dispatches of one frame: area, guides (one slot for the pair), low field, resolve; eight leaves room. Six frames
    // are twice the three that can be in flight, with no fence, on the assumption of one NR evaluate per rendered frame.
    static constexpr uint32_t kSlotsPerFrame = 8;
    static constexpr uint32_t kFramesInFlight = 6;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    DlssNrSuperior_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~DlssNrSuperior_Vk();

    bool Ready() const { return _init && !_blobsFailed; }

    // Whether this device can run the low field pass (its tile needs 29,376 bytes of shared memory).
    static bool DeviceCanRun(VkPhysicalDevice physicalDevice);

    // One dispatch of `blob`. InConstants is copied into the pass's slot (at most 512 bytes: the Superior constants are the
    // first 256 of the live struct, which is 512); the written images are InOut0 (u0, binding 5) and InOut1 (u1, binding
    // 6) in GENERAL. groupsX x groupsY are thread groups: the callers know each blob's group size.
    bool Dispatch(VkCommandBuffer InCmdList, Blob InBlob, const void* InConstants, size_t InConstantBytes,
                  uint32_t InGroupsX, uint32_t InGroupsY, const Reads& InReads, VkImageView InOut0,
                  VkImageView InOut1 = VK_NULL_HANDLE);

    // The guide pair: `InPrimary` over groupsX x groupsY, then, if InCorrection is not Count, the correction blob over
    // its own groups, both on one set of constants and bindings and with a barrier between.
    bool DispatchGuides(VkCommandBuffer InCmdList, Blob InPrimary, Blob InCorrection, const void* InConstants,
                        size_t InConstantBytes, uint32_t InGroupsX, uint32_t InGroupsY, uint32_t InCorrectionX,
                        uint32_t InCorrectionY, const Reads& InReads, VkImageView InOut0, VkImageView InOut1);

  private:
    VkDeviceSize _slotStride = 0;
    uint32_t _slot = 0;

    std::array<VkPipeline, (size_t) Blob::Count> _blobs {};
    std::array<bool, (size_t) Blob::Count> _blobTried {};
    bool _blobsFailed = false; // a blob that could not be built: the pass is given up, not retried every frame

    VkImage _dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory _dummyMemory = VK_NULL_HANDLE;
    VkImageView _dummyView = VK_NULL_HANDLE;
    bool _dummyReady = false;
    bool _dummyFailed = false; // not tried again: a half-made placeholder is never used

    bool CreateDummy(VkCommandBuffer cmdList);
    VkPipeline PipelineOf(Blob blob);
    // Takes a slot, writes the constants and the descriptors; returns the slot's set (null on failure).
    VkDescriptorSet Prepare(VkCommandBuffer cmdList, const void* constants, size_t bytes, const Reads& reads,
                            VkImageView out0, VkImageView out1);
    void Record(VkCommandBuffer cmdList, VkPipeline pipeline, VkDescriptorSet set, uint32_t groupsX, uint32_t groupsY);
};
