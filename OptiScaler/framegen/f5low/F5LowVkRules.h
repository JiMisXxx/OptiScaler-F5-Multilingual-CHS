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

// A VkDeviceGroupPresentInfoKHR (RDR2 chains one) the real and the generated presents may carry: one device only.
// mode is its VkDeviceGroupPresentModeFlagsKHR (1 = LOCAL); with swapchainCount 1 the swapchain's device mask has one
// bit, with 0 the default applies.
inline bool SingleDeviceMask(uint32_t mask) { return mask != 0 && (mask & (mask - 1)) == 0; }

inline bool DeviceGroupCarryable(uint32_t swapchainCount, uint32_t mode, uint32_t mask0)
{
    if (mode != 1)
        return false;

    if (swapchainCount == 0)
        return true;

    return swapchainCount == 1 && SingleDeviceMask(mask0);
}

// A raise of the multiplier past what the swapchain was made for and past its spare images
inline bool NeedsRaise(int wanted, int createdFor, uint32_t headroom)
{
    return wanted > createdFor && wanted > static_cast<int>(headroom);
}

// Getting the game to make a new swapchain for a raise: idle -> asked (a SUBOPTIMAL present) -> forcePending (no new
// swapchain after kAskPresents presents and kAskMs) -> forced (its next acquire answered OUT_OF_DATE, once) -> gaveUp
// (no new swapchain kForcedMs later: the clamp and the status line stay). A new swapchain or another wanted count
// starts over; a lowered count (or enough spare images) goes back to idle.
enum class RecreateState
{
    Idle,
    Asked,
    ForcePending,
    Forced,
    GaveUp
};

inline constexpr uint32_t kAskPresents = 30;
inline constexpr double kAskMs = 500.0;
inline constexpr double kForcedMs = 2000.0;

struct RecreateAsk
{
    RecreateState state = RecreateState::Idle;
    int wanted = 0;
    uint32_t presents = 0;
    double sinceMs = 0.0;
};

// Per present that went through. Returns true when this present should answer SUBOPTIMAL to the game.
inline bool OnPresentRecreate(RecreateAsk& ask, int wanted, int createdFor, uint32_t headroom, double nowMs)
{
    if (ask.state != RecreateState::Idle && ask.wanted != wanted)
        ask = {};

    if (!NeedsRaise(wanted, createdFor, headroom))
    {
        ask = {};
        return false;
    }

    switch (ask.state)
    {
    case RecreateState::Idle:
        ask = { RecreateState::Asked, wanted, 0, nowMs };
        return true;
    case RecreateState::Asked:
        if (++ask.presents >= kAskPresents && nowMs - ask.sinceMs >= kAskMs)
            ask.state = RecreateState::ForcePending;
        return false;
    case RecreateState::Forced:
        if (nowMs - ask.sinceMs >= kForcedMs)
            ask.state = RecreateState::GaveUp;
        return false;
    default:
        return false;
    }
}

// The game's acquire on the swapchain: true once, when the escalation is due (answer OUT_OF_DATE, acquire nothing)
inline bool OnAcquireRecreate(RecreateAsk& ask, double nowMs)
{
    if (ask.state != RecreateState::ForcePending)
        return false;

    ask.state = RecreateState::Forced;
    ask.sinceMs = nowMs;
    return true;
}

// Spare images asked for up front: the most frames this session may generate, so the multiplier never needs a new
// swapchain. (RequestedMinImageCount caps it by the surface.)
inline uint32_t SpareImagesUpFront(int maxInterpolations)
{
    return static_cast<uint32_t>(std::clamp(maxInterpolations, 1, static_cast<int>(kMaxGenerated)));
}

// The game's latency bound: how many of its presents may be unshown at once. Without us it had gameMinImages images,
// one on screen and the rest in flight. The extra images must not add depth to that, whatever the multiplier.
inline constexpr uint32_t kGateRing = 8; // present ids remembered
inline uint32_t LatencyBound(uint32_t gameMinImages) { return gameMinImages > 1 ? gameMinImages - 1 : 1; }
inline bool GateUsable(uint32_t bound) { return bound >= 1 && bound < kGateRing; }

// Before the game acquires for its next present (number issued + 1): the number (1-based) of the present that has to be
// on screen already, 0 when none yet.
inline uint64_t GateWaitNumber(uint64_t issued, uint32_t bound)
{
    const uint64_t next = issued + 1;
    return next > bound ? next - bound : 0;
}

// The gate gives up (stands aside, logged) after this many waits in a row that ran into their limit
inline constexpr uint32_t kGateGiveUp = 5;

// FlipMetering= as f5low reads it (src/oob_driver.h): -1 auto, 0 off ("0", "off", "false", "no"), 1 on ("1", "on",
// "true", "yes"), case and surrounding spaces ignored, anything else counts as auto.
inline int FlipMeteringForced(const char* setting)
{
    char v[8] = {};
    size_t n = 0;

    if (setting == nullptr)
        return -1;

    while (*setting == ' ' || *setting == '\t')
        ++setting;

    for (; *setting != 0; ++setting)
    {
        if (n + 1 >= sizeof(v))
            return -1;

        v[n++] = (*setting >= 'A' && *setting <= 'Z') ? static_cast<char>(*setting - 'A' + 'a') : *setting;
    }

    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r' || v[n - 1] == '\n'))
        v[--n] = 0;

    const auto is = [&](const char* word)
    {
        size_t i = 0;
        while (word[i] != 0 && v[i] == word[i])
            ++i;
        return word[i] == 0 && v[i] == 0;
    };

    if (is("0") || is("off") || is("false") || is("no"))
        return 0;

    if (is("1") || is("on") || is("true") || is("yes"))
        return 1;

    return -1;
}

// Hardware flip metering (VK_NV_present_metering): needs the extension and its feature on the device; auto = RTX 50 and
// newer (the display hardware spaces the flips there, on older GPUs nothing does), a forced 1 / 0 wins.
inline bool MeteringOn(int forced, bool rtx50OrNewer, bool deviceHasIt)
{
    if (!deviceHasIt)
        return false;

    return forced >= 0 ? forced == 1 : rtx50OrNewer;
}

// Whether the generated frames of a batch are spaced on the CPU: not when the driver meters them
inline bool PacedOnCpu(bool metered) { return !metered; }

// Whether the frame may be generated at all: the interval is known and the generator has a count to make.
inline bool CanPace(double intervalMs, uint32_t count) { return intervalMs > 0.0 && count > 0; }
} // namespace F5LowVk
