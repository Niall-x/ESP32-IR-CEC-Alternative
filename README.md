# ESP32 IR Remote

A USB stick that makes your TV follow your PC's power state. The PC sleeps, the
TV turns off. The PC wakes, the TV turns on.

It is a replacement for HDMI-CEC on machines where CEC does not work — which is
most PCs. If you use a TV as a monitor and are tired of reaching for the remote
every time the machine wakes, this fixes that.

```
  PC                                  ESP32-S3 stick               TV
  ──                                  ─────────────                ──
  daemon  ──── USB HID ────────────►  firmware  ──── IR LED ────►
  (background service)                (Arduino)      (infrared)
```

A small background service on the PC watches for power events and sends `ON` or
`OFF` over USB. The stick blinks the matching infrared code at the TV, exactly
as the TV's own remote would. The stick appears as a standard USB HID device, so
there is no driver to install on any OS.

**The rule it follows:** the TV follows your PC's power. On Windows and Linux
alike, the TV turns off when the PC sleeps or shuts down, and on when it wakes
or boots.

Windows goes one step further and follows the screen itself, so an idle screen
blank turns the TV off and touching the mouse turns it back on. Linux has no
system-wide signal for that, so there an idle blank leaves the TV on. If you
want the TV off when a Linux machine idles, set the desktop to sleep when idle
rather than only blank the screen. And leave the TV's own "no signal" auto
power-off disabled: on Linux nothing would turn the TV back on afterwards.

---

## Status

Working and in daily use, but not yet released — read this before relying on it.

| | |
|---|---|
| **Linux** | Well tested. Sleep, wake, shutdown and boot all verified on hardware |
| **Windows** | Builds and runs as a service, tested less. Some paths unverified |
| **TV profiles** | LG is verified on real hardware. Samsung, Sony and Toshiba are derived from published code databases but **untested** |
| **Releases** | None tagged yet. Build from source for now |
| **USB ID** | Currently `1209:0001`, a shared *test* ID. A permanent one will be applied for; see [`brief.md`](brief.md) §2 |

---

## Hardware

| Part | Notes |
|---|---|
| **ESP32-S3 SuperMini** | Must be an S3 or S2 — native USB is required. A classic ESP32, ESP8266 or C3 **will not work** |
| **KY-005 IR LED module** | Any 940nm IR LED module. A bare LED with a resistor works too |
| **SSD1306 OLED, 128×32, I²C** | Optional but recommended — it is how you see which profile is active, and the setup WiFi password |
| **Momentary push button** | Any. Wired to ground; the internal pull-up is used |

### Wiring

| Signal | GPIO |
|---|---|
| IR LED data | 4 |
| OLED SDA | 2 |
| OLED SCL | 3 |
| Button | 5 → GND |

The OLED is addressed at `0x3C`. All pins are set in `firmware/src/main.cpp` — a
different board only needs those four constants changed.

### Enclosure

FreeCAD sources are in `hardware/`. `ESP32-Remote-Housing.FCStd` is the editable
master; the `.step` files are supplier models of the components, for fit
checking. Mesh exports are not tracked — export your own STL from the FCStd.

---

## Flashing the firmware

Needs [PlatformIO](https://platformio.org/install/cli).

```bash
git clone https://github.com/Niall-xox/ESP32-IR-CEC-Alternative
cd ESP32-IR-CEC-Alternative/firmware

pio run -t upload      # the firmware
pio run -t uploadfs    # the web config UI — needed once
```

Both steps are required on a fresh board. `uploadfs` writes the configuration
page into the stick's filesystem; without it the web UI will 404.

Some boards need a button combination to enter download mode before flashing,
and do not leave it automatically afterwards — if the device does not reappear
after `upload`, press reset or replug it.

> **`uploadfs` erases saved profiles.** It rewrites the whole filesystem, so any
> IR codes you have entered are lost, the defaults come back, and the stick picks
> a new setup WiFi password. That is useful for a clean slate and unwelcome
> otherwise. Firmware-only updates (`upload`) leave your profiles alone.

---

## Installing the daemon

### Linux — from source

```bash
cmake -B daemon/build -S daemon -DCMAKE_INSTALL_PREFIX=/usr
cmake --build daemon/build
sudo cmake --install daemon/build
```

Requires `sdbus-c++` **2.x**, `hidapi` and `libudev` (the development package —
the library itself ships with systemd, so it is already installed).

> **`-DCMAKE_INSTALL_PREFIX=/usr` is not optional.** The default prefix is
> `/usr/local`, which puts the udev rule in `/usr/local/lib/udev/rules.d/` and
> the service in `/usr/local/lib/systemd/system/`. Neither udev nor systemd
> looks in those directories, so the install appears to succeed and then the
> daemon cannot open the device and `systemctl enable` cannot find the unit.

Then create the service account the unit runs as, and let udev pick up the new
rule:

```bash
sudo groupadd --system esp32ir
sudo useradd --system --no-create-home --shell /usr/sbin/nologin \
             --gid esp32ir esp32ir
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw --action=add
sudo systemctl enable --now esp32-ir-remote
```

Check it worked — the stick's node should be group `esp32ir`:

```bash
ls -l /dev/hidraw*
journalctl -u esp32-ir-remote -f
```

### Linux — packages

`.deb` and `.rpm` are built for each release, and handle the account, udev rule
and service for you.

> **The `.deb` needs sdbus-c++ 2.x, which Ubuntu 24.04 LTS does not have** — it
> ships 1.4.0. The package installs on Debian 13+ and Ubuntu 25.04+. On 24.04,
> build from source. The `.rpm` needs Fedora 44 or newer.

**Arch** builds from source on your machine:

```bash
cd daemon/packaging && makepkg -si
```

This fetches the `v$pkgver` git tag, so it only works once a release has been
tagged — see *Status* above.

**NixOS** — add the flake and enable the module:

```nix
inputs.esp32-ir-remote.url = "github:Niall-xox/ESP32-IR-CEC-Alternative";

# then
imports = [ inputs.esp32-ir-remote.nixosModules.default ];
services.esp32-ir-remote.enable = true;
```

The module declares the user, udev rule and service for you.

### Windows

Needs Visual Studio Build Tools and `hidapi` from vcpkg.

```powershell
cmake -B daemon/build -S daemon `
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build daemon/build --config Release
```

Then from an **elevated** PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\daemon\packaging\windows\install-service.ps1 `
           -BinaryPath <path>\esp32-ir-daemon.exe
```

The daemon must run as a Windows Service — a normal program is not sent power
events at all. The installer registers it, sets restart-on-failure and creates
the log directory. Logs go to `C:\ProgramData\ESP32IRRemote\daemon.log`.

To check the stick is reachable without installing anything, run the binary with
`--console`.

---

## Using it

### The button

One button, four gestures, by how long you hold it:

| Hold | Result |
|---|---|
| Tap | Wake the screen. Tap again to switch to the next TV profile |
| ~1–4 s | Nothing — a bar fills, one block per second. Let go to cancel |
| **4–7 s** | **Enter or leave wireless config mode** |
| 7–16 s | Nothing — the reset bar fills. Let go to cancel |
| **Past 16 s** | **Factory reset** — restores the default profiles and picks a new setup WiFi password |

When the screen is off, the first tap only wakes it; the *next* tap changes
profile. That stops a blind press in the dark silently switching TVs.

### The screen

Shows the active profile, `TV On` / `TV Off` when a command arrives, and
`Not Configured` if the selected profile has no IR code. It blanks after two
seconds unless you turn on "display always on" in the web UI.

### Configuring it

Hold the button for 4–7 seconds. The stick starts its own WiFi access point and
puts everything you need to connect on its screen:

- **Network:** `ESP32-IR-Remote-XXXX`, where `XXXX` is unique to the stick
- **Password:** eight random characters, chosen by the stick the first time it
  starts and kept until a factory reset
- **Address:** `http://192.168.4.1`

A stick built without the screen cannot show its password, so it uses
`irremote123` instead.

Connect and open that address in a browser. You can add, edit and delete TV
profiles, set the active one, toggle the always-on display, and factory reset.
Hold the button 4–7 seconds again to exit — power syncing keeps working while
the config page is up.

---

## TV profiles and IR codes

A **profile** is one TV: a name, an IR protocol, and two codes — one to turn the
TV on, one to turn it off. Four ship by default, and all four transmit:

| Profile | Protocol |
|---|---|
| LG | NEC |
| Samsung | Samsung |
| Sony | Sony |
| Toshiba | NEC |

If your TV is not listed, add it through the web UI. You will need two things:
the **protocol** and the two **codes**.

### Discrete codes, not toggle codes

This is the part that trips people up. Most remotes send a single *toggle* code
for power — press it and the TV flips state. That is no use here: if the TV and
PC ever fall out of sync, every command makes it worse.

You need **discrete** codes — a separate "power on" and "power off". Most TVs
understand them even though their own remote never sends them, which is why you
have to look them up rather than copy them off the remote in front of you.

### Where to find them

[**irdb**](https://github.com/probonopd/irdb) is the best starting point. Browse
`codes/<Brand>/TV/` and look for `POWER ON` and `POWER OFF` entries:

```
POWER ON/OFF,Sony12,1,-1,21
POWER ON,Sony12,1,-1,46
POWER OFF,Sony12,1,-1,47
```

[**RemoteCentral**](https://files.remotecentral.com/library/index.html) has
curated discrete codes for brands irdb lacks.

Two warnings from experience. **Some brands have no discrete codes at all** —
TCL and Hisense are the notable ones, which is why they are not shipped as
profiles. And **never put a toggle code in both fields**; the TV would flip on
every single command.

Converting a database entry into the hex the firmware wants is fiddly. The
method, including how to check a code before trusting it, is written up in
[`brief.md`](brief.md) under *Where IR codes come from*.

---

## Troubleshooting

**The daemon says "ESP32 not connected".** Check the stick enumerates —
`lsusb` should show `1209:0001`. On Linux, check `/dev/hidraw*` for a node owned
by group `esp32ir`; if it is `root:root`, the udev rule has not been applied.
Replug the stick after installing.

**The TV does nothing but the daemon reports success.** The IR code was
transmitted but your TV did not accept it — usually the wrong profile, or codes
that do not match your model. Try aiming the stick directly at the TV's sensor;
IR needs line of sight.

**The screen says `Not Configured`.** The active profile has no code for that
direction. Add one through the web UI.

**Nothing happens on sleep.** Check the service is running:
`systemctl status esp32-ir-remote`, or on Windows look at
`C:\ProgramData\ESP32IRRemote\daemon.log`.

---

## Licence

**GPL-3.0-or-later.** See [LICENSE](LICENSE).

This covers everything in the repository, including the enclosure design in
`hardware/`.

Built on these, with thanks:

| Project | Licence |
|---|---|
| [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) | LGPL-2.1 |
| [ArduinoJson](https://arduinojson.org/) | MIT |
| [Adafruit SSD1306 / GFX](https://github.com/adafruit/Adafruit_SSD1306) | BSD |
| [hidapi](https://github.com/libusb/hidapi) | BSD / GPL-3 / HIDAPI |
| [libudev](https://github.com/systemd/systemd) (systemd) | LGPL-2.1 |
| [sdbus-c++](https://github.com/Kistler-Group/sdbus-cpp) | LGPL-2.1 |
| [irdb](https://github.com/probonopd/irdb) | IR code database |

---

## More detail

[`brief.md`](brief.md) is the working document — how the two halves talk to each
other, why the design decisions were made, what is verified and what is not, and
what is still outstanding. Read that before changing anything.
