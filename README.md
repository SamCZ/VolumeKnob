# Volume Knob

A desk knob for the Windows volume mixer, built on the Elecrow **CrowPanel 1.28" HMI ESP32-S3
rotary display** (240×240 round IPS screen with a rotary encoder and push button).

The knob shows the master volume and every app that is playing sound, with its icon and name.
Turning it changes the volume, and changes made on the PC show up on the knob straight away.
A small tray app on Windows connects the two over USB.

Hardware, schematics, datasheets, 3D models and the factory firmware are in Elecrow's original
repository: <https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen>

## Contents

| Folder | What it is |
|---|---|
| `src/` | Knob firmware (PlatformIO, Arduino core 3.x, LVGL 9.3, LovyanGFX) |
| `companion/VolumeKnob/` | Windows tray app (.NET 10, NAudio Core Audio) |
| `docs/protocol.md` | Serial protocol between the knob and the companion |
| `stand/` | 3D-printable 45° desk stand, with print and assembly notes |

## Using the knob

| Action | Master screen | App list | App screen |
|---|---|---|---|
| Turn | Change master volume | Select an app | Change the app's volume |
| Click | Open the app list | Open the selected app | Back to the app list |
| Double click | Mute / unmute master | – | Mute / unmute the app |
| Hold | – | Back to master | Back to master |

- Turning faster changes the volume in bigger steps. Turning a muted channel unmutes it, as in
  Windows.
- The app list and app screens return to the master screen after 15 s without input.
- If the output device has a fixed volume (some headsets with a hardware dial report a 0 dB to
  0 dB range), the master screen is read-only and shows "Fixed by device". App volumes still work.
- Without the companion the knob shows "PC offline".

Icons are sent once as 64×64 images and cached on the knob, in PSRAM. They are freed when no
app uses them any more, and they survive a companion restart.

## Firmware

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code extension). The first build
downloads the pioarduino platform and the libraries.

```bash
pio run
```

```bash
pio run -t upload --upload-port COM16
```

Replace `COM16` with the knob's port. The knob uses the ESP32-S3's built-in USB Serial/JTAG
port, so no buttons need to be pressed to flash it.

Configuration:

- `kFlip180` in `src/board.cpp` turns the picture 180°. It is on for the stand, which holds the
  knob upside down compared with the factory orientation. Set it to `false` for the original
  orientation.
- Display, backlight and encoder pins are in `src/board.cpp`. UI timings and colours are at the
  top of `src/ui.cpp`.

## Companion (Windows)

Build with the [.NET 10 SDK](https://dotnet.microsoft.com/download):

```bash
dotnet run --project companion/VolumeKnob
```

Or publish a single self-contained `VolumeKnob.exe` that needs no .NET install:

```bash
dotnet publish companion/VolumeKnob -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o release
```

The companion runs in the notification area. Its menu has:

- the connection status;
- **Start with Windows**;
- **Forget paired knob**;
- **Dump knob state to log**;
- **Open log**.

The log is written to `%LOCALAPPDATA%\VolumeKnob\log.txt`, and settings to
`%APPDATA%\VolumeKnob\settings.json`.

### Finding the knob

Many ESP32-S3 boards share the same USB IDs (303A:1001), so the companion never writes to a port
until the device on it has said it is a knob:

1. **First run:** the companion only *listens* on each 303A:1001 port for the knob's
   `!VOLKNOB` beacon. It then saves the knob's USB serial number, which is the chip's MAC address.
2. **After that:** it opens only the port whose USB serial number matches the saved one. Other
   boards are never opened.

Use **Forget paired knob** to switch to a different knob. `VolumeKnob.exe --port COM16` skips
detection and uses that port.

## Stand

`stand/` has a 45° desk stand that replaces the knob's white back plate and reuses its two
screws. Print-ready STL and 3MF files are included, along with a STEP file and the Fusion 360
script that generates them. See [stand/README.md](stand/README.md).
