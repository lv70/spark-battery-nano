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
 * ── Recovery steps ────────────────────────────────────────────────────────
 *  U → Unseal
 *  P → Clear PF  (PermanentFailDataReset 0x0029, verified, up to 5 tries)
 *  R → Reset chip (then re-checks PF a few seconds later: if it re-latches,
 *                  a cell is still below the undervoltage threshold)
 *  L → Seal (re-lock)
 *  A → All of the above automatically (second round if PF survives reset)
 *  K → Pump: repeat U→P→R while a cell is below the undervoltage threshold.
 *      The BMS precharges for the 2-3 s between reset and re-latch, so each
 *      round lifts the cells a few mV until the PF stops re-latching.
 *      (Procedure from dvdsosa/dji-spark-battery-unbrick, pf_pump.sh.)
 *
 * ── Protocol notes (TI bq40z50-R2 TRM SLUUBK0, which the Z307 follows) ───
 *  Status subcommands are written to ManufacturerAccess (0x00) and the reply
 *  is block-read from ManufacturerData (0x23).  That works even when sealed.
 *  DJI packs refuse the ManufacturerBlockAccess (0x44) path this sketch used
 *  before, which is why the Seal/Safety/PF lines never printed.
 *  OperationStatus (0x0054, 32-bit): SEC1:SEC0 = bits 9:8
 *    (1 = Full Access, 2 = Unsealed, 3 = Sealed), PF = bit 12,
 *    XDSG = 13, XCHG = 14, PCHG = 3, CHG = 2, DSG = 1.
 *  0x002A is BlackBoxRecorderReset and 0x002B toggles the LEDs — neither is
 *  a PF clear; earlier versions sent them as such.
 */

#include <Wire.h>

#define BATT_ADDR  0x0B   // Standard SBS/SMBus battery address

// ── Standard SBS registers ────────────────────────────────────────────────
#define R_TEMP      0x08
#define R_VOLTAGE   0x09
#define R_CURRENT   0x0A
#define R_RSOC      0x0D   // Relative State of Charge (%)
#define R_STATUS    0x16   // BatteryStatus flags (0x16 per SBS spec; upstream had 0x19 = DesignVoltage)
#define R_MAC_DATA  0x23   // ManufacturerData: block reply to a 0x00 subcommand

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
#define MAC_RESET          0x0041   // DeviceReset
#define MAC_SEAL           0x0030   // Seal

// OperationStatus bits (32-bit value)
#define OP_DSG       (1UL << 1)
#define OP_CHG       (1UL << 2)
#define OP_PCHG      (1UL << 3)
#define OP_SEC_SHIFT 8               // SEC1:SEC0
#define OP_PF        (1UL << 12)
#define OP_XDSG      (1UL << 13)
#define OP_XCHG      (1UL << 14)
#define SEC_FULL     1
#define SEC_UNSEALED 2
#define SEC_SEALED   3

// DJI Spark confirmed key: 0xCCDF7EE0 — LOW word written first, then HIGH word.
// First key write always NACKs — that is a security feature of the chip, not an error.
#define KEY_SPARK_1        0x7EE0   // low  16 bits of 0xCCDF7EE0 — first write
#define KEY_SPARK_2        0xCCDF   // high 16 bits of 0xCCDF7EE0 — second write

// Fallback: default TI keys (tried if Spark key fails)
#define KEY_UNSEAL_1       0x0414
#define KEY_UNSEAL_2       0x3672
#define KEY_FULL_1         0xFFFF
#define KEY_FULL_2         0xFFFF

struct KeyPair { uint16_t w0, w1; };
const KeyPair KEY_SPARK   = { KEY_SPARK_1,  KEY_SPARK_2  };
const KeyPair KEY_TI_UNS  = { KEY_UNSEAL_1, KEY_UNSEAL_2 };
const KeyPair KEY_TI_FULL = { KEY_FULL_1,   KEY_FULL_2   };

// PermanentFailDataReset: the only PF-clear subcommand. Written to reg 0x00.
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

const __FlashStringHelper* busResultStr(uint8_t code) {
    if (code == 0) return F("ACK");
    if (code == 5) return F("TIMEOUT (chip busy — usually OK)");
    return F("NACK");
}

// SMBus block read: [count][data...]. Returns byte count, or 0 on failure.
// AVR Wire buffer is 32 bytes, so never request more than that.
uint8_t readBlock(uint8_t reg, uint8_t* buf, uint8_t maxLen) {
    Wire.beginTransmission(BATT_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;   // repeated start
    uint8_t req = maxLen + 1;
    if (req > 32) req = 32;
    if (Wire.requestFrom((uint8_t)BATT_ADDR, req) == 0) return 0;
    uint8_t len = Wire.read();               // byte-count prefix
    if (len > req - 1) len = req - 1;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.available() ? Wire.read() : 0;
    while (Wire.available()) Wire.read();    // flush remainder
    return len;
}

// Subcommand to ManufacturerAccess (0x00), reply from ManufacturerData (0x23).
// Works on a sealed chip for the status subcommands. Retries: the Spark
// connector contacts are unreliable.
uint8_t macCmd(uint16_t subcmd, uint8_t* buf, uint8_t maxLen) {
    for (uint8_t i = 0; i < 3; i++) {
        if (mac00Write(subcmd) == 0) {
            delay(5);
            uint8_t n = readBlock(R_MAC_DATA, buf, maxLen);
            if (n > 0) return n;
        }
        delay(10);
    }
    return 0;
}

// 32-bit status word (OperationStatus, SafetyStatus, PFStatus ...)
bool macRead32(uint16_t subcmd, uint32_t& out) {
    uint8_t b[4] = {0};
    uint8_t n = macCmd(subcmd, b, 4);
    if (n < 2) return false;
    out = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
        | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

// Security level from OperationStatus: SEC_FULL/UNSEALED/SEALED, 0 = unreadable
uint8_t readSec(uint32_t* opOut = NULL) {
    uint32_t op;
    if (!macRead32(MAC_OP_STATUS, op)) return 0;
    if (opOut) *opOut = op;
    return (op >> OP_SEC_SHIFT) & 0x03;
}

const __FlashStringHelper* secName(uint8_t sec) {
    if (sec == SEC_FULL)     return F("Full Access");
    if (sec == SEC_UNSEALED) return F("Unsealed");
    if (sec == SEC_SEALED)   return F("Sealed");
    return F("unreadable");
}

// PF is active if the OperationStatus PF bit or any PFStatus bit is set
bool pfActive(uint32_t* pfsOut = NULL) {
    uint32_t op = 0, pfs = 0;
    readSec(&op);
    macRead32(MAC_PF_STATUS, pfs);
    if (pfsOut) *pfsOut = pfs;
    return (op & OP_PF) || pfs;
}

bool present() {
    Wire.beginTransmission(BATT_ADDR);
    return Wire.endTransmission() == 0;
}

// SMBus block-read of a string register (first byte = length, then ASCII)
void printSBSString(uint8_t reg) {
    uint8_t b[31];
    uint8_t n = readBlock(reg, b, sizeof(b));
    if (!n) { Serial.println(F("(no reply)")); return; }
    for (uint8_t i = 0; i < n; i++)
        if (b[i] >= 32 && b[i] < 127) Serial.write(b[i]);   // printable ASCII only
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
        if (vmin < 2200) {
            Serial.print(F("  !! Lowest cell "));
            Serial.print(vmin);
            Serial.println(F(" mV is below the ~2.2 V undervoltage threshold:"));
            Serial.println(F("     PF will re-latch 2-3 s after every clear until it rises."));
        }
        if (vmin < 2000)
            Serial.println(F("  !! Cell < 2.0 V: possible internal damage. Supervise or discard."));
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

    uint8_t buf[4] = {0};

    // Device type
    uint8_t n = macCmd(MAC_DEV_TYPE, buf, 2);
    if (n >= 2) {
        uint16_t id = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        Serial.print(F("Device type  : "));
        printHex16(id);
        if (id == 0x4307 || id == 0xFA02) Serial.print(F("  (BQ9003/BQ40Z307 - OK)"));
        Serial.println();
    }

    // OperationStatus: security level, PF bit, FET state
    uint32_t op;
    uint8_t sec = readSec(&op);
    if (sec) {
        Serial.print(F("OpStatus     : ")); printHex32(op); Serial.println();
        Serial.print(F("  Security   : ")); Serial.println(secName(sec));
        Serial.print(F("  PF active  : ")); Serial.println((op & OP_PF) ? F("YES  <- locked") : F("no"));
        Serial.print(F("  Charge     : "));
        Serial.println((op & OP_XCHG) ? F("disabled (XCHG)")
                     : (op & OP_PCHG) ? F("PRECHARGE on")
                     : (op & OP_CHG)  ? F("FET on") : F("FET off"));
        Serial.print(F("  Discharge  : "));
        Serial.println((op & OP_XDSG) ? F("disabled (XDSG)")
                     : (op & OP_DSG)  ? F("FET on") : F("FET off"));
    } else {
        Serial.println(F("OpStatus     : unreadable"));
    }

    // Safety status (live faults). TRM bit map: 0 CUV, 1 COV, 2-3 OCC, 4-5 OCD
    uint32_t ss;
    if (macRead32(MAC_SAFETY_STATUS, ss)) {
        Serial.print(F("Safety status: "));
        printHex32(ss);
        Serial.println(ss ? F("  <- FAULTS ACTIVE") : F("  OK"));
        if (ss & 0x01) Serial.println(F("  -> CUV: cell under-voltage"));
        if (ss & 0x02) Serial.println(F("  -> COV: cell over-voltage"));
        if (ss & 0x0C) Serial.println(F("  -> OCC: overcurrent charge"));
        if (ss & 0x30) Serial.println(F("  -> OCD: overcurrent discharge"));
    }

    // Permanent Fail status. Bit 0 = SUV (safety cell undervoltage PF)
    uint32_t pf;
    if (macRead32(MAC_PF_STATUS, pf)) {
        Serial.print(F("PF status    : "));
        printHex32(pf);
        Serial.println(pf ? F("  <- PERMANENT FAIL (needs clearing)") : F("  OK"));
        if (pf & 0x01) Serial.println(F("  -> SUV: cell under-voltage PF (deep discharge)"));
    }

    Serial.println(F("------------------------------------\n"));
}

// One CSV line, read-only. Same format as dvdsosa/dji-spark-battery-unbrick
// so its monitor_charge.sh works with this sketch:
// D,pack_mV,c1_mV,c2_mV,c3_mV,current_mA,temp_dC,OperationStatus,PFStatus
// Unreadable fields are -1 (status words as 4294967295).
void printDump() {
    int32_t pack = readWord(R_VOLTAGE);
    int32_t c1 = readWord(R_CELL1), c2 = readWord(R_CELL2), c3 = readWord(R_CELL3);
    int32_t cur = readWord(R_CURRENT);
    int32_t tk  = readWord(R_TEMP);
    uint32_t op = 0xFFFFFFFFUL, pfs = 0xFFFFFFFFUL;
    if (!macRead32(MAC_OP_STATUS, op))  op  = 0xFFFFFFFFUL;
    if (!macRead32(MAC_PF_STATUS, pfs)) pfs = 0xFFFFFFFFUL;
    Serial.print(F("D,"));
    Serial.print(pack); Serial.print(',');
    Serial.print(c1);   Serial.print(',');
    Serial.print(c2);   Serial.print(',');
    Serial.print(c3);   Serial.print(',');
    Serial.print(cur < 0 ? -1 : (int16_t)cur); Serial.print(',');
    Serial.print(tk  < 0 ? -1 : tk - 2731);    Serial.print(',');
    Serial.print(op);   Serial.print(',');
    Serial.println(pfs);
}

// ── Recovery operations ───────────────────────────────────────────────────

// Both key halves back to back (the chip needs them within 4 s). A NACK on a
// word is not conclusive — the result is verified by reading OperationStatus.
void sendKey(const KeyPair& k) {
    mac00Write(k.w0); delay(2);
    mac00Write(k.w1); delay(50);
}

// Try a key up to 5 times until the security level is <= target
bool tryKey(const KeyPair& k, uint8_t target, const __FlashStringHelper* label) {
    for (uint8_t attempt = 1; attempt <= 5; attempt++) {
        sendKey(k);
        uint8_t sec = readSec();
        Serial.print(F("[U]   ")); Serial.print(label);
        Serial.print(F(" attempt ")); Serial.print(attempt);
        Serial.print(F(" -> ")); Serial.println(secName(sec));
        if (sec != 0 && sec <= target) return true;
        delay(300);
    }
    return false;
}

bool doUnseal() {
    Serial.println(F("[U] Unseal"));
    uint8_t sec = readSec();
    if (sec == 0) {
        Serial.println(F("[!] OperationStatus unreadable — check wiring/boost, then retry."));
        return false;
    }
    if (sec != SEC_SEALED) { Serial.println(F("[OK] Already unsealed.")); return true; }
    // DJI Spark key 0xCCDF7EE0, low word first (dji-firmware-tools issue #258)
    if (tryKey(KEY_SPARK, SEC_UNSEALED, F("Spark key 0xCCDF7EE0"))) return true;
    Serial.println(F("[U] Spark key failed — trying TI default 0x36720414"));
    if (tryKey(KEY_TI_UNS, SEC_UNSEALED, F("TI key"))) return true;
    Serial.println(F("[!] Still sealed. Check contacts (T) and keep the 9V boost held."));
    return false;
}

bool doFullAccess() {
    Serial.println(F("[F] Full Access (not needed for PF clear)"));
    if (readSec() == SEC_SEALED && !doUnseal()) return false;
    if (tryKey(KEY_TI_FULL, SEC_FULL, F("TI FA key 0xFFFFFFFF"))) return true;
    Serial.println(F("[!] Full Access not available with the default key."));
    return false;
}

// PermanentFailDataReset (0x0029) via 0x00, verified after each attempt.
// 0x002A/0x002B (sent by earlier versions) are BlackBoxRecorderReset and LED
// toggle — not PF clears — and are no longer sent.
bool doClearPF() {
    Serial.println(F("[P] PermanentFailDataReset (0x0029)"));
    uint8_t sec = readSec();
    if (sec == SEC_SEALED) { Serial.println(F("[!] Chip is sealed — run U first.")); return false; }
    if (sec == 0)          { Serial.println(F("[!] OperationStatus unreadable.")); return false; }

    busTimeout(BUS_TIMEOUT_SLOW_US);   // chip clock-stretches during the flash write
    bool ok = false;
    for (uint8_t attempt = 1; attempt <= 5; attempt++) {
        uint8_t r = mac00Write(MAC_PF_DATA_RESET);
        delay(1000);
        uint32_t pfs;
        bool still = pfActive(&pfs);
        Serial.print(F("[P]   attempt ")); Serial.print(attempt);
        Serial.print(' '); Serial.print(busResultStr(r));
        Serial.print(F("  PF status ")); printHex32(pfs);
        Serial.println(still ? F("  (still set)") : F("  (cleared)"));
        if (!still) { ok = true; break; }
    }
    busTimeout(BUS_TIMEOUT_FAST_US);
    if (ok) Serial.println(F("[OK] PF cleared."));
    else    Serial.println(F("[?] PF still set. It may only update after reset (R)."));
    return ok;
}

// DeviceReset, then watch for the PF re-latching. With a cell below ~2.2 V the
// BMS logs the undervoltage PF again 2-3 s after it restarts.
void doReset() {
    Serial.println(F("[R] DeviceReset (0x0041)"));
    busTimeout(BUS_TIMEOUT_SLOW_US);
    mac00Write(MAC_RESET);
    busTimeout(BUS_TIMEOUT_FAST_US);
    bool back = false;
    for (uint8_t i = 0; i < 30; i++) {           // up to ~3 s to reboot
        delay(100);
        if (present()) { back = true; break; }
    }
    if (!back) {
        Serial.println(F("[!] No reply after reset — 9V boost dropped, or cells too flat."));
        return;
    }
    Serial.println(F("[OK] BMS restarted (comes back sealed)."));
    delay(5000);                                  // past the re-latch window
    uint32_t pfs;
    if (pfActive(&pfs)) {
        Serial.print(F("[!] PF RE-LATCHED within 5 s of reset: ")); printHex32(pfs); Serial.println();
        Serial.println(F("    A cell is still below the undervoltage threshold (press H)."));
        Serial.println(F("    Clearing again will not hold until the cells come up."));
    } else {
        Serial.println(F("[OK] PF still clear 5 s after reset."));
    }
}

void doSeal() {
    Serial.println(F("[L] Seal (0x0030)"));
    uint8_t sec = readSec();
    if (sec == SEC_SEALED) { Serial.println(F("[OK] Already sealed.")); return; }
    mac00Write(MAC_SEAL);
    delay(300);
    sec = readSec();
    Serial.print(sec == SEC_SEALED ? F("[OK] State: ") : F("[!] State: "));
    Serial.println(secName(sec));
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
    Serial.println(F("\n[A] AUTO RECOVERY: Unseal -> Clear PF -> Reset (-> second round) -> Seal\n"));

    if (!present()) {
        Serial.println(F("[!] No I2C response. Apply 9V boost first:"));
        Serial.println(F("    Touch 9V+ to Pin 3, 9V- to Pin 2, hold while running this."));
        return;
    }
    int32_t v = readWord(R_VOLTAGE);
    Serial.print(F("[*] Pack voltage before: ")); Serial.print(v); Serial.println(F(" mV"));

    const uint8_t cellReg[3] = { R_CELL1, R_CELL2, R_CELL3 };
    int32_t vmin = 0;
    for (uint8_t i = 0; i < 3; i++) {
        int32_t cv = readWord(cellReg[i]);
        if (cv > 0 && (!vmin || cv < vmin)) vmin = cv;
    }
    if (vmin) {
        Serial.print(F("[*] Lowest cell: ")); Serial.print(vmin); Serial.println(F(" mV"));
        if (vmin < 2200)
            Serial.println(F("[!] Below ~2.2 V: expect PF to re-latch after reset. Proceeding anyway."));
        if (vmin < 2000)
            Serial.println(F("[!] Below 2.0 V: the cell may be damaged. Supervise any charge closely."));
    }

    if (!doUnseal()) return;
    delay(200);
    doClearPF();
    delay(200);
    doReset();

    uint32_t pfs;
    bool still = pfActive(&pfs);
    if (still) {
        Serial.println(F("\n[A] PF still active after reset — second round..."));
        if (doUnseal()) { doClearPF(); doReset(); }
        still = pfActive(&pfs);
    }
    doSeal();

    v = readWord(R_VOLTAGE);
    Serial.print(F("[*] Pack voltage after:  ")); Serial.print(v); Serial.println(F(" mV\n"));
    if (still) {
        Serial.println(F("[!] PF still active. If H shows a cell below ~2.2 V, the cells"));
        Serial.println(F("    must come up before a clear will hold (see README: pumping)."));
    } else {
        Serial.println(F("[DONE] PF clear. Plug into the DJI charger and supervise the first charge."));
        Serial.println(F("       Normal charging = alternating LEDs."));
    }
}

// ── Pump mode ─────────────────────────────────────────────────────────────
// With a cell below ~2.2 V the BMS re-latches the undervoltage PF 2-3 s after
// a reset. In that window it precharges from the wake-up supply, lifting the
// cells by roughly 6-10 mV per round. Repeat until the PF stays clear, then
// the BMS continues precharging on its own (PCHG bit).
//
// Every round that re-latches is a write to the chip's data flash (limited
// endurance), and this is deliberately re-clearing a safety fault on deeply
// discharged lithium cells. Round limit, temperature cutoff and key-press
// abort are built in. Keep the wake-up supply connected throughout.

#define PUMP_MAX_ROUNDS   90
#define PUMP_TEMP_STOP_DK 3081    // 35.0 C in deci-kelvin (273.15 + 35) * 10
#define PUMP_REST_MS      5000    // settle after reset before sampling
#define PUMP_CONFIRM_MS   10000   // PF must stay clear this long to count

char waitKey() {
    while (!Serial.available()) {}
    char c = Serial.read();
    delay(20);
    while (Serial.available()) Serial.read();
    return c;
}

// One sample of the pack. Returns false if the bus did not answer.
bool pumpSample(int32_t& vmin, int32_t& spread, int32_t& tempDk, bool& pf, bool& pchg) {
    const uint8_t cellReg[3] = { R_CELL1, R_CELL2, R_CELL3 };
    int32_t cv[3];
    vmin = 0; int32_t vmax = 0;
    for (uint8_t i = 0; i < 3; i++) {
        cv[i] = readWord(cellReg[i]);
        if (cv[i] <= 0) return false;
        if (!vmin || cv[i] < vmin) vmin = cv[i];
        if (cv[i] > vmax) vmax = cv[i];
    }
    spread = vmax - vmin;
    tempDk = readWord(R_TEMP);
    if (tempDk < 0) return false;
    uint32_t op = 0, pfs = 0;
    if (!macRead32(MAC_OP_STATUS, op)) return false;
    macRead32(MAC_PF_STATUS, pfs);
    pf   = (op & OP_PF) || pfs;
    pchg = op & OP_PCHG;
    Serial.print(F("  cells ")); Serial.print(cv[0]); Serial.print('/');
    Serial.print(cv[1]); Serial.print('/'); Serial.print(cv[2]);
    Serial.print(F("  min ")); Serial.print(vmin);
    Serial.print(F("  spread ")); Serial.print(spread);
    Serial.print(F("  ")); Serial.print(tempDk / 10.0f - 273.15f, 1); Serial.print(F(" C"));
    Serial.print(F("  PF ")); Serial.print(pf ? F("LATCHED") : F("clear"));
    if (pchg) Serial.print(F("  PCHG"));
    Serial.println();
    return true;
}

bool pumpUnseal() {
    uint8_t sec = readSec();
    if (sec == 0) return false;
    if (sec != SEC_SEALED) return true;
    for (uint8_t i = 0; i < 5; i++) {
        sendKey(KEY_SPARK);
        sec = readSec();
        if (sec == SEC_UNSEALED || sec == SEC_FULL) return true;
        delay(200);
    }
    return false;
}

void pumpMode() {
    Serial.println(F("\n[K] PUMP: repeat Unseal -> Clear PF -> Reset while PF re-latches"));
    if (!present()) { Serial.println(F("[!] No reply at 0x0B. Apply the 9V boost first.")); return; }

    int32_t vmin, spread, tempDk; bool pf, pchg;
    Serial.println(F("[K] Start:"));
    if (!pumpSample(vmin, spread, tempDk, pf, pchg)) {
        Serial.println(F("[!] Could not read the pack. Run T and fix the contacts first."));
        return;
    }
    if (!pf) {
        Serial.println(F("[K] PF is not latched. Nothing to pump; use D/1 to watch the charge."));
        return;
    }

    Serial.println(F("\n[K] This re-clears a safety fault on deeply discharged cells, up to"));
    Serial.print(F("    ")); Serial.print(PUMP_MAX_ROUNDS);
    Serial.println(F(" times. Each re-latch is a flash write. Keep the 9V boost on throughout."));
    Serial.println(F("    Do this on a fireproof surface. Press any key during the run to stop."));
    if (vmin < 2000) {
        Serial.print(F("    Lowest cell ")); Serial.print(vmin);
        Serial.println(F(" mV is below 2.0 V: possible internal damage."));
    }
    Serial.println(F("    Type y to continue:"));
    char c = waitKey();
    if (c != 'y' && c != 'Y') { Serial.println(F("[K] Cancelled. Nothing written.")); return; }

    int32_t startMin = vmin;
    uint8_t commFails = 0;
    for (uint16_t r = 1; r <= PUMP_MAX_ROUNDS; r++) {
        if (Serial.available()) {
            while (Serial.available()) Serial.read();
            Serial.println(F("[K] Stopped by key press."));
            break;
        }
        Serial.print(F("[K] round ")); Serial.println(r);

        if (!pumpUnseal()) { Serial.println(F("[!] Unseal failed. Stopping.")); break; }

        busTimeout(BUS_TIMEOUT_SLOW_US);
        mac00Write(MAC_PF_DATA_RESET);
        delay(1000);
        if (pfActive()) { mac00Write(MAC_PF_DATA_RESET); delay(1000); }   // one retry
        mac00Write(MAC_RESET);
        busTimeout(BUS_TIMEOUT_FAST_US);

        bool back = false;
        for (uint8_t i = 0; i < 30; i++) { delay(100); if (present()) { back = true; break; } }
        if (!back) { Serial.println(F("[!] BMS did not come back after reset. Boost dropped? Stopping.")); break; }
        delay(PUMP_REST_MS);

        if (!pumpSample(vmin, spread, tempDk, pf, pchg)) {
            if (++commFails >= 3) { Serial.println(F("[!] Lost communication. Stopping.")); break; }
            Serial.println(F("  (sample failed, retrying next round)"));
            continue;
        }
        commFails = 0;

        if (tempDk >= PUMP_TEMP_STOP_DK) {
            Serial.println(F("[!] Temperature above 35 C. STOP: disconnect the supply."));
            break;
        }
        if (!pf) {
            delay(PUMP_CONFIRM_MS);
            Serial.println(F("[K] confirm:"));
            if (pumpSample(vmin, spread, tempDk, pf, pchg) && !pf) {
                Serial.print(F("\n[OK] PF stays clear after ")); Serial.print(r);
                Serial.print(F(" round(s). Lowest cell ")); Serial.print(startMin);
                Serial.print(F(" -> ")); Serial.print(vmin); Serial.println(F(" mV."));
                Serial.println(F("     Leave the 9V boost on; the BMS should now precharge by itself."));
                Serial.println(F("     Watch with 1 or D. Move to the DJI charger once the lowest cell"));
                Serial.println(F("     is comfortably above 3.0 V, and supervise that first charge."));
                doSeal();
                return;
            }
            Serial.println(F("  PF came back during confirm; continuing."));
        }
    }
    Serial.print(F("\n[K] Ended with PF ")); Serial.print(pf ? F("LATCHED") : F("clear"));
    Serial.print(F(". Lowest cell ")); Serial.print(startMin);
    Serial.print(F(" -> ")); Serial.print(vmin); Serial.println(F(" mV."));
    if (pf) Serial.println(F("    If the cells rose but did not reach ~2.2 V, more rounds may get there."));
    doSeal();
}

// ── Arduino entry points ──────────────────────────────────────────────────

void printMenu() {
    Serial.println(F("\n======================================="));
    Serial.println(F("  DJI Spark Battery Recovery — Nano"));
    Serial.println(F("======================================="));
    Serial.println(F("  1  Read battery status (security, PF, FETs, faults)"));
    Serial.println(F("  H  Battery health (cells, wear, serial no.)"));
    Serial.println(F("  S  Scan I2C bus"));
    Serial.println(F("  W  Wiring self-test (no battery needed)"));
    Serial.println(F("  T  Bus stress test (run before recovery if no pull-up resistors)"));
    Serial.println(F("  D  One CSV status line (read-only, for monitor scripts)"));
    Serial.println(F("  U  Unseal (Spark: 0xCCDF7EE0, fallback TI defaults)"));
    Serial.println(F("  F  Full Access (TI default key; not needed for PF clear)"));
    Serial.println(F("  P  Clear PF (PermanentFailDataReset 0x0029, verified)"));
    Serial.println(F("  R  Reset chip (then checks whether PF re-latches)"));
    Serial.println(F("  L  Seal (re-lock)"));
    Serial.println(F("  A  Auto: full recovery sequence"));
    Serial.println(F("  K  Pump: repeat A while PF re-latches (cells below ~2.2 V)"));
    Serial.println(F("---------------------------------------"));
    Serial.print(F("Choice: "));
}

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println(F("\n========================================="));
    Serial.println(F("  DJI Spark Battery Recovery — Nano"));
    Serial.println(F("  BQ40Z307 via SMBus  (no CP2112 needed)"));
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
        case 'D': printDump();      return;   // no menu: keeps the CSV stream clean
        case 'U': doUnseal();       break;
        case 'F': doFullAccess();   break;
        case 'P': doClearPF();      break;
        case 'R': doReset();        break;
        case 'L': doSeal();         break;
        case 'A': autoRecover();    break;
        case 'K': pumpMode();       break;
        default:  break;
    }
    printMenu();
}
