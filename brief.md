# ESP32 IR Remote — Project Brief

A USB stick that makes a TV follow a PC's power state. The PC sleeps, the TV
turns off. The PC wakes, the TV turns on. It replaces HDMI-CEC, which most PCs
do not have.

This is the single source of truth for how the project works and where it
stands. The README covers building and using it; this covers why it is the way
it is.

---

## 1. How it works

```
  PC                                  ESP32-S3 stick                TV
  daemon  ──── USB HID ────────────►  firmware  ──── IR LED ─────►
  (background service)                (Arduino)      (infrared)
```

**The daemon** runs on the PC. It notices power events and sends `ON` or `OFF`
down the USB cable.

**The firmware** runs on the stick. When it hears `ON` or `OFF` it sends the
matching infrared code to the TV, like the TV's own remote would. It also runs a
small screen, one button, and a WiFi setup page.

The two sides share nothing but those two words and a reply.

### The rule

**The TV mirrors the PC's screen.** Screen on, TV on. Screen off — idle, sleep,
shutdown — TV off. That is what CEC does, which is the point.

**Windows follows this exactly. Linux follows power state instead.** Windows
tells a service whenever the screen turns on or off, idle blanks included.
Linux has no system-wide equivalent, so on Linux the TV follows sleep, wake,
shutdown and boot, and an idle blank that does not lead to sleep leaves the TV
on. That was a deliberate choice (§10).

### Why USB HID and not a serial port

HID is the USB class keyboards and mice use, so every OS already has a driver:
no COM port to pick, nothing to install. The cost is that the chip needs native
USB. The ESP32-S3 and S2 have it; the C3, classic ESP32 and ESP8266 do not. A
serial version existed and was removed (§11).

### Where things live

```
README.md  LICENSE  VERSION  brief.md  flake.nix
firmware/          the stick: src/, data/ (the web page), platformio.ini
daemon/            the PC side: src/, packaging/, CMakeLists.txt
hardware/          FreeCAD enclosure and part models
tools/hid-test/    a bare HID echo, for a board that will not talk at all
.github/workflows/ compiles the daemon on every push; packages on a v* tag
```

- `VERSION` is at the root because both halves share one protocol and ship
  together. CMake, the flake and the PKGBUILD all read it.
- No path contains a space, so no script ever needs quoting.
- `tools/hid-test` uses its own throwaway USB ID, `1234:5678`, so it can never
  be mistaken for the real device.

---

## 2. Hardware

| Part | Notes |
|---|---|
| ESP32-S3 SuperMini | Any S3 board works; pins are set in code. |
| KY-005 IR LED module | GPIO 4. |
| SSD1306 OLED, 128×32, I²C | SDA GPIO 2, SCL GPIO 3, address 0x3C. |
| Push button | GPIO 5 to ground (internal pull-up). |

The enclosure is `hardware/ESP32-Remote-Housing.FCStd`. STL exports are not
kept in git; export fresh ones from the FreeCAD file.

### The USB ID, `1209:0001`

Every USB device has a vendor ID and a product ID. `1209` belongs to
**pid.codes**, which gives product IDs to open-source hardware for free.
`0001` is their shared **test** ID. Their rule: it "MUST NOT be used on any
device that will be redistributed, sold, or manufactured."

That binds on hardware. A release here ships only daemon packages, and the one
stick carrying the test ID is the author's, so releasing on it was accepted. It
stops being acceptable once a stick goes to anyone else.

Applying for a real ID needs only a public open-source repository, which
exists, so it can happen any time. The new ID will be `0x2000` or higher. Do
not invent a number in the meantime.

The ID is copied by hand into five files, which must change together (kept by
hand on purpose — §10):

| File | Spelling |
|---|---|
| `firmware/src/main.cpp` | `0x1209` / `0x0001` |
| `daemon/src/main.cpp` | `0x1209` / `0x0001` |
| `daemon/99-esp32-ir-remote.rules` | `"1209"` / `"0001"` |
| `daemon/packaging/windows/install-service.ps1` | `"1209"` / `"0001"` |
| `daemon/packaging/windows/verify-windows.ps1` | `"1209"` / `"0001"` |

---

## 3. The USB conversation

One message, one reply, each a fixed 64-byte packet.

- **PC → stick:** a sequence number, then `ON` or `OFF`.
- **Stick → PC:** the same sequence number, then `ACK` or `ERR`.

**`ACK` means the infrared was actually sent.** `ERR` means it was not, which in
practice means the active profile has no code for that direction. The daemon
treats both as "did not work", but `ERR` arrives instantly and says why,
instead of looking like a broken cable.

**The sequence number** (1–255, then back to 1) stops a late reply being taken
for the wrong command. Without it: `OFF` times out, its `ACK` arrives late, and
the next `ON` reads that stale `ACK` as its own success.

### One timeout, and not waiting for a stick that is not there

A send gets **2 seconds** in total — opening the device, any retry, and the
reply. A healthy send takes under 200 ms. The number lives in one place in the
transport, and callers cannot change it.

What happens when the stick is missing matters more:

- **Not plugged in** → give up immediately.
- **Was working a moment ago** → keep retrying until the 2 s are up; it is
  probably re-connecting.
- **Owed a command before it has arrived** (the boot `ON`) → wait for the OS to
  say it has arrived. Never retry on a timer.

That is why the daemon can stay installed with the stick unplugged without ever
slowing down sleep or shutdown.

---

## 4. The firmware (`firmware/`)

About 1,200 lines.

| File | Does |
|---|---|
| `main.cpp` | USB, sending IR, the web server, and wiring the rest together. |
| `Profiles.cpp/.h` | TV profiles and settings, saved to flash. |
| `Display.cpp/.h` | Everything on the screen, and when it blanks. |
| `Button.cpp/.h` | Turns the button's raw signal into press, hold and release. |
| `HoldTimings.h` | The button hold times, in one place. |

### Profiles

A **profile** is one TV: a name, an IR protocol (NEC, Samsung or Sony), an ON
code, an OFF code, and whether the button cycles through it. Up to 32.

Four ship by default, and every one transmits:

| Profile | Protocol | ON | OFF |
|---|---|---|---|
| LG | NEC | `0x20DF23DC` | `0x20DFA35C` |
| Samsung | SAMSUNG | `0xE0E09966` | `0xE0E019E6` |
| Sony | SONY | `0x00000750` | `0x00000F50` |
| Toshiba | NEC | `0x02FD7E81` | `0x02FDFE01` |

**A brand only ships as a default if one discrete ON/OFF pair covers its whole
range.** A default that cannot work is worse than none, because it looks like
support. So **TCL and Hisense are left out**: TCL only has a power toggle, in a
protocol the firmware does not speak, and Hisense is not in the main code
database at all. Both use different remotes across markets, so there is no one
code set to ship. Their owners add a profile in the web page.

**Only LG is proven on a real TV.** Samsung, Sony and Toshiba were converted
from published codes and checked against the encoder, but never tried. Two
Samsung risks:

- Reports say some Samsungs stop listening to IR after being off a long time.
  If true, the TV would not wake with the PC in the morning. Check this first
  if a Samsung turns up.
- Samsung's discrete pair is not in irdb (below); it comes from a single other
  source.

**Defaults are only written on first boot**, when `profiles.json` does not
exist. Picking up new defaults needs a factory reset; re-flashing is not
enough.

**Storage.** Profiles and settings are two JSON files in the stick's flash.
Each save writes a temporary file, checks it was written in full, then swaps it
in. The size check catches a save cut short by full flash or lost power;
without it, a half-written file would replace a good one and every saved code
would be gone.

### Discrete codes, not toggles

Most remotes send one **toggle** code for power, which flips the TV. That is
useless here: once the TV and PC are out of step, every command makes it
worse. This project needs **discrete** codes — one that only turns the TV on,
one that only turns it off. Most TVs accept them even though their own remote
never sends them, so they have to be looked up.

A bonus: repeating a command is always harmless. `OFF` to a TV that is off does
nothing, so the daemon never has to remember what it last sent.

### Where IR codes come from

Kept because finding codes again from scratch is slow, and a wrong code is
worse than none.

**[irdb](https://github.com/probonopd/irdb) first.** It stores codes as
protocol parameters rather than raw hex, which is what you want. Look in
`codes/<Brand>/TV/`:

```
curl -s https://cdn.jsdelivr.net/gh/probonopd/irdb@master/codes/Sony/TV/1,-1.csv | grep -i power
  POWER ON/OFF,Sony12,1,-1,21
  POWER ON,Sony12,1,-1,46
  POWER OFF,Sony12,1,-1,47
```

That shows at a glance whether discrete codes exist, and how many code sets the
brand has. One set means one scheme for the whole brand; five means it varies
by model, and no single default is safe.

**Convert with the firmware's own encoder**, never with hex copied off a forum,
so the bit order cannot disagree. `encodeNEC` and `encodeSony` are in
`firmware/.pio/libdeps/lolin_s3_mini/IRremoteESP8266/src/`; they are small
enough to copy into a scratch C file and run. **Check the method** by encoding
a code you already know: Sony's toggle should come out `0xA90`, and LG's ON
`0x20DF23DC`.

**Other sources:** RemoteCentral's discrete code library (where Samsung's pair
came from); the SB-Projects protocol pages, when a code looks right but does
not work (Sony's 12/15/20-bit lengths are the classic trap — TVs use 12);
Flipper Zero and LIRC databases, usually model-specific.

**Red flags:** hex with no protocol named; one model presented as a whole
brand; NEC codes that fail the checksum — the fourth byte must be the bitwise
inverse of the third, so `0x…8B75` is wrong because `0x8B` inverts to `0x74`.

### The button

| Hold for | Result |
|---|---|
| under 0.3 s | **Press**: wake the screen; press again to switch to the next profile |
| 0.3–4 s | Nothing — a bar fills; let go to cancel |
| 4–7 s | **Turn WiFi setup mode on or off**, on release |
| 7–16 s | Nothing — the reset bar fills; let go to cancel |
| 16 s | **Factory reset**, while still held |

When the screen is off, the first press only wakes it; the next one switches
profile. That stops a press in the dark silently changing which TV you control.
With "display always on" set, every press switches.

### The screen

Shows the active profile, then blanks after 2 seconds unless "display always
on" is set. Also shows `TV On` / `TV Off` when a command arrives,
`Not Configured` when a profile has no code, the two hold bars, and in WiFi
mode the address to browse to.

Each bar has one block per second held, worked out from the hold times, so the
hold bar has 4 blocks and the reset bar 9. Neither ever fills completely: the
moment it would, the screen changes instead, and that is the signal.

### WiFi setup mode

Hold the button 4–7 s and the stick starts its own WiFi network, `ESP32-IR-Remote`
(password `irremote123`). The page at the address on the screen can add, edit,
delete and reorder profiles, choose the active one, toggle "display always on",
and factory reset. It is stored in the stick's flash and uploaded with
`pio run -t uploadfs`. IR keeps working while setup mode is on.

### Recovering on its own

**Watchdog.** If the firmware's main loop ever stops for **20 seconds**, the
chip restarts itself. A hung stick looks exactly like an unplugged one to the
PC, so without this a hang would last until somebody noticed. A restart loses
nothing: profiles are in flash, and the daemon reconnects on its next command.
20 s is set by the web server, which can legitimately stall ~12 s on a slow
browser; everything else takes under 200 ms. The watchdog starts after setup,
so first-boot flash formatting never has to beat it.

**USB after sleep.** The stick's USB side freezes across a sleep unless the PC
resets it on wake: it accepts nothing until it is replugged. Whether a PC does
that depends on the port. On the test machine the chipset's ports reset every
device on resume and the CPU's ports do not, so moving the stick between them
turned working wakes into failed ones. On Linux the daemon resets the stick
itself on every wake, which makes every port behave like the ones that work.
The firmware's own recovery cannot help: it waits for a resume event this
Arduino core never delivers. The Windows installer stops Windows suspending the
idle stick at all (§5); whether a Windows sleep freezes it is untested.

---

## 5. The daemon (`daemon/`)

About 1,400 lines of C++, one program, built with CMake. Two small interfaces
keep the platforms apart:

- **Transport** — "send this command; did it work?" One implementation
  (`HIDTransport`), shared by both platforms.
- **Power monitor** — "tell me when the TV should change." One per platform
  (`LinuxPowerMonitor`, `WindowsPowerMonitor`).

### Linux

**Sleep and shutdown** come from `systemd-logind`, which also lets a program
take an **inhibitor lock**: sleep and shutdown are held off until the program
lets go. So the TV is really off before the PC goes down:

```
sleep requested → daemon sends OFF → stick sends IR → ACK → lock released → PC sleeps
```

The lock has to be held *before* the announcement; logind only waits for locks
that already exist. So the daemon takes it at start-up and again after every
wake. On wake it sends `ON`.

**Boot** is harder, because the daemon starts about 4.5 s before the stick is
ready — the stick is powered by the PC that is booting. So the first `ON` fails,
and the daemon waits for **udev** to announce the stick, then sends it. The
waiting is event-driven, with no timer.

**Two guards** stop that `ON` firing when it should not. It is only sent if the
PC has been up for under 3 minutes, checked at start-up and again when the stick
arrives. So a package upgrade restarting the service at 3 a.m. does not turn
the TV on, and neither does replugging the stick a week later.

**The service's sandbox must allow `AF_NETLINK`**, the socket type udev uses.
Without it, only the boot `ON` is lost — sleep, wake and shutdown all still
work — so nothing looks wrong unless you read the log. This shipped once. The
log now says exactly what to change.

**Linux does not watch the screen.** An idle blank leaves the TV on (§10).

### Windows

**Only a Windows Service is told about power events**, so the daemon runs as
one, installed by `install-service.ps1`. This is normal for PC accessory
software. Running the program by hand with `--console` sends one `ON` to check
the stick answers, then exits.

**Windows does not wait for sleep.** There is no inhibitor lock; the PC goes
down about 2 s after announcing sleep, whatever the daemon is doing. A send
usually takes ~200 ms, so it normally makes it, but it is not guaranteed.
Shutdown *does* wait — the installer allows up to 60 s.

| Event | Guaranteed? |
|---|---|
| Linux sleep / shutdown | Yes — inhibitor lock |
| Windows shutdown | Yes — Windows waits for the service |
| Windows sleep | Best effort — ~2 s |

**The screen's state drives everything.** Windows reports display on, off or
dimmed; the daemon sends `ON`, `OFF` or `ON` (dimmed counts as on). This works
the same on every type of Windows machine. When Windows says "display off" it
has already blanked every screen, the TV included, so acting on it never
blacks out a picture somebody is watching.

Sleep and wake are a backup trigger underneath. Wake does not send `ON` if the
screen is still known to be off, and neither does the PC waking itself for
overnight maintenance, since the screen stays off.

**Boot `ON`.** Windows also reports the stick arriving, which delivers the boot
`ON` if the stick was late. That only happens in the first 3 minutes after the
**service** started, so a replug later leaves the TV alone. It is timed from
the service starting, not from boot, because Windows' default **Fast Startup**
is really a resume from hibernation: the PC's uptime can read hours after what
the user saw as a cold boot, and an uptime check would skip the boot `ON` on
most machines.

This is why Linux and Windows use different clocks for the same job. Linux
reacts to the daemon starting, which also happens on a service restart, so it
must ask how long the PC has been up. Windows reacts to the stick arriving,
which a restart does not cause. Sharing the code was tried and reverted.

**Threading.** Windows delivers events on its own thread, which must return at
once, so it only records the event and a worker thread does the sending. Linux
needs none of this: everything runs on one thread.

**The installer** (`install-service.ps1`) registers the service to start
automatically, restarts it 5 s after a crash, sets the 60 s shutdown wait,
turns off USB power-saving for the stick, and creates the log folder. Re-running
it upgrades in place. `verify-windows.ps1` writes a report on the machine and
the daemon's log, for comparing machines.

---

## 6. Rules that must not be broken

Each of these, broken, brings back a bug that was already fixed once.

**Behaviour**
- On Windows the TV follows the screen; sleep, wake, shutdown and boot are only
  a backup under that.
- On Linux power state is the whole signal. Do not add screen watching (§10).
- Repeated commands are harmless (discrete codes), so the daemon never tracks
  what it last sent.

**Honesty**
- `ACK` only after the IR has actually been sent. A profile with no code answers
  `ERR`.
- Replies are matched by sequence number, never by timing.
- A failed send must never stop the PC sleeping or shutting down.

**Timing**
- One timeout for a whole send, in the transport. Callers cannot change it.
- No stick plugged in → fail at once. A command owed to a stick that has not
  arrived waits for the arrival event, never a timer.

**One process owns the stick.** Two programs talking to it steal each other's
replies, and the sequence number cannot tell — the stolen reply is correctly
numbered, for someone else. The likely cause is running `--console` while the
service runs. Linux prevents it with the udev group, Windows with a named lock.

**Threading.** The transport is never used from two threads, and has no lock.
On Linux the udev watch shares the one D-Bus thread; moving it to its own thread
would make a lock necessary.

**Storage**
- Flash saves are all-or-nothing *and* size-checked.
- Every lookup of a profile by position is range-checked.

**Logging is unbuffered.** Otherwise the last lines before the process is killed
— including the shutdown `OFF` confirmation — are lost.

**Build**
- The Linux service keeps `AF_NETLINK` in `RestrictAddressFamilies`, in both the
  unit file and the NixOS module (§5).
- The firmware builds with `ARDUINO_USB_MODE=0`. At `1` the stick shows up as a
  serial port and the daemon never finds it.
- The flash partition table is named explicitly, so a toolchain update can
  never move the stored profiles.
- Never name a PowerShell variable `$Pid`: PowerShell reserves `$PID`, and the
  script fails.

---

## 7. Building and packaging

Step-by-step instructions are in the [README](README.md), not repeated here.
What the README does not cover:

- **Dependencies:** `sdbus-c++` **2.x** (1.x will not compile, and the build
  says so up front), `hidapi`, and `libudev`. All three come with any system
  that has systemd, which the daemon needs anyway.
- **A hand install must use `-DCMAKE_INSTALL_PREFIX=/usr`.** The default,
  `/usr/local`, puts the udev rule and service file where udev and systemd never
  look. The install "succeeds" and nothing works. (Packages are unaffected.)
- **Four routes, one build.** `.deb` and `.rpm` are built by GitHub Actions on
  a `v*` tag. Arch builds from `daemon/packaging/PKGBUILD` on the user's
  machine. NixOS uses the flake's module. All four run the same
  `CMakeLists.txt`.
- **Which systems the packages suit.** The `.deb` needs Debian 13 or Ubuntu
  25.04+; Ubuntu 24.04 only has sdbus-c++ 1.x. The `.rpm` is built on Fedora
  Rawhide and needs Fedora 44+, so it is a preview.
- **Windows** needs Visual Studio Build Tools and hidapi from vcpkg. The log is
  `C:\ProgramData\ESP32IRRemote\daemon.log`, moved to `daemon.log.old` past
  1 MB.

---

## 8. Where the project stands

### Verified on hardware (Linux, NixOS)

- Sleep, wake, shutdown and boot all work. The inhibitor lock held shutdown
  until the stick confirmed the IR had gone out. Each send took ~120 ms.
- **Boot:** booted with the stick unplugged, plugged it in 10 s later, and the
  `ON` went out 190 ms after it appeared. On a normal boot the stick arrives
  4.5 s after the daemon starts, every time — the boot `ON` used to be lost on
  every boot, not occasionally.
- The udev filter matched only the stick among eleven HID devices.
- The uptime guard works both ways.
- A profile with no code: nothing sent, `Not Configured` on screen.
- Idle cost of the daemon: no measurable CPU.

**If the boot `ON` ever goes missing, read the log first.** `Watching udev for
the ESP32` means it is working. `udev watch unavailable` means only sleep, wake
and shutdown are active — and they keep working perfectly, which hides it.

### Windows: works, less proven

Compiled on every push. Run as a service on three machines, one of each sleep
type, and correct in what was tested. But it has had far less use than Linux,
and it changed after that testing. To re-test, in this order:

1. **The installer.** On 2026-09-26 its two USB helper functions were found to
   fail under PowerShell 7 (a parameter named `$Pid`) and were fixed. Whether
   Windows' built-in PowerShell hit the same failure is unknown. Check a clean
   install and an uninstall first.
2. **Classic-sleep (S3) machine:** an idle screen blank should now turn the TV
   off. The biggest change.
3. **Stick unplugged:** shutdown should be instant (it used to wait up to 20 s).
4. **Normal sleep and wake:** 2 s is enough.
5. **Boot with Fast Startup on** (the default): the TV comes on.
6. **Replug later on:** the TV does not change.

### Not verified

- Booting with "display always on" set: the status screen should show from
  boot.
- Recovering from a corrupt `profiles.json`. The code exists; testing it means
  flashing a deliberately broken image. Judged not worth it.

### Known gaps

Things that are wrong or missing now and easy not to notice:

| Gap | Severity |
|---|---|
| **Before any stick goes to someone else:** it still uses the shared test USB ID (§2), and every stick has the same WiFi password. The password should come from the chip ID and show on the screen. | Medium |
| **Only the LG codes are proven.** Samsung, Sony and Toshiba have never been tried on a real TV (§4). | Medium |
| **Any `v*` tag publishes a public release**, and nothing checks anything first. | Medium |
| **The Windows service runs as LocalSystem**, the most privileged account. Linux runs unprivileged. | Medium |
| **A hand install without `-DCMAKE_INSTALL_PREFIX=/usr` silently half-works** (§7). A CMake warning would catch it at the moment of the mistake. | Medium |

---

## 9. What to do next

**Tagging publishes.** A `v*` tag makes a public GitHub Release with the
packages attached. Everything is at `1.0.0` and nothing blocks the tag; first
update the README Status table, which still says "None tagged yet". The items
below are improvements, not blockers.

1. **Try the Samsung, Sony and Toshiba codes on real TVs.** Samsung matters
   most: the most common brand, and the sleep risk in §4.
2. **Apply to pid.codes for a product ID.** It is a pull request reviewed by
   volunteers, so start early. When it arrives, change all five copies (§2) in
   one commit.
3. **Re-test Windows** against the six checks in §8, then move the service to
   LocalService. The installer already prepares the log folder for it; the open
   question is whether that account can open the stick, which is very likely.
   Test with `sc.exe config esp32-ir-remote obj= "NT AUTHORITY\LocalService"`.
4. **Build the firmware in CI.** Today CI only compiles the daemon.

**Worth considering: an IR receiver.** A ~£1 VS1838B on one more pin would let
the web page "learn" codes: point your remote at the stick and press power. The
IR library already decodes. That would turn "works if you can find codes for
your TV" into "works for anyone" — the biggest improvement available.

---

## 10. Decisions taken, and why

Things that look removable, improvable or missing, and are not. Read before
"tidying" any of them.

- **The sequence number stays.** Without it a late `ACK` gets taken as the
  reply to the next command (§3).
- **`ERR` stays.** It turns a confusing two-second timeout into an instant,
  accurate log line. If it were ever removed, `ACK` must not start meaning
  "received"; make a profile with no code unselectable instead.
- **The Windows single-instance lock stays** even with a real USB ID. A unique
  ID does not stop two programs on the same PC opening the stick.
- **The boot `ON` guards stay, and stay different per platform.** Linux checks
  uptime, or a 3 a.m. package upgrade restarting the service turns the TV on.
  Windows times from service start, because Fast Startup makes uptime
  meaningless. Merging them into shared code looks tidy and is wrong; it was
  tried and reverted (§5).
- **A cancelled shutdown is not handled on Linux.** A normal cancel
  (`shutdown -c`) happens before logind announces anything, so the daemon never
  sees it. The only case that matters is logind announcing the shutdown and
  then calling it off, in the ~120 ms the daemon holds the lock while sending
  `OFF`. That only happens when the shutdown fails to start at all. Even then,
  the first sleep and wake afterwards puts everything right. The fix would be
  three lines that could never be tested on real hardware.
- **Both services restart forever after a crash, 5 s apart.** Windows repeats
  its last restart action indefinitely, and systemd's limit (5 failures in
  10 s) is never reached at 5 s intervals. The two platforms already match.
- **The five copies of the USB ID are not generated from one source.** Once the
  real ID arrives it changes once and never again. Generating it would mean
  build steps in PlatformIO, CMake and PowerShell for a one-time edit; the
  table in §2 is enough.
- **The web server may serve `profiles.json` directly.** The setup page already
  hands out the same data through `/api/profiles`, so blocking the file would
  protect nothing.
- **Linux follows power state, not the screen** (2026-09-26). The two ways to
  watch the screen were both rejected:
  - *Read the display power state from the kernel.* Works on every desktop, but
    the kernel gives no notification, so it must be polled; it reads hardware
    beneath the desktop rather than asking it; and the NVIDIA driver may not
    keep it current.
  - *Ask the desktop.* The proper way, but GNOME, KDE and the wlroots family
    (Hyprland, Sway…) each do it differently, and the system service cannot
    reach any of them. It would need a helper in every user session, a
    locked-down D-Bus interface, and changes to all four packages — roughly
    500–800 lines.

  The cost is an idle blank without sleep, where the TV stays on. Sleep, wake,
  shutdown and boot — most real use — behave the same on both platforms, and
  setting the desktop to sleep when idle covers most of the rest. (DPMS itself
  cannot help: it is the graphics card stopping the signal. Monitors sleep on
  that and TVs do not, which is why this project exists.)

---

## 11. Still open

Deferred on purpose. Neither blocks a release.

- **A serial version**, for ESP32 boards without native USB. The transport
  interface is the right place for it, and the old code is in git history. The
  hard part is finding the device: those boards use common USB-serial chips
  whose IDs thousands of other gadgets share. It needs a config file naming the
  port (or probing each port), a second firmware build, and a looser Linux
  sandbox. Perhaps 150 lines.
- **Panasonic.** A major UK brand with published discrete codes, but its
  protocol uses 48-bit codes and profiles hold 32 bits. Dropped for now.
