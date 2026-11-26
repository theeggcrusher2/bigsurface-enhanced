//
//  SurfaceHIDDriver.hpp
//  SurfaceHID
//
//  Created by Xavier on 2022/5/16.
//  Copyright © 2022 Xia Shangning. All rights reserved.
//

#ifndef SurfaceHIDDriver_hpp
#define SurfaceHIDDriver_hpp

#include "../../../BigSurface/BigSurface/SurfaceSerialHubDevices/SurfaceHIDNub.hpp"

class SurfaceHIDDevice;

#define SURFACE_HID_REPORT_QUEUE_LEN    16
#define SURFACE_HID_MAX_REPORT_LEN      256

/*
 * Reports arrive on the SSH work loop and are handed to IOHIDDevice on ours. Queue them
 * so that several reports received in one UART poll are neither lost nor reordered
 * (a lost keyboard report means a dropped key press or a stuck key).
 */
struct SurfaceHIDReportQueue {
    IOSimpleLock* lock {nullptr};
    UInt8   head {0};
    UInt8   count {0};
    bool    overflow {false};
    UInt16  length[SURFACE_HID_REPORT_QUEUE_LEN];
    UInt8   data[SURFACE_HID_REPORT_QUEUE_LEN][SURFACE_HID_MAX_REPORT_LEN];
};

class EXPORT SurfaceHIDDriver : public IOService {
	OSDeclareDefaultStructors(SurfaceHIDDriver)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	
	bool start(IOService *provider) override;
	
	void stop(IOService *provider) override;
    
    IOReturn getHIDDescriptor(SurfaceHIDDeviceType device, SurfaceHIDDescriptor *desc);
    
    IOReturn getHIDAttributes(SurfaceHIDDeviceType device, SurfaceHIDAttributes *attr);
    
    IOReturn getReportDescriptor(SurfaceHIDDeviceType device, UInt8 *buffer, UInt16 len);
    
    IOReturn getRawReport(SurfaceHIDDeviceType device, UInt8 report_id, UInt8 *buffer, UInt16 len);
    
    void setRawReport(SurfaceHIDDeviceType device, UInt8 report_id, bool feature, UInt8 *buffer, UInt16 len);
    
private:
    SurfaceHIDNub*          nub {nullptr};
    IOWorkLoop*             work_loop {nullptr};
    IOInterruptEventSource* kbd_interrupt {nullptr};
    IOInterruptEventSource* tpd_interrupt {nullptr};
    SurfaceHIDDevice*       keyboard {nullptr};
    SurfaceHIDDevice*       touchpad {nullptr};
    
    bool    legacy {false};
    bool    ready {false};
    
    IOBufferMemoryDescriptor* kbd_report {nullptr};
    IOBufferMemoryDescriptor* tpd_report {nullptr};
    SurfaceHIDReportQueue*    kbd_queue {nullptr};
    SurfaceHIDReportQueue*    tpd_queue {nullptr};

    static SurfaceHIDReportQueue *createQueue();

    static void destroyQueue(SurfaceHIDReportQueue **queue);

    bool enqueueReport(SurfaceHIDReportQueue *queue, UInt8 *buffer, UInt16 len);

    void deliverReports(SurfaceHIDReportQueue *queue, IOBufferMemoryDescriptor *report, SurfaceHIDDevice *device);
    
    void eventReceived(SurfaceHIDNub *sender, SurfaceHIDDeviceType device, UInt8 *buffer, UInt16 len);
    
    void keyboardInputReceived(IOInterruptEventSource *sender, int count);
    
    void touchpadInputReceived(IOInterruptEventSource *sender, int count);
    
    void releaseResources();
};

#endif /* SurfaceHIDDriver_hpp */
