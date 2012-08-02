#ifndef FACTORYTEST_H
#define FACTORYTEST_H

#include <stdint.h>
#include "usbprotocol.h"
#include "radio.h"

class FactoryTest
{
public:
    static const unsigned UART_MAX_COMMAND_LEN = 6;
    static const unsigned UART_LEN_INDEX = 0;   // first byte is length
    static const unsigned UART_CMD_INDEX = 1;   // second byte is opcode

    static void init();
    static void onUartIsr();
    static void usbHandler(const USBProtocolMsg &m);

    /*
     * RF test handlers.
     *
     * Since we're not sending meaningful protocol data, we treat any ACK,
     * with payload or not, as a success.
     *
     * Because the cube is toggling between 2 channels in its default
     * disconnected state, we send a packet on each, and count it as a
     * failure if we don't get an ACK from either one.
     */
    static void produce(PacketTransmission &tx);
    static void ALWAYS_INLINE ackWithPacket(const PacketBuffer &packet) {
        rfSuccessCount++;
        if (rfTransmissionsRemaining)
            rfTransmissionsRemaining--;
    }
    static void ALWAYS_INLINE timeout() {
        if (rfTransmissionsRemaining)
            rfTransmissionsRemaining--;
    }
    static void ALWAYS_INLINE ackEmpty() {
        rfSuccessCount++;
        if (rfTransmissionsRemaining)
            rfTransmissionsRemaining--;
    }

private:
    static uint8_t commandBuf[UART_MAX_COMMAND_LEN];
    static uint8_t commandLen;

    static volatile uint16_t rfTransmissionsRemaining;
    static uint16_t rfSuccessCount;
    static RadioAddress rfTestAddr;
    static uint8_t rfTestAddrPrimaryChannel;
    static const uint8_t RF_TEST_BYTE = 0x11;

    static void handleRfPacketComplete();

    typedef void(*TestHandler)(uint8_t argc, const uint8_t *args);
    static const TestHandler handlers[];

    static void nrfCommsHandler(uint8_t argc, const uint8_t *args);
    static void flashCommsHandler(uint8_t argc, const uint8_t *args);
    static void flashReadWriteHandler(uint8_t argc, const uint8_t *args);
    static void ledHandler(uint8_t argc, const uint8_t *args);
    static void uniqueIdHandler(uint8_t argc, const uint8_t *args);
    static void volumeCalibrationHandler(uint8_t argc, const uint8_t *args);
    static void batteryCalibrationHandler(uint8_t argc, const uint8_t *args);
    static void homeButtonHandler(uint8_t argc, const uint8_t *args);
    static void shutdownHandler(uint8_t argc, const uint8_t *args);
    static void audioTestHandler(uint8_t argc, const uint8_t *args);
    static void bootloadRequestHandler(uint8_t argc, const uint8_t *args);
    static void rfPacketTestHandler(uint8_t argc, const uint8_t *args);
};

#endif // FACTORYTEST_H
