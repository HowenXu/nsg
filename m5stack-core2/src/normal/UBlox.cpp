#include "UBlox.h"

void UBlox::sendMessage(HardwareSerial& ublox, uint8_t classID, uint8_t msgID, const uint8_t* payload, uint16_t length) {
    // fixed header
    ublox.write(0xb5);
    ublox.write(0x62);
    ublox.write(classID);
    ublox.write(msgID);
    // length
    ublox.write(length & 0xFF);
    ublox.write((length >> 8) & 0xFF);

    // checksum include class id, message id, all the way to the end of the payload.
    // ckA and ckB start with zero, for each byte:
    // ckA = ckA + byte
    // ckB = ckB + ckA
    uint8_t ckA = 0, ckB = 0;
    // lambda for calculating checksum
    auto addChecksum = [&](uint8_t b) {
        ckA = ckA + b;
        ckB = ckB + ckA;
    };

    // add headers to checksum
    addChecksum(classID);
    addChecksum(msgID);
    addChecksum(length & 0xFF);
    addChecksum((length >> 8) & 0xFF);

    // send payload, while calculating checksum
    for (uint16_t i = 0; i < length; i++) {
        ublox.write(payload[i]);
        addChecksum(payload[i]);
    }

    // finally send checksum
    ublox.write(ckA);
    ublox.write(ckB);
    ublox.flush();
}

bool UBlox::sendSatelliteConfig(HardwareSerial& ublox) {
    // chapter 3.10.18 UBX-CFG-VALSET (0x06 0x8a)
    uint8_t cfgPayload[] = {0x00,        // Version, fixed to 0x00
                            0x03,        // Layers: bit0=RAM, bit1=BBR, bit2=FLASH, we don't have flash
                            0x00, 0x00,  // Reserved, must set to 0

                            // chapter 4.5 Configuration data
                            // each group is 4 bytes key + 1/2/4/8 bytes data
                            // no alignment or padding is required
                            // chapter 4.9.19 CFG-SIGNAL: Satellite systems (GNSS) signal configuration
                            // CFG-SIGNAL-GPS_ENA 0x1031001f L - - GPS enable
                            0x1F, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-GPS_L1CA_ENA 0x10310001 L - - GPS L1C/A
                            0x01, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-SBAS_ENA 0x10310020 L - - SBAS enable
                            0x20, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-SBAS_L1CA_ENA 0x10310005 L - - SBAS L1C/A
                            0x05, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-GAL_ENA 0x10310021 L - - Galileo enable
                            0x21, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-GAL_E1_ENA 0x10310007 L - - Galileo E1
                            0x07, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-BDS_ENA 0x10310022 L - - BeiDou Enable
                            0x22, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-BDS_B1_ENA 0x1031000d L - - BeiDou B1I
                            0x0d, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-QZSS_ENA 0x10310024 L - - QZSS enable
                            0x24, 0x00, 0x31, 0x10, 0x01,
                            // CFG-SIGNAL-QZSS_L1CA_ENA 0x10310012 L - - QZSS L1C/A
                            0x12, 0x00, 0x31, 0x10, 0x01};  // NOTE: these configs will trigger a reset to the GNSS subsystem

    // UBX-CFG-VALSET (Class = 0x06, ID = 0x8A)
    sendMessage(ublox, 0x06, 0x8A, cfgPayload, (uint16_t)sizeof(cfgPayload));
    return waitAck(ublox, 0x06, 0x8A, 1000);
}

bool UBlox::setBaudRate(HardwareSerial& ublox, uint32_t baudRate) {
    // chapter 3.10.18 UBX-CFG-VALSET (0x06 0x8a)
    uint8_t cfgPayload[] = {0x00,        // Version, fixed to 0x00
                            0x03,        // Layers: bit0=RAM, bit1=BBR, bit2=FLASH, we don't have flash
                            0x00, 0x00,  // Reserved, must set to 0

                            // chapter 4.5 Configuration data
                            // each group is 4 bytes key + 1/2/4/8 bytes data
                            // no alignment or padding is required
                            // chapter 4.9.25 CFG-UART1: Configuration of the UART1 interface
                            // CFG-UART1-BAUDRATE 0x40520001 U4 - - The baud rate that should be configured on the UART1
                            0x01, 0x00, 0x52, 0x40,  // key
                            static_cast<uint8_t>(baudRate), static_cast<uint8_t>(baudRate >> 8), static_cast<uint8_t>(baudRate >> 16),
                            static_cast<uint8_t>(baudRate >> 24)};

    // UBX-CFG-VALSET (Class = 0x06, ID = 0x8A)
    sendMessage(ublox, 0x06, 0x8A, cfgPayload, (uint16_t)sizeof(cfgPayload));
    return waitAck(ublox, 0x06, 0x8A, 1000);
}

// Either get a UBX-ACK-ACK or UBX-ACK-NAK
bool UBlox::waitAck(HardwareSerial& ublox, uint8_t targetClass, uint8_t targetID, uint32_t timeoutMs) {
    uint32_t startTime = millis();
    uint8_t state = 0;
    bool isAck = false;

    while (millis() - startTime < timeoutMs) {
        if (ublox.available()) {
            uint8_t c = ublox.read();
            // a simple state machine to match ublox reply from NMEA sentences
            switch (state) {
                // header 1: 0xb5
                case 0:
                    if (c == 0xb5)
                        state = 1;
                    else
                        state = 0;
                    break;
                // header 2: 0x62
                case 1:
                    if (c == 0x62)
                        state = 2;
                    else
                        state = (c == 0xb5) ? 1 : 0;
                    break;
                // class id 0x05 for UBX-ACK-XXX
                case 2:
                    if (c == 0x05)
                        state = 3;
                    else
                        state = 0;
                    break;
                // message id, 0x00 for NAK, 0x01 for ACK
                case 3:
                    if (c == 0x01) {
                        isAck = true;
                        state = 4;
                    } else if (c == 0x00) {
                        isAck = false;
                        state = 4;
                    } else
                        state = 0;
                    break;
                // length LSB, should always be 0x02
                case 4:
                    if (c == 0x02)
                        state = 5;
                    else
                        state = 0;
                    break;
                // length MSB, should always be 0x00
                case 5:
                    if (c == 0x00)
                        state = 6;
                    else
                        state = 0;
                    break;
                // check replied message's class id
                case 6:
                    if (c == targetClass)
                        state = 7;
                    else
                        state = (c == 0xb5) ? 1 : 0;
                    break;
                // check replied message's message id
                case 7:
                    if (c == targetID) {
                        // skip checksum because I'm lazy
                        return isAck;
                    }
                    state = (c == 0xb5) ? 1 : 0;
                    break;
            }
        } else {
            yield();
        }
    }
    return false;
}
