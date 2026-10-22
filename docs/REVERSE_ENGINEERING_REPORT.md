# Reverse Engineering Report: Windows Snipping Tool High-FPS Architecture

## Executive Summary

Windows Snipping Tool (`SnippingTool.exe`, packaged under `Microsoft.ScreenSketch`) has long been restricted to recording video at a fixed 30 frames per second. Even on 144 Hz, 240 Hz, or 360 Hz gaming displays with powerful GPUs, video recordings exhibit noticeable stutter and judder.

Through binary analysis and dynamic instrumentation, we discovered that this 30 FPS ceiling is **not** an architectural hardware limitation. Rather, it is enforced by **4 independent throttle mechanisms** spanning the Windows Graphics Capture (WGC) compositor, frame generation math, an internal micro-jitter frame dropper, and Media Foundation encoder initialization.

This document outlines each mechanism, how they interact, and how **High Snipping Frames** resolves them dynamically in memory.

---

## 1. The 4 Bottlenecks Analyzed

```
+-------------------------------------------------------------+
|                       DISPLAY MONITOR                       |
|                   (e.g., 144 Hz Refresh Rate)               |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| [Layer 1] Windows Graphics Capture (WGC) Compositor         |
| Stock behavior: Throttles frame delivery to 60 Hz by default|
| Fix: Call IGraphicsCaptureSession5::put_MinUpdateInterval   |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| [Layer 2] VideoFrameGenerator::OnFrameArrived (Dropper)     |
| Stock behavior: Drops frames if delta < 33 ms (7D 0E jge)   |
| Fix: Patch conditional jump to unconditional jmp (EB 0E)    |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| [Layer 3] VideoFrameGenerator::Initialize (Interval Math)   |
| Stock behavior: Divides 1000 / 30 = 33 ms minimum interval  |
| Fix: Patch interval calculation to 1000 / TargetFPS (6 ms)  |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| [Layer 4] MediaEncodingProfile / IMediaRatio (Encoder)      |
| Stock behavior: Hardcoded 30/1 ratio into Media Foundation  |
| Fix: Hook IMediaRatio getter/setter at 0x140085e20 to 144/1 |
+-------------------------------------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|                      OUTPUT MP4 FILE                        |
|                     (Encoded at 144 FPS)                    |
+-------------------------------------------------------------+
```

---

### Layer 1: Windows Graphics Capture (WGC) Compositor Throttle

On modern Windows 11 (build 22621+ and 24H2 build 26100+), DWM's Windows Graphics Capture subsystem imposes a default frame delivery cap of ~60 Hz (16.6 ms per frame) to preserve battery life and compositor overhead.

- **Interface:** `Windows.Graphics.Capture.IGraphicsCaptureSession5`
- **IID:** `67c0ea62-1f85-5061-925a-239be0ac09cb`
- **Method:** `put_MinUpdateInterval(TimeSpan value)` (vtable slot 7)
- **High Snipping Frames Solution:** Before `IGraphicsCaptureSession::StartCapture` is called, our hook intercepts the session wrapper and invokes `put_MinUpdateInterval(10,000,000 / TargetFPS)` (69,444 ticks / ~6.94 ms for 144 FPS). This tells DWM to push frames immediately upon presentation without compositor throttling.

---

### Layer 2 & 3: Frame Dropper & Interval Math

Inside `VideoFrameGenerator` (`SnippingTool.exe`), two routines actively drop high-rate frames:

1. **Interval Calculation (`0x140091d6c`):**
   ```asm
   33 D2             xor edx, edx
   B8 E8 03 00 00    mov eax, 1000
   F7 F1             div ecx        ; ecx = 30 -> eax = 33 ms
   ```
   Our patch replaces this with `B8 06 00 00 00 90 90` (`mov eax, 6; nop; nop`), setting the interval to 6 ms.

2. **Micro-Jitter Frame Dropper (`0x140092eb4`):**
   ```asm
   10 27 00 00       ; 10000 multiplier
   48 3B C1          cmp rax, rcx
   7D 0E             jge drop_frame
   ```
   If a frame arrives earlier than 33 ms from the previous frame, it is rejected. Our patch replaces `7D 0E` with `EB 0E` (`jmp`), completely bypassing the drop branch.

---

### Layer 4: Encoder Media Foundation Ratio

When `Encoder::Initialize` (`0x140084a30`) constructs the output `MediaEncodingProfile`, it calls `get_Video()` and queries the `IMediaRatio` (`d2d0fee5-8929-401d-ac78-7d357e378163`) of `IVideoEncodingProperties`.

- **Routine `0x140085e20`:** Snipping Tool queries `IMediaRatio::get_Numerator` and multiplies `width * height * fps * 2 * factor` to calculate target bitrate.
- **High Snipping Frames Solution:** We hook `0x140085e20` using a 14-byte direct jump to `MediaRatioHook`. In this routine, we call:
  - `IMediaRatio::put_Numerator(pRatio, TargetFPS)` (slot 6)
  - `IMediaRatio::put_Denominator(pRatio, 1)` (slot 8)
  - Return `TargetFPS` in `RAX` so that the encoder automatically calculates appropriate high-framerate bitrates.

---

## 2. Surviving Windows & Microsoft Store Updates

Static modification of files inside `C:\Program Files\WindowsApps\Microsoft.ScreenSketch_*` has severe drawbacks:
1. Microsoft Store updates wipe out modified files and restore stock versions.
2. Package signature checks and AppContainer integrity verification can flag modified binaries.
3. Windows Control Flow Guard (CFG) triggers `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` (`0xC0000409`) on tampered indirect call targets.

### The Solution: Dynamic In-Memory AOB Scanning

`High Snipping Frames` never touches disk binaries in the package:
- All 4 patches are located using unique Array of Bytes (AOB) patterns inside `.text` memory at runtime.
- Near calls and direct branches are patched, avoiding CFG bitmap traps.
- When Microsoft Store updates Snipping Tool to a new build, the background daemon automatically re-locates the byte signatures and applies the patches on the fly.
