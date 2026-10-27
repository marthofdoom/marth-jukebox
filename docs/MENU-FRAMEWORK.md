# SKSE Menu Framework: registration research (2026-10-27)

Research only. Sources are cited inline: "H" = `resources/SKSEMenuFramework.h` in
QTR-Modding/SKSE-Menu-Framework-3 (master, 11303 lines, read via `gh api`); "U" = that repo's `Usage.md`;
"R" = its `README.md`; "EX" = QTR-Modding/SKSE-Menu-Framework-3-Example (`src/UI.cpp`, `src/plugin.cpp`);
"FW" = framework source (`src/plugin.cpp`, `src/Hooks.cpp`, `src/Licence.cpp`); "DLL" = the installed binary
inspected read-only with `strings`/`python3 -I`. Anything not confirmed is marked NOT FOUND or NOT CONFIRMED.

## 1. Summary & verdict

Verdict: **adopt it, with changes to the plan.** A plain `SetSection` + `AddSectionItem(path, void __stdcall fn())`
registration is all that is needed, the page is drawn by the framework's ImGui (1.90.8 docking) through a
header-only wrapper, and every widget we need (InputText with Password flag, Checkbox, Button, BeginCombo,
BeginListBox/Selectable, ProgressBar, TextColored) exists in the header.

- Registration is two calls, made in `SKSEPluginLoad` by every example found (see section 3). No SKSE message is required.
- The render callback runs inside the engine's UI-render hook (FW Hooks.cpp), every frame the menu is open, not on our thread. It must only read atomics/copies. This matches our plan.
- The header is LGPL-2.1 (fine for MIT us); the **DLL is "All Rights Reserved", no redistribution**: we can only list it as a Nexus requirement, never bundle it.
- There is NO plugin-side persistence API and NO plugin-registered hotkey API (only a toggle for the whole framework). We keep our INI, as DESIGN 3.6 already says.
- Biggest practical risks: the header caches a null function pointer forever if the framework DLL is not yet loaded at the first call; `GetModuleHandleW` load-order; a second ImGui user in the process (our fallback) conflicts with the framework's D3D/UI hooks.

Correction to the brief: **MFO does not use this framework.** `grep -rIni "SKSEMenu|menu.?framework"` over
`marth-follower-overhaul/native`, `Docs`, `docs` and root `*.md` returns nothing; MFO's Board is its own ImGui overlay
(`native/Board.cpp`, Present/Resize vtable hooks, own `ImGui::CreateContext` at line ~481). So the worked example is
third-party (section 4).

## 2. Version / runtime / licence

Installed copy: `/mnt/gaming/modlists/LoreRim/mods/SKSE Menu Framework/` (listing: `SKSE/Plugins/` with
`SKSEMenuFramework.dll` 2,550,784 B dated Sep 14, `.dll.fresh` and `.dll.bak-May29-5dc6224` 4,104,192 B dated May 29,
`.pdb`, `.ini`, `_preload.txt` (0 bytes), `.LICENSE.md`, `Strings(_PT).json`, `fonts/` (fa-brands/regular/solid, SkyrimMenuFont.ttf), `SKSEMenuFrameworkThemes/{classic,modern,skyrimDefault}.json`).

| File | PE ProductVersion (UTF-16 VERSIONINFO) | Build evidence |
|---|---|---|
| `SKSEMenuFramework.dll` (the live one) | **3.14.0** | built by GitHub Actions (`D:\a\SKSE-Menu-Framework-3\...\extern\CommonLibSSE-NG`), contains `Dear ImGui 1.90.8 (19080)`, exports `GetMenuFrameworkAPIVersion`, `GetMenuFrameworkVersion`, `AddSectionItem`, `AddWindow`, `IsHotkeyEnabled`, `SetHotkeyEnabled`, `UnregisterInputEvent` |
| `.dll.fresh`, `.dll.bak-May29-5dc6224` | **3.0.0** | built from a local `D:\Programs\CommonLibVR` tree, has the VR address-library strings |

So the memory note is right in outline: the LoreRim folder holds BOTH, a 3.0.0 pair kept as backups and the 3.14.0
DLL as the active file. The MO2 `meta.ini` is stale (`version=3.9.0.0`, installationFile `...-3-9-...7z`, Nexus mod id 120352), do not trust it. Upstream
`pushedAt` 2026-09-29; no GitHub releases or tags exist (`gh release list` empty), releases are on Nexus only.
The exact string in the DLL is `961c151d2e87f2686a955a9be24d316f1362bf21 3.12.0` too (a baked git-hash string, meaning of the 3.12.0 vs ProductVersion 3.14.0 mismatch NOT CONFIRMED; trust the VERSIONINFO).

Repos: framework `QTR-Modding/SKSE-Menu-Framework-3` (author is Thiago099, now under QTR-Modding; Nexus page says
"Quantumyilmaz's idea"), header-only API repo `QTR-Modding/SKSE-Menu-Framework-3-API`, example
`QTR-Modding/SKSE-Menu-Framework-3-Example`.

Runtimes: FW Hooks.cpp uses `REL::RelocationID(se, ae, vr)` and `REL::Relocate(se, ae, vr)` triples, i.e. SE (1.5.97),
AE (1.6.x) and VR are all code-paths; it needs Address Library (strings in DLL). The DLL's declared compatible-version
list was NOT parsed, so "1.6.1170 works" is inferred from AE ids + the user running it in LoreRim (1.6.1170 is LoreRim's runtime per our own docs, not re-verified here). R says nothing explicit about runtimes: NOT FOUND in docs.
Requirements: Address Library, and **"ImGui Icons" (Nexus 114790)** for the gamepad button prompts (R). DESIGN.md does not list it.

Licences (this matters):
- Framework source repo: **GPL-3.0** (`gh repo view` licenseInfo gpl-3.0).
- API header repo (`SKSE-Menu-Framework-3-API`): **LGPL-2.1**. R: "The header file has a license that does not require mods using it as a library to be open source and with the same license". Safe to vendor into an MIT project with the LGPL notice kept next to it.
- The **compiled DLL**: `SKSEMenuFramework.LICENSE.md` says "(c) 2025 A. Thiago Kaqiue. All Rights Reserved. ... may not be shared, resold, repackaged, or included in any other distribution without prior written consent". The DLL even checks that this file is present and unmodified (FW `Licence.cpp`, `VALIDATE_LICENSE`). Conclusion: never ship or repackage the DLL; users install it from Nexus. (The GPL repo vs the "all rights reserved" binary is an inconsistency; NOT resolved, treat the stricter one as binding.)

## 3. Registration API with quoted code and sources

Include (H line 1-60 prelude; needs CommonLib types first since it uses `RE::InputEvent`, `std::filesystem`, `std::atomic`):

```cpp
#include "SKSEMenuFramework.h"
```
(U lines 1-5). The header is vendored into the plugin, there is no link library. It resolves everything at run time:

```cpp
// H:11-14
inline HMODULE GetMenuFrameworkModule() {
    static HMODULE menuFramework = nullptr;
    if (!menuFramework) { menuFramework = GetModuleHandleW(L"SKSEMenuFramework"); }
    return menuFramework; }
// H:17-24  GetMenuFrameworkFunction<T>(name) = GetProcAddress(module, name)  (null if module absent)
```

Installed check (H:37-40): `std::filesystem::exists("Data/SKSE/Plugins/SKSEMenuFramework.dll")` (relative to the game cwd; works under MO2's VFS).

Minimal working registration (EX `src/UI.cpp:4-12`, same shape as ReLight `src/Menu.cpp:14-18`; U "You should create a function in order to register your menu entries"):

```cpp
void UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) { return; }
    SKSEMenuFramework::SetSection("marth's Jukebox");          // top-level folder in the panel
    SKSEMenuFramework::AddSectionItem("Settings", UI::Render); // a page; "Folder/Sub page" nests
}
void __stdcall UI::Render() { /* ImGuiMCP:: calls only */ }
```
Signatures (H:42-150): `using RenderFunction = void(__stdcall*)();`
`inline void AddSectionItem(std::string menu, RenderFunction)` (prefixes `key + "/"`), `FullPathAddSectionItem(path, fn)`,
`SetSection(std::string)`, `RenameSection(path, newName)` / `DeleteSection(path)` (return false if unsupported by the installed DLL; U "Renaming and deleting menu entries at runtime"). Paths use `/` as separator, `\/` for a literal slash (H:148 comment, EX line 19).

When to call: **EX and ReLight both call `UI::Register()` straight from `SKSEPluginLoad`** (EX `plugin.cpp:16`, ReLight `plugin.cpp:72`), no message needed. The framework registers its own windows in its `SKSEPluginLoad` (FW plugin.cpp:29) and `AddSectionItem` just appends to a tree (`SKSEMenuFramework.cpp:15-27`), so it is not tied to kDataLoaded. Why this works even for plugins whose DLL name sorts before "SKSEMenuFramework": the framework ships an empty `SKSEMenuFramework_preload.txt`, which is the SKSE preload marker; that this is the mechanism is an inference, NOT CONFIRMED in the framework's docs. **Pitfall:** every API function caches `static auto func = GetFunction(...)`, so if the first call happens before the DLL is mapped, the null is cached for the whole session and a later retry at kPostLoad will not help. Do the registration once, at `SKSEPluginLoad`, and log `GetMenuFrameworkVersion()` / `GetMenuFrameworkAPIVersion()` (H:214-232) right after to prove it connected; if both return 0, log it loudly (framework missing) and run headless.

Other API (H:42-265):
- `AddWindow(RenderFunction, bool doesWindowPauseGame = true)` returns `WindowInterface*` with atomic `IsOpen` / `BlockUserInput`; `AddWindowWithView(fn, viewName, pause)`; `GetMainWindow()` (the panel itself, `->IsOpen = false` closes it, EX line 241).
- `AddHudElement(fn)` (draw on the foreground list when no blocking window is open), `AddInputEvent(bool(__stdcall*)(RE::InputEvent*))` (return true to swallow), `AddEvent(cb, priority)` for `kOpenMenu/kCloseMenu/kBeforeRender/kAfterRender` (H:44, ReLight uses kCloseMenu to save).
- `IsAnyBlockingWindowOpened()`, `LoadTexture(path, size)` / `DisposeTexture`, `PushFont(name)` + `FontAwesome::Pop()` (U "Font Awesome"; FontAwesome solid/regular/brands and fonts in `Data/SKSE/Plugins/Fonts`, optional `<font>.json` with `small/default/big` sizes).
- `SetHotkeyEnabled(bool)` / `IsHotkeyEnabled()` (H:199-208), see section 5.

## 4. Threading & ImGui context

- ImGui lives **inside the framework DLL**. We do not link or build imgui. The header exposes the full ImGui 1.90.8 C API as `ImGuiMCP::Xxx(...)` inline wrappers, each doing `GetMenuFrameworkFunction<func_t>("igXxx")` (cimgui exports; H:3819 ff., e.g. `igInputText` H:4789, `igButton` H:4562). Types are `ImGuiMCP::ImVec2` etc. Older docs (U) show `ImGui::`; the current header's namespace is `ImGuiMCP` (EX uses `ImGuiMCP::`), U is outdated on this point. Because our code never defines ImGui symbols, there is no duplicate-ImGui-symbol clash inside our DLL. A call per widget does a GetModuleHandle+GetProcAddress (no caching in the ImGuiMCP wrappers), negligible for one settings page.
- The context is created in the framework's `D3DInitHook` (`ImGui::CreateContext()`, DX11 + Win32 backends, `io.IniFilename = nullptr`, NavEnableKeyboard|Gamepad) (FW Hooks.cpp). `ImGuiMCP::GetCurrentContext()/SetCurrentContext()` exist, but we never need them: our callbacks are called with the framework's context already current.
- Render thread: `Render()` is invoked from `RenderUIHook::thunk1/thunk2` right after the engine's UI draw call (`REL::RelocationID(35556,36555,35556)` and `(38085,39039)` call sites), between `ImGui_ImplDX11_NewFrame` and `ImGui::Render`, then `kBeforeRender/kAfterRender` events. So our callback runs on the engine thread that draws the game UI (the main/render thread), once per frame, with no framework lock around it. The exact OS thread identity is NOT stated in any doc, but the practical rule is clear: **anything slow in the callback stalls the whole frame.** The tree has a `recursive_mutex menuTreeMutex` (UI.cpp:42) only around menu-tree edits. Our rule: callback reads `std::atomic` values and a mutex-guarded copy of a small struct (or a double-buffered snapshot), never touches WinHTTP, disk, or game forms.
- Game state while open: `FreezeTimeOnMenu = true` and `BlurBackgroundOnMenu` by default (framework INI); `AddWindow(fn, false)` makes a non-blocking window and the panel has "Resume Game" (R "Unpause"). The game is "locked" via `GameLock` (FW), so a game-thread SKSE-task poll (DESIGN 3.3, 250 ms) may be paused while the panel is open: NOT CONFIRMED, test it.
- Input: with a text field active, framework skips its own key handling (`if (!ImGui::IsAnyItemActive()) InputEventHandler::Process(...)`, Hooks.cpp); keys are fed through the game's InputEvent queue (translated by `UI::TranslateInputEvent`) plus the WndProc hook, so typing works. Clipboard paste for the key field: wrapper `SetClipboardText` exists (H:5814), a `GetClipboardText` wrapper was not looked up, Ctrl+V into InputText is standard ImGui and NOT TESTED here.
- Fonts / style / themes: framework-owned. User picks `MenuStyle` (SKYRIMDEFAULT/Modern/Classic) and `[Fonts]` in `SKSEMenuFramework.ini` (+ CJK/Cyrillic toggles); custom JSON themes in `Data/SKSE/Plugins/SKSEMenuFrameworkThemes/`. A plugin cannot choose its own style globally; it can `PushFont(name)` and use `PushStyleColor` etc. locally. We should not ship fonts.
- Controller navigation: Nexus text: open with gamepad LB double press, D-pad selects a section, A enters, B goes back; ImGui nav is enabled by the framework (`NavEnableGamepad`), widgets are navigable by default, an InputText on gamepad has no on-screen keyboard (NOT FOUND in docs/source): for text entry a controller user needs the keyboard.
- Persist settings: **NOT FOUND**. The framework saves only its own state (`Failed to save window layouts to SKSEMenuFramework.ini`, `SKSEMenuFrameworkMenuConfig.json`). ReLight saves in an event callback on `kCloseMenu`; do the same: write our INI/JSON on kCloseMenu (and Apply button), on a worker.

## 5. Worked example

ReLight (GPL-3.0, TrumanGIT/ReLight): `src/plugin.cpp:67-72` calls `UI::Register()` inside `SKSEPluginLoad`; `src/Menu.cpp:12-35`:

```cpp
void Register() {
    if (!SKSEMenuFramework::IsInstalled()) return;
    SKSEMenuFramework::SetSection("ReLight");
    SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
    ...
    SKSEMenuFramework::AddEvent(OnMenuEvent, 0);   // saves on kCloseMenu
}
```
Official example (EX `src/plugin.cpp:11-18`, `src/UI.cpp:4-24`) is the same with `SKSEMenuFramework::SetSection(MOD_NAME)`, nested path `"Folder Example/Example 2"`, a window via `AddWindow(RenderWindow, true)`, HUD via `AddHudElement`, event via `AddEvent(OnEvent, 0)`. Other registrants seen in a `gh search code` (not read): crajjjj/slwidgets, Frostwalker, Campfire-2026, FluidMotion, Dynamic-Damage-Scaling, adya/NPCs-Names-Distributor (via SKSE-MCP shim), etc.

Hotkey: the panel key is user config only (`ToggleKey = F1`, `ToggleMode`, gamepad `LB` DOUBLEPRESS). Plugin API is limited to `SetHotkeyEnabled/IsHotkeyEnabled`, `GetMainWindow()->IsOpen`, and `AddInputEvent` to build our own key that toggles our own `AddWindow` (EX `UI.cpp:191,198`). There is no "register my own hotkey for my page" API: NOT FOUND.

## 6. Fit for our page

| Need | Framework support | Verdict |
|---|---|---|
| Text field server URL | `ImGuiMCP::InputText(label, char* buf, size, flags)` H:4789 (also `InputTextWithHint` H:4800) | straightforward (fixed `char[256]` buffers) |
| Masked key | `ImGuiInputTextFlags_Password` exists (H:550) | straightforward; key sits in memory, written to INI plaintext (DESIGN decision) |
| Buttons | `Button` H:4562 | straightforward; set an atomic/enqueue a request to the worker, never call network inline |
| Checkboxes | `Checkbox(label, bool*)` H:4582 | straightforward (our bool storage; copy to config on change) |
| Combo (source picker) | `BeginCombo/EndCombo/Selectable` H:4629 | straightforward |
| Live-searched list | InputText + `BeginListBox/Selectable` (H:5003 etc.) fed from a vector copy updated by the worker (debounce in the worker, publish results through a shared_ptr/mutex swap) | possible but manual: no built-in async search widget; clipping long lists needs `ImGuiListClipper` (not checked in H); beware result vector mutation while rendering |
| Progress / status line | `ProgressBar` H:4607, `TextColored` H:4492 | straightforward, from atomics |
| Async work | none provided; our own worker thread, callback only reads atomics | fits the plan; framework imposes nothing |
| Persist | not provided | our INI, write on kCloseMenu/Apply |
| Many categories (~12) | sections/sub-pages with `/` paths, or one page with CollapsingHeader | straightforward |
| Controller | partly (nav yes, text entry needs keyboard) | acceptable, note in README |
| Per-category preview (audio) | n/a to the framework | ours |

Not straightforward: the live search (manual), controller text entry, and any assumption that our worker thread may call ImGui (it must not; ImGui is single-threaded).

## 7. Risks & fallback

Risks: (1) null function pointers cached if registration runs before the DLL maps (silent no-op page): register in `SKSEPluginLoad`, verify with `GetMenuFrameworkVersion()` and log; (2) ABI/API drift: framework is at 3.14 and moves fast (151+ commits, header "older headers remain compatible" per R); pin the header copy we vendor and feature-test `RenameSection` etc. by return value; (3) licence: DLL all-rights-reserved, never bundle; mention it and Address Library and ImGui Icons as requirements in the Nexus description; (4) 3 DLLs in the same MO2 folder (3.0.0 VR build vs 3.14.0) is a LoreRim local state, not a user-facing issue, but test against the active 3.14.0 only; (5) callback on the render thread: any block is a frame hitch; (6) paused game while panel open may starve a game-thread poll or the music state machine; (7) another ImGui user in the process (including our own Board-style fallback) duplicates the D3D init/Present hooks and creates a second context, which is the known conflict MFO's Board would have with this framework; (8) closed-source binary, single maintainer, a framework crash takes the page with it; (9) text entry on gamepad; (10) header uses `std::wstring_convert` (deprecated, C++17-only), `RE::InputEvent` dependency: build against CommonLibSSE-NG with C++23 may warn.

Fallback (own overlay as in MFO `native/Board.cpp`): create our own ImGui context and DX11/Win32 backends, hook `IDXGISwapChain::Present` (vtable slot 8) and `ResizeBuffers` (slot 13) like Board does, own input capture and a hotkey; cost is real (ImGui build via vcpkg, input blocking, controller handling, font/style) and it collides with the framework's hooks if both are installed, so it must be a build-time/INI choice, mutually exclusive with registering a page.

## 8. Open questions

1. Is the preload marker the reason early-loading plugins can call the API from `SKSEPluginLoad`? Test with a plugin named before "SKSEMenuFramework" (Jukebox sorts earlier: "marth..." < "SKSE...").
2. Does the 3.14.0 DLL declare AE 1.6.1170 compatible (SKSEPlugin_Version table not parsed)? Check in-game log.
3. Does a game-thread SKSE task keep ticking while the panel freezes the game? Does music keep playing?
4. Paste (Ctrl+V) into a Password InputText; clipboard works under Wine/Proton?
5. Exact OS thread of the render callback (assumed engine main thread).
6. Is `ImGuiListClipper` exposed by the header, needed for >1000 search results.
7. Licence ambiguity GPL-3.0 repo vs "All Rights Reserved" DLL: only matters if we ever want to bundle; we do not.

### Conflicts with DESIGN.md (3.6 and "8.2")
- There is no section 8.2; DESIGN.md ends at section 7. The framework text is in 3.6 (item 6 of section 3) and 7 (M2).
- 3.6 says the API licence was "to be verified": header LGPL-2.1 (OK), DLL not redistributable (new constraint, matters for the Nexus listing and any installer).
- 3.6 says "hard requirement": it also pulls in **ImGui Icons (Nexus 114790)** for gamepad prompts and Address Library; not in the plan.
- 3.6 says "verified for 1.5.97/1.6.1170": only inferred (RelocationID triples and the installed 3.14.0 DLL); not proven.
- The brief/DESIGN claim "MFO's Board" as fallback is accurate, but MFO is NOT an existing user of the framework.
- DESIGN 3.6 "Settings persist to an INI": correct, the framework offers none.
