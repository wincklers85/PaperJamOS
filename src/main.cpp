#include <Arduino.h>
#include <M5EPD.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp32-hal-bt.h>

#include <PN532.h>
#include "PaperJamPN532I2C.h"
#include <MFRC522v2.h>
#include <MFRC522DriverI2C.h>

#include "PaperJamConfig.h"

using namespace PaperJamConfig;

M5EPD_Canvas canvas(&M5.EPD);
TwoWire NFCWire(1);
PaperJamPN532I2C pn532Interface(NFCWire, 0x24);
PN532 nfc(pn532Interface);

// MFRC522v2 normally calls Wire.begin() inside its I2C driver.
// PaperJam OS configures Wire1 itself on the M5Paper Port B pins, so keep it.
class PaperJamMFRC522DriverI2C : public MFRC522DriverI2C {
public:
    PaperJamMFRC522DriverI2C(uint8_t address, TwoWire &wire)
        : MFRC522DriverI2C(address, wire) {}
    bool init() override { return true; }
};

PaperJamMFRC522DriverI2C rc522Driver(0x28, NFCWire);
MFRC522 rc522(rc522Driver);

enum class NfcBackend {
    None,
    PN532,
    MFRC522Compat
};

NfcBackend nfcBackend = NfcBackend::None;

enum class Page {
    Home,
    NFC,
    QuickSettings
};

struct NfcCardInfo {
    bool valid = false;
    uint8_t uid[10] = {0};
    uint8_t uidLength = 0;
    uint16_t atqa = 0;
    uint8_t sak = 0;
    String type = "Unknown ISO14443A";
    uint32_t seenAt = 0;
};

Page currentPage = Page::Home;
Page pageBeforeQuickSettings = Page::Home;

bool wifiEnabled = false;
bool btEnabled = false;
bool nfcEnabled = true;
bool nfcReaderReady = false;
int activeNfcSda = NFC_SDA_PIN;
int activeNfcScl = NFC_SCL_PIN;
String nfcDiagnostic = "Not tested";
bool nfcFoundAt28 = false;

bool fingerWasDown = false;
int touchStartX = 0;
int touchStartY = 0;
int touchLastX = 0;
int touchLastY = 0;

uint32_t lastNfcScan = 0;
uint32_t lastStatusRefresh = 0;
uint32_t lastTouchPoll = 0;
uint32_t lastUidHash = 0;

NfcCardInfo lastCard;

static String hex2(uint8_t value) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", value);
    return String(buf);
}

static String uidToString(const uint8_t *uid, uint8_t len) {
    String out;
    for (uint8_t i = 0; i < len; ++i) {
        if (i) out += ":";
        out += hex2(uid[i]);
    }
    return out;
}

static uint32_t uidHash(const uint8_t *uid, uint8_t len) {
    uint32_t h = 2166136261UL;
    for (uint8_t i = 0; i < len; ++i) {
        h ^= uid[i];
        h *= 16777619UL;
    }
    return h;
}

static int batteryPercent() {
    uint32_t mv = M5.getBatteryVoltage();
    if (mv < 3300) mv = 3300;
    if (mv > 4350) mv = 4350;
    return (int)(((mv - 3300.0f) / (4350.0f - 3300.0f)) * 100.0f);
}

static String timeString() {
    rtc_time_t rtcTime;
    M5.RTC.getTime(&rtcTime);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", rtcTime.hour, rtcTime.min);
    return String(buf);
}

static String classifyIso14443A(uint16_t atqa, uint8_t sak, uint8_t uidLen) {
    // Common ISO14443A / NXP family signatures.
    // Some MIFARE Plus security levels and compatible clones intentionally
    // overlap with Classic/DESFire SAK values, so labels stay conservative.
    switch (sak) {
        case 0x09:
            return "MIFARE Classic Mini";
        case 0x08:
            if (atqa == 0x0004 || atqa == 0x0002 || atqa == 0x0044)
                return "MIFARE Classic 1K / Plus SL1";
            return "MIFARE Classic-compatible";
        case 0x18:
            return "MIFARE Classic 4K / Plus SL1";
        case 0x10:
            return "MIFARE Plus / ISO14443-4";
        case 0x11:
            return "MIFARE Plus";
        case 0x20:
            return "DESFire / Plus SL3 / ISO14443-4";
        case 0x28:
            return "SmartMX / ISO14443-4";
        case 0x00:
            if (atqa == 0x0044 || atqa == 0x4400 || uidLen == 7)
                return "MIFARE Ultralight / NTAG / Type 2";
            return "ISO14443A Type 2-compatible";
        default:
            break;
    }

    if (sak & 0x20) return "ISO14443-4 (Type A)";
    return "ISO14443A tag";
}

static String backendName() {
    switch (nfcBackend) {
        case NfcBackend::PN532:
            return "PN532";
        case NfcBackend::MFRC522Compat:
            return "Si512/MFRC522";
        default:
            return "NFC";
    }
}

static String classifyRc522Type(MFRC522::PICC_Type type) {
    switch (type) {
        case MFRC522Constants::PICC_TYPE_MIFARE_MINI:
            return "MIFARE Classic Mini";
        case MFRC522Constants::PICC_TYPE_MIFARE_1K:
            return "MIFARE Classic 1K";
        case MFRC522Constants::PICC_TYPE_MIFARE_4K:
            return "MIFARE Classic 4K";
        case MFRC522Constants::PICC_TYPE_MIFARE_UL:
            return "MIFARE Ultralight / NTAG";
        case MFRC522Constants::PICC_TYPE_MIFARE_PLUS:
            return "MIFARE Plus";
        case MFRC522Constants::PICC_TYPE_MIFARE_DESFIRE:
            return "MIFARE DESFire";
        case MFRC522Constants::PICC_TYPE_ISO_14443_4:
            return "ISO14443-4 Type A";
        case MFRC522Constants::PICC_TYPE_ISO_18092:
            return "ISO18092 / NFC";
        case MFRC522Constants::PICC_TYPE_TNP3XXX:
            return "TNP3XXX";
        default:
            return "ISO14443A tag";
    }
}

static void serialLog(const String &line) {
    Serial.println(line);
}

static void drawBattery(int x, int y, int pct) {
    canvas.drawRect(x, y, 38, 19, 0);
    canvas.fillRect(x + 38, y + 5, 4, 9, 0);
    int inner = map(constrain(pct, 0, 100), 0, 100, 0, 32);
    if (inner > 0) canvas.fillRect(x + 3, y + 3, inner, 13, 0);
}

static void drawStatusBar() {
    canvas.fillRect(0, 0, SCREEN_W, STATUS_H, 15);
    canvas.drawLine(0, STATUS_H - 1, SCREEN_W, STATUS_H - 1, 8);

    canvas.setTextColor(0);
    canvas.setTextSize(2);
    canvas.drawString(timeString(), 18, 16);

    int x = 160;
    canvas.drawString(wifiEnabled ? "WiFi" : "WiFi-", x, 16);
    x += 82;
    canvas.drawString(btEnabled ? "BT" : "BT-", x, 16);
    x += 58;
    canvas.drawString(nfcEnabled ? "NFC" : "NFC-", x, 16);

    int pct = batteryPercent();
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
    canvas.drawString(pctBuf, 430, 16);
    drawBattery(486, 17, pct);
}

static void drawFooterHint(const String &hint) {
    canvas.drawLine(0, 905, SCREEN_W, 905, 12);
    canvas.setTextSize(1);
    canvas.setTextColor(5);
    canvas.drawString(hint, 20, 925);
}

static void renderHome() {
    canvas.fillCanvas(15);
    drawStatusBar();

    canvas.setTextColor(0);
    canvas.setTextSize(4);
    canvas.drawString("PaperJam OS", 28, 92);

    canvas.setTextSize(1);
    canvas.setTextColor(6);
    canvas.drawString(String("v") + PAPERJAM_VERSION + "  •  M5Paper first generation", 30, 142);

    // NFC app tile.
    canvas.fillRect(30, 215, 210, 215, 14);
    canvas.drawRect(30, 215, 210, 215, 6);
    canvas.drawRoundRect(87, 252, 96, 96, 12, 0);
    canvas.drawCircle(135, 300, 26, 0);
    canvas.drawCircle(135, 300, 15, 0);
    canvas.setTextColor(0);
    canvas.setTextSize(3);
    canvas.drawString("NFC", 94, 368);
    canvas.setTextSize(1);
    canvas.setTextColor(5);
    canvas.drawString(nfcReaderReady ? (backendName() + " ready") : "NFC reader not found", 58, 405);

    canvas.setTextColor(0);
    canvas.setTextSize(2);
    canvas.drawString("Applications", 30, 185);

    drawFooterHint("Swipe down: Quick Settings   •   PWR: sleep");
}

static void drawToggle(int x, int y, int w, int h, const String &name, bool on, const String &detail) {
    canvas.fillRect(x, y, w, h, on ? 12 : 15);
    canvas.drawRect(x, y, w, h, 6);
    canvas.setTextColor(0);
    canvas.setTextSize(2);
    canvas.drawString(name, x + 18, y + 18);
    canvas.setTextSize(1);
    canvas.setTextColor(5);
    canvas.drawString(detail, x + 18, y + 55);
    canvas.setTextColor(0);
    canvas.drawString(on ? "ON" : "OFF", x + w - 58, y + 20);
}

static void renderQuickSettings() {
    canvas.fillCanvas(15);
    drawStatusBar();

    canvas.setTextColor(0);
    canvas.setTextSize(3);
    canvas.drawString("Quick Settings", 28, 88);

    drawToggle(28, 155, 484, 100, "NFC", nfcEnabled,
               nfcReaderReady ? (backendName() + String(" • SDA G") + activeNfcSda + " / SCL G" + activeNfcScl)
                              : nfcDiagnostic);
    drawToggle(28, 275, 484, 100, "Wi-Fi", wifiEnabled,
               wifiEnabled ? "Radio STA attiva" : "Radio disattivata");
    drawToggle(28, 395, 484, 100, "Bluetooth", btEnabled,
               btEnabled ? "Controller BT attivo" : "Radio disattivata");

    canvas.setTextSize(1);
    canvas.setTextColor(6);
    canvas.drawString("Tocca un modulo per attivarlo/disattivarlo.", 30, 545);
    canvas.drawString("Swipe up o tocca la status bar per chiudere.", 30, 575);

    drawFooterHint("PaperJam OS modular controls");
}

static void renderNfc() {
    canvas.fillCanvas(15);
    drawStatusBar();

    canvas.setTextColor(0);
    canvas.setTextSize(2);
    canvas.drawString("< Home", 22, 83);

    canvas.setTextSize(4);
    canvas.drawString("NFC Reader", 30, 135);

    canvas.setTextSize(1);
    canvas.setTextColor(6);
    canvas.drawString(backendName() + String(" • SDA G") + activeNfcSda + " • SCL G" + activeNfcScl + " • RST G" + NFC_RST_PIN, 30, 185);

    canvas.fillRect(28, 228, 484, 520, 14);
    canvas.drawRect(28, 228, 484, 520, 7);

    if (!nfcEnabled) {
        canvas.setTextColor(0);
        canvas.setTextSize(3);
        canvas.drawString("NFC disattivato", 95, 420);
        canvas.setTextSize(1);
        canvas.setTextColor(5);
        canvas.drawString("Riattivalo dalla tendina Quick Settings.", 86, 470);
    } else if (!nfcReaderReady) {
        canvas.setTextColor(0);
        canvas.setTextSize(3);
        canvas.drawString("Reader NFC non rilevato", 58, 400);
        canvas.setTextSize(1);
        canvas.setTextColor(5);
        canvas.drawString(nfcDiagnostic, 52, 455);
        canvas.drawString("Vedi Serial Monitor 115200 per il test dettagliato.", 52, 488);
    } else if (!lastCard.valid) {
        canvas.setTextColor(0);
        canvas.setTextSize(3);
        canvas.drawString("Avvicina un tag", 93, 388);
        canvas.setTextSize(1);
        canvas.setTextColor(5);
        canvas.drawString("Avvicina MIFARE / NTAG / ISO14443A", 96, 445);
        canvas.drawCircle(270, 535, 56, 6);
        canvas.drawCircle(270, 535, 38, 6);
        canvas.drawCircle(270, 535, 20, 6);
    } else {
        canvas.setTextColor(0);
        canvas.setTextSize(1);
        canvas.drawString("TYPE", 55, 268);
        canvas.setTextSize(2);
        canvas.drawString(lastCard.type, 55, 298);

        canvas.setTextSize(1);
        canvas.setTextColor(5);
        canvas.drawString("UID / SERIAL", 55, 365);
        canvas.setTextColor(0);
        canvas.setTextSize(2);
        canvas.drawString(uidToString(lastCard.uid, lastCard.uidLength), 55, 398);

        char meta[64];
        snprintf(meta, sizeof(meta), "UID bytes: %u", lastCard.uidLength);
        canvas.setTextSize(1);
        canvas.setTextColor(5);
        canvas.drawString(meta, 55, 452);

        snprintf(meta, sizeof(meta), "ATQA: 0x%04X", lastCard.atqa);
        canvas.drawString(meta, 55, 495);

        snprintf(meta, sizeof(meta), "SAK:  0x%02X", lastCard.sak);
        canvas.drawString(meta, 55, 532);

        canvas.drawLine(55, 580, 485, 580, 10);
        canvas.setTextColor(0);
        canvas.drawString("Ultima lettura acquisita correttamente.", 55, 610);
        canvas.setTextColor(6);
        canvas.drawString("Il riconoscimento famiglia usa ATQA/SAK.", 55, 648);
    }

    drawFooterHint("UID e metadati vengono stampati anche su USB Serial");
}

static void fullRefresh() {
    switch (currentPage) {
        case Page::Home:
            renderHome();
            break;
        case Page::NFC:
            renderNfc();
            break;
        case Page::QuickSettings:
            renderQuickSettings();
            break;
    }

    // Draw into a full 540x960 framebuffer and update the EPD only after
    // the complete page has been composed.
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    lastStatusRefresh = millis();
}

static void bootFrame(int percent, const String &message) {
    canvas.fillCanvas(15);
    canvas.setTextColor(0);

    canvas.setTextSize(5);
    canvas.drawString("PaperJam", 54, 170);
    canvas.setTextSize(5);
    canvas.drawString("OS", 344, 170);

    canvas.setTextSize(1);
    canvas.setTextColor(5);
    canvas.drawString(String("v") + PAPERJAM_VERSION + " alpha boot", 58, 240);

    canvas.drawRect(55, 320, 430, 28, 6);
    int fill = map(constrain(percent, 0, 100), 0, 100, 0, 424);
    if (fill > 0) canvas.fillRect(58, 323, fill, 22, 4);

    char p[8];
    snprintf(p, sizeof(p), "%d%%", percent);
    canvas.setTextColor(0);
    canvas.setTextSize(2);
    canvas.drawString(p, 238, 370);

    canvas.setTextSize(1);
    canvas.drawString("[ OK ] " + message, 58, 440);
    canvas.setTextColor(6);
    canvas.drawString("paperjam@m5paper: booting userspace...", 58, 477);

    canvas.pushCanvas(0, 0, percent == 100 ? UPDATE_MODE_GC16 : UPDATE_MODE_DU4);
}

static void hardwareResetNfcReader() {
    pinMode(NFC_RST_PIN, OUTPUT);
    digitalWrite(NFC_RST_PIN, LOW);
    delay(20);
    digitalWrite(NFC_RST_PIN, HIGH);
    delay(80);
    Serial.printf("[nfc] hardware reset pulse on GPIO%d\n", NFC_RST_PIN);
}

static uint8_t probeI2cAddress(uint8_t address) {
    NFCWire.beginTransmission(address);
    return NFCWire.endTransmission();
}

static bool configureAndProbeBus(int sda, int scl, bool &found24, bool &found28) {
    NFCWire.end();
    delay(10);

    if (!NFCWire.begin(sda, scl, NFC_I2C_FREQ)) {
        Serial.printf("[i2c2] failed to start SDA=%d SCL=%d\n", sda, scl);
        found24 = false;
        found28 = false;
        return false;
    }

    NFCWire.setTimeOut(20);
    NFCWire.setClock(NFC_I2C_FREQ);
    delay(30);

    uint8_t e24 = probeI2cAddress(0x24);
    uint8_t e28 = probeI2cAddress(0x28);

    found24 = (e24 == 0);
    found28 = (e28 == 0);

    Serial.printf("[i2c2] SDA=%d SCL=%d -> 0x24:%s 0x28:%s\n",
                  sda, scl,
                  found24 ? "ACK" : "no",
                  found28 ? "ACK" : "no");

    return found24 || found28;
}

static bool tryPn532At(uint8_t address) {
    pn532Interface.setAddress(address);
    Serial.printf("[nfc] testing PN532 frame protocol at 0x%02X\n", address);

    nfc.begin();
    delay(40);

    uint32_t version = nfc.getFirmwareVersion();
    if (!version) {
        pn532Interface.wakeup();
        delay(80);
        version = nfc.getFirmwareVersion();
    }

    if (!version) {
        Serial.printf("[nfc] 0x%02X ACKs but PN532 GetFirmwareVersion failed\n", address);
        return false;
    }

    uint8_t ic = (version >> 24) & 0xFF;
    uint8_t fwMajor = (version >> 16) & 0xFF;
    uint8_t fwMinor = (version >> 8) & 0xFF;

    Serial.printf("[nfc] PN5%02X firmware %u.%u at 0x%02X\n",
                  ic, fwMajor, fwMinor, address);

    if (!nfc.SAMConfig()) {
        serialLog("[nfc] PN532 SAMConfig failed");
        return false;
    }

    nfc.setPassiveActivationRetries(0x01);
    nfcBackend = NfcBackend::PN532;
    nfcDiagnostic = String("PN532 ready @ 0x") + hex2(address);
    return true;
}

static bool tryRc522CompatibleAt28() {
    serialLog("[nfc] testing Si512/MFRC522-compatible register protocol at 0x28");

    // Wire1 has already been configured by configureAndProbeBus().
    rc522.PCD_Init();
    delay(12);

    uint8_t version = rc522Driver.PCD_ReadRegister(MFRC522Constants::VersionReg);
    Serial.printf("[nfc] RC522-compatible VersionReg = 0x%02X\n", version);

    if (version == 0x00 || version == 0xFF) {
        serialLog("[nfc] MFRC522-compatible register test failed");
        return false;
    }

    rc522.PCD_AntennaOff();
    delay(5);
    rc522.PCD_AntennaOn();
    rc522.PCD_SetAntennaGain(MFRC522Constants::RxGain_max);
    delay(10);

    uint8_t txControl = rc522Driver.PCD_ReadRegister(MFRC522Constants::TxControlReg);
    uint8_t rfCfg = rc522Driver.PCD_ReadRegister(MFRC522Constants::RFCfgReg);
    Serial.printf("[nfc] TxControlReg=0x%02X RFCfgReg=0x%02X gain=0x%02X\n",
                  txControl, rfCfg, rc522.PCD_GetAntennaGain());

    if ((txControl & 0x03) != 0x03) {
        rc522Driver.PCD_WriteRegister(MFRC522Constants::TxControlReg, txControl | 0x03);
        delay(5);
        txControl = rc522Driver.PCD_ReadRegister(MFRC522Constants::TxControlReg);
        Serial.printf("[nfc] forced antenna ON, TxControlReg=0x%02X\n", txControl);
    }

    nfcBackend = NfcBackend::MFRC522Compat;
    nfcDiagnostic = String("Si512/MFRC522 ready @ 0x28 v") + hex2(version);
    return true;
}

static bool initNfcReader() {
    serialLog("[boot] NFC I2C auto-detect");
    hardwareResetNfcReader();
    serialLog("[boot] Testing both Port B pin orientations");

    bool found24 = false;
    bool found28 = false;

    bool anyAck = configureAndProbeBus(33, 26, found24, found28);
    activeNfcSda = 33;
    activeNfcScl = 26;

    if (!anyAck) {
        serialLog("[i2c2] no ACK, trying SDA/SCL swapped");
        anyAck = configureAndProbeBus(26, 33, found24, found28);
        activeNfcSda = 26;
        activeNfcScl = 33;
    }

    nfcFoundAt28 = found28;

    if (!found24 && !found28) {
        nfcDiagnostic = "Nessun ACK I2C a 0x24/0x28";
        serialLog("[nfc] no known reader on I2C bus");
        nfcBackend = NfcBackend::None;
        return false;
    }

    // Native PN532 first when the standard address exists.
    if (found24 && tryPn532At(0x24)) {
        return true;
    }

    // Address 0x28 is characteristic of MFRC522/Si512-class I2C frontends.
    // Try that register protocol before treating it as an unusual PN532.
    if (found28 && tryRc522CompatibleAt28()) {
        return true;
    }

    // Last compatibility test: a board could theoretically move PN532 frames
    // to 0x28 while retaining the PN532 host protocol.
    if (found28 && tryPn532At(0x28)) {
        return true;
    }

    nfcBackend = NfcBackend::None;
    nfcDiagnostic = found28
        ? "0x28 risponde, ma driver Si512/PN532 falliscono"
        : "0x24 risponde, ma protocollo PN532 fallisce";
    return false;
}

static void setWifi(bool enabled) {
    wifiEnabled = enabled;
    if (enabled) {
        WiFi.mode(WIFI_STA);
        serialLog("[radio] Wi-Fi STA enabled (not connected)");
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        serialLog("[radio] Wi-Fi disabled");
    }
}

static void setBluetooth(bool enabled) {
    btEnabled = enabled;
    if (enabled) {
        btStart();
        serialLog("[radio] Bluetooth controller enabled");
    } else {
        btStop();
        serialLog("[radio] Bluetooth controller disabled");
    }
}

static void setNfc(bool enabled) {
    nfcEnabled = enabled;
    serialLog(enabled ? "[radio] NFC scanning enabled" : "[radio] NFC scanning disabled");
}

static void openQuickSettings() {
    if (currentPage != Page::QuickSettings) {
        pageBeforeQuickSettings = currentPage;
        currentPage = Page::QuickSettings;
        fullRefresh();
    }
}

static void closeQuickSettings() {
    if (currentPage == Page::QuickSettings) {
        currentPage = pageBeforeQuickSettings;
        fullRefresh();
    }
}

static bool pointIn(int x, int y, int left, int top, int right, int bottom) {
    return x >= left && x <= right && y >= top && y <= bottom;
}

static void handleTap(int x, int y) {
    if (y <= STATUS_H) {
        if (currentPage == Page::QuickSettings) closeQuickSettings();
        else openQuickSettings();
        return;
    }

    if (currentPage == Page::Home) {
        if (pointIn(x, y, 30, 215, 240, 430)) {
            currentPage = Page::NFC;
            fullRefresh();
        }
        return;
    }

    if (currentPage == Page::NFC) {
        if (pointIn(x, y, 0, 58, 155, 125)) {
            currentPage = Page::Home;
            fullRefresh();
        }
        return;
    }

    if (currentPage == Page::QuickSettings) {
        if (pointIn(x, y, 28, 155, 512, 255)) {
            setNfc(!nfcEnabled);
            fullRefresh();
        } else if (pointIn(x, y, 28, 275, 512, 375)) {
            setWifi(!wifiEnabled);
            fullRefresh();
        } else if (pointIn(x, y, 28, 395, 512, 495)) {
            setBluetooth(!btEnabled);
            fullRefresh();
        }
    }
}

static void processTouch() {
    if (millis() - lastTouchPoll < TOUCH_POLL_MS) return;
    lastTouchPoll = millis();

    if (!M5.TP.available()) return;
    M5.TP.update();

    bool down = !M5.TP.isFingerUp();
    if (down) {
        int x = M5.TP.readFingerX(0);
        int y = M5.TP.readFingerY(0);

        if (!fingerWasDown) {
            fingerWasDown = true;
            touchStartX = x;
            touchStartY = y;
        }

        touchLastX = x;
        touchLastY = y;
        return;
    }

    if (fingerWasDown) {
        fingerWasDown = false;
        int dx = touchLastX - touchStartX;
        int dy = touchLastY - touchStartY;

        if (touchStartY <= SWIPE_START_MAX_Y && dy >= SWIPE_MIN_DISTANCE) {
            openQuickSettings();
            return;
        }

        if (currentPage == Page::QuickSettings && dy <= -SWIPE_MIN_DISTANCE) {
            closeQuickSettings();
            return;
        }

        if (abs(dx) < 35 && abs(dy) < 35) {
            handleTap(touchLastX, touchLastY);
        }
    }
}

static void publishCard(const uint8_t *uid, uint8_t uidLength,
                        uint16_t atqa, uint8_t sak, const String &type) {
    uint32_t hash = uidHash(uid, uidLength);
    bool changed = !lastCard.valid || hash != lastUidHash ||
                   lastCard.atqa != atqa || lastCard.sak != sak ||
                   lastCard.type != type;

    lastCard.valid = true;
    lastCard.uidLength = min<uint8_t>(uidLength, sizeof(lastCard.uid));
    memcpy(lastCard.uid, uid, lastCard.uidLength);
    lastCard.atqa = atqa;
    lastCard.sak = sak;
    lastCard.type = type;
    lastCard.seenAt = millis();
    lastUidHash = hash;

    Serial.println();
    Serial.println("----- NFC TAG -----");
    Serial.println("Backend: " + backendName());
    Serial.println("Type   : " + type);
    Serial.println("UID    : " + uidToString(uid, uidLength));
    Serial.printf("ATQA   : 0x%04X\n", atqa);
    Serial.printf("SAK    : 0x%02X\n", sak);
    Serial.println("-------------------");

    if (changed && currentPage == Page::NFC) {
        fullRefresh();
    }
}

static void scanPn532() {
    uint8_t uid[10] = {0};
    uint8_t uidLength = 0;

    bool found = nfc.readPassiveTargetID(
        PN532_MIFARE_ISO14443A,
        uid,
        &uidLength,
        80,
        false
    );

    if (!found || uidLength == 0) return;

    uint8_t bufferLen = 0;
    uint8_t *raw = nfc.getBuffer(&bufferLen);
    uint16_t atqa = 0;
    uint8_t sak = 0;

    if (raw && bufferLen >= 6) {
        atqa = ((uint16_t)raw[2] << 8) | raw[3];
        sak = raw[4];
    }

    publishCard(uid, uidLength, atqa, sak,
                classifyIso14443A(atqa, sak, uidLength));
}

static void scanRc522Compatible() {
    static uint32_t lastRfDiag = 0;

    byte atqaBytes[2] = {0, 0};
    byte atqaSize = sizeof(atqaBytes);

    // WUPA is deliberate here: it also detects a PICC that was left in HALT
    // after a previous successful read.
    MFRC522::StatusCode request = rc522.PICC_WakeupA(atqaBytes, &atqaSize);

    if (request != MFRC522Constants::STATUS_OK &&
        request != MFRC522Constants::STATUS_COLLISION) {

        if (millis() - lastRfDiag >= 2000) {
            lastRfDiag = millis();
            uint8_t txControl = rc522Driver.PCD_ReadRegister(MFRC522Constants::TxControlReg);
            uint8_t rfCfg = rc522Driver.PCD_ReadRegister(MFRC522Constants::RFCfgReg);
            Serial.printf("[rf] no tag; WUPA status=%u TxControl=0x%02X RFCfg=0x%02X\n",
                          (unsigned)request, txControl, rfCfg);
        }
        return;
    }

    Serial.printf("[rf] PICC response, ATQA=%02X %02X status=%u\n",
                  atqaBytes[0], atqaBytes[1], (unsigned)request);

    MFRC522::StatusCode select = rc522.PICC_Select(&rc522.uid);
    if (select != MFRC522Constants::STATUS_OK) {
        Serial.printf("[rf] anticollision/select failed status=%u\n", (unsigned)select);
        return;
    }

    uint8_t uidLength = min<uint8_t>(rc522.uid.size, 10);
    uint8_t uid[10] = {0};
    memcpy(uid, rc522.uid.uidByte, uidLength);

    uint16_t atqa = 0;
    if (atqaSize >= 2) {
        atqa = ((uint16_t)atqaBytes[0] << 8) | atqaBytes[1];
    }

    uint8_t sak = rc522.uid.sak;
    MFRC522::PICC_Type piccType = rc522.PICC_GetType(sak);

    publishCard(uid, uidLength, atqa, sak, classifyRc522Type(piccType));

    rc522.PICC_HaltA();
    rc522.PCD_StopCrypto1();
}

static void scanNfc() {
    if (!nfcEnabled || !nfcReaderReady) return;
    if (millis() - lastNfcScan < NFC_SCAN_INTERVAL_MS) return;
    lastNfcScan = millis();

    switch (nfcBackend) {
        case NfcBackend::PN532:
            scanPn532();
            break;
        case NfcBackend::MFRC522Compat:
            scanRc522Compatible();
            break;
        default:
            break;
    }
}

static void goToSleep() {
    serialLog("[power] sleep requested");

    canvas.fillCanvas(15);
    drawStatusBar();
    canvas.setTextColor(0);
    canvas.setTextSize(4);
    canvas.drawString("Sleeping", 130, 390);
    canvas.setTextSize(1);
    canvas.setTextColor(5);
    canvas.drawString("Press the PWR button to wake PaperJam OS.", 98, 455);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (btEnabled) btStop();

    delay(250);

    // M5EPD handles first-generation M5Paper shutdown and RTC/power circuitry.
    // With no timer supplied, wake is performed with the physical PWR button.
    M5.shutdown();
}

void setup() {
    Serial.begin(115200);
    delay(100);

    M5.begin();
    M5.EPD.SetRotation(90);
    M5.TP.SetRotation(90);
    M5.RTC.begin();
    M5.BatteryADCBegin();

    canvas.createCanvas(SCREEN_W, SCREEN_H);
    canvas.setTextWrap(false);

    M5.EPD.Clear(true);

    bootFrame(8, "display framebuffer");
    delay(100);
    bootFrame(22, "GT911 touch");
    delay(100);
    bootFrame(36, "RTC + battery ADC");
    delay(100);

    setWifi(false);
    setBluetooth(false);

    bootFrame(52, "radios safe state");
    delay(100);

    nfcReaderReady = initNfcReader();

    bootFrame(74, nfcReaderReady ? (backendName() + " detected") : "NFC reader unavailable");
    delay(120);
    bootFrame(88, "PaperJam UI services");
    delay(120);
    bootFrame(100, "userspace ready");
    delay(260);

    currentPage = Page::Home;
    fullRefresh();

    Serial.println();
    Serial.println("PaperJam OS " PAPERJAM_VERSION);
    Serial.println("Ready.");
}

void loop() {
    M5.update();

    if (M5.BtnP.wasPressed()) {
        goToSleep();
    }

    // Side buttons are reserved for system navigation in future builds.
    // In this alpha the left button returns Home and right opens Quick Settings.
    if (M5.BtnL.wasPressed()) {
        currentPage = Page::Home;
        fullRefresh();
    }

    if (M5.BtnR.wasPressed()) {
        openQuickSettings();
    }

    processTouch();
    scanNfc();

    // RTC/status bar is minute-resolution. Repaint only once per minute so
    // the e-paper is not continuously refreshed.
    if (millis() - lastStatusRefresh >= STATUS_REFRESH_MS) {
        fullRefresh();
    }

    delay(4);
}
