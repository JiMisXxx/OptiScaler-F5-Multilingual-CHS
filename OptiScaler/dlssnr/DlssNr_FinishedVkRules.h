#pragma once

// The rules of Finished Picture on a native Vulkan game (dlssnr/DlssNr_FinishedVk.h), apart from any Vulkan object so a
// host test can run them (tests/nr_finished_vk_rules_smoke.cpp): which formats of the game's depth and motion images the
// capture pass can read, and the bookkeeping of the capture ring -- which slots hold a frame, whether the game's
// command buffer that wrote one was really submitted, and which slot a present uses.
//
// The D3D12 Finished Picture knows when the game executes the command list that holds its capture (FinishedPictureSubmitted,
// DlssNr_Late.inl). On Vulkan the same knowledge comes from the vkQueueSubmit hooks: a slot is "armed" with the command
// buffer the capture was recorded into, becomes "submitted" when that command buffer is in a submit, and is dropped when
// the command buffer is begun again or freed first (the game recorded something else into it), or when it stays unsent for
// more than a few presents. A present only ever uses a submitted slot: a recording that never ran leaves no stale guides.

#include "DlssNr_FinishedGameFrame.h"
#include "../shaders/dlssnr/DlssNr_FinishedReady.h"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace DlssNr::FinishedVkRules
{
// How the capture pass reads a guide image through the view the game's upscaler call handed over.
enum class GuideRead
{
    Refused,   // not a format the pass can sample as floats
    View,      // the game's own view is read as it is (colour formats, depth-only formats)
    DepthOnly  // a depth+stencil view: the pass makes its own depth-aspect view
};

// The motion vectors: two or four float or normalised channels, the first two are the vector.
inline GuideRead ReadMotion(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
        return GuideRead::View;
    default:
        return GuideRead::Refused;
    }
}

// The depth: a depth format (its depth aspect), or a single float or normalised colour channel.
inline GuideRead ReadDepth(VkFormat format, VkImageAspectFlags viewAspect)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return GuideRead::View;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        // A view that covers stencil too cannot be sampled; a depth-aspect-only one can
        return viewAspect == VK_IMAGE_ASPECT_DEPTH_BIT ? GuideRead::View : GuideRead::DepthOnly;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
        return GuideRead::View;
    default:
        return GuideRead::Refused;
    }
}

// Slots of the capture ring, round robin. Four: the game has up to three frames in flight, so a slot is written again
// only after the frame that used it is long finished.
inline constexpr size_t kSlots = 4;

// Presents an unsent recording may wait before it counts as never submitted. A game submits well before its present, so
// this only has to outlast a game that records a frame ahead.
inline constexpr uint64_t kArmedWindow = 6;

// The slots' bookkeeping. Callers lock; nothing in here is thread safe.
class GuideRing
{
  public:
    struct Slot
    {
        uint64_t serial = 0;       // order of capture; 0: never used
        uint64_t cmd = 0;          // the command buffer the capture was recorded into, while it is unsent
        bool armed = false;        // recorded, not yet seen in a submit
        bool submitted = false;    // seen in a submit, not yet used or dropped
        uint64_t armedAt = 0;      // the present count at the recording (for Expire)
        FinishedGameFrame frame {};
    };

    // A capture is recorded into `cmd` at present count `now`. Returns the slot to write. A game that calls its upscaler
    // twice in one command buffer (two views, two features) gets two slots, both sent by the one submit; a command buffer
    // recorded over is the begin/reset hook's to say (Forget), not told apart here.
    size_t Begin(uint64_t cmd, uint64_t now, const FinishedGameFrame& frame)
    {
        const size_t index = _next++ % kSlots;
        Slot& slot = _slots[index];

        if (slot.armed || slot.submitted)
            Drop(slot);

        slot.serial = ++_serial;
        slot.cmd = cmd;
        slot.armed = true;
        slot.submitted = false;
        slot.armedAt = now;
        slot.frame = frame;
        return index;
    }

    // The game's submit carries `cmd`: what was recorded into it will run. True when a slot was waiting for it.
    bool Submitted(uint64_t cmd)
    {
        bool any = false;

        for (auto& slot : _slots)
        {
            if (slot.armed && slot.cmd == cmd)
            {
                slot.armed = false;
                slot.submitted = true;
                slot.cmd = 0;
                any = true;
            }
        }

        return any;
    }

    // `cmd` is begun again, reset or freed: an unsent capture in it is gone.
    bool Forget(uint64_t cmd)
    {
        bool any = false;

        for (auto& slot : _slots)
        {
            if (slot.armed && slot.cmd == cmd)
            {
                Drop(slot);
                any = true;
            }
        }

        return any;
    }

    // A secondary command buffer was executed from `primary`: the submit will carry the primary.
    void Rearm(uint64_t secondary, uint64_t primary)
    {
        for (auto& slot : _slots)
        {
            if (slot.armed && slot.cmd == secondary)
                slot.cmd = primary;
        }
    }

    // Recordings that stayed unsent for more than kArmedWindow presents.
    void Expire(uint64_t now)
    {
        for (auto& slot : _slots)
        {
            if (slot.armed && now > slot.armedAt && now - slot.armedAt > kArmedWindow)
                Drop(slot);
        }
    }

    uint32_t Armed() const
    {
        uint32_t count = 0;

        for (const auto& slot : _slots)
            count += slot.armed ? 1 : 0;

        return count;
    }

    // The slot a present at epoch `epoch` composes on a picture of `width` x `height`: the newest submitted capture of
    // that size inside the window of FinishedStale (one present; the same rule the D3D12 slots use). Everything older is
    // used up with it, and a game reset flag of a capture that was dropped unapplied rides on the one that is used.
    // False when there is none.
    bool Pick(uint64_t epoch, uint32_t width, uint32_t height, size_t& index, bool& reset)
    {
        const Slot* best = nullptr;

        for (size_t i = 0; i < kSlots; ++i)
        {
            Slot& slot = _slots[i];

            if (!slot.submitted)
                continue;

            if (DlssNr::FinishedStale(epoch, slot.frame.captureEpoch, 1))
            {
                Drop(slot);
                continue;
            }

            if (slot.frame.outputWidth != width || slot.frame.outputHeight != height)
                continue;

            if (best == nullptr || slot.serial > best->serial)
            {
                best = &slot;
                index = i;
            }
        }

        if (best == nullptr)
            return false;

        reset = best->frame.reset || _carriedReset;

        for (auto& slot : _slots)
        {
            if (slot.submitted && slot.serial <= best->serial)
            {
                if (&slot != best && slot.frame.reset)
                    reset = true;

                slot.submitted = false;
            }
        }

        _carriedReset = false;
        return true;
    }

    // Everything is forgotten (a resize, the device going): no capture of before is used again.
    void Clear()
    {
        for (auto& slot : _slots)
            slot = Slot {};

        _carriedReset = true;
    }

    const Slot& At(size_t index) const { return _slots[index]; }

  private:
    void Drop(Slot& slot)
    {
        // An unapplied capture's reset request is not lost: the next one applied carries it
        if ((slot.armed || slot.submitted) && slot.frame.reset)
            _carriedReset = true;

        slot.armed = false;
        slot.submitted = false;
        slot.cmd = 0;
    }

    std::array<Slot, kSlots> _slots {};
    uint64_t _serial = 0;
    size_t _next = 0;
    bool _carriedReset = false;
};

// NR drops its history when nothing was applied yet or the capture of this frame is more than two game frames of presents
// after the last applied one (the D3D12 rule, one present per game frame: FinishedHistoryGap).
inline bool HistoryGap(uint64_t lastAppliedEpoch, uint64_t captureEpoch)
{
    return DlssNr::FinishedHistoryGap(lastAppliedEpoch, captureEpoch, 1);
}
} // namespace DlssNr::FinishedVkRules
