# GirottOS-TempeR

A Linux temperature monitor written in C for my future Arch based Linux distribution (GirottOS), showing the temperatures of your computer (processor, graphics card, disks, memory, system) in a simple and understandable way, with a look inspired by Windows 10 File Explorer.
Even if I said before that was specifically for my future distro I want all GirottOS' software available for other systems rather than mine.

> **Beta software**
>
> This project is currently in **beta**. It is functional and actively developed, but bugs, missing features and unexpected behavior may still occur.
>
> TempeR only reads sensors: it never changes fan speeds, clocks or any other hardware setting.


## Features

* **Overview** of every component with a coloured bar and a plain status:
  *Normal*, *Warm*, *Hot* or *Critical*, plus one sentence that sums it all up
  ("Everything is running at normal temperatures…").
* **Detail pages** for processor, graphics cards, storage, memory, battery,
  system and fans: every sensor with its current and highest temperature.
  Hover a row to see where *Hot* and *Critical* start for that sensor.
* **On-screen overlay** with temperatures and usage (CPU, GPU, VRAM, disk,
  RAM, swap, fans, battery, clock), drawn above games and full-screen apps.
  Position, screen, text size, opacity and every line can be changed in the
  Settings.
* **Runs in the system tray**: close the window and TempeR (and the overlay)
  keep running in the background. Click the tray icon to bring it back.
* Celsius or Fahrenheit, update every 1, 2, 5 or 10 seconds.
* Light, dark, purple or "follow system" theme, same as ExploreR.


## Supported hardware

Everything is read from the kernel, without root and without extra packages:

| Component      | Source                                                        |
|----------------|---------------------------------------------------------------|
| Processor      | `coretemp` (Intel), `k10temp` / `zenpower` (AMD), ARM thermal |
| Graphics card  | `amdgpu`, `radeon`, `nouveau`, `i915`, `xe`; NVIDIA proprietary driver through `nvidia-smi` (from `nvidia-utils`) |
| Storage        | NVMe (`nvme`), SATA (`drivetemp` module)                      |
| Memory         | DDR5 / DDR4 modules with a sensor (`spd5118`, `jc42`)         |
| System         | motherboard and laptop chips, ACPI / firmware thermal zones   |
| Fans           | every fan the kernel exposes                                  |

A graphics card that is switched off to save power (laptops) is shown as
*Sleeping* and is never woken up just to be measured.

Notes:

* GPU usage is available for AMD and NVIDIA; Intel GPUs don't expose it.
* SATA disk temperatures need the `drivetemp` kernel module
  (`sudo modprobe drivetemp`).
* Game FPS is **not** shown: on Linux a program outside the game can't read
  it without loading code into the game itself, and TempeR never does that.


## Disclaimer

Works on **Wayland and X11** (GNOME, KDE Plasma, Hyprland, Sway, XFCE…), on
Arch Linux and Arch based distributions.

* The overlay stays above full-screen windows through XWayland (or X11). On a
  Wayland session without XWayland it is shown as a normal small window.
* The tray icon needs a tray that supports StatusNotifierItem: KDE, XFCE,
  Cinnamon, LXQt, waybar… On GNOME install the *AppIndicator and
  KStatusNotifierItem Support* extension (enabled by default on many
  distributions). Without a tray, closing the window quits TempeR.

This software is provided during its beta stage and may contain bugs. Sensor
readings come from your hardware and its drivers: if a value looks wrong, it
may be reported wrong by the chip itself.

The developer is not responsible for damage or other issues resulting from the use of this software.


## Bug Reports

Because this is a beta release, bug reports are especially useful.

When opening an issue, please include:

* Linux distribution
* Architecture
* Desktop environment / compositor, Wayland or X11
* TempeR version/commit
* Processor and graphics card model
* Steps to reproduce the problem
* Expected behavior
* Actual behavior
* Relevant terminal output or logs
* The output of `grep . /sys/class/hwmon/*/name`
  if a sensor is missing or wrong
* Screenshots, (I beg you)


## Build

*HIGHLY RECOMMENDED*

```bash
cd current
chmod +x build.sh
./build.sh

./TempeR
```

`build.sh` checks for GTK 4 (>= 4.12) and the Tela icon theme (installed
through `yay`/`paru` if missing), enables the overlay above full-screen windows
when GTK has X11 support (always on Arch, `libx11` is already a GTK
dependency) and tells you if you have an NVIDIA card but no `nvidia-smi`.

Build dependencies on Arch:

```bash
sudo pacman -S --needed base-devel pkgconf gtk4
```


## Usage

| Shortcut          | Action                                        |
|-------------------|-----------------------------------------------|
| F5                | Refresh: look for sensors again               |
| Ctrl+,            | Settings                                      |
| Ctrl+W            | Close the window (stays in the tray)          |
| Ctrl+Q            | Quit TempeR                                   |

The overlay is turned on and off with the **Overlay** button in the command
bar, from the tray menu, or in the Settings.


## Configuration

Settings (theme, icons, unit, update interval, window size, tray, overlay)
are saved in `settings.ini` next to the `TempeR` executable. If that folder is
not writable (for example when installed in `/usr/bin`),
`~/.config/temper/settings.ini` is used instead. The file can be edited by hand
or through the Settings window (Ctrl+,).


## Credits

* **[Tela icon theme](https://github.com/vinceliuice/Tela-icon-theme)** by
  [vinceliuice](https://github.com/vinceliuice), licensed under the GPL-3.0.
  TempeR uses it for its component, tray and status icons (only inside
  TempeR, your desktop icon theme is not changed). `build.sh` installs it with
  `yay` or `paru` (`tela-icon-theme` on the AUR) when it is missing. Without it,
  or with *Settings → Icons → Emoji*, emoji icons are shown instead.
* GPU names come from the **pci.ids** database (`hwdata` package), when it is
  installed.


## License

This project is licensed under the **Apache License 2.0**.

You may use, modify, distribute and contribute to the project according to the terms of the license.

See the [`LICENSE`](LICENSE) file for the complete license text.
