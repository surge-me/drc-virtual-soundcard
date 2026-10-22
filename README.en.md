# DRC Virtual Soundcard — System-Level Audio Dynamic Range Controller for Windows

**Gunshots pushed back under the ceiling, footsteps lifted into audibility.** A zero-kernel-driver, user-mode audio chain: virtual-cable capture → real-time DSP (lookahead limiter + upward compander + 10-band EQ) → real output device, with a WPF tray GUI, global hotkeys and a live level curve.

[中文](README.md) | **English**

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![driver](https://img.shields.io/badge/kernel%20driver-none-success)
![license](https://img.shields.io/badge/license-MIT-green)
![tests](https://img.shields.io/badge/ctest-13%2F13%20passing-success)
![release](https://img.shields.io/github/v/release/surge-me/drc-virtual-soundcard?label=release)

> **Just want to use it, not build it?** Get `DRC-VirtualSoundcard-*-win-x64.zip` from
> [**Releases**](https://github.com/surge-me/drc-virtual-soundcard/releases/latest), unzip and run
> `DrcGui.exe` (self-contained — no .NET runtime install needed).

---

## 1. What problem does it solve

The classic FPS dilemma: **gunshots and explosions are painfully loud, footsteps and reloads are inaudible.** Turning the master volume down protects your ears — and removes the footsteps with it.

DRC does not change the overall volume. It **compresses the dynamic range**, processing audio in three bands by level:

| Input level | Processing | Parameter |
|---|---|---|
| **≥ ceiling** | Lookahead limiting — transients pushed back to the ceiling, without clipping distortion | `ceilingDb` (factory −6) |
| **≤ floor** | Upward compression — quiet sounds lifted, band-limited, capped | `floorDb` (factory −40), cap `maxBoostDb` (factory 10) |
| **in between** | Linear pass-through — untouched, original timbre preserved | — |

A 10-band graphic EQ sits on top for taste.

**Every parameter is hot-updatable**: change it in the GUI and the running audio chain picks it up on the next processing block — no dropout, no restart.

---

## 2. Key features

- **Lookahead limiter** — `lookaheadMs` (default 2.5 ms) of look-ahead reduces gain *before* the transient arrives, so peaks are caught without clipping.
- **Upward compander** — lifts quiet signals; `releaseMs` (default 120 ms) controls recovery to avoid a pumping feel.
- **Mid-level pass-through** — not "compress everything": the normal listening range is left as-is.
- **10-band graphic EQ** — 31 / 62 / 125 / 250 / 500 / 1k / 2k / 4k / 8k / 16k Hz, peaking biquads, Q=1.1, ±12 dB.
- **Hot parameter updates (seqlock double-write)** — the GUI writes shared memory; the audio thread reads a `seq` parity-checked snapshot every block. Lock-free, tear-free.
- **Live level visualisation** — 30 fps read-back of input / output / gain-reduction curves, with `ceiling` / `floor` reference lines.
- **Tray GUI** — quick bypass (red/grey icon shows state at a glance), **one-click output switching** (headphones ⇄ speakers, no settings page needed).
- **Global hotkeys** — `Ctrl+Alt+B` bypass on/off · `Ctrl+Alt+↑/↓` ceiling ±0.5 dB · `Ctrl+Alt+Shift+↑/↓` floor ±0.5 dB. Work with the window minimised to tray and while a game is in exclusive fullscreen.
- **Endpoint hot-plug self-healing** — when the headset is unplugged or the virtual cable is reinstalled and the endpoint dies, the router process **does not exit**: it waits for the endpoint to return, re-resolves it by name and reconnects. The GUI side has a back-off restart as a second line of defence, and always displays the *actual* current output.
- **Zero kernel driver / zero system-file modification** — pure user-mode WASAPI shared mode. No changes to the system audio stack, no injection into any game process.

---

## 3. Architecture

```
   Game / system audio
        │   (Windows "Default Playback Device" set to the virtual cable input: CABLE Input)
        ▼
  ┌────────────────┐  capture   ┌──────────────────┐  render  ┌──────────────────┐
  │ VB-CABLE        ├───────────►│  DrcRouter.exe   ├─────────►│ Real output      │
  │ (3rd-party      │            │  WASAPI shared   │          │ headphones /     │
  │  virtual cable) │            │   └─► dsp-core   │          │ speakers         │
  └────────────────┘            └────────┬─────────┘          └──────────────────┘
                              params down / levels up (shared memory, seqlock)
                                         │
                                ┌────────▼─────────┐
                                │  WPF tray GUI     │  Dynamic range / EQ / Settings
                                │  DrcGui.exe       │  live curve + tray menu
                                └──────────────────┘

  Global\DrcApoSharedMem_v1 (4096 B):
      offsets 0–511    params  (GUI → DSP)
      offsets 512–1023 levels  (DSP → GUI)
```

| Module | Language | Responsibility |
|---|---|---|
| `apo/dsp` | C++ (static lib, no Windows dependency) | Single source of truth for the algorithms: limiting / compander / EQ / parameter smoothing. Guarded by golden vectors + mutation testing. |
| `router` | C++ (WASAPI) | Real-time capture → DSP → render pipeline; endpoint watchdog and auto-reconnect; IPC read/write. |
| `gui` | C# / WPF (.NET 8) | Three-page UI, live metering, tray menu, router process supervision, config persistence. |
| `apo/com`, `installer` | C++ / C# | **Archived**: legacy of the earlier APO (Audio Processing Object) injection approach (see §4). |

**Why a user-mode pipeline instead of a kernel driver**: kernel audio drivers need signing + WHQL, carry high BSOD risk, require a reboot to install and uninstall, and many anti-cheat systems watch kernel modules. The user-mode WASAPI build is driver-free, reboot-free and cleanly removable — the trade-off is that it needs a virtual cable (VB-CABLE here) as the capture entry point.

---

## 4. Two integration shapes (including one real pivot)

1. **Virtual cable (current main path)** — the shape diagrammed above. Depends on a third-party virtual soundcard for the capture endpoint; best compatibility, removable at any time.
2. **APO COM DLL injection (archived)** — writes directly into the Windows audio engine's FX chain (same principle as Equalizer APO). This project implemented and hardware-verified it as far as "engine loads + audio thread runs" (including registry injection / backup / uninstall, signing, and LFX/GFX slot differences) — and then **the project owner decided to abandon it**: injection carries uncontrolled risk in anti-cheat contexts. The system side was fully restored (value-by-value comparison against the backup matched). The code remains in `apo/` and `installer/` as archive only and is no longer developed.

---

## 5. Quick start

### Option A: download the package (recommended for end users)

Get `DRC-VirtualSoundcard-<version>-win-x64.zip` from
[Releases](https://github.com/surge-me/drc-virtual-soundcard/releases/latest) → unzip to any directory
(**not** `C:\Program Files` — that needs administrator rights and config/log writes will fail) → run `DrcGui.exe`.

The package ships a `使用说明.txt` (Chinese) covering first-time setup, hotkeys, tuning advice and uninstall steps.

### Option B: build from source

```bat
git clone https://github.com/surge-me/drc-virtual-soundcard.git
cd drc-virtual-soundcard
```

### Prerequisites

- Windows 10 / 11 x64 (**tested on Windows 11 24H2**; Windows 10 unverified)
- A virtual soundcard: **VB-CABLE** (third-party donationware, **not bundled and not redistributed** by this repo; the GUI offers a one-click link)
- Only for Option B: [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0)
  + Visual Studio 2022 (C++ desktop workload) + CMake ≥ 3.15
  (the Option A package is a self-contained publish — no .NET runtime needed)

### Three steps to get running

1. Open Windows sound settings and set the **default playback device** to `CABLE Input (VB-Audio Virtual Cable)`
   (**not** `CABLE In 16ch` — that is the 16-channel endpoint; a format mismatch makes the router exit with code `5`).
2. Open DRC, go to the **Settings** page, and pick your **real** headphones/speakers as **Output device**
   (changing it takes effect **immediately**, no router restart needed); leave "auto-switch when offline" enabled.
3. Click **Start router**. When "captured / frames processed > 0" appears in the bottom-right corner it is live —
   from then on both games and media are processed by DRC and returned to your headphones.

**Tray menu**: right-click the tray icon for quick bypass, **one-click output switching**
(`Switch to <the other device>`), and quit.
No need to open the settings page to swap devices temporarily: the menu item is named after the other real
output device you can switch to; clicking it takes effect immediately and is persisted.

**Global hotkeys** (active while the window is in the tray and while a game is in fullscreen):

| Shortcut | Action |
|---|---|
| `Ctrl+Alt+B` | Bypass on / off (instant A/B comparison) |
| `Ctrl+Alt+↑` / `Ctrl+Alt+↓` | Ceiling ±0.5 dB (gunshots are pushed back to this line) |
| `Ctrl+Alt+Shift+↑` / `Ctrl+Alt+Shift+↓` | Floor ±0.5 dB (only what falls below this is lifted) |

---

## 6. Build

```bat
:: ---- C++ side (DSP + router + tests) ----
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure      :: expect 13/13 Passed

:: ---- GUI ----
dotnet build gui/src/DrcGui -c Release                     :: expect 0 warnings 0 errors

:: ---- assemble the distributable directory (dist\DrcGui) ----
dotnet publish gui/src/DrcGui -c Release -o dist\DrcGui
copy /y build\router\Release\DrcRouter.exe dist\DrcGui\
```

> ⚠️ `DrcRouter.exe` is **not** part of the GUI project — `dotnet publish` will not include it. You must copy it
> into `dist\DrcGui\` manually and verify the two md5 sums match, otherwise users keep running the old router
> process (this has actually happened here).

---

## 7. Repository layout

```
├─ apo/
│  ├─ dsp/            DSP algorithm source of truth (C++ static lib): limiter / compander / EQ / smoothing
│  ├─ com/            [archived] APO COM DLL shell
│  └─ include/        drc_ipc.h — shared-memory layout (C++ side of the IPC contract)
├─ router/            WASAPI capture → DSP → render real-time pipeline + endpoint watchdog
│  └─ src/DrcRouter/  CLI entry point (--capture / --render / --list-devices …)
├─ gui/
│  └─ src/DrcGui/     WPF tray GUI (Dynamic range / EQ / Settings pages)
├─ installer/         [archived] FX-chain registry injection / backup / uninstall
└─ tests/
   ├─ dsp/            T1–T7: golden vectors + mutation testing
   └─ router/         R1–R6: pipeline regression (including endpoint watchdog policy)
```

> Internal task cards, development logs, on-device screenshots and build/forensics scripts are local development
> material and are **not published in this repository**; only buildable source and tests are kept here.

---

## 8. Parameters (frozen vocabulary)

One entity, one name, across the whole stack — GUI / IPC / DSP / docs share them:

| Parameter | Meaning | Unit | Factory default (GUI) / range |
|---|---|---|---|
| `ceilingDb` | Ceiling (gunshots pushed back to this line) | dB | -6 (-24 ~ -1, step 0.5) |
| `floorDb` | Floor (footsteps lifted toward this line) | dB | -40 (-60 ~ -24, step 0.5) |
| `maxBoostDb` | Maximum upward boost | dB | 10 (0 ~ 24) |
| `lookaheadMs` | Look-ahead time | ms | 2.5 (1 ~ 10) |
| `releaseMs` | Release time | ms | 120 (20 ~ 500) |
| `eqGainDb[10]` | Ten-band EQ gain | dB | all 0, ±12 |
| `bypass` | Bypass (output identical to input, sample by sample) | bool | false |
| `seq` | Parameter sequence (seqlock: +1 before write, +1 after) | uint32 | — |

> The DSP static library's own struct defaults are `-6 / -50 / 12`; the GUI writes the factory values above on
> first run, and from then on the config file is authoritative.

**EQ band frequencies**: 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz.

---

## 9. Command line (`DrcRouter.exe`)

```
DrcRouter.exe [--capture <substring>] [--render <substring>]
              [--buffer-ms <n>] [--duration-ms <n>]
              [--simulate-device-loss-ms <n>] [--list-devices] [--help]

--capture                    Match the capture endpoint name by substring (default "CABLE Output")
--render                     Match the render endpoint name by substring (default: system default render device)
                             Matching = friendly name + space + device description, case-insensitive, Unicode-aware
--buffer-ms                  Engine buffer duration in ms (default 10)
--duration-ms                Exit automatically after n ms (default: run until Ctrl+C)
--simulate-device-loss-ms    Test hook: inject one "endpoint lost" event n ms after start (3 s, single shot)
                             Default 0 = disabled; never used in production, only to verify endpoint self-healing
--list-devices               List active audio endpoints and exit
```

**Exit codes**: `0` OK · `2` usage error · `3` virtual soundcard not found · `4` audio initialisation failed · `5` format mismatch.

> The render endpoint **must be an explicitly specified real device**. If the router also uses the "system default
> render device" and that default happens to be the virtual cable input, the processed audio is fed back into the
> virtual cable ⇒ **self-recursion**.

---

## 10. Quality and evidence discipline (how this is developed)

This project is run on the principle that **every claim must be falsifiable**:

- **Four-state facts**: `confirmed` / `unverified` / `refuted` / `blocked`. Any registry or engine behaviour that has
  not been measured is marked `unverified` and may never be written as confirmed.
- **Compute-on-the-spot forensics**: any "red / green / equal / not equal" conclusion must carry a freshly computed
  md5 + byte count + line count + timestamp, and be measured at least twice with identical results before reporting.
  Cached values are not accepted.
- **Screenshot every visual**: UI / rendering / layout changes are always verified with a screenshot — "the code looks
  right" is not evidence.
- **Check the byte count on every write**: `0` bytes means the write failed, not that "the content is empty".
- **End-to-end discriminating power**: fix-type changes require a mutation control — deliberately build a
  "pre-fix behaviour" variant and run the same script against it. One must pass and the other fail; otherwise the
  criterion has no discriminating power.

Current baseline: `ctest` **13/13** (T1–T7 DSP + R1–R6 router); GUI builds with 0 warnings, 0 errors.

---

## 11. Deliberately not included in this repository

| Excluded | Reason |
|---|---|
| `docs/` (task cards / dev logs / on-device screenshots) | Internal development material containing local environment records; not published |
| `scripts/` (build / forensics / gate scripts) | Internal tooling tightly coupled to the local machine |
| `build/`, `build-mutant/`, `dist/`, `**/bin`, `**/obj` | Build artifacts, reproducible |
| `tools/` (raw Procmon traces, third-party binaries) | Raw traces and third-party binaries; copyright belongs to their authors, obtain them yourself |
| `installer/backup/` | Local registry / ACL restore backups containing **this machine's** audio endpoint GUIDs — meaningless elsewhere |
| `gui/config/drc-config.json` | Runtime config (contains local device GUIDs), generated on first run |
| VB-CABLE driver and installer | Third-party donationware; obtain from the official source. This repo only calls it, never redistributes it |

---

## 12. Known limitations (honest disclosure)

- **No double-blind listening validation**: DSP behaviour is guarded by golden vectors and numeric assertions
  (e.g. 0 dBFS → -6.000 dBFS equals `ceiling`; -60 dBFS → -50.0 dBFS equals `floor`; EQ ±6 dB @1k measured ±6.000 dB;
  bypass identical sample by sample), but whether it *sounds* good is for you to judge.
- **Physical endpoint hot-plug has not been validated on real hardware**: this machine cannot disable an endpoint
  without breaking the user's own audio path (all three `pnputil` approaches failed in testing). End-to-end validation
  uses the router's built-in fault-injection hook (real process / real WASAPI / real dual endpoints / real thresholds /
  real reconnect — only the "endpoint death signal" is simulated), paired with a mutation control.
- **"Wireless headset powered off only" cannot be auto-degraded**: measurements show zero signal on the Windows side in
  that case (endpoint still Active, no PnP anomaly, problem code 0, USB count unchanged), so only one-click switching is
  offered, not automatic switching.
- **No installer**: uninstalling = delete the directory + delete the shortcut. The release package is
  **self-contained** (`win-x64`, no .NET runtime required), roughly 62 MB.
- **Windows 10 unverified**: all measurements were made on Windows 11 24H2.
- **Single-machine validation**: all data comes from one machine (one VB-CABLE + one USB wireless headset + onboard
  audio); other hardware combinations may differ.

---

## 13. License and disclaimer

- Code is released under the **MIT** license, see [`LICENSE`](LICENSE).
- This project performs **audio post-processing only**: it does not read or write any game process memory, does not
  inject into games, and does not modify system audio files.
- If you use it with a competitive game that has anti-cheat, please check that game's terms of service yourself; the
  compliance of audio post-processing is your own judgement.
- The virtual soundcard (VB-CABLE) is third-party software; its license and distribution rules belong to its author.
  This repository neither bundles nor redistributes it.
- Audio processing involves playback devices related to hearing — control the volume yourself. The author is not liable
  for any damage caused by using this software.
