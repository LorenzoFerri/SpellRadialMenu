# AGENTS.md — Development Guide for AI Agents

This document describes how the codebase is structured, how to build and test changes, and where the key logic lives. Read this before making any changes.

---

## Build

```bash
bash build.sh
```

Always use `build.sh` — it updates vendor libraries, selects the right cross-compiler, and deploys the DLL to the test folder (`~/ERRv2.2.4.4/dll/offline/`) automatically. Do not invoke `cmake` or `ninja` directly.

The project cross-compiles a Windows DLL on Linux using mingw-w64 or llvm-mingw. Do **not** attempt a native Linux build.

---

## Testing

There is no automated test suite. The only way to verify behaviour is to build and run the mod in-game via:

```bash
me3 launch --profile "/home/faith/RadialSpellMenu/myprofile.me3"
```

Log output goes to `RadialSpellMenu.log` next to the DLL (i.e. `~/ERRv2.2.4.4/dll/offline/RadialSpellMenu.log`). Check this file for errors from `Log()` calls. The log is also written to the debug output stream, visible in tools like DebugView on Windows.

---

## Architecture

The mod is a Windows DLL loaded by ModEngine 3 with `load_early = false`. That means the game's D3D12 renderer and all game systems are fully initialised before `DllMain` runs.

### Subsystems (in initialisation order)

| File | Responsibility |
|---|---|
| `dllmain.cpp` | Entry point. Spawns one thread that calls `MH_Initialize`, then `InitializeSpellManager`, `input_hook::Install`, `dx12_hook::Install` in order. |
| `common.h` | Header-only utilities: `Log()`, `GetModuleBase()`, `GetModuleSize()`, `FindPattern<N>()`, `ResolveRipRelative()`, `SafeRelease<T>()`. |
| `spell_metadata.cpp` | Locates `SoloParamRepository` and `MsgRepository` via pattern scan. Reads icon IDs and categories from `MagicParam`. Resolves spell names by calling the game's message lookup function. Caches results. |
| `spell_manager.cpp` | Locates `GameDataMan` → `GameDataRoot` → `EquipMagicData` via pattern scan. Builds the `SpellSlot` list. Writes `EquipMagicData.selected_slot` for direct spell switching. |
| `input_hook.cpp` | Hooks `XInputGetState`. Implements hold-to-open logic, right-stick selection, and release-to-confirm. Falls back to synthetic D-pad Up taps if the direct memory write fails. |
| `dx12_hook.cpp` | Hooks `IDXGISwapChain::Present` and `ID3D12CommandQueue::ExecuteCommandLists`. Initialises ImGui lazily on first Present. Renders the radial menu every frame. |
| `radial_menu.cpp` | Pure ImGui drawing. Stateless from the D3D12 perspective — receives a `vector<SpellSlot>` each frame. |

---

## Key Invariants

**`load_early = false` is non-negotiable.** The DX12 hook creates a dummy swap chain to read the `Present` vtable pointer. This only works without deadlocking because the game's swap chain already exists. If this is ever changed to `true`, the dummy swap chain creation will race with the game's own and deadlock.

**Per-buffer command allocators.** `dx12_hook.cpp` keeps one `ID3D12CommandAllocator` per back-buffer index, indexed by `GetCurrentBackBufferIndex()`. Never use a cycling counter. Resetting allocator `[i]` is safe because swap-chain synchronisation guarantees the GPU has finished with buffer `i` by the time it becomes current again.

**sRGB format.** Elden Ring uses `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` for its swap chain. RTVs and the ImGui `RTVFormat` must use `DXGI_FORMAT_R8G8B8A8_UNORM` (strip the sRGB flag), or RTV creation will fail.

**Command queue type filter.** `HookedECL` only captures `D3D12_COMMAND_LIST_TYPE_DIRECT` queues. The game submits copy and compute work on other queues; using one of those as the ImGui command queue will cause GPU errors.

**Pattern scan addresses are cached.** `FindPattern` walks the entire `.exe` image. All resolved addresses are stored in file-static variables and computed once. Do not call `FindPattern` on a hot path.

---

## Adding or Changing Features

### Changing the rendering
All D3D12 + ImGui setup is in `dx12_hook.cpp`. The drawing call is a single line in `HookedPresent`:
```cpp
radial_menu::Draw(GetMemorizedSpells());
```
Replace or extend this to add more UI elements.

### Changing the radial menu appearance
Edit `radial_menu.cpp`. The function `Draw(const vector<SpellSlot>&)` is self-contained ImGui draw-list code. All sizes scale with `ui_scale` (derived from viewport height relative to 1080p).

### Changing input behaviour
Edit `input_hook.cpp` → `HookedXInputGetState`. The state machine constants at the top of the file (`kHoldThresholdMs`, `kQuickSelectHoldMs`, etc.) control timing.

### Changing memory offsets
If a game update moves structures, update the constants in `spell_manager.cpp`:
- `kGameDataManPattern` / `kGameDataManMask` — byte pattern to locate `GameDataMan`
- `kEquipMagicDataOffset`, `kFirstMagicSlotOffset`, `kSelectedSlotOffset` — offsets within the manager chain

For spell name / icon resolution, update the patterns and offsets in `spell_metadata.cpp`.

### Updating the embedded font
Rebuild `src/eldenring_font.h` from a `.ttf` file using ImGui's compression tool:
```bash
g++ -o /tmp/b2c vendor/imgui/misc/fonts/binary_to_compressed_c.cpp
/tmp/b2c /path/to/font.ttf EldenRingFont > src/eldenring_font.h
```
Then adjust the pixel size in `dx12_hook.cpp` → `Init()`:
```cpp
io.Fonts->AddFontFromMemoryCompressedTTF(EldenRingFont_compressed_data,
    (int)EldenRingFont_compressed_size, 20.0f);
```

---

## What NOT to Do

- Do not add new dependencies beyond ImGui and MinHook without strong justification.
- Do not call `FindPattern` on a hot path (every frame). Cache the result in a static.
- Do not use `load_early = true`.
- Do not store raw game pointers between frames — the game can unload/reload subsystems; re-resolve through the pointer chain each time.
- Do not create external asset files (fonts, textures) that need to be deployed alongside the DLL. Embed them instead.
- Do not commit `toolchains/`, `build/`, or `natives/*.dll` — they are gitignored for good reason.
