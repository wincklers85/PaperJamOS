#pragma once

#ifndef PAPERJAM_VERSION
#define PAPERJAM_VERSION "0.0.1-alpha"
#endif

namespace PaperJamConfig {
constexpr int SCREEN_W = 540;
constexpr int SCREEN_H = 960;
constexpr int STATUS_H = 58;

// M5Paper first generation physical buttons.
constexpr int BTN_RIGHT_PIN = 37;
constexpr int BTN_POWER_PIN = 38;
constexpr int BTN_LEFT_PIN  = 39;

// NFC reader on M5Paper Port B using a second I2C bus.
// Wiring: SDA = GPIO33, SCL = GPIO26.
// Hardware reset uses GPIO18 (free Port C pin) for HW-147C / Si512-class boards.
constexpr int NFC_SDA_PIN = 33;
constexpr int NFC_SCL_PIN = 26;
constexpr int NFC_RST_PIN = 18;
constexpr uint32_t NFC_I2C_FREQ = 100000;

constexpr uint32_t NFC_SCAN_INTERVAL_MS = 350;
constexpr uint32_t STATUS_REFRESH_MS = 60000;
constexpr uint32_t TOUCH_POLL_MS = 18;

constexpr int SWIPE_START_MAX_Y = 130;
constexpr int SWIPE_MIN_DISTANCE = 95;
}
