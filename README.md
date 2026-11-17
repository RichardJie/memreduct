<h1 align="center">Mem Reduct</h1>

<p align="center">
	<a href="https://github.com/henrypp/memreduct/releases"><img src="https://img.shields.io/github/v/release/henrypp/memreduct?style=flat-square&include_prereleases&label=version" /></a>
	<a href="https://github.com/henrypp/memreduct/releases"><img src="https://img.shields.io/github/downloads/henrypp/memreduct/total.svg?style=flat-square" /></a>
	<a href="https://github.com/henrypp/memreduct/issues"><img src="https://img.shields.io/github/issues-raw/henrypp/memreduct.svg?style=flat-square&label=issues" /></a>
	<a href="https://github.com/henrypp/memreduct/graphs/contributors"><img src="https://img.shields.io/github/contributors/henrypp/memreduct?style=flat-square" /></a>
	<a href="https://github.com/henrypp/memreduct/blob/master/LICENSE"><img src="https://img.shields.io/github/license/henrypp/memreduct?style=flat-square" /></a>
</p>

-------

<p align="center">
	<img src="/images/memreduct.png?cachefix" />
</p>

### Description:
Lightweight real-time memory management application to monitor
and clean system memory on your computer.

The program used undocumented internal system features (Native API) to clear system cache (system working set, working set, standby page lists, modified page lists) with variable result ~10-50%. Application it is compatible with <s>Windows XP SP3</s> Windows 7 SP1 and higher operating systems.

You can download either the installer or portable version. For correct working you are require administrator rights.

```
To activate portable mode, create "memreduct.ini" in application folder, or move it from "%APPDATA%\Henry++\Mem Reduct".
```

### Lag Reduct Submodule (`src/lagreduct.c` / `src/lagreduct.h`):
This fork extends **Mem Reduct** with a pure C Windows **Native API** (`ntdll.dll` / `dwmapi.dll`) **System Lag Guard & Remediation Submodule** designed to resolve long-uptime Windows UI stutter even when physical RAM usage is low:

1. **Three Lag-Source Remediation Engines** (`NtQuerySystemInformation`, `NtOpenProcess`, `NtTerminateProcess`, `NtWaitForSingleObject`, `NtDelayExecution`, `NtSetSystemInformation`):
   - **Handle-Leak & Runaway Kernel-CPU Background Reaper**: Detects non-interactive user-session background helpers leaking tens of thousands of handles or pegging kernel-mode CPU, while strictly protecting Session 0 system services, security cores, productivity apps, and any process owning a visible user window.
   - **Safe `dwm.exe` Handle-Bloat Reset**: Monitors Desktop Window Manager (`dwm.exe`) handle count and resets it **only** when handles exceed the configured bloat threshold (default `50,000` handles), reclaiming tens of thousands of leaked composition handles without closing open user applications.
   - **Orphaned / Runaway Headless Sandbox Reaper & Kernel Pool Relief**: Cleans orphaned headless WebView2/sandbox worker processes spinning CPU in the background and flushes kernel registry/standby caches under elevated system-wide handle pressure.
2. **Threshold Auto-Clean & Manual Clean Controls**:
   - Real-time **System lag guard** group in the main window (`System handles`, `DWM handles`, `Kernel pool NP / P`).
   - Dedicated **Lag Reduct (防卡顿清理)** tab in Settings with configurable thresholds (`System handles limit`, `Process / DWM handle limit`), automatic threshold trigger, main-button linkage, split-button dropdown actions, tray menu item, and CLI flag `-clean:lag`.
3. **Built-in Anti-Bug Safeguards**:
   - **Threshold & Cooldown Gates**: Never restarts `dwm.exe` when healthy (`< limit`), enforces a 10-minute cooldown to prevent `winlogon.exe` session logoff, and skips fullscreen 3D/game sessions (`SHQueryUserNotificationState`).
   - **Automatic Win11 Shell XAML Resync**: After a `dwm.exe` reset, an async Native worker thread waits for the new `dwm.exe` instance to initialize and automatically refreshes stale DirectComposition hosts (`SearchHost.exe`, `StartMenuExperienceHost.exe`, `ShellExperienceHost.exe`, `TextInputHost.exe`) so Windows Search, Start Menu, and Clipboard flyouts never turn blank.
   - **DWM Backdrop & Frame Restoration**: Snapshots top-level window `DWMWA_SYSTEMBACKDROP_TYPE`, `DWMWA_USE_IMMERSIVE_DARK_MODE`, and `DWMWA_WINDOW_CORNER_PREFERENCE` attributes prior to reset, then restores attributes and broadcasts non-blocking `WM_DWMCOMPOSITIONCHANGED` / `WM_THEMECHANGED` / `SWP_FRAMECHANGED` frame redraws to preserve Mica/Acrylic window borders.

### System requirements:
- Windows 7, 8, 8.1, 10, 11 (64-bit/ARM64)
- An SSE2-capable CPU
- <s>KB2533623</s> [KB3063858](https://www.microsoft.com/en-us/download/details.aspx?id=47442) update for Windows 7 was required

### Donate:
- [Bitcoin](https://www.blockchain.com/btc/address/1LrRTXPsvHcQWCNZotA9RcwjsGcRghG96c) (BTC)
- [Ethereum](https://www.blockchain.com/explorer/addresses/eth/0xe2C84A62eb2a4EF154b19bec0c1c106734B95960) (ETH)
- [Yandex Money](https://yoomoney.ru/to/4100115776040583) (RUB)
- [Paypal](https://paypal.me/henrypp) (USD)

### GPG Signature:
Binaries have GPG signature `memreduct.exe.sig` in application folder.

- Public key: [pubkey.asc](https://raw.githubusercontent.com/henrypp/builder/master/pubkey.asc) ([pgpkeys.eu](https://pgpkeys.eu/pks/lookup?op=index&fingerprint=on&search=0x5635B5FD))
- Key ID: 0x5635B5FD
- Fingerprint: D985 2361 1524 AB29 BE73 30AC 2881 20A7 5635 B5FD
---
- Website: [github.com/henrypp](https://github.com/henrypp)
- Support: sforce5@mail.ru
---
(c) 2011-2026 Henry++
