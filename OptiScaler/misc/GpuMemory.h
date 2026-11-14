#pragma once

#include <d3d12.h>

// GPU memory by module (misc/GpuMemTally.h holds the bookkeeping). The D3D12 device hooks report every committed
// resource and heap here; the tracker attached to each one takes it off again when D3D12 destroys it.
namespace GpuMemory
{
// Reads [Log] LogGpuMemory (auto = on with LogToFile) once and says whether the hooks are wanted. Call before hooking.
bool Init();
bool Active();

// A committed resource or heap that was just created. `object` is what the game got (any interface), `bytes` its size,
// `heapType`/`memoryPool` the D3D12 heap properties it lives in, `caller` the hook's return address.
void Note(IUnknown* object, uint64_t bytes, int heapType, int memoryPool, void* caller);

// The totals line now, at process exit
void LogFinal();
} // namespace GpuMemory
