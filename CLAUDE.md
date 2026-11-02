# TESReloaded10 — Dev Notes for Claude

## Project
Vegas Shaders (formerly New Vegas Reloaded, NVR) — a post-process graphics injector for Fallout: New Vegas.
Built as a 32-bit DLL, `VegasShaders.dll` (project NewVegasReloaded.vcxproj, `TargetName` VegasShaders), injected via xNVSE.

### Runtime names (2026-11-02 rename) — `src/core/Paths.h/.cpp` owns every one
| New | Old (still read when the new one is missing) |
|---|---|
| `Data\NVSE\Plugins\VegasShaders.dll` (+ `.pdb`) | `NewVegasReloaded.dll` (an old copy left beside the new one is logged as an error: two plugins) |
| `VegasShaders.toml` (user settings; always written under this name) | `NewVegasReloaded.dll.toml` (read only; never changed or deleted) |
| `VegasShaders.defaults.toml`, `.maxquality.toml`, `.maxquality.user.toml`, `.materials.txt`, `.imgui.ini` | `NewVegasReloaded.dll.<same>` / `NewVegasReloaded.imgui.ini` |
| `Data\Shaders\VegasShaders\` (Shaders, Effects, Compute, caches) | `Data\Shaders\NewVegasReloaded\` |
| `Data\Textures\VegasShaders\` (LUTs) | `Data\Textures\NewVegasReloaded\` |
| `Data\VegasShaders\` (Presets, Variants, Keywords, Skins, EnabledVariants.ini) | `Data\NVR\` |
| `VegasShaders-Logs\VegasShaders_<date>.log` (game folder) | none: logs are only written under the new names |

Folders are chosen once at startup (new if it exists, else old if it exists, else new) and logged in one `PATHS folders:` line.
Kept unchanged on purpose: the xNVSE plugin name `NewVegasReloaded` (Main.cpp; mods check it), the console prefix `NVR`, every
settings section/key, `TESR_*` names, preset formats, log line tags, and the legacy `NewVegasReloaded.dll.config` (only read).
New code that touches a runtime file or folder goes through `Paths::` (or `VS_LOGS_FOLDER` for the logs), never a literal path.

## Build/deploy workflow
The user does **not** use `build.bat`. They compile the DLL themselves and always patch a
live install by hand. When a change touches this repo, tell them exactly which files to copy
and where, rather than pointing at `build.bat` or assuming a fresh `robocopy /mir` deploy:
- `VegasShaders.dll` (+ `.pdb` if useful for a crash) -> `<deploy>\nvse\Plugins\`. If that folder still has an old
  `NewVegasReloaded.dll`, tell them to delete or rename it (two copies of the plugin must not load).
- Any new/changed runtime `.hlsl` files under `src/hlsl/NewVegas/Shaders/` -> the same relative
  path under `<deploy>\Shaders\VegasShaders\Shaders\` (these are loaded/compiled at runtime,
  not built into the DLL). On an install that still has only the old `Shaders\NewVegasReloaded\` folder the DLL
  uses that folder, so copy there instead (or move the whole old folder to the new name first).
- Mention `Shaders\VegasShaders\Shaders\Cache\` as the place to clear if a shader change
  doesn't seem to take effect (`ShaderRecord::LoadShader` caches compiled/preprocessed shaders
  per name and normally auto-recompiles on content change, but it's a reasonable troubleshooting
  step)

## Key architecture
- `src/core/ImGuiManager.cpp` — ImGui settings overlay (DX9 + Win32 backend)
- `src/core/ShaderManager.cpp` — main render loop; calls `ImGuiManager::NewFrame()` and `Render()`
- `src/core/SettingManager.cpp` — TOML-backed settings; ImGui menu is auto-populated from this
- `lib/imgui` — git submodule, docking branch (`git submodule update --init --recursive` required after fresh clone)
- xNVSE headers live under `src/NewVegas/nvse/`

## DXVK input — critical finding
FNV is commonly run under **DXVK** (Vulkan translation layer). DXVK suppresses
`WM_KEYDOWN` and `WM_MOUSEWHEEL` before they reach any subclassed WndProc.
**Do not rely on WM messages for keyboard or scroll wheel input.**

`WM_INPUT` (Raw Input) is also unreliable — DXVK's message pump may not dispatch
`WM_INPUT` to the subclassed WndProc at all. **Do not use WM_INPUT for any keyboard input.**

### What works instead
| Input | Solution |
|---|---|
| Keyboard (all — text, nav, characters) | `GetAsyncKeyState` polled for VK 1–255 in `NewFrame`, edge-detect against `s_prevKeyState`, inject via `io.AddKeyEvent` + `ToUnicode` → `io.AddInputCharacterUTF16` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` → `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `GetAsyncKeyState(DikToVk(KeyEnable))` polled in `NewFrame` every render frame. `DikToVk` has a hardcoded table for extended keys that `MapVirtualKey` gets wrong (END, HOME, arrows, etc.) |

**Rule**: everything goes through `GetAsyncKeyState` polled on the render thread. No WM messages, no cross-thread queues.

### DIK → VK conversion
`MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX)` **fails silently** for E0-prefixed extended keys
(END=0xCF, HOME=0xC7, arrow keys, Insert, Delete, etc.) — returns 0 or wrong VK.
Use the hardcoded `DikToVk()` table in `ImGuiManager.cpp` instead. Non-extended keys
fall through to `MapVirtualKey` which handles them correctly.

### ImGui keyboard navigation
`ImGuiConfigFlags_NavEnableKeyboard` is intentionally **not set**. With it enabled,
END/HOME/arrow keys leak into ImGui widget navigation (jumping to last item, etc.)
even while the overlay is trying to close. Text field editing is unaffected — it uses
a separate code path that does not require nav to be enabled.

### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any `WM_MOUSEWHEEL` handler in WndProc
- `WM_INPUT` for keyboard input (may never be dispatched to WndProc under DXVK)
- `MapVirtualKey` for extended DIK scancodes (returns 0 or wrong VK)

WM_ACTIVATE and other non-input system messages still arrive normally.

## Mouse cursor position — click-to-refocus quirk
When the user **clicks** the game window to refocus (rather than Alt+Tab), the OS
generates a `WM_MOUSEMOVE`. The ImGui Win32 backend handles this by calling
`TrackMouseEvent`, which sets its internal `bd->MouseTrackedArea = 1`. Once that
flag is set, `ImGui_ImplWin32_UpdateMouseData` **suppresses** its `GetCursorPos`
fallback path. No further `WM_MOUSEMOVE` arrives because the game registers raw
input with `RIDEV_NOLEGACY`, which eats legacy mouse messages. Result: cursor
position freezes wherever it was when the click landed.

**Fix** (`NewFrame`, after `ImGui_ImplWin32_NewFrame`): inject our own
`io.AddMousePosEvent` from `GetCursorPos` + `ScreenToClient`. Last write wins in
ImGui's event queue, so it overrides whatever stale position `UpdateMouseData` left.
A bounds-check ensures we only inject when the cursor is inside the client rect.

Multi-monitor alt-tab fix is complementary: `SetOverlayVisible(true)` warps the OS
cursor to the window center via `SetCursorPos` if it is outside the client rect.
This runs once on open; the per-frame injection above keeps it correct thereafter.

## Input blocking when overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros `DIMOUSESTATE2` buffer. Mouse device only — keyboard device has a separate vtable under DXVK.
- **Keyboard actions**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0–265 (`BlockGameInput`).
- **Mouse buttons / wheel macros**: same `DIHookControl` call covers codes 256+.

## xNVSE
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input, UI, game objects). Prefer xNVSE APIs over vtable patches where available.

## Effects are modular (2026-11-02)
- One folder per effect: `src/effects/<Name>/` holds its C++, `README.md` (what it does, inputs, settings, passes, edge cases, Vulkan path,
  what correct/wrong looks like), `defaults.toml` (its settings defaults) and `SHADERS.txt` (its shader files; the shaders themselves stay in
  `src/hlsl/NewVegas/`, mirroring the game's `Shaders\VegasShaders\` layout). Index: `src/effects/INDEX.md` (`tools/effects_index.py`).
- **Read the effect's README.md before changing it, and update it in the same change.** A stale doc is worse than none.
- Settings defaults: edit the fragment (`src/effects/<Name>/defaults.toml`, or `resource/defaults/<Name>.toml` / `Core.toml` for sections
  without an effect folder), then `py -3.11 -I tools/defaults_tool.py merge .`; `check` verifies the committed
  `resource/VegasShaders.defaults.toml` matches. A new section also needs its line in `resource/defaults/ORDER.txt`.
- Frame order: `src/core/EffectChain.cpp` (two ordered step tables, pre- and post-tonemap). Adding an effect = a step there, a member in
  `ShaderManager` (Effects struct + registration), its folder, its README.

