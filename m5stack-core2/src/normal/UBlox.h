#ifndef UBLOX_H
#define UBLOX_H

#include <HardwareSerial.h>

namespace UBlox {
// send general message to ublox
// reference: u-blox M10 SPG 5.10 Interface Description, chapter 3
void sendMessage(HardwareSerial& ublox, uint8_t classID, uint8_t msgID, const uint8_t* payload, uint16_t length);

// send config to enable GPS, SBAS, Galileo, BeiDou and QZSS
bool sendSatelliteConfig(HardwareSerial& ublox);

bool setBaudRate(HardwareSerial& ublox, uint32_t baudRate);

bool waitAck(HardwareSerial& ublox, uint8_t targetClass, uint8_t targetID, uint32_t timeoutMs);

}  // namespace UBlox

#endif