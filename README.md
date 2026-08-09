# DJI Spark Battery Recovery: Arduino Nano port

Recovers DJI Spark batteries stuck in **Permanent Fail** mode (the battery will not charge and the LEDs flash a fault pattern) using a classic Arduino Nano. A port of [Lishen99/DJI-Spark-Battery-Recovery-ESP32](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32) from ESP32 to ATmega328P.

**New to Arduino or electronics? Start with [HOWTO.md](HOWTO.md)**: a beginner step-by-step guide (wiring, software install, recovery, troubleshooting).

> **Safety:** lithium batteries can catch fire. Do not attempt recovery on a swollen pack, never connect the 9V boost to the Arduino, and supervise the first charge on a fireproof surface. Full safety notes in [HOWTO.md](HOWTO.md#safety).

<!-- TODO: replace with a real photo of the Nano wired to a Spark battery -->
![Arduino Nano wired to a DJI Spark battery](docs/setup-photo.jpg)

## Quick start

1. Wire battery Pin 6 → Nano A4 (SDA), Pin 1 → A5 (SCL), Pin 2 → GND, with a 4.7 kΩ pull-up from each data line to 5V.
2. Upload the sketch (board: Arduino Nano) and open the Serial Monitor at 115200 baud.
3. If the pack is completely dead, hold a 9V battery to Pin 3 (+) and Pin 2 (−) to wake it.
4. Press `A`. Keep the 9V held until the reset completes, then charge on the official DJI charger.

**Target board:** "Nano 3.0 compatible" clone (USB-C connector, CH340 USB-serial chip, ATmega328P @ 16 MHz, bootloader pre-installed). In the Arduino IDE select board **Arduino Nano**; try processor **ATmega328P** first and fall back to **ATmega328P (Old Bootloader)** if upload fails. Any ATmega328P-based Nano or Uno works. Very old Nano 2.x boards (ATmega168) do not have enough flash for this sketch, and the Nano Every is not supported (the wiring self-test uses ATmega328P registers directly).

## Folder layout

| Path | What it is |
|---|---|
| `dji_spark_battery_recovery_nano/` | The Arduino sketch (open the `.ino` in the Arduino IDE) |
| `HOWTO.md` | Beginner guide: parts list, wiring, upload, recovery |

## What changed in the port

The battery protocol (unseal key `0xCCDF7EE0`, PF-clear commands, register map for the BQ40Z307 chip) is identical to upstream. Hardware-specific changes:

- **Pins:** ESP32 GPIO21/22 → Nano **A4 (SDA) / A5 (SCL)**, the fixed hardware I2C pins on the ATmega328P. Pull-ups go to **5V** instead of 3.3V (acceptable: I2C is open-drain and the BQ chip's SMBus pins are 5V-tolerant).
- **`Serial.printf` → `Serial.print`:** the AVR core has no `printf` on Serial.
- **I2C buffer:** AVR `Wire` is limited to 32 bytes per transfer; block reads are capped accordingly (upstream requested up to 37).
- **Bus timeout:** ESP32's `Wire.setTimeOut(6000)` → AVR `Wire.setWireTimeout()`, kept short (25 ms) for responsive bus scans and raised to 6 s only around the PF-clear/reset commands, where the chip legitimately clock-stretches during flash writes. Timeouts on those commands are reported as expected chip-busy behaviour, matching upstream.

Additions beyond upstream:

- **`H` health screen** (idea from [davext/unbrick-dji](https://github.com/davext/unbrick-dji)): DJI serial number, manufacture date, cycle count, full-charge vs design capacity, state of health, per-cell voltages and cell spread. These are the figures that indicate whether a recovered pack is still safe to fly.
- **`W` wiring self-test:** verifies pull-ups and checks for shorted data lines, with no battery connected.
- **`T` bus stress test:** 300 rapid reads with an error count, used as a go/no-go check on connection quality before writing to the chip.
- **Bug fixes vs upstream:** (1) BatteryStatus was read from register `0x19`, which per the SBS spec is DesignVoltage; the flags line was decoding the design voltage as fault bits. Now reads `0x16` (BatteryStatus). (2) The flag decode used wrong bit positions/labels (e.g. printed "FullyCharged" for the Discharging bit, and took the error code from bits 12–15 instead of 0–3); now matches the SBS BatteryStatus bit map.

Verified to compile with `arduino-cli` against `arduino:avr:nano`: 55% of flash, 21% of RAM. The health screen and the wiring/bus-test tools account for most of the growth over the bare port; there is still comfortable headroom on a 328P.

## License

MIT, same as upstream. See [LICENSE](LICENSE) for both copyright lines (original ESP32 version and this port).
