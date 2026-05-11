# RadialMenu

RadialMenu is an Elden Ring DLL mod that adds radial menus for spells and quick items.

![RadialMenu screenshot](Screenshot.png)

## Features

- Hold `D-pad Up` to open a radial menu for memorized spells.
- Hold `D-pad Down` to open a radial menu for quick items.
- Select with the right stick and release the held D-pad direction to confirm.
- Select with mouse movement while a D-pad radial is open.
- Short D-pad taps pass through to the game, so vanilla cycling still works.
- Spell and item names are resolved from the game's runtime message repository.
- Spell/item icons are resolved from runtime params and loader-backed game/mod icon assets.
- The UI font is embedded in the DLL; no loose font or texture files are required.

## Controls

| Input | Action |
|---|---|
| Hold `D-pad Up` | Open spell radial menu |
| Hold `D-pad Down` | Open quick item radial menu |
| Right stick | Select radial entry |
| Release held D-pad direction | Confirm selected entry |
| Tap `D-pad Up` / `D-pad Down` | Pass through as normal game input |
| Mouse movement | Select radial entry |

## Requirements

- Elden Ring on Steam.
- ModEngine 3, or another compatible DLL loader such as ModEngine 2.
- A controller supported by Elden Ring's native input system.
- Easy Anti-Cheat disabled, as required for DLL mods.

## Installation

Download `RadialMenu.zip` from the latest release:

`https://github.com/LorenzoFerri/RadialMenu/releases/latest`

The zip contains:

```text
RadialMenu/
  RadialMenu.dll
  RadialMenuProfile.me3
```

Launch the included profile with ModEngine 3:

```bash
me3 launch --profile "/path/to/RadialMenu/RadialMenuProfile.me3"
```

To add the DLL to an existing ModEngine 3 profile, place `RadialMenu.dll` where the profile can find it and add:

```toml
[[natives]]
path = 'RadialMenu.dll'
optional = false
load_early = false
```

`load_early = false` is required. The mod discovers D3D12 hook targets by creating a dummy swap chain after the game renderer is already initialized. Loading early can deadlock with the game's own renderer startup.

## Logs

The mod writes `RadialMenu.log` next to `RadialMenu.dll`.

Useful startup lines include:

```text
Spell manager initialized ...
ChrCam input acceleration update hook installed ...
Initialization completed.
Command queue captured
ImGui ready
```

`Icon loader ready ...` appears after gameplay is ready and icon assets are warmed, not necessarily at process startup.

For modded icon troubleshooting, useful lines include:

```text
Asset reader: read icon asset through loader filesystem override.
Asset reader: read icon asset through mounted game VFS.
Icon loader: low/hi assets read ...
Icon loader ready ...
```

## Building

End users should use the release zip. To build from source on Linux:

```bash
git clone <repo-url> RadialMenu
cd RadialMenu
bash build.sh
```

`build.sh` is the supported build path. It prepares vendored dependencies if needed, selects a Windows cross-compiler, configures CMake, and builds the DLL.

Build output:

```text
natives/RadialMenu.dll
```

The project cross-compiles a Windows DLL. Do not attempt a native Linux build.

## Source Layout

```text
src/core/
  dllmain.cpp                 DLL entry point and subsystem startup
  common.h                    logging, pattern scan, memory helpers

src/game/equipment/
  equip_access.*              GameDataMan/equip/inventory memory access
  radial_slots.*              spell and quick-item radial slot lists and switching

src/game/messages/
  message_repository.*        runtime localized name lookup

src/game/metadata/
  spell_metadata.*            spell icon IDs and categories
  item_metadata.*             quick-item names and icon IDs
  seamless_coop_metadata.*    Seamless Coop item icon metadata extraction

src/game/params/
  param_repository.*          runtime param repository row access

src/game/state/
  gameplay_state.*            normal gameplay HUD-state check
  singleton_resolver.*        shared singleton static-address resolver

src/input/
  radial_input.*              radial open/selection/confirm state machine

src/game/input/
  native_input.*              native input facade for radial switch/camera modules
  in_game_pad.*               cached game input-map polling
  radial_switch.*             D-pad hold/tap capture and passthrough
  radial_camera.*             camera-input selection and camera suppression

src/render/assets/
  dcx.*                       DCX/KRAK/DFLT decompression
  icon_assets.*               icon layout and TPF parsing

src/render/d3d/
  dx12_hook.*                 Present/ECL hooks and ImGui rendering
  dx12_vtable.*               dummy swap-chain vtable discovery
  d3d_texture_upload.*        BC7 DDS upload and SRV creation

src/render/icons/
  icon_loader.*               icon atlas loading and UV lookup

src/render/vfs/
  asset_reader.*              loader-backed/game VFS asset read orchestration
  game_vfs.*                  Dantelion VFS layout and device discovery
  path_utils.*                path normalization and disk reads

src/render/ui/
  radial_menu.*               radial menu state and public UI API
  radial_menu_draw.*          radial menu layout and draw primitives
  eldenring_font.h            embedded compressed font
```

## Runtime Design

The DLL is loaded after game initialization. Startup installs MinHook, initializes game-data access, installs the asset reader hook, installs native input hooks, and installs D3D12 hooks.

D-pad input is handled through the game's native input paths. Holds open a radial menu, right-stick or mouse movement selects a radial entry, and releasing the held D-pad direction confirms the selected spell or item. Camera input acceleration is captured for selection and cleared while a radial menu is open so the camera does not move. Spell and quick-item selection are direct game-memory writes; short D-pad taps pass through via the native slot writers.

Rendering is handled by hooking `IDXGISwapChain::Present` and `ID3D12CommandQueue::ExecuteCommandLists`. The command queue hook captures the real direct graphics queue. ImGui initializes lazily on the first suitable `Present` call. The overlay draws only while a radial menu is open.

Metadata is resolved from runtime game systems instead of static data files. Names come from `MsgRepository`; spell and item icons come from runtime params and icon asset layouts. Modded icon assets are supported through loader-backed filesystem overrides and mounted game VFS reads, avoiding hardcoded mod-folder assumptions.

## Notes

- The mod is intended for offline play with EAC disabled.
- `toolchains/`, `build/`, and `natives/` are build artifacts and are gitignored.
- The included profile uses a separate savefile name. If you merge this DLL into another profile, review the profile-level savefile settings for your setup.
