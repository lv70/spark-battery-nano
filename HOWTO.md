# Recovering a DJI Spark battery with an Arduino Nano

A step-by-step guide for beginners. No soldering is required if you have jumper wires that fit the battery connector.

---

## Background

Every DJI Spark battery contains a battery management chip (a Texas Instruments BQ40Z307) whose job is to protect the battery cells. If the battery is left unused for long enough, the cells discharge below the chip's safety threshold. The chip then sets a **Permanent Fail** flag and disconnects the battery: the charger will not charge it, the drone will not see it, and the LEDs blink an error pattern.

Despite the name, the flag is a setting in the chip's memory, not physical damage. The chip has a service interface (two data wires) through which the flag can be cleared and the chip restarted. That is what the Arduino sketch does. DJI's repair centres perform the same operation with a dedicated USB adapter; the Nano replaces that adapter.

---

## Safety

- **Lithium batteries can catch fire.** A battery that sat deeply discharged for years may have damaged cells. After recovery, do the **first full charge outdoors or on a fireproof surface, and stay nearby.** If the battery gets hot, swells, or gives off a sweet or chemical smell, stop and dispose of it at a battery recycling point.
- **If the battery pack is swollen, do not attempt recovery. Recycle it.** In that case the cells are the problem, not the flag.
- **Never connect the 9V battery to the Arduino.** The 9V only ever touches the Spark battery's power pins (Pins 3 and 2). 9V into an Arduino data pin destroys the Arduino.
- **Do not short pins.** Pin 3/4 (battery +) touching Pin 1 or 6 (data) can damage the chip you are trying to talk to. Work slowly and double-check before powering anything.
- This procedure voids any warranty and is at your own risk. A recovered battery is a repaired battery: treat it with more suspicion than a new one, and do not fly over people with it.

---

## Parts list

| Item | Notes | Rough cost |
|---|---|---|
| Arduino Nano | The classic ATmega328P version. Clones are fine. An Arduino **Uno** also works: same pins (A4/A5), same sketch. | £3–20 |
| USB cable for the Nano | Older clones use **mini-USB**; newer ones use **USB-C**. Must be a *data* cable, not a charge-only one | £2 |
| 2 × 4.7 kΩ resistors | Any wattage. 2.2–10 kΩ also works (two 10 kΩ twisted in parallel = 5 kΩ, also fine) | <£1 |
| Breadboard + jumper wires | "Dupont" male-to-male wires. The wires need a thin single pin to reach into the battery connector; a standard male Dupont pin fits | £5 |
| 9V battery (PP3) + battery clip | Only needed if the battery is completely dead (it usually is) | £3 |

> **Reference board:** this guide was written against a "Nano 3.0 compatible" clone (USB-C connector, CH340 USB chip, ATmega328P @ 16 MHz, bootloader pre-installed). The notes below call out where the board choice matters (USB driver and bootloader setting).

---

## Step 1: Find the battery connector pins

Look at the connector on the Spark battery (where it plugs into the drone). It has **6 pins**. Hold the battery **portrait, with the pins facing you, at the top**, and number the pins **1 to 6, left to right**:

```
 ┌─────────────────────────────┐
 │  1    2    3    4    5    6 │   ← pins at top, facing you
 └──┬────┬────┬────┬────┬────┬─┘
   SCL  GND  BAT+ BAT+ GND  SDA
```

- Pins 1 and 6 (the outer ones) are the two **data** lines (SCL and SDA)
- Pins 2 and 5 are **ground** (−)
- Pins 3 and 4 are the **battery + output** (never connect these to the Arduino)

> ### Getting a reliable connection
>
> The Spark connector is designed for a mating plug, not bare jumper pins, so contact reliability is the most common point of failure. Intermittent scans, `READ ERROR`, or a bus stress test that starts clean and then reports a burst of errors almost always trace back to the connector. A pin can touch the contact without gripping it, and the spring of the wire then levers it off a few seconds later. In order of effectiveness:
>
> 1. **Pre-bend the last ~2 mm of each jumper pin by about 15°.** Insert it with the bend facing the metal contact side of the slot, so the tip springs against the contact instead of resting on it.
> 2. **Tape the wire bundle to the battery body** a few cm back from the connector, so the weight and spring of the wires cannot twist the tips out. (This alone was sufficient on the three batteries used to develop this guide.)
> 3. **Verify with software, not by eye.** Do not judge seating by the LEDs. Run **`T`** (bus stress test) and proceed only after a clean **0-error** run with hands off the wiring. That confirms the contact will survive an unattended recovery.
> 4. **For repeated use:** a "DJI Spark battery adapter board" (a small breakout PCB that converts the connector to solder pads or header pins) removes this failure mode entirely. Inexpensive, and worthwhile beyond the first couple of packs.

## Step 2: Wire it up

The Nano's data pins for this job are fixed in hardware: **A4 and A5** (labelled on the board).

| Spark battery pin | → | Arduino Nano pin |
|---|---|---|
| Pin 6 (SDA) | → | **A4** |
| Pin 1 (SCL) | → | **A5** |
| Pin 2 (GND) | → | **GND** (any GND pin) |

Then add the two pull-up resistors (each data line must be pulled toward a voltage or communication does not work):

- One 4.7 kΩ resistor between **A4 and the Nano's 5V pin**
- One 4.7 kΩ resistor between **A5 and the Nano's 5V pin**

Easiest on a breadboard:

```
   Spark Pin 6 ──────┬────────── Nano A4
                   [4.7kΩ]
   Spark Pin 1 ──┐   │     ┌──── Nano A5
                 │   ├─────│──── Nano 5V
                 │ [4.7kΩ] │
                 └───┴─────┘
   Spark Pin 2 ───────────────── Nano GND
```

(Both resistors go from a data line to the 5V rail. Resistors are not polarized; orientation does not matter.)

**Do not connect anything to battery pins 3, 4, or 5 yet.** The Nano itself is powered by its USB cable only.

Build the Nano side first and leave the three wires loose at the battery end. Step 3 includes a self-test (`W`) that proves the resistors and rails are right before the battery is involved; plug the battery in only after it passes.

> **Why 5V is acceptable here:** the BQ40Z307 is designed for laptop-style battery buses and its data pins are rated for 5V signals. On an I2C bus nothing actively drives the lines high; only the pull-up resistors do.

## Step 3: Install the software

1. Download the **Arduino IDE** from [arduino.cc/en/software](https://www.arduino.cc/en/software) and install it.
2. Open the file `dji_spark_battery_recovery_nano/dji_spark_battery_recovery_nano.ino` in the IDE.
3. Plug the Nano into your computer via USB (USB-C data cable for the reference board).
4. In the IDE, from the menu bar: **Tools → Board → Arduino AVR Boards → Arduino Nano**.
5. In the IDE, from the menu bar: **Tools → Port**. To identify which entry is the Nano: unplug the Nano, open the Port menu and note what is already listed, plug the Nano in, and reopen the menu. The entry that just appeared is the Nano. Its name depends on the computer:
   - **Windows:** `COM3`, `COM4` or similar, often labelled "USB-SERIAL CH340"
   - **macOS:** `/dev/cu.usbserial-XXXX` or `/dev/cu.wchusbserialXXXX`
   - **Linux:** `/dev/ttyUSB0` or similar
   If no new entry appears, first suspect the cable (it must be a data cable, not a charge-only one), then install the CH340 driver for your operating system (search "CH340 driver" plus the OS name). On a recent Mac the CH340 needs no driver.
6. Click the **→ Upload** button.
   - If upload fails with `avrdude: stk500_recv()` errors, switch **Tools → Processor** between **ATmega328P** and **ATmega328P (Old Bootloader)** and try again. Clones ship with either bootloader and this setting has to match. Newer USB-C clones usually work with the plain **ATmega328P** setting; older ones need Old Bootloader. The wrong choice does no harm; the upload just fails.
7. In the IDE, from the menu bar: **Tools → Serial Monitor** and set the speed dropdown at the bottom to **115200 baud**.

You should see the welcome banner and a menu. Garbage characters mean the baud rate is wrong.

8. **Before connecting the battery, press `W`** (wiring self-test). It checks that each data line is lifted high by its pull-up resistor and that the two lines are not shorted together, with nothing attached. Expect both `SDA (A4) pull-up: OK` and `SCL (A5) pull-up: OK`. A `FAIL` on either line means that resistor is missing, in the wrong breadboard row, or on a dead part of the 5V rail (breadboard rails often have a break in the middle). Fix it and press `W` again until both pass. A bus scan can never succeed with a failed pull-up, so this one test saves a lot of poking at the battery connector.

## Step 4: Wake the dead battery (9V boost)

The chip inside the battery is itself unpowered when the battery is deeply discharged; it cannot respond until it is fed some power:

1. With the three wires from Step 2 connected, take the 9V battery with its clip.
2. Touch the **red (+) wire to battery Pin 3** and the **black (−) wire to battery Pin 2**.
3. **Hold them there.** LEDs blinking on the Spark battery indicate the chip has woken up.

The 9V must stay in place during the whole recovery (about 15 seconds), so a helper or some tape is useful. A reliable hands-free rig: seat two spare jumper pins in Pins 3 and 2 (pre-bent tips, as in Step 1), connect the 9V leads to the jumper tails with alligator clips, and tape the bundle down.

Two notes on verifying the boost:

- The `Pack voltage` line in the status screen reads the **cells**, not the 9V, so it may barely rise even when the boost is working (deeply discharged cells accept little or no current). Do not use it as the boost check.
- To verify with a multimeter: measure DC volts across battery Pins 3 and 2 at the connector. The 9V's full voltage there means the chip is being fed. A 9V that reads healthy on its own but sags badly while connected is worn out; replace it.

## Step 5: Read-only checks

Before changing anything on the battery, confirm communication works using commands that **only read**. They cannot alter the battery, so they can be repeated as often as needed:

1. If the battery is dead, hold the 9V boost (Step 4) during each check below.
2. Press **`S`** (scan). Expect: `0x0B <- DJI BMS`. This proves the wiring and pull-ups work.
3. Press **`1`** (status). Expect: a plausible pack voltage, a temperature, a `Batt flags` line, and then the chip's own view of its state. On a locked battery a typical readout is:

```
Batt flags   : 0x48C0  StopChargeAlarm  StopDischargeAlarm  Discharging
Device type  : 0x4307  (BQ9003/BQ40Z307 - OK)
OpStatus     : 0x00007300
  Security   : Sealed
  PF active  : YES  <- locked
  Charge     : disabled (XCHG)
  Discharge  : disabled (XDSG)
Safety status: 0x00000001  <- FAULTS ACTIVE
  -> CUV: cell under-voltage
PF status    : 0x00000001  <- PERMANENT FAIL (needs clearing)
  -> SUV: cell under-voltage PF (deep discharge)
```

   `Security: Sealed` plus `PF active: YES` is the expected starting point. If `OpStatus` says `unreadable`, the chip is not answering properly; fix the connection before going on.
4. Press **`H`** (health). Expect: serial number, cycle count, capacity figures and **per-cell voltages**. Write down the lowest cell. Below about **2.2 V** the chip will set PF again within seconds of any clear, and the health screen says so. Below **2.0 V** the cell may be damaged.

Only move on to Step 6 once all three respond sensibly and the lowest cell is known. If they do not, go back over Step 2 and the troubleshooting table; no command in this step can have changed anything.

> **No resistors yet?** This step can still be attempted: the Nano has weak built-in pull-ups, and over short wires (≤15 cm) reading often works with those alone. Two caveats: a failure proves nothing (it is probably the missing resistors, not the wiring), and do not run Step 6 until the real resistors are fitted. Reads on a marginal bus are harmless; writes are not worth the risk.

## Step 6: Run the recovery

1. While holding the 9V boost, click into the Serial Monitor's input box, type **`A`** and press Enter.
2. Watch the output. A successful run looks like:

```
[*] Pack voltage before: 8222 mV
[*] Lowest cell: 2710 mV
[U] Unseal
[U]   Spark key 0xCCDF7EE0 attempt 1 -> Unsealed
[P] PermanentFailDataReset (0x0029)
[P]   attempt 1 ACK  PF status 0x00000000  (cleared)
[OK] PF cleared.
[R] DeviceReset (0x0041)
[OK] BMS restarted (comes back sealed).
[OK] PF still clear 5 s after reset.
[L] Seal (0x0030)
[OK] Already sealed.
[DONE] PF clear. Plug into the DJI charger and supervise the first charge.
```

3. Keep holding the 9V until the `PF still clear 5 s after reset` line appears, then release it.
4. Disconnect everything and put the battery on the **official DJI charger**. Alternating/chasing LEDs indicate it is charging again.

Normal behaviour that can look like an error:
- A "NACK" or error on the very first unseal attempt is expected; it is a security feature of the chip.
- The voltage shown during recovery may read high (~8.2V, the 9V feeding through) or may stay near the flat cell voltage; neither indicates a problem.
- `PF still set. It may only update after reset (R)` after the P step: the sketch carries on to the reset and re-checks there. Judge by the line after the reset.
- A second round (`PF still active after reset — second round...`) is normal; the chip comes back sealed after a reset, so it unseals and clears again.
- `Already sealed` at the L step is expected, for the same reason.

## If PF re-latches after reset

If the output ends with `PF RE-LATCHED within 5 s of reset`, the unlock worked but the chip immediately found the same fault again: one of the cells is below its undervoltage threshold (measured by the dvdsosa project at about 2.2 V). No amount of clearing will hold until that cell rises, and the chip's charge path is disabled, so the DJI charger cannot raise it.

What is known to work, from [dvdsosa/dji-spark-battery-unbrick](https://github.com/dvdsosa/dji-spark-battery-unbrick), which recovered two packs with cells between 1.77 V and 2.2 V this way: during the 2–3 s between a reset and the re-latch, the chip precharges the cells from the wake-up supply (about 13 mA with a 12 V supply through 100 Ω). Repeating unseal → clear → reset ("pumping") lifts the lowest cell by 6–10 mV per round until it crosses the threshold, after which the chip stays in normal precharge on its own.

This sketch has that procedure built in as **`K`**. It reads the pack, asks you to type `y`, then runs rounds of unseal → clear → reset, waiting 5 s after each reset and printing one line per round with the three cell voltages, the lowest, the spread, the temperature and whether PF re-latched. It stops by itself when PF stays clear for 10 s (success), after 90 rounds, if the temperature passes 35 °C, if an unseal fails, if the chip stops answering, or when you press any key. On success it says so and re-seals the chip. Leave the 9V boost connected afterwards: the chip is now precharging the cells from it, and the `PCHG` marker on a `1` or `D` reading confirms that. Move to the DJI charger only once the lowest cell is well above 3.0 V. From 1.8 V expect 40 to 60 rounds, roughly 10 minutes, so use the hands-free boost rig from Step 4 and a fresh PP3. dvdsosa's read-only `monitor_charge.sh` also works with this sketch via the `D` command if you want a live display during the precharge.

**Field result, 7 October 2026 (pack serial 141, 22 cycles, 91% capacity).** First reading: cells 1843 / 1828 / 1999 mV, PF status SUV, Safety status CUV live. Pump with a PP3 9V directly on pins 3 and 2, no series resistor: lowest cell 2085 mV at pump start, PF re-latched after round 1 (2175 mV), stayed clear after round 2 (2243 mV). Two flash writes. Each round gave 70 to 90 mV, far more than dvdsosa's 6 to 10 mV with 12 V through 100 Ω. After the clear the chip reported SLEEP with both FETs off and no PCHG bit, yet the cells kept rising on the 9V alone: 2383 mV after a few minutes, 2525, 2663, with the spread closing from 157 to 42 mV. So "the BMS precharges by itself" may not show as PCHG on these packs; judge by `H` readings a few minutes apart.

Before trying it, understand what it is: deliberately re-clearing a safety fault on deeply discharged lithium cells, dozens of times. Each re-latch is a write to the chip's flash, which has limited endurance. It is two people's reported successes, not a validated repair. Do it outdoors or on a fireproof surface, with a current-limited supply, watching the temperature, and treat a pack recovered this way as suspect for flight. A cell that has sat below 2.0 V may have grown copper dendrites and can fail during charge even if the numbers look fine afterwards.

## Step 7: First charge (supervised)

**Charge promptly, and to completion.** A recovered pack that is left deeply discharged can set the Permanent Fail flag again within weeks, and the recovery has to be repeated. Recovery is not finished until the pack has completed a full charge; watch that the charger actually runs through to full rather than stalling partway.

Charge the battery on a fireproof surface and check on it periodically. When full, insert it in the drone and check the reported battery health in the DJI app. If the battery drains abnormally fast or the app reports large cell-voltage differences, the cells are worn: use it as a bench/testing battery at most, and recycle it eventually.

**Built-in health check:** after the first full charge, reconnect the three wires (no 9V needed once the battery holds a charge) and press **`H`** in the Serial Monitor. It reads the battery's own records and prints the figures that indicate whether the cells are still trustworthy:

- **Full-chg cap … % of new**: remaining capacity compared to when the pack was new. Below ~60–70%, expect very short flights.
- **Cell spread**: the voltage difference between the individual cells. On a charged pack, more than ~100 mV of spread means the cells have aged unevenly; such a pack can cut out mid-flight and should not be flown.
- **Cycle count / manufacture date**: how heavily the pack has been used.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Serial Monitor shows nothing / garbage | Baud rate must be **115200** (dropdown at bottom of Serial Monitor) |
| Upload fails (`stk500` errors) | Toggle **Tools → Processor** between **ATmega328P** and **ATmega328P (Old Bootloader)**; it must match the bootloader on your clone. Also check the Port menu. |
| No port in the Port menu | A charge-only USB cable is the usual cause; swap for a data cable. Otherwise install the CH340 driver. |
| `Nothing found!` when pressing `S` | First unplug the battery and press `W`: a failed pull-up explains it on its own. If `W` passes, the chip has no power (do the 9V boost while scanning) or the battery-end wiring is wrong: Pin 6→A4, Pin 1→A5, Pin 2→GND, and the 9V negative on the same ground rail as the Nano |
| `READ ERROR` on voltage | Same causes as above |
| `Chip is sealed — run U first` | Press `U`, then `P`. If `U` fails five times, check contacts (`T`) and the 9V boost |
| `OpStatus : unreadable` | The chip is not answering the status subcommands. Usually a contact problem: run `T` and re-seat the pins |
| `PF RE-LATCHED within 5 s of reset` | A cell is below ~2.2 V (check `H`). See [If PF re-latches after reset](#if-pf-re-latches-after-reset) |
| Recovery ran but battery still won't charge | Press `1`: if `PF active: no` and `Charge: disabled (XCHG)` is gone, the problem is on the charger side; otherwise press `A` again with the 9V boost held the whole time |
| LEDs never blink during 9V boost | Check the 9V battery is fresh; check +→Pin 3, −→Pin 2; hold firmly (the pins are small) |

---

## Menu reference (for step-by-step use)

| Key | Action |
|---|---|
| `1` | Read battery status: voltage, temperature, fault flags. Useful for before/after comparison |
| `H` | Battery health report: serial number, age, cycle count, remaining capacity, per-cell voltages |
| `S` | Scan for the chip; should find address `0x0B` |
| `T` | Bus stress test: 300 rapid reads with an error count. Run before recovery if working without pull-up resistors; proceed only on 0 errors |
| `D` | One CSV line of pack/cell voltages, current, temperature and status words. Read-only; for monitor scripts |
| `U` | Unseal (authenticate to) the chip with the DJI key, five attempts, verified by reading the security level |
| `F` | Full Access with the TI default key. Not needed for a PF clear; included for completeness |
| `P` | PermanentFailDataReset (0x0029), up to five attempts, PF status re-read after each |
| `R` | Restart the chip, then wait 5 s and report whether PF came back |
| `L` | Re-seal the chip |
| `A` | All of the above, in order: the normal recovery path |
| `K` | Pump: repeat `A` while PF re-latches, for packs with a cell below ~2.2 V. Asks for confirmation; stops on success, 90 rounds, 35 °C, lost contact or a key press. See [If PF re-latches after reset](#if-pf-re-latches-after-reset) |

---

*Ported from the ESP32 original by [Lishen99](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32) (MIT licence). The unseal key and command sequence come from the [dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools) community. The health-report idea comes from [davext/unbrick-dji](https://github.com/davext/unbrick-dji).*
