// Host check of DlssNr_VulkanRoute.h: what the menu says about a Vulkan game's route. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 /WX tests/vulkan_route_rule_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_VulkanRoute.h"

#include <cstdio>

using namespace DlssNrVulkanRoute;

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

int main()
{
    // Which outputs need the bridge: the two D3D12-only libraries
    CHECK(!OutputNeedsBridge(Output::NoFG));
    CHECK(!OutputNeedsBridge(Output::FSRFG));
    CHECK(!OutputNeedsBridge(Output::DLSSG));
    CHECK(OutputNeedsBridge(Output::XeFG));
    CHECK(OutputNeedsBridge(Output::F5Low));

    // The combo's label carries the note only on the native route, only on those two
    CHECK(OutputLabel(Output::XeFG, true) == "XeFG (needs the bridge)");
    CHECK(OutputLabel(Output::F5Low, true) == "F5Low (needs the bridge)");
    CHECK(OutputLabel(Output::XeFG, false) == "XeFG");
    CHECK(OutputLabel(Output::F5Low, false) == "F5Low");
    CHECK(OutputLabel(Output::FSRFG, true) == "FSR FG");
    CHECK(OutputLabel(Output::DLSSG, true) == "DLSSG");
    CHECK(OutputLabel(Output::NoFG, true) == "None");

    // The native route: offered, route off, Optical F5Low's frame generation off
    CHECK(NativeRoute(true, false, false));
    CHECK(!NativeRoute(true, true, false));
    CHECK(!NativeRoute(true, false, true));
    CHECK(!NativeRoute(false, false, false)); // a D3D game or dxvk: no route at all

    // The offer: only for those two outputs on the native route; the route needs an upscaler call, else F5Low
    for (bool calls : { false, true })
    {
        CHECK(BridgeOffer(Output::FSRFG, true, calls) == Offer::None);
        CHECK(BridgeOffer(Output::DLSSG, true, calls) == Offer::None);
        CHECK(BridgeOffer(Output::NoFG, true, calls) == Offer::None);
        CHECK(BridgeOffer(Output::XeFG, false, calls) == Offer::None);
        CHECK(BridgeOffer(Output::F5Low, false, calls) == Offer::None);
    }
    CHECK(BridgeOffer(Output::XeFG, true, true) == Offer::UseBridge);
    CHECK(BridgeOffer(Output::F5Low, true, true) == Offer::UseBridge);
    CHECK(BridgeOffer(Output::XeFG, true, false) == Offer::UseF5Low);
    CHECK(BridgeOffer(Output::F5Low, true, false) == Offer::UseF5Low);
    CHECK(BridgeNeededLine(Output::F5Low) == "F5Low runs on D3D12: it needs the Vulkan bridge.");
    CHECK(BridgeNeededLine(Output::XeFG) == "XeFG runs on D3D12: it needs the Vulkan bridge.");

    // Where Neural Rendering runs
    CHECK(NrRunsOn(false, false) == RunsOn::Vulkan);
    CHECK(NrRunsOn(true, false) == RunsOn::D3D12Bridge);
    CHECK(NrRunsOn(false, true) == RunsOn::D3D12Bridge); // a leftover dlss_12
    CHECK(NrRunsOn(true, true) == RunsOn::D3D12Bridge);
    CHECK(std::string(RunsOnText(RunsOn::Vulkan)) == "Vulkan (native swapchain)");
    CHECK(std::string(RunsOnText(RunsOn::D3D12Bridge)) == "D3D12 (Vulkan bridge)");

    // The leftover D3D12 upscaler: route off, no F5Low mode, a _12 backend, a Vulkan game only
    CHECK(LeftoverD3D12Upscaler(true, false, false, true));
    CHECK(!LeftoverD3D12Upscaler(false, false, false, true)); // D3D11/D3D12/dxvk: not ours
    CHECK(!LeftoverD3D12Upscaler(true, true, false, true));   // the route is on: that is what it does
    CHECK(!LeftoverD3D12Upscaler(true, false, true, true));   // F5Low makes the upscaler call itself
    CHECK(!LeftoverD3D12Upscaler(true, false, false, false));
    CHECK(LeftoverLine("dlss_12") ==
          "The game's upscaler runs on its D3D12 copy (dlss_12), so Neural Rendering runs on D3D12.");

    // The always-shown rows name what each route can run
    CHECK(std::string(NativeSummary()).find("FSR FG, DLSS-G") != std::string::npos);
    CHECK(std::string(BridgeSummary()).find("F5Low, XeFG") != std::string::npos);

    // The Status page's row, every state
    CHECK(StatusText(true, false, false, true, "") == "Bridge, game's upscaler (Neural Rendering on D3D12)");
    CHECK(StatusText(false, true, false, true, "") ==
          "Bridge, Optical F5Low NR + frame generation (Neural Rendering on Vulkan)");
    CHECK(StatusText(false, false, true, false, "dlss_12") ==
          "Native swapchain, but the upscaler is still dlss_12 (Neural Rendering on D3D12)");
    CHECK(StatusText(false, false, false, true, "") == "Native swapchain after a restart (the bridge is still up)");
    CHECK(StatusText(false, false, false, false, "") == "Native swapchain (Neural Rendering on Vulkan)");

    if (fails == 0)
        printf("vulkan_route_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
