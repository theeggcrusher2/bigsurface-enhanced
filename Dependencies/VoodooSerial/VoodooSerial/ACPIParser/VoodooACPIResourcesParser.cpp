//
//  VoodooACPIResourcesParser.cpp
//  VoodooI2C
//
//  Created by CoolStar on 8/15/17.
//  Copyright © 2017 CoolStar. All rights reserved.
//

// Demo Standalone Program: https://ghostbin.com/paste/tqt73

#include <IOKit/IOLib.h>
#include <string.h>

#include "VoodooACPIResourcesParser.hpp"
#include "linuxirq.hpp"

#define ACPI_RES_LARGE_ITEM         0x80
#define ACPI_RES_SMALL_LEN_MASK     0x07
#define ACPI_RES_SMALL_END_TAG      0x79

#define ACPI_RES_GPIO               0x8c
#define ACPI_RES_SERIAL_BUS         0x8e

/* Offset of the type specific data in a serial bus connection descriptor */
#define ACPI_SERIAL_BUS_DATA_OFFSET 12

VoodooACPIResourcesParser::VoodooACPIResourcesParser() {
    found_i2c = false;
    found_uart = false;
    found_gpio_interrupts = false;
    found_gpio_io = false;
    memset(&i2c_info, 0, sizeof(i2c_info));
    memset(&uart_info, 0, sizeof(uart_info));
    memset(&gpio_interrupts, 0, sizeof(gpio_interrupts));
    memset(&gpio_io, 0, sizeof(gpio_io));
    memset(serial_bus_source, 0, sizeof(serial_bus_source));
}

void VoodooACPIResourcesParser::parseACPISerialBus(UInt8 const* res, UInt32 offset, UInt32 desc_len) {
    if (found_i2c || found_uart)
        return;
    if (desc_len < ACPI_SERIAL_BUS_DATA_OFFSET)
        return;

    UInt8 bustype = res[offset + 5];
    UInt8 flags = res[offset + 6];

    UInt16 tflags;
    memcpy(&tflags, res + offset + 7, sizeof(UInt16));

    UInt16 datalen;
    memcpy(&datalen, res + offset + 10, sizeof(UInt16));
    if (ACPI_SERIAL_BUS_DATA_OFFSET + datalen > desc_len)
        return;

    /*
     * The ResourceSource string naming the bus controller (e.g. "\_SB.PC00.UA00")
     * follows the type specific data and runs to the end of the descriptor.
     */
    UInt32 src_off = ACPI_SERIAL_BUS_DATA_OFFSET + datalen;
    UInt32 src_len = desc_len - src_off;
    if (src_len >= sizeof(serial_bus_source))
        src_len = sizeof(serial_bus_source) - 1;
    memcpy(serial_bus_source, res + offset + src_off, src_len);
    serial_bus_source[src_len] = '\0';

    if (bustype == BUS_TYPE_I2C) {
        if (datalen < 6)
            return;
        found_i2c = true;

        i2c_info.resource_consumer = (flags >> 1) & 0x1;
        i2c_info.device_initiated = flags & 0x1;
        i2c_info.address_mode_10Bit = tflags & 0x1;

        UInt32 busspeed;
        memcpy(&busspeed, res + offset + 12, sizeof(UInt32));
        i2c_info.bus_speed = busspeed;

        UInt16 address;
        memcpy(&address, res + offset + 16, sizeof(UInt16));
        i2c_info.address = address;
    } else if (bustype == BUS_TYPE_UART) {
        if (datalen < 10)
            return;
        found_uart = true;

        uart_info.resource_consumer = flags>>1 & 0x1;
        uart_info.device_initiated = flags & 0x1;

        uart_info.flow_control = tflags & 0x03;
        uart_info.stop_bits = tflags>>2 & 0x03;
        uart_info.data_bits = tflags>>4 & 0x07;
        uart_info.big_endian = tflags>>7 & 0x01;

        UInt32 baudrate;
        memcpy(&baudrate, res + offset + 12, sizeof(UInt32));
        uart_info.baudrate = baudrate;

        UInt16 rx_fifo;
        memcpy(&rx_fifo, res + offset + 16, sizeof(UInt16));
        uart_info.rx_fifo = rx_fifo;

        UInt16 tx_fifo;
        memcpy(&tx_fifo, res + offset + 18, sizeof(UInt16));
        uart_info.tx_fifo = tx_fifo;

        UInt8 parity;
        memcpy(&parity, res + offset + 20, sizeof(UInt8));
        uart_info.parity = parity;

        UInt8 lines_enabled;
        memcpy(&lines_enabled, res + offset + 21, sizeof(UInt8));
        uart_info.dtd_enabled = lines_enabled>>2 & 0x01;
        uart_info.ri_enabled  = lines_enabled>>3 & 0x01;
        uart_info.dsr_enabled = lines_enabled>>4 & 0x01;
        uart_info.dtr_enabled = lines_enabled>>5 & 0x01;
        uart_info.cts_enabled = lines_enabled>>6 & 0x01;
        uart_info.rts_enabled = lines_enabled>>7 & 0x01;
    }
}

void VoodooACPIResourcesParser::parseACPIGPIO(UInt8 const* res, UInt32 offset, UInt32 desc_len) {
    if (desc_len < 21)
        return;

    UInt8 gpio_type = res[offset + 4];
    UInt8 flags = res[offset + 5];

    UInt8 gpio_flags = res[offset + 7];

    UInt8 pin_config = res[offset + 9];

    UInt16 pin_offset;
    memcpy(&pin_offset, res + offset + 14, sizeof(UInt16));
    if (pin_offset + sizeof(UInt16) > desc_len)
        return;

    UInt16 pin_number;
    memcpy(&pin_number, res + offset + pin_offset, sizeof(UInt16));

    if (pin_number == 0xFFFF) // pinNumber 0xFFFF is invalid
        return;

    if (gpio_type == 0) {
        // GPIOInt, only the first one is used
        if (found_gpio_interrupts)
            return;
        found_gpio_interrupts = true;

        gpio_interrupts.resource_consumer = flags & 0x1;
        gpio_interrupts.level_interrupt = !(gpio_flags & 0x1);

        gpio_interrupts.interrupt_polarity = (gpio_flags >> 1) & 0x3;

        gpio_interrupts.shared_interrupt = (gpio_flags >> 3) & 0x1;
        gpio_interrupts.wake_interrupt = (gpio_flags >> 4) & 0x1;

        gpio_interrupts.pin_config = pin_config;
        gpio_interrupts.pin_number = pin_number;

        int irq = 0;
        if (gpio_interrupts.level_interrupt) {
            switch (gpio_interrupts.interrupt_polarity) {
                case 0:
                    irq = IRQ_TYPE_LEVEL_HIGH;
                    break;
                case 1:
                    irq = IRQ_TYPE_LEVEL_LOW;
                    break;
                default:
                    irq = IRQ_TYPE_LEVEL_LOW;
                    break;
            }
        } else {
            switch (gpio_interrupts.interrupt_polarity) {
                case 0:
                    irq = IRQ_TYPE_EDGE_FALLING;
                    break;
                case 1:
                    irq = IRQ_TYPE_EDGE_RISING;
                    break;
                case 2:
                    irq = IRQ_TYPE_EDGE_BOTH;
                    break;
                default:
                    irq = IRQ_TYPE_EDGE_FALLING;
                    break;
            }
        }
        gpio_interrupts.irq_type = irq;
    } else if (gpio_type == 1) {
        // GPIOIo, only the first one is used
        if (found_gpio_io)
            return;
        found_gpio_io = true;

        gpio_io.resource_consumer = flags & 0x1;
        gpio_io.io_restriction = gpio_flags & 0x3;
        gpio_io.sharing = (gpio_flags >> 3) & 0x1;

        gpio_io.pin_config = pin_config;
        gpio_io.pin_number = pin_number;
    }
}

UInt32 VoodooACPIResourcesParser::parseACPIResources(UInt8 const* res, UInt32 offset, UInt32 sz) {
    while (offset < sz) {
        UInt8 opcode = res[offset];

        if (!(opcode & ACPI_RES_LARGE_ITEM)) {
            // Small resource descriptor: 1 byte header, length in bits 2:0
            if (opcode == ACPI_RES_SMALL_END_TAG)
                break;
            offset += 1 + (opcode & ACPI_RES_SMALL_LEN_MASK);
            continue;
        }

        // Large resource descriptor: 1 byte tag + 2 bytes length
        if (offset + 3 > sz)
            break;
        UInt16 len;
        memcpy(&len, res + offset + 1, sizeof(UInt16));
        UInt32 desc_len = 3 + len;
        if (offset + desc_len > sz)
            break;

        /*
         * Once a GpioInt (or serial bus) has been found, stop at the next descriptor of the
         * same kind and return its offset: callers with several interrupts (e.g. the three
         * Surface buttons) walk them by resuming a fresh parser from the returned offset.
         */
        if (opcode == ACPI_RES_GPIO) {
            if (found_gpio_interrupts)
                break;
            parseACPIGPIO(res, offset, desc_len);
        } else if (opcode == ACPI_RES_SERIAL_BUS) {
            if (found_i2c || found_uart)
                break;
            parseACPISerialBus(res, offset, desc_len);
        }

        offset += desc_len;
    }
    return offset;
}
