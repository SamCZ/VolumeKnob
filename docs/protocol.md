# Volume Knob serial protocol

Transport: the ESP32-S3 native USB Serial/JTAG port (VID 303A, PID 1001), 115200 baud
nominal (the baud rate is ignored by USB). The companion opens the port with DTR and RTS
low so the board is never reset.

## Finding the knob

Other ESP32 boards share VID 303A / PID 1001, and the USB descriptors of the S3's built-in
USB Serial/JTAG port cannot be changed by firmware. Instead:

1. **Pairing (first run, or after "Forget paired knob" in the tray menu):** the companion opens
   each 303A:1001 port and only *listens* for the `!VOLKNOB 1 <id>` beacon. It never writes to a
   port that has not announced itself. The port's USB serial number is then saved as the paired knob.
2. **Paired:** the companion looks up every 303A:1001 port's USB serial number (parent device
   instance id via CfgMgr32) and opens only the one that matches. Other boards are never opened.

`--port COMx` on the command line skips both and talks to that port directly.

## Messages

Every message is one ASCII line terminated by `\n`. Fields are separated by a single space.
Volumes are integers 0-100, mute flags are `0` or `1`, icon keys are 8 hex digits.

## PC -> knob

| Line | Meaning |
|---|---|
| `?` | Handshake. Knob answers `!VOLKNOB 1`. |
| `P` | Keep-alive, sent when nothing else was sent for 1 s. The knob shows "PC offline" after 3.5 s without any line. |
| `X` | Companion is exiting. The knob goes offline immediately. |
| `S` | Start full sync. Every app the knob knows is marked stale. |
| `E` | End full sync. Apps still stale are removed. |
| `O <name>` | Name of the default output device. |
| `G <vol> <mute> <fixed>` | Master volume. `fixed` = 1 when the output device has no adjustable volume (e.g. a headset with a hardware dial); the knob then skips the master screen and uses the app list as its home screen. |
| `A <id> <iconKey> <vol> <mute> <name>` | Add or update an app (one volume-mixer entry). `name` is the rest of the line. |
| `V <id> <vol> <mute>` | App volume changed. |
| `R <id>` | App closed / left the mixer. |
| `I <iconKey> <w> <h> <base64>` | Icon pixels, `w*h` ARGB8888 pixels in B,G,R,A byte order (straight alpha). Only sent when the knob asked for it. |
| `D` | Debug dump; the knob answers with `#` lines. |
| `J r <n>` / `J c` / `J d` / `J l` | Debug: simulate rotating n detents / click / double click / long press. |
| `W` | Debug: screenshot, answered with `#shot w h`, 240 `#row y <base64 RGB565 LE>` lines, `#endshot`. |

Icon keys: `00000000` = no icon (the knob draws a letter badge), `00000001` = System Sounds
(the knob draws a speaker badge), anything else = FNV-1a hash of the lower-cased exe path.

## Knob -> PC

| Line | Meaning |
|---|---|
| `k <iconKey>` | Icon line consumed (stored or rejected). The PC sends the next icon only after this, because the USB driver drops bytes when its 32 KB RX queue overflows. |
| `!HELLO` | Sent at boot. The PC answers with a full sync. |
| `!VOLKNOB 1 <id>` | Handshake answer, and a beacon sent every second while no companion is talking to the knob. `<id>` is the chip MAC (`CC:68:C7:8C:3C:3C`), the same value Windows reports as the port's USB serial number. |
| `g <vol> <mute>` | User changed master volume / mute on the knob. |
| `a <id> <vol> <mute>` | User changed an app's volume / mute on the knob. |
| `q <iconKey>` | Knob needs this icon (it is not in its cache). |
| `r` | Knob dropped value updates while the user was turning the knob; the PC re-sends all current values. |
| `#...` | Debug text, logged by the companion. |

## Caching

The knob keeps icons in PSRAM keyed by icon key, shared by all apps with the same key.
After every change to the app list the UI is rebuilt and any icon no app references is freed.
Because keys are content-based, icons survive a companion restart: after the resync the knob
only asks for icons it does not already hold.
