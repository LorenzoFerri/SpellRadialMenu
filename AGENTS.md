# AGENTS.md - Development Guide

This file is the working guide for AI agents and maintainers. Read it before changing code.

## Build

Always build with:

```bash
bash build.sh
```

`build.sh` prepares pinned vendor libraries when missing and selects the correct Windows cross-compiler. The project cross-compiles a Windows DLL on Linux with mingw-w64 or llvm-mingw. Do not attempt a native Linux build.

Build output:

```text
natives/RadialSpellMenu.dll
```

## Testing

There is no automated test suite. Verify by building and running in-game through ModEngine 3:

```bash
me3 launch --profile "./RadialSpellMenuProfile.me3"
```

For local ERR testing, the DLL is commonly copied to:

```text
/home/faith/ERRv2.2.4.4/dll/offline/RadialSpellMenu.dll
```

The log is written next to the DLL as `RadialSpellMenu.log` and also to the debug output stream.

## Architecture

The mod is a Windows DLL loaded by ModEngine 3 with `load_early = false`. The game's D3D12 renderer and game systems are initialized before `DllMain` runs.

Initialization order:

| File | Responsibility |
|---|---|
| `src/core/dllmain.cpp` | Starts one initialization thread and calls `MH_Initialize`, `InitializeSpellManager`, `asset_reader::Install`, `input_hook::Install`, and `dx12_hook::Install`. |
| `src/core/common.h` | Header-only utilities: logging, module info, pattern scanning, RIP-relative resolution, readable-memory checks, COM release helper. |
| `src/game/equipment/equip_access.cpp` | Resolves `GameDataMan`, equip data, selected slots, and inventory item IDs. |
| `src/game/equipment/spell_manager.cpp` | Builds spell and quick-item lists and writes selected spell/item slots. |
| `src/game/messages/message_repository.cpp` | Resolves `MsgRepository` and localized runtime names. |
| `src/game/metadata/spell_metadata.cpp` | Resolves spell/item icon IDs and spell categories from runtime params. |
| `src/game/metadata/seamless_coop_metadata.cpp` | Extracts Seamless Coop item icon IDs from loaded `ersc.dll` code data. |
| `src/game/params/param_repository.cpp` | Shared runtime param repository and row lookup helpers. |
| `src/game/state/gameplay_state.cpp` | Checks normal gameplay HUD state through `CSFeMan`. |
| `src/game/state/singleton_resolver.cpp` | Generic singleton static-address resolver used by gameplay-state code. |
| `src/input/input_hook.cpp` | Hooks `XInputGetState`. |
| `src/input/radial_input.cpp` | Owns radial input state, D-pad hold/tap handling, right-stick selection, and open-slot snapshots. |
| `src/render/vfs/asset_reader.cpp` | Hooks the game VFS file open path and reads game assets. |
| `src/render/assets/loose_asset_reader.cpp` | Looks for loose mod assets near the DLL when VFS lookup is unavailable. |
| `src/render/assets/dcx.cpp` | Decompresses DCX/KRAK/DFLT containers. |
| `src/render/assets/icon_assets.cpp` | Parses icon layouts, TPF entries, and encrypted Data0 icon ranges. |
| `src/render/icons/icon_loader.cpp` | Coordinates icon asset loading, atlas upload, and icon UV lookup. |
| `src/render/d3d/dx12_vtable.cpp` | Creates a dummy D3D12 swap chain after game init to discover hook vtable targets. |
| `src/render/d3d/dx12_hook.cpp` | Hooks `Present` and `ExecuteCommandLists`, initializes ImGui, and renders the overlay. |
| `src/render/d3d/d3d_texture_upload.cpp` | Uploads BC7 DDS textures and creates SRVs. |
| `src/render/ui/radial_menu.cpp` | Public radial menu state/API. |
| `src/render/ui/radial_menu_draw.cpp` | Radial menu layout and ImGui draw-list primitives. |

## Key Invariants

- `load_early = false` is required. The dummy swap-chain vtable discovery relies on the game renderer already existing.
- `HookedECL` must only capture `D3D12_COMMAND_LIST_TYPE_DIRECT` queues. Copy/compute queues are not valid for ImGui rendering.
- Keep one command allocator per swap-chain back buffer, indexed by `GetCurrentBackBufferIndex()`. Do not replace this with a cycling counter.
- Elden Ring's swap chain may be `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`; RTV and ImGui formats must strip this to `DXGI_FORMAT_R8G8B8A8_UNORM`.
- Pattern scans must stay cached. Never call `FindPattern` from hot paths.
- Do not store raw game subsystem pointers across frames. Re-resolve through pointer chains when needed.
- Radial selection confirms through direct slot writes. Do not reintroduce fallback cycling for radial confirm. Synthetic input is only for short tap passthrough.

## Common Tasks

Changing input behavior:

`src/input/radial_input.cpp`

Changing radial menu visuals:

`src/render/ui/radial_menu_draw.cpp`

Changing D3D/ImGui setup:

`src/render/d3d/dx12_hook.cpp`

Changing D3D vtable discovery:

`src/render/d3d/dx12_vtable.cpp`

Changing spell/quick-item memory offsets:

`src/game/equipment/equip_access.cpp`

Changing runtime param metadata:

`src/game/metadata/spell_metadata.cpp` and `src/game/params/param_repository.cpp`

Changing localized name lookup:

`src/game/messages/message_repository.cpp`

Changing icon archive/layout parsing:

`src/render/assets/icon_assets.cpp`, `src/render/assets/dcx.cpp`, and `src/render/icons/icon_loader.cpp`

Updating the embedded font:

```bash
g++ -o /tmp/b2c vendor/imgui/misc/fonts/binary_to_compressed_c.cpp
/tmp/b2c /path/to/font.ttf EldenRingFont > src/render/ui/eldenring_font.h
```

Then adjust the font size in `src/render/d3d/dx12_hook.cpp` where `AddFontFromMemoryCompressedTTF` is called.

## Source Organization

- `src/core`: entrypoint and shared utilities.
- `src/game/equipment`: equip slots, inventory, spell/quick-item selection.
- `src/game/messages`: runtime localized names.
- `src/game/metadata`: spell/item metadata resolution.
- `src/game/params`: runtime param repository access.
- `src/game/state`: gameplay/HUD state and singleton resolution.
- `src/input`: XInput hook and radial input state machine.
- `src/render/assets`: file/asset parsing helpers.
- `src/render/d3d`: D3D12 hook, vtable discovery, texture upload.
- `src/render/icons`: icon atlas loading and UV lookup.
- `src/render/ui`: ImGui radial menu state and drawing.
- `src/render/vfs`: game VFS access and file reading.

## What Not To Do

- Do not add dependencies beyond ImGui and MinHook without strong justification.
- Do not change `load_early` to `true`.
- Do not create external runtime asset files that must be deployed beside the DLL. Embed assets instead.
- Do not commit `toolchains/`, `build/`, or `natives/*.dll`.
- Do not use destructive git commands unless explicitly requested.

## Verification Checklist

After code changes:

1. Run `bash build.sh`.
2. Run `git diff --check`.
3. If testing locally with ERR, copy `natives/RadialSpellMenu.dll` to the ERR offline DLL folder.
4. In game, verify spell radial, quick-item radial, short D-pad taps, icon loading, and first-open behavior.
