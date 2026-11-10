# How to resume: nRF52832 fob bring-up

Status notes for picking this work up later. Last updated 2026-11-10.

## Where things stand

- **The protocol is verified on current hardware.** The ESP32 port (`esp32/`, M5Stack Atom Lite) compiles the
  unmodified `src/nuki*.c`, `src/utils.c` and `external/`. With it, pairing and Fob Actions 1–3 work against a
  **Nuki Smart Lock 3.0**, and the pairing survives reboots and power loss.
- **ESP32 speed-ups are measured.** Fob Action 2 took about 1.06 s from the tap decision to the Nuki's reply:
  - lock found 256 ms *before* the decision, thanks to the early scan;
  - connected at +773 ms, with a 15 ms interval accepted;
  - link ready at +797 ms, using the cached handles;
  - Nuki replied at +1062 ms.

  Connecting is now the slowest step, because it has to wait for the Nuki's next advertisement.
- **The nRF firmware (`src/`) is unchanged.** The ESP32 speed-ups have not been ported to it yet; see "Next steps".

## Hardware to get

| Item | Where | Notes |
|---|---|---|
| nRF52832 beacon (DUOWEISI = Radioland, KX022 + SHT30) | [AliExpress item 32965114958](https://www.aliexpress.com/i/32965114958.html) | ~US$7, price and shipping not verified. **Must say nRF52832**: nRF52810/52811 can't run this firmware, because their Bluetooth stack can't act as central. Comes without a battery (CR2032). |
| Backup beacon (Taidacent nRF52832) | [AliExpress item 4000349635081](https://www.aliexpress.com/i/4000349635081.html) | Pinout unknown. |
| SWD probe | A Raspberry Pi Pico you already have + [debugprobe firmware](https://github.com/raspberrypi/debugprobe/releases) | Can also unlock a read-locked chip (CTRL-AP), which a plain ST-Link V2 can't. |
| Wiring | Breadboard and jumpers you already have | Pads are small: solder, or use pogo clips. |

Rejected: Holyiot keychain beacons (nRF52810), and Amazon.de beacon tags (nRF51822). No German or EU shop
sells finished nRF52832 beacons.

## Pinouts

| Board | Button | LED | LED on | `NRF_DFU_HW_VERSION` |
|---|---|---|---|---|
| Old revision / DUOWEISI sensor beacon (matches `build/*.hex`) | P0.25 | P0.26 | 1 (active high) | 51 |
| New revision (see `Hardware.txt`) | P0.11 | P0.30 | 0 (active low) | 52 |

The pin defines are at `src/main.c:69-72`. DUOWEISI beacon SWD pads: SWDIO = TP1, SWCLK = TP2, plus GND and VCC.
Sources: [an-erd/ble_beacon](https://github.com/an-erd/ble_beacon),
[aeremin/greenlight_button](https://github.com/aeremin/greenlight_button) (has a reflashing guide) and
[parasquid/mcu-iot-sensors#9](https://github.com/parasquid/mcu-iot-sensors/issues/9).

The firmware uses the internal RC low-frequency clock (`NRF_SDH_CLOCK_LF_SRC 0`), so it does not need a 32 kHz
crystal on the beacon.

## Flashing with the Pico

1. **Make the Pico a probe.** Hold BOOTSEL, plug the Pico in, and drag the matching `debugprobe_*.uf2` onto the
   drive that appears.
2. **Wire it.** Take the coin cell out of the beacon first; never power it from both.

   | Pico | Beacon |
   |---|---|
   | GP2 (pin 4) | SWCLK |
   | GP3 (pin 5) | SWDIO |
   | GND (pin 3) | GND |
   | 3V3 OUT (pin 36) | VCC |
3. **Install OpenOCD:** `brew install open-ocd`.
4. **Change the repo config. This is TODO and not done yet:**
   - `config/openocd.cfg` targets an ST-Link. For the Pico, switch it to
     `source [find interface/cmsis-dap.cfg]`, `transport select swd` and `adapter speed 1000`.
   - The `Makefile` hardcodes `OPENOCD_HOME=/usr/local/Cellar/open-ocd/0.11.0/`, which is Intel Homebrew.
     Point it at the Apple Silicon Homebrew install instead.
5. **Unlock if needed, then flash the full image** (MBR + SoftDevice S132 + bootloader + app).
   No SDK is needed, because the image is prebuilt:
   - If the chip is read-locked, run OpenOCD's `nrf52_recover`. It erases the chip and unlocks it.
   - Then program `build/nrf52832_xxaa_dfu_s132.hex` with `verify` and reset.
   - Flashing only the app hex won't boot.
6. **Pair and test.** Follow "Usage" in `README.md`. Firmware updates after this can go over the air (DFU mode,
   nRF Toolbox or nRF Connect).

## Next steps

- [ ] Order the beacons. Find the Pico and flash the debugprobe firmware.
- [ ] Switch `config/openocd.cfg` to CMSIS-DAP and fix `OPENOCD_HOME` in the `Makefile`.
- [ ] Flash a beacon with the prebuilt image, then pair it and test Fob Actions 1–3.
- [ ] Port the ESP32 speed-ups to the nRF firmware. Handle caching and early scanning already exist there.
  - In `input_timer_handler` (`src/main.c`): decide 300 ms after the last release instead of using a fixed 2 s window.
  - Change the connection interval from 20–200 ms to 7.5–15 ms (`MIN/MAX_CONNECTION_INTERVAL`).
- [ ] Optional: decrypt and check the Nuki's reply, so a rejected command (e.g. during Lock 'n' Go) shows red
      instead of green. Today `command_finished` in `src/nuki_unlock.c` only checks that a reply arrived.
- [ ] Optional (ESP32): keep the last 5–10 timings instead of only the last one, to catch occasional slow taps.
- [ ] Known bug: `src/fob_data.c:200` reads the backup data from `FOB_DATA_FLASH_ADDRESS` instead of
      `FOB_DATA_BACKUP_FLASH_ADDRESS`, so the backup copy is never used.

## ESP32 test setup notes

- Build and flash: `cd esp32 && pio run -t upload`. On Apple Silicon, `platformio.ini` pins `tool-cmake @ 3.30.2`
  because the bundled CMake is x86-only.
- The boot log prints the pairing and the last operation's timing (`pio device monitor`, 115200 baud).
- To erase the pairing: `pio run -t erase`, then reflash and remove the fob in the Nuki app.
