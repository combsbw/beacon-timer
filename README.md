# Beacon Timer

Interval timer with two RGB LEDs (Timer and Set) for the **ESP32-S3 Super Mini**. The timer runs on the board; a phone
joins the open Wi-Fi network **Beacon-timer**, opens `http://192.168.4.1` (a captive portal does it automatically) and
gets the control panel: Start / Pause / Stop / Reset, every setting, and firmware update.

## Build in the cloud (no local install)

The repository has a GitHub Actions workflow (`.github/workflows/build.yml`). Every push and pull request compiles the
sketch with core 2.0.17 and runs the engine tests. Open the run on the **Actions** tab and download the
`beacon-timer-firmware` artifact:

- `beacon-timer.ota.bin` - the app image: upload it in Settings > Device (firmware update) or with `make ota-http`.
- `beacon-timer.full.bin` - a complete image (bootloader + partitions + app) to flash at address 0x0 with a web
  flasher (Chrome/Edge, over USB), for the very first install.

Every push to `main` also refreshes the **Releases > Latest build** page, where both `.bin` files are separate
downloads (no tags to create).
The repository folder must be named `beacon-timer` (the sketch name) when built locally.

## Build and flash

Needs `arduino-cli`, `make` and `python3`.

```
make ports     # list serial ports
make deps      # esp32 core 2.0.17 (ArduinoOTA, WebServer, Update, DNSServer, ESPmDNS, Preferences come with it)
make build     # FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc
make flash     # build + upload over USB   (PORT=/dev/ttyACM0 to choose the port)
make ota       # build + upload over Wi-Fi with ArduinoOTA (join "Beacon-timer" first)
```

Also: `make test` (host tests of the engine and settings parser), `make ui` (rebuild `webui.h` after editing
`ui/app.html`), `make monitor`, `make ota-http` (same update through the page's `/update` endpoint), `make clean`.

If the board does not appear as a port, hold **BOOT** while plugging it in, then `make ports` again.
The Settings > Device page can also upload a `.bin` (`build/beacon-timer.ino.bin`) and reboot the board.

## Wiring (defaults, common anode)

| LED   | R  | G  | B  | anode (driven HIGH) |
|-------|----|----|----|---------------------|
| Timer | 13 | 11 | 10 | 12                  |
| Set   | 2  | 4  | 5  | 3                   |

The R/G/B pins sink current, so the LED is lit when the pin is LOW (the sketch inverts the PWM for you). Pins, the
common pin and anode/cathode polarity can all be changed in Settings > Device and are applied immediately. Choose
"—" for the common pin if it is wired straight to 3V3 or GND. Use a series resistor per color, and keep the current
through one GPIO anode pin under about 20 mA (or switch the anode with a transistor). Allowed GPIOs: 1–18 and 21
(not 0, the USB pins 19/20, or the flash pins).

## How it fits together

| File                  | What it is                                                                                  |
|-----------------------|---------------------------------------------------------------------------------------------|
| `beacon-timer.ino`    | Soft AP + DNS captive portal, web server, ArduinoOTA, HTTP update, LED PWM, settings in flash |
| `engine.h`            | The timing engine: session plan (sets x repeats), accelerating pulse, transition, set colors, timer states |
| `json.h`, `config.h`  | Small JSON reader and the settings parser (no external libraries)                            |
| `ui/app.html`         | The web page (same file as the Claude artifact); `tools/embed_ui.py` turns it into `webui.h` |
| `test/host_test.cpp`  | PC tests for the engine and parser                                                           |

The LED task runs at 250 Hz on its own core, so the LEDs stay smooth while the web server is busy. Settings are saved
to flash two seconds after the last change.

### HTTP API

| Route                     | Use                                                                     |
|---------------------------|-------------------------------------------------------------------------|
| `GET /api/state`          | `{app, ver, s, t, total, rev, ota, up}` — `s` is idle / run / pause / preview / done / stopped |
| `POST /api/cmd?c=start&t=` | `c` = start (optional `t` seconds), pause, stop, reset, seek (needs `t`) |
| `GET/POST /api/config`    | The settings JSON (POST body is plain text); `rev` changes when it does |
| `POST /api/reboot`        | Restart                                                                 |
| `POST /update`            | Multipart firmware upload (field `firmware`), reboots when done         |

## Session layout

Lead-in once, then the run of **Sets** (default 5), played **Repeat** times (default 4; 1 = no repeat). Each set is
Load, then the red Hold. A Transition separates sets and also leads from the last set of one repeat into the first set
of the next. "Final transition" only decides whether the very last set ends with a transition. With the defaults the
whole session is 15 s + 4 x 5 x (30 + 2 + 15) s - 15 s = 15:40.

## Load start and home-screen app

- The first 3 s of every Load window are a solid light in the first color (green), then it fades out over 1 s into the
  pulse train; the pulses keep running on the window clock. Both times are in Settings > Load ("Start of window"; 0 = off).
- Home-screen app: join **Beacon-timer**, open `http://192.168.4.1` in **Safari** (not the captive-portal pop-up), tap
  Share > Add to Home Screen. The icon (`icons/`, source `icon.svg`) is served by the board and opens full screen.
  Rebuild `icons.h` with `make icons.h` after changing the PNGs.

## Notes

- Flashing faster than about 3 Hz can affect people with photosensitive epilepsy; "Max Hz" in Settings > LEDs caps the rate (default 7).
- During any firmware update the Timer LED breathes cyan and the Set LED is off.
- The AP is open by design (no password). Anyone in range can change the timer or flash firmware.
