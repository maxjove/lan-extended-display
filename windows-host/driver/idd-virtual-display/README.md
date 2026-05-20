# LED IddCx Virtual Display Driver

This module implements the Windows virtual display part of the LAN extended-display plan.
It follows the same Windows mechanism observed in ToDesk: a UMDF driver using `IndirectKmd`
and `IddCx0102`.

## Current State

- Builds `led_idd.dll` as a UMDF v2 driver.
- Generates `led_idd.inf` with `UpperFilters=IndirectKmd`.
- Creates and signs `led_idd.cat` with a local test certificate.
- Stages successfully with `pnputil`.
- Installs a root display device as `Root\LedIdd`.
- Local validation after reboot: device status is `Started`, Windows enumerates a
  real `1920x1080` extended display, and `led_host_app --list-displays` sees it
  as an active non-primary monitor.
- The host capture path now selects that active monitor and starts DXGI Desktop
  Duplication against its desktop coordinates.

## Build And Sign

Run from an elevated PowerShell:

```powershell
.\windows-host\driver\idd-virtual-display\scripts\build-sign-package.ps1
```

Output package:

```text
build-idd\driver\idd-virtual-display\
  led_idd.dll
  led_idd.inf
  led_idd.cat
```

## Install

Run from an elevated PowerShell:

```powershell
.\windows-host\driver\idd-virtual-display\scripts\install-driver.ps1
```

Check status:

```powershell
pnputil /enum-devices /instanceid ROOT\DISPLAY\0002 /properties
Get-PnpDevice -Class Display
```

If Windows reports `CM_PROB_NEED_RESTART`, reboot once and check again.

Expected healthy signs:

```text
Status: Started
ProblemCode: 0
Device stack: IndirectKmd, WUDFRd, PnpManager
```

Driver bring-up log:

```powershell
Get-Content C:\ProgramData\lan-extended-display\led_idd.log
```

## Uninstall

Run from an elevated PowerShell:

```powershell
.\windows-host\driver\idd-virtual-display\scripts\uninstall-driver.ps1
```

## Current Video Path

The current implementation keeps the IddCx swap-chain alive and lets the existing
host process capture the virtual display through DXGI Desktop Duplication:

```text
Windows desktop on LED virtual monitor
  -> DXGI Desktop Duplication for that monitor
  -> Media Foundation H.264 encoder
  -> existing RTP/UDP sender
  -> Linux ARM64 client decoder/display
```

The in-driver swap-chain pump still only acquires frames and marks them processed.
That is enough for Windows to expose the monitor, while user-mode host capture
handles encoding and network transport.
