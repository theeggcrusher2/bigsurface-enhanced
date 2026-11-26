//
//  SurfaceBatteryNub.cpp
//  SurfaceSerialHubDevices
//
//  Created by Xavier on 2022/5/19.
//  Copyright © 2022 Xia Shangning. All rights reserved.
//
//  SAM requests follow Linux drivers/power/supply/surface_battery.c, surface_charger.c,
//  drivers/platform/surface/surface_platform_profile.c and drivers/hwmon/surface_{fan,temp}.c
//

#include "SurfaceBatteryNub.hpp"

#define super SurfaceSerialHubClient
OSDefineMetaClassAndStructors(SurfaceBatteryNub, SurfaceSerialHubClient)

/* Thermal sensor name response, see Linux surface_temp.c */
struct PACKED SurfaceTemperatureSensorName {
    UInt16 unknown1;
    char   unknown2;
    char   name[SURFACE_TEMP_SENSOR_NAME_LEN];
};
static_assert(sizeof(SurfaceTemperatureSensorName) == 21, "Invalid sensor name response size");

/* Platform profile response, see Linux surface_platform_profile.c */
struct PACKED SurfacePerformanceModeInfo {
    UInt32 profile;
    UInt16 unknown1;
    UInt16 unknown2;
};

bool SurfaceBatteryNub::attach(IOService* provider) {
    if (!super::attach(provider))
        return false;

    ssh = OSDynamicCast(SurfaceSerialHubDriver, provider);
    if (!ssh)
        return false;

    return true;
}

void SurfaceBatteryNub::detach(IOService* provider) {
    ssh = nullptr;
    super::detach(provider);
}

bool SurfaceBatteryNub::start(IOService *provider) {
    if (!super::start(provider))
        return false;

    memset(temp_sensor_names, 0, sizeof(temp_sensor_names));
    probeTemperatureSensors();
    setProperty("Fan", hasFan());

    PMinit();
    ssh->joinPMtree(this);
    registerPowerDriver(this, myIOPMPowerStates, kIOPMNumberPowerStates);

    registerService();
    return true;
}

void SurfaceBatteryNub::stop(IOService *provider) {
    unregisterBatteryEvent(target);
    PMstop();
    super::stop(provider);
}

IOReturn SurfaceBatteryNub::setPowerState(unsigned long whichState, IOService *device) {
    if (device != this)
        return kIOReturnInvalid;
    return kIOPMAckImplied;
}

IOReturn SurfaceBatteryNub::registerBatteryEvent(OSObject* owner, EventHandler _handler) {
    if (!owner || !_handler)
        return kIOReturnError;
    if (target) {
        LOG("Battery event already registered for a handler!");
        return kIOReturnNoResources;
    }

    // Set before enabling the events, an event may arrive right away
    target = owner;
    handler = _handler;

    // SAM expects battery events to be enabled for instance 0, see Linux spwr_battery_init()
    IOReturn ret = ssh->registerEvent(this, SurfaceSerialEventHostManagedV1, SSH_TC_BAT, 0x00);
    if (ret != kIOReturnSuccess) {
        target = nullptr;
        handler = nullptr;
    }
    return ret;
}

void SurfaceBatteryNub::unregisterBatteryEvent(OSObject* owner) {
    if (target) {
        if (target == owner) {
            ssh->unregisterEvent(this, SurfaceSerialEventHostManagedV1, SSH_TC_BAT, 0x00);
            target = nullptr;
            handler = nullptr;
        } else
            LOG("Battery event not registered for this handler!");
    }
}

void SurfaceBatteryNub::eventReceived(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *data_buffer, UInt16 length) {
    // iid corresponds to battery/adaptor index, starts with 1
    SurfaceBatteryEventType type;
    switch (cid) {
        case SSH_EVENT_CID_BAT_BIX:
            type = SurfaceBatteryInformationChanged;
            break;
        case SSH_EVENT_CID_BAT_BST:
            type = SurfaceBatteryStatusChanged;
            break;
        case SSH_EVENT_CID_BAT_PSR:
            type = SurfaceAdaptorStatusChanged;
            break;
        case SSH_EVENT_CID_BAT_PROT:
        case SSH_EVENT_CID_BAT_DPTF:
            // Battery protection and DPTF notifications, not handled (same as Linux)
            DBG_LOG("Ignoring battery event cid %x, iid %d", cid, iid);
            return;
        default:
            DBG_LOG("Unknown battery event! tid: %d, iid: %d, cid: %x, data_len: %d", tid, iid, cid, length);
            return;
    }
    if (handler)
        handler(target, this, type);
}

IOReturn SurfaceBatteryNub::getBatteryConnection(UInt8 index, bool *connected) {
    UInt32 sta = 0x00;
    if (ssh->getResponse(SSH_TC_BAT, index, 0x01, SSH_CID_BAT_STA, nullptr, 0, true, reinterpret_cast<UInt8 *>(&sta), sizeof(sta)) != kIOReturnSuccess)
        return kIOReturnError;

    *connected = (sta & 0x10) ? true : false;

    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::getBatteryInformation(UInt8 index, SurfaceBatteryBIX *bix) {
    UInt16 received = 0;
    memset(bix, 0, sizeof(SurfaceBatteryBIX));
    if (ssh->getResponse(SSH_TC_BAT, index, 0x01, SSH_CID_BAT_BIX, nullptr, 0, true, reinterpret_cast<UInt8 *>(bix), sizeof(SurfaceBatteryBIX), &received) != kIOReturnSuccess)
        return kIOReturnError;
    if (received < offsetof(SurfaceBatteryBIX, model)) {
        LOG("BIX response too short (%d bytes)", received);
        return kIOReturnUnderrun;
    }

    // Enforce NUL terminated strings in case anything goes wrong
    bix->model[sizeof(bix->model) - 1] = '\0';
    bix->serial[sizeof(bix->serial) - 1] = '\0';
    bix->type[sizeof(bix->type) - 1] = '\0';
    bix->oem_info[sizeof(bix->oem_info) - 1] = '\0';

    if (bix->revision != 0)
        LOG("Unsupported battery BIX revision: %u", bix->revision);
    return kIOReturnSuccess;
}

OSArray *SurfaceBatteryNub::createBIXArray(const SurfaceBatteryBIX *bix) {
    const OSObject *arr[] = {
        OSNumber::withNumber(bix->revision, 8),
        OSNumber::withNumber(bix->power_unit, 32),
        OSNumber::withNumber(bix->design_cap, 32),
        OSNumber::withNumber(bix->last_full_charge_cap, 32),
        OSNumber::withNumber(bix->technology, 32),
        OSNumber::withNumber(bix->design_voltage, 32),
        OSNumber::withNumber(bix->design_cap_warn, 32),
        OSNumber::withNumber(bix->design_cap_low, 32),
        OSNumber::withNumber(bix->cycle_count, 32),
        OSNumber::withNumber(bix->measurement_accuracy, 32),
        OSNumber::withNumber(bix->max_sampling_time, 32),
        OSNumber::withNumber(bix->min_sampling_time, 32),
        OSNumber::withNumber(bix->max_avg_interval, 32),
        OSNumber::withNumber(bix->min_avg_interval, 32),
        OSNumber::withNumber(bix->bat_cap_granularity_1, 32),
        OSNumber::withNumber(bix->bat_cap_granularity_2, 32),
        OSString::withCString(bix->model),
        OSString::withCString(bix->serial),
        OSString::withCString(bix->type),
        OSString::withCString(bix->oem_info),
    };
    const unsigned int count = sizeof(arr) / sizeof(arr[0]);

    OSArray *array = nullptr;
    bool complete = true;
    for (unsigned int i = 0; i < count; i++)
        complete &= arr[i] != nullptr;
    if (complete)
        array = OSArray::withObjects(arr, count);

    // The array holds its own references
    for (unsigned int i = 0; i < count; i++) {
        if (arr[i])
            arr[i]->release();
    }
    return array;
}

IOReturn SurfaceBatteryNub::getBatteryInformation(UInt8 index, OSArray **bix) {
    SurfaceBatteryBIX raw;
    IOReturn ret = getBatteryInformation(index, &raw);
    if (ret != kIOReturnSuccess)
        return ret;

    *bix = createBIXArray(&raw);
    if (!(*bix)) {
        LOG("Could not create the BIX array!");
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::getBatteryStatus(UInt8 index, UInt32 *bst, UInt16 *temp) {
    if (ssh->getResponse(SSH_TC_BAT, index, 0x01, SSH_CID_BAT_BST, nullptr, 0, true, reinterpret_cast<UInt8 *>(bst), BST_LENGTH) != kIOReturnSuccess)
        return kIOReturnError;

    // Skip the battery sensor if enumeration says it does not exist on this model
    if (temp_sensors && !(temp_sensors & BIT(SSH_TEMP_SENSOR_BAT - 1)))
        return kIOReturnSuccess;
    if (getTemperature(SSH_TEMP_SENSOR_BAT, temp) != kIOReturnSuccess)
        DBG_LOG("Failed to get battery temperature!");

    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::setBatteryTripPoint(UInt8 index, UInt32 capacity) {
    if (!ssh->sendCommand(SSH_TC_BAT, index, 0x01, SSH_CID_BAT_BTP, reinterpret_cast<UInt8 *>(&capacity), sizeof(capacity), true))
        return kIOReturnError;
    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::getAdaptorStatus(UInt32 *psr) {
    return ssh->getResponse(SSH_TC_BAT, SSH_TID_PRIMARY, 0x01, SSH_CID_BAT_PSR, nullptr, 0, true, reinterpret_cast<UInt8 *>(psr), 4);
}

IOReturn SurfaceBatteryNub::getPerformanceMode(UInt32 *mode) {
    SurfacePerformanceModeInfo info = {};
    UInt16 received = 0;
    IOReturn ret = ssh->getResponse(SSH_TC_TMP, SSH_TID_PRIMARY, 0, SSH_CID_TMP_GET_PERF, nullptr, 0, true, reinterpret_cast<UInt8 *>(&info), sizeof(info), &received);
    if (ret != kIOReturnSuccess)
        return ret;
    if (received < sizeof(info.profile))
        return kIOReturnUnderrun;
    *mode = info.profile;
    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::setPerformanceMode(UInt32 mode) {
    UInt8 fan_profile;
    switch (mode) {
        // The fan profile ids do not match the platform profile ids (Linux ssam_fan_profile)
        case SurfacePerformanceModeRecommended:         fan_profile = 2; break;
        case SurfacePerformanceModeBatterySaver:        fan_profile = 1; break;
        case SurfacePerformanceModeBetterPerformance:   fan_profile = 3; break;
        case SurfacePerformanceModeBestPerformance:     fan_profile = 4; break;
        default:
            LOG("Invalid performance mode %u", mode);
            return kIOReturnBadArgument;
    }

    if (!ssh->sendCommand(SSH_TC_TMP, SSH_TID_PRIMARY, 0, SSH_CID_TMP_SET_PERF, reinterpret_cast<UInt8 *>(&mode), sizeof(mode), true))
        return kIOReturnError;

    if (ssh->hasFeature(SurfaceSAMFeatureFanProfile) &&
        !ssh->sendCommand(SSH_TC_FAN, SSH_TID_PRIMARY, 0x01, SSH_CID_FAN_SET_PROFILE, &fan_profile, sizeof(fan_profile), true))
        return kIOReturnError;

    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryNub::getFanSpeed(UInt16 *rpm) {
    if (!hasFan())
        return kIOReturnUnsupported;
    UInt16 received = 0;
    IOReturn ret = ssh->getResponse(SSH_TC_FAN, SSH_TID_PRIMARY, 0x01, SSH_CID_FAN_GET_RPM, nullptr, 0, true, reinterpret_cast<UInt8 *>(rpm), sizeof(UInt16), &received);
    if (ret == kIOReturnSuccess && received < sizeof(UInt16))
        ret = kIOReturnUnderrun;
    return ret;
}

IOReturn SurfaceBatteryNub::getTemperature(UInt8 sensor, UInt16 *temp) {
    UInt16 value = 0, received = 0;
    IOReturn ret = ssh->getResponse(SSH_TC_TMP, SSH_TID_PRIMARY, sensor, SSH_CID_TMP_SENSOR, nullptr, 0, true, reinterpret_cast<UInt8 *>(&value), sizeof(value), &received);
    if (ret != kIOReturnSuccess)
        return ret;
    if (received < sizeof(value))
        return kIOReturnUnderrun;
    *temp = value;
    return kIOReturnSuccess;
}

const char *SurfaceBatteryNub::getTemperatureSensorName(UInt8 sensor) const {
    if (sensor < 1 || sensor > SURFACE_TEMP_SENSOR_MAX || !(temp_sensors & BIT(sensor - 1)))
        return nullptr;
    return temp_sensor_names[sensor - 1];
}

void SurfaceBatteryNub::probeTemperatureSensors() {
    temp_sensors = 0;
    if (!ssh->hasFeature(SurfaceSAMFeatureThermalSensors))
        return;

    UInt16 sensors = 0, received = 0;
    if (ssh->getResponse(SSH_TC_TMP, SSH_TID_PRIMARY, 0, SSH_CID_TMP_GET_SENSORS, nullptr, 0, true, reinterpret_cast<UInt8 *>(&sensors), sizeof(sensors), &received) != kIOReturnSuccess || received < sizeof(sensors)) {
        LOG("Failed to enumerate thermal sensors");
        return;
    }

    OSDictionary *names = OSDictionary::withCapacity(SURFACE_TEMP_SENSOR_MAX);
    for (int i = 0; i < SURFACE_TEMP_SENSOR_MAX; i++) {
        if (!(sensors & BIT(i)))
            continue;
        SurfaceTemperatureSensorName rsp = {};
        if (ssh->getResponse(SSH_TC_TMP, SSH_TID_PRIMARY, i + 1, SSH_CID_TMP_GET_NAME, nullptr, 0, true, reinterpret_cast<UInt8 *>(&rsp), sizeof(rsp)) != kIOReturnSuccess) {
            LOG("Failed to get name of thermal sensor %d", i + 1);
            sensors &= ~BIT(i);
            continue;
        }
        strlcpy(temp_sensor_names[i], rsp.name, SURFACE_TEMP_SENSOR_NAME_LEN);
        if (!temp_sensor_names[i][0])
            snprintf(temp_sensor_names[i], SURFACE_TEMP_SENSOR_NAME_LEN, "Sensor %d", i + 1);
        if (names) {
            char key[8];
            snprintf(key, sizeof(key), "%d", i + 1);
            OSString *name = OSString::withCString(temp_sensor_names[i]);
            if (name) {
                names->setObject(key, name);
                name->release();
            }
        }
    }
    temp_sensors = sensors;
    LOG("Thermal sensors: 0x%04x", temp_sensors);
    if (names) {
        setProperty("Thermal Sensors", names);
        names->release();
    }
}
