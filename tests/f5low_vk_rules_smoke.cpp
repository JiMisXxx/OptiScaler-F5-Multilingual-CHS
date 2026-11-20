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

    if (fails == 0)
        std::printf("f5low_vk_rules_smoke: PASS\n");
    return fails == 0 ? 0 : 1;
}
