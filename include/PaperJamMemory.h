#pragma once
#include <Arduino.h>
#include <SD.h>
#include <MFRC522v2.h>

// Read-only dumps. Only explicitly supplied keys are attempted.
// /paperjam/keys.txt: one key per line, A:001122334455 or B:001122334455.
// No brute force, key recovery, card writes or sector-trailer modification.
namespace PaperJamMemory {
struct Result {
  bool success = false;
  uint16_t readBlocks = 0;
  uint16_t lockedSectors = 0;
  String path;
  String message;
};
struct Credential {
  uint8_t command;
  MFRC522::MIFARE_Key key;
};
inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline uint8_t loadKeys(Credential *keys, uint8_t capacity) {
  File file = SD.open("/paperjam/keys.txt", FILE_READ);
  if (!file) return 0;
  uint8_t count = 0;
  while (file.available() && count < capacity) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() != 14 || line[1] != ':' ||
        (line[0] != 'A' && line[0] != 'B')) continue;
    Credential entry{};
    entry.command = (line[0] == 'A')
        ? MFRC522Constants::PICC_CMD_MF_AUTH_KEY_A
        : MFRC522Constants::PICC_CMD_MF_AUTH_KEY_B;
    bool valid = true;
    for (int i = 0; i < 6; ++i) {
      int high = hexNibble(line[2 + i * 2]);
      int low = hexNibble(line[3 + i * 2]);
      if (high < 0 || low < 0) { valid = false; break; }
      entry.key.keyByte[i] = (uint8_t)((high << 4) | low);
    }
    if (valid) keys[count++] = entry;
  }
  file.close();
  return count;
}
inline String uidName(const MFRC522::Uid &uid) {
  String s;
  for (uint8_t i = 0; i < uid.size; ++i) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", uid.uidByte[i]);
    s += buf;
  }
  return s;
}
inline void printHex(File &file, const byte *data, uint8_t length) {
  for (uint8_t i = 0; i < length; ++i) {
    if (i) file.print(' ');
    if (data[i] < 16) file.print('0');
    file.print(data[i], HEX);
  }
  file.println();
}
inline Result dump(MFRC522 &reader, MFRC522::PICC_Type type) {
  Result result;
  // Legacy M5EPD initializes the SD interface in M5.begin().
  if (SD.cardType() == CARD_NONE) {
    result.message = "SD NOT FOUND";
    return result;
  }
  if (!SD.exists("/paperjam")) SD.mkdir("/paperjam");
  if (!SD.exists("/paperjam/dumps")) SD.mkdir("/paperjam/dumps");
  result.path = "/paperjam/dumps/" + uidName(reader.uid) +
                "_" + String(millis()) + ".txt";
  File out = SD.open(result.path.c_str(), FILE_WRITE);
  if (!out) {
    result.message = "SD FILE ERROR";
    return result;
  }
  out.println("PaperJAM OS NFC read-only memory dump");
  out.print("UID: "); out.println(uidName(reader.uid));
  out.print("SAK: 0x"); out.println(reader.uid.sak, HEX);
  out.println("Protected blocks are omitted, never fabricated.");

  bool classic = type == MFRC522Constants::PICC_TYPE_MIFARE_MINI ||
                 type == MFRC522Constants::PICC_TYPE_MIFARE_1K ||
                 type == MFRC522Constants::PICC_TYPE_MIFARE_4K;
  if (classic) {
    Credential keys[12];
    const uint8_t count = loadKeys(keys, 12);
    out.print("Configured keys: "); out.println(count);
    const uint8_t sectors = type == MFRC522Constants::PICC_TYPE_MIFARE_4K
        ? 40 : type == MFRC522Constants::PICC_TYPE_MIFARE_MINI ? 5 : 16;
    if (!count) {
      out.println("NO KEYS: create /paperjam/keys.txt (A: or B: + 12 hex digits)");
      result.message = "NO KEYS ON SD";
    } else {
      for (uint8_t sector = 0; sector < sectors; ++sector) {
        uint8_t start = sector < 32 ? sector * 4 : 128 + (sector - 32) * 16;
        uint8_t blocks = sector < 32 ? 4 : 16;
        bool authenticated = false;
        for (uint8_t k = 0; k < count; ++k) {
          // After failed authentication a card may need to be reselected.
          byte atqa[2], atqaLen = sizeof(atqa);
          reader.PCD_StopCrypto1();
          if (reader.PICC_WakeupA(atqa, &atqaLen) ==
              MFRC522Constants::STATUS_OK) reader.PICC_Select(&reader.uid);
          if (reader.PCD_Authenticate(keys[k].command, start + blocks - 1,
                                      &keys[k].key, &reader.uid) ==
              MFRC522Constants::STATUS_OK) {
            authenticated = true;
            break;
          }
        }
        out.printf("SECTOR %u: %s\n", sector,
                   authenticated ? "AUTH OK" : "LOCKED/KEY NOT ACCEPTED");
        if (!authenticated) { ++result.lockedSectors; continue; }
        for (uint8_t block = 0; block < blocks; ++block) {
          // Never export sector trailers: they contain access configuration
          // and can expose Key B on some card layouts.
          if (block == blocks - 1) {
            out.printf("BLOCK %u: [TRAILER OMITTED]\n", start + block);
            continue;
          }
          byte data[18] = {0}, length = sizeof(data);
          auto status = reader.MIFARE_Read(start + block, data, &length);
          out.printf("BLOCK %u: ", start + block);
          if (status == MFRC522Constants::STATUS_OK && length >= 16) {
            printHex(out, data, 16);
            ++result.readBlocks;
          } else {
            out.printf("READ ERROR %u\n", (unsigned)status);
          }
        }
        reader.PCD_StopCrypto1();
      }
      result.message = result.readBlocks ? "DUMP SAVED" : "NO READABLE BLOCKS";
    }
  } else if (type == MFRC522Constants::PICC_TYPE_MIFARE_UL) {
    // Conservative first-stage scan: the first sixteen 4-byte pages.
    // READ returns four pages (16 bytes) at once.
    for (uint8_t page = 0; page < 16; page += 4) {
      byte data[18] = {0}, length = sizeof(data);
      auto status = reader.MIFARE_Read(page, data, &length);
      if (status != MFRC522Constants::STATUS_OK || length < 16) {
        out.printf("PAGES %u-%u: READ ERROR %u\n",
                   page, page + 3, (unsigned)status);
        break;
      }
      for (uint8_t n = 0; n < 4; ++n) {
        out.printf("PAGE %u: ", page + n);
        printHex(out, data + n * 4, 4);
        ++result.readBlocks;
      }
    }
    result.message = result.readBlocks ? "DUMP SAVED" : "NO READABLE PAGES";
  } else {
    out.println("UNSUPPORTED MEMORY TYPE: UID metadata only");
    result.message = "TYPE NOT SUPPORTED";
  }
  reader.PCD_StopCrypto1();
  out.flush();
  out.close();
  result.success = true;
  return result;
}
} // namespace PaperJamMemory
