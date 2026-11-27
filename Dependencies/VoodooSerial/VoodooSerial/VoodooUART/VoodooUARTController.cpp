//
//  VoodooUARTController.cpp
//  BigSurface
//
//  Created by Xavier on 2021/9/23.
//  Copyright © 2021 Xia Shangning. All rights reserved.
//

#include "VoodooUARTController.hpp"
#include "VoodooUARTConstants.h"

#define CONFIGURED(a) a?"YES":"NO"

#define UART_LONG_IDLE_TIMEOUT  50
#define UART_IDLE_TIMEOUT       10
#define UART_ACTIVE_TIMEOUT     5

#define UART_TX_TIMEOUT_NS      50000000    // 50ms

#define PCI_PM_CAP_FALLBACK     0x80

#define super IOService
OSDefineMetaClassAndStructors(VoodooUARTController, IOService);

/*
 * Best rational approximation of given_numerator/given_denominator with both terms
 * below the given maximums, ported from Linux lib/math/rational.c. Used to program
 * the LPSS fractional clock divider like Linux's clk-fractional-divider does.
 */
static void rational_best_approximation(UInt64 given_numerator, UInt64 given_denominator,
                                        UInt64 max_numerator, UInt64 max_denominator,
                                        UInt64 *best_numerator, UInt64 *best_denominator) {
    UInt64 n, d, n0, d0, n1, d1, n2, d2;
    n = given_numerator;
    d = given_denominator;
    n0 = d1 = 0;
    n1 = d0 = 1;

    for (;;) {
        UInt64 dp, a;

        if (d == 0)
            break;

        dp = d;
        a = n / d;
        d = n % d;
        n = dp;

        n2 = n0 + a * n1;
        d2 = d0 + a * d1;

        if ((n2 > max_numerator) || (d2 > max_denominator)) {
            UInt64 t = UINT64_MAX;

            if (d1)
                t = (max_denominator - d0) / d1;
            if (n1 && (max_numerator - n0) / n1 < t)
                t = (max_numerator - n0) / n1;

            if (!d1 || 2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
                n1 = n0 + t * n1;
                d1 = d0 + t * d1;
            }
            break;
        }
        n0 = n1;
        n1 = n2;
        d0 = d1;
        d1 = d2;
    }
    *best_numerator = n1;
    *best_denominator = d1;
}

/*
 * Normalise one ACPI name segment for comparison: drop the "@unit" suffix used in
 * the IOACPIPlane and the trailing '_' padding of 4-character ACPI names.
 */
static size_t acpi_segment_length(const char *seg, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (seg[i] == '@') {
            len = i;
            break;
        }
    }
    while (len > 0 && seg[len - 1] == '_')
        len--;
    return len;
}

static bool acpi_segments_equal(const char *a, size_t a_len, const char *b, size_t b_len) {
    a_len = acpi_segment_length(a, a_len);
    b_len = acpi_segment_length(b, b_len);
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z') cb -= 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return true;
}

bool VoodooUARTController::init(OSDictionary* properties) {
    if (!super::init(properties))
        return false;
    memset(&bus, 0, sizeof(VoodooUARTBus));
    memset(&device, 0, sizeof(VoodooUARTPhysicalDevice));
    device.state = UART_IDLE;

    return true;
}

void VoodooUARTController::free() {
    super::free();
}

IOReturn VoodooUARTController::mapMemory() {
    if (device.pci_device->getDeviceMemoryCount() == 0) {
        return kIOReturnDeviceError;
    } else {
        device.mmap = device.pci_device->mapDeviceMemoryWithIndex(0);
        if (!device.mmap) return kIOReturnDeviceError;
        return kIOReturnSuccess;
    }
}

IOReturn VoodooUARTController::unmapMemory() {
    OSSafeReleaseNULL(device.mmap);
    return kIOReturnSuccess;
}

void VoodooUARTController::setPCIPowerState(UInt16 state) {
    IOPCIDevice *pci_device = device.pci_device;
    IOByteCount offset = 0;

    /*
     * Some firmwares leave the LPSS controllers in D3 and macOS does not change the
     * power state on initial boot (VoodooI2C PR #246). Locate the power management
     * capability instead of assuming it lives at 0x80.
     */
    if (!pci_device->extendedFindPCICapability(kIOPCIPowerManagementCapability, &offset))
        offset = PCI_PM_CAP_FALLBACK;
    UInt16 pmcsr = pci_device->configRead16(offset + 4);
    pmcsr = (pmcsr & ~kPCIPMCSPowerStateMask) | state;
    pci_device->configWrite16(offset + 4, pmcsr);
}

IOReturn VoodooUARTController::initDevice() {
    IOPCIDevice *pci_device = device.pci_device;
    pci_device->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    setPCIPowerState(kPCIPMCSPowerStateD0);

    pci_device->setBusMasterEnable(true);
    pci_device->setMemoryEnable(true);

    if (mapMemory() != kIOReturnSuccess) {
        LOG("Could not map memory");
        return kIOReturnError;
    }

    return kIOReturnSuccess;
}

void VoodooUARTController::configDevice() {
    if (!device.mmap)
        return;
    writeRegister(0, LPSS_PRIV + LPSS_PRIV_RESETS); // reset device
    writeRegister(LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA, LPSS_PRIV + LPSS_PRIV_RESETS); // deassert reset

    // Remap addr
    UInt64 addr = device.mmap->getPhysicalAddress();
    writeRegister(addr & 0xffffffff, LPSS_PRIV + LPSS_PRIV_REMAP_ADDR);
    writeRegister((addr >> 32) & 0xffffffff, LPSS_PRIV + LPSS_PRIV_REMAP_ADDR + 4);

    startUARTClock();

    fcr = 0;
    mcr = UART_MCR_OUT2;
    ier = 0;

    // Clear FIFO disable all interrupts, they will be renabled in prepareCommunication.
    writeRegister(UART_FCR_ENABLE_FIFO, DW_UART_FCR);
    writeRegister(UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RX_FIFO | UART_FCR_CLEAR_TX_FIFO, DW_UART_FCR);
    writeRegister(0, DW_UART_FCR);

    // Clear interrupt registers.
    writeRegister(mcr, DW_UART_MCR);
    readRegister(DW_UART_LSR);
    readRegister(DW_UART_RX);
    readRegister(DW_UART_IIR);
    readRegister(DW_UART_MSR);
    // Disable all interrupts
    writeRegister(0, DW_UART_IER);
    // Clear busy status
    readRegister(DW_UART_USR);
}

void VoodooUARTController::startUARTClock() {
    /*
     * The divisor latch is always programmed to 1, so the UART clock has to be
     * 16 * baud rate. Derive M/N from the platform input clock, as Linux 8250_dw
     * does through clk_set_rate() on the LPSS fractional divider.
     */
    UInt64 baud = bus.baud_rate ? bus.baud_rate : UART_DEFAULT_BAUDRATE;
    UInt64 uart_clk = baud * 16;
    if (uart_clk > clock_rate) {
        LOG("Baud rate %llu is too high for a %u Hz input clock", baud, clock_rate);
        uart_clk = clock_rate;
    }

    UInt64 m = 1, n = 1;
    rational_best_approximation(uart_clk, clock_rate, UART_CLK_DIV_MAX, UART_CLK_DIV_MAX, &m, &n);
    if (!m || !n)
        m = n = 1;

    // m and n are bounded by UART_CLK_DIV_MAX
    writeRegister(UART_UPDATE_CLK | CALC_CLK((UInt32)m, (UInt32)n) | UART_ENABLE_CLK, LPSS_PRIV + LPSS_UART_CLK);
    DBG_LOG("UART clock %u Hz * %llu / %llu = %llu Hz for %llu baud", clock_rate, m, n, clock_rate * m / n, baud);
}

void VoodooUARTController::stopUARTClock() {
    writeRegister(0, LPSS_PRIV + LPSS_UART_CLK);
}

IOService* VoodooUARTController::probe(IOService* provider, SInt32* score) {
    if (!super::probe(provider, score))
        return nullptr;

    device.pci_device = OSDynamicCast(IOPCIDevice, provider);
    if (!device.pci_device)
        return nullptr;

    device.name = device.pci_device->getName();
    device.device_id = device.pci_device->configRead16(kIOPCIConfigDeviceID);

    OSNumber *clk = OSDynamicCast(OSNumber, getProperty(UART_CLK_RATE_KEY));
    clock_rate = clk ? clk->unsigned32BitValue() : 0;
    if (!clock_rate) {
        LOG("Missing %s, assuming %u Hz", UART_CLK_RATE_KEY, UART_CLK_RATE_DEFAULT);
        clock_rate = UART_CLK_RATE_DEFAULT;
    }
    LOG("device id: %x, input clock: %u Hz", device.device_id, clock_rate);

    return this;
}

bool VoodooUARTController::start(IOService* provider) {
    UInt32 reg;
    if (!super::start(provider))
        return false;

    // Balanced by releaseResources()
    device.pci_device->retain();

    work_loop = IOWorkLoop::workLoop();
    if (!work_loop) {
        LOG("Could not get work loop");
        goto exit;
    }
    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        LOG("Could not open command gate");
        goto exit;
    }
    transmit_action = OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooUARTController::transmitDataGated);

    if (!device.pci_device->open(this)) {
        LOG("Could not open provider");
        goto exit;
    }

    if (initDevice() != kIOReturnSuccess) {
        LOG("Initialise device failed!");
        goto exit;
    }
    configDevice();

    fifo_size = DW_UART_CPR_FIFO_SIZE(readRegister(DW_UART_CPR));
    if (!fifo_size) {
        // The component parameter register is optional, fall back to the LPSS default
        LOG("UART does not report its FIFO size, assuming %d", UART_DEFAULT_FIFO_SIZE);
        fifo_size = UART_DEFAULT_FIFO_SIZE;
    }
    bus.rx_buffer = new UInt8[fifo_size];
    bus.tx_buffer = nullptr;
    bus.tx_buffer_len = 0;
    reg = readRegister(DW_UART_UCV);
    LOG("Designware UART version %c.%c%c, FIFO size %d", (reg >> 24) & 0xff, (reg >> 16) & 0xff, (reg >> 8) & 0xff, fifo_size);

    interrupt_simulator = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &VoodooUARTController::simulateInterrupt));
    if (!interrupt_simulator) {
        LOG("Could not create timer event!");
        goto exit;
    }
    work_loop->addEventSource(interrupt_simulator);

    PMinit();
    device.pci_device->joinPMtree(this);
    registerPowerDriver(this, myIOPMPowerStates, kIOPMNumberPowerStates);

    registerService();
    return true;
exit:
    releaseResources();
    return false;
}

void VoodooUARTController::stop(IOService *provider) {
    if (command_gate)
        command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooUARTController::stopCommunication));
    PMstop();
    releaseResources();
    super::stop(provider);
}

void VoodooUARTController::toggleInterruptType(UInt32 type, bool enable) {
    if (enable)
        ier |= type;
    else
        ier &= ~type;
    writeRegister(ier, DW_UART_IER);
}

inline UInt32 VoodooUARTController::readRegister(int offset) {
    if (device.mmap) {
         IOVirtualAddress address = device.mmap->getVirtualAddress();
         if (address != 0)
             return *(const volatile UInt32 *)(address + offset);
     }
     return 0;
}

inline void VoodooUARTController::writeRegister(UInt32 value, int offset) {
    if (device.mmap) {
        IOVirtualAddress address = device.mmap->getVirtualAddress();
        if (address != 0)
            *(volatile UInt32 *)(address + offset) = value;
    }
}

void VoodooUARTController::releaseResources() {
    if (interrupt_simulator) {
        interrupt_simulator->cancelTimeout();
        interrupt_simulator->disable();
        work_loop->removeEventSource(interrupt_simulator);
        OSSafeReleaseNULL(interrupt_simulator);
    }
    if (bus.rx_buffer) {
        delete[] bus.rx_buffer;
        bus.rx_buffer = nullptr;
    }
    unmapMemory();
    if (device.pci_device && device.pci_device->isOpen(this))
        device.pci_device->close(this);
    if (command_gate) {
        work_loop->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }
    OSSafeReleaseNULL(work_loop);

    OSSafeReleaseNULL(device.pci_device);
}

bool VoodooUARTController::matchesACPIPath(const char *resource_source) {
    if (!resource_source || !resource_source[0] || !device.pci_device)
        return false;

    OSString *path = OSDynamicCast(OSString, device.pci_device->getProperty("acpi-path"));
    if (!path)
        return false;

    // e.g. "IOACPIPlane:/_SB/PC00@0/UA00@1e0000" vs "\_SB.PC00.UA00"
    const char *p = path->getCStringNoCopy();
    const char *colon = strchr(p, ':');
    if (colon)
        p = colon + 1;
    const char *s = resource_source;
    while (*s == '\\' || *s == '^')
        s++;

    while (true) {
        while (*p == '/')
            p++;
        const char *p_end = p;
        while (*p_end && *p_end != '/')
            p_end++;
        const char *s_end = s;
        while (*s_end && *s_end != '.')
            s_end++;

        if (p == p_end || s == s_end)
            return p == p_end && s == s_end;
        if (!acpi_segments_equal(p, p_end - p, s, s_end - s))
            return false;

        p = p_end;
        s = *s_end ? s_end + 1 : s_end;
    }
}

IOReturn VoodooUARTController::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this || !command_gate)
        return kIOPMAckImplied;

    // ensure that we are not in the middle of a data transmission or interrupt
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooUARTController::setPowerStateGated), &whichState);
}

IOReturn VoodooUARTController::setPowerStateGated(unsigned long *whichState) {
    if (*whichState == kIOPMPowerOff) {
        if (device.state != UART_SLEEP) {
            DBG_LOG("Prepare to sleep");
            device.state = UART_SLEEP;
            stopCommunication();
            stopUARTClock();
            unmapMemory();
            device.pci_device->enablePCIPowerManagement(kPCIPMCSPowerStateD3);
            setPCIPowerState(kPCIPMCSPowerStateD3);
            DBG_LOG("Going to sleep");
        }
    } else {
        if (device.state == UART_SLEEP) {
            if (initDevice() != kIOReturnSuccess) {
                // Stay in UART_SLEEP: nothing is mapped, every request fails with kIOReturnNotReady
                LOG("Initialise device failed after wake, UART unusable until the next wake!");
                return kIOPMAckImplied;
            }
            configDevice();
            device.state = UART_IDLE;
            if (target && prepareCommunication() != kIOReturnSuccess)
                LOG("Failed to reopen the UART after wake!");
            DBG_LOG("Woke up");
        }
    }
    return kIOPMAckImplied;
}

IOReturn VoodooUARTController::requestConnect(OSObject *owner, MessageHandler _handler, UInt32 baud_rate, UInt8 data_bits, UInt8 stop_bits, UInt8 parity, bool flow_control) {
    if (target)
        return kIOReturnNoResources;
    if (!owner || !_handler)
        return kIOReturnError;
    if (!command_gate)
        return kIOReturnNotReady;

    target = owner;
    handler = _handler;

    bus.baud_rate = baud_rate;
    bus.data_bits = data_bits;
    bus.stop_bits = stop_bits;
    bus.parity = parity;
    bus.flow_control = flow_control;

    IOReturn ret = command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooUARTController::prepareCommunication));
    if (ret != kIOReturnSuccess) {
        target = nullptr;
        handler = nullptr;
    }
    return ret;
}

void VoodooUARTController::requestDisconnect(OSObject *owner) {
    if (target == owner) {
        if (command_gate)
            command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooUARTController::stopCommunication));
        target = nullptr;
        handler = nullptr;
    }
}

IOReturn VoodooUARTController::prepareCommunication() {
    if (ready)
        return kIOReturnStillOpen;
    if (device.state == UART_SLEEP || !device.mmap)
        return kIOReturnNotReady;

    UInt16 divisor = 1;
    UInt32 lcr = 0;
    if (bus.data_bits >= UART_DATABITS_9) {
        LOG("unsupported data bits 9!");
        return kIOReturnUnsupported;
    }
    if (bus.stop_bits == UART_STOPBITS_1_5 && bus.data_bits != UART_DATABITS_5) {
        LOG("unsupported stop bits 1.5 when data bits is not 5!");
        return kIOReturnUnsupported;
    }
    if (bus.stop_bits == UART_STOPBITS_NONE) {
        LOG("unsupported stop bits 0!");
        return kIOReturnUnsupported;
    }
    lcr |= bus.data_bits;
    if (bus.stop_bits == UART_STOPBITS_1_5 || bus.stop_bits == UART_STOPBITS_2)
        lcr |= UART_LCR_STOP;
    if (bus.parity != UART_PARITY_NONE) {
        lcr |= UART_LCR_PARITY;
        if (bus.parity == UART_PARITY_EVEN)
            lcr |= UART_LCR_EPAR;
        else if (bus.parity != UART_PARITY_ODD) {
            LOG("unsupported parity type %d!", bus.parity);
            return kIOReturnUnsupported;
        }
    }

    if (waitUntilNotBusy() != kIOReturnSuccess)
        return kIOReturnBusy;

    // Program the input clock for the requested baud rate before latching the divisor
    startUARTClock();

    writeRegister(UART_LCR_DLAB, DW_UART_LCR);
    writeRegister((divisor>>8) & 0xFF, DW_UART_DLH);
    writeRegister(divisor & 0xFF, DW_UART_DLL);
    writeRegister(lcr, DW_UART_LCR);

    // enable FIFO and set threshold trigger, auto flow control deasserts RTS at the RX trigger level
    fcr = UART_FCR_ENABLE_FIFO | UART_FCR_T_TRIG_HALF | UART_FCR_R_TRIG_HALF;
    writeRegister(fcr, DW_UART_FCR);

    if (bus.flow_control) {
        mcr |= UART_MCR_RTS | UART_MCR_AFC;
        writeRegister(mcr, DW_UART_MCR);
        bool cts = false;
        for (int tries=0; tries < 100; tries++) {   // 10ms in total
            if (readRegister(DW_UART_MSR) & UART_MSR_CTS) {
                DBG_LOG("Received Clear To Send signal!");
                cts = true;
                break;
            }
            IODelay(100);
        }
        // With auto flow control the hardware holds transmission until CTS, so carry on
        if (!cts)
            LOG("Warning! No Clear To Send from the device yet");
    }
    ready = true;

    if (bus.flow_control)
        toggleInterruptType(UART_IER_ENABLE_MODEM_STA_INT, true);
    toggleInterruptType(UART_IER_ENABLE_RX_AVAIL_INT | UART_IER_ENABLE_ERR_INT, true);

    // Surface Pro 6 quirk, wait for UART to sync
    if (device.device_id == 0x9d27)
        IOSleep(50);

    device.state = UART_ACTIVE;
    clock_get_uptime(&last_activate_time);
    interrupt_simulator->enable();
    interrupt_simulator->setTimeoutMS(UART_ACTIVE_TIMEOUT);

    return kIOReturnSuccess;
}

IOReturn VoodooUARTController::stopCommunication() {
    if (!ready)
        return kIOReturnSuccess;

    interrupt_simulator->cancelTimeout();
    interrupt_simulator->disable();

    // Abort a transmission that is still in flight, its buffer belongs to the caller
    if (bus.tx_buffer_len) {
        bus.tx_buffer = nullptr;
        bus.tx_buffer_len = 0;
        command_gate->commandWakeup(&write_complete);
    }

    //Clear the interrupt registers.
    writeRegister(0, DW_UART_IER);
    readRegister(DW_UART_LSR);
    readRegister(DW_UART_RX);
    readRegister(DW_UART_IIR);
    readRegister(DW_UART_MSR);

    writeRegister(fcr | UART_FCR_CLEAR_RX_FIFO | UART_FCR_CLEAR_TX_FIFO, DW_UART_FCR);
    writeRegister(0, DW_UART_FCR);

    writeRegister(0, DW_UART_MCR);

    fcr = 0;
    ier = 0;
    mcr = 0;

    waitUntilNotBusy();

    writeRegister(UART_LCR_DLAB, DW_UART_LCR);
    writeRegister(0, DW_UART_DLH);
    writeRegister(0, DW_UART_DLL);
    writeRegister(0, DW_UART_LCR);

    ready = false;
    return kIOReturnSuccess;
}

IOReturn VoodooUARTController::waitUntilNotBusy() {
    int timeout = UART_TIMEOUT, firstDelay = 100;

    while (readRegister(DW_UART_USR) & UART_USR_BUSY) {
        if (timeout <= 0) {
            LOG("Warning: Timeout waiting for UART not to be busy");
            return kIOReturnBusy;
        }
        timeout--;
        if (firstDelay-- >= 0)
            IODelay(100);   // 0.1 ms
        else
            IOSleep(5);     // 5ms
    }
    return kIOReturnSuccess;
}

IOReturn VoodooUARTController::transmitData(UInt8 *buffer, UInt16 length) {
    IOReturn ret = kIOReturnSuccess;
    if (!buffer || !length)
        return kIOReturnBadArgument;
    if (!command_gate)
        return kIOReturnNotReady;
    for (int tries=0; tries < 5; tries++) {
        if (!ready)
            return kIOReturnNotReady;

        ret = command_gate->runAction(transmit_action, buffer, &length);
        if (ret == kIOReturnBusy) {
            DBG_LOG("Busy");
            IOSleep((random() & 0x03) + UART_ACTIVE_TIMEOUT);
        } else
            break;
    }
    return ret;
}

IOReturn VoodooUARTController::transmitDataGated(UInt8* buffer, UInt16* length) {
    AbsoluteTime abstime, deadline;
    IOReturn sleep;

    if (!ready)
        return kIOReturnNotReady;
    if (bus.tx_buffer_len)
        return kIOReturnBusy;

    nanoseconds_to_absolutetime(UART_TX_TIMEOUT_NS + (UInt64)(*length / fifo_size + 1) * 2 * UART_ACTIVE_TIMEOUT * 1000000ULL, &abstime);
    clock_absolutetime_interval_to_deadline(abstime, &deadline);

    bus.tx_buffer = buffer;
    bus.tx_buffer_len = *length;
    toggleInterruptType(UART_IER_ENABLE_TX_EMPTY_INT | UART_IER_ENABLE_THRE_INT_MODE, true);
    interrupt_simulator->setTimeoutUS(1);
    sleep = command_gate->commandSleep(&write_complete, deadline, THREAD_INTERRUPTIBLE);

    if (bus.tx_buffer_len) {
        // Timed out or interrupted: never touch the caller's buffer after returning
        bus.tx_buffer = nullptr;
        bus.tx_buffer_len = 0;
        toggleInterruptType(UART_IER_ENABLE_TX_EMPTY_INT | UART_IER_ENABLE_THRE_INT_MODE, false);
        LOG("%s waiting for transfer request", sleep == THREAD_TIMED_OUT ? "Timeout" : "Interrupted");
        return sleep == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
    }
    return ready ? kIOReturnSuccess : kIOReturnNotReady;
}

void VoodooUARTController::transmitAndReceiveData() {
    UInt32 status = readRegister(DW_UART_IIR)&UART_IIR_INT_MASK;
    UInt32 lsr = readRegister(DW_UART_LSR);
    if (bus.tx_buffer_len) {
        device.state = UART_ACTIVE;
        clock_get_uptime(&last_activate_time);
        // With programmable THRE interrupt mode enabled, LSR[5] reports a full TX FIFO
        if (!(lsr&UART_LSR_TX_FIFO_FULL)) {
            do {
                writeRegister(*bus.tx_buffer++, DW_UART_TX);
                lsr = readRegister(DW_UART_LSR);
            } while (--bus.tx_buffer_len && !(lsr&UART_LSR_TX_FIFO_FULL));
            if (!bus.tx_buffer_len) {
                bus.tx_buffer = nullptr;
                toggleInterruptType(UART_IER_ENABLE_TX_EMPTY_INT | UART_IER_ENABLE_THRE_INT_MODE, false);
                command_gate->commandWakeup(&write_complete);
            }
        }
    }
    if (lsr & (UART_LSR_DATA_READY|UART_LSR_BREAK_INT)) {
        device.state = UART_ACTIVE;
        clock_get_uptime(&last_activate_time);
        UInt8 *pos = bus.rx_buffer;
        UInt16 length = 0;
        UInt32 errors = lsr & UART_LSR_BRK_ERROR_BITS;
        do {
            *pos++ = readRegister(DW_UART_RX);
            lsr = readRegister(DW_UART_LSR);
            errors |= lsr & UART_LSR_BRK_ERROR_BITS;
        } while (++length < fifo_size && (lsr & (UART_LSR_DATA_READY|UART_LSR_BREAK_INT)));
        if (errors)
            LOG("Receiving data error! LSR errors: 0x%x", errors);
        if (handler)
            handler(target, this, bus.rx_buffer, length);
    }
    if (status==UART_IIR_RX_LINE_STA_INT) {
        DBG_LOG("Receiver line status interrupt, LSR: 0x%x", readRegister(DW_UART_LSR));
    } else if (status==UART_IIR_MODEM_STA_INT) {
        DBG_LOG("Modem status changed! MSR: 0x%x", readRegister(DW_UART_MSR));
    } else if (status==UART_IIR_BUSY_DETECT) {
        LOG("Detected writing LCR while busy! USR: 0x%x", readRegister(DW_UART_USR));
    }
}

void VoodooUARTController::simulateInterrupt(IOTimerEventSource* timer) {
    if (device.state == UART_SLEEP || !ready)
        return;

    transmitAndReceiveData();

    AbsoluteTime    cur_time;
    UInt64 nsecs;
    clock_get_uptime(&cur_time);
    SUB_ABSOLUTETIME(&cur_time, &last_activate_time);
    absolutetime_to_nanoseconds(cur_time, &nsecs);
    if (device.state == UART_ACTIVE) {
        if (nsecs < 100000000) { // < 100ms
            interrupt_simulator->setTimeoutMS(UART_ACTIVE_TIMEOUT);
            return;
        }
        device.state = UART_IDLE;
    }
    if (device.state == UART_IDLE) {
        if (nsecs < 1000000000) { // < 1s
            interrupt_simulator->setTimeoutMS(UART_IDLE_TIMEOUT);
            return;
        }
        device.state = UART_LONG_IDLE;
    }
    interrupt_simulator->setTimeoutMS(UART_LONG_IDLE_TIMEOUT);
}
