# Beacon Timer

Interval timer with two RGB LEDs (Timer and Set) for the **ESP32-S3 Super Mini**. The timer runs on the board. A phone
joins the open Wi-Fi network **Beacon-timer** and opens `http://192.168.4.1` (a captive portal does it automatically),
or, after Settings > Device > Network is set to home Wi-Fi, opens `http://beacon-timer.local` on that network. The page
is the control panel: Start / Pause / Stop / Reset, every setting, and firmware update. The same page is also a timer
on its own, with no board connected.

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
| `GET/POST /api/presets`   | The saved-timer list (one JSON array, up to 32 kB), kept in LittleFS      |
| `POST /api/reboot`        | Restart                                                                 |
| `GET/POST /api/net`       | Home Wi-Fi vs access point. GET does not return the password. POST `mode=sta` or `mode=ap` (optional `ssid`, `pass`) and the board reboots |
| `POST /api/wake`          | Leave modem sleep (home Wi-Fi only)                                     |
| `POST /api/sleep`         | Modem sleep now, if no session is running or paused                     |
| `POST /update`            | Multipart firmware upload (field `firmware`), reboots when done         |

## Session layout

Lead-in once, then the run of **Sets** (default 5), played **Repeat** times (default 4; 1 = no repeat). Each set is
Load, then the red Hold. A Transition separates sets and also leads from the last set of one repeat into the first set
of the next. "Final transition" only decides whether the very last set ends with a transition. With the defaults the
whole session is 15 s + 4 x 5 x (30 + 2 + 15) s - 15 s = 15:40.

## The page on a phone

The timer in the page runs on the phone either way. The board is the LEDs, and the shared copy of settings and saved timers.

- **Offline iPhone icon.** The site in `docs/` is the timer with a service worker. With the repository public, set GitHub Pages to deploy from the `main` branch, folder `/docs`. The address is `https://combsbw.github.io/beacon-timer/`. In Safari, open that address, tap Share > Add to Home Screen, then open the new icon once while the site is still up. After that the timer works with no computer, no board, and no network. The phone keeps its copy if the repository is made private again. To publish a new timer page, run `make pages`, push, make the repository public long enough to open the icon once, then it can be private again.
- **On the board.** Join **Beacon-timer** and open `http://192.168.4.1`, or in home Wi-Fi mode open `http://beacon-timer.local`. That copy can wake the board and edit the shared settings. An icon saved from the board only loads while the phone is on the board's network. Rebuild `icons.h` with `make icons.h` after changing the PNGs.

## Home Wi-Fi, sleep, and the access point

Settings > Device > **Network**. The Wi-Fi name and password are the ones under **Online update** (2.4 GHz only). Turn on **Home Wi-Fi** and tap **Reboot** twice. The board joins that network, serves this same page at `http://beacon-timer.local`, and shows its address in Device.

On that network the board stays fully awake while a session is running or paused. Five minutes after the session stops, or when **Sleep** is tapped, the LEDs go off and the Wi-Fi radio sleeps between beacons. It is still associated, so **Wake** (or **Asleep** in the header, or starting a session) reaches it and brings the LEDs back. The phone has to be on that same network.

Turn **Home Wi-Fi** off and reboot to return to the **Beacon-timer** access point. That mode does not sleep.

If the network is missing or the password is wrong, that boot brings **Beacon-timer** up instead and Device says so. The saved choice is kept, so the next boot tries the home network again. Turn Home Wi-Fi off to stop trying.

## Load start

The first 3 s of every Load window are a solid light in the first color (green), then it fades out over 1 s into the
pulse train; the pulses keep running on the window clock. Both times are in Settings > Load ("Start of window"; 0 = off).

## Second LED and long loads

- **Set LED shows** (Settings > Timing): *Sets* steps the second LED through one color per set (default); *Repeats* gives
  it one color per repeat, so it stays one color for a whole pass and changes when the next repeat begins (the closing
  transition already shows the next repeat's color). The caption under the LED reads SET or REPEAT.
- **Loads up to 20 minutes.** The Load length steps in 5 s (under 2 min), 15 s (2-5 min) and 30 s (over 5 min); type a
  value for anything exact. Times of an hour or more read h:mm:ss.
- **Long-load lighting** (Settings > Load > Long loads). Only the last *Final* seconds (30 by default) run the accelerating
  pulse. Before that the light pulses slowly (*Cycle*, *Dim*), one color after another, each color getting an equal share
  of the time; the final stretch then starts again at the first color. A 10 min load with 4 colors is 4 shares of 2:22.5,
  then the last 30 s. Loads of 30 s or less behave as before. *Marks* adds an optional double flash every N seconds.

## Saved timers

Settings > **Saved timers**: type a name and tap **Save** to store the current timing, names and light settings. Tap a
saved timer to load it; the buttons on each row update it with the current settings, rename it, or delete it (update and
delete ask for a second tap). Up to 20 are kept. Pins, brightness limits and the hide-LEDs switch are not part of a saved
timer, so loading one never changes the wiring settings. On the board the list is stored in its flash (LittleFS), so every
phone sees the same list; in a plain browser it is kept in that browser.

## Online update from GitHub

Settings > Device > **Online update**: enter the Wi-Fi network the board should use for the internet (a phone hotspot works;
2.4 GHz only) and its password, tap **Save**, then **Update now** (two taps). The board:

1. leaves its own Wi-Fi and joins your network (Timer LED breathes amber, Set LED flashes once),
2. gets the internet time, needed to check GitHub's certificate (Set LED flashes twice),
3. finds the release (Set LED flashes three times),
4. downloads `beacon-timer.ota.bin` from the repo's **Latest build** release over verified HTTPS (Timer LED breathes cyan,
   Set LED is a bar from red through yellow to green with a quick dip each second),
5. installs it, flashes green, and restarts into whichever mode was saved (home Wi-Fi, or the Beacon-timer access point).

If it fails, the Timer LED blinks red and the Set LED flashes the error number (1 Wi-Fi, 2 clock, 3 GitHub, 4 download,
5 flash write), then the board restarts on the old firmware, again in the saved mode. The page shows the result when you
reconnect. The update is refused while a timer is running. The repository defaults to `combsbw/beacon-timer`; for a private repository
put a read-only token under *GitHub source* (the token is sent only to api.github.com).

The Wi-Fi name and password are also what **Home Wi-Fi** mode joins. The password and token are stored in the board's flash as plain text and are never sent back to the page. The Beacon-timer
Wi-Fi is open, so anyone in range could change them or start an update, as they could already upload firmware. On a home network the same page is reachable by anyone on that network.

## Notes

- Flashing faster than about 3 Hz can affect people with photosensitive epilepsy; "Max Hz" in Settings > LEDs caps the rate (default 7).
- During any firmware update the Timer LED breathes cyan and the Set LED is off.
- The AP is open by design (no password). Anyone in range can change the timer or flash firmware.
