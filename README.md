<p align="center">
  <img src="assets/icon.png" width="130" height="130" alt="High Snipping Frames Logo" />
</p>

<h1 align="center">High Snipping Frames</h1>

<p align="center">
  <strong>Featherweight 60, 90, 120, 144, 165, and 240 FPS Screen Recording for Windows Snipping Tool</strong><br />
  <sub>v2.0 Overhaul — Event-Driven, Zero Notification Spam, Pure Win32 GDI</sub>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Version-2.0-10B981?style=for-the-badge" alt="Version 2.0" />
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011-0078D4?style=for-the-badge&logo=windows11&logoColor=white" alt="Windows 11" />
  <img src="https://img.shields.io/badge/Recording-60%20%7C%20120%20%7C%20144%20%7C%20240%20FPS-38BDF8?style=for-the-badge" alt="High FPS" />
  <img src="https://img.shields.io/badge/Idle%20CPU-~0.0%25-10B981?style=for-the-badge" alt="Zero Idle CPU" />
  <img src="https://img.shields.io/badge/Updates-Survives%20Store%20Updates-059669?style=for-the-badge&logo=microsoftstore&logoColor=white" alt="Update Proof" />
  <img src="https://img.shields.io/badge/License-MIT-gray?style=for-the-badge" alt="MIT License" />
</p>

---

## Overview

Windows Snipping Tool (`Microsoft.ScreenSketch`) natively caps video recordings to **30 FPS**. On 120 Hz, 144 Hz, or 240 Hz displays, recordings look choppy compared to what is on screen.

**High Snipping Frames v2.0** unlocks smooth **60, 90, 120, 144, 165, and 240 FPS** recording directly inside the stock Windows Snipping Tool using lightweight, in-memory timing unlocks—without modifying any system files inside `WindowsApps`.

### What Changed in v2.0
- **Event-Driven Detection (~0% Idle CPU):** Replaced continuous 200ms `CreateToolhelp32Snapshot` process/module polling with Win32 `SetWinEventHook` (`EVENT_OBJECT_SHOW` + `EVENT_SYSTEM_FOREGROUND`) and a sparse 3-second `EnumProcesses` PID-diff fallback that only inspects newly spawned PIDs.
- **Zero Notification & Clipboard Interference:** Removed all unsolicited tray balloon spam, clipboard hijacking (`CF_HDROP`), background disk scanners, and external `ffprobe` subprocesses.
- **Minimal Footprint & Non-Intrusive Design:** Pure Win32 GDI Control Center (no GDI+ dependency, backbuffer freed on hide), least-privilege process inspection, zero runtime registry `IFEO` access, and no overlay windows or thread priority hooks injected inside Snipping Tool's AppContainer.
- **Live Config Reloading:** Changing Target FPS or Bitrate in the Control Center writes the `.ini` file and takes effect on your very next recording (`GetFileAttributesExW` mtime check).

---

## Architecture: The 4 Timing Unlocks

Windows Snipping Tool restricts video capture frame rate across four layers. High Snipping Frames unlocks all four cleanly in memory:

| Layer | Component | Stock Snipping Tool | High Snipping Frames v2.0 |
|---|---|---|---|
| **1. Compositor** | `IGraphicsCaptureSession5` | Default capture throttling | Sets `put_MinUpdateInterval` at `StartCapture` with 15% VSync jitter headroom (`85%` of `1 / TargetFPS`). |
| **2. Frame Dropper & Queue** | `VideoFrameGenerator` | Drops frames `< interval` and slices 50%–90% of frames past `80`/`100` queued frames (`10`-buffer WGC pool) | Bypasses micro-jitter drop, disables mid-queue decimator, scales overflow headroom to `4 × TargetFPS` (`300–900` frames), and expands frame pool to `20` buffers to stop fast-mouse startup cuts. |
| **3. Interval Math** | `VideoFrameGenerator::Initialize` | Computes `1000 / 30` interval | Patches interval immediate to calibrated `850 / TargetFPS` ms (clamped `>= 1 ms`). |
| **4. Encoder & Color Profile** | `IMediaRatio` & `VideoEncodingProperties` | Hardcoded `30/1` ratio, `H.264 Main`, untagged color matrix/range | Sets `put_Numerator(TargetFPS)` / `put_Denominator(1)`, calibrated bitrate basis with 4K overflow guard, upgrades H.264 to **High Profile (`100`)**, and tags Full-Range RGB input → **BT.709** color metadata for sharp details and accurate colors. |

---

## Quick Start

### Option 1: Install (`install.bat`)
1. Clone or download the release ZIP.
2. Double-click **`install.bat`** (or run `.\scripts\install.ps1` in PowerShell).

The installer copies binaries to `C:\Program Files\SnippingToolHighFPS\`, sets read/execute ACLs for Snipping Tool's AppContainer (`*S-1-15-2-1`), creates a Start Menu shortcut, adds a silent `--minimized` `HKCU\...\Run` startup entry, and starts the tray Control Center.

### Option 2: Portable Mode
Run **`bin\HighSnippingFramesGUI.exe`** directly from any folder. It creates/reads a local `SnippingToolHighFPS.ini` beside the executable and ensures AppContainer read/execute access once cleanly.

---

## Tray & Control Center

- **Left-click tray icon:** Toggle the Control Center window.
- **Right-click tray icon:** Choose target framerate (`Auto`, `60`, `90`, `120`, `144`, `165`, `240`), launch Snipping Tool, open your `Videos\Screen Recordings` folder, rescan processes, or exit.
- **Global Hotkey:** `Ctrl + Alt + R` quick-launches Snipping Tool (optional toggle in Control Center).

---

## Configuration (`SnippingToolHighFPS.ini`)

Stored in `C:\ProgramData\SnippingToolHighFPS\SnippingToolHighFPS.ini` (installed) or alongside the executable (portable):

```ini
[Settings]
; Target Frame Rate (0 = Auto-match active monitor refresh rate, or 60, 90, 120, 144, 165, 240)
TargetFPS=0

; Video Bitrate Profile (0 = Balanced ~20M, 1 = High ~27M, 2 = Ultra ~35M at 1080p)
BitrateMultiplier=0

; Per-Monitor Dynamic Refresh Rate Matching when TargetFPS=0
PerMonitorAutoMatch=1

; Global Quick-Launch Hotkey (1 = Ctrl + Alt + R, 0 = Disabled)
EnableGlobalHotkey=1

; Frame Timing, Queue & Color Fixes (1 = Enabled, 0 = Disabled)
UnlockCompositor=1
UnlockEncoder=1
BypassThrottle=1
PreventStartupDrops=1
OptimizeColor=1
```

---

## Building from Source

Requires Windows 10/11 (64-bit) with MinGW-w64 (`g++` and `windres` in `PATH`):

```cmd
cd scripts
build.bat
```

Outputs `bin\SnippingToolHighFPS.dll` and `bin\HighSnippingFramesGUI.exe`.

---

## Uninstallation

Double-click **`uninstall.bat`** (or run `.\scripts\uninstall.ps1`). Everything installed (shortcut, startup key, install folder, ProgramData folder, and any legacy v1.0 IFEO keys) is removed cleanly.

---

## License

MIT License — see [LICENSE](LICENSE).
