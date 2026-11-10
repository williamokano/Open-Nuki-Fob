# ESP32 test harness (M5Stack Atom Lite)

Runs the **unmodified** Nuki protocol code of this repo (`../src/nuki*.c`, `../src/utils.c`, `../external/`)
on an ESP32 so pairing and Fob Actions can be tested against a real Nuki without the nRF52832 beacon.
Only the platform layer is ESP32-specific: NimBLE central (`ble_central.c`), transport (`bt_comm_esp32.c`),
NVS storage (`fob_store.c`), button/LED (`board.c`) and two shim headers (`main/shim/`) that map the
nRF logger and RNG used by the shared sources.

Differences from the nRF fob: single pairing, no DFU, no deep sleep (returns to idle so you can
test repeatedly on USB power). For a faster response it decides the number of presses 300 ms after
the last release (instead of a fixed 2 s), starts connecting to the lock on the first press, asks
for a 7.5–15 ms connection interval, and caches the GATT handles (rediscovering once if they fail).
Each operation's per-phase timing is logged and kept in NVS; it is printed at the next boot.

## Build & flash

Requires PlatformIO (`pip install platformio`).

```sh
cd esp32
pio run                      # build
pio run -t upload            # flash (Atom Lite on USB)
pio device monitor           # serial log, 115200 baud
```

## Usage

| Input on the Atom button       | Action                                   |
|--------------------------------|------------------------------------------|
| hold ≥ 5 s (LED turns purple), release | Pair with a Nuki in pairing mode |
| 1 / 2 / 3 presses              | Send Fob Action 1 / 2 / 3                 |

Use the **big button on the top** (GPIO39). The small side button is reset.

LED: short white flash = booted, purple = pairing, blue blink = action in progress, green = success, red = failure.

1. Put the Nuki in pairing mode (hold its button ~5 s until the ring stays lit).
2. Hold the Atom button ≥ 5 s and release. The log should end with `SUCCESS: paired`, and
   "Open Nuki Fob XXXXXXXX" appears in the Nuki app under users.
3. In the Nuki app, configure Fob Actions 1–3 for that fob (e.g. unlock / lock / unlatch).
4. Press once / twice / three times → the lock performs the action; log ends with `SUCCESS: lock action sent`.

The pairing is stored in NVS and survives reboots. To start over, erase flash: `pio run -t erase`
(then flash again) and remove the fob from the Nuki app.
