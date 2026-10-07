# DJI Spark battery recovery with an Arduino Nano

Brings back DJI Spark batteries that are stuck in **Permanent Fail**: the charger ignores them and the lights blink an error. The chip inside has switched the battery off after it ran down too far. A £3 Arduino Nano can clear the lock and, if the cells are very low, nudge them back up until the lock stays off.

**Start with [HOWTO.md](HOWTO.md).** It is written for someone who has never used an Arduino.

> **Safety:** lithium batteries can catch fire. Never recover a swollen pack. Never connect the 9V to the Arduino. Supervise the first charge on a fireproof surface.

## In short

1. Nano + two 4.7 kΩ resistors + breadboard. Spark pin 6 → A4, pin 1 → A5, pin 2 → GND.
2. Upload the sketch, open the Serial Monitor at 115200, press `W` to test the wiring before the battery goes on.
3. Hold a 9V battery across Spark pins 3 (+) and 2 (−) to wake the chip.
4. Press `1` and `H` to read the state. Then `A` to clear, or `K` to pump if a cell is under 2.2 V.
5. Charge to full on the DJI charger, supervised. Press `H` afterwards to judge the pack.

Works with any ATmega328P Nano or Uno, including clones. Not the Nano Every or the old ATmega168 Nano.

## Files

| Path | What |
|---|---|
| `dji_spark_battery_recovery_nano/` | The Arduino sketch |
| `HOWTO.md` | The guide |

## Where this came from

The sketch started as a port of [Lishen99's ESP32 version](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32) to the Nano. In October 2026 it was checked against the TI bq40z50 reference manual and [dvdsosa/dji-spark-battery-unbrick](https://github.com/dvdsosa/dji-spark-battery-unbrick), and three inherited protocol faults were fixed:

- Status was read through a register path DJI packs refuse, so the lock state never showed. It now reads through ManufacturerAccess (0x00) and ManufacturerData (0x23), which works even when sealed.
- The security level was decoded from the wrong bits with the wrong labels.
- The "clear" sent two unrelated commands (black-box reset and LED toggle) alongside the real one. Only PermanentFailDataReset (0x0029) is sent now, and the result is checked.

Added since: a wiring self-test, a connection stress test, a cell-health screen, a check for the lock coming back after reset, and the `K` pump mode for cells below the threshold. Two real packs were recovered with it on 7 October 2026; details are in the HOWTO.

Compiles with `arduino-cli` for `arduino:avr:nano`: 70% of flash, 21% of RAM.

## License

MIT. See [LICENSE](LICENSE).
