#include "PaperJamPN532I2C.h"

int8_t PaperJamPN532I2C::writeCommand(const uint8_t *header, uint8_t hlen,
                                      const uint8_t *body, uint8_t blen) {
    _command = header[0];

    _wire->beginTransmission(_address);

    writeByte(PN532_PREAMBLE);
    writeByte(PN532_STARTCODE1);
    writeByte(PN532_STARTCODE2);

    uint8_t length = hlen + blen + 1;
    writeByte(length);
    writeByte((uint8_t)(~length + 1));

    writeByte(PN532_HOSTTOPN532);
    uint8_t sum = PN532_HOSTTOPN532;

    for (uint8_t i = 0; i < hlen; ++i) {
        if (!writeByte(header[i])) return PN532_INVALID_FRAME;
        sum += header[i];
    }

    for (uint8_t i = 0; i < blen; ++i) {
        if (!writeByte(body[i])) return PN532_INVALID_FRAME;
        sum += body[i];
    }

    writeByte((uint8_t)(~sum + 1));
    writeByte(PN532_POSTAMBLE);

    uint8_t tx = _wire->endTransmission();
    if (tx != 0) return PN532_TIMEOUT;

    return readAckFrame();
}

int8_t PaperJamPN532I2C::readAckFrame() {
    static const uint8_t expectedAck[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

    // Some clone boards are slower than the original Seeed defaults.
    const uint32_t start = millis();
    while (millis() - start < 120) {
        size_t count = _wire->requestFrom((uint16_t)_address,
                                         sizeof(expectedAck) + 1,
                                         true);
        if (count >= sizeof(expectedAck) + 1) {
            int status = readByte();
            if (status >= 0 && (status & 0x01)) {
                uint8_t ack[sizeof(expectedAck)];
                for (size_t i = 0; i < sizeof(expectedAck); ++i) {
                    int v = readByte();
                    if (v < 0) return PN532_INVALID_ACK;
                    ack[i] = (uint8_t)v;
                }

                if (memcmp(ack, expectedAck, sizeof(expectedAck)) == 0) {
                    return 0;
                }
                return PN532_INVALID_ACK;
            }
        }

        while (_wire->available()) _wire->read();
        delay(2);
    }

    return PN532_TIMEOUT;
}

int16_t PaperJamPN532I2C::readResponse(uint8_t buf[], uint8_t len, uint16_t timeout) {
    const uint32_t start = millis();

    while (true) {
        size_t count = _wire->requestFrom((uint16_t)_address,
                                         (size_t)len + 2,
                                         true);

        if (count > 0) {
            int status = readByte();
            if (status >= 0 && (status & 0x01)) break;
        }

        while (_wire->available()) _wire->read();

        if (timeout != 0 && millis() - start > timeout) {
            return PN532_TIMEOUT;
        }
        delay(2);
    }

    if (readByte() != PN532_PREAMBLE ||
        readByte() != PN532_STARTCODE1 ||
        readByte() != PN532_STARTCODE2) {
        return PN532_INVALID_FRAME;
    }

    int lengthRaw = readByte();
    int lengthChecksum = readByte();
    if (lengthRaw < 0 || lengthChecksum < 0) return PN532_INVALID_FRAME;

    uint8_t length = (uint8_t)lengthRaw;
    if ((uint8_t)(length + (uint8_t)lengthChecksum) != 0) {
        return PN532_INVALID_FRAME;
    }

    int tfi = readByte();
    int responseCommand = readByte();
    uint8_t expectedCommand = (uint8_t)(_command + 1);

    if (tfi != PN532_PN532TOHOST || responseCommand != expectedCommand) {
        return PN532_INVALID_FRAME;
    }

    if (length < 2) return PN532_INVALID_FRAME;
    length -= 2;

    if (length > len) return PN532_NO_SPACE;

    uint8_t sum = PN532_PN532TOHOST + expectedCommand;
    for (uint8_t i = 0; i < length; ++i) {
        int v = readByte();
        if (v < 0) return PN532_INVALID_FRAME;
        buf[i] = (uint8_t)v;
        sum += buf[i];
    }

    int checksum = readByte();
    if (checksum < 0 || (uint8_t)(sum + (uint8_t)checksum) != 0) {
        return PN532_INVALID_FRAME;
    }

    readByte(); // postamble
    return length;
}
