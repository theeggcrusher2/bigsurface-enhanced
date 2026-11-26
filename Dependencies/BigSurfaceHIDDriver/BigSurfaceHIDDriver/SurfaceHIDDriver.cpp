//
//  SurfaceHIDDriver.cpp
//  SurfaceHID
//
//  Created by Xavier on 2022/5/16.
//  Copyright © 2022 Xia Shangning. All rights reserved.
//

#include "SurfaceHIDDriver.hpp"
#include "SurfaceHIDDevice.hpp"

#define super IOService
OSDefineMetaClassAndStructors(SurfaceHIDDriver, IOService)

SurfaceHIDReportQueue *SurfaceHIDDriver::createQueue() {
    SurfaceHIDReportQueue *queue = IONew(SurfaceHIDReportQueue, 1);
    if (!queue)
        return nullptr;
    memset(queue, 0, sizeof(SurfaceHIDReportQueue));
    queue->lock = IOSimpleLockAlloc();
    if (!queue->lock) {
        IODelete(queue, SurfaceHIDReportQueue, 1);
        return nullptr;
    }
    return queue;
}

void SurfaceHIDDriver::destroyQueue(SurfaceHIDReportQueue **queue) {
    if (!*queue)
        return;
    IOSimpleLockFree((*queue)->lock);
    IODelete(*queue, SurfaceHIDReportQueue, 1);
    *queue = nullptr;
}

bool SurfaceHIDDriver::enqueueReport(SurfaceHIDReportQueue *queue, UInt8 *buffer, UInt16 len) {
    if (!queue || !len)
        return false;
    if (len > SURFACE_HID_MAX_REPORT_LEN) {
        LOG("HID report too long (%d bytes), dropped", len);
        return false;
    }
    IOSimpleLockLock(queue->lock);
    if (queue->count == SURFACE_HID_REPORT_QUEUE_LEN) {
        // Should not happen, drop the oldest report so the latest state wins
        queue->head = (queue->head + 1) % SURFACE_HID_REPORT_QUEUE_LEN;
        queue->count--;
        queue->overflow = true;
    }
    UInt8 slot = (queue->head + queue->count) % SURFACE_HID_REPORT_QUEUE_LEN;
    memcpy(queue->data[slot], buffer, len);
    queue->length[slot] = len;
    queue->count++;
    IOSimpleLockUnlock(queue->lock);
    return true;
}

void SurfaceHIDDriver::deliverReports(SurfaceHIDReportQueue *queue, IOBufferMemoryDescriptor *report, SurfaceHIDDevice *device) {
    if (!queue || !report || !device)
        return;
    while (true) {
        bool overflow;
        IOSimpleLockLock(queue->lock);
        if (!queue->count) {
            IOSimpleLockUnlock(queue->lock);
            break;
        }
        UInt8 slot = queue->head;
        UInt16 len = queue->length[slot];
        report->setLength(len);
        report->writeBytes(0, queue->data[slot], len);
        queue->head = (queue->head + 1) % SURFACE_HID_REPORT_QUEUE_LEN;
        queue->count--;
        overflow = queue->overflow;
        queue->overflow = false;
        IOSimpleLockUnlock(queue->lock);

        if (overflow)
            LOG("HID report queue overflow, reports lost!");
        if (device->handleReport(report) != kIOReturnSuccess)
            LOG("Handle %s report error!", device->device_name);
    }
}

void SurfaceHIDDriver::eventReceived(SurfaceHIDNub *sender, SurfaceHIDDeviceType device, UInt8 *buffer, UInt16 len) {
    // Called on the SSH work loop: queue the report and handle it on our own work loop
    if (!ready)
        return;
    
    switch (device) {
        case SurfaceLegacyKeyboardDevice:
        case SurfaceKeyboardDevice:
            if (enqueueReport(kbd_queue, buffer, len))
                kbd_interrupt->interruptOccurred(nullptr, this, 0);
            break;
        case SurfaceTouchpadDevice:
            if (!legacy && enqueueReport(tpd_queue, buffer, len))
                tpd_interrupt->interruptOccurred(nullptr, this, 0);
            break;
        default:
            DBG_LOG("WTF? Unknown event type %d", device);
            break;
    }
}

void SurfaceHIDDriver::keyboardInputReceived(IOInterruptEventSource *sender, int count) {
    deliverReports(kbd_queue, kbd_report, keyboard);
}

void SurfaceHIDDriver::touchpadInputReceived(IOInterruptEventSource *sender, int count) {
    deliverReports(tpd_queue, tpd_report, touchpad);
}

IOService *SurfaceHIDDriver::probe(IOService *provider, SInt32 *score) {
	if (!super::probe(provider, score))
        return nullptr;

    nub = OSDynamicCast(SurfaceHIDNub, provider);
    if (!nub)
        return nullptr;
    
    OSBoolean *type = OSRequiredCast(OSBoolean, nub->getProperty(SURFACE_LEGACY_HID_STRING));
    legacy = type->getValue();
    
	return this;
}

bool SurfaceHIDDriver::start(IOService *provider) {
	if (!super::start(provider))
		return false;

    work_loop = IOWorkLoop::workLoop();
    if (!work_loop) {
        LOG("Could not create work loop!");
        goto exit;
    }
    
    if (nub->registerHIDEvent(this, OSMemberFunctionCast(SurfaceHIDNub::EventHandler, this, &SurfaceHIDDriver::eventReceived)) != kIOReturnSuccess) {
        LOG("HID event registration failed!");
        goto exit;
    }
    
    // create keyboard & touchpad devices
    keyboard = OSTypeAlloc(SurfaceHIDDevice);
    if (!keyboard) {
        LOG("Could not create keyboard device");
        goto exit;
    }
    if (legacy) {
        keyboard->device = SurfaceLegacyKeyboardDevice;
        keyboard->device_name = "Surface Legacy Keyboard";
    } else {
        keyboard->device = SurfaceKeyboardDevice;
        keyboard->device_name = "Surface Keyboard";
    }
    if (!keyboard->init() || !keyboard->attach(this)) {
        LOG("Could not init keyboard device");
        goto exit;
    }
    if (!keyboard->start(this)) {
        keyboard->detach(this);
        LOG("Could not start keyboard device");
        goto exit;
    }
    kbd_report = IOBufferMemoryDescriptor::withCapacity(SURFACE_HID_MAX_REPORT_LEN, kIODirectionNone);
    kbd_queue = createQueue();
    if (!kbd_report || !kbd_queue) {
        LOG("Could not allocate keyboard report buffers");
        goto exit;
    }
    kbd_interrupt = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceHIDDriver::keyboardInputReceived));
    if (!kbd_interrupt) {
        LOG("Could not create keyboard interrupt event!");
        goto exit;
    }
    work_loop->addEventSource(kbd_interrupt);
    kbd_interrupt->enable();

    if (!legacy) {
        touchpad = OSTypeAlloc(SurfaceHIDDevice);
        if (!touchpad) {
            LOG("Could not create touchpad device");
            goto exit;
        }
        touchpad->device = SurfaceTouchpadDevice;
        touchpad->device_name = "Surface Touchpad";
        if (!touchpad->init() || !touchpad->attach(this)) {
            LOG("Could not init Surface touchpad device");
            goto exit;
        }
        if (!touchpad->start(this)) {
            touchpad->detach(this);
            LOG("Could not start Surface touchpad device");
            goto exit;
        }
        tpd_report = IOBufferMemoryDescriptor::withCapacity(SURFACE_HID_MAX_REPORT_LEN, kIODirectionNone);
        tpd_queue = createQueue();
        if (!tpd_report || !tpd_queue) {
            LOG("Could not allocate touchpad report buffers");
            goto exit;
        }
        tpd_interrupt = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceHIDDriver::touchpadInputReceived));
        if (!tpd_interrupt) {
            LOG("Could not create touchpad interrupt event!");
            goto exit;
        }
        work_loop->addEventSource(tpd_interrupt);
        tpd_interrupt->enable();
    }
    ready = true;
    
	return true;
exit:
    releaseResources();
    return false;
}

void SurfaceHIDDriver::stop(IOService *provider) {
    releaseResources();
    super::stop(provider);
}

void SurfaceHIDDriver::releaseResources() {
    // No more events after unregistering: it is serialised with event delivery by the SSH gate
    ready = false;
    nub->unregisterHIDEvent(this);
    if (kbd_interrupt) {
        kbd_interrupt->disable();
        work_loop->removeEventSource(kbd_interrupt);
        OSSafeReleaseNULL(kbd_interrupt);
    }
    if (tpd_interrupt) {
        tpd_interrupt->disable();
        work_loop->removeEventSource(tpd_interrupt);
        OSSafeReleaseNULL(tpd_interrupt);
    }
    OSSafeReleaseNULL(work_loop);
    
    OSSafeReleaseNULL(kbd_report);
    OSSafeReleaseNULL(tpd_report);
    destroyQueue(&kbd_queue);
    destroyQueue(&tpd_queue);
    
    if (keyboard) {
        keyboard->stop(this);
        keyboard->detach(this);
        OSSafeReleaseNULL(keyboard);
    }
    if (touchpad) {
        touchpad->stop(this);
        touchpad->detach(this);
        OSSafeReleaseNULL(touchpad);
    }
}

IOReturn SurfaceHIDDriver::getHIDDescriptor(SurfaceHIDDeviceType device, SurfaceHIDDescriptor *desc) {
    return nub->getHIDDescriptor(device, desc);
}

IOReturn SurfaceHIDDriver::getHIDAttributes(SurfaceHIDDeviceType device, SurfaceHIDAttributes *attr) {
    return nub->getHIDAttributes(device, attr);
}

IOReturn SurfaceHIDDriver::getReportDescriptor(SurfaceHIDDeviceType device, UInt8 *buffer, UInt16 len) {
    return nub->getReportDescriptor(device, buffer, len);
}

IOReturn SurfaceHIDDriver::getRawReport(SurfaceHIDDeviceType device, UInt8 report_id, UInt8 *buffer, UInt16 len) {
    return nub->getHIDRawReport(device, report_id, buffer, len);
}

void SurfaceHIDDriver::setRawReport(SurfaceHIDDeviceType device, UInt8 report_id, bool feature, UInt8 *buffer, UInt16 len) {
    nub->setHIDRawReport(device, report_id, feature, buffer, len);
}
