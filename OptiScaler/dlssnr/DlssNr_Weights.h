#pragma once
// [DlssNr] WeightsSetA / WeightsSetB: weight set folders for the two Neural Rendering models (Preset A, Preset B).
// The forwarder (nvngx.dll_dlssnr.dll) serves the files; this only says where they are.
#include <Config.h>
#include <dlssnr/DlssNr_WeightsRules.h>
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

// The forwarder's exports (dlssnr_weights_select2, dlssnr_weights_select, dlssnr_weights_take_log); null with an
// older forwarder. select = the first version, one folder for both models.
using SelectFn = void (*)(const wchar_t*);
using Select2Fn = void (*)(const wchar_t*, const wchar_t*);
using TakeLogFn = int (*)(char*, int);

struct Exports
{
    Select2Fn select2 = nullptr;
    SelectFn select = nullptr;
    bool Available() const { return select2 != nullptr || select != nullptr; }
};

inline void Load(Exports& exports, HMODULE forwarder)
{
    exports.select2 = (Select2Fn) GetProcAddress(forwarder, "dlssnr_weights_select2");
    exports.select = (SelectFn) GetProcAddress(forwarder, "dlssnr_weights_select");
}

inline std::wstring Dir(const std::string& set, const char* api)
{
    const auto dir = Folder(set).wstring();
    if (!set.empty() && dir.empty())
        LOG_WARN("DLSS-NR{}: Model weights '{}' is not a folder name; the DLL's own weights are used", api, set);
    return dir;
}

// Hands the forwarder the sets the next feature create reads, one per model (both APIs: it watches the model DLL
// either way). An older forwarder has one folder for both models: used when both models take the same set or only
// one model has a set; two different sets need the forwarder from this release.
inline void Select(const Exports& exports, const std::string& setA, const std::string& setB, const char* api)
{
    if (exports.select2 != nullptr)
    {
        const auto dirA = Dir(setA, api);
        const auto dirB = Dir(setB, api);
        exports.select2(dirA.c_str(), dirB.c_str());
        return;
    }
    if (exports.select == nullptr)
    {
        if (!setA.empty() || !setB.empty())
            LOG_WARN("DLSS-NR{}: Model weights need the forwarder from this release (nvngx.dll_dlssnr.dll); "
                     "the DLL's own weights are used",
                     api);
        return;
    }
    std::string set = setA.empty() ? setB : setA;
    if (!setA.empty() && !setB.empty() && setA != setB)
    {
        LOG_WARN("DLSS-NR{}: The forwarder is too old for separate Model A and Model B weights (nvngx.dll_dlssnr.dll "
                 "from this release); Model A uses '{}', Model B the DLL's own",
                 api, setA);
        set = setA;
    }
    const auto dir = Dir(set, api);
    exports.select(dir.c_str());
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
