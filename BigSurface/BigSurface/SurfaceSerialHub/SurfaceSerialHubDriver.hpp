//
//  SurfaceSerialHubDriver.hpp
//  SurfaceSerialHub
//
//  Created by Xavier on 2021/10/29.
//  Copyright © 2021 Xia Shangning. All rights reserved.
//

#ifndef SurfaceSerialHubDriver_hpp
#define SurfaceSerialHubDriver_hpp

#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOLocks.h>

#include "../../../Dependencies/VoodooGPIO/VoodooGPIO/VoodooGPIO.hpp"
#include "../../../Dependencies/VoodooSerial/VoodooSerial/VoodooUART/VoodooUARTController.hpp"
#include "SerialProtocol.h"

enum SurfaceSerialEventRegistryType {
    SurfaceSerialEventHostManagedV1 = 0,    // SAM registry (TC 0x01, TID 0x01)
    SurfaceSerialEventHostManagedV2,        // REG registry (TC 0x21, TID 0x02)
    SurfaceSerialEventHubManaged,           // KIP registry (TC 0x0e, TID 0x02)
    SurfaceSerialEventTypeCount
};

/*
 * Optional SAM functions, which depend on the model. Mirrors the device groups of
 * Linux drivers/platform/surface/surface_aggregator_registry.c, keyed by the ACPI
 * id of the SSAM platform hub (e.g. MSHW0250 for the Surface Laptop 4).
 */
enum SurfaceSAMFeature : UInt32 {
    SurfaceSAMFeatureBattery            = BIT(0),   // BAT/ADP via SAM (7th gen and later)
    SurfaceSAMFeaturePerformanceMode    = BIT(1),   // TMP platform profile
    SurfaceSAMFeatureFanProfile         = BIT(2),   // FAN profile follows the platform profile
    SurfaceSAMFeatureFanSpeed           = BIT(3),   // FAN rpm readout
    SurfaceSAMFeatureThermalSensors     = BIT(4),   // TMP sensor enumeration
    SurfaceSAMFeatureLegacyKeyboard     = BIT(5),   // KBD subsystem keyboard (Surface Laptop 1/2)
};

#define SSH_REQID_MIN           (SSH_TC_COUNT + 1)  // request ids below are reserved for events
#define SSH_SEQ_HISTORY         8                   // remembered sequence ids for retransmission detection
#define SSH_SEQ_INVALID         0xffff
#define SSH_MAX_RESTORED_EVENTS 32

#define SSH_RX_FIFO_SIZE        4096                // bytes buffered between UART and parser
#define SSH_RX_BUF_SIZE         2048                // largest message the parser accepts

#define SSH_ACK_TIMEOUT         250                 // ms until an un-ACKed packet is resent
#define SSH_MAX_PACKET_TRIES    3
#define SSH_PACKET_TIMEOUT      3000                // ms a request may wait in the queue and for its ACK
#define SSH_RESPONSE_TIMEOUT    1000                // ms after the ACK until a request without response fails
#define SSH_REQUEST_TRIES       3                   // attempts for a request that timed out
#define SSH_NUB_PUBLISH_DELAY   20000               // ms, overridable with "NubPublishDelay"

class EXPORT SurfaceSerialHubClient : public IOService {
    OSDeclareAbstractStructors(SurfaceSerialHubClient);

public:
    /*
     * This method is called when SSH receives corresponding registered events
     * it should NOT block the thread
     * the ACTUAL event handling codes should be done in device driver's own workloop thread
     */
    virtual void eventReceived(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *data_buffer, UInt16 length) = 0;
};

class SurfaceBatteryNub;
class SurfaceHIDNub;

class EXPORT SurfaceSerialHubDriver : public IOService {
    OSDeclareDefaultStructors(SurfaceSerialHubDriver);

public:
    /*
     * Send a command without waiting for its response.
     * Returns the request id, 0 on failure.
     */
    UInt16 sendCommand(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *payload, UInt16 payload_len, bool seq);

    /*
     * Send a command and wait for its response. The response is copied into buffer
     * (truncated to buffer_len); its actual length is returned in received_len if given.
     * Must not be called from SurfaceSerialHubClient::eventReceived().
     */
    IOReturn getResponse(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *payload, UInt16 payload_len, bool seq, UInt8 *buffer, UInt16 buffer_len, UInt16 *received_len = nullptr);

    IOReturn registerEvent(SurfaceSerialHubClient *client, SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid);

    void unregisterEvent(SurfaceSerialHubClient *client, SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid);

    bool hasFeature(SurfaceSAMFeature feature) const { return (features & feature) != 0; }

    /* Whether the model was found in the model table, i.e. its features are known */
    bool isKnownModel() const { return known_model; }

    bool init(OSDictionary* properties) override;

    IOService* probe(IOService* provider, SInt32* score) override;

    bool start(IOService* provider) override;

    void stop(IOService* provider) override;

    void free() override;

    IOReturn setPowerState(unsigned long whichState, IOService *whatDevice) override;

    IOReturn enableInterrupt(int source) override;

    IOReturn disableInterrupt(int source) override;

    IOReturn getInterruptType(int source, int *interruptType) override;

    IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) override;

    IOReturn unregisterInterrupt(int source) override;

private:
    class CircleIDCounter {
    private:
        UInt16 min;
        UInt16 max;
        UInt16 id;
    public:
        CircleIDCounter(UInt16 _min, UInt16 _max){
            min = _min;
            max = _max;
            id = min-1;
        }

        UInt16 getID() {
            UInt16 ret = ++id;
            if (id == max)
                id = min-1;
            return ret;
        }
    };

    struct WaitingRequest;

    struct Packet {
        queue_entry entry;          // must stay the first member (queue_remove_first)
        UInt8*  buffer {nullptr};
        UInt16  len {0};
        UInt8   seq_id {0};
        UInt8   tries {0};
        WaitingRequest* waiter {nullptr};   // request waiting for the response, if any
    };

    struct Request {
        // in
        UInt8   tc, tid, iid, cid;
        UInt8*  payload;
        UInt16  payload_len;
        bool    seq;
        bool    wait;
        bool    force;      // allowed while asleep (D0 entry/exit notifications)
        UInt8*  buffer;
        UInt16  buffer_len;
        // out
        UInt16  rqid;
        UInt16  received_len;
    };

    struct WaitingRequest {
        queue_entry entry;
        UInt16  rqid;
        UInt8*  buffer;
        UInt16  buffer_len;
        UInt16  received_len;
        bool    completed;
        bool    queued;
        bool    acked;          // the packet carrying the request was ACKed
        bool    tx_failed;      // ... or given up without ACK
        UInt64  deadline;
    };

    struct EventKey {
        SurfaceSerialEventRegistryType type;
        UInt8 tc;
        UInt8 iid;
    };

    struct EventHandler {
        queue_entry entry;
        SurfaceSerialEventRegistryType type;
        UInt8 tc;
        UInt8 target_iid;
        SurfaceSerialHubClient *client;
    };

    IOWorkLoop*             work_loop {nullptr};
    IOCommandGate*          command_gate {nullptr};
    IOTimerEventSource*     publish_timer {nullptr};
    IOTimerEventSource*     retransmit_timer {nullptr};
    IOTimerEventSource*     restore_timer {nullptr};
    IOInterruptEventSource* uart_interrupt {nullptr};
    IOInterruptEventSource* gpio_interrupt {nullptr};
    IOACPIPlatformDevice*   acpi_device {nullptr};
    VoodooUARTController*   uart_controller {nullptr};
    VoodooGPIO*             gpio_controller {nullptr};
    SurfaceBatteryNub*      battery_nub {nullptr};
    SurfaceHIDNub*          hid_nub {nullptr};

    bool            awake {true};       // changed under rx_fifo_lock
    bool            publishing {false};
    bool            stopping {false};
    UInt32          features {0};
    bool            known_model {false};

    // UART -> parser byte FIFO, filled on the UART work loop and drained on ours
    IOSimpleLock*   rx_fifo_lock {nullptr};
    UInt8*          rx_fifo {nullptr};
    UInt32          rx_fifo_head {0};
    UInt32          rx_fifo_count {0};
    bool            rx_fifo_overrun {false};

    // Parser state: bytes not consumed yet, always starting at a potential SYN
    UInt8*          rx_buf {nullptr};
    UInt16          rx_len {0};
    UInt16          rx_seq_history[SSH_SEQ_HISTORY];
    UInt8           rx_seq_history_pos {0};

    // Only one sequenced packet may wait for its ACK, the rest are queued (like Linux)
    queue_head_t    tx_queue;
    Packet*         tx_inflight {nullptr};

    queue_head_t    waiting_list;
    queue_head_t    event_handler_lists[SSH_REQID_MIN];

    CircleIDCounter seq_counter {CircleIDCounter(0x00, 0xff)};
    CircleIDCounter req_counter {CircleIDCounter(SSH_REQID_MIN, 0xffff)};

    UInt32  baudrate {0};
    UInt8   data_bits {0};
    UInt8   stop_bits {0};
    UInt8   parity {0};
    bool    flow_control {false};
    char    uart_resource_source[64];
    bool    has_gpio {false};
    UInt16  gpio_pin {0};
    UInt16  gpio_irq {0};

    void bufferReceived(VoodooUARTController *sender, UInt8 *buffer, UInt16 length);

    void processReceivedBuffer(IOInterruptEventSource *sender, int count);

    void parseReceivedData();

    void processFrame(UInt8 *message, UInt16 length);

    void processCommand(UInt8 frame_type, UInt8 *payload, UInt16 length);

    void resetSeqHistory();

    bool isRetransmission(UInt8 seq_id);

    IOReturn sendControlFrame(UInt8 type, UInt8 seq_id);

    IOReturn submitRequestGated(Request *rq);

    void transmitNextPacket();

    void completeInflightPacket(bool acked);

    void detachWaiter(WaitingRequest *w);

    void retransmitTimeout(IOTimerEventSource *timer);

    IOReturn registerHandlerGated(EventHandler *h, bool *exists);

    IOReturn unregisterHandlerGated(EventHandler *key, EventHandler **removed, bool *still_used);

    IOReturn sendEventCommand(SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid, bool enable);

    void restoreEvents(IOTimerEventSource *sender);

    void setAwake(bool state);

    IOReturn beginStopGated();

    IOReturn collectEventsGated(EventKey *events, int *count);

    IOReturn notifySAM(UInt8 cid, const char *what);

    void delayedPublishingNubs(IOTimerEventSource *sender);

    void gpioWakeUp(IOInterruptEventSource *sender, int count);

    IOReturn getDeviceResources();

    void detectFeatures();

    VoodooUARTController* getUARTController();

    IOReturn flushGated();

    void releaseResources();
};

#endif /* SurfaceSerialHubDriver_hpp */
