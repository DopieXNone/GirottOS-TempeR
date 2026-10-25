# GirottOS-TempeR

A Linux temperature monitor written in C for my future Arch based Linux distribution (GirottOS), showing the temperatures of your computer (processor, graphics card, disks, memory, system) in a simple and understandable way, with a look inspired by Windows 10 File Explorer.
Even if I said before that was specifically for my future distro I want all GirottOS' software available for other systems rather than mine.

> **Beta software**
>
> This project is currently in **beta**. It is functional and actively developed, but bugs, missing features and unexpected behavior may still occur.
>
> TempeR reads sensors and never changes clocks, power limits or the power profile.
> It changes fan speeds only when you turn on its **fan curves**, and gives the
> fans back to the computer when you turn them off. On laptops it can also set
> the **keyboard light**, only when you ask it to.


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
* **Fan curves**, as in ASUS Armoury Crate: 8 points per fan ("at this
  temperature, this speed") dragged on a graph, with Silent / Standard /
  Turbo / Full speed presets, and your own presets made from them. See
  [Fan curves](#fan-curves).
* **Keyboard light** on laptops with a backlit keyboard: colour and
  brightness, without asusd, OpenRGB or other keyboard programs. See
  [Keyboard light](#keyboard-light).
* **Keep awake**, as in Caffeine-ng: the computer (desktop or laptop) does not
  go to sleep and the screen does not turn off or lock by itself. See
  [Keep awake](#keep-awake).
* **Runs in the system tray**: close the window and TempeR (and the overlay)
  keep running in the background. Click the tray icon to bring it back.
* **Starts at login** (on by default, *Settings → Background*): in the
  background, without the window, only the tray icon and the overlay if it
  is on.
* Celsius or Fahrenheit, update every 1, 2, 5 or 10 seconds.
* Light, dark, purple or "follow system" theme, same as ExploreR.


## Supported devices

Everything is read from the kernel (`/sys/class/hwmon`, `/sys/class/thermal`,
`/sys/class/leds`), without root. **✅** works out of the box, **⚠️** works
after enabling something (see [Services and modules to
enable](#services-and-modules-to-enable)), **❌** not possible on that
hardware, **–** the device has nothing of that kind.

| Device | Temperatures | Fan speed (RPM) | Fan curves | Keyboard light | Notes |
|--------|:-----------:|:---------------:|:----------:|:--------------:|-------|
| Intel processors (`coretemp`) | ✅ | – | – | – | package and every core |
| AMD processors (`k10temp`, `zenpower`, old `k8temp`) | ✅ | – | – | – | Tctl / Tdie and every CCD |
| VIA processors (`via_cputemp`) | ✅ | – | – | – | |
| ARM / SoC (`cpu_thermal`, `soc_thermal`: Raspberry Pi, Pinebook…) | ✅ | ✅ if the fan is exposed | ✅ software curve, when the fan driver has `pwm1_enable` | ✅ brightness, if exposed | |
| AMD graphics cards (`amdgpu`, `radeon`) | ✅ edge, hotspot, memory | ✅ | ❌ left to the driver | – | usage and VRAM too |
| NVIDIA, proprietary driver | ✅ through `nvidia-smi` | ❌ | ❌ left to the driver | – | ⚠️ needs `nvidia-utils`; usage and VRAM too |
| NVIDIA, `nouveau` | ✅ | ✅ if exposed | ❌ | – | |
| Intel graphics (`i915`, `xe`) | ✅ Arc cards | ✅ Arc cards | ❌ | – | integrated GPUs have no sensor of their own (see the CPU package); no usage |
| NVMe drives | ✅ | – | – | – | |
| SATA disks and SSDs | ⚠️ `drivetemp` | – | – | – | |
| DDR5 / DDR4 memory with a sensor (`spd5118`, `jc42`) | ⚠️ modules | – | – | – | some boards hide the modules: then nothing can be read |
| Batteries | ✅ when the battery reports it | – | – | – | |
| Wi-Fi cards (`iwlwifi`, `mt76`, `ath`, `rtw`, `brcm`), chipset (PCH), ACPI zones | ✅ | – | – | – | |
| Desktop motherboards (`nct67xx`, `it87`, `f71882fg`, `w83627ehf`…) | ⚠️ chip module | ⚠️ chip module | ⚠️ software curve, follows the CPU | – | `it87` from the AUR (`it87-dkms-git`) |
| ASUS ROG / TUF laptops with custom fan curves (`asus-wmi`, 2020 and later) | ✅ | ✅ | ✅ firmware curves, CPU and GPU fan | ✅ colour (RGB) and brightness | as Armoury Crate |
| Older ASUS laptops | ✅ | ✅ | ✅ software curve, on models whose firmware lets the speed be set by hand (`pwm1_enable`) | ✅ brightness, colour on RGB models | |
| Lenovo ThinkPad (`thinkpad_acpi`) | ✅ | ✅ | ⚠️ `fan_control=1` | ✅ brightness | |
| Dell (`dell_smm_hwmon`) | ✅ | ⚠️ older models: `force=1` | ✅ only on models whose BIOS allows it | ✅ brightness | |
| System76 (`system76_acpi`) | ✅ | ✅ | ❌ | ✅ colour and brightness | |
| Clevo / Tuxedo | ✅ | ✅ if exposed | ❌ | ✅ colour with `tuxedo-drivers` (AUR) | |
| Apple Macs (`applesmc`) | ✅ | ✅ | ❌ | ✅ brightness | |
| Other laptops (HP, Acer, MSI, Lenovo IdeaPad / Legion…) | ✅ | ✅ if exposed | ❌ the firmware keeps the fans | ✅ brightness, if exposed | |

Keep awake works on every computer, see [Platforms](#platforms).

A graphics card that is switched off to save power (laptops) is shown as
*Sleeping* and is never woken up just to be measured.

Notes:

* Graphics cards keep their own fan control: their drivers already follow
  the card's temperature.
* Fan curves on desktop boards and the other chips with `pwmN` follow the
  CPU temperature, so they need a processor sensor (Intel, AMD, VIA or ARM).
  A fan whose driver refuses manual control (a ThinkPad without
  `fan_control=1`) is left to the firmware.
* Game FPS is **not** shown: on Linux a program outside the game can't read
  it without loading code into the game itself, and TempeR never does that.
* `grep . /sys/class/hwmon/*/name` lists the sensor chips the kernel found;
  `sudo temper-fand --list` the fans and keyboard light TempeR can control.


## Old computers

TempeR is meant to run on old machines too (Core 2, Athlon 64, Pentium
Dual-Core, laptops of the Windows Vista / 7 years) with a current Arch Linux:

* **Processor temperature**: Core 2 and later Intel (`coretemp`), Athlon 64 /
  Turion (`k8temp`), Phenom and later AMD (`k10temp`), VIA (`via_cputemp`).
  Pentium 4 / Pentium M and older have no sensor of their own in the kernel:
  the firmware thermal zones (ACPI) are shown instead.
* **Graphics cards**: old Radeon cards through `radeon` (HD 2000 and later
  have a sensor), old GeForce through `nouveau` (the legacy NVIDIA drivers
  are no longer in Arch). Intel GMA chips have no sensor.
* **Disks**: SATA and IDE (PATA) disks through `drivetemp`.
* **Fans**: many old laptops (HP, Acer, Toshiba…) only report fans to the
  firmware: then there is no Fans page and the fans stay with the firmware.
  ThinkPads (`fan_control=1`) and Dells (`force=1`) can usually be
  controlled, see [Services and modules to enable](#services-and-modules-to-enable).
* **Screen**: GTK 4 draws with Vulkan or OpenGL 3; on older graphics chips
  (Intel GMA, Radeon X1000 and older) it falls back to drawing in software:
  it works, a bit slower. To force it from the start:
  `GSK_RENDERER=cairo TempeR`.
* **Old X11 setups** (Openbox, Fluxbox, i3, xscreensaver…): window, overlay,
  tray (with a StatusNotifier tray such as the XFCE panel, tint2 does not
  have one) and Keep awake all work.
* **32-bit computers** run Arch Linux 32: `makepackage.sh` builds the package
  for the machine it runs on (not tested yet).


## Platforms

TempeR works on **Wayland and X11**, on any desktop or compositor:

| Desktop / compositor | Window | Overlay above full-screen | Tray icon | Keep awake | Password prompt (polkit agent) |
|----------------------|:------:|:-------------------------:|:---------:|:----------:|:------------------------------:|
| GNOME | ✅ | ✅ (XWayland) | ⚠️ AppIndicator extension | ✅ | ✅ |
| KDE Plasma | ✅ | ✅ | ✅ | ✅ | ✅ |
| XFCE, Cinnamon, MATE, LXQt | ✅ | ✅ | ✅ | ✅ | ✅ |
| Hyprland | ✅ | ✅ (XWayland) | ✅ with waybar | ✅ with hypridle | ⚠️ `hyprpolkitagent` |
| Sway, river, labwc and other wlroots compositors | ✅ | ✅ (XWayland) | ✅ with waybar | ⚠️ logind only: swayidle still blanks the screen | ⚠️ `polkit-gnome` |
| Wayland without XWayland | ✅ | ⚠️ as a normal small window | depends on the bar | as above | as above |

| System | Status |
|--------|--------|
| Arch Linux and Arch based (EndeavourOS, Manjaro, CachyOS, GirottOS…) | ✅ package with everything (`./makepackage.sh`) |
| Other distributions with GTK ≥ 4.12 and systemd | ✅ TempeR builds with `build.sh` and runs; the service (fan curves, keyboard light) is packaged for Arch only for now |
| Processors | x86_64, i686 (Arch Linux 32) and aarch64 (Arch Linux ARM): `makepackage.sh` builds the package for the machine it runs on. Tested on x86_64 only so far |


## Services and modules to enable

Most of TempeR needs nothing. This is everything that can be enabled, and
when it is needed (commands for Arch Linux):

| What | For | Needed on | How |
|------|-----|-----------|-----|
| **temper-fand** (TempeR's service) | fan curves, keyboard light | every computer that uses them | install the package, then turn the curves on in TempeR, or `sudo systemctl enable --now temper-fand.service`. See [Enabling the service](#enabling-the-service-arch-linux) |
| polkit | lets you change curves and light without a password | every computer | nothing: a dependency of the package, started by D-Bus |
| polkit agent | asks the password (remote / inactive sessions) | Hyprland, Sway and other bare compositors | `sudo pacman -S hyprpolkitagent` (Hyprland) or `polkit-gnome`, and start it at login (`/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1`) |
| `drivetemp` module | SATA disk and SSD temperatures | computers with SATA drives | `sudo modprobe drivetemp`, and at every boot: `echo drivetemp \| sudo tee /etc/modules-load.d/drivetemp.conf` |
| motherboard chip module | desktop temperatures, fans, fan curves | desktop PCs whose chip is not loaded by itself | `sudo pacman -S lm_sensors`, `sudo sensors-detect` (answer the defaults), then `sudo systemctl enable --now lm_sensors.service`. ITE chips: `it87-dkms-git` from the AUR |
| `thinkpad_acpi fan_control=1` | fan curves on ThinkPads | Lenovo ThinkPad | `echo "options thinkpad_acpi fan_control=1" \| sudo tee /etc/modprobe.d/thinkpad_acpi.conf`, then restart |
| `dell_smm_hwmon force=1` | fans (and fan curves) on Dell models the driver does not know | older Dell laptops and desktops (no "Fans" page in TempeR) | `echo "options dell_smm_hwmon force=1" \| sudo tee /etc/modprobe.d/dell-smm-hwmon.conf`, then `sudo modprobe -r dell_smm_hwmon; sudo modprobe dell_smm_hwmon`. If the module still does not load, add `ignore_dmi=1` too. Only on Dell computers |
| memory sensor modules | RAM temperatures | PCs with DDR5 / DDR4 modules that have a sensor | `sudo modprobe i2c-i801` (Intel) or `i2c-piix4` (AMD), then `sudo modprobe spd5118` (DDR5) or `jc42` (DDR4); list them in `/etc/modules-load.d/memory.conf` to keep them |
| `nvidia-utils` | NVIDIA temperature, usage and VRAM | NVIDIA cards with the proprietary driver | `sudo pacman -S nvidia-utils` (no service to enable) |
| AppIndicator extension | tray icon | GNOME | `sudo pacman -S gnome-shell-extension-appindicator`, then `gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com` and log in again |
| hypridle | Keep awake also stops the screen blanking | Hyprland | nothing to change, unless its config has `ignore_dbus_inhibit = true` |
| TempeR at login | tray, overlay, keep awake and curves back at login | everybody | on by default, *Settings → Background* |

Not needed: **asusd / ROG Control Center** (TempeR does fan curves and
keyboard light without them; if they are installed they can stay, see
[TempeR comes before ROG Control Center](#temper-comes-before-rog-control-center)),
**lm_sensors' `fancontrol.service`**, **CoolerControl** and other fan programs
(paused by TempeR while its curves are on: better not to enable them), and
**Caffeine-ng** (Keep awake does the same).


## Fan curves

*Fan curves* in the command bar (also on the Fans page, in the tray menu and
in the Settings) opens the editor. Drag the 8 points of each fan, or start
from a preset, then *Apply*.

TempeR's presets (Silent, Standard, Turbo, Full speed) can't be changed. To
have your own, use *My presets → New preset…*: give it a name and choose the
TempeR preset it starts from. Your preset is put on the graph: drag the points,
then *My presets → Save the graph to "…"*. From the same menu you load it again
or delete it (✕). Your presets are saved in `presets.ini`, next to
`settings.ini` (see [Configuration](#configuration)).

The curves are applied by **temper-fand**, a small service that comes with
the package and runs as root (TempeR itself never writes to the hardware).
It is started by D-Bus the first time TempeR asks for it, and at every boot
while the curves are on. Changing the curves is allowed to the user sitting
at the computer (polkit, like power profiles); anybody else needs the
administrator password.

| Hardware                        | How                                                |
|---------------------------------|----------------------------------------------------|
| ASUS laptops (`asus-wmi`)       | the firmware's own 8 point curves (`asus_custom_fan_curve`): the CPU fan follows the CPU, the GPU fan the GPU, exactly like Armoury Crate |
| Desktop boards, ThinkPads… (`pwmN` in hwmon) | the same curve, followed by temper-fand every second against the CPU temperature |

While the curves are on, nothing else drives the fans and nothing fights
with the system:

* a power profile change (power-profiles-daemon, asusd, Fn+F5) or a resume
  makes the firmware drop custom curves: temper-fand notices within a second
  and applies them again. The power profile itself is never changed;
* **asusd** keeps running (keyboard light, battery limit, profiles…), only its
  own fan curves are switched off, and switched off again if ROG Control
  Center or `asusctl` turns them back on;
* fan control daemons are paused: **CoolerControl**, `fancontrol`, fan2go,
  NoteBook FanControl, thinkfan, i8kmon, mbpfan, amdgpu-fan (stopped and
  masked until the next boot, and again at every boot);
* for safety, from 85 °C up the fans always turn at least at 50 %, and the
  processor and graphics card keep their own thermal protection.

Turning the curves off (or uninstalling the package) gives everything back:
the firmware's automatic fan control, asusd's curves as they were, the
paused programs started again.

`sudo temper-fand --list` shows the fans it found, the curves and the keyboard
light. The curves are saved in `/etc/temper/fan-control.ini`.

### TempeR comes before ROG Control Center

asusd and ROG Control Center apply their own fan curves when they start
(asusd at boot, ROG Control Center at login). While TempeR's curves are on,
TempeR's are the ones that stay:

* temper-fand starts at boot, right after asusd, and puts TempeR's curves on
  the fans. If the service was disabled (by hand, or by an old package) it
  enables itself again the next time it runs;
* TempeR, when it starts at login, wakes the service too: the curves are back
  even if the service did not start at boot;
* asusd's settings are checked every second: when ROG Control Center (or
  `asusctl`) turns asusd's curves back on, they are switched off again and
  TempeR's are written again within a second.

ROG Control Center can stay installed and in autostart: its other settings
(keyboard, battery limit, profiles…) keep working.

### Enabling the service (Arch Linux)

The service only works from the package, because it has to be installed
system-wide (systemd unit, D-Bus and polkit files): the `./temper-fand` built
by `build.sh` alone can't be used.

1. Build and install the package:

   ```bash
   cd current
   ./makepackage.sh
   sudo pacman -U ./girottos-temper-0.2-1-x86_64.pkg.tar.zst
   ```

2. Open TempeR → *Fan curves* and turn on **Use TempeR's fan curves** (or
   press *Apply and turn on*). This starts the service and enables it at
   every boot: nothing else to do.

   From a terminal, if the curves are already on but the service does not
   start at boot:

   ```bash
   sudo systemctl enable --now temper-fand.service
   ```

   With the curves off there is no need: the service stops by itself after
   2 minutes without requests (this is normal, D-Bus starts it again when
   TempeR asks).

3. Check that it is running and starts at boot:

   ```bash
   systemctl is-enabled temper-fand    # enabled
   systemctl status temper-fand        # active (running)
   sudo temper-fand --list             # fans, curves and keyboard light found
   journalctl -b -u temper-fand        # what it did since boot
   ```

To stop using it, turn the curves off in TempeR: the fans, asusd's curves and
the paused programs are given back, and the service is disabled. Stopping it
by hand (`sudo systemctl disable --now temper-fand`) while the curves are on
gives the fans back to the firmware, but TempeR turns it on again at the next
login; `sudo temper-fand --release` switches the curves off for good, as the
window does.

Changing curves and keyboard light is allowed to the user sitting at the
computer without a password. Remote (SSH) or inactive sessions are asked for
the administrator password through the desktop's polkit agent (GNOME, KDE,
XFCE… have one; on Hyprland, Sway and other compositors install one, e.g.
`sudo pacman -S polkit-gnome` or `hyprpolkitagent`, and start it at login).


## Keyboard light

On laptops with a backlit keyboard, *Keyboard light* in the command bar (also
in the Settings) changes its brightness and, when the keyboard has colours,
its colour. TempeR shows it only when it finds a laptop (the firmware's
chassis type, or a battery) whose keyboard light is known to the kernel
(`/sys/class/leds/…::kbd_backlight`): no keyboard driver or program is
needed, nor asusd, OpenRGB or vendor tools.

| Keyboard                                    | What can be changed             |
|---------------------------------------------|---------------------------------|
| ASUS ROG / TUF with RGB (`asus-wmi`, `kbd_rgb_mode`) | colour (static) and brightness, kept by the keyboard after a restart |
| System76 (`system76_acpi`)                  | colour and brightness           |
| Clevo, Tuxedo and other multicolour keyboards (`multi_intensity`) | colour and brightness |
| any other backlit keyboard                  | brightness                      |

As for the fan curves, the light is set by the temper-fand service (so the
package is needed, see [Enabling the service](#enabling-the-service-arch-linux)).
It is saved in `/etc/temper/keyboard.ini` and put back every time the service
starts. Keyboards with one colour per zone, or effects (breathing, rainbow…),
are set to one static colour. If asusd is running it may put its own light
back when it starts or after a resume: choose the light again in TempeR, or
set it with ROG Control Center.


## Keep awake

*Keep awake* in the command bar (also in the tray menu and in *Settings →
Power*) stops the automatic sleep: while it is on the computer does not
suspend by itself and the screen does not turn off or lock when you don't
use it, on desktops and laptops alike. The button is highlighted while it is
on. It stays on until you turn it off, also after TempeR is restarted.

Suspending by hand still works (power menu, power button, closing the
laptop's lid). Nothing needs root or another package; TempeR asks every
desktop the way it listens:

| Request                         | Who honours it                                   |
|---------------------------------|--------------------------------------------------|
| session inhibit (GTK / portal)  | GNOME, KDE Plasma and desktops with xdg-desktop-portal |
| `org.freedesktop.ScreenSaver`   | KDE, XFCE, LXQt, Cinnamon, MATE, hypridle        |
| systemd-logind `idle` inhibitor | logind's `IdleAction` and programs that follow it |
| X11 screen saver reset          | X11 sessions (screen saver and DPMS), xss-lock   |
| `xscreensaver-command -deactivate` | xscreensaver (Openbox, i3, Fluxbox and other X11 setups) |

`systemd-inhibit --list` shows TempeR while it is on. Turning it off, or
quitting TempeR, gives everything back. Use it instead of Caffeine-ng, not
together with it: either one is enough.

Note: swayidle (Sway, and other compositors that use it) only listens to
windows that are on screen, so there *Keep awake* stops logind's automatic
sleep but not the screen blanking set up in swayidle.


## Disclaimer

See [Platforms](#platforms) for the desktops and systems TempeR runs on.

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
* The output of `sudo temper-fand --list` and `journalctl -b -u temper-fand`
  for fan curves or keyboard light problems
* Screenshots, (I beg you)


## Build

*HIGHLY RECOMMENDED*

```bash
cd current
chmod +x build.sh
./build.sh

./TempeR
```

`build.sh` builds `TempeR` and the service `temper-fand` (fan curves and
keyboard light). These need the package (`./makepackage.sh`, then
`sudo pacman -U` the result), because the service has to be installed
system-wide: see [Enabling the service](#enabling-the-service-arch-linux). It checks for GTK 4 (>= 4.12) and the Tela icon theme (installed
through `yay`/`paru` if missing), enables the overlay above full-screen windows
when GTK has X11 support (always on Arch, `libx11` is already a GTK
dependency) and tells you if you have an NVIDIA card but no `nvidia-smi`.

Build dependencies on Arch:

```bash
sudo pacman -S --needed base-devel pkgconf gtk4
```


## Package (Arch)

```bash
cd current
./makepackage.sh
sudo pacman -U ./girottos-temper-0.2-1-x86_64.pkg.tar.zst
```

Start it with `temper` (or `TempeR`). To use the fan curves and the keyboard
light, see [Enabling the service](#enabling-the-service-arch-linux). Uninstall it with
`sudo pacman -R girottos-temper`: the fans go back to the computer's own
control first.


## Usage

| Shortcut          | Action                                        |
|-------------------|-----------------------------------------------|
| F5                | Refresh: look for sensors again               |
| Ctrl+,            | Settings                                      |
| Ctrl+Shift+Alt+Space+Enter | Specs window                         |
| Ctrl+W            | Close the window (stays in the tray)          |
| Ctrl+Q            | Quit TempeR                                   |

The overlay is turned on and off with the **Overlay** button in the command
bar, from the tray menu, or in the Settings.


## Configuration

Settings (theme, icons, unit, update interval, window size, tray, start at
login, keep awake, overlay)
are saved in `settings.ini` next to the `TempeR` executable. If that folder is
not writable (for example when installed in `/usr/bin`),
`~/.config/temper/settings.ini` is used instead. The file can be edited by hand
or through the Settings window (Ctrl+,). Your own fan curve presets are in
`presets.ini`, in the same folder.

The fan curves (`/etc/temper/fan-control.ini`) and the keyboard light
(`/etc/temper/keyboard.ini`) belong to the whole computer and are written by
the temper-fand service.


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
