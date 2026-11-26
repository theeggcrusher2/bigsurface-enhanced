//
//  SurfaceBatteryDriver.cpp
//  SurfaceBattery
//
//  Copyright © 2022 Xia Shangning. All rights reserved.
//

#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <Headers/kern_util.hpp>

#include "SurfaceBatteryDriver.hpp"
#include "KeyImplementations.hpp"
#include <IOKit/battery/AppleSmartBatteryCommands.h>

#define super IOService
OSDefineMetaClassAndStructors(SurfaceBatteryDriver, IOService)

volatile UInt16 SurfaceBatteryDriver::fan_rpm = 0;

void SurfaceBatteryDriver::eventReceived(SurfaceBatteryNub *sender, SurfaceBatteryEventType type) {
    // Called on the SSH work loop, defer the actual work to ours
    switch (type) {
        case SurfaceBatteryInformationChanged:
            update_bix->interruptOccurred(nullptr, this, 0);
            break;
        case SurfaceBatteryStatusChanged:
        case SurfaceAdaptorStatusChanged:
            DBG_LOG("Got BST/PSR update event");
            AbsoluteTime cur_time;
            UInt64 nsecs;
            clock_get_uptime(&cur_time);
            SUB_ABSOLUTETIME(&cur_time, &last_update);
            absolutetime_to_nanoseconds(cur_time, &nsecs);
            if (nsecs > 1000000000) {   // PSR often comes with BST changes, so avoid update it twice in a short period (1s).
                /*
                 * Quick updates for a few seconds also cover the Linux "battery update quirk":
                 * when the AC adapter changes while the battery is full, SAM does not send a
                 * separate BST event and the battery state only changes after a while.
                 */
                quick_cnt = BST_UPDATE_QUICK_CNT;
                clock_get_uptime(&last_update);
                update_bst->interruptOccurred(nullptr, this, 0);
            }
            break;
        default:
            DBG_LOG("WTF? Unknown event type");
            break;
    }
}

void SurfaceBatteryDriver::updateBatteryInformation(IOInterruptEventSource *sender, int count) {
    if (!awake or bat_missing)
        return;

    bix_fail = false;
    bool connected = false;
    if (nub->getBatteryConnection(1, &connected) != kIOReturnSuccess) {
        LOG("Failed to get battery connection status from SSH!");
        bix_fail = true;
        // It is impossible to occur unless the battery is not present
        bat_missing = true;
        return;
    }
    if (!connected) {
        BatteryManager::getShared()->updateBatteryInfoExtended(1, nullptr); // reset battery
        trip_point_set = false;
    } else if (BatteryManager::getShared()->needUpdateBIX(1) || !trip_point_set) {
        SurfaceBatteryBIX raw;
        OSArray *bix = nullptr;
        if (nub->getBatteryInformation(1, &raw) != kIOReturnSuccess || !(bix = SurfaceBatteryNub::createBIXArray(&raw))) {
            LOG("Failed to get battery information extended from SSH!");
            bix_fail = true;
        } else {
            BatteryManager::getShared()->updateBatteryInfoExtended(1, bix);
            bix->flushCollection();
            OSSafeReleaseNULL(bix);

            // Like Linux, (re-)arm the battery alarm at the design warning capacity on attach
            if (!trip_point_set && raw.design_cap_warn && raw.design_cap_warn != 0xffffffff) {
                if (nub->setBatteryTripPoint(1, raw.design_cap_warn) == kIOReturnSuccess)
                    trip_point_set = true;
                else
                    LOG("Failed to set battery trip point");
            }
        }
    }
}

void SurfaceBatteryDriver::updateBatteryStatus(IOInterruptEventSource *sender, int count) {
    if (!awake or bat_missing)
        return;

    UInt32 psr;
    bool connected = false;

    timer->cancelTimeout();
    if (bix_fail) {
        updateBatteryInformation(nullptr, 0);
        if (bix_fail)
            goto fail;
    }

    if (nub->getAdaptorStatus(&psr) != kIOReturnSuccess) {
        LOG("Failed to get power source status from SSH!");
        goto fail;
    } else
        power_connected = BatteryManager::getShared()->updateAdapterStatus(1, psr);
    BatteryManager::getShared()->externalPowerNotify(power_connected);

    if (nub->getBatteryConnection(1, &connected) != kIOReturnSuccess) {
        LOG("Failed to get battery connection status from SSH!");
        goto fail;
    }
    if (connected) {
        UInt16 temp = 0;
        UInt32 bst[4];
        if (nub->getBatteryStatus(1, bst, &temp) != kIOReturnSuccess) {
            LOG("Failed to get BST from SSH!");
            goto fail;
        } else
            BatteryManager::getShared()->updateBatteryStatus(1, bst);
        if (temp)
            BatteryManager::getShared()->updateBatteryTemperature(1, temp);
        BatteryManager::getShared()->informStatusChanged();
    } else if (trip_point_set) {
        // Battery removed (Surface Book 3 base), re-read BIX when it comes back
        bix_fail = true;
    }

    if (quick_cnt) {
        if (--quick_cnt == 0)
            sync = true;    // after finishing quick update, sync with normal update
        timer->setTimeoutMS(BST_UPDATE_QUICK);
    } else {    // sync normal update interval
        if (sync) {
            AbsoluteTime cur_time;
            UInt64 nsecs;
            sync = false;
            clock_get_uptime(&cur_time);
            IOSimpleLockLock(BatteryManager::getShared()->stateLock);
            SUB_ABSOLUTETIME(&cur_time, &BatteryManager::getShared()->lastAccess);
            IOSimpleLockUnlock(BatteryManager::getShared()->stateLock);
            absolutetime_to_nanoseconds(cur_time, &nsecs);
            UInt64 timerDelta = nsecs / (1000000ULL * BST_UPDATE_QUICK);
            if (timerDelta + 5 < BST_UPDATE_NORMAL/BST_UPDATE_QUICK) {
                timer->setTimeoutMS(BST_UPDATE_NORMAL - (UInt32)(2 + timerDelta) * BST_UPDATE_QUICK);
                return;
            }
        }
        timer->setTimeoutMS(BST_UPDATE_NORMAL);
    }
    return;
fail:
    timer->setTimeoutMS(BST_UPDATE_QUICK/2);
}

void SurfaceBatteryDriver::reportFakeBattery() {
    // Fake a 100% charging battery based on BIX from SP7.
    static const UInt8 buffer[BIX_LENGTH] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0x00,
                                             0x00, 0xc0, 0xa8, 0x00, 0x00, 0x01, 0x00, 0x00,
                                             0x00, 0x92, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                                             0x00, 0xe8, 0x03, 0x00, 0x00, 0xff, 0xff, 0xff,
                                             0xff, 0xff, 0xff, 0xff, 0xff, 0xe8, 0x03, 0x00,
                                             0x00, 0xe8, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x00,
                                             0x00, 0x0a, 0x00, 0x00, 0x00, 0x4d, 0x31, 0x31,
                                             0x30, 0x39, 0x35, 0x39, 0x37, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x30, 0x30, 0x32, 0x37, 0x30, 0x36,
                                             0x36, 0x30, 0x32, 0x30, 0x00, 0x4c, 0x49, 0x4f,
                                             0x4e, 0x00, 0x44, 0x59, 0x4e, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    SurfaceBatteryBIX raw;
    memcpy(&raw, buffer, sizeof(raw));

    OSArray *bix = SurfaceBatteryNub::createBIXArray(&raw);
    if (bix) {
        BatteryManager::getShared()->updateBatteryInfoExtended(1, bix);
        bix->flushCollection();
        OSSafeReleaseNULL(bix);
    }

    UInt32 bst[4] = {0x02, 0x00, 0xa8c0, 0x1d92};
    BatteryManager::getShared()->updateBatteryStatus(1, bst);
    BatteryManager::getShared()->updateAdapterStatus(1, true);
    BatteryManager::getShared()->externalPowerNotify(true);
}

void SurfaceBatteryDriver::pollBatteryStatus(IOTimerEventSource *sender) {
    if (bat_missing)
        reportFakeBattery();
    else
        updateBatteryStatus(nullptr, 0);
}

void SurfaceBatteryDriver::updateThermalSensors() {
    UInt16 sensors = nub->getTemperatureSensors();
    if (!sensors)
        return;

    OSDictionary *temps = OSDictionary::withCapacity(SURFACE_TEMP_SENSOR_MAX);
    if (!temps)
        return;
    for (UInt8 i = 1; i <= SURFACE_TEMP_SENSOR_MAX; i++) {
        const char *name = nub->getTemperatureSensorName(i);
        UInt16 dk;
        if (!name || nub->getTemperature(i, &dk) != kIOReturnSuccess)
            continue;
        // 0.1 Kelvin to 0.1 degree Celsius
        OSNumber *value = OSNumber::withNumber((SInt64)dk - 2731, 32);
        if (value) {
            temps->setObject(name, value);
            value->release();
        }
    }
    setProperty("Temperatures (0.1 C)", temps);
    temps->release();
}

void SurfaceBatteryDriver::pollThermalStatus(IOTimerEventSource *sender) {
    if (!awake)
        return;

    UInt16 rpm;
    if (nub->hasFan() && nub->getFanSpeed(&rpm) == kIOReturnSuccess) {
        fan_rpm = rpm;
        setProperty("Fan Speed (RPM)", rpm, 16);
    }
    if (thermal_ticks++ % THERMAL_SENSOR_UPDATE_TICKS == 0)
        updateThermalSensors();

    thermal_timer->setTimeoutMS(THERMAL_UPDATE_INTERVAL);
}

void SurfaceBatteryDriver::applyPerformanceMode() {
    OSNumber *mode = OSDynamicCast(OSNumber, getProperty("PerformanceMode"));
    if (!mode)
        return;
    UInt32 m = mode->unsigned32BitValue();
    if (nub->setPerformanceMode(m) != kIOReturnSuccess)
        LOG("Set performance mode %u failed!", m);
}

IOService *SurfaceBatteryDriver::probe(IOService *provider, SInt32 *score) {
	if (!super::probe(provider, score))
        return nullptr;

    nub = OSDynamicCast(SurfaceBatteryNub, provider);
    if (!nub)
        return nullptr;

    //TODO: SurfaceBook series have two batteries
//    OSNumber *bat_cnt_prop = OSDynamicCast(OSNumber, getProperty("BatteryCount"));
    UInt32 bat_cnt = 1;
//    if (bat_cnt_prop) {
//        bat_cnt = bat_cnt_prop->unsigned32BitValue();
//    } else {
//        LOG("Fall back to default: battery count = 1");
//    }
    BatteryManager::createShared(bat_cnt, 1);

	//TODO: implement the keys below as well
	// IB0R: sp4s or sp5s
	// IBAC: sp7s
	// PB0R = IB0R * VP0R

	return this;
}

bool SurfaceBatteryDriver::start(IOService *provider) {
	if (!super::start(provider))
		return false;

	// AppleSMC presence is a requirement, wait for it.
	auto dict = nameMatching("AppleSMC");
	auto applesmc = waitForMatchingService(dict);
    OSSafeReleaseNULL(dict);
	if (!applesmc) {
		LOG("Timeout in waiting for AppleSMC");
		return false;
	}
    OSSafeReleaseNULL(applesmc);

    const UInt8 adaptCount = BatteryManager::getShared()->adapterCount;
    const UInt8 batCount = min(BatteryManager::getShared()->batteryCount, MaxIndexCount);
    UInt32 mode;

    work_loop = IOWorkLoop::workLoop();
    if (!work_loop) {
        LOG("Could not get work loop!");
        return false;
    }

    timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SurfaceBatteryDriver::pollBatteryStatus));
    if (!timer) {
        LOG("Could not create timer!");
        goto exit;
    }
    work_loop->addEventSource(timer);

    if (nub->hasFan() || nub->getTemperatureSensors()) {
        thermal_timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SurfaceBatteryDriver::pollThermalStatus));
        if (!thermal_timer) {
            LOG("Could not create thermal timer!");
            goto exit;
        }
        work_loop->addEventSource(thermal_timer);
    }

    update_bix = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceBatteryDriver::updateBatteryInformation));
    update_bst = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceBatteryDriver::updateBatteryStatus));
    if (!update_bix || !update_bst) {
        LOG("Could not create interrupt event!");
        goto exit;
    }
    work_loop->addEventSource(update_bix);
    work_loop->addEventSource(update_bst);

    if (nub->registerBatteryEvent(this, OSMemberFunctionCast(SurfaceBatteryNub::EventHandler, this, &SurfaceBatteryDriver::eventReceived)) != kIOReturnSuccess) {
        LOG("Battery event registration failed!");
        goto exit;
    }

    if (nub->getPerformanceMode(&mode) == kIOReturnSuccess)
        LOG("Current performance mode: %u", mode);

	//WARNING: watch out, key addition is sorted here!
	if (adaptCount > 0) {
		VirtualSMCAPI::addKey(KeyACEN, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0, new ACIN));
		VirtualSMCAPI::addKey(KeyACFP, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new ACIN));
		VirtualSMCAPI::addKey(KeyACID, vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 8, SmcKeyTypeCh8s, new ACID));
		VirtualSMCAPI::addKey(KeyACIN, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new ACIN));
	}

	for (UInt8 i = 0; i < batCount; i++) {
		VirtualSMCAPI::addKey(KeyB0AC(i), vsmcPlugin.data, VirtualSMCAPI::valueWithSint16(400, new B0AC(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0AV(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(13000, new B0AV(i)));
		VirtualSMCAPI::addKey(KeyB0BI(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new B0BI(i)));
		VirtualSMCAPI::addKey(KeyB0CT(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(1, new B0CT(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0FC(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(4000, new B0FC(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0PS(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 2, SmcKeyTypeHex, new B0PS(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0RM(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(2000, new B0RM(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0St(i), vsmcPlugin.data, VirtualSMCAPI::valueWithData(nullptr, 2, SmcKeyTypeHex, new B0St(i), SMC_KEY_ATTRIBUTE_PRIVATE_WRITE|SMC_KEY_ATTRIBUTE_WRITE|SMC_KEY_ATTRIBUTE_READ));
		VirtualSMCAPI::addKey(KeyB0TF(i), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0, new B0TF(i)));
		VirtualSMCAPI::addKey(KeyTB0T(i+1), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TB0T(i)));
        if (i == 0)
            VirtualSMCAPI::addKey(KeyTB0T(0), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TB0T(0)));
	}

	VirtualSMCAPI::addKey(KeyBATP, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(true, new BATP));
	VirtualSMCAPI::addKey(KeyBBAD, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(false, new BBAD));
	VirtualSMCAPI::addKey(KeyBBIN, vsmcPlugin.data, VirtualSMCAPI::valueWithFlag(true, new BBIN));
	VirtualSMCAPI::addKey(KeyBFCL, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(100, new BFCL, SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE));
	VirtualSMCAPI::addKey(KeyBNum, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new BNum));
	VirtualSMCAPI::addKey(KeyBRSC, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(40, new BRSC, SMC_KEY_ATTRIBUTE_READ | SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_PRIVATE_WRITE));
	VirtualSMCAPI::addKey(KeyBSIn, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(0, new BSIn));

	VirtualSMCAPI::addKey(KeyCHBI, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(0, new CHBI));
	VirtualSMCAPI::addKey(KeyCHBV, vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(8000, new CHBV));
	VirtualSMCAPI::addKey(KeyCHLC, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, new CHLC));

    for (UInt8 i = 0; i < batCount; i++) {
        VirtualSMCAPI::addKey(KeyBC1V(i+1), vsmcPlugin.data, VirtualSMCAPI::valueWithUint16(defaultBatteryCellVoltage, new BC1V(i)));
    }

    if (nub->hasFan()) {
        VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data, VirtualSMCAPI::valueWithUint8(1, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));
        VirtualSMCAPI::addKey(KeyF0Ac(0), vsmcPlugin.data, VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new F0Ac));
    }

	qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);

    PMinit();
    nub->joinPMtree(this);
    registerPowerDriver(this, myIOPMPowerStates, kIOPMNumberPowerStates);

	vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
    if (!vsmcNotifier) {
        PMstop();
        goto exit;
    }

    registerService();
	return true;
exit:
    releaseResources();
    return false;
}

bool SurfaceBatteryDriver::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
	if (sensors && vsmc) {
		auto &plugin = static_cast<SurfaceBatteryDriver *>(sensors)->vsmcPlugin;
		auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
		if (ret == kIOReturnSuccess) {
			IOLog("SurfaceBatteryDriver::Plugin submitted\n");
			return true;
		} else
            IOLog("SurfaceBatteryDriver::Plugin submission failure %X\n", ret);
	}
	return false;
}

void SurfaceBatteryDriver::stop(IOService *provider) {
    /*
     * A submitted VirtualSMC plugin cannot be withdrawn, but its keys only read the
     * BatteryManager singleton, which is never freed. So it is safe to tear down here,
     * the keys simply keep reporting the last known state.
     */
    LOG("Stopping, battery state will no longer be updated");
    if (vsmcNotifier) {
        vsmcNotifier->remove();
        vsmcNotifier = nullptr;
    }
    PMstop();
    releaseResources();
    super::stop(provider);
}

void SurfaceBatteryDriver::releaseResources() {
    if (nub)
        nub->unregisterBatteryEvent(this);
    if (timer) {
        timer->cancelTimeout();
        timer->disable();
        work_loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    if (thermal_timer) {
        thermal_timer->cancelTimeout();
        thermal_timer->disable();
        work_loop->removeEventSource(thermal_timer);
        OSSafeReleaseNULL(thermal_timer);
    }
    if (update_bix) {
        update_bix->disable();
        work_loop->removeEventSource(update_bix);
        OSSafeReleaseNULL(update_bix);
    }
    if (update_bst) {
        update_bst->disable();
        work_loop->removeEventSource(update_bst);
        OSSafeReleaseNULL(update_bst);
    }
    OSSafeReleaseNULL(work_loop);
}

IOReturn SurfaceBatteryDriver::setProperties(OSObject *props) {
    OSDictionary* dict = OSDynamicCast(OSDictionary, props);
    if (!dict)
        return kIOReturnBadArgument;

    OSNumber *mode = OSDynamicCast(OSNumber, dict->getObject("PerformanceMode"));
    if (!mode)
        return kIOReturnUnsupported;

    UInt32 m = mode->unsigned32BitValue();
    if (m < SurfacePerformanceModeRecommended || m > SurfacePerformanceModeBestPerformance) {
        LOG("Invalid performance mode %u, valid values are 1-4", m);
        return kIOReturnBadArgument;
    }
    LOG("Set performance mode to %u", m);
    if (nub->setPerformanceMode(m) != kIOReturnSuccess) {
        LOG("Set performance mode failed!");
        return kIOReturnError;
    }
    // Remember it, it is re-applied after waking up
    setProperty("PerformanceMode", m, 32);
    return kIOReturnSuccess;
}

IOReturn SurfaceBatteryDriver::setPowerState(unsigned long whichState, IOService *device) {
    if (device != this)
        return kIOReturnInvalid;
    if (whichState == 0) {
        if (awake) {
            awake = false;
            bat_missing = false;
            timer->cancelTimeout();
            timer->disable();
            if (thermal_timer) {
                thermal_timer->cancelTimeout();
                thermal_timer->disable();
            }
            update_bix->disable();
            update_bst->disable();
            DBG_LOG("Going to sleep");
        }
    } else {
        if (!awake) {
            awake = true;
            timer->enable();
            update_bix->enable();
            update_bst->enable();
            bix_fail = true;
            trip_point_set = false;
            quick_cnt = BST_UPDATE_QUICK_CNT;
            clock_get_uptime(&last_update);
            // Run the first update on our work loop, not on the power management thread
            update_bst->interruptOccurred(nullptr, this, 0);
            if (thermal_timer) {
                thermal_timer->enable();
                thermal_timer->setTimeoutMS(THERMAL_UPDATE_INTERVAL);
            }
            applyPerformanceMode();
            DBG_LOG("Woke up");
        }
    }
    return kIOPMAckImplied;
}
