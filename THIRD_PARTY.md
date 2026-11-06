# Monitor Brightness add-on: sources and licenses

The add-on as a whole (this repository and its release zip) is distributed under the
GNU General Public License, version 3 only (`LICENSE`). It combines the parts below.

## The mod (GPL-3.0-only)

`mod/brightness.wh.cpp` (built into `MonitorBrightness.dll`) and `tools/finish-bundle.py`
were written for this add-on.

The code that finds the taskbar's XAML follows PillTaskbar
(https://github.com/hyeon1035298/PillTaskbar_BloomAddon, GPL-3.0-only), which in turn
follows the taskbar mods of the windhawk-mods collection
(https://github.com/ramensoftware/windhawk-mods).

Monitors are driven through documented Windows interfaces: the Monitor Configuration
API in `dxva2.dll` (DDC/CI VCP codes 0x10 brightness, 0x12 contrast and 0xD6 power mode,
as the VESA MCCS standard numbers them) and the WMI classes `WmiMonitorBrightness` and
`WmiMonitorBrightnessMethods` for a built-in panel.

What the add-on does was chosen after looking at how Twinkle Tray
(https://github.com/xanderfrangos/twinkle-tray, MIT) behaves. No code, text, icon or name
of Twinkle Tray is used.

## The add-on runtime and host (GPL-3.0-only)

`MonitorBrightnessRuntime.dll`, `MonitorBrightnessHost.exe` and the add-on entry code built
into `MonitorBrightness.dll` come from PillTaskbar
(https://github.com/hyeon1035298/PillTaskbar_BloomAddon), GPL-3.0-only, built with this
add-on's names from `addon.conf`. A release zip's `SOURCE.txt` names the commit of this
repository and of PillTaskbar_BloomAddon it was built from.

The runtime implements the part of the Windhawk mod API that the mod uses. It was
written for PillTaskbar and follows the behavior of Windhawk
(https://github.com/ramensoftware/windhawk, GPL-3.0).

The runtime and host contain or link these libraries. Their license texts are in
`THIRD_PARTY_NOTICES.txt` next to the binaries.

| Library | License | Used for |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (with HDE64) | BSD-2-Clause | function hooks |
| [Zydis](https://github.com/zyantific/zydis) / Zycore 4.1.0 amalgamation | MIT | `Wh_Disasm` |
| mingw-w64 runtime | see `COPYING.MinGW-w64-runtime.txt` | C runtime startup code |
| libc++, libunwind, compiler-rt (LLVM 20) | Apache-2.0 WITH LLVM-exception | the statically linked C++ runtime |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | MIT | headers the mod includes |

## Microsoft symbols

The host downloads Microsoft's public symbol file for `taskbar.dll` from the
Microsoft symbol server on first start and keeps only the addresses the mod needs.
Nothing from Microsoft is included in this repository or in the release zip.

## Fonts and icons

The add-on asks Windows for Segoe UI and Malgun Gothic, and for the glyphs of Segoe
Fluent Icons (Segoe MDL2 Assets on older systems). No font is included. The taskbar
icon (a monitor with the brightness in it) is drawn in code.
