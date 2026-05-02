# RadialSpellMenu

An Elden Ring mod that replaces the default d-pad spell cycling with a radial wheel for selecting memorized spells. Loaded via [ModEngine 3 (me3)](https://github.com/soulsmods/ModEngine2).

## Features

- **Radial wheel** — hold D-pad Up to open, use the right stick to select a spell, release to confirm
- **Spell names and categories** — reads live from the game's param/message repositories at runtime; no data files needed
- **Category colour coding** — sorceries in blue, incantations in gold
- **Current spell indicator** — a pip highlights the slot that is already active
- **Embedded font** — the Elden Ring UI font is baked into the DLL; no external assets required

## Controls

| Input | Action |
|---|---|
| Hold D-pad Up (~180 ms) | Open radial menu |
| Right stick | Move selection |
| Release D-pad Up | Confirm selection |
| Release without moving stick | Pass through as a normal D-pad Up tap |

## Requirements

- Elden Ring (Steam)
- [ModEngine 3](https://github.com/soulsmods/ModEngine2) installed and configured
- Linux host with `cmake`, `ninja`, `git`, `curl` available for building

## Building

```bash
git clone <this-repo> RadialSpellMenu
cd RadialSpellMenu
bash build.sh
```

`build.sh` handles everything automatically:
1. Clones `imgui` and `minhook` into `vendor/`
2. Detects or installs a mingw-w64 / llvm-mingw cross-compiler
3. Configures and builds with CMake + Ninja
4. Copies the resulting `RadialSpellMenu.dll` to `natives/` and, if present, to `~/ERRv2.2.4.4/dll/offline/`

The output DLL is at `natives/RadialSpellMenu.dll`.

## Installation

1. Copy `natives/RadialSpellMenu.dll` next to your me3 profile
2. Add it to your me3 profile:

```toml
[[natives]]
path = 'natives/RadialSpellMenu.dll'
optional = false
load_early = false
```

> **`load_early = false` is required.** The DLL hooks into an already-running D3D12 swap chain; it must not load before the game's renderer is initialised.

Launch via me3:

```bash
me3 launch --profile "/path/to/myprofile.me3"
```

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
`XInputGetState` is hooked via MinHook. A hold of D-pad Up longer than 180 ms opens the radial menu and suppresses the button from the game. The right stick axes are consumed while the menu is open and fed to `UpdateSelectionFromStick`. On release, `SwitchToSpellSlot` writes directly to `EquipMagicData.selected_slot` in game memory; if that fails it falls back to replaying synthetic D-pad Up taps.

### Memory (`spell_manager.cpp`, `spell_metadata.cpp`)
Both files use byte-pattern scanning (`FindPattern`) against the game's `.exe` image to locate `GameDataMan` and `SoloParamRepository` without relying on hardcoded absolute addresses. Spell names are read from the game's `MsgRepository` by hooking its internal lookup function.
