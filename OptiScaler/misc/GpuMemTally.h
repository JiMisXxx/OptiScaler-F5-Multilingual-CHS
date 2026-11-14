#pragma once

// GPU memory by module: the bookkeeping behind the "GPU memory by module" log line (misc/GpuMemory.*). No D3D12 and no
// Windows in here, so tests/gpu_mem_tally_smoke.cpp checks it on the host.
//
// Every allocation is added to the bucket of the module that asked for it and taken off again when D3D12 destroys it.
// DEFAULT heap memory (and a custom heap in the GPU pool) counts as GPU memory; upload and readback memory is kept
// apart as host-visible. A line is produced when the totals moved enough since the last one and the last one is old
// enough.

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string_view>
#include <string>
#include <vector>

namespace gpumem
{

constexpr uint64_t kMegabyte = 1024ull * 1024ull;
constexpr uint64_t kMinMovedBytes = 16 * kMegabyte;
constexpr uint64_t kMinIntervalMs = 30000;

// D3D12_HEAP_TYPE / D3D12_MEMORY_POOL values, kept as numbers so this file needs no D3D12 header.
enum HeapType : int
{
    HeapDefault = 1,
    HeapUpload = 2,
    HeapReadback = 3,
    HeapCustom = 4,
    HeapGpuUpload = 5
};
enum MemoryPool : int
{
    PoolUnknown = 0,
    PoolL0 = 1,
    PoolL1 = 2
};

// true = counts as GPU memory, false = host-visible (upload, readback, custom in system memory)
inline bool IsGpuMemory(int heapType, int memoryPool)
{
    if (heapType == HeapDefault)
        return true;
    if (heapType == HeapCustom)
        return memoryPool == PoolL1;
    return false;
}

// The modules whose allocations get D3D12_RESIDENCY_PRIORITY_HIGH ([Memory] ProtectMemory): OptiScaler itself (its
// bucket name), f5low and the two NVIDIA runtimes it drives. The game, DLSS super resolution and Streamline stay
// normal; Intel's libxess_fg asks for HIGH itself.
inline bool ProtectedModule(std::string_view name)
{
    auto same = [&](std::string_view want)
    {
        if (name.size() != want.size())
            return false;
        for (size_t i = 0; i < name.size(); ++i)
            if (std::tolower((unsigned char) name[i]) != std::tolower((unsigned char) want[i]))
                return false;
        return true;
    };
    return same("OptiScaler") || same("f5low.dll") || same("nvngx_dlssnr.dll") || same("nvngx_dlssg.dll");
}

inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

class Tally
{
  public:
    // Text appended to every line, before the closing bracket (the budget and usage); called under the tally's lock
    std::function<std::string()> suffix;

    // bytes > 0 on creation, < 0 on destruction; prot: the part of it that got the high residency priority (same sign)
    // Returns a line to log when one is due (see Due), empty otherwise
    std::string Add(const std::string& bucket, bool hostVisible, int64_t bytes, uint64_t nowMs, int64_t prot = 0)
    {
        std::lock_guard lock(_m);
        Entry& e = Find(bucket);
        int64_t& v = hostVisible ? e.host : e.gpu;
        v += bytes;
        if (v < 0)
            v = 0; // a release of something counted before the tally was running; never go below nothing
        e.prot += prot;
        if (e.prot < 0)
            e.prot = 0;

        const int64_t total = TotalGpu();
        if (total > _peakGpu)
            _peakGpu = total;

        return DueLocked(nowMs, false);
    }

    // The shutdown line: always produced, with the peak
    std::string Final()
    {
        std::lock_guard lock(_m);
        return FormatLocked(true);
    }

    // A line if the totals moved by kMinMovedBytes since the last line and the last line is kMinIntervalMs old
    std::string Due(uint64_t nowMs)
    {
        std::lock_guard lock(_m);
        return DueLocked(nowMs, false);
    }

    uint64_t Gpu(const std::string& bucket)
    {
        std::lock_guard lock(_m);
        return (uint64_t) Find(bucket).gpu;
    }

    uint64_t Host(const std::string& bucket)
    {
        std::lock_guard lock(_m);
        return (uint64_t) Find(bucket).host;
    }

    uint64_t ProtectedBytes()
    {
        std::lock_guard lock(_m);
        int64_t t = 0;
        for (const auto& e : _e)
            t += e.prot;
        return (uint64_t) t;
    }

    uint64_t TotalGpuBytes()
    {
        std::lock_guard lock(_m);
        return (uint64_t) TotalGpu();
    }

  private:
    struct Entry
    {
        std::string name;
        int64_t gpu = 0;
        int64_t host = 0;
        int64_t prot = 0;
        int64_t loggedGpu = 0;
        int64_t loggedHost = 0;
    };

    std::mutex _m;
    std::vector<Entry> _e;
    uint64_t _lastLogMs = 0;
    bool _logged = false;
    int64_t _peakGpu = 0;

    Entry& Find(const std::string& name)
    {
        for (auto& e : _e)
            if (e.name == name)
                return e;
        _e.push_back({ name });
        return _e.back();
    }

    int64_t TotalGpu() const
    {
        int64_t t = 0;
        for (const auto& e : _e)
            t += e.gpu;
        return t;
    }

    static int64_t Abs(int64_t v) { return v < 0 ? -v : v; }

    std::string DueLocked(uint64_t nowMs, bool force)
    {
        if (!force)
        {
            if (_logged && nowMs - _lastLogMs < kMinIntervalMs)
                return {};

            int64_t moved = 0;
            for (const auto& e : _e)
                moved += Abs(e.gpu - e.loggedGpu) + Abs(e.host - e.loggedHost);

            if (moved < (int64_t) kMinMovedBytes)
                return {};
        }

        _logged = true;
        _lastLogMs = nowMs;
        return FormatLocked(false);
    }

    std::string FormatLocked(bool final)
    {
        std::vector<const Entry*> order;
        int64_t host = 0, prot = 0;
        for (auto& e : _e)
        {
            prot += e.prot;
            e.loggedGpu = e.gpu;
            e.loggedHost = e.host;
            host += e.host;
            order.push_back(&e);
        }
        std::sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) { return a->gpu > b->gpu; });

        std::string line = final ? "GPU memory by module (final):" : "GPU memory by module:";
        bool first = true;
        int64_t folded = 0;
        for (const Entry* e : order)
        {
            if (e->gpu < (int64_t) kMegabyte)
            {
                folded += e->gpu;
                continue;
            }
            char buf[160];
            snprintf(buf, sizeof(buf), "%s %s %lld MB", first ? "" : ",", e->name.c_str(),
                     (long long) (e->gpu / (int64_t) kMegabyte));
            line += buf;
            first = false;
        }
        char tail[200];
        if (folded > 0)
        {
            snprintf(tail, sizeof(tail), "%s others %lld MB", first ? "" : ",",
                     (long long) (folded / (int64_t) kMegabyte));
            line += tail;
        }
        const int64_t total = TotalGpu();
        snprintf(tail, sizeof(tail), " (total %lld MB, host-visible %lld MB", (long long) (total / (int64_t) kMegabyte),
                 (long long) (host / (int64_t) kMegabyte));
        line += tail;
        snprintf(tail, sizeof(tail), ", protected %lld MB", (long long) (prot / (int64_t) kMegabyte));
        line += tail;
        if (suffix)
            line += suffix();
        if (final)
        {
            snprintf(tail, sizeof(tail), ", peak total %lld MB", (long long) (_peakGpu / (int64_t) kMegabyte));
            line += tail;
        }
        line += ")";
        return line;
    }
};

} // namespace gpumem
