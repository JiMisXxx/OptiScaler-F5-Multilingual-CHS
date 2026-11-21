#pragma once

// What the menu says about a Vulkan game's route: the native swapchain (the game's own device, FSR FG or DLSS-G) or the
// bridge (a D3D12 swapchain on screen, a D3D12 device and copies; Optical F5Low's NR + frame generation or the Vulkan
// route "Bridge, game's upscaler"). Pure, so a host test runs every value (tests/vulkan_route_rule_smoke.cpp); the menu
// (dlssnr/DlssNr_Menu.cpp, menu/menu_common.cpp, menu/StatusPage.cpp) reads the state and draws.

#include <cstdint>
#include <string>

namespace DlssNrVulkanRoute
{
// Mirror of framegen/FGOutput.h, by value (menu_common.cpp checks it with static_asserts).
enum class Output : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    F5Low
};

// XeFG and Optical F5Low's generator are D3D12 libraries (Intel ships no Vulkan frame generation; F5Low has the same
// interface and paces through DXGI): on a Vulkan game they need the bridge. FSR FG and DLSS-G have a Vulkan
// swapchain of their own.
inline bool OutputNeedsBridge(Output output) { return output == Output::XeFG || output == Output::F5Low; }

inline const char* OutputName(Output output)
{
    switch (output)
    {
    case Output::FSRFG:
        return "FSR FG";
    case Output::DLSSG:
        return "DLSSG";
    case Output::XeFG:
        return "XeFG";
    case Output::F5Low:
        return "F5Low";
    default:
        return "None";
    }
}

// The FG Output combo's entry: the name, and where the route is the native swapchain a note on the two that need
// the bridge, so it is read before the entry is chosen.
inline std::string OutputLabel(Output output, bool nativeRoute)
{
    std::string label = OutputName(output);

    if (nativeRoute && OutputNeedsBridge(output))
        label += " (needs the bridge)";

    return label;
}

// The route is the native swapchain: neither the game's-upscaler route nor Optical F5Low's NR + frame generation is on.
inline bool NativeRoute(bool routeOffered, bool routeOn, bool f5lowFrameGeneration)
{
    return routeOffered && !routeOn && !f5lowFrameGeneration;
}

// What a chosen output on the native swapchain offers: the same switch as the route card's radio, or, where the
// game makes no upscaler call (the route needs one), Optical F5Low's NR + frame generation instead.
enum class Offer
{
    None,
    UseBridge,
    UseF5Low
};

inline Offer BridgeOffer(Output chosen, bool nativeRoute, bool gameCallsUpscaler)
{
    if (!nativeRoute || !OutputNeedsBridge(chosen))
        return Offer::None;

    return gameCallsUpscaler ? Offer::UseBridge : Offer::UseF5Low;
}

inline std::string BridgeNeededLine(Output chosen)
{
    return std::string(OutputName(chosen)) + " runs on D3D12: it needs the Vulkan bridge.";
}

// Where Neural Rendering runs. The game's upscaler runs on its D3D12 copy (dlss_12, ffx_12, fsr21_12) while the route is
// on, and stays there when the route is left through the ini (on12Backend); Neural Rendering follows it. Optical F5Low's
// modes run it on Vulkan, with the bridge only presenting.
enum class RunsOn
{
    Vulkan,
    D3D12Bridge
};

inline RunsOn NrRunsOn(bool routeOn, bool on12Backend)
{
    return routeOn || on12Backend ? RunsOn::D3D12Bridge : RunsOn::Vulkan;
}

inline const char* RunsOnText(RunsOn runsOn)
{
    return runsOn == RunsOn::D3D12Bridge ? "D3D12 (Vulkan bridge)" : "Vulkan (native swapchain)";
}

// The route is off, no Optical F5Low mode is on (it makes the upscaler call itself), and the Vulkan upscaler setting is
// still a D3D12 copy: Neural Rendering silently stays on D3D12.
inline bool LeftoverD3D12Upscaler(bool routeOffered, bool routeOn, bool motionModeOn, bool on12Backend)
{
    return routeOffered && !routeOn && !motionModeOn && on12Backend;
}

inline std::string LeftoverLine(const std::string& backendCode)
{
    return "The game's upscaler runs on its D3D12 copy (" + backendCode + "), so Neural Rendering runs on D3D12.";
}

// The route card's two rows, always shown.
inline const char* NativeSummary() { return "Native swapchain: FSR FG, DLSS-G. One device, no copies."; }
inline const char* BridgeSummary() { return "Bridge: also F5Low, XeFG. Adds a D3D12 device and copies."; }

// The Status page's "Vulkan route" row. The F5Low modes: the bridge presents, Neural Rendering stays on Vulkan.
inline std::string StatusText(bool routeOn, bool f5lowFrameGeneration, bool leftover, bool bridgeUp,
                              const std::string& backendCode)
{
    if (routeOn)
        return "Bridge, game's upscaler (Neural Rendering on D3D12)";

    if (f5lowFrameGeneration)
        return "Bridge, Optical F5Low NR + frame generation (Neural Rendering on Vulkan)";

    if (leftover)
        return "Native swapchain, but the upscaler is still " + backendCode + " (Neural Rendering on D3D12)";

    if (bridgeUp)
        return "Native swapchain after a restart (the bridge is still up)";

    return "Native swapchain (Neural Rendering on Vulkan)";
}
} // namespace DlssNrVulkanRoute
