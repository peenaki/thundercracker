/*
 * Thundercracker Firmware -- Confidential, not for redistribution.
 * Copyright <c> 2013 Sifteo, Inc. All rights reserved.
 */

/*
 * Driver for the Nordic nRF8001 Bluetooth Low Energy controller
 */

#include "nrf8001/nrf8001.h"
#include "nrf8001/services.h"
#include "nrf8001/constants.h"
#include "board.h"
#include "sampleprofiler.h"
#include "systime.h"
#include "factorytest.h"
#include "flash_syslfs.h"
#include "tasks.h"

#ifdef HAVE_NRF8001

/*
 * States for our produceSystemCommand() state machine.
 */
namespace SysCS {
    enum SystemCommandState {
        // Actual states
        SetupFirst = 0,
        SetupLast = SetupFirst + NB_SETUP_MESSAGES - 1,
        Idle,           // Must follow SetupLast
        BeginBond,
        RadioReset,
        InitSysVersion,
        ChangeTimingRequest,
        Disconnect,
        ReadDynamicData,
        WriteDynamicData,
        EnterTest,
        ExitTest,
        Echo,
        DtmRX,
        DtmEnd,

        // State ordering definitions
        AfterReadDynamicData = BeginBond,
        AfterWriteDynamicData = InitSysVersion,
        AfterInitSysVersion = BeginBond,
        AfterConnect = ChangeTimingRequest,
    };
}

namespace Test {
    enum TestState {
        Idle = NRF8001::TestPhase2 + 1,
        RadioReset,
        EnterTest,
        BeginRX,
        EndRX,
        ExitTest
    };

    static const uint8_t echoData[] = {
        0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0a,
    };

    static const uint16_t dtmParams[] = {
        0x3040,         // Receiver Test, channel 0x10, length 0x10, PRBS9 packet
        (0x3 << 6),     // Test End
    };
}

void NRF8001::init()
{
    // 3 MHz maximum SPI clock according to data sheet. Mode 0, LSB first.
    const SPIMaster::Config cfg = {
        Dma::MediumPrio,
        SPIMaster::fPCLK_16 | SPIMaster::fLSBFIRST
    };

    spi.init(cfg);

    // Reset state
    txBuffer.length = 0;
    requestsPending = 0;
    dataCredits = 0;
    needStoreDynamicData = false;
    sysCommandState = SysCS::RadioReset;
    sysCommandPending = false;
    testState = Test::Idle;

    // Output pin, requesting a transaction
    reqn.setHigh();
    reqn.setControl(GPIOPin::OUT_10MHZ);

    // Input IRQ pin, beginning a (requested or spontaneous) transaction
    rdyn.setControl(GPIOPin::IN_FLOAT);
    rdyn.irqInit();

    /*
     * The RDYN level isn't valid until at least 62ms after reset, according
     * to the data sheet. This is a conservative delay, but still shorter
     * than Radio::init()'s delay.
     */
    while (SysTime::ticks() < SysTime::msTicks(80));

    // Now we can enable the IRQ
    rdyn.irqSetFallingEdge();
    rdyn.irqEnable();

    // Ask for the first transaction, so we can start the SETUP process.
    requestTransaction();

    /*
     * It's possible the chip is already ready, and we missed the falling edge
     * because it happened before the IRQ was set up. To avoid getting stuck in
     * this case, we directly pend an interrupt at this point. If the chip is in
     * fact already waiting on us, we'll have a transaction. If not, the ISR
     * will notice that RDYN is high and exit without doing any work.
     */
    rdyn.softwareInterrupt();
}

void NRF8001::isr()
{
    /*
     * This ISR triggers when there's a falling edge on RDYN.
     * This is our signal to do one SPI transaction, which consists
     * of an optional command (out) an an optional event (in).
     *
     * We currently always perform a maximum-length transaction
     * (32 bytes) in order to avoid having to split the transaction
     * into two pieces to handle the length byte and the payload
     * separately.
     */

    SampleProfiler::SubSystem s = SampleProfiler::subsystem();
    SampleProfiler::setSubsystem(SampleProfiler::BluetoothISR);

    // Acknowledge to the IRQ controller
    rdyn.irqAcknowledge();

    /*
     * Make sure the chip is actually ready. This serves two purposes:
     * rejecting very small noise spikes on the RDYN line, and (more
     * importantly) to avoid a race condition during the very first ISR
     * we service after initialization. See the comments in NRF8001::init().
     */

    if (rdyn.isLow()) {

        /*
         * Set REQN low to indicate we're ready to start the transaction.
         * Effectively, the nRF8001's virtual "chip select" is (REQN && RDYN).
         * If this ISR was due to a command rather than an event, REQN will
         * already be low and this has no effect.
         *
         * Note that this must happen prior to produceCommand(). In case
         * that function calls requestTransaction(), we must know that we're
         * already in a transaction.
         */
        reqn.setLow();

        // Populate the transmit buffer now, or set it empty if we have nothing to say.
        produceCommand();

        #ifdef TRACE
        if (txBuffer.length) {
            UART("BT Cmd >");
            Usart::Dbg.writeHex(txBuffer.command, 2);
            Usart::Dbg.put(' ');
            Usart::Dbg.writeHexBytes(txBuffer.param, txBuffer.length - 1);
            UART("\r\n");
        }
        #endif

        // Fire off the asynchronous SPI transfer. We finish up in onSpiComplete().
        STATIC_ASSERT(sizeof txBuffer == sizeof rxBuffer);
        spi.transferDma((uint8_t*) &txBuffer, (uint8_t*) &rxBuffer, sizeof txBuffer);
    }

    SampleProfiler::setSubsystem(s);
}

void NRF8001::task()
{
    /*
     * Our nRF8001 driver is almost entirely interrupt-driven, but we do need
     * filesytem access in order to load and save bonding state packaged in
     * hardware-specific "dynamic data" packets. This task handler shuttles
     * individual dyn packets to and from SysLFS.
     */

    SysLFS::Key k = static_cast<SysLFS::Key>( SysLFS::kBluetoothBase + dyn.sequence );
    int result;

    switch (dyn.state) {

        case DynStateLoadRequest:
            if (SysLFS::read(k, dyn.record.bytes(), sizeof dyn.record) < dyn.record.calculatedLength()) {
                // I/O error or bad length byte. Treat the record as missing.
                dyn.record.length = 0;
            }
            dyn.state = DynStateLoadComplete;
            break;

        case DynStateStoreRequest:
            SysLFS::write(k, dyn.record.bytes(), dyn.record.calculatedLength());
            dyn.state = DynStateStoreComplete;
            break;
    }

    // Wake up our state machine, if they were waiting on us.
    requestTransaction();
}

void NRF8001::test(unsigned phase)
{
    /*
     * A request to enter test mode.
     *
     * Set the command state to enter Test mode at the next opportunity.
     * Testing continues as each step completes.
     */

    testState = phase;
    requestTransaction();
}

void NRF8001::staticSpiCompletionHandler()
{
    NRF8001::instance.onSpiComplete();
}

void NRF8001::onSpiComplete()
{
    SampleProfiler::SubSystem s = SampleProfiler::subsystem();
    SampleProfiler::setSubsystem(SampleProfiler::BluetoothISR);

    // Done with the transaction! End our SPI request.
    reqn.setHigh();

    #ifdef TRACE
    if (rxBuffer.length) {
        UART("BT Evt <");
        Usart::Dbg.writeHex(rxBuffer.event, 2);
        Usart::Dbg.put(' ');
        Usart::Dbg.writeHexBytes(rxBuffer.param, rxBuffer.length - 1);
        UART("\r\n");
    }
    #endif

    // Handle the event we received, if any.
    // This also may call requestTransaction() to keep the cycle going.
    handleEvent();

    // Start the next pending transaction, if any. (Serialized by requestTransaction)
    if (requestsPending) {
        requestsPending = false;
        reqn.setLow();
    }

    SampleProfiler::setSubsystem(s);
}

void NRF8001::requestTransaction()
{
    /*
     * Ask for produceCommand() to be called once. This can be called from
     * Task context at any time, or from ISR context during produceCommand()
     * or handleEvent(). This is idempotent; multiple calls to requestTransaction()
     * are only guaranteed to lead to a single produceCommand() call.
     *
     * If a transaction is already in progress, this will set 'requestsPending'
     * which will cause another transaction to start in onSpiComplete(). If not,
     * we start the transaction immediately by asserting REQN.
     */

    // Critical section
    NVIC.irqDisable(IVT.NRF8001_EXTI_VEC);
    NVIC.irqDisable(IVT.NRF8001_DMA_CHAN_RX);
    NVIC.irqDisable(IVT.NRF8001_DMA_CHAN_TX);

    if (reqn.isOutputLow()) {
        // Already in a transaction. Pend another one for later.
        requestsPending = true;
    } else {
        reqn.setLow();
    }

    // End critical section
    NVIC.irqEnable(IVT.NRF8001_EXTI_VEC);
    NVIC.irqEnable(IVT.NRF8001_DMA_CHAN_RX);
    NVIC.irqEnable(IVT.NRF8001_DMA_CHAN_TX);
}

void NRF8001::produceCommand()
{
    // System commands are highest priority, but at most one can be pending at a time.
    if (!sysCommandPending && produceSystemCommand()) {
        sysCommandPending = true;
        return;
    }

    // If we can transmit, see if BTProtocol wants to.
    if (dataCredits && (openPipes & (1 << PIPE_SIFTEO_BASE_DATA_IN_TX))) {
        unsigned len = BTProtocolCallbacks::onProduceData(&txBuffer.param[1]);
        if (len) {
            txBuffer.length = len + 2;
            txBuffer.command = Op::SendData;
            txBuffer.param[0] = PIPE_SIFTEO_BASE_DATA_IN_TX;
            dataCredits--;
            return;
        }
    }

    // Nothing to do.
    txBuffer.length = 0;
}

bool NRF8001::produceSystemCommand()
{
    /*
     * Test state machine
     * 
     * Do we need to inject a test command instead of our regularly
     * requested sys command?
     */

    switch (testState) {

        case TestPhase1:
            sysCommandState = SysCS::RadioReset;
            testState = Test::RadioReset;
            break;

        case TestPhase2:
            sysCommandState = SysCS::DtmEnd;
            testState = Test::EndRX;
            break;
    }

    /*
     * Task state machine
     *
     * If a userspace task completed, take the next steps.
     */

    switch (dyn.state) {

        case DynStateStoreComplete:
            // Finished storing one dynamic data packet. Move to the next, or go back to Bonding mode.
            sysCommandState = needStoreDynamicData ? SysCS::ReadDynamicData : SysCS::AfterReadDynamicData;
            dyn.state = DynStateIdle;
            break;

        case DynStateLoadComplete:
            // Finished loading one dynamic data packet. Send it maybe.
            sysCommandState = dyn.record.length ? SysCS::WriteDynamicData : SysCS::AfterWriteDynamicData;
            dyn.state = DynStateIdle;
            break;
    }

    /*
     * Main state machine
     */

    switch (sysCommandState) {

        default:
        case SysCS::Idle:
            return false;

        case SysCS::RadioReset: {
            /*
             * Send a RadioReset command. This may well fail if we aren't setup yet,
             * but we ignore that error. If we experienced a soft reset of any kind, this
             * will ensure the nRF8001 isn't in the middle of anything.
             *
             * After this finishes, we'll start SETUP.
             */

            txBuffer.length = 1;
            txBuffer.command = Op::RadioReset;
            if (testState == Test::RadioReset) {
                sysCommandState = SysCS::EnterTest;
                testState = Test::EnterTest;
            } else {
                sysCommandState = SysCS::SetupFirst;
            }
            dataCredits = 0;
            return true;
        }

        case SysCS::SetupFirst ... SysCS::SetupLast: {
            /*
             * Send the next SETUP packet.
             * Thanks a lot, Nordic, this format is terrible.
             *
             * After SETUP completes, we'll head to the Idle state.
             * When the device finishes initializing, we'll get a DeviceStartedEvent.
             */

            static const struct {
                uint8_t unused;
                uint8_t data[ACI_PACKET_MAX_LEN];
            } packets[] = SETUP_MESSAGES_CONTENT;

            STATIC_ASSERT(sizeof txBuffer == sizeof packets[0].data);
            STATIC_ASSERT(SysCS::SetupLast - SysCS::SetupFirst == arraysize(packets) - 1);
            STATIC_ASSERT(SysCS::SetupLast + 1 == SysCS::Idle);

            memcpy(&txBuffer, packets[sysCommandState - SysCS::SetupFirst].data, sizeof txBuffer);
            sysCommandState++;

            return true;
        }

        case SysCS::InitSysVersion: {
            /*
             * Send our system version identifier to the nRF8001, to be stored in its RAM.
             * It will handle firmware version reads without bothering us. This is the
             * same version we report to userspace with _SYS_version().
             *
             * This happens after SETUP is finished and we've entered Standby mode, but
             * before initiating a Bond.
             */

            txBuffer.length = 6;
            txBuffer.command = Op::SetLocalData;
            txBuffer.param[0] = PIPE_SIFTEO_BASE_SYSTEM_VERSION_SET;

            uint32_t version = _SYS_version();
            memcpy(&txBuffer.param[1], &version, sizeof version);

            sysCommandState = SysCS::AfterInitSysVersion;
            return true;
        };

        case SysCS::BeginBond: {
            /*
             * After all setup is complete, send a 'Bond' command. This begins the potentially
             * long-running process of looking for a peer. This is what enables advertisement
             * broadcasts. Bonding is analogous to Connecting, but with authentication and
             * encryption.
             *
             * After this command, we'll be idle until a connection event arrives.
             *
             * We use Apple's recommended advertising interval of 20ms here. If we need
             * to save power, we could increase it. See the Apple Bluetooth Design Guidelines:
             *
             * https://developer.apple.com/hardwaredrivers/BluetoothDesignGuidelines.pdf
             */

            txBuffer.length = 5;
            txBuffer.command = Op::Bond;
            txBuffer.param16[0] = 10;           // Try again after 10 seconds
            txBuffer.param16[1] = 32;           // 20ms, in 0.625ms units
            sysCommandState = SysCS::Idle;
            return true;
        }

        case SysCS::ChangeTimingRequest: {
            /*
             * After connecting, see if we can adjust the connection interval down
             * so we can get higher throughput.
             *
             * Apple has some annoying and somewhat opaque restrictions on the connection
             * intervals they allow, so we may have to tread lightly to get the
             * best performance on iOS. Again, the Apple Bluetooth Design Guidelines:
             *
             * https://developer.apple.com/hardwaredrivers/BluetoothDesignGuidelines.pdf
             *
             * That design guide would imply that the best we can do is a range of 20 to 40ms.
             * Apple seems to like picking an actual connection interval that's just below the
             * maximum. However, intervals like 10-20ms actually do work. I've obverved an iPhone
             * with iOS 6.1 give me a 18.75ms window in this case, which yields a max data rate
             * of 1066 bytes/sec.
             *
             * This can be really annoying to test, since iOS seems to cache timing information
             * per-device. Rebooting the phone will clear this cache.
             */
            
            txBuffer.length = 9;
            txBuffer.command = Op::ChangeTimingRequest;
            txBuffer.param16[0] = 8;        // Minimum interval
            txBuffer.param16[1] = 16;       // Maximum interval
            txBuffer.param16[2] = 0;        // Slave latency
            txBuffer.param16[3] = 30;       // Supervision timeout
            sysCommandState = SysCS::Idle;
            return true;
        }

        case SysCS::Disconnect: {
            /*
             * Send an explicit disconnect. We use this when we need to back up a new
             * link key before letting a connection start for reals.
             */

            txBuffer.length = 2;
            txBuffer.command = Op::Disconnect;
            txBuffer.param[0] = 0x01;       // Connection terminated by peer
            sysCommandState = SysCS::Idle;
            return true;
        }

        case SysCS::ReadDynamicData: {
            /*
             * Read the next dynamic data packet out of the nRF8001.
             */

            txBuffer.length = 1;
            txBuffer.command = Op::ReadDynamicData;
            sysCommandState = SysCS::Idle;
            return true;
        }

        case SysCS::WriteDynamicData: {
            /*
             * Write the next dynamic data packet out of the nRF8001.
             *
             * If there are more packets to send after this one, we'll also request
             * the next one from our Task handler.
             */

            txBuffer.length = 2 + dyn.record.length;
            txBuffer.command = Op::WriteDynamicData;
            txBuffer.param[0] = dyn.sequence + 1;
            memcpy(&txBuffer.param[1], dyn.record.data, dyn.record.length);

            if (dyn.record.continued) {
                // Load the next packet
                sysCommandState = SysCS::Idle;
                dyn.sequence++;
                dyn.state = DynStateLoadRequest;
                Tasks::trigger(Tasks::BluetoothDriver);

            } else {
                // Move on
                dyn.state = DynStateIdle;
                sysCommandState = SysCS::AfterWriteDynamicData;
            }
            return true;
        }

        case SysCS::EnterTest: {
            txBuffer.length = 2;
            txBuffer.command = Op::Test;
            txBuffer.param[0] = 0x02;       // Enable DTB over ACI
            sysCommandState = SysCS::Echo;  // send an echo as the first step of our test
            return true;
        }

        case SysCS::ExitTest: {
            txBuffer.length = 2;
            txBuffer.command = Op::Test;
            txBuffer.param[0] = 0xff;       // exit test mode
            sysCommandState = SysCS::SetupFirst;
            return true;
        }

        case SysCS::Echo: {
            txBuffer.length = 1 + sizeof(Test::echoData);
            txBuffer.command = Op::Echo;
            memcpy(txBuffer.param, Test::echoData, sizeof Test::echoData);
            sysCommandState = SysCS::DtmRX;
            testState = Test::BeginRX;
            return true;
        }

        case SysCS::DtmRX:
        case SysCS::DtmEnd: {
            txBuffer.length = 3;
            txBuffer.command = Op::DtmCommand;
            txBuffer.param16[0] = Test::dtmParams[sysCommandState - SysCS::DtmRX];
            sysCommandState = SysCS::Idle;
            return true;
        }
    }
}

void NRF8001::handleEvent()
{
    if (rxBuffer.length == 0) {
        // No pending event.
        return;
    }

    switch (rxBuffer.event) {

        case Op::CommandResponseEvent: {
            /*
             * The last command finished. This is where we would take note of the status
             * if we need to. Only one system command may be pending at a time, so this
             * lets us move to the next command if we want.
             */

            sysCommandPending = false;
            handleCommandStatus(rxBuffer.param[0], rxBuffer.param[1]);

            if (sysCommandState != SysCS::Idle || dyn.state != DynStateIdle) {
                // More work to do, ask for another transaction.
                requestTransaction();
            }
            return;
        }

        case Op::DeviceStartedEvent: {
            /*
             * The device has changed operating modes. This happens after SETUP
             * finishes, when the device enters Standby mode. When this happens,
             * we want to set up any local data that needs to be sent to the
             * nRF8001's RAM, then initiate a Connect to start broadcasting
             * advertisement packets.
             *
             * This is also where our pool of data credits gets initialized.
             */

            uint8_t mode = rxBuffer.param[0];
            dataCredits = rxBuffer.param[2];

            if (mode == OperatingMode::Standby && sysCommandState == SysCS::Idle) {

                /*
                 * We can only enter Test mode from Standby mode, but in normal
                 * operation we transition immediately from Standby to Active mode by
                 * sending an Op::Bond.
                 *
                 * To get back to Standby, we issue a RadioReset since Op::Disconnect
                 * fails if we're not yet connected to a host. Check here to see
                 * whether we're re-entering Standby on our way to Test mode,
                 * or simply as part of our normal start procedure.
                 */

                if (testState == Test::EnterTest) {
                    sysCommandState = SysCS::EnterTest;
                    testState = Test::Idle;

                } else {
                    // Send local data, beginning with "Dynamic" data from the filesystem.
                    // This requires fetching the first packet from SysLFS.

                    dyn.sequence = 0;
                    dyn.state = DynStateLoadRequest;
                    Tasks::trigger(Tasks::BluetoothDriver);
                }
            }

            // Op::Test doesn't get a CommandResponseEvent,
            // so must clear sysCommandPending explicitly
            sysCommandPending = false;

            if (sysCommandState != SysCS::Idle) {
                // More work to do, ask for another transaction.
                requestTransaction();
            }
            return;
        }

        case Op::BondStatusEvent: {
            /*
             * Maybe we established a bonded connection!
             *
             * If the bonding failed, we'll also get a DisconnectedEvent,
             * so we won't bother handling unsuccessful status events here.
             *
             * If bonding succeeded, we'd like to:
             *    - Request new timing parameters, to get more frequent connection windows
             *    - Notify BTProtocol
             *    - Back up the new link key we may have just generated.
             *
             * Unfortunately, there's no way to back up *just* the link keys.
             * Nordic expects us to use this "read dynamic data" command which
             * gives us a binary blob of unspecified size or format. Eww. So...
             * we could defer saving this until "later", but that seems like a great
             * way to have a bunch of lingering bugs where keys may not get saved
             * in this or that situation. So, it seems best to deal with this problem
             * sooner rather than later.
             *
             * If we were provided a pairing code, that might have led to generating
             * a new link key. We can test for this state by asking BTProtocol whether
             * we're in pairing mode. If we successfully finish a connection in this
             * state, we'll actually immediately ask for the nRF8001 to disconnect so
             * we can save the dynamic data. The peer will have to know to retry the
             * connection, but the next attempt should succeed without any user
             * interaction.
             */

            uint8_t status = rxBuffer.param[0];
            if (status != 0x00) {
                // Ignore failure here, we'll get a DisconnectedEvent too.
                return;
            }

            if (BTProtocol::isPairingInProgress()) {
                sysCommandState = SysCS::Disconnect;
                needStoreDynamicData = true;
            } else {
                sysCommandState = SysCS::AfterConnect;
                BTProtocolCallbacks::onConnect();
            }
            return;
        }

        case Op::DisconnectedEvent: {
            /*
             * One connection ended, and now the nRF8001 is back in standby mode.
             * Start trying to establish another connection, or back up dynamic
             * data if that's needed.
             */

            openPipes = 0;
            sysCommandState = needStoreDynamicData ? SysCS::ReadDynamicData : SysCS::AfterReadDynamicData;
            BTProtocolCallbacks::onDisconnect();
            requestTransaction();
            return;
        }

        case Op::PipeStatusEvent: {
            /*
             * This event contains two 64-bit bitmaps, indicating which
             * pipes are open and which ones are closed and require
             * opening prior to use.
             *
             * This is a form of flow control. Data credits are flow
             * control at the ACI level, pipe status is flow control
             * at the per-pipe level. This is how we know that the peer
             * will be listening when we transmit.
             *
             * We use very few pipes, since we're using the nRF8001 mostly
             * lke a dumb serial pipe rather than a normal GATT device.
             * So, we won't bother storing the whole bitmap.
             * 
             * This may mean we can now send data wheras before we couldn't,
             * so we'll request a transaction in case we need to transmit.
             */

             openPipes = rxBuffer.param[0];     // Just the LSB of the 'opened' bitmap.
             requestTransaction();
             return;
        }

        case Op::DataReceivedEvent: {
            /*
             * Data received from an nRF8001 pipe.
             *
             * Our data pipe is configured to auto-acknowledge. These over-the-air
             * ACKs are used as flow control for the radio link, but we currently assume
             * that our CPU can process incoming data as fast as we read it from the
             * nRF8001's ACI interface.
             */

            int length = int(rxBuffer.length) - 1;
            uint8_t pipe = rxBuffer.param[0];

            if (length > 0 && pipe == PIPE_SIFTEO_BASE_DATA_OUT_RX_ACK_AUTO) {
                BTProtocolCallbacks::onReceiveData(&rxBuffer.param[1], length);
            }
            return;
        }

        case Op::DataCreditEvent: {
            /*
             * Received flow control credits that allow us to transmit more packets.
             * 
             * This may mean we can now send data wheras before we couldn't,
             * so we'll request a transaction in case we need to transmit.
             */

            dataCredits += rxBuffer.param[0];
            requestTransaction();
            return;
        }

        case Op::EchoEvent: {
            /*
             * During testing, we send some echo data to verify we can communicate
             * successfully with the 8001.
             */

            bool matched = (rxBuffer.length - 1 == sizeof(Test::echoData) &&
                            memcmp(rxBuffer.param, Test::echoData, sizeof(Test::echoData)) == 0);
            FactoryTest::onBtlePhaseComplete(ACI_STATUS_SUCCESS, matched);
            // Op::Echo doesn't get a CommandResponseEvent,
            // so must clear sysCommandPending explicitly
            sysCommandPending = false;
            requestTransaction();
            return;
        }

        case Op::DisplayKeyEvent: {
            /*
             * A 6-digit pairing code was received. Display it to the user until we either
             * finish connecting or the pairing fails and we send a disconnect event.
             */

            BTProtocolCallbacks::onDisplayPairingCode((const char *) rxBuffer.param);
            return;
        }
    }
}

void NRF8001::handleCommandStatus(unsigned command, unsigned status)
{
    switch (command) {

        case Op::RadioReset:
            /*
             * RadioReset will complain if the device hasn't been setup yet.
             * We care not, since we send the reset just-in-case. Ignore errors here.
             */

            return;

        case Op::DtmCommand:
            /*
             * Response to a Direct Test Mode command.
             * (DTM uses big endian values.)
             */

            handleDtmResponse(status, (rxBuffer.param[2] << 8) | rxBuffer.param[3]);
            break;

        case Op::ReadDynamicData: {
            /*
             * Received some data to back up. Store it and wake up our task handler.
             * Is there still more to download?
             */

            unsigned length = rxBuffer.length - 4;
            unsigned sequence = rxBuffer.param[2] - 1;

            if (length > sizeof dyn.record.data || sequence >= SysLFS::NUM_BLUETOOTH) {
                // Bad parameter. Give up now.

                needStoreDynamicData = false;
                sysCommandState = SysCS::RadioReset;

            } else {
                // Hand this packet off to task() for writing to SysLFS.
                // We'll resume in produceSystemCommand with DynStateStoreComplete.

                bool more = (status == ACI_STATUS_TRANSACTION_CONTINUE);
                needStoreDynamicData = more;
                sysCommandState = SysCS::Idle;

                memcpy(dyn.record.data, &rxBuffer.param[3], length);
                dyn.record.continued = more;
                dyn.record.length = length;
                dyn.sequence = sequence;
                dyn.state = DynStateStoreRequest;
                Tasks::trigger(Tasks::BluetoothDriver);
            }
            break;
        }
    }

    if (status > ACI_STATUS_TRANSACTION_COMPLETE) {
        /*
         * An error occurred! For now, just try resetting as best we can...
         */

        #ifdef TRACE
            UART("BT Err: ");
            UART_HEX( (command << 24) | status );
            UART(" <---\r\n");
        #endif

        sysCommandState = SysCS::RadioReset;
    }
}

void NRF8001::handleDtmResponse(unsigned status, uint16_t response)
{
    // is this a packet report?
    if (response & 0x8000) {
        FactoryTest::onBtlePhaseComplete(status, response);
    }

    // tick along our state machine as appropriate.
    switch (testState) {

    case Test::BeginRX:
        // end of Phase1
        // we're now waiting to receive a Phase2 command to continue
        testState = Test::Idle;
        break;

    case Test::EndRX:
        // this is the last DTM command in Phase2
        sysCommandState = SysCS::ExitTest;
        testState = Test::Idle;
        break;
    }
}

#endif //  HAVE_NRF8001
