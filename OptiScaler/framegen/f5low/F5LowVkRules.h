#pragma once

#include <algorithm>
#include <cstdint>

// F5Low on a native Vulkan swapchain (framegen/f5low/F5Low_Vk.h): the pure counting and timing rules, host-tested in
// tests/f5low_vk_rules_smoke.cpp.
namespace F5LowVk
{
// Frames in flight of our own per-frame objects (command buffers, semaphores). A present that waits on one of our
// semaphores is only known to be done when the swapchain hands the image out again, so the ring is the size of a
// swapchain's usual image count.
inline constexpr uint32_t kSlots = 4;

// What the generator will be asked for.
inline constexpr uint32_t kMaxGenerated = 5;

// The game thread never waits longer than this in one present, in all (the job two frames back, the slot, the fences,
// the worker's drain, the swapchain lock). The worker's wait for the swapchain lock is longer: a game's direct present
// may block on vsync inside it.
inline constexpr double kBlockMsRule = 50.0;
inline constexpr unsigned long kWorkerLockMsRule = 1000;

// What is left of the game thread's budget (at least 1 ms, a zero wait would be a poll that fails by itself)
inline uint32_t RemainingMs(double budgetMs, double elapsedMs)
{
    const double left = budgetMs - elapsedMs;
    return left < 1.0 ? 1u : static_cast<uint32_t>(left);
}

// The swapchain's image count asked of the driver: the game's own plus one image per frame we may generate, capped at
// what the surface offers (surfaceMax 0 = no limit).
inline uint32_t RequestedMinImageCount(uint32_t gameMin, uint32_t extraImages, uint32_t surfaceMax)
{
    const uint32_t wanted = gameMin + extraImages;
    return surfaceMax == 0 ? wanted : std::min(wanted, std::max(surfaceMax, gameMin));
}

// How many images the driver gave beyond the game's minimum: the headroom we generate into.
inline uint32_t ImageHeadroom(uint32_t imageCount, uint32_t gameMin)
{
    return imageCount > gameMin ? imageCount - gameMin : 0;
}

// Frames to generate per game frame: what the multiplier asks for, never above the generator's limit nor the images
// we can acquire next to the game's.
inline uint32_t GeneratedCount(int requested, uint32_t generatorMax, uint32_t headroom)
{
    if (requested <= 0)
        return 0;

    return std::min({ static_cast<uint32_t>(requested), generatorMax, kMaxGenerated, headroom });
}

// Smoothed game frame interval from the time between two presents. A sample out of range (a pause, a hitch, the first
// frame) is dropped; 0 stays "unknown".
inline double NextInterval(double previousMs, double sampleMs)
{
    if (!(sampleMs >= 2.0 && sampleMs <= 100.0))
        return previousMs;

    if (previousMs <= 0.0)
        return sampleMs;

    return previousMs * 0.8 + sampleMs * 0.2;
}

// Space between the shown frames of one game frame (the generated ones, then the game's own). The waits add up to
// count * spacing, which is always below one interval. 0 when the interval is not known: no pacing, no generation.
inline double SpacingMs(double intervalMs, uint32_t count)
{
    if (!(intervalMs > 0.0) || count == 0)
        return 0.0;

    return intervalMs / (count + 1);
}

// A fence the generator signals: true when its completed value is more than `allowed` values behind what was issued, or
// is UINT64_MAX (D3D12 says so for a removed device). The game queue must not wait on a value that will not arrive.
inline bool FenceBehind(uint64_t completed, uint64_t issued, uint64_t allowed = 1)
{
    if (completed == UINT64_MAX)
        return true;

    return issued > completed && issued - completed > allowed;
}

// A timeline value the host hands out: the next one is read before the submit and only taken once it succeeded, so a
// failed submit leaves no gap and the values strictly increase.
struct FenceCounter
{
    uint64_t issued = 0;
    uint64_t Peek() const { return issued + 1; }
    uint64_t Take() { return ++issued; }
};

// What the frame shows after f5low's answer (result as F5LowFramesResult: 0 OK; anything else makes no frames). f5low
// makes all asked frames or none.
inline uint32_t GeneratedAfter(int result, uint32_t reported, uint32_t asked)
{
    return result == 0 && reported >= asked ? asked : 0;
}

// Whether the frame may be generated at all: the interval is known and the generator has a count to make.
inline bool CanPace(double intervalMs, uint32_t count) { return intervalMs > 0.0 && count > 0; }
} // namespace F5LowVk
