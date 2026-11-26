## VoodooI2C part

**Currently VoodooI2C is disabled due to none of I2C devices on Surface series is usable** (Ambient Light Sensor can be enabled but it readings are not properly configured)

VoodooI2C is a project consisting of macOS kernel extensions that add support for I2C bus devices.
This kext is intended to be installed by anyone whose computer requires some form of I2C support. It consists of I2C controller drivers and is responsible for publishing device nubs to the IOService plane.

The controller code is synced with upstream [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C): a single
`VoodooI2CPCIController` is configured per platform through its IOKit personality (`I2CClkRate`, `I2CSdaHoldTimeNs`,
`I2CSdaFallingTimeNs`, `I2CSclFallingTimeNs`, values from Linux `drivers/mfd/intel-lpss-pci.c`), and the bus timings are
derived from them unless the ACPI `SSCN`/`FMCN` methods provide their own.

## Current Status

The following Intel I2C controllers are supported:

1. `pci8086,9d60`–`9d65`, `pci8086,a160`–`a163`, `pci8086,a2e0`–`a2e3` - Sunrise Point (Skylake / Kaby Lake) era
2. `pci8086,9de8`–`9deb`, `9dc5`, `9dc6`, `pci8086,a368`–`a36b` - Cannon Lake / Whiskey Lake era
3. `pci8086,2e8`–`2eb`, `2c5`, `2c6`, `pci8086,6e8`–`6eb` - Comet Lake era
4. `pci8086,34e8`–`34eb`, `34c5`, `34c6` - Ice Lake era
5. `pci8086,a0e8`–`a0eb`, `a0c5`, `a0c6`, `a0d8`, `a0d9` - Tiger Lake LP, `pci8086,43e8`–`43eb`, `43ad`, `43ae`, `43d8` - Tiger Lake H
6. `pci8086,51e8`–`51eb`, `51c5`, `51c6`, `51d8`, `51d9` - Alder Lake P / Raptor Lake P, `pci8086,54e8`–`54eb`, `54c5`, `54c6` - Alder Lake M,
   `pci8086,7acc`–`7acf`, `7afc`, `7afd` - Alder Lake S
7. `pci8086,7a4c`–`7a4f`, `7a7c`, `7a7d` - Raptor Lake S
8. `pci8086,7e78`–`7e7b`, `7e50`, `7e51` - Meteor Lake P

The following device classes are fully supported:

1. I2C-HID devices
2. ELAN devices
3. FTE devices

Note that there is sometimes an overlap between device classes. For example, some ELAN devices may also be I2C-HID devices.

## VoodooUART

The following Intel LPSS UART controllers are supported:

| Platform | Device ids | Input clock |
|---|---|---|
| Sunrise Point LP/H | `9d27` `9d28` `9d66`, `a127` `a128` `a166` | 120 MHz |
| Cannon Lake LP/H | `9da8` `9da9` `9dc7`, `a328` `a329` `a347` | 120 MHz |
| Comet Lake LP/H | `02a8` `02a9` `02c7`, `06a8` `06a9` `06c7` | 120 MHz |
| Ice Lake LP | `34a8` `34a9` `34c7` | 120 MHz |
| Tiger Lake LP/H | `a0a8` `a0a9` `a0c7` `a0da`–`a0dd`, `43a7`–`43a9` `43da` | 100 MHz |
| Alder Lake P/M/S | `51a8` `51a9` `51c7`, `54a8` `54a9` `54c7`, `7aa8` `7aa9` `7adc` `7afe` | 100 MHz |
| Raptor Lake S | `7a28` `7a29` `7a5c` `7a7e` | 100 MHz |
| Meteor Lake P | `7e25` `7e26` `7e52` | 100 MHz |

The divisor latch is always 1, the LPSS fractional clock divider (M/N) is programmed so that the UART clock is 16 times
the requested baud rate, like Linux 8250_dw does. For example:

`34a8`  clk_rate 120 MHz, baud_rate 4000000 -> clk_config M=8 N=15 (64 MHz)

`9d27`  clk_rate 120 MHz, baud_rate 3000000 -> clk_config M=2 N=5 (48 MHz, in this config it is usable but produces a lot of transmission errors, further investigations are needed)

`a0a8`  clk_rate 100 MHz, baud_rate 4000000 -> clk_config M=16 N=25 (64 MHz)

`VoodooUARTController::matchesACPIPath()` lets a client pick the UART named by the `ResourceSource` of its
`UartSerialBusV2` descriptor when several UARTs are present.

Interrupt mode is not realised due to the failure of interrupt registration, the controller is polled.

## License

This program is protected by the GPL license. Please refer to the `LICENSE.txt` file for more information

