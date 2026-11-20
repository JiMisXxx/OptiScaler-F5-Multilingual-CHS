#pragma once
#include <cstdint>

// Superior Scaling on native Vulkan (dlssnr/DlssNr_Superior_Vk.inl): when the Vulkan path runs it, and what the menu says
// when it does not. A pure rule, so the host test (tests/nr_superior_vk_rule_smoke.cpp) can state every case.
//
// Today the Vulkan path runs Superior's No DLSS composition (Area downsample, matched guides, raw low field, guided
// resolve), unpacked, in one model pass, below 100% Model resolution: the cases D3D12 runs the same way. Where it does not
// run, the ordinary Matched residual composition stays, as it was before Superior had a Vulkan path.
namespace DlssNr::SuperiorScaling::VkRule
{
enum class Outcome : uint8_t
{
    NotSelected,  // another Upscale Mode
    Runs,         // the No DLSS composition runs
    NotReduced,   // 100% and above: the ordinary composition, as on D3D12
    Packed,       // Compress screen edges packs the model's picture
    MorePasses,   // more than one model pass
    DetailReuse,  // Reuse detail is on
    DeviceLimits, // the GPU cannot hold the low field pass's shared memory
    PassFailed    // the passes could not be built on this device
};

struct Frame
{
    bool selected = false;
    bool reduced = false; // the model works on fewer pixels than the frame (below 100%)
    bool packed = false;  // Compress screen edges is packing this frame
    unsigned passes = 1;
    bool detailReuse = false;
    bool deviceCanRun = true;
    bool passBuilt = true;
};

constexpr Outcome Decide(const Frame& f)
{
    return !f.selected       ? Outcome::NotSelected
           : f.packed        ? Outcome::Packed
           : !f.reduced      ? Outcome::NotReduced
           : f.passes > 1    ? Outcome::MorePasses
           : f.detailReuse   ? Outcome::DetailReuse
           : !f.deviceCanRun ? Outcome::DeviceLimits
           : !f.passBuilt    ? Outcome::PassFailed
                             : Outcome::Runs;
}

// The low field pass keeps a 36 x 68 tile of RGB floats in shared memory (superior/RawLow.hlsl).
constexpr uint32_t kLowFieldSharedBytes = 36u * 68u * 3u * 4u;
constexpr bool SharedMemoryEnough(uint32_t maxComputeSharedMemorySize)
{
    return maxComputeSharedMemorySize >= kLowFieldSharedBytes;
}

// The line under the Upscale Mode controls. privateWanted: the saved preset asks for CNN or Transformer, which Vulkan does
// not have yet. Empty when nothing needs saying.
constexpr const char* Describe(Outcome outcome, bool privateWanted)
{
    switch (outcome)
    {
    case Outcome::NotSelected:
        return "";
    case Outcome::Runs:
        return privateWanted ? "On Vulkan, Superior Scaling runs without a private DLSS for now (No DLSS): CNN and "
                               "Transformer come in an update."
                             : "";
    case Outcome::NotReduced:
        return "Superior Scaling works below 100% Model resolution; at 100% and above the Matched residual composition "
               "is used, as on DirectX 12.";
    case Outcome::Packed:
        return "Superior Scaling does not run with Compress screen edges on Vulkan yet; Matched residual is used.";
    case Outcome::MorePasses:
        return "Superior Scaling runs one model pass on Vulkan; with more passes Matched residual is used.";
    case Outcome::DetailReuse:
        return "Superior Scaling and Reuse detail do not run together on Vulkan yet; Matched residual is used.";
    case Outcome::DeviceLimits:
        return "This GPU cannot hold Superior Scaling's low field pass; Matched residual is used.";
    case Outcome::PassFailed:
        return "Superior Scaling's Vulkan passes could not be built on this GPU; Matched residual is used.";
    }
    return "";
}
} // namespace DlssNr::SuperiorScaling::VkRule
