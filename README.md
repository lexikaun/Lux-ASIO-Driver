# Lux ASIO Driver

A modern, low-latency, user-mode Windows ASIO driver built on top of the WASAPI `IAudioClient3` shared-mode engine.

## Features

- **`IAudioClient3` Low-Latency Shared Mode:** Leverages the Windows low-latency audio engine period negotiation while allowing simultaneous multi-client audio output (e.g. DAW + YouTube / Spotify).
- **Exclusive Mode Opt-In:** One checkbox in the control panel bypasses the shared engine entirely for the lowest latency the hardware driver allows (typically 3–5 ms periods even on consumer codecs locked at 10 ms shared). The device becomes single-app while active; format conversion (float32/int32/int24/int16) is automatic.
- **Buffer Decoupling Engine:** Lock-free atomic ring buffers bridge the DAW's ASIO block size (64 to 2048 samples) and the hardware endpoint's native WASAPI period, solving the fixed-buffer flaw.
- **MMCSS Pro Audio Thread Scheduling + P-Core Pinning:** Real-time audio threads registered with Windows MMCSS (`AvSetMmThreadCharacteristicsW`, `AVRT_PRIORITY_CRITICAL`) and pinned to performance cores via CPU Sets on hybrid CPUs.
- **Native Hardware Setup Control Panel:** Lightweight Win32 dialog allowing real-time device selection and buffer size switching via `kAsioResetRequest`.
- **Pure User-Mode COM Component:** 100% user-mode implementation without kernel-mode driver dependencies.

## Architecture

- **ASIO Shim (`src/lux_asio.cpp`):** Implementation of Steinberg's `IASIO` COM interface.
- **WASAPI Backend (`src/wasapi_backend.cpp`):** Handles `IAudioClient3` device discovery, format negotiation, and shared streams.
- **Audio Thread & Ring Buffer (`src/audio_thread.cpp`, `src/ring_buffer.h`):** High-priority worker loop with SPSC lock-free block adaptation.
- **Control Panel (`src/control_panel.cpp`, `src/panel.rc`):** Native Windows dialog for device enumeration and buffer reconfiguration.

## Building

### Prerequisites
- Visual Studio 2022 / Build Tools with C++ and ATL support
- CMake (>= 3.15)
- Windows 10/11 SDK

### Build Steps
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Registration
The DLL is a self-registering COM server. From an **elevated** (Administrator) prompt:
```powershell
regsvr32 build\Release\lux_asio.dll
```
To uninstall: `regsvr32 /u build\Release\lux_asio.dll`

Then select **Lux ASIO Driver** in your DAW (Ableton Live, FL Studio, Reaper, Cubase, etc.).

### Verifying
A registration-free test host exercises the full driver lifecycle (init, buffer
protocol, start/stop/restart cycles, channel subsets, callback pacing):
```powershell
build\Release\lux_test_host.exe build\Release\lux_asio.dll
# stability soak (streams continuously, asserts pacing + zero underruns):
build\Release\lux_test_host.exe build\Release\lux_asio.dll --soak 300
```
`lux_probe_periods.exe` prints every device's shared-mode period range and
exclusive-mode floor — the ground truth for achievable latency per device.

### Getting the lowest latency
Open the driver's **Hardware Setup** panel from your DAW and pick a buffer size
that matches a hardware-valid period (the dropdown only lists valid sizes; the
hardware default is starred). A hardware-valid size runs in **aligned mode** —
a direct passthrough with no ring buffer. Sizes that don't match a hardware
period still work, but fall back to the decoupled ring-buffer engine, which
adds a safety prefill and therefore *more* total latency even at smaller block
sizes. Smallest listed size ≠ lowest latency: the lowest-latency choice is the
smallest **hardware-valid** size.

For the absolute minimum, enable **Exclusive mode** in the panel: it bypasses
the shared engine's period floor (many consumer codecs locked at 480 frames
shared accept 144 frames exclusive). Tradeoff: while the DAW has the driver
open, no other application can play audio on that device. If another app holds
the device, starting the stream fails — close it or disable exclusive mode.

## Licensing

This project is licensed under the **GNU General Public License v3** (see
`LICENSE`). The bundled Steinberg ASIO SDK is used under its GPLv3 licensing
option (dual-licensed by Steinberg since ASIO SDK 2.3.4); distributing this
driver or derivatives therefore requires GPLv3 compliance. "ASIO" is a
trademark of Steinberg Media Technologies GmbH.
