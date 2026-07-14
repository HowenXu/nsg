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

bool UBlox::sendConfig(HardwareSerial& ublox) {
    // chapter 3.10.18 UBX-CFG-VALSET (0x06 0x8a)
    uint8_t cfgPayload[] = {
        0x00,        // Version, fixed to 0x00
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
        0x12, 0x00, 0x31, 0x10, 0x01,
        // NOTE: configs above will trigger a reset to the GNSS subsystem

        // PSMCT (Cyclic Tracking), ref MAX-M10S integration manual
        // our board use M10050-KB chip, but that document requires NDA
        // 0. When the receiver is switched on, it first enters the "Acquisition" state (ACQ).
        //    A larger number of signals tracked later helps the receiver to remain in the "POT" state (Power Optimized Tracking)
        //    if some signals get blocked and are lost. This may reduce the overall power consumption.
        // 1. The ACQ state will end if:
        //    1a) we get a valid fix, enter Tracking mode
        //    1b) timeout on CFG-PM-MAXACQTIME, enter Inactive for Search mode
        // 2. If we are in Inactive for Search mode, we will trigger ACQ every CFG-PM-ACQPERIOD,
        //    aka the interval between two ACQ start is equal to CFG-PM-ACQPERIOD.
        // 3. If we are in Tracking mode, we will keep tracking for CFG-PM-ONTIME,
        //    then move into POT mode, this mode will not download any data from satellite.
        // 4. If the signal get lost or become weak, we go back to Tracking mode for CFG-PM-ONTIME,
        //    then move into POT mode again.
        // 5. If we still failed to get valid fix in Tracking mode, we go back to ACQ mode.

        // CFG-PM-OPERATEMODE 0x20d00001, set to FULL
        // NOTE We DO NOT use PSMCT because position drift is observed when using it
        0x01, 0x00, 0xd0, 0x20, 0x00  // 0 -> FULL, 1 -> PSMOO, 2 -> PSMCT
        // CFG-PM-MAXACQTIME  0x20d00007
        // max time (seconds) spend when searching for satellites
        // if reached, goes into Inactive for Search mode
        // set to 180s for cold start
        // 0x07, 0x00, 0xd0, 0x20, 0xb4,
        // CFG-PM-ACQPERIOD 0x40d00003
        // period (seconds) that triggers another search
        // set to 240s, search every 4 minutes, each time search 3 minutes
        // 0x03, 0x00, 0xd0, 0x40, 0xf0, 0x00, 0x00, 0x00,
        // CFG-PM-ONTIME 0x30d00005
        // if get a valid fix, keep tracking for this long (seconds)
        // set to 30s to download data
        // 0x05, 0x00, 0xd0, 0x30, 0x1e, 0x00
    };

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
