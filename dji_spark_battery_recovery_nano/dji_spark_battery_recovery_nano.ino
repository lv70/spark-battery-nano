/*
 * DJI Spark Battery Recovery — Arduino Nano (ATmega328P)
 *
 * Port of https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32
 * (MIT, Copyright (c) 2026 lishen.madusha@gmail.com) to the classic
 * Arduino Nano / Uno (ATmega328P only — the wiring self-test touches
 * the 328P's TWI registers directly).
 *
 * Battery chip : BQ40Z307 (branded BQ9003 by DJI), SMBus addr 0x0B
 *
 * ── Wiring ────────────────────────────────────────────────────────────────
 *
 *  The Nano's I2C pins are FIXED in hardware:  SDA = A4,  SCL = A5.
 *
 *  DJI Spark 6-pin connector (pins numbered left → right, notch on top):
 *
 *    Pin 1: SCL   → Nano A5   (+ 4.7 kΩ pull-up to 5V)
 *    Pin 2: GND   → Nano GND
 *    Pin 3: VBAT+ → 9 V battery +  (only when boosting a dead battery)
 *    Pin 4: VBAT+ → (same, parallel)
 *    Pin 5: GND   → (same as Pin 2)
 *    Pin 6: SDA   → Nano A4   (+ 4.7 kΩ pull-up to 5V)
 *
 *  Pull-ups to the Nano's 5V pin are fine: I2C lines are open-drain (nobody
 *  drives them high, only the resistors pull them up) and the BQ40Z307's
 *  SMBus pins are rated for 5V-pulled buses.
 *
 *  Power the Nano from USB only.  NEVER feed 9 V into the Nano's pins.
 *  The 9 V temporary boost gives the dead BMS enough power to talk.
 *
 * ── 9 V boost procedure ───────────────────────────────────────────────────
 *  1. Connect Nano A4/A5/GND to battery pins 6/1/2.
 *  2. Touch 9 V+ to Pin 3 (and/or 4), 9 V- to Pin 2 (and/or 5).
 *     Hold it – two LEDs flashing on the battery = BMS awake.
 *  3. Immediately run option A (auto recover) or step through U/P/R/L.
 *  4. Keep the 9 V held until the Reset step completes.
 *
 * ── Recovery steps (mirrors DJI Battery Killer software) ─────────────────
 *  U → Unseal
 *  P → Clear PF  (sends all three PF-clear commands)
 *  R → Reset chip
 *  L → Seal (re-lock)
 *  A → All of the above automatically
 */

#include <Wire.h>

#define BATT_ADDR  0x0B   // Standard SBS/SMBus battery address

// ── Standard SBS registers ────────────────────────────────────────────────
#define R_TEMP      0x08
#define R_VOLTAGE   0x09
#define R_CURRENT   0x0A
#define R_RSOC      0x0D   // Relative State of Charge (%)
#define R_STATUS    0x16   // BatteryStatus flags (0x16 per SBS spec; upstream had 0x19 = DesignVoltage)
#define R_MAC       0x44   // ManufacturerBlockAccess (BQ40Z307 gateway)

// ── Health / info registers (standard SBS unless noted) ──────────────────
#define R_FCC        0x10  // FullChargeCapacity (mAh) — what the pack holds NOW
#define R_CYCLES     0x17  // CycleCount
#define R_DESIGN_CAP 0x18  // DesignCapacity (mAh) — what it held when new
#define R_DESIGN_V   0x19  // DesignVoltage (mV)
#define R_MFG_DATE   0x1B  // packed day/month/year
#define R_SERIAL     0x1C  // SerialNumber (word)
#define R_MFG_NAME   0x20  // ManufacturerName (string)
#define R_DEV_NAME   0x21  // DeviceName (string)
#define R_CELL4      0x3C  // CellVoltage4 (mV) — reads 0 if the pack has fewer cells
#define R_CELL3      0x3D
#define R_CELL2      0x3E
#define R_CELL1      0x3F
#define R_SOH        0x4F  // StateOfHealth (%) — non-standard but supported by BQ40Zxx
#define R_DJI_SERIAL 0xD8  // DJI serial number (string, DJI-specific)

// ── BQ40Z307 MAC subcommands ──────────────────────────────────────────────
#define MAC_DEV_TYPE       0x0001
#define MAC_FW_VER         0x0002
#define MAC_SAFETY_ALERT   0x0050
#define MAC_SAFETY_STATUS  0x0051
#define MAC_PF_ALERT       0x0052
#define MAC_PF_STATUS      0x0053
#define MAC_OP_STATUS      0x0054
#define MAC_PF_CLEAR       0x002A   // Clear PF flag 1
#define MAC_PF2_CLEAR      0x002B   // Clear PF flag 2
#define MAC_RESET          0x0041   // Soft reset
#define MAC_SEAL           0x0030   // Re-seal device

// DJI Spark confirmed key: 0xCCDF7EE0 — LOW word written first, then HIGH word.
// First key write always NACKs — that is a security feature of the chip, not an error.
#define KEY_SPARK_1        0x7EE0   // low  16 bits of 0xCCDF7EE0 — first write
#define KEY_SPARK_2        0xCCDF   // high 16 bits of 0xCCDF7EE0 — second write

// Fallback: default TI keys (tried if Spark key fails)
#define KEY_UNSEAL_1       0x0414
#define KEY_UNSEAL_2       0x3672
#define KEY_FULL_1         0xFFFF
#define KEY_FULL_2         0xFFFF

// PermanentFailDataReset: clears ALL PF flags — goes to reg 0x00, not 0x44
#define MAC_PF_DATA_RESET  0x0029

// ── Bus timeout handling ──────────────────────────────────────────────────
// The AVR Wire library hangs forever on a stuck bus unless a timeout is set.
// Keep it short normally (fast bus scans when wiring is wrong), but raise it
// around the PF-clear/reset commands: the chip clock-stretches for seconds
// while writing flash, and that must not be aborted.
#define BUS_TIMEOUT_FAST_US    25000UL   // 25 ms
#define BUS_TIMEOUT_SLOW_US  6000000UL   // 6 s  (PF flash write)

void busTimeout(uint32_t us) {
#ifdef WIRE_HAS_TIMEOUT
    Wire.setWireTimeout(us, true);   // true = reset the bus on timeout
#else
    (void)us;  // very old AVR cores: no timeout support, sketch may hang on bad wiring
#endif
}

// ── Print helpers (Nano has no Serial.printf) ─────────────────────────────

void printHex16(uint16_t v) {
    char b[7];
    snprintf(b, sizeof(b), "0x%04X", v);
    Serial.print(b);
}

void printHex32(uint32_t v) {
    char b[11];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)v);
    Serial.print(b);
}

// ── Low-level helpers ─────────────────────────────────────────────────────

bool writeWord(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(val & 0xFF));
    Wire.write((uint8_t)(val >> 8));
    return Wire.endTransmission() == 0;
}

int32_t readWord(uint8_t reg) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)BATT_ADDR, (uint8_t)2) != 2) return -1;
    uint16_t lo = Wire.read();
    uint16_t hi = Wire.read();
    return (int32_t)((hi << 8) | lo);
}

// Write 2-byte subcommand to register 0x00 (ManufacturerAccess)
// Used for: unseal keys, full-access keys, reset, seal — these work even while sealed
// Returns the Wire status code: 0 = ACK, 2/3 = NACK, 5 = timeout
uint8_t mac00Write(uint16_t subcmd) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)(subcmd & 0xFF));
    Wire.write((uint8_t)(subcmd >> 8));
    return Wire.endTransmission();
}

// Write 2-byte MAC subcommand to register 0x44 (ManufacturerBlockAccess)
// Used for: PF clear, status reads — only accessible after unsealing
uint8_t macSend(uint16_t subcmd) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(R_MAC);
    Wire.write((uint8_t)(subcmd & 0xFF));
    Wire.write((uint8_t)(subcmd >> 8));
    return Wire.endTransmission();
}

const __FlashStringHelper* busResultStr(uint8_t code) {
    if (code == 0) return F("ACK");
    if (code == 5) return F("TIMEOUT (chip busy — usually OK)");
    return F("NACK");
}

// Block-read response from register 0x44 (first byte = byte count)
// Uses stop+start instead of repeated-start — some BMS chips need this.
// AVR Wire buffer is 32 bytes, so never request more than that.
uint8_t macRead(uint8_t* buf, uint8_t maxLen) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(R_MAC);
    if (Wire.endTransmission(true) != 0) return 0;  // STOP, then new START below
    delay(5);
    uint8_t req = maxLen + 1;
    if (req > 32) req = 32;                  // AVR Wire hard limit
    Wire.requestFrom((uint8_t)BATT_ADDR, req);
    if (!Wire.available()) return 0;
    uint8_t len = Wire.read();               // byte-count prefix
    len = min(len, (uint8_t)(req - 1));
    for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
    while (Wire.available()) Wire.read();    // flush remainder
    return len;
}

// Send command, wait, read response
uint8_t macCmd(uint16_t subcmd, uint8_t* buf, uint8_t maxLen) {
    if (macSend(subcmd) != 0) return 0;
    delay(20);
    return macRead(buf, maxLen);
}

// SMBus block-read of a string register (first byte = length, then ASCII)
void printSBSString(uint8_t reg) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) { Serial.println(F("(no reply)")); return; }
    delay(5);
    if (!Wire.requestFrom((uint8_t)BATT_ADDR, (uint8_t)32)) {  // AVR Wire max
        Serial.println(F("(no reply)"));
        return;
    }
    uint8_t len = Wire.read();
    if (len > 31) len = 31;
    for (uint8_t i = 0; i < len && Wire.available(); i++) {
        char ch = (char)Wire.read();
        if (ch >= 32 && ch < 127) Serial.print(ch);   // printable ASCII only
    }
    while (Wire.available()) Wire.read();             // flush remainder
    Serial.println();
}

// ── Status display ────────────────────────────────────────────────────────

// Health/info screen — idea borrowed from davext/unbrick-dji.
// The interesting numbers after a recovery: full-charge capacity vs design
// capacity (how worn the pack is) and the spread between cell voltages
// (mismatched cells = pack is unsafe/worn regardless of the cleared PF flag).
void printHealth() {
    Serial.println(F("\n---- Battery Health / Info ---------"));

    Serial.print(F("Manufacturer : ")); printSBSString(R_MFG_NAME);
    Serial.print(F("Device name  : ")); printSBSString(R_DEV_NAME);
    Serial.print(F("DJI serial   : ")); printSBSString(R_DJI_SERIAL);

    int32_t w = readWord(R_SERIAL);
    if (w >= 0) { Serial.print(F("Serial (SBS) : ")); Serial.println(w); }

    w = readWord(R_MFG_DATE);
    if (w >= 0) {
        Serial.print(F("Manufactured : "));
        Serial.print(w & 0x1F);           Serial.print('.');
        Serial.print((w >> 5) & 0x0F);    Serial.print('.');
        Serial.println(1980 + ((w >> 9) & 0x7F));
    }

    w = readWord(R_CYCLES);
    if (w >= 0) { Serial.print(F("Cycle count  : ")); Serial.println(w); }

    int32_t dc  = readWord(R_DESIGN_CAP);
    int32_t fcc = readWord(R_FCC);
    if (dc > 0) {
        Serial.print(F("Design cap   : "));
        Serial.print(dc);
        Serial.println(F(" mAh (when new)"));
    }
    if (fcc >= 0) {
        Serial.print(F("Full-chg cap : "));
        Serial.print(fcc);
        Serial.print(F(" mAh"));
        if (dc > 0) {
            Serial.print(F("  = "));
            Serial.print((fcc * 100) / dc);
            Serial.print(F("% of new"));
        }
        Serial.println();
    }

    w = readWord(R_SOH);
    if (w >= 0) { Serial.print(F("Health (SOH) : ")); Serial.print(w); Serial.println('%'); }

    w = readWord(R_DESIGN_V);
    if (w >= 0) { Serial.print(F("Design volt  : ")); Serial.print(w); Serial.println(F(" mV")); }

    // Per-cell voltages — slots the pack doesn't have read 0 and are skipped
    const uint8_t cellReg[4] = { R_CELL1, R_CELL2, R_CELL3, R_CELL4 };
    int32_t vmin = 0, vmax = 0;
    for (uint8_t i = 0; i < 4; i++) {
        int32_t cv = readWord(cellReg[i]);
        if (cv <= 0) continue;
        Serial.print(F("Cell "));
        Serial.print(i + 1);
        Serial.print(F(" volt  : "));
        Serial.print(cv);
        Serial.println(F(" mV"));
        if (!vmin || cv < vmin) vmin = cv;
        if (cv > vmax) vmax = cv;
    }
    if (vmin) {
        Serial.print(F("Cell spread  : "));
        Serial.print(vmax - vmin);
        Serial.println(F(" mV  (>100 mV when charged = worn/unsafe cells)"));
    }

    Serial.println(F("------------------------------------\n"));
}

void printStatus() {
    Serial.println(F("\n---- Battery Status ----------------"));

    int32_t v = readWord(R_VOLTAGE);
    if (v >= 0) {
        Serial.print(F("Pack voltage : "));
        Serial.print(v);
        Serial.println(F(" mV"));   // real per-cell voltages: press H
    } else {
        Serial.println(F("Pack voltage : READ ERROR  <- check wiring & pull-ups"));
    }

    int32_t t = readWord(R_TEMP);
    if (t >= 0) {
        Serial.print(F("Temperature  : "));
        Serial.print(t / 10.0f - 273.15f, 1);
        Serial.println(F(" C"));
    }

    int32_t c = readWord(R_CURRENT);
    if (c >= 0) {
        Serial.print(F("Current      : "));
        Serial.print((int16_t)c);
        Serial.println(F(" mA"));
    }

    int32_t soc = readWord(R_RSOC);
    if (soc >= 0) {
        Serial.print(F("State of chg : "));
        Serial.print((int)soc);
        Serial.println(F("%"));
    }

    // Bit positions per the SBS spec (upstream's decode had wrong bits/labels)
    int32_t bs = readWord(R_STATUS);
    if (bs >= 0) {
        uint8_t errCode = (uint16_t)bs & 0x0F;   // error code = bits 0-3
        Serial.print(F("Batt flags   : "));
        printHex16((uint16_t)bs);
        if (errCode) { Serial.print(F("  ERR=0x")); Serial.print(errCode, HEX); }
        if (bs & 0x8000) Serial.print(F("  OverChargedAlarm"));
        if (bs & 0x4000) Serial.print(F("  StopChargeAlarm"));
        if (bs & 0x1000) Serial.print(F("  OverTempAlarm"));
        if (bs & 0x0800) Serial.print(F("  StopDischargeAlarm"));
        if (bs & 0x0200) Serial.print(F("  LowCapacityAlarm"));
        if (bs & 0x0040) Serial.print(F("  Discharging"));
        if (bs & 0x0020) Serial.print(F("  FullyCharged"));
        if (bs & 0x0010) Serial.print(F("  FullyDischarged"));
        Serial.println();
    }

    uint8_t buf[30] = {0};

    // Device type
    uint8_t n = macCmd(MAC_DEV_TYPE, buf, sizeof(buf));
    if (n >= 4) {
        uint16_t id = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        Serial.print(F("Device type  : "));
        printHex16(id);
        if (id == 0x4307 || id == 0xFA02) Serial.print(F("  (BQ9003/BQ40Z307 - OK)"));
        Serial.println();
    }

    // Seal state from OperationStatus
    n = macCmd(MAC_OP_STATUS, buf, sizeof(buf));
    if (n >= 1) {
        uint8_t sec = (buf[0] >> 1) & 0x03;
        Serial.print(F("Seal state   : "));
        if      (sec == 0) Serial.println(F("Full Access"));
        else if (sec == 1) Serial.println(F("Unsealed"));
        else if (sec == 3) Serial.println(F("Sealed"));
        else               Serial.println(F("?"));
        if (buf[0] & 0x08) Serial.println(F("  CHG FET ON"));
        if (buf[0] & 0x10) Serial.println(F("  DSG FET ON"));
    }

    // Safety status (latched faults)
    n = macCmd(MAC_SAFETY_STATUS, buf, sizeof(buf));
    if (n >= 2) {
        uint16_t ss = buf[0] | ((uint16_t)buf[1] << 8);
        Serial.print(F("Safety status: "));
        printHex16(ss);
        Serial.println(ss ? F("  <- FAULTS LATCHED") : F("  OK"));
        if (ss & 0x0001) Serial.println(F("  -> CUV: cell under-voltage latched"));
        if (ss & 0x0008) Serial.println(F("  -> COV: cell over-voltage latched"));
        if (ss & 0x0010) Serial.println(F("  -> OCC: overcurrent charge"));
        if (ss & 0x0020) Serial.println(F("  -> OCD: overcurrent discharge"));
    }

    // Permanent Fail status
    n = macCmd(MAC_PF_STATUS, buf, sizeof(buf));
    if (n >= 4) {
        uint32_t pf = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                    | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
        Serial.print(F("PF status    : "));
        printHex32(pf);
        Serial.println(pf ? F("  <- PERMANENT FAIL (needs clearing)") : F("  OK"));
    }

    Serial.println(F("------------------------------------\n"));
}

// ── Recovery operations ───────────────────────────────────────────────────

// Check seal state by probing 0x44 — sealed chips NACK writes to 0x44
// sec: 3 = sealed, 1 = unsealed, 0 = full access (we treat 0 and 1 the same here)
bool readSealState(uint8_t& sec) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(R_MAC);
    Wire.write((uint8_t)0x01); Wire.write((uint8_t)0x00);   // MAC_DEV_TYPE probe
    bool acked = (Wire.endTransmission() == 0);
    delay(10);
    if (!acked) { sec = 3; return true; } // NACK = still sealed

    // ACK means 0x44 is accessible — try reading OperationStatus for detail
    uint8_t buf[8] = {0};
    uint8_t n = macCmd(MAC_OP_STATUS, buf, sizeof(buf));
    sec = (n >= 1) ? ((buf[0] >> 1) & 0x03) : 1;
    return true;
}

void printSealName(uint8_t sec) {
    if      (sec == 0) Serial.print(F("Full Access"));
    else if (sec == 1) Serial.print(F("Unsealed"));
    else if (sec == 3) Serial.print(F("Sealed"));
    else               Serial.print(F("?"));
}

void doUnseal() {
    // DJI Spark confirmed unseal key: 0xCCDF7EE0, low word first
    // Source: dji-firmware-tools GitHub issue #258 (multiple independent confirmations)
    Serial.println(F("[U] Trying DJI Spark key (0x7EE0/0xCCDF = 0xCCDF7EE0, low word first)..."));
    mac00Write(KEY_SPARK_1); delay(15);
    mac00Write(KEY_SPARK_2); delay(150);

    uint8_t sec = 3;
    readSealState(sec);
    if (sec != 3) {
        Serial.print(F("[OK] Unsealed with Spark key! State: "));
        printSealName(sec);
        Serial.println();
        if (sec != 0) {
            // Write Spark key pair a second time to escalate to Full Access.
            // DJI Spark appears to use the same key for both Unseal and Full Access.
            Serial.println(F("[U] Re-applying Spark key for Full Access..."));
            mac00Write(KEY_SPARK_1); delay(15);
            mac00Write(KEY_SPARK_2); delay(150);
            readSealState(sec);
            Serial.print(sec == 0 ? F("[OK] State: ") : F("[!] State: "));
            if (sec == 0) Serial.println(F("Full Access"));
            else          Serial.println(F("Unsealed only (PF clear will proceed; may need FA)"));
        }
        return;
    }

    // Fallback: standard TI defaults
    Serial.println(F("[U] Spark key failed — trying TI defaults (0x0414/0x3672)..."));
    mac00Write(KEY_UNSEAL_1); delay(15);
    mac00Write(KEY_UNSEAL_2); delay(150);
    readSealState(sec);
    if (sec == 3) {
        Serial.println(F("[!] Still sealed after both key attempts."));
        Serial.println(F("    Check wiring, then retry with the 9V boost held."));
        return;
    }
    Serial.print(F("[OK] Unsealed with TI keys! State: "));
    printSealName(sec);
    Serial.println();

    if (sec != 0) {
        // Try FA escalation with TI defaults
        Serial.println(F("[U] Trying Full Access escalation (0xFFFF/0xFFFF)..."));
        mac00Write(KEY_FULL_1); delay(15);
        mac00Write(KEY_FULL_2); delay(150);
        readSealState(sec);
        Serial.print(sec == 0 ? F("[OK] State now: ") : F("[!] State now: "));
        if (sec == 0) Serial.println(F("Full Access"));
        else          Serial.println(F("Unsealed (no FA key worked)"));
    }
}

// Try all three PF-clear approaches and report exactly which ACK/NACK.
// DJI Battery Killer works in Unsealed mode, so 0x002A/0x002B via 0x44 are the most likely path.
void doClearPF() {
    // Timeouts (not NACKs) on these commands mean the chip accepted and is clock-stretching
    // during a flash write — that is expected and means the command ran.
    Serial.println(F("[P] Sending PF clear commands (timeouts = chip processing, that's OK):"));
    busTimeout(BUS_TIMEOUT_SLOW_US);

    uint8_t r1 = macSend(MAC_PF_CLEAR);            // 0x002A → reg 0x44
    Serial.print(F("[P]   0x002A -> reg 0x44 : "));
    Serial.println(busResultStr(r1));
    delay(300);

    uint8_t r2 = macSend(MAC_PF2_CLEAR);           // 0x002B → reg 0x44
    Serial.print(F("[P]   0x002B -> reg 0x44 : "));
    Serial.println(busResultStr(r2));
    delay(300);

    uint8_t r3 = mac00Write(MAC_PF_DATA_RESET);    // 0x0029 → reg 0x00 (needs FA but try anyway)
    Serial.print(F("[P]   0x0029 -> reg 0x00 : "));
    Serial.println(busResultStr(r3));
    delay(1000);

    busTimeout(BUS_TIMEOUT_FAST_US);

    uint8_t buf[8] = {0};
    uint8_t n = macCmd(MAC_PF_STATUS, buf, sizeof(buf));
    if (n < 2) {
        Serial.println(F("[?] PF status unreadable after commands."));
        Serial.println(F("    Run R, then U, then 1 to re-check PF status after reset."));
        return;
    }
    uint32_t pf = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
    if (!pf) {
        Serial.println(F("[OK] All PF flags cleared!"));
    } else {
        Serial.print(F("[?] PF still: "));
        printHex32(pf);
        Serial.println();
    }
}

void doReset() {
    Serial.println(F("[R] Sending chip reset..."));
    busTimeout(BUS_TIMEOUT_SLOW_US);
    mac00Write(MAC_RESET);
    delay(2500);
    busTimeout(BUS_TIMEOUT_FAST_US);
    int32_t v = readWord(R_VOLTAGE);
    if (v >= 0) {
        Serial.print(F("[OK] BMS restarted. Voltage: "));
        Serial.print(v);
        Serial.println(F(" mV"));
    } else {
        Serial.println(F("[!] No reply after reset — normal if very low voltage."));
    }
}

void doSeal() {
    Serial.println(F("[L] Sealing battery..."));
    // First attempt — may NACK if chip is guarding while PF is active; retry once
    mac00Write(MAC_SEAL);
    delay(500);
    uint8_t sec = 3;
    readSealState(sec);
    if (sec != 3) {
        Serial.println(F("[L] First seal attempt pending — retrying..."));
        mac00Write(MAC_SEAL);
        delay(500);
        readSealState(sec);
    }
    Serial.print(sec == 3 ? F("[OK] Seal state: ") : F("[!] Seal state: "));
    if (sec == 3) Serial.println(F("Sealed"));
    else          Serial.println(F("Not sealed — chip may need Reset first"));
}

// Wiring self-test — works with NO battery connected.
// Checks that each data line is lifted high by its pull-up resistor (proves
// resistor + 5V rail + breadboard row are all connected), and that the two
// lines are not shorted to each other or to ground.
// CANNOT detect: swapped SDA/SCL at the battery end, or bad contact in the
// battery connector — those need the battery (or a multimeter).
void wiringTest() {
    Serial.println(F("[W] Wiring self-test (no battery needed)..."));

    TWCR &= ~_BV(TWEN);   // take A4/A5 back from the I2C hardware

    // Discharge both lines, then release them with internal pull-ups OFF.
    // Only an external pull-up can lift a line high afterwards.
    pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW);
    pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW);
    delay(5);
    pinMode(SDA, INPUT);
    pinMode(SCL, INPUT);
    delayMicroseconds(100);
    bool sdaUp = digitalRead(SDA);
    bool sclUp = digitalRead(SCL);

    Serial.print(F("[W]   SDA (A4) pull-up: "));
    Serial.println(sdaUp ? F("OK") : F("FAIL - resistor/rail/row not connected"));
    Serial.print(F("[W]   SCL (A5) pull-up: "));
    Serial.println(sclUp ? F("OK") : F("FAIL - resistor/rail/row not connected"));

    // Hold one line low, the other must stay high — else they are bridged
    bool independent = true;
    if (sdaUp && sclUp) {
        pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW);
        delayMicroseconds(100);
        if (!digitalRead(SCL)) independent = false;
        pinMode(SDA, INPUT);
        pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW);
        delayMicroseconds(100);
        if (!digitalRead(SDA)) independent = false;
        pinMode(SCL, INPUT);
        Serial.print(F("[W]   Lines independent: "));
        Serial.println(independent ? F("OK - no short between SDA and SCL")
                                   : F("FAIL - SDA and SCL are shorted together"));
    }

    if (sdaUp && sclUp && independent) {
        Serial.println(F("[W] Nano-side circuit is GOOD."));
        Serial.println(F("[W] If scan still fails with battery: suspect the battery"));
        Serial.println(F("[W] connector contacts, or SDA/SCL swapped at the battery end."));
    }

    // Hand the pins back to the I2C hardware
    Wire.begin();
    Wire.setClock(100000);
    busTimeout(BUS_TIMEOUT_FAST_US);
}

// Bus stress test: hammer the bus with reads and count errors.
// Use to judge whether a bus without proper pull-up resistors (Nano internal
// pull-ups only) is reliable enough to risk the recovery writes.
void busStressTest() {
    Serial.println(F("[T] Bus stress test: 100 rounds x 3 reads..."));
    uint16_t errs = 0;
    int32_t vFirst = -1;
    for (uint8_t i = 0; i < 100; i++) {
        int32_t v = readWord(R_VOLTAGE);
        if (v < 0) errs++;
        else if (vFirst < 0) vFirst = v;
        else if (v > vFirst + 500 || v + 500 < vFirst) errs++;  // wildly different = corrupt read
        if (readWord(R_STATUS) < 0) errs++;
        uint8_t buf[8] = {0};
        if (macCmd(MAC_DEV_TYPE, buf, sizeof(buf)) == 0) errs++;
        if ((i + 1) % 20 == 0) {
            Serial.print(F("[T]   "));
            Serial.print(i + 1);
            Serial.print(F("/100 rounds, errors: "));
            Serial.println(errs);
        }
    }
    Serial.print(F("[T] Done. 300 transactions, "));
    Serial.print(errs);
    Serial.println(F(" errors."));
    if (errs == 0) Serial.println(F("[T] Bus looks solid."));
    else           Serial.println(F("[T] Bus is flaky — fit real pull-up resistors before recovery."));
}

void scanBus() {
    Serial.println(F("[S] Scanning I2C (1-126)..."));
    bool found = false;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("  0x"));
            if (a < 0x10) Serial.print('0');
            Serial.print(a, HEX);
            if (a == BATT_ADDR) Serial.print(F(" <- DJI BMS"));
            Serial.println();
            found = true;
        }
    }
    if (!found) {
        Serial.println(F("  Nothing found!"));
        Serial.println(F("  Check: SDA->Pin6->A4, SCL->Pin1->A5, GND->Pin2, pull-ups fitted?"));
        Serial.println(F("  If battery is dead, apply 9V boost to Pin3(+) / Pin2(-)."));
    }
}

void autoRecover() {
    Serial.println(F("\n[A] AUTO RECOVERY — mirrors DJI Battery Killer sequence"));
    Serial.println(F("    Unseal -> Clear PF (two passes) -> Reset -> Seal\n"));

    int32_t v = readWord(R_VOLTAGE);
    if (v < 0) {
        Serial.println(F("[!] No I2C response. Apply 9V boost first:"));
        Serial.println(F("    Touch 9V+ to Pin 3, 9V- to Pin 2, hold while running this."));
        return;
    }
    Serial.print(F("[*] Pack voltage before: "));
    Serial.print(v);
    Serial.println(F(" mV"));

    doUnseal();   delay(400);
    doClearPF();  delay(400);
    doClearPF();  delay(400);   // second pass — some packs only clear on the repeat
    doReset();    delay(2500);
    doSeal();

    v = readWord(R_VOLTAGE);
    Serial.print(F("[*] Pack voltage after:  "));
    Serial.print(v);
    Serial.println(F(" mV\n"));
    Serial.println(F("[DONE] Now plug into the DJI charger."));
    Serial.println(F("       Normal charging = alternating LEDs."));
    Serial.println(F("       Still flashing 1+2 = try Auto again, or hold 9V longer."));
}

// ── Arduino entry points ──────────────────────────────────────────────────

void printMenu() {
    Serial.println(F("\n======================================="));
    Serial.println(F("  DJI Spark Battery Recovery — Nano"));
    Serial.println(F("======================================="));
    Serial.println(F("  1  Read battery status"));
    Serial.println(F("  H  Battery health (cells, wear, serial no.)"));
    Serial.println(F("  S  Scan I2C bus"));
    Serial.println(F("  W  Wiring self-test (no battery needed)"));
    Serial.println(F("  T  Bus stress test (run before recovery if no pull-up resistors)"));
    Serial.println(F("  U  Unseal (Spark: 0xCCDF7EE0, fallback TI defaults)"));
    Serial.println(F("  P  Clear PF (all Permanent Fail flags)"));
    Serial.println(F("  R  Reset chip"));
    Serial.println(F("  L  Seal (re-lock)"));
    Serial.println(F("  A  Auto: full recovery sequence"));
    Serial.println(F("---------------------------------------"));
    Serial.print(F("Choice: "));
}

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println(F("\n========================================="));
    Serial.println(F("  DJI Spark Battery Recovery — Nano"));
    Serial.println(F("  BQ40Z307 via I2C  (no CP2112 needed)"));
    Serial.println(F("========================================="));
    Serial.println(F("  SDA -> A4,  SCL -> A5,  GND -> GND"));
    Serial.println(F("  4.7 kOhm pull-ups on SDA & SCL to 5V"));
    Serial.println(F("  9V boost -> Battery Pin3(+) / Pin2(-)\n"));

    Wire.begin();
    Wire.setClock(100000);            // 100 kHz = SMBus speed
    busTimeout(BUS_TIMEOUT_FAST_US);  // raised automatically around PF/reset ops

    scanBus();
    printMenu();
}

void loop() {
    if (!Serial.available()) return;

    char c = toupper((char)Serial.read());
    delay(5);
    while (Serial.available()) Serial.read(); // flush rest of line

    Serial.println(c);
    switch (c) {
        case '1': printStatus();    break;
        case 'H': printHealth();    break;
        case 'S': scanBus();        break;
        case 'W': wiringTest();     break;
        case 'T': busStressTest();  break;
        case 'U': doUnseal();       break;
        case 'P': doClearPF();      break;
        case 'R': doReset();        break;
        case 'L': doSeal();         break;
        case 'A': autoRecover();    break;
        default:  break;
    }
    printMenu();
}
