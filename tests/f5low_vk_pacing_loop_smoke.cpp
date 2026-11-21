// Closed-loop host check of F5Low on a native Vulkan swapchain: the game's interval measurement, our-wait accounting,
// synthetic arrival ticks and batch spacing (F5LowVkRules.h, the same functions F5Low_Vk.cpp calls) run against a
// virtual game and a virtual display, with each frame's measured interval and our waits fed back the way
// F5Low_Vk::QueuePresent does. No real sleeping. The "legacy" model (interval from raw arrival deltas, CPU spacing under
// FIFO) is the pre-fix behaviour and must run away, which proves the loop can fail.
//
// Part 2 (full loop, with f5low's real pacer core) needs the f5low-fg sources, which are not in this repo:
//   cl /std:c++20 /EHsc /W4 /I<f5low-fg>/include /I<f5low-fg>/src tests/f5low_vk_pacing_loop_smoke.cpp
// Without them part 2 is skipped (the CI build has no such path); part 1 builds with
//   cl /std:c++20 /EHsc /W4 tests/f5low_vk_pacing_loop_smoke.cpp
#define NOMINMAX
#include "../OptiScaler/framegen/f5low/F5LowVkRules.h"

#if __has_include("pace_api.h")
#define HAVE_PACER 1
#include "pace_api.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

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

using namespace F5LowVk;

namespace
{
// The display: FIFO takes one image per vblank and a present blocks while the swapchain's images are all queued;
// IMMEDIATE never blocks (each present is shown at once).
struct Display
{
    bool fifo = true;
    double vblankMs = 8.333;
    size_t images = 3; // set per run: the game's images plus one per generated frame
    std::deque<double> slots; // vblank time of each queued image
    double lastSlot = 0.0;

    // Returns when the present call returns
    double Present(double t)
    {
        if (!fifo)
            return t;

        while (!slots.empty() && slots.front() <= t)
            slots.pop_front();

        while (slots.size() >= images)
        {
            t = std::max(t, slots.front());
            slots.pop_front();
        }

        const double next = (std::floor(t / vblankMs) + 1.0) * vblankMs;
        lastSlot = std::max(lastSlot + vblankMs, next);
        slots.push_back(lastSlot);
        return t;
    }
};

struct Result
{
    std::vector<double> interval; // our smoothed interval after each game frame
    std::vector<double> period;   // time between two game presents
    double endMs = 0.0;
};

struct Cfg
{
    double workMs;
    double hz;
    int multiplier;
    bool fifo;
    bool legacy;
    int frames = 900;
};

Result Run(const Cfg& cfg)
{
    Display disp;
    disp.fifo = cfg.fifo;
    disp.vblankMs = 1000.0 / cfg.hz;

    const uint32_t n = GeneratedCount(cfg.multiplier - 1, kMaxGenerated, kMaxGenerated);
    // The swapchain has the game's 3 images plus one per frame we generate (RequestedMinImageCount)
    disp.images = RequestedMinImageCount(3, n, 0);
    constexpr int64_t kTps = 1000000;

    Result r;
    double t = 0.0, interval = 0.0, lastEntry = 0.0, ourWait = 0.0;
    int64_t synth = 0;
    uint32_t seed = 12345;

    for (int f = 0; f < cfg.frames; ++f)
    {
        // The game's own CPU+GPU time, +-5 % deterministic jitter
        seed = seed * 1664525u + 1013904223u;
        const double jitter = (static_cast<double>(seed >> 16) / 65535.0 - 0.5) * 0.10;
        t += cfg.workMs * (1.0 + jitter);

        // QueuePresent entry
        const double entry = t;
        const int64_t real = static_cast<int64_t>(entry * kTps / 1000.0);

        if (lastEntry > 0.0)
        {
            const double raw = entry - lastEntry;
            interval = NextInterval(interval, cfg.legacy ? raw : Unthrottled(raw, ourWait));
        }

        synth = NextSyntheticTick(synth, real, interval, kTps);
        lastEntry = entry;

        // Generate: nothing until the interval is known
        const uint32_t gen = interval > 0.0 ? n : 0;
        const double sp = cfg.legacy ? SpacingMs(interval, gen) : BatchSpacingMs(interval, gen, cfg.fifo);
        const double base = t;

        for (uint32_t k = 0; k < gen; ++k)
        {
            if (k > 0 && sp > 0.0)
                t = std::max(t, base + k * sp);

            t = disp.Present(t);
        }

        if (gen > 0 && sp > 0.0)
            t = std::max(t, base + gen * sp);

        t = disp.Present(t); // the game's own frame; the game thread returns now

        ourWait = t - entry; // AddOurWait of everything this hook blocked on
        r.interval.push_back(interval);
        r.period.push_back(f > 0 ? entry - r.endMs : 0.0);
        r.endMs = entry;
    }

    (void) synth;
    return r;
}

double Mean(const std::vector<double>& v, int from, int to)
{
    double s = 0.0;

    for (int i = from; i < to; ++i)
        s += v[i];

    return s / (to - from);
}

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::fabs(b); }
} // namespace


#ifdef HAVE_PACER
// ---------------------------------------------------------------------------------------------------------------------
// Part 2: the whole loop. Game thread (hook: interval measurement, our-wait accounting, the wait for the job two frames
// back and for a free slot, kSlots=4), the present worker (one job at a time: f5low's pacer plans the batch, waits to the
// release time, acquires an image, waits for the GPU, presents), and a FIFO / IMMEDIATE display with its image pool.
// The game's own vkAcquireNextImage block is the game's (not counted as ours). Everything on a virtual clock.
// Not modelled: the latency gate (GateAcquire), the swapchain mutex, flip metering and the burst (RTX 50 only).
// ---------------------------------------------------------------------------------------------------------------------
namespace loop
{
constexpr double kFreq = 10000000.0;
int64_t T(double ms) { return static_cast<int64_t>(ms * kFreq / 1000.0); }
double M(int64_t t) { return static_cast<double>(t) * 1000.0 / kFreq; }

struct Display
{
    bool fifo = true;
    double vblank = 8.333;
    size_t images = 3;
    struct Rec
    {
        double presentT, slot;
    };
    std::deque<Rec> recs;
    double lastSlot = 0.0;
    double lastShown = 0.0; // when the last present reaches the screen

    size_t Queued(double t) const
    {
        size_t c = 0;

        for (const auto& r : recs)
            if (r.presentT <= t && r.slot > t)
                ++c;

        return c;
    }

    double WaitBelow(double t, size_t limit)
    {
        if (!fifo)
            return t;

        while (!recs.empty() && recs.front().slot < t - 500.0)
            recs.pop_front();

        while (Queued(t) >= limit)
        {
            double next = 1e300;

            for (const auto& r : recs)
                if (r.presentT <= t && r.slot > t)
                    next = std::min(next, r.slot);

            t = next;
        }

        return t;
    }

    double Acquire(double t) { return WaitBelow(t, images - 1); } // one image is on screen, one is the one acquired

    double Present(double t)
    {
        if (!fifo)
        {
            lastShown = t;
            return t;
        }

        t = WaitBelow(t, images);
        const double next = (std::floor(t / vblank) + 1.0) * vblank;
        lastSlot = std::max(lastSlot + vblank, next);
        recs.push_back({ t, lastSlot });
        lastShown = lastSlot;
        return t;
    }
};

struct Model
{
    const char* name;
    bool unthrottled; // interval = arrival delta minus our waits (794c9858f), else raw (before)
    bool synthArrive; // the pacer's arrival ticks are synthetic (794c9858f), else the real present time
    bool readyGpu;    // readyTicks = game GPU done (target), else the worker's pickup (current)
    bool legacySpace; // plain spacing = SpacingMs under FIFO too (before 794c9858f)
    bool pacer;       // f5low's pacer plans the batch (default), else the plain spacing
    uint32_t gate = 0;       // the latency gate: the game's present N back must be on screen before it acquires (0 off)
    bool untimedFifo = false; // design: under FIFO the batch is presented back to back (the queue spaces it), no timer
};

struct Out
{
    std::vector<double> interval, pacerInterval, period;
    std::vector<double> realShown; // when each job's own frame reaches the screen
    std::vector<double> gateWait;  // ms the game waited in the gate before the frame
    double endMs = 0.0;
    uint32_t n = 0;
};

Out Run(const Cfg& c, const Model& m)
{
    const uint32_t n = GeneratedCount(c.multiplier - 1, kMaxGenerated, kMaxGenerated);
    Display d;
    d.fifo = c.fifo;
    d.vblank = 1000.0 / c.hz;
    d.images = RequestedMinImageCount(3, n, 0);

    pacecore::Config pc;
    pc.ticksPerSecond = kFreq;
    pc.refreshHz = c.hz;
    pacecore::Pacer pacer(pc);

    constexpr double kGpuTail = 2.0, kGenMs = 1.5, kHookMs = 0.3;
    constexpr int64_t kTps = static_cast<int64_t>(kFreq);

    Out o;
    o.n = n;
    std::vector<double> done;
    double tg = 0.0, wfree = 0.0, interval = 0.0, lastEntry = 0.0, ourWait = 0.0;
    int64_t synth = 0;
    uint32_t seed = 12345;

    for (int f = 0; f < c.frames; ++f)
    {
        // The latency gate (GateAcquire in the game's acquire): the present `gate` back must be on screen; counted as ours
        double gateWaited = 0.0;

        if (m.gate > 0 && f >= static_cast<int>(m.gate))
        {
            const double until = o.realShown[f - m.gate];

            if (until > tg)
            {
                gateWaited = until - tg;
                tg = until;
            }
        }

        o.gateWait.push_back(gateWaited);
        tg = d.Acquire(tg); // the game's own acquire, not ours
        seed = seed * 1664525u + 1013904223u;
        tg += c.workMs * (1.0 + (static_cast<double>(seed >> 16) / 65535.0 - 0.5) * 0.10);

        const double entry = tg;
        const int64_t real = T(entry);

        if (lastEntry > 0.0)
        {
            const double raw = entry - lastEntry;
            interval = NextInterval(interval, m.unthrottled ? Unthrottled(raw, ourWait) : raw);
        }

        synth = NextSyntheticTick(synth, real, interval, kTps);
        const int64_t arrive = m.synthArrive ? synth : real;
        lastEntry = entry;

        // Generate: the job two frames back (the output set) and a free slot, both counted as ours
        double ready = entry;

        if (f >= 2)
            ready = std::max(ready, done[f - 2]);

        if (f >= static_cast<int>(kSlots))
            ready = std::max(ready, done[f - kSlots]);

        tg = ready + kHookMs;
        ourWait = tg - entry + gateWaited;

        const double gpuDone = entry + kGpuTail, genDone = gpuDone + kGenMs;
        const uint32_t gen = interval > 0.0 ? n : 0;
        double t = std::max(wfree, tg);
        const double start = t;

        if (m.pacer)
        {
            const double pickup = std::max(start, genDone);
            const int64_t readyTicks = gen == 0 ? 0 : (m.readyGpu ? T(gpuDone) : T(pickup));

            F5LowPacerFrame fr {};
            fr.structSize = sizeof(fr);
            fr.arriveTicks = arrive;
            fr.readyTicks = readyTicks;
            fr.generated = gen;
            fr.displayPaced = c.fifo ? 1u : 0u;
            F5LowPacerBatch b {};
            b.structSize = sizeof(b);
            pacer.FrameArrived(fr, b);

            for (uint32_t k = 1; k <= gen + 1; ++k)
            {
                F5LowPacerTarget tgt {};
                tgt.structSize = sizeof(tgt);
                pacer.Target(b, k, tgt);

                if ((tgt.flags & F5LOW_PACER_TARGET_PACED) != 0 && tgt.releaseTicks != 0 &&
                    !(m.untimedFifo && c.fifo))
                    t = std::max(t, M(ClampRelease(tgt.releaseTicks, T(t), kTps, interval)));

                if (k <= gen)
                {
                    t = d.Acquire(t);
                    t = std::max(t, genDone);
                }
                else
                    t = std::max(t, gpuDone);

                t = d.Present(t);
                F5LowPacerPresented w {};
                w.structSize = sizeof(w);
                w.presentTicks = T(t);
                w.gpuDoneTicks = readyTicks;
                pacer.Presented(b, k, w);
            }

            pacer.BatchDone(b);
        }
        else
        {
            const double sp = m.legacySpace ? SpacingMs(interval, gen) : BatchSpacingMs(interval, gen, c.fifo);

            for (uint32_t k = 0; k < gen; ++k)
            {
                if (k > 0 && sp > 0.0)
                    t = std::max(t, start + k * sp);

                t = d.Acquire(t);
                t = std::max(t, genDone);
                t = d.Present(t);
            }

            if (gen > 0 && sp > 0.0)
                t = std::max(t, start + gen * sp);

            t = d.Present(std::max(t, gpuDone));
        }

        o.realShown.push_back(d.lastShown);
        done.push_back(t);
        wfree = t;

        F5LowPacerInfo info {};
        pacer.Info(info);
        o.interval.push_back(interval);
        o.pacerInterval.push_back(info.intervalMs);
        o.period.push_back(f > 0 ? entry - o.endMs : 0.0);
        o.endMs = entry;
    }

    return o;
}
} // namespace loop

// Shown fps and the mean spacing between two shown frames over frames [a, b) of the run
struct Shown
{
    double fps, spacingMs;
};

static Shown ShownOver(const loop::Out& r, int a, int b)
{
    const double span = r.realShown[b - 1] - r.realShown[a - 1];
    const double frames = static_cast<double>(b - a) * (r.n + 1.0);
    return { span > 0.0 ? frames * 1000.0 / span : 0.0, span > 0.0 ? span / frames : 0.0 };
}

static void GateLoop()
{
    using loop::Model;
    std::printf("\nWITH THE LATENCY GATE (bound = max(gameMin-1, 2), game images 3 -> bound 2), 120 Hz FIFO, 3000 frames: "
                "W ms, xN | game fps | shown fps (frames 300-400 -> 2800-2900) | present spacing ms (expect 8.33) | gate "
                "wait ms/frame | pacer interval\n");

    const Model cur { "794c, pacer+gate", true, true, false, false, true, 2, false };
    const Model curPlain { "794c, plain+gate", true, true, false, false, false, 2, false };
    const Model tgt { "target ready, pacer+gate", true, false, true, false, true, 2, false };
    const Model untimed { "target + untimed FIFO", true, false, true, false, true, 2, true };
    const Model untimed3 { "  + gate 3", true, false, true, false, true, 3, true };
    const Model untimed4 { "  + gate 4", true, false, true, false, true, 4, true };
    const Model untimed1 { "  + gate 1 (no gate effect)", true, false, true, false, true, 1, true };
    const Model all[] = { cur, curPlain, tgt, untimed, untimed3, untimed4, untimed1 };

    for (const Model& m : all)
    {
        std::printf(" %s\n", m.name);

        for (const double w : { 8.3, 16.7, 25.0 })
            for (int mult : { 2, 4, 6 })
            {
                Cfg c { w, 120.0, mult, true, false };
                c.frames = 3000;
                const loop::Out r = loop::Run(c, m);
                const Shown a = ShownOver(r, 300, 400), b = ShownOver(r, 2800, 2900);
                const double expShown = std::min(120.0, std::min(1000.0 / w, 120.0 / mult) * mult);
                const double gw = Mean(r.gateWait, 2800, 2900);
                std::printf("   %5.1f x%d | %5.1f | %6.1f -> %6.1f (expect %5.1f) | %5.2f -> %5.2f | %5.1f | %6.1f -> "
                            "%6.1f\n",
                            w, mult, 1000.0 / Mean(r.period, 2800, 2900), a.fps, b.fps, expShown, a.spacingMs,
                            b.spacingMs, gw, Mean(r.pacerInterval, 300, 400), Mean(r.pacerInterval, 2800, 2900));

            }
    }

    // Asserts: an untimed FIFO design with a deep enough gate keeps the screen fed in the cases the display can serve
    for (const double w : { 8.3, 16.7, 25.0 })
        for (int mult : { 2, 4, 6 })
        {
            Cfg c { w, 120.0, mult, true, false };
            c.frames = 3000;
            const Model design { "design", true, false, true, false, true, 2, true };
            const loop::Out r = loop::Run(c, design);
            const Shown a = ShownOver(r, 300, 400), b = ShownOver(r, 2800, 2900);
            const double expShown = std::min(120.0, std::min(1000.0 / w, 120.0 / mult) * mult);
            CHECK(Near(b.fps, expShown, 0.10));
            CHECK(b.fps >= a.fps * 0.97);
        }
}

static void FullLoop()
{
    using loop::Model;
    const Model all[] = {
        { "legacy, plain", false, false, false, true, false }, { "legacy, pacer", false, false, false, false, true },
        { "794c, plain", true, true, false, false, false },    { "794c, pacer", true, true, false, false, true },
        { "target, plain", true, false, true, false, false },  { "target, pacer", true, false, true, false, true },
    };

    const double works[] = { 8.3, 16.7, 25.0, 33.0 };
    const double rates[] = { 60.0, 120.0, 144.0 };

    std::printf("\nFULL LOOP: model | W ms, Hz F=FIFO I=IMMEDIATE, xN | game fps (expect) | shown fps (expect) | our "
                "interval ms, pacer interval ms (first -> last window)\n");

    for (const Model& m : all)
    {
        int bad = 0, total = 0;
        double worstRatio = 1.0;

        for (const bool fifo : { true, false })
            for (const double hz : rates)
            {
                if (!fifo && hz != 120.0)
                    continue;

                for (const double w : works)
                    for (int mult = 2; mult <= 6; ++mult)
                    {
                        const loop::Out r = loop::Run({ w, hz, mult, fifo, false }, m);
                        const double period = Mean(r.period, 800, 900);
                        const double gameFps = 1000.0 / period;
                        const double expGame = fifo ? std::min(1000.0 / w, hz / mult) : 1000.0 / w;
                        const double shown = gameFps * mult;
                        const double expShown = fifo ? std::min(hz, expGame * mult) : expGame * mult;
                        const double i1 = Mean(r.interval, 300, 400), i2 = Mean(r.interval, 800, 900);
                        const double p1 = Mean(r.pacerInterval, 300, 400), p2 = Mean(r.pacerInterval, 800, 900);
                        const bool ok = Near(gameFps, expGame, 0.10) && Near(shown, expShown, 0.10) &&
                                        i2 <= i1 * 1.05 + 0.2 && i2 < 100.0;
                        ++total;
                        bad += ok ? 0 : 1;
                        worstRatio = std::min(worstRatio, gameFps / expGame);

                        const bool sample = (mult == 2 || mult == 4 || mult == 6) && (w == 16.7 || w == 33.0) &&
                                            (hz == 120.0);

                        if (sample || !ok)
                            std::printf("  %-14s | %5.1f %3.0f%s x%d | %6.1f (%6.1f) | %6.1f (%6.1f) | %6.1f -> %6.1f, "
                                        "%6.1f -> %6.1f%s\n",
                                        m.name, w, hz, fifo ? "F" : "I", mult, gameFps, expGame, shown, expShown, i1,
                                        i2, p1, p2, ok ? "" : "  <-- off");
                    }
            }

        std::printf("  == %-14s: %d of %d cases outside the 10%% band, worst game fps vs expected %.2f\n", m.name, bad,
                    total, worstRatio);

        if (m.name[0] == 't') // the target model must hold everywhere
            CHECK(bad == 0);
        else if (std::strcmp(m.name, "legacy, pacer") == 0) // a pre-fix model must fail somewhere (the loop can fail)
            CHECK(bad > 0);
        // (legacy, plain with the async worker does not run away: the job two frames back and the slots decouple the
        // game from the worker's spacing; part 1's synchronous hook is the model that shows that runaway)
    }
}
#endif

int main()
{
    const double works[] = { 8.3, 16.7, 25.0, 33.0 };
    const double rates[] = { 60.0, 120.0, 144.0 };

    std::printf("FIFO (fixed): W ms, Hz, xN | interval ms (own W) | period ms (expect max(W, N/Hz)) | game fps (expect) "
                "| shown fps (expect)\n");

    for (const double hz : rates)
        for (const double w : works)
            for (int mult = 2; mult <= 6; ++mult)
            {
                const Result r = Run({ w, hz, mult, true, false });
                const double n1 = mult;

                const double intFirst = Mean(r.interval, 300, 400), intLast = Mean(r.interval, 800, 900);
                const double period = Mean(r.period, 800, 900);
                const double expectPeriod = std::max(w, n1 * 1000.0 / hz);
                const double gameFps = 1000.0 / period, expectGame = std::min(1000.0 / w, hz / n1);
                const double shownFps = gameFps * n1, expectShown = std::min(hz, expectGame * n1);

                if (mult == 2 || mult == 4 || mult == 6)
                    std::printf("  %5.1f %5.0f x%d | %6.2f (%5.2f) | %6.2f (%6.2f) | %6.1f (%6.1f) | %6.1f (%6.1f)\n", w,
                                hz, mult, intLast, w, period, expectPeriod, gameFps, expectGame, shownFps, expectShown);

                CHECK(Near(intLast, w, 0.10)); // the game's own time, not inflated by our waits
                CHECK(intLast <= intFirst * 1.05 + 0.2); // no upward trend
                CHECK(Near(period, expectPeriod, 0.10)); // the display or the game limits, nothing else
                CHECK(Near(gameFps, expectGame, 0.10));
                CHECK(Near(shownFps, expectShown, 0.10));
                CHECK(intLast < 100.0);
            }

    std::printf("IMMEDIATE (fixed, our CPU spacing is real): W ms, xN | interval | period (expect W+n/(n+1)*(W-0.5)) "
                "| shown fps (expect)\n");

    for (const double w : works)
        for (int mult = 2; mult <= 6; ++mult)
        {
            const Result r = Run({ w, 120.0, mult, false, false });
            const double n = mult - 1.0;

            const double intFirst = Mean(r.interval, 300, 400), intLast = Mean(r.interval, 800, 900);
            const double period = Mean(r.period, 800, 900);
            const double expectPeriod = w + n / (n + 1.0) * (w - 0.5);
            const double shownFps = 1000.0 / period * (n + 1.0), expectShown = 1000.0 / expectPeriod * (n + 1.0);

            if (mult == 2 || mult == 4 || mult == 6)
                std::printf("  %5.1f x%d | %6.2f | %6.2f (%6.2f) | %6.1f (%6.1f)\n", w, mult, intLast, period,
                            expectPeriod, shownFps, expectShown);

            CHECK(Near(intLast, w, 0.10));
            CHECK(intLast <= intFirst * 1.05 + 0.2);
            CHECK(Near(period, expectPeriod, 0.10));
            CHECK(Near(shownFps, expectShown, 0.10));
            // a batch never outlasts the interval
            CHECK(n * BatchSpacingMs(intLast, static_cast<uint32_t>(n), false) < intLast);
        }

    // The pre-fix model must run away (so the loop above is able to fail): raw arrival deltas include our waits and the
    // spacing is applied under FIFO too
    std::printf("LEGACY (pre-fix, must run away): W ms, mode, xN | interval first 100 -> last 100 frames\n");

    for (const double w : works)
        for (const bool fifo : { true, false })
            for (int mult = 2; mult <= 6; ++mult)
            {
                const Result r = Run({ w, 120.0, mult, fifo, true });
                const double first = Mean(r.interval, 10, 110), last = Mean(r.interval, 800, 900);

                if (mult == 2 || mult == 4 || mult == 6)
                    std::printf("  %5.1f %s x%d | %6.2f -> %6.2f (own %5.1f)\n", w, fifo ? "FIFO " : "IMMED", mult,
                                first, last, w);

                CHECK(last > w * 1.3); // far above the game's own time
                CHECK(last > first * 1.15 || last > w * 1.8); // growing, or settled well above it
            }

    // The reported 33 -> 72 ms case: a 30 fps game at x2 on 120 Hz FIFO, legacy
    {
        const Result r = Run({ 33.0, 120.0, 2, true, true });
        std::printf("RDR2-like legacy (33 ms, 120 Hz FIFO, 2X): interval %.1f -> %.1f ms\n", Mean(r.interval, 10, 60),
                    Mean(r.interval, 800, 900));
        CHECK(Mean(r.interval, 800, 900) > 50.0);
    }

#ifdef HAVE_PACER
    FullLoop();
    GateLoop();
#else
    std::printf("full loop skipped (no f5low-fg include path)\n");
#endif

    std::printf(fails == 0 ? "OK\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
