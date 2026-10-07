# Recovering a DJI Spark battery with an Arduino Nano

A step-by-step guide for beginners. No soldering needed.

**What is wrong with the battery.** Inside every Spark battery is a small chip that protects the cells. If the battery sits unused for a long time, the cells run down below the chip's safety limit. The chip then sets a "Permanent Fail" flag and switches the battery off. The charger ignores it, the drone does not see it, and the lights blink an error.

**What this guide does.** The flag is a setting in the chip's memory, not physical damage. An Arduino Nano can talk to the chip through two data pins, clear the flag and restart it. If the cells have gone very low, the sketch can also nudge them back up until the flag will stay cleared.

---

## Safety

- **Lithium batteries can catch fire.** Do the first charge on a fireproof surface, with you nearby. If the pack gets hot, swells or smells, stop and recycle it.
- **A swollen pack is finished.** Do not try to recover it.
- **The 9V battery never touches the Arduino.** It only ever goes to the Spark battery's power pins. 9V into an Arduino pin destroys the Arduino.
- **Do not let the Spark's + pins touch its data pins.** Work slowly and check before powering anything.
- A recovered battery has had a hard life. Treat it with more suspicion than a new one.

---

## What you need

| Item | Notes | Cost |
|---|---|---|
| Arduino Nano | Any ATmega328P Nano or Uno. Clones are fine | £3 to £20 |
| USB data cable | Mini-USB or USB-C to match the Nano. A charge-only cable will not work | £2 |
| 2 resistors, 4.7 kΩ | Anything from 2.2 to 10 kΩ is fine | under £1 |
| Breadboard and jumper wires | Male-to-male "Dupont" wires. The pins fit the Spark connector | £5 |
| 9V battery (PP3) and clip | Wakes up the dead battery | £3 |
| Multimeter | Optional, but useful for checking the 9V | |

---

## Step 1: Learn the battery pins

Hold the Spark battery upright with the connector pins at the top, facing you. Number the six pins **1 to 6 from left to right**.

```
 ┌─────────────────────────────┐
 │  1    2    3    4    5    6 │
 └──┬────┬────┬────┬────┬────┬─┘
   SCL  GND  BAT+ BAT+ GND  SDA
```

| Pin | What it is | Goes to |
|---|---|---|
| 1 | SCL (data clock) | Nano **A5** |
| 2 | GND (ground) | Nano **GND** and 9V **black** |
| 3 | BAT+ | 9V **red** |
| 4 | BAT+ | nothing |
| 5 | GND | nothing |
| 6 | SDA (data) | Nano **A4** |

**Getting a good contact.** Push each jumper pin straight into its slot until it sits flush with the battery body. Ordinary male Dupont pins fit and grip well enough; that is how all three packs in this guide were done. If `T` in Step 5 reports errors, bend the last 2 mm of the pin slightly so the tip presses against the metal inside the slot, and tape the wires to the battery so they cannot twist out.

---

## Step 2: Build the Nano side

Do this with **nothing connected to the Spark battery yet**.

1. Put the Nano on the breadboard and plug it into your computer by USB.
2. Run a jumper from **A4** to an empty row. This row is SDA.
3. Run a jumper from **A5** to another empty row. This row is SCL.
4. Put a resistor from the SDA row to the **5V** rail.
5. Put a resistor from the SCL row to the **5V** rail.
6. Run a jumper from a Nano **GND** pin to the **negative** rail.

Breadboard rails are often split in the middle. If a resistor is on the far half of a rail, it is not connected. The self-test in Step 3 will catch this.

---

## Step 3: Install and test the software

1. Download and install the **Arduino IDE** from arduino.cc.
2. Open `dji_spark_battery_recovery_nano/dji_spark_battery_recovery_nano.ino`.
3. In the menu bar: **Tools → Board → Arduino AVR Boards → Arduino Nano**.
4. **Tools → Port**: unplug the Nano, look at the list, plug it back in. The entry that appears is the Nano.
5. Click **Upload** (the arrow button).
   - If it fails with `stk500` errors, change **Tools → Processor** to the other ATmega328P option and try again. Clones come with either setting.
6. **Tools → Serial Monitor**. Set the speed at the bottom to **115200**.

You should see a menu. Garbage means the speed is wrong.

7. **Type `W` and press Enter.** This tests your breadboard with no battery attached. You want:

```
[W]   SDA (A4) pull-up: OK
[W]   SCL (A5) pull-up: OK
[W]   Lines independent: OK
```

A `FAIL` means that resistor is missing, in the wrong row, or on a dead part of the rail. Fix it and press `W` again. Do not go on until both say OK.

---

## Step 4: Connect the battery

1. Spark **pin 6** → the SDA row (A4).
2. Spark **pin 1** → the SCL row (A5).
3. Spark **pin 2** → the negative rail.
4. 9V **black** → the negative rail.
5. 9V **red** → Spark **pin 3**. Nowhere else.

The 9V has to stay connected the whole time you are working on the battery, often for many minutes. Seat two spare jumper pins in Spark pins 3 and 2, clip the 9V leads to them with crocodile clips and tape it all down.

If the Spark's lights blink, the chip is awake. If not, check the 9V with a multimeter across Spark pins 3 and 2: you want close to 9 V.

---

## Step 5: Read the battery (nothing is changed)

These three commands only read. You can repeat them as often as you like.

**`S`** finds the chip. You want `0x0B <- DJI BMS`. If it says `Nothing found!`, go to Troubleshooting.

**`T`** checks the connection is stable. You want `0 errors` with your hands off the wires. If not, re-seat the pins. A bad connection halfway through a recovery wastes a round.

**`1`** shows the chip's status. The lines that matter:

```
  Security   : Sealed              <- normal starting point
  PF active  : YES  <- locked      <- this is the problem
Safety status: 0x00000001          <- see below
PF status    : 0x00000001          <- SUV = ran down too far
```

**`H`** shows the three cell voltages. Write down the lowest one.

Now you know which case you have:

| Lowest cell | Safety status | What it means | Do |
|---|---|---|---|
| above 2200 mV | `OK` | Cells recovered on their own. Easy case | Step 6 |
| below 2200 mV | `CUV` | A cell is still too low. The flag will come straight back | Step 7 |
| below 2000 mV | `CUV` | Cell may be damaged. Your call | Step 7, with care |
| one cell far below the others | | That cell is probably faulty | Consider recycling |

---

## Step 6: Clear the flag (the easy case)

Hold the 9V on, type **`A`** and press Enter. A good run looks like this:

```
[U]   Spark key 0xCCDF7EE0 attempt 1 -> Unsealed
[P]   attempt 1 ACK  PF status 0x00000000  (cleared)
[OK] BMS restarted (comes back sealed).
[OK] PF still clear 5 s after reset.
[DONE] PF clear. Plug into the DJI charger and supervise the first charge.
```

The line to look for is **`PF still clear 5 s after reset`**. Go to Step 8.

If instead you see **`PF RE-LATCHED within 5 s of reset`**, a cell is below the limit after all. Go to Step 7.

---

## Step 7: Pump the cells up (the hard case)

When a cell is below about 2.2 V, the chip puts the flag back a few seconds after every clear. But in those few seconds it also lets a little charge from the 9V into the cells. Repeating clear-and-restart ("pumping") lifts the cells a bit each time until they are over the limit and the flag stays off. This is how two other people recovered packs with cells as low as 1.8 V. It is not a factory procedure.

Type **`K`** and press Enter. It shows the cells, warns you, and waits for you to type **`y`**. Then it prints one line per round:

```
[K] round 1
  cells 2190/2175/2332  min 2175  spread 157  22.1 C  PF LATCHED
[K] round 2
  cells 2258/2243/2393  min 2243  spread 150  22.1 C  PF clear
[OK] PF stays clear after 2 round(s).
```

It stops on its own when the flag stays off, or after 90 rounds, or if the pack warms past 35 °C, or if the connection drops. Press any key to stop it yourself. Stop if the pack feels warm, or if a cell's voltage goes **down** over several rounds.

**After it succeeds, keep the 9V connected.** The cells carry on charging slowly from it, around 100 mV per hour. Press `H` now and then. Once the lowest cell is around 2.7 to 3.0 V, go to Step 8. On the first pack this took about four and a half hours; the DJI charger is much faster once it accepts the pack, so do not feel you must wait for 3.0 V.

---

## Step 8: Charge it

1. Press `1` once more. You want `PF active: no` and a normal temperature.
2. Unclip the 9V and the data wires.
3. Put the battery on the **official DJI charger**, on a fireproof surface. Stay with it for the first ten minutes and feel for warmth.
4. Running lights mean it is charging. **Let it charge to full.** A pack left half-empty can set the flag again within weeks.
5. If the lights blink an error pattern, the charger has refused it. Reconnect the Nano, press `1`, and check whether PF came back. If the cells are still low, give it more time on the 9V.

**After the full charge**, reconnect the three data wires (no 9V needed now) and press `H`:

- **Full-chg cap … % of new**: under about 70% means very short flights.
- **Cell spread**: over about 100 mV when full means the cells have aged unevenly. Such a pack can cut out in flight. Do not fly it.

A pack whose cells were ever below 2.0 V is best kept as a bench or test battery even if these numbers look fine.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Serial Monitor shows nothing or garbage | Speed must be 115200 |
| Upload fails with `stk500` errors | Change Tools → Processor to the other ATmega328P option |
| No port appears | The USB cable is charge-only. Swap it. Otherwise install the CH340 driver |
| `W` says FAIL | That resistor is missing, in the wrong row, or on the dead half of the rail |
| `S` says `Nothing found!` | Unplug the battery and press `W` first. If `W` passes: 9V not connected or flat, pins 1 and 6 swapped, pin 2 not on the same ground as the Nano |
| `T` shows errors | A pin is not gripping. Push it in flush. If that is not enough, bend the tip slightly and tape the wires down |
| `OpStatus : unreadable` | Same as above: the chip is not answering properly |
| `Chip is sealed — run U first` | Press `U`, then `P` |
| `PF RE-LATCHED within 5 s of reset` | A cell is under 2.2 V. Step 7 |
| Pack lights never blink with 9V on | Check the 9V with a meter across Spark pins 3 and 2. Check red is on pin 3, black on pin 2 |
| Charger refuses the pack after recovery | Press `1`. If PF is back, the cells were too low: Step 7. If PF is clear, try the charger again after more time on the 9V |

---

## Menu

| Key | Does | Changes the battery? |
|---|---|---|
| `1` | Status: security, PF flag, faults, temperature | no |
| `H` | Health: cells, capacity, serial, age | no |
| `S` | Find the chip | no |
| `W` | Test the breadboard wiring, no battery needed | no |
| `T` | Connection stability test, 300 reads | no |
| `D` | One line of numbers for monitor scripts | no |
| `U` | Unseal the chip | yes |
| `F` | Full access. Not needed | yes |
| `P` | Clear the PF flag | yes |
| `R` | Restart the chip, then check the flag stays off | yes |
| `L` | Seal the chip again | yes |
| `A` | The normal recovery: U, P, R, L | yes |
| `K` | Pump: repeat A until the flag stays off | yes |

---

*Based on the ESP32 version by [Lishen99](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32) (MIT). The unseal key comes from the [dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools/issues/258) community. The pumping method and protocol corrections come from [dvdsosa/dji-spark-battery-unbrick](https://github.com/dvdsosa/dji-spark-battery-unbrick). The health screen idea is from [davext/unbrick-dji](https://github.com/davext/unbrick-dji).*
