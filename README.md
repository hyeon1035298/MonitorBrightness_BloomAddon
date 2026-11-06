# Monitor Brightness (Bloom add-on)

[한국어](README.ko.md)

An add-on for the [Bloom](https://github.com/hyeon1035298/Bloom) launcher that puts monitor brightness on the Windows 11 taskbar. A small monitor next to the clock shows the brightness; the wheel over it changes every monitor at once, and a click opens a window with a slider for each monitor.

- A slider per monitor, external monitors over DDC/CI and a built-in laptop panel through WMI
- The wheel over the taskbar icon moves every monitor by the same step
- A range per monitor: pick the real brightness the window's 0 and 100 stand for, so monitors of different brightness move together
- "All together": moving one slider moves the others to the same value
- Hotkeys (Ctrl+Alt+↑ / ↓ to start with), changeable in the settings
- Dim when idle: after a set time without input the monitors go to a set brightness and come back on the next input. Nothing dims while a program keeps the screen on, such as a video player
- DDC/CI contrast and turning one monitor off, and a button that turns every screen off until the mouse moves

It works on the stock Windows 11 taskbar and on [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon) alike, and does not need PillTaskbar. When the Memory Clean add-on is on too, the two sit side by side as brightness, RAM, clock, and only one of their windows is open at a time.

## How it works

The add-on runtime and host from PillTaskbar load the mod in `mod/` into Explorer: the host finds the taskbar function the mod uses to reach the taskbar's XAML, injects a small runtime that provides the Windhawk API the mod uses, and keeps it running. The host itself is a separate small process that Bloom starts.

Inside Explorer the mod adds the icon to the tray, opens its window on demand, and runs one worker thread for the monitors. Every DDC/CI or WMI call happens on that thread, and while a slider is dragged only the newest value per monitor is written, so a slow monitor never holds up the taskbar. The monitor list is read when the window opens and when displays change.

## Requirements

- Windows 11, 64-bit
- Bloom 0.19.0 or later
- For external monitors, DDC/CI turned on in the monitor's own menu (most monitors have it on by default)
- An internet connection on the first start (see below)

## Install

1. Download `MonitorBrightness_BloomAddon-<version>.zip` from [Releases](https://github.com/hyeon1035298/MonitorBrightness_BloomAddon/releases).
2. Unzip it into `%LOCALAPPDATA%\Bloom\user-addons\brightness\`. The folder name must be `brightness`.
3. Restart Bloom, open Settings › Add-ons and turn on "Monitor Brightness".

Bloom checks every file against the SHA-256 hashes in `addon.json` before it starts the add-on. To remove the add-on, turn it off and delete the folder.

### First start

The mod reaches the taskbar's XAML through internal functions of `taskbar.dll`, so the host needs that DLL's symbols. On the first start, and again after a Windows update changes the DLL, it downloads the matching public symbol file from Microsoft's symbol server and keeps only the offsets. Nothing from Microsoft ships with the add-on.

## Using it

- **Wheel over the icon**: every shown monitor up or down by one step (5 % to start with).
- **Click the icon**: the window. Drag or click a track, use the wheel over a row (1 % a notch), or click the number to type one. The arrow at the end of a row shows contrast and "Turn this monitor off" for that monitor; such a monitor comes back with its own power button.
- **Top of the window**: "All together", turn every screen off (any mouse move or key brings them back), and the settings.
- **Settings**: the taskbar number (first monitor or average), the wheel step, each monitor's range and whether it shows in the window, the hotkeys (click a field, then press the new combination; Backspace clears it, Escape keeps the old one), the hotkey step, and dimming when idle.

A monitor whose DDC/CI is off is named in the window with a "Look again" button. If a hotkey is taken by another program, its field says so.

The light or dark look follows Bloom's theme, and the window's glass follows Bloom's glass effect. Everything else is saved by the mod in `%LOCALAPPDATA%\Bloom\addons\brightness\prefs.json`.

## Build from source

You need a clang that targets `x86_64-w64-mingw32` (llvm-mingw, or the compiler that ships with Windhawk), Python 3, and a bash shell such as Git Bash. The build uses PillTaskbar's add-on tools, so clone both repositories side by side:

```
git clone https://github.com/hyeon1035298/MonitorBrightness_BloomAddon
git clone https://github.com/hyeon1035298/PillTaskbar_BloomAddon
cd MonitorBrightness_BloomAddon
export CXX=/path/to/llvm-mingw/bin/clang++.exe   # optional, defaults to Windhawk's compiler
bash build.sh
```

`PILL_ROOT` points `build.sh` at a PillTaskbar_BloomAddon checkout elsewhere. llvm-mingw has no C++/WinRT headers; set `CPPWINRT_INCLUDE` to a folder that the Windows SDK's `cppwinrt.exe` wrote, as the workflow does. The result lands in `build/addon`.

The workflow in `.github/workflows/build.yml` builds every push against the PillTaskbar_BloomAddon commit pinned in `PILL_REF`. A `v*` tag publishes a release with the zip, its SHA-256, and a `SOURCE.txt` that names the commits of both repositories.

## License

The add-on is free software under the GNU General Public License, version 3 only. See `LICENSE`.

The runtime and host bundle MinHook (BSD-2-Clause) and Zydis (MIT). Credits and license texts are in `THIRD_PARTY.md` and, in the release zip, `THIRD_PARTY_NOTICES.txt`.

This program comes with no warranty. It runs code inside explorer.exe and sends commands to your monitors, so use it at your own risk. Windows is a trademark of Microsoft. This project is not affiliated with Microsoft or with the authors of other brightness tools.
