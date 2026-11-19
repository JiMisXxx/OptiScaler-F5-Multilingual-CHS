#pragma once
// [DlssNr] WeightsSet: weight set folders for the Neural Rendering model.
// The forwarder (nvngx.dll_dlssnr.dll) serves the files; this only says where they are.
#include <Config.h>
#include <misc/CustomProviders.h>

#include <filesystem>
#include <string>

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
} // namespace DlssNr::Weights
