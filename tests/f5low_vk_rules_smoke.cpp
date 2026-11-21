// Host check of F5Low on a native Vulkan swapchain: F5LowVkRules.h (image count, headroom, generated count, pacing).
// cl /std:c++20 /EHsc /W4 tests/f5low_vk_rules_smoke.cpp
#include "../OptiScaler/framegen/f5low/F5LowVkRules.h"

#include <cmath>
#include <string>
#include <cstdio>

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                                           \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    using namespace F5LowVk;

    // image count: game's plus the generated ones, capped by the surface
    CHECK(RequestedMinImageCount(3, 5, 0) == 8);
    CHECK(RequestedMinImageCount(3, 5, 8) == 8);
    CHECK(RequestedMinImageCount(3, 5, 6) == 6);
    CHECK(RequestedMinImageCount(3, 5, 2) == 3); // never below the game's own request
    CHECK(RequestedMinImageCount(2, 0, 0) == 2);

    // headroom
    CHECK(ImageHeadroom(8, 3) == 5);
    CHECK(ImageHeadroom(3, 3) == 0);
    CHECK(ImageHeadroom(2, 3) == 0);

    // generated count
    CHECK(GeneratedCount(3, 5, 5) == 3);
    CHECK(GeneratedCount(5, 3, 5) == 3);  // generator limit
    CHECK(GeneratedCount(5, 5, 2) == 2);  // image headroom
    CHECK(GeneratedCount(9, 9, 9) == 5);  // API limit
    CHECK(GeneratedCount(0, 5, 5) == 0);
    CHECK(GeneratedCount(-1, 5, 5) == 0);
    CHECK(GeneratedCount(3, 5, 0) == 0);

    // interval smoothing
    CHECK(NextInterval(0.0, 16.6) == 16.6);
    CHECK(NextInterval(0.0, 500.0) == 0.0);
    CHECK(NextInterval(16.0, 500.0) == 16.0);
    CHECK(NextInterval(16.0, 0.5) == 16.0);
    CHECK(std::fabs(NextInterval(20.0, 10.0) - 18.0) < 1e-9);

    // spacing: waits stay below one interval
    CHECK(SpacingMs(0.0, 3) == 0.0);
    CHECK(SpacingMs(20.0, 0) == 0.0);
    CHECK(std::fabs(SpacingMs(20.0, 3) - 5.0) < 1e-9);
    for (uint32_t count = 1; count <= kMaxGenerated; ++count)
        CHECK(SpacingMs(33.3, count) * count < 33.3);

    CHECK(CanPace(16.6, 2));
    CHECK(!CanPace(0.0, 2));
    CHECK(!CanPace(16.6, 0));

    // fences: never wait on a value that will not arrive
    CHECK(!FenceBehind(5, 5));
    CHECK(!FenceBehind(4, 5));
    CHECK(FenceBehind(3, 5));
    CHECK(FenceBehind(UINT64_MAX, 5));
    CHECK(!FenceBehind(0, 0));

    // fake fence model: NOT_READY, an error and OK frames; a failed submit takes no value, every taken value is higher
    {
        FenceCounter ready, consumed, done;
        uint64_t lastConsumed = 0;
        const int results[] = { 1 /*NOT_READY*/, -5 /*error*/, 0, 0, 1, 0 };
        const uint32_t reported[] = { 0, 0, 0, 3, 0, 3 };
        const bool submitOk[] = { true, true, false, true, true, true };
        uint32_t shown = 0;

        for (int i = 0; i < 6; ++i)
        {
            const uint64_t copied = ready.Peek();
            CHECK(copied == ready.issued + 1);

            if (!submitOk[i])
                continue; // nothing taken, f5low not called

            ready.Take();
            const uint64_t c = consumed.Take(), d = done.Take(); // f5low signals both for every answer
            CHECK(c > lastConsumed);
            lastConsumed = c;
            CHECK(d == c);
            shown += GeneratedAfter(results[i], reported[i], 3);
        }

        CHECK(ready.issued == 5);
        CHECK(consumed.issued == 5);
        CHECK(shown == 6); // the two OK frames with all 3
    }

    CHECK(GeneratedAfter(0, 3, 3) == 3);
    CHECK(GeneratedAfter(0, 0, 3) == 0);
    CHECK(GeneratedAfter(1, 3, 3) == 0);
    CHECK(GeneratedAfter(-2, 3, 3) == 0);

    // asking the game for a new swapchain after a raise: suboptimal, then out of date once, then give up
    {
        RecreateAsk ask;
        CHECK(!OnPresentRecreate(ask, 1, 1, 1, 0.0));  // nothing raised
        CHECK(OnPresentRecreate(ask, 3, 1, 1, 10.0));  // raised: the suboptimal ask
        CHECK(ask.state == RecreateState::Asked);
        CHECK(!OnPresentRecreate(ask, 3, 1, 1, 20.0)); // not again
        for (uint32_t i = 0; i < kAskPresents + 2; ++i)
            CHECK(!OnPresentRecreate(ask, 3, 1, 1, 100.0)); // 30 presents but not 500 ms yet
        CHECK(ask.state == RecreateState::Asked);
        CHECK(!OnAcquireRecreate(ask, 120.0)); // not due
        CHECK(!OnPresentRecreate(ask, 3, 1, 1, 600.0));
        CHECK(ask.state == RecreateState::ForcePending);
        CHECK(OnAcquireRecreate(ask, 610.0)); // out of date, once
        CHECK(!OnAcquireRecreate(ask, 611.0));
        CHECK(ask.state == RecreateState::Forced);
        CHECK(!OnPresentRecreate(ask, 3, 1, 1, 2000.0));
        CHECK(ask.state == RecreateState::Forced);
        CHECK(!OnPresentRecreate(ask, 3, 1, 1, 2700.0)); // 2 s without a new swapchain
        CHECK(ask.state == RecreateState::GaveUp);
        CHECK(!OnPresentRecreate(ask, 3, 1, 1, 9000.0)); // stays given up for this count
        CHECK(!OnAcquireRecreate(ask, 9001.0));
        CHECK(OnPresentRecreate(ask, 4, 1, 1, 9100.0)); // another count starts over
        CHECK(ask.state == RecreateState::Asked);
        CHECK(!OnPresentRecreate(ask, 2, 1, 2, 9200.0)); // lowered: idle
        CHECK(ask.state == RecreateState::Idle);
        CHECK(NeedsRaise(3, 1, 1));
        CHECK(!NeedsRaise(3, 3, 1));
        CHECK(!NeedsRaise(3, 1, 3));
    }

    // device group present info: one device only
    CHECK(SingleDeviceMask(1));
    CHECK(SingleDeviceMask(4));
    CHECK(!SingleDeviceMask(0));
    CHECK(!SingleDeviceMask(3));
    CHECK(DeviceGroupCarryable(1, 1, 1));
    CHECK(DeviceGroupCarryable(0, 1, 0));
    CHECK(!DeviceGroupCarryable(1, 1, 3)); // two devices
    CHECK(!DeviceGroupCarryable(1, 4, 1)); // SUM mode
    CHECK(!DeviceGroupCarryable(2, 1, 1)); // two swapchains

    // spare images up front, the latency bound and its gate
    CHECK(SpareImagesUpFront(5) == 5);
    CHECK(SpareImagesUpFront(3) == 3);
    CHECK(SpareImagesUpFront(0) == 1);
    CHECK(SpareImagesUpFront(9) == 5);
    CHECK(SpareImagesToAsk(true, 5, 2) == 5);  // bounded: the most
    CHECK(SpareImagesToAsk(false, 5, 2) == 2); // no bound: what the multiplier needs
    CHECK(SpareImagesToAsk(false, 3, 5) == 3);
    CHECK(LatencyBound(4) == 3);
    CHECK(LatencyBound(3) == 2);
    CHECK(LatencyBound(2) == 2); // never below 2
    CHECK(LatencyBound(1) == 2);
    CHECK(GateUsable(3));
    CHECK(!GateUsable(kGateRing));
    CHECK(GateWaitNumber(0, 3) == 0); // nothing issued: no wait
    CHECK(GateWaitNumber(2, 3) == 0); // third present may go out
    CHECK(GateWaitNumber(3, 3) == 1); // the fourth needs the first on screen
    CHECK(GateWaitNumber(10, 3) == 8);
    CHECK(OwnIdAfterGame(5, 9) == 9);
    CHECK(OwnIdAfterGame(9, 5) == 9);

    // the gate gives up after 5 timeouts in a row, neutral results change nothing, it comes back later
    {
        GateState gate;
        for (uint32_t i = 0; i < kGateGiveUp - 1; ++i)
            GateOnWait(gate, WaitOutcome::Timeout, 10.0);
        CHECK(gate.on);
        GateOnWait(gate, WaitOutcome::Neutral, 11.0);
        CHECK(gate.on && gate.timeouts == kGateGiveUp - 1);
        GateOnWait(gate, WaitOutcome::Done, 12.0); // a good wait clears the count
        CHECK(gate.timeouts == 0);
        for (uint32_t i = 0; i < kGateGiveUp; ++i)
            GateOnWait(gate, WaitOutcome::Timeout, 100.0);
        CHECK(!gate.on);
        GateOnPresent(gate, 200.0);
        CHECK(!gate.on);
        for (uint32_t i = 0; i < kGateRearmPresents; ++i)
            GateOnPresent(gate, 200.0 + i);
        CHECK(gate.on && gate.timeouts == 0); // 120 presents
        for (uint32_t i = 0; i < kGateGiveUp; ++i)
            GateOnWait(gate, WaitOutcome::Timeout, 1000.0);
        CHECK(!gate.on);
        GateOnPresent(gate, 1000.0 + kGateRearmMs); // or 3 s
        CHECK(gate.on);
    }

    // flip metering: f5low's words; without the pacer export it is off unless forced on, never on V-Sync or without
    // the device extension
    CHECK(FlipMeteringForced("auto") == -1);
    CHECK(FlipMeteringForced("") == -1);
    CHECK(FlipMeteringForced(nullptr) == -1);
    CHECK(FlipMeteringForced("0") == 0);
    CHECK(FlipMeteringForced(" Off ") == 0);
    CHECK(FlipMeteringForced("FALSE") == 0);
    CHECK(FlipMeteringForced("no") == 0);
    CHECK(FlipMeteringForced("1") == 1);
    CHECK(FlipMeteringForced("On") == 1);
    CHECK(FlipMeteringForced("yes") == 1);
    CHECK(FlipMeteringForced("sometimes") == -1);
    CHECK(FlipMeteringForced("averyverylongword") == -1);
    CHECK(!MeteringOn(-1, true, false)); // auto = off here
    CHECK(MeteringOn(1, true, false));   // forced on
    CHECK(!MeteringOn(1, true, true));   // V-Sync paces
    CHECK(!MeteringOn(1, false, false)); // no extension
    CHECK(!MeteringOn(0, true, false));
    CHECK(PacedOnCpu(false) && !PacedOnCpu(true));
    CHECK(VsyncPaces(2) && VsyncPaces(3) && !VsyncPaces(0) && !VsyncPaces(1));

    // the driver's feedback: 8 bad in a row turn it off, a good one clears the count
    {
        MeterHealth health;
        for (uint32_t i = 0; i < kMeterRefusals - 1; ++i)
            MeterNote(health, MeterNegative(false, 1));
        CHECK(!health.off);
        MeterNote(health, MeterNegative(false, 0));
        CHECK(health.bad == 0);
        for (uint32_t i = 0; i < kMeterRefusals; ++i)
            MeterNote(health, MeterNegative(true, 0));
        CHECK(health.off);
        MeterNote(health, false);
        CHECK(health.off);
    }

    // batch size on a present: presented + 1 when metered, 1 to close a metered run, else none
    CHECK(BatchConfig(true, false, 3) == 4);
    CHECK(BatchConfig(true, true, 1) == 2);  // a cut-short batch
    CHECK(BatchConfig(true, true, 0) == 1);
    CHECK(BatchConfig(false, true, 0) == 1); // closes the metered run
    CHECK(BatchConfig(false, false, 0) == 0);

    // swapchain formats: 8-bit and HDR10 generate, FP16 / scRGB and the A2R10G10B10 order are refused with a reason
    CHECK(ClassifySwapchain(44, 0) == SwapchainSupport::Ok);
    CHECK(ClassifySwapchain(50, 0) == SwapchainSupport::Ok);
    CHECK(ClassifySwapchain(37, 0) == SwapchainSupport::Ok);
    CHECK(ClassifySwapchain(64, 1000104008LL) == SwapchainSupport::OkHdr10);
    CHECK(ClassifySwapchain(64, 0) == SwapchainSupport::Ok); // 10-bit SDR
    CHECK(ClassifySwapchain(58, 1000104008LL) == SwapchainSupport::RefusedOrder);
    CHECK(ClassifySwapchain(97, 1000104002LL) == SwapchainSupport::RefusedFp16);
    CHECK(ClassifySwapchain(97, 0) == SwapchainSupport::RefusedFp16);
    CHECK(ClassifySwapchain(123, 0) == SwapchainSupport::RefusedFormat);
    CHECK(SwapchainGenerates(SwapchainSupport::OkHdr10) && !SwapchainGenerates(SwapchainSupport::RefusedFp16));
    CHECK(std::string(SwapchainReason(SwapchainSupport::RefusedFp16)) == "only HDR10 output is supported");
    CHECK(std::string(SwapchainReason(SwapchainSupport::Ok)).empty());

    // hudless
    CHECK(HudlessModeOf(false, true, true, true) == HudlessMode::ColourOnly);
    CHECK(HudlessModeOf(true, false, true, true) == HudlessMode::NoLayout);
    CHECK(HudlessModeOf(true, true, false, true) == HudlessMode::SizeMismatch);
    CHECK(HudlessModeOf(true, true, true, false) == HudlessMode::FormatMismatch);
    CHECK(HudlessModeOf(true, true, true, true) == HudlessMode::Used);
    CHECK(std::string(HudlessText(HudlessMode::Used)).find("hudless") != std::string::npos);

    // Dynamic MFG glue
    CHECK(DynamicRuns(true, false, 3));
    CHECK(!DynamicRuns(true, true, 3));  // follows the game
    CHECK(!DynamicRuns(false, false, 3));
    CHECK(!DynamicRuns(true, false, 1)); // nothing to choose from
    CHECK(CountAfterDynamic(0, 3) == 3); // 1X: the kept count is what comes back
    CHECK(CountAfterDynamic(2, 3) == 2);
    CHECK(DynamicHolds(true, 0));
    CHECK(!DynamicHolds(true, 2));
    CHECK(!DynamicHolds(false, 0));
    CHECK(InFront(100, 100) && !InFront(100, 200) && !InFront(0, 0));

    // the game's Reflex cap for F5Low on Vulkan: per rendered frame, no driver multiplier
    CHECK(std::fabs(ReflexCapForVulkanF5Low(120.0f, 3) - 40.0f) < 1e-4f);
    CHECK(ReflexCapForVulkanF5Low(120.0f, 1) == 120.0f);

    // the game thread's budget
    CHECK(RemainingMs(50.0, 0.0) == 50);
    CHECK(RemainingMs(50.0, 20.5) == 29);
    CHECK(RemainingMs(50.0, 49.5) == 1);
    CHECK(RemainingMs(50.0, 80.0) == 1);

    if (fails == 0)
        std::printf("f5low_vk_rules_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
