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
3. Press **`1`** (status). Expect: a plausible pack voltage, a temperature, and a `Batt flags` line. On a locked battery the flags typically include `StopChargeAlarm` and `StopDischargeAlarm` even though the pack is empty; that combination confirms the diagnosis. The screen can also show `Seal state`, `Safety status`, and `PF status` lines, but on many DJI packs the firmware refuses those three reads and the lines simply never print, in any state. That is normal and does not block recovery.
4. Press **`H`** (health). Expect: serial number, cycle count, capacity figures. Worth recording as a "before" snapshot.

Only move on to Step 6 once all three respond sensibly. If they do not, go back over Step 2 and the troubleshooting table; no command in this step can have changed anything.

> **No resistors yet?** This step can still be attempted: the Nano has weak built-in pull-ups, and over short wires (≤15 cm) reading often works with those alone. Two caveats: a failure proves nothing (it is probably the missing resistors, not the wiring), and do not run Step 6 until the real resistors are fitted. Reads on a marginal bus are harmless; writes are not worth the risk.

## Step 6: Run the recovery

1. While holding the 9V boost, click into the Serial Monitor's input box, type **`A`** and press Enter.
2. Watch the output. A successful run looks like:

```
[*] Pack voltage before: 8222 mV
[U] Trying DJI Spark key (0x7EE0/0xCCDF = 0xCCDF7EE0, low word first)...
[OK] Unsealed with Spark key! State: Unsealed
[P] Sending PF clear commands (timeouts = chip processing, that's OK):
[P]   0x002A -> reg 0x44 : ACK
[P]   0x002B -> reg 0x44 : ACK
[P]   0x0029 -> reg 0x00 : ACK
[R] Sending chip reset...
[OK] BMS restarted. Voltage: 8226 mV
[DONE] Now plug into the DJI charger.
```

3. Keep holding the 9V until the `[R] ... BMS restarted` line appears, then release it.
4. Disconnect everything and put the battery on the **official DJI charger**. Alternating/chasing LEDs indicate it is charging again.

Normal behaviour that can look like an error:
- A "NACK" or error on the very first unseal attempt is expected; it is a security feature of the chip.
- The voltage shown during recovery may read high (~8.2V, the 9V feeding through) or may stay near the flat cell voltage; neither indicates a problem.
- `PF status unreadable after commands` is normal on packs whose firmware blocks that read (see Step 5). Judge success by the `Batt flags` line instead: after a successful clear the `StopChargeAlarm`/`StopDischargeAlarm` bits disappear, and the charger accepts the pack.
- "Not sealed" at the very end is harmless; the chip re-locks itself on restart.
- "Unsealed only (may need FA)" is fine; the PF clear works from the Unsealed state.

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
| `Nothing found!` when pressing `S` | Chip has no power: do the 9V boost while scanning. Also re-check: Pin 6→A4, Pin 1→A5, Pin 2→GND, both resistors in place |
| `READ ERROR` on voltage | Same causes as above |
| All PF commands say NACK | Chip did not unseal: press `U` first, then `P`. If `U` fails repeatedly, check wiring |
| Recovery ran but battery still won't charge | Press `A` again (some batteries need two rounds), and hold the 9V boost the whole time |
| `Seal state` / `Safety status` / `PF status` lines never print | Normal on many DJI packs: the firmware refuses those reads even after unsealing. Diagnose from the `Batt flags` line; verify recovery with the charger |
| LEDs never blink during 9V boost | Check the 9V battery is fresh; check +→Pin 3, −→Pin 2; hold firmly (the pins are small) |

---

## Menu reference (for step-by-step use)

| Key | Action |
|---|---|
| `1` | Read battery status: voltage, temperature, fault flags. Useful for before/after comparison |
| `H` | Battery health report: serial number, age, cycle count, remaining capacity, per-cell voltages |
| `S` | Scan for the chip; should find address `0x0B` |
| `T` | Bus stress test: 300 rapid reads with an error count. Run before recovery if working without pull-up resistors; proceed only on 0 errors |
| `U` | Unseal (authenticate to) the chip with the DJI key |
| `P` | Clear the Permanent Fail flags (all of them) |
| `R` | Restart the chip |
| `L` | Re-seal the chip |
| `A` | All of the above, in order: the normal recovery path |

---

*Ported from the ESP32 original by [Lishen99](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32) (MIT licence). The unseal key and command sequence come from the [dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools) community. The health-report idea comes from [davext/unbrick-dji](https://github.com/davext/unbrick-dji).*
