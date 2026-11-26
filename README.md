# BigSurface
The name comes from macOS Big Sur.
Big Sur + Surface -> Big Surface (LOL)

PS : If you have a better name, please let me know.

**A proposition for a fully intergrated kext for all Surface related hardwares**

## How to install

You will need to first **DELETE** all the original **VoodooI2C** kexts including its plugins (all of them), **SMCBatteryManager.kext**, **SMCLightSensor.kext** and **SSDT_ALS0.aml**, then add BigSurface and SSDT-SURFACE.aml into opencore's `config.plist` in the order specified as below<img width="897" alt="截屏2023-02-09 23 22 47" src="https://user-images.githubusercontent.com/18528518/217953005-a00e42ff-39c5-44f8-90e9-4e8efba6b1ea.png">

BigSurface bundles its own copies of VoodooGPIO (`org.coolstar.VoodooGPIO`) and VoodooSerial
(I2C + UART). Do not load another VoodooGPIO next to it.

## Which Surface series are supported ?
The Surface Aggregator Module (SAM) is reached through an Intel LPSS UART. Check the UART device id in Windows
(Device Manager → *Intel(R) Serial IO UART Host Controller* → Hardware Ids):

| Platform | UART ids | Models |
|---|---|---|
| Sunrise Point (Skylake / Kaby Lake) | `9d27` `9d28` `9d66` | Surface Pro 4, 5, 6, Book 2, Laptop 1, 2 |
| Ice Lake | `34a8` `34a9` `34c7` | Surface Pro 7, Laptop 3 (Intel), Book 3, Laptop Go 1 |
| Tiger Lake | `a0a8` `a0a9` `a0c7` `a0da`–`a0dd`, TGL-H `43a7`–`43a9` `43da` | Surface Laptop 4 (Intel), Pro 7+, Pro 8, Laptop Go 2, Laptop Studio |
| Alder Lake / Raptor Lake | `51a8` `51a9` `51c7`, `54a8` `54a9` `54c7`, `7aa8` `7aa9` `7adc` `7afe`, `7a28` `7a29` `7a5c` `7a7e` | Surface Laptop 5, Pro 9, Laptop Go 3, Laptop Studio 2 |
| Meteor Lake | `7e25` `7e26` `7e52` | Surface Laptop 6, Pro 10 |

Cannon Lake and Comet Lake UARTs are matched as well. The UART input clock (120 MHz up to Ice Lake, 100 MHz from Tiger Lake on)
is set per platform and the clock divider is computed from the baud rate in the ACPI `UartSerialBusV2` descriptor. When a
platform exposes more than one UART, the one named by that descriptor is used.

`34a8` is known to work well, `9d27` is usable but still produces some transmission errors. **Tiger Lake and newer have not been
tested on real hardware yet**, please report your results (a `log show --last boot --predicate 'sender == "BigSurface"'` dump helps).

The GPIO controllers (VoodooGPIO) are supported for Sunrise Point LP/H, Cannon Lake LP/H, Ice Lake LP, Tiger Lake LP/H
(`INT34C5`, `INT34C6`), Alder Lake / Raptor Lake P (`INTC1055`), Alder Lake S / Raptor Lake S (`INTC1056`, `INTC1085`),
Alder Lake N (`INTC1057`) and Meteor Lake P (`INTC1083`, `INTC105E`). The I2C controllers are supported from Sunrise Point up
to Meteor Lake (see `Dependencies/VoodooSerial/README.md`).

The Surface Laptop 3/4 15" with AMD CPUs are not supported, their SAM is connected to an AMD UART.

## What works
- Surface Type Cover
  > The code is based on VoodooI2CHID.kext, but added **integrated and hot pluggable touchpad&keyboard support**.
- Buttons
  > Power/VolumeUp/VolumeDown buttons all works properly
- Ambient Light Sensor
  > ACPI device name: ACSD, attached under I2C4. **ONLY SP7 and SL3 devices use this hardware** for other devices, you need to write your own driver and then make a pull request. I am happy to merge your driver in :)
- Battery status--Surface Serial Hub
  > UART driver as well as MS's SAM module driver are implemented. 
  > The battery trip point is armed at the design warning capacity, so SAM reports a low battery in time.
  > **TODO**: support dual batteries for SB3.
- Performance mode
  > Set with `PerformanceMode` in the `Surface Battery` personality (default 0x01), or at runtime with
  > `ioio -s SurfaceBatteryDriver PerformanceMode 3`. The last value set is restored after waking up.
  > On models with a SAM controlled fan (Surface Laptop 5/6, Pro 9 and later) the matching fan profile is set as well,
  > like Linux `surface_platform_profile` does.
- Fan speed and thermal sensors (Surface Laptop 5/6, Pro 9 and later)
  > The fan speed is published as SMC key `F0Ac` (visible in tools like Stats/iStat) and as `Fan Speed (RPM)` on
  > `SurfaceBatteryDriver`. The SAM thermal sensors are listed in `Temperatures (0.1 C)` on the same object.
- Surface Laptop 3/4/5/6 keyboard & touchpad
  > Works on the Laptop 3, all keys and gestures are recognised properly. Laptop 4, 5 and 6 use the same SAM HID devices.
  > 
  > Known issue: neither keyboard nor touchpad can wake up the system.
- Touch Screen & Stylus Yes, this also works :)
  > The code is ported from linux, including `mei` and surface-linux's `ipts` & `iptsd` drivers.
  > 
  > 10 finger touch & stylus
  > 
  > **Important**: need a SSDT patch (SSDT-SURFACE) and a user-space daemon ([IPTSDaemon](https://github.com/Xiashangning/IPTSDaemon)) to work with.
  >
  > Only devices up to Ice Lake (Surface Pro 4–7, Book 1–3, Laptop 3) expose the touch screen through the Management Engine.
  > From Tiger Lake on (Surface Laptop 4 and later, Pro 7+ and later) it is attached to the Intel Touch Host Controller (ITHC),
  > which is not supported yet. Linux supports it with [ithc](https://github.com/quo/ithc-linux).
  
Possible values for Performance mode are:

      State              Value
      
    Recommended          0x01
    
    Battery Saver        0x02 (Only in battery mode)
    
    Better Performance   0x03
    
    Best Performance     0x04

## Configuration
These properties can be changed in the IOKit personalities of `BigSurface.kext/Contents/Info.plist`:

| Personality | Property | Default | Meaning |
|---|---|---|---|
| Surface Battery | `PerformanceMode` | 1 | Platform profile, see above |
| Surface Serial Hub | `NubPublishDelay` | 20000 | Delay in ms before the battery and HID devices are published after SAM is up |
| Surface Serial Hub | `SAMFeatures` | (detected) | Override the optional SAM functions of unknown models, a sum of: `0x01` battery, `0x02` performance mode, `0x04` fan profile, `0x08` fan speed, `0x10` thermal sensors, `0x20` legacy keyboard (Laptop 1/2). A Laptop 5 is `0x1F` |

The model is detected from the ACPI id of the SSAM platform hub (e.g. `MSHW0250` for the Surface Laptop 4) and shown as
`Surface Model` on `SurfaceSerialHubDriver` in IORegistryExplorer.
    
## TODO
- Cameras                            Impossible so far
  > ACPI devices: CAMR,CAMF,CAM3(infrared camera)
  > 
  > Corresponding device id: OV8865,OV5693,OV7251
  > 
  > Even Linux failed to drive the cameras on SP7 (IPU4), SP6 and before (IPU3) might be possible but I do not have the device.
- Touch screen on Tiger Lake and later (Intel Touch Host Controller, see above)

## Important
Surface Pro 7 and Book 3 seem to have issues for macOS to recognize properly the battery in System Preferences. PM Profile in FACP is tablet not laptop.

Thus, an ACPI patch is needed:

```
Table Signature: 46414350
Find: 00080900 B2000000 F0F1
Replace: 00020900 B2000000 F0F1
```

Thanks to @he1833

The lid only wakes the system if its GPE is enabled as a wake source, which Linux does with `surface_gpe`.
The lid GPEs are `_L17` (Pro 4, Book 1/2), `_L4F` (Pro 5/6), `_L4D` (Pro 7, Book 3, Laptop 3), `_L4B` (Pro 8, Laptop 4 Intel,
Laptop Studio) and `_L57` (Laptop 1/2). If lid wake does not work, point the `_PRW` of the lid device at that GPE in an SSDT.

## Changes in 1.3.0
- Tiger Lake, Alder Lake, Raptor Lake and Meteor Lake support for the UART, I2C and GPIO controllers
- I2C controller code synced with upstream VoodooI2C (timings derived from the platform clock like Linux, FIFO depth read from
  the controller, interrupt fixes), VoodooGPIO synced with upstream (Tiger/Alder Lake tables, Intel/AMD split) plus Meteor Lake
- Surface Serial Hub rewritten around the Linux packet layer: length based frame parsing, one sequenced packet in flight,
  retransmission detection, timeouts and retries like Linux, event registrations restored after wake, and fixes for a
  use-after-free, a buffer overflow and several races between requests and responses
- Battery: no more kernel panic when the driver stops, battery trip point, leak fixes; performance mode is validated, kept
  across sleep and also sets the fan profile where needed; fan speed and thermal sensors on Surface Laptop 5/6
- Keyboard/touchpad reports are queued instead of overwriting each other when several arrive at once
- The dependency submodules are now part of this repository (`Dependencies/`), only MacKernelSDK is still a submodule

## If you like my project, please consider to star this project, thanks!
### If you appreciate my effort and would like to pay me a coffee, here is my PayPal address: `ritchiexia@163.com`
