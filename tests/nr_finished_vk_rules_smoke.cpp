// Host check of dlssnr/DlssNr_FinishedVkRules.h: the capture ring of Finished Picture on a native Vulkan game and the
// formats its capture pass reads. No GPU and no game.
//
//   cl /std:c++20 /EHsc /W4 /Iexternal\vulkan\include tests\nr_finished_vk_rules_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_FinishedVkRules.h"

#include <cstdio>

using namespace DlssNr::FinishedVkRules;
using DlssNr::FinishedGameFrame;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static FinishedGameFrame Frame(uint64_t epoch, bool reset = false, uint32_t w = 1920, uint32_t h = 1080)
{
    FinishedGameFrame f;
    f.captureEpoch = epoch;
    f.reset = reset;
    f.outputWidth = w;
    f.outputHeight = h;
    return f;
}

int main()
{
    // ---- formats ----
    CHECK(ReadMotion(VK_FORMAT_R16G16_SFLOAT) == GuideRead::View);
    CHECK(ReadMotion(VK_FORMAT_R32G32_SFLOAT) == GuideRead::View);
    CHECK(ReadMotion(VK_FORMAT_R16G16B16A16_SFLOAT) == GuideRead::View);
    CHECK(ReadMotion(VK_FORMAT_R16G16_SNORM) == GuideRead::View);
    CHECK(ReadMotion(VK_FORMAT_R16G16_SINT) == GuideRead::Refused);   // integer: no float sample
    CHECK(ReadMotion(VK_FORMAT_R32_SFLOAT) == GuideRead::Refused);    // one channel is no vector
    CHECK(ReadMotion(VK_FORMAT_D32_SFLOAT) == GuideRead::Refused);
    CHECK(ReadMotion(VK_FORMAT_B10G11R11_UFLOAT_PACK32) == GuideRead::Refused);

    CHECK(ReadDepth(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT) == GuideRead::View);
    CHECK(ReadDepth(VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT) == GuideRead::View);
    CHECK(ReadDepth(VK_FORMAT_X8_D24_UNORM_PACK32, VK_IMAGE_ASPECT_DEPTH_BIT) == GuideRead::View);
    // A view over depth and stencil cannot be sampled: the pass makes a depth-only view
    CHECK(ReadDepth(VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ==
          GuideRead::DepthOnly);
    CHECK(ReadDepth(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT) == GuideRead::View);
    CHECK(ReadDepth(VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT) == GuideRead::View); // colour-format depth
    CHECK(ReadDepth(VK_FORMAT_R32_UINT, VK_IMAGE_ASPECT_COLOR_BIT) == GuideRead::Refused);
    CHECK(ReadDepth(VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT) == GuideRead::Refused);

    // ---- a recording is not a capture until its command buffer is submitted ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        const size_t slot = ring.Begin(0x10, 100, Frame(100));
        CHECK(ring.Armed() == 1);
        CHECK(!ring.Pick(101, 1920, 1080, index, reset)); // recorded, never submitted: nothing to use
        CHECK(ring.Submitted(0x10));
        CHECK(ring.Armed() == 0);
        CHECK(!ring.Submitted(0x10)); // a second submit of the same buffer is not a second capture
        CHECK(ring.Pick(101, 1920, 1080, index, reset));
        CHECK(index == slot);
        CHECK(!ring.Pick(101, 1920, 1080, index, reset)); // used once: the same capture does not serve two presents
    }

    // ---- the window: one present after the capture, like the D3D12 slots ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 100, Frame(100));
        ring.Submitted(1);
        CHECK(ring.Pick(100, 1920, 1080, index, reset)); // a present at the capture's own count
        ring.Begin(2, 101, Frame(101));
        ring.Submitted(2);
        CHECK(ring.Pick(102, 1920, 1080, index, reset)); // one later
        ring.Begin(3, 110, Frame(110));
        ring.Submitted(3);
        CHECK(!ring.Pick(112, 1920, 1080, index, reset)); // two later: stale, dropped
        CHECK(!ring.Pick(111, 1920, 1080, index, reset)); // and gone
        ring.Begin(4, 120, Frame(120));
        ring.Submitted(4);
        CHECK(!ring.Pick(119, 1920, 1080, index, reset)); // a present before the capture's count (race): not this one
    }

    // ---- the newest capture wins, older ones go with it ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        const size_t a = ring.Begin(1, 200, Frame(200));
        const size_t b = ring.Begin(2, 200, Frame(200)); // the game records a frame ahead
        ring.Submitted(1);
        ring.Submitted(2);
        CHECK(a != b);
        CHECK(ring.Pick(201, 1920, 1080, index, reset));
        CHECK(index == b);
        CHECK(!ring.Pick(201, 1920, 1080, index, reset)); // the older one was used up with it
    }

    // ---- size: a capture for another output size is not this picture's ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 300, Frame(300, false, 960, 540));
        ring.Submitted(1);
        CHECK(!ring.Pick(301, 1920, 1080, index, reset));
        CHECK(ring.Pick(301, 960, 540, index, reset)); // still there for the swapchain it fits
    }

    // ---- a command buffer recorded over: the earlier recording of it is gone (the begin hook says so) ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(7, 400, Frame(400));
        CHECK(ring.Forget(7)); // the game began buffer 7 again before it was ever submitted
        ring.Begin(7, 401, Frame(401));
        CHECK(ring.Armed() == 1);
        ring.Submitted(7);
        CHECK(ring.Pick(402, 1920, 1080, index, reset));
        CHECK(ring.At(index).frame.captureEpoch == 401);
    }

    // ---- two upscaler calls in one command buffer (two sizes): both go with the one submit ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(8, 410, Frame(410, false, 1920, 1080));
        ring.Begin(8, 410, Frame(410, false, 640, 360));
        CHECK(ring.Armed() == 2);
        ring.Submitted(8);
        CHECK(ring.Armed() == 0);
        CHECK(ring.Pick(411, 1920, 1080, index, reset)); // each picture finds the capture of its own size
        CHECK(ring.Pick(411, 640, 360, index, reset));
        CHECK(ring.At(index).frame.outputWidth == 640);
    }

    // ---- reset / free of an unsent buffer; a submitted one is not touched ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 500, Frame(500));
        ring.Begin(2, 500, Frame(500));
        ring.Submitted(2);
        CHECK(ring.Forget(1));
        CHECK(ring.Armed() == 0);
        CHECK(!ring.Forget(2)); // already submitted: what it recorded runs
        CHECK(ring.Pick(501, 1920, 1080, index, reset));
    }

    // ---- a secondary command buffer: the submit carries its primary ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(0x20, 600, Frame(600)); // recorded into a secondary buffer
        ring.Rearm(0x20, 0x30);            // executed from primary 0x30
        CHECK(!ring.Submitted(0x20));
        CHECK(ring.Submitted(0x30));
        CHECK(ring.Pick(601, 1920, 1080, index, reset));
    }

    // ---- unsent for too long: dropped ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 700, Frame(700));
        ring.Expire(700 + kArmedWindow);
        CHECK(ring.Armed() == 1);
        ring.Expire(700 + kArmedWindow + 1);
        CHECK(ring.Armed() == 0);
        CHECK(!ring.Submitted(1));
        CHECK(!ring.Pick(701, 1920, 1080, index, reset));
    }

    // ---- ring wrap: the fifth capture takes the first slot, whatever it held ----
    {
        GuideRing ring;
        size_t first = 0, fifth = 0;

        for (uint64_t i = 0; i < kSlots + 1; ++i)
        {
            const size_t s = ring.Begin(10 + i, 800 + i, Frame(800 + i));

            if (i == 0)
                first = s;

            if (i == kSlots)
                fifth = s;
        }

        CHECK(first == fifth);
        CHECK(ring.Armed() == kSlots); // the first recording was dropped by the wrap
        CHECK(!ring.Submitted(10));
    }

    // ---- the game's reset request outlives a capture that was dropped unapplied ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 900, Frame(900, true)); // the game reset its upscaler here
        ring.Submitted(1);
        ring.Begin(2, 900, Frame(900, false));
        ring.Submitted(2);
        CHECK(ring.Pick(901, 1920, 1080, index, reset)); // takes the newer; the older one's reset rides on it
        CHECK(reset);

        ring.Begin(3, 910, Frame(910, true)); // reset capture goes stale unapplied
        ring.Submitted(3);
        CHECK(!ring.Pick(913, 1920, 1080, index, reset));
        ring.Begin(4, 914, Frame(914, false));
        ring.Submitted(4);
        CHECK(ring.Pick(915, 1920, 1080, index, reset));
        CHECK(reset); // not lost
        ring.Begin(5, 916, Frame(916, false));
        ring.Submitted(5);
        CHECK(ring.Pick(917, 1920, 1080, index, reset));
        CHECK(!reset); // and used once
    }

    // ---- an armed (unsent) reset capture that expires also keeps the request ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 1000, Frame(1000, true));
        ring.Expire(1000 + kArmedWindow + 1);
        ring.Begin(2, 1010, Frame(1010, false));
        ring.Submitted(2);
        CHECK(ring.Pick(1011, 1920, 1080, index, reset));
        CHECK(reset);
    }

    // ---- Clear: a new swapchain or device starts the history over ----
    {
        GuideRing ring;
        size_t index = 0;
        bool reset = false;
        ring.Begin(1, 1100, Frame(1100));
        ring.Submitted(1);
        ring.Clear();
        CHECK(!ring.Pick(1101, 1920, 1080, index, reset));
        ring.Begin(2, 1102, Frame(1102));
        ring.Submitted(2);
        CHECK(ring.Pick(1103, 1920, 1080, index, reset));
        CHECK(reset);
    }

    // ---- history gap (the D3D12 rule with one present per game frame) ----
    CHECK(HistoryGap(0, 100));         // nothing applied yet
    CHECK(!HistoryGap(100, 101));      // the next frame
    CHECK(!HistoryGap(100, 102));      // one frame missed is still a continuation
    CHECK(HistoryGap(100, 103));       // more than two game frames
    CHECK(!HistoryGap(100, 100));
    CHECK(!HistoryGap(100, 90));       // an earlier capture is no gap

    if (fails == 0)
        printf("nr_finished_vk_rules_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
