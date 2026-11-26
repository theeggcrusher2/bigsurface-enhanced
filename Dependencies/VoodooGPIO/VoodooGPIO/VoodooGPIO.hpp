//
//  VoodooGPIO.hpp
//  VoodooGPIO
//
//  Created by CoolStar on 8/14/17.
//  Copyright © 2017 CoolStar. All rights reserved.
//  Copyright © 2023 Visual/Noot Inc/ChefKiss Inc. All rights reserved.
//

#ifndef VoodooGPIO_h
#define VoodooGPIO_h
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOCommandGate.h>
#include "linuxirq.h"

// Exists in the macOS 10.15 SDK
#ifndef IOSafeDeleteNULL
#define IOSafeDeleteNULL(ptr, type, count)              \
    do {                                                \
        if (NULL != (ptr)) {                            \
            IODelete((ptr), type, count);               \
            (ptr) = NULL;                               \
        }                                               \
    } while (0)
#endif

inline UInt32 readl(IOVirtualAddress addr) {
    return *(const volatile UInt32 *)addr;
}

inline void writel(UInt32 b, IOVirtualAddress addr) {
    *(volatile UInt32 *)(addr) = b;
}

struct pinctrl_pin_desc {
    unsigned number;
    char *name;
    void *drv_data;
};

#define PINCTRL_PIN(a, b) {.number = a, .name = b}
#define PINCTRL_PIN_ANON(a) {.number = a}

#ifndef kIOPMPowerOff
#define kIOPMPowerOff 0
#endif

#ifndef BITS_PER_LONG
#if defined(__LP64__) && __LP64__
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif
#endif

// Must stay parenthesised: "~BIT(n)" used to expand to "(~1UL) << n", clearing all lower bits too
#ifndef BIT
#define BIT(x) (1UL << (x))
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

// Log only if current thread is interruptible, otherwise we will get a panic.
#ifndef TryLog
#define TryLog(args...) do { if (ml_get_interrupts_enabled()) IOLog(args); } while (0)
#endif

class VoodooGPIO : public IOService {
    OSDeclareAbstractStructors(VoodooGPIO);

protected:
    const struct pinctrl_pin_desc *pins = nullptr;
    size_t npins {0};
    bool controllerIsAwake = false;

    IOWorkLoop *workLoop = nullptr;
    IOCommandGate* command_gate = nullptr;
    OSArray* registered_pin_list = nullptr;
    bool isInterruptBusy = false;

    IOWorkLoop* getWorkLoop() const override;

public:
    virtual IOReturn setInterruptTypeForPin(int pin, int type) = 0;

    /* Whether the pad is asserted, e.g. a button held down. Takes the GPIO number from ACPI. */
    virtual bool getPinStatus(int pin) = 0;

    /*
     * Wait for the GPIO controller to be published, up to timeout_ns.
     *
     * Clients in other kexts must use this instead of OSDynamicCast(VoodooGPIO, ...):
     * a direct reference to the VoodooGPIO::metaClass symbol from another kext can be
     * bound to an invalid address by the kext linker, which was observed as a page
     * fault on Alder Lake-P (INTC1055) systems. metaCast() by class name goes through
     * the OSMetaClass registry instead and needs no symbol from this kext.
     */
    static inline VoodooGPIO *waitForController(UInt64 timeout_ns) {
        OSDictionary *match = IOService::serviceMatching("VoodooGPIO");
        if (!match)
            return nullptr;
        IOService *service = IOService::waitForMatchingService(match, timeout_ns);
        match->release();
        if (!service)
            return nullptr;
        VoodooGPIO *gpio = static_cast<VoodooGPIO *>(service->metaCast("VoodooGPIO"));
        // waitForMatchingService() returned a reference; the controller stays attached
        // to its ACPI provider for the lifetime of the clients.
        service->release();
        return gpio;
    }
};

#endif /* VoodooGPIO_h */
