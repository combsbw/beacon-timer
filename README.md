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
| `beacon-timer.ino`    | Soft AP + DNS captive portal, web server, home Wi-Fi, ArduinoOTA, HTTP update, LED PWM, settings in flash |
| `tlshttps.cpp/.h`     | https://beacon-timer.local: own certificate authority, TLS server on 443 (direct answers, relay for the rest) |
| `engine.h`            | The timing engine: session plan (sets x repeats), accelerating pulse, transition, set colors, timer states |
| `json.h`, `config.h`  | Small JSON reader and the settings parser (no external libraries)                            |
| `ui/app.html`         | The web page (same file as the Claude artifact); `tools/embed_ui.py` turns it into `webui.h` |
| `test/host_test.cpp`  | PC tests for the engine and parser                                                           |

The LED task runs at 250 Hz on its own core, so the LEDs stay smooth while the web server is busy. Settings are saved
to flash two seconds after the last change.

### HTTP API

| Route                     | Use                                                                     |
|---------------------------|-------------------------------------------------------------------------|
| `GET /api/state`          | `{app, ver, s, t, total, rev, ota, up, sleep, net, fail, rst, bo, heap, rssi}` — `s` is idle / run / pause / preview / done / stopped; `rst` is the last reset reason, `bo` the brown-out count, `heap` free RAM, `rssi` the home-Wi-Fi signal |
| `POST /api/cmd?c=start&t=` | `c` = start (optional `t` seconds), pause, stop, reset, seek (needs `t`) |
| `GET/POST /api/config`    | The settings JSON (POST body is plain text); `rev` changes when it does |
| `GET/POST /api/presets`   | The saved-timer list (one JSON array, up to 32 kB), kept in LittleFS      |
| `POST /api/reboot`        | Restart                                                                 |
| `GET/POST /api/net`       | Home Wi-Fi vs access point. GET does not return the password. POST `mode=sta` or `mode=ap` (optional `ssid`, `pass`) and the board reboots |
| `POST /api/wake`          | Leave modem sleep (home Wi-Fi only)                                     |
| `POST /api/power`         | `tx` = 0 full, 1 reduced (default), 2 low radio power, applied at once; `clear` resets the brown-out count. Returns the `/api/net` JSON |
| `POST /api/sleep`         | Modem sleep now, if no session is running or paused                     |
| `POST /update`            | Multipart firmware upload (field `firmware`), reboots when done         |

## Session layout

Lead-in once, then the run of **Sets** (default 5), played **Repeat** times (default 4; 1 = no repeat). Each set is
Load, then the red Hold. A Transition separates sets and also leads from the last set of one repeat into the first set
of the next. "Final transition" only decides whether the very last set ends with a transition. With the defaults the
whole session is 15 s + 4 x 5 x (30 + 2 + 15) s - 15 s = 15:40.

## The page on a phone

The timer in the page runs on the phone either way. The board is the LEDs, and the shared copy of settings and saved timers.

- **One icon that also runs the board.** Put the board on home Wi-Fi (below), then in Safari open `http://beacon-timer.local`. Settings > Device > **One icon** downloads the certificate. Install that profile, then turn it on under Settings > General > About > Certificate Trust Settings. Open `https://beacon-timer.local`, tap Share > Add to Home Screen, and open the new icon once while the board is on. After that the icon opens with the board off or asleep, and connects when the board is on the same Wi-Fi. Delete any older Beacon icons.
- **Offline iPhone icon with no board.** The site in `docs/` is the timer alone. With the repository public, set GitHub Pages to deploy from the `main` branch, folder `/docs`. The address is `https://combsbw.github.io/beacon-timer/`. That icon never connects to the board. To publish a new timer page, run `make pages`, push, make the repository public long enough to open the icon once, then it can be private again.
- **Soft AP.** Join **Beacon-timer** and open `http://192.168.4.1`. That page can edit the board, and it only loads while the phone is on that network. Rebuild `icons.h` with `make icons.h` after changing the PNGs.

## Home Wi-Fi, sleep, and the access point

Settings > Device > **Network**. The Wi-Fi name and password are the ones under **Online update** (2.4 GHz only). Turn on **Home Wi-Fi** and tap **Reboot** twice. The board joins that network, serves this same page at `http://beacon-timer.local` and, after the certificate above is trusted, at `https://beacon-timer.local`. The address is shown in Device.

On that network the board stays fully awake while a session is running or paused. Five minutes after the session stops, or when **Sleep** is tapped, the LEDs go off and the Wi-Fi radio goes into light modem sleep: it wakes for every router beacon that announces broadcast traffic, so the name `beacon-timer.local` still resolves and the board still answers. **Wake** (or **Asleep** in the header, or starting a session) brings the LEDs back. The phone has to be on that same network.

### Finding the board

A page served by the board (its names or a private address) looks for the board until it finds it: every few seconds, and at once when the page comes back to the front or the phone's network returns. The header shows **Connect** while it has not found it and **Reconnect** if the board stopped answering; tapping either (or the **Connect** button in Settings > Device > Network) sends a wake and a status request right away. Each lookup is started again after 0.7 s (2.4 s over https) if the first try has not answered, so one lost packet no longer costs seconds. A page from anywhere else (GitHub Pages, the Claude artifact) never searches and shows **Standalone**.

On the board: the join picks the strongest access point with the saved name, retries once, and is watched afterwards (the board asks to reconnect if the link stays down). Every 90 s, and after each reconnect, it announces `beacon-timer.local` again. A connection that has sent nothing is dropped after 0.4 s when another is waiting; Safari opens spare ones, and each used to stall the page for 5 s. Over https the page, icons, manifest, service worker and status poll are answered directly by the TLS server; only other calls are relayed to port 80.

### Power and battery

The board runs at 160 MHz and starts with **Radio power: Reduced** (13 dBm), which lowers the supply current peaks of the radio. Settings > Device > Network > **Radio power** changes it at once (Full 19.5 dBm, Reduced 13, Low 8.5); use Full if the board is far from the router. Device also shows the last reset reason, the signal strength and free memory. If the board resets because the supply dipped (**brown-out**), the count and a note appear there. On a lithium cell that points to a low battery, or a regulator/diode path that cannot deliver the Wi-Fi bursts; try a charged cell, Radio power Low, or a larger capacitor (100 µF or more) across 3V3 and GND.

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
- **Final stretch of a long load** (Settings > Load > Final stretch). *Blink ramp* (default) keeps the slow pulse going into the last
  *Final* seconds and speeds it up slightly (about 0.25 to 0.6 Hz with the default cycle) until *Blink* seconds (15) are left. Then it
  becomes a soft-edged blink with a longer on-time (about 65 %) whose rate climbs to *Blink to* (3 Hz), capped by Max Hz. It is one
  continuous waveform, so nothing jumps at the hand-over. The colors in the final stretch still start again at the first color.
  *Pulse train* is the earlier behavior: the normal accelerating pulse sequence. Loads of 30 s or less are not affected.

## Saved timers

Settings > **Saved timers**: type a name and tap **Save** to store the current timing, names and light settings. Tap a
saved timer to load it. Its name then shows on the timer screen. Changing the loaded timer marks it **Edited** and leaves
the stored copy as it was until you tap **Save** again (with the name field empty) or the update button on that row.
**Clear timer** unmounts it from the current screen and does not delete the stored copy. Up to 20 are kept. Pins, brightness limits and the hide-LEDs switch are not part of a saved
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
