// Host check of F5Low on a native Vulkan swapchain: F5LowVkRules.h (image count, headroom, generated count, pacing).
// cl /std:c++20 /EHsc /W4 tests/f5low_vk_rules_smoke.cpp
#include "../OptiScaler/framegen/f5low/F5LowVkRules.h"

#include <cmath>
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

    // job queue: push while there is room, wait briefly, then drain
    CHECK(OnQueue(0, 0.0) == QueueAction::Push);
    CHECK(OnQueue(1, 0.0) == QueueAction::Push);
    CHECK(OnQueue(kJobQueueCap, 0.0) == QueueAction::Wait);
    CHECK(OnQueue(kJobQueueCap, 2.9) == QueueAction::Wait);
    CHECK(OnQueue(kJobQueueCap, 3.0) == QueueAction::Drain);

    if (fails == 0)
        std::printf("f5low_vk_rules_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
