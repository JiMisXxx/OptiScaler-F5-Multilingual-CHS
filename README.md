
# OptiScaler 中英双语汉化包

> 本仓库**仅提供汉化版本**，目前支持中 / 英双语切换。

## 一、下载准备

请按以下顺序下载两个文件，缺一不可：

1. **原版插件** —— [Download the latest version](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases/latest)
2. **本汉化包** —— [最新 Release](../../releases/latest)

## 二、安装指引

> 以《巫师 3》DX12 为例，其他游戏同理：**找到 `d3dcompiler_47.dll` 所在的目录**即可。

### 步骤 1：确认目标目录与文件

找到你已部署、并改过名的原版 `OptiScaler.dll`（本例中为 `dxgi.dll`）：

```text
E:\SteamLibrary\steamapps\common\The Witcher 3\bin\x64_dx12
├─ dxgi.dll          ← 就是 OptiScaler.dll，需要先改名 / 备份
└─ OptiScaler.ini    ← 保留你原来的配置即可，不需要动
```

### 步骤 2：替换文件

1. **关闭游戏**，并在任务管理器中确认 `witcher3.exe` / `witcher3_dx12.exe` 已完全退出。
2. 把旧的 `dxgi.dll` 改名为 `dxgi.dll.bak`（或直接移到其他目录备份）。
3. 将本汉化包中的 `OptiScaler.dll` 复制到该目录，并改名为 `dxgi.dll`。

### 步骤 3：切换语言

1. 启动游戏，打开 OptiScaler 菜单。
2. 进入 **Overlay & Look（叠加层与外观）→ Language（语言）**。
3. 选择所需语言，切换即时生效。

## 三、注意事项

- 文件名必须改成与原部署一致（如 `dxgi.dll`），否则汉化包不会加载。
- `OptiScaler.ini` 无需修改，原有配置可以继续沿用。
- 如需回退，删除新的 `dxgi.dll`，把 `dxgi.dll.bak` 改回 `dxgi.dll` 即可。

#
#
# 以下是作者原版介绍：
# OptiScaler Neural Rendering

A game mod that uses NVIDIA AI to change lighting, detail and colour. You can adjust the look and how much performance the effect costs.

This is an experimental community version of OptiScaler. Results and game support vary.

**[Download the latest version](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases/latest)** · [Setup guide](INSTALL-DLSSNR.md) · [What's new](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases) · [Discord](https://discord.gg/xzkNNwxbZu)

## What you can change

- Adjust the strength, lighting, detail and colour of the effect.
- Use separate settings for skin and scenery.
- Run the effect before or after the game's upscaling.
- Apply NR to the finished picture to help with green noise. Works with frame generation on or off in native DirectX 12 games, including HDR.
- Apply it more than once, with different settings each time. Extra passes cost more performance.
- Lower the model resolution to reduce the performance cost.
- Use Superior Scaling (Upscale Mode) to enlarge reduced-resolution NR with a private DLSS that steadies it over time (DirectX 12, NVIDIA DLSS).
- Switch plugins on or off on the **Plugins** page: see which `*.asi` plugins loaded or failed, disable one without touching the file, and turn the built-in multi frame generation unlock on per GPU family (off by default, no extra file needed). Changes apply at the next start. See [Plugins page](docs/PLUGINS.md).

## What you need

An NVIDIA RTX 20, 30, 40 or 50-series GPU and a supported 64-bit game. Older cards can be much slower.

Download the NVIDIA model file, `nvngx_dlssnr.dll`, separately. The file you need depends on your GPU. The [setup guide](INSTALL-DLSSNR.md#choose-the-correct-runtime) explains which one to use and how to check it.

## Install on Windows

1. Close the game and back up any existing mod files.
2. Download the release ZIP and extract **all files** beside the game's executable.
3. Add the model file described above to the same folder.
4. Run `setup_windows.bat` and choose **NVIDIA** when asked.
5. Start the game, select DLSS, then press **Insert** to open OptiScaler. Enable Neural Rendering and start with one pass.

See the [setup guide](INSTALL-DLSSNR.md) for game-specific steps and troubleshooting.

## Keep in mind

- Neural Rendering costs performance and can cause flicker or other visual problems. Using it before Ray Reconstruction is still experimental.
- The optional **hybrid mode** is for RTX 50 GPUs. Its files are included. Loading may pause the game and look like a freeze; please wait.
- Avoid anti-cheat-protected multiplayer games.

For frame generation, see the [setup notes](docs/DLSS-FRAME-GENERATION.md). More setup guides: [ReShade](docs/SETUP-RESHADE.md) · [Vulkan games and emulators](docs/SETUP-VULKAN.md) · [dxvk](docs/SETUP-DXVK.md) · [DirectX 11 games without DLSS](docs/SETUP-DX11.md). For bugs, [open an issue](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/issues) with your game, GPU, settings and `OptiScaler.log`. For questions and help, join the [Discord server](https://discord.gg/xzkNNwxbZu).

## Credits

Built on [OptiScaler](https://github.com/optiscaler/OptiScaler) and [Dagherbou's Neural Rendering fork](https://github.com/Dagherbou/OptiScaler_DLSSNR), with colour processing from [RenoDX](https://github.com/clshortfuse/renodx).

[Full credits](docs/CREDITS.md) · [Licence](LICENSE) · [OptiScaler documentation](https://github.com/optiscaler/OptiScaler/wiki)
