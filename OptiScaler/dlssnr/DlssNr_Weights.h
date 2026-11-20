#pragma once
// [DlssNr] WeightsSet: weight set folders for the Neural Rendering model.
// The forwarder (nvngx.dll_dlssnr.dll) serves the files; this only says where they are.
#include <Config.h>
#include <misc/CustomProviders.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace DlssNr::Weights
{
// OptiScaler\nr_weights, beside the other OptiScaler folders (custom, plugins)
inline std::filesystem::path Root()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"nr_weights";
}

// The chosen set's folder, or empty for the DLL's own weights. A name that is not a plain folder name is ignored.
inline std::filesystem::path Folder(const std::string& name)
{
    if (name.empty() || !CustomProviders::IsValidName(name))
        return {};
    return Root() / string_to_wstring(name);
}

// The forwarder's two exports (dlssnr_weights_select, dlssnr_weights_take_log); null with an older forwarder
using SelectFn = void (*)(const wchar_t*);
using TakeLogFn = int (*)(char*, int);

// Hands the forwarder the set the next feature create reads (both APIs: it watches the model DLL either way)
inline void Select(SelectFn select, const std::string& set, const char* api)
{
    if (select == nullptr)
    {
        if (!set.empty())
            LOG_WARN("DLSS-NR{}: Model weights '{}' needs the forwarder from this release (nvngx.dll_dlssnr.dll); "
                     "the DLL's own weights are used",
                     api, set);
        return;
    }
    const auto dir = Folder(set).wstring();
    if (!set.empty() && dir.empty())
        LOG_WARN("DLSS-NR{}: Model weights '{}' is not a folder name; the DLL's own weights are used", api, set);
    select(dir.c_str());
}

// The forwarder's account of what each weights read got (stock or which file), into OptiScaler.log
inline void LogReads(TakeLogFn takeLog, const char* api)
{
    if (takeLog == nullptr)
        return;
    char text[4096] {};
    if (takeLog(text, (int) sizeof(text)) <= 0)
        return;
    std::string_view rest(text);
    while (!rest.empty())
    {
        const auto end = rest.find('\n');
        const auto line = rest.substr(0, end);
        if (!line.empty())
            LOG_INFO("DLSS-NR{} weights: {}", api, line);
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
}
} // namespace DlssNr::Weights
