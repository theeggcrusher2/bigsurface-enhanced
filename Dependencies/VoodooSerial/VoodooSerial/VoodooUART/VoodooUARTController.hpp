//
//  VoodooUARTController.hpp
//  BigSurface
//
//  Created by Xavier on 2021/9/23.
//  Copyright © 2021 Xia Shangning. All rights reserved.
//
/* Implements an Intel LPSS Designware UART Controller
 * type 16550A (8250)
 *
 * This is the base class from which all implementations of a physical
 * Intel LPSS Designware UART Controller should inherit from.
 * The members of this class are responsible for low-level interfacing with the physical hardware.
 * The code is based on Linux 8250_dw.c
 */

#ifndef VoodooUARTController_hpp
#define VoodooUARTController_hpp

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "../ACPIParser/VoodooACPIResourcesParser.hpp"
#include "../helpers.hpp"

enum UART_State {
    UART_SLEEP=0,
    UART_LONG_IDLE,
    UART_IDLE,
    UART_ACTIVE
};

struct VoodooUARTPhysicalDevice {
    UART_State state;
    const char* name;
    UInt16 device_id;
    IOPCIDevice* pci_device;
    IOMemoryMap* mmap;
};

struct VoodooUARTBus {
    bool command_error;
    UInt32 baud_rate;
    UInt8 data_bits;
    UInt8 stop_bits;
    UInt8 parity;
    bool flow_control;
    UInt8 *tx_buffer;
    UInt16 tx_buffer_len;
    UInt8 *rx_buffer;
};

/* Implements an Intel LPSS Designware 16550A compatible UART Controller
 *
 * This is the base class from which all implementations of a physical
 * Intel LPSS Designware UART Controller should inherit from. The members of this class
 * are responsible for low-level interfacing with the physical hardware.
 *
 * The input clock of the LPSS block differs between generations (120 MHz up to
 * Ice Lake, 100 MHz from Tiger Lake on) and is given by the "UARTClkRate" property
 * of the IOKit personality. The fractional clock divider is programmed so that
 * the requested baud rate is reached with a divisor latch of 1.
 */
class EXPORT VoodooUARTController : public IOService {
  OSDeclareDefaultStructors(VoodooUARTController);

 public:
    typedef void (*MessageHandler)(OSObject *owner, VoodooUARTController *sender, UInt8 *buffer, UInt16 length);

    bool init(OSDictionary* properties) override;

    IOService* probe(IOService* provider, SInt32* score) override;

    bool start(IOService* provider) override;

    void stop(IOService* provider) override;

    void free() override;

    IOReturn setPowerState(unsigned long whichState, IOService* whatDevice) override;

    IOReturn requestConnect(OSObject *owner, MessageHandler _handler, UInt32 baud_rate, UInt8 data_bits, UInt8 stop_bits, UInt8 parity, bool flow_control);

    void requestDisconnect(OSObject *owner);

    IOReturn transmitData(UInt8 *buffer, UInt16 length);

    /* Maximum number of bytes passed to the MessageHandler in a single call */
    UInt16 getFIFOSize() const { return fifo_size; }

    /* Whether this controller is the ACPI device named by a serial bus ResourceSource
     * string such as "\_SB.PC00.UA00". Used to pick the right UART when several are present.
     */
    bool matchesACPIPath(const char *resource_source);

 protected:
    VoodooUARTPhysicalDevice device;

    IOReturn mapMemory();

    IOReturn unmapMemory();

    IOReturn initDevice();

    void configDevice();

 private:
    OSObject*               target {nullptr};
    MessageHandler          handler {nullptr};
    IOWorkLoop*             work_loop {nullptr};
    IOCommandGate*          command_gate {nullptr};
    IOCommandGate::Action   transmit_action {nullptr};
    IOTimerEventSource*     interrupt_simulator {nullptr};
    AbsoluteTime            last_activate_time {0};
    bool ready {false};

    UInt16 fifo_size {0};
    UInt32 clock_rate {0};
    UInt32 fcr {0};
    UInt32 mcr {0};
    UInt32 ier {0};

    VoodooUARTBus bus;
    bool write_complete {false};

    inline UInt32 readRegister(int offset);

    inline void writeRegister(UInt32 value, int offset);

    void startUARTClock();

    void stopUARTClock();

    void setPCIPowerState(UInt16 state);

    void releaseResources();

    IOReturn setPowerStateGated(unsigned long *whichState);

    IOReturn transmitDataGated(UInt8* buffer, UInt16* length);

    /* Set permitted Interrupt type by IER*/
    IOReturn prepareCommunication();

    IOReturn stopCommunication();

    IOReturn waitUntilNotBusy();

    void transmitAndReceiveData();

    void simulateInterrupt(IOTimerEventSource* timer);

    void toggleInterruptType(UInt32 type, bool enable);
};

#endif /* VoodooUARTController_hpp */
