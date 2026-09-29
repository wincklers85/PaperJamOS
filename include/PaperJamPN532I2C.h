#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <PN532Interface.h>

// PN532 I2C transport with runtime-configurable 7-bit address.
// Based on the Seeed Studio PN532 I2C transport (BSD licensed),
// adapted for PaperJam OS to support HW-147C variants/clones that
// acknowledge on 0x28 instead of the standard PN532 address 0x24.
class PaperJamPN532I2C : public PN532Interface {
public:
    explicit PaperJamPN532I2C(TwoWire &wire, uint8_t address = 0x24)
        : _wire(&wire), _address(address), _command(0) {}

    void setAddress(uint8_t address) { _address = address; }
    uint8_t getAddress() const { return _address; }

    void begin() override {
        // The bus is configured by PaperJam OS with the selected SDA/SCL pins.
        // Do not call Wire.begin() here or the ESP32 core may attempt to use
        // default pins instead of the active Port B mapping.
    }

    void wakeup() override {
        _wire->beginTransmission(_address);
        delay(20);
        _wire->endTransmission();
    }

    int8_t writeCommand(const uint8_t *header, uint8_t hlen,
                        const uint8_t *body = nullptr, uint8_t blen = 0) override;

    int16_t readResponse(uint8_t buf[], uint8_t len, uint16_t timeout = 1000) override;

private:
    TwoWire *_wire;
    uint8_t _address;
    uint8_t _command;

    int8_t readAckFrame();

    inline uint8_t writeByte(uint8_t data) {
        return _wire->write(data);
    }

    inline int readByte() {
        return _wire->read();
    }
};
