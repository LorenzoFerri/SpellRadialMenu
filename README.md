# RadialSpellMenu

An Elden Ring mod that replaces the default d-pad spell cycling with a radial wheel for selecting memorized spells. Loaded via [ModEngine 3 (me3)](https://github.com/soulsmods/ModEngine2).

![RadialSpellMenu screenshot](Screenshot.png)

## Features

- **Spell radial wheel** — hold D-pad Up to open, use the right stick to select a spell, release to confirm
- **Quick item radial wheel** — hold D-pad Down to open, use the right stick to select a belt item, release to confirm
- **Spell names and categories** — reads live from the game's param/message repositories at runtime; no data files needed
- **Category colour coding** — sorceries in blue, incantations in gold
- **Selection highlight** — the currently selected spell arc is highlighted with a coloured border
- **Embedded font** — the Elden Ring UI font is baked into the DLL; no external assets required

## Controls

| Input | Action |
|---|---|
| Hold D-pad Up (~180 ms) | Open spell radial menu |
| Hold D-pad Down (~180 ms) | Open quick item radial menu |
| Right stick | Move selection |
| Release held D-pad direction | Confirm selection |
| Release without opening a radial | Pass through as a normal D-pad tap |

## Requirements

- Elden Ring (Steam)
- [ModEngine 3](https://github.com/soulsmods/ModEngine2) installed and configured
- A controller using XInput

## Installation

Download `RadialSpellMenu.zip` from the [latest GitHub release](https://github.com/LorenzoFerri/SpellRadialMenu/releases/latest). The same package is used on Linux/Proton and Windows.

If there is no release yet, download the latest workflow artifact from the repository's **Actions** tab or build the DLL from source.

The zip contains:

```text
RadialSpellMenu/
  RadialSpellMenu.dll
  RadialSpellMenuProfile.me3
```

### Linux / Steam Deck / Bazzite

1. Extract `RadialSpellMenu.zip` anywhere you keep ModEngine profiles.
2. Launch the included profile through ModEngine 3:

```bash
me3 launch --profile "/path/to/RadialSpellMenu/RadialSpellMenuProfile.me3"
```

The included profile loads the DLL from the same folder:

```toml
[[natives]]
path = 'RadialSpellMenu.dll'
optional = false
load_early = false
```

For ERR or another modpack, you can either use the included profile as a reference or copy both files into the same folder used by that setup. If you add it to an existing profile, use this native entry:

```toml
[[natives]]
path = 'RadialSpellMenu.dll'
optional = false
load_early = false
```

The important part is still that the DLL and profile path match, and that `load_early = false` is kept.

### Windows

1. Extract `RadialSpellMenu.zip` anywhere you keep ModEngine profiles.
2. Launch Elden Ring through your usual ModEngine 3 launcher using `RadialSpellMenuProfile.me3`.

The included profile loads the DLL from the same folder:

```toml
[[natives]]
path = 'RadialSpellMenu.dll'
optional = false
load_early = false
```

For ERR or another modpack, you can copy both files into the same folder used by that setup, or add the native entry to an existing profile. If the DLL is beside the profile, use:

```toml
[[natives]]
path = 'RadialSpellMenu.dll'
optional = false
load_early = false
```

Do not load this DLL through old-style `external_dlls` entries if your setup also supports `[[natives]]`. This mod hooks D3D12 and must be loaded after the game renderer exists.

> **`load_early = false` is required.** The DLL hooks into an already-running D3D12 swap chain; it must not load before the game's renderer is initialised.

### Logs

The mod writes `RadialSpellMenu.log` next to `RadialSpellMenu.dll`.

Useful startup lines look like this:

```text
Spell manager initialized ...
Installed XInputGetState hook ...
Hooks installed
Initialization completed.
Command queue captured
ImGui ready
```

`Icon loader ready ...` appears the first time the radial menu opens, not necessarily at game launch.

If the game freezes or the menu does not appear, check this log first and include the last few lines when reporting the issue.

## Building

Building is only needed if you want to compile the DLL yourself. End users can use the release zip.

GitHub Actions builds `RadialSpellMenu.zip` automatically for pushes, pull requests, manual workflow runs, and published releases. Pushing a version tag like `v1.0.0` creates a GitHub release with the zip attached; publishing a release manually also attaches the zip.

On Linux, install `cmake`, `ninja`, `git`, and `curl`, then run:

```bash
git clone <this-repo> RadialSpellMenu
cd RadialSpellMenu
bash build.sh
```

`build.sh` handles everything automatically:
1. Clones `imgui` and `minhook` into `vendor/`
2. Detects or installs a mingw-w64 / llvm-mingw cross-compiler
3. Configures and builds with CMake + Ninja
4. Copies the resulting `RadialSpellMenu.dll` to `natives/`

The output DLL is at `natives/RadialSpellMenu.dll`.

## Project Structure

```
src/
  dllmain.cpp          — DLL entry point, wires up all subsystems
  common.h             — Log(), pattern-scanning helpers (GetModuleBase, FindPattern, …)
  dx12_hook.cpp/h      — D3D12 + ImGui rendering (vtable hooks for Present & ECL)
  input_hook.cpp/h     — XInput hook, open/close/select radial menu on D-pad Up
  radial_menu.cpp/h    — ImGui drawing code for the radial wheel
  spell_manager.cpp/h  — Reads memorized spells and current slot from game memory
  spell_metadata.cpp/h — Resolves spell names and icons from MagicParam / MsgRepository
  eldenring_font.h     — Elden Ring UI font embedded as a compressed C array

vendor/
  imgui/               — Dear ImGui (cloned by build.sh)
  minhook/             — MinHook (cloned by build.sh)

cmake/
  mingw-w64-toolchain.cmake    — Toolchain file for system mingw-w64
  llvm-mingw-toolchain.cmake   — Toolchain file for bundled llvm-mingw

natives/               — Build output (gitignored)
toolchains/            — Downloaded llvm-mingw binary (gitignored)
```

## How It Works

### Rendering (`dx12_hook.cpp`)
Because `load_early = false`, the game's D3D12 swap chain already exists when the DLL loads. `Install()` creates a tiny hidden window and a throw-away swap chain purely to read the `IDXGISwapChain::Present` vtable pointer, then destroys everything. MinHook patches that address so every `Present` call goes through `HookedPresent`. A second hook on `ID3D12CommandQueue::ExecuteCommandLists` captures the game's real command queue, which ImGui's DX12 backend needs. ImGui is initialised lazily on the first `Present` call after the queue is captured.

### Input (`input_hook.cpp`)
`XInputGetState` is hooked via MinHook. A hold of D-pad Up longer than 180 ms opens the spell radial, while D-pad Down opens the quick item radial. The held D-pad direction is suppressed once a radial is opening/open, and short taps are replayed so vanilla cycling still works. The right stick axes are consumed while the menu is open and fed to `UpdateSelectionFromStick`. On release, spell selection writes directly to `EquipMagicData.selected_slot`; quick item selection writes to `EquipItemData.selected_quick_slot`.

### Memory (`spell_manager.cpp`, `spell_metadata.cpp`)
Both files use byte-pattern scanning (`FindPattern`) against the game's `.exe` image to locate `GameDataMan` and `SoloParamRepository` without relying on hardcoded absolute addresses. Spell names are read from the game's `MsgRepository` by hooking its internal lookup function.
