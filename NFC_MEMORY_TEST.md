# NFC Memory Lab — experimental read-only build

Branch: `feature/nfc-memory-sd` (based on `fix/si512-initiator-rfdiag`).

## Hardware
M5Paper (first generation), Si512 at 0x28 on I2C Port B, reset GPIO18 on Port C, microSD inserted in M5Paper.
Prepare a FAT32 microSD. The existing M5EPD startup handles SD initialization when enabled.

## Use
1. Flash firmware from this branch and insert microSD.
2. On the SD create directory `/paperjam`.
3. For your own MIFARE Classic test cards, optionally create `/paperjam/keys.txt` containing one credential per line:
   ```
   A:001122334455
   B:112233445566
   ```
   Replace examples with **actual keys you are authorized to use**. They are not default recommendations. Up to 12 entries. Keys are kept on the microSD as plain text; keep the card secure and delete the file when no longer needed. No key-recovery, cracking, brute-force, sector trailer export or NFC writes are implemented.
4. Launch NFC, hold a card steady, tap **READ + SAVE SD**.
5. Results are written to `/paperjam/dumps/<UID>_<millis>.txt`. The dump reports inaccessible sectors and read errors instead of fabricating data.

## Coverage
- MIFARE Classic Mini/1K/4K: read data blocks of sectors where an explicitly provided Key A or Key B authenticates; sector trailer deliberately omitted.
- MIFARE Ultralight-family recognized by MFRC522 library: first 16 pages only (read-only); extended pages and NDEF decoding not implemented yet.
- Other card types: UID and SAK metadata only.

## Diagnostics and known limitations
- Firmware compiles and hardware compatibility **not yet independently verified**; preserve the working NFC firmware on the main branch.
- If the card is moved after UID detection, the save operation stops instead of reading another UID.
- Stored files may contain private data; protect the removable storage.
- A failed authentication can disturb the PICC selection state; the implementation attempts to reselect before each supplied key. Inspect serial output when behavior differs.
- The M5EPD library is legacy, and SD pin/SPI behavior is hardware-dependent; test with the actual device.
