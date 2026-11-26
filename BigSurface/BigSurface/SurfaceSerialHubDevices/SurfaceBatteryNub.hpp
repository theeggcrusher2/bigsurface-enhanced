//
//  SurfaceBatteryNub.hpp
//  SurfaceSerialHubDevices
//
//  Created by Xavier on 2022/5/19.
//  Copyright © 2022 Xia Shangning. All rights reserved.
//

#ifndef SurfaceBatteryNub_hpp
#define SurfaceBatteryNub_hpp

#include "../SurfaceSerialHub/SurfaceSerialHubDriver.hpp"

#define BIX_LENGTH          119
#define BST_LENGTH          16

#define SURFACE_TEMP_SENSOR_MAX         16
#define SURFACE_TEMP_SENSOR_NAME_LEN    18

/* Equivalent to data returned in ACPI _BIX method, revision 0 (see Linux surface_battery.c) */
struct PACKED SurfaceBatteryBIX {
    UInt8  revision;
    UInt32 power_unit;
    UInt32 design_cap;
    UInt32 last_full_charge_cap;
    UInt32 technology;
    UInt32 design_voltage;
    UInt32 design_cap_warn;
    UInt32 design_cap_low;
    UInt32 cycle_count;
    UInt32 measurement_accuracy;
    UInt32 max_sampling_time;
    UInt32 min_sampling_time;
    UInt32 max_avg_interval;
    UInt32 min_avg_interval;
    UInt32 bat_cap_granularity_1;
    UInt32 bat_cap_granularity_2;
    char   model[21];
    char   serial[11];
    char   type[5];
    char   oem_info[21];
};
static_assert(sizeof(SurfaceBatteryBIX) == BIX_LENGTH, "Invalid BIX size");

/* Platform profile values of the TMP subsystem */
enum SurfacePerformanceMode : UInt32 {
    SurfacePerformanceModeRecommended       = 1,
    SurfacePerformanceModeBatterySaver      = 2,
    SurfacePerformanceModeBetterPerformance = 3,
    SurfacePerformanceModeBestPerformance   = 4,
};

enum SurfaceBatteryEventType {
    SurfaceBatteryInformationChanged = 0,
    SurfaceBatteryStatusChanged,
    SurfaceAdaptorStatusChanged,
};

/*
 * Nub for the SAM power and thermal subsystems: battery (BAT), AC adapter,
 * platform profile (TMP), fan (FAN) and thermal sensors (TMP).
 */
class EXPORT SurfaceBatteryNub : public SurfaceSerialHubClient {
    OSDeclareDefaultStructors(SurfaceBatteryNub);

public:
    typedef void (*EventHandler)(OSObject *owner, SurfaceBatteryNub *sender, SurfaceBatteryEventType type);

    bool attach(IOService* provider) override;

    void detach(IOService* provider) override;

    bool start(IOService* provider) override;

    void stop(IOService* provider) override;

    IOReturn setPowerState(unsigned long whichState, IOService *device) override;

    IOReturn registerBatteryEvent(OSObject* owner, EventHandler _handler);

    void unregisterBatteryEvent(OSObject* owner);

    void eventReceived(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *data_buffer, UInt16 length) override;

    /*
     * index: The index of battery, start with 1
     */
    IOReturn getBatteryConnection(UInt8 index, bool *connected);

    /*
     * index: The index of battery, start with 1
     * Returned OSArray bix need to be released by the caller!
     */
    IOReturn getBatteryInformation(UInt8 index, OSArray **bix);

    /*
     * Raw _BIX data of the battery, strings are NUL terminated.
     */
    IOReturn getBatteryInformation(UInt8 index, SurfaceBatteryBIX *bix);

    /*
     * index: The index of battery, start with 1
     * temp is left untouched if the battery temperature is not available
     */
    IOReturn getBatteryStatus(UInt8 index, UInt32 *bst, UInt16 *temp);

    /*
     * Set the battery trip point (_BTP), SAM signals a status change when the remaining
     * capacity crosses it.
     */
    IOReturn setBatteryTripPoint(UInt8 index, UInt32 capacity);

    IOReturn getAdaptorStatus(UInt32 *psr);

    IOReturn getPerformanceMode(UInt32 *mode);

    /*
     * Set the platform profile, and on models with a fan (Surface Laptop 5/6, Pro 9 and
     * later) the matching fan profile.
     */
    IOReturn setPerformanceMode(UInt32 mode);

    bool hasFan() const { return ssh && ssh->hasFeature(SurfaceSAMFeatureFanSpeed); }

    IOReturn getFanSpeed(UInt16 *rpm);

    /*
     * Bit n set means sensor n+1 is available. Only on models with thermal sensor
     * enumeration (Surface Laptop 5/6, Pro 9 and later), 0 otherwise.
     */
    UInt16 getTemperatureSensors() const { return temp_sensors; }

    const char *getTemperatureSensorName(UInt8 sensor) const;

    /*
     * sensor: sensor index, start with 1
     * temp: temperature in 0.1 Kelvin
     */
    IOReturn getTemperature(UInt8 sensor, UInt16 *temp);

    /* Build the OSArray consumed by BatteryManager from raw _BIX data */
    static OSArray *createBIXArray(const SurfaceBatteryBIX *bix);

private:
    SurfaceSerialHubDriver* ssh {nullptr};
    OSObject*               target {nullptr};
    EventHandler            handler {nullptr};

    UInt16  temp_sensors {0};
    char    temp_sensor_names[SURFACE_TEMP_SENSOR_MAX][SURFACE_TEMP_SENSOR_NAME_LEN];

    void probeTemperatureSensors();
};

#endif /* SurfaceBatteryNub_hpp */
