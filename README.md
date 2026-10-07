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
4. Press `1` first and check the `Lowest cell` / `PF status` lines, then press `A`. Keep the 9V held until the reset completes, then charge on the official DJI charger.
5. If the sketch reports `PF RE-LATCHED within 5 s of reset`, a cell is below the chip's ~2.2 V undervoltage threshold and no clear will hold until it rises. `K` (pump) is the way through that; read [HOWTO.md](HOWTO.md#if-pf-re-latches-after-reset) first.

**Target board:** "Nano 3.0 compatible" clone (USB-C connector, CH340 USB-serial chip, ATmega328P @ 16 MHz, bootloader pre-installed). In the Arduino IDE select board **Arduino Nano**; try processor **ATmega328P** first and fall back to **ATmega328P (Old Bootloader)** if upload fails. Any ATmega328P-based Nano or Uno works. Very old Nano 2.x boards (ATmega168) do not have enough flash for this sketch, and the Nano Every is not supported (the wiring self-test uses ATmega328P registers directly).

## Folder layout

| Path | What it is |
|---|---|
| `dji_spark_battery_recovery_nano/` | The Arduino sketch (open the `.ino` in the Arduino IDE) |
| `HOWTO.md` | Beginner guide: parts list, wiring, upload, recovery |

## What changed in the port

The unseal key (`0xCCDF7EE0`) and register map are as upstream. Three protocol faults inherited from upstream were fixed in October 2026 after comparing against the TI bq40z50-R2 technical reference manual (SLUUBK0) and [dvdsosa/dji-spark-battery-unbrick](https://github.com/dvdsosa/dji-spark-battery-unbrick):

- **Status reads used the wrong register.** Subcommands went to ManufacturerBlockAccess (0x44) and the reply was read from 0x44. DJI packs refuse that path, which is why the Seal/Safety/PF status lines never printed. Status subcommands are now written to ManufacturerAccess (0x00) and the reply block-read from ManufacturerData (0x23). This works even on a sealed chip.
- **Security level was decoded from the wrong bits with the wrong labels.** SEC1:SEC0 are bits 9:8 of the 32-bit OperationStatus: 1 = Full Access, 2 = Unsealed, 3 = Sealed. The old decode read bits 2:1 of the first byte and labelled 0/1 as Full Access/Unsealed.
- **"PF clear" sent two unrelated commands.** 0x002A is BlackBoxRecorderReset and 0x002B toggles the LEDs. Only 0x0029 (PermanentFailDataReset) clears PF, and it is now the only command sent, with the result verified after each of up to five attempts.

Consequence for earlier recoveries: a pack that "failed" under the old sketch may simply have had its state misreported, or may have a cell below the undervoltage threshold so that PF re-latched seconds after the reset. The sketch now checks for that re-latch after every reset and says so.

Hardware-specific changes:

- **Pins:** ESP32 GPIO21/22 → Nano **A4 (SDA) / A5 (SCL)**, the fixed hardware I2C pins on the ATmega328P. Pull-ups go to **5V** instead of 3.3V (acceptable: I2C is open-drain and the BQ chip's SMBus pins are 5V-tolerant).
- **`Serial.printf` → `Serial.print`:** the AVR core has no `printf` on Serial.
- **I2C buffer:** AVR `Wire` is limited to 32 bytes per transfer; block reads are capped accordingly (upstream requested up to 37).
- **Bus timeout:** ESP32's `Wire.setTimeOut(6000)` → AVR `Wire.setWireTimeout()`, kept short (25 ms) for responsive bus scans and raised to 6 s only around the PF-clear/reset commands, where the chip legitimately clock-stretches during flash writes. Timeouts on those commands are reported as expected chip-busy behaviour, matching upstream.

Additions beyond upstream:

- **`H` health screen** (idea from [davext/unbrick-dji](https://github.com/davext/unbrick-dji)): DJI serial number, manufacture date, cycle count, full-charge vs design capacity, state of health, per-cell voltages and cell spread. These are the figures that indicate whether a recovered pack is still safe to fly.
- **`W` wiring self-test:** verifies pull-ups and checks for shorted data lines, with no battery connected.
- **`T` bus stress test:** 300 rapid reads with an error count, used as a go/no-go check on connection quality before writing to the chip.
- **`D` CSV status line:** read-only, same column layout as dvdsosa's `monitor_charge.sh`, so that live precharge monitor can be used with this sketch.
- **Re-latch check after reset:** `R` waits 5 s after the chip restarts and reports whether PF came back. That distinguishes a pack whose cells are too low from a failed unlock.
- **`K` pump mode:** for packs with a cell below ~2.2 V, repeats unseal → clear → reset so the chip precharges in the 2–3 s before each re-latch, until PF stays clear. Procedure from dvdsosa; confirmation prompt, round limit, temperature cutoff and key-press abort built in. Read the safety notes in [HOWTO.md](HOWTO.md#if-pf-re-latches-after-reset) first.
- **Bug fixes vs upstream:** (1) BatteryStatus was read from register `0x19`, which per the SBS spec is DesignVoltage; the flags line was decoding the design voltage as fault bits. Now reads `0x16` (BatteryStatus). (2) The flag decode used wrong bit positions/labels (e.g. printed "FullyCharged" for the Discharging bit, and took the error code from bits 12–15 instead of 0–3); now matches the SBS BatteryStatus bit map.

Verified to compile with `arduino-cli` against `arduino:avr:nano`: 70% of flash, 21% of RAM. The health screen and the wiring/bus-test tools account for most of the growth over the bare port; there is still comfortable headroom on a 328P.

## License

MIT, same as upstream. See [LICENSE](LICENSE) for both copyright lines (original ESP32 version and this port).
