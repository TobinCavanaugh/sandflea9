//
// Created by tobin on 2025-11-30.
//

#include "../include/kern_pci.h"
#include "../include/kern_asmstubs.h"

u32 pci_serial_base = 0;


u0 init_uart_port(u32 port) {
    outb(port + 1, 0x00); // Disable all interrupts
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(port + 1, 0x00); //                  (hi byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set
}


u0 pci_enable_device_io(u8 bus, u8 slot, u8 func) {
    u32 command_reg = pci_read_32(bus, slot, func, 0x04);
    command_reg |= 0x5;

    u32 addr = (u32) ((bus << 16) | (slot << 11) | (func << 8) | 0x04 | 0x80000000);
    asm volatile("outl %0, %1" : : "a"(addr), "Nd"((u16) 0xCF8));
    asm volatile("outl %0, %1" : : "a"(command_reg), "Nd"((u16) 0xCFC));
}

u0 serial_outc_pci(const char c) {
    if (pci_serial_base == 0) return;
    while ((inb(pci_serial_base + 5) & 0x20) == 0);
    outb(pci_serial_base + 0, c);
}

u0 serial_outs_pci(const char *s) {
    if (pci_serial_base == 0) return;
    while (*s) {
        serial_outc_pci(*s++);
    }
}

u0 pci_scan_and_init_serial() {
    // serial_outs("Scanning PCI bus...");

    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {

            u16 vendor_id = pci_read_16(bus, slot, 0, 0x00);

            for (u8 func = 0; func < 8; func++) {
                u32 id_reg = pci_read_32(bus, slot, func, 0x00);

                if (vendor_id == 0xFFFF) continue; // device doesn't exist

                u32 class_reg = pci_read_32(bus, slot, func, 0x08);
                u8 class_code = (class_reg >> 24) & 0xFF;
                u8 subclass = (class_reg >> 16) & 0xFF;

                //
                if (class_code == 0x07 && subclass == 0x00) {
                    //@formatter:off
                    serial_outs("Found Serial Controller at B:D:F ");
                    serial_outi64(bus,  BASE_10); serial_outc(':');
                    serial_outi64(slot, BASE_10); serial_outc(':');
                    serial_outi64(func, BASE_10); serial_outs("\n");
                    //@formatter:on

                    u32 bar0 = pci_read_32(bus, slot, func, 0x10);

                    // check if its IO mapped (1) or mmio (0)
                    if (bar0 & 1) {
                        pci_enable_device_io(bus, slot, func);
                        pci_serial_base = bar0 & 0xffffFFFC;
                        serial_outs("Found PCI serial IO at: 0x");
                        serial_outi64(bus, BASE_HEX);
                        serial_outs("\n");

                        init_uart_port(pci_serial_base);

                        serial_outs_pci("PCI serial initialized\n");
                        return;
                    } else {
                        u32 mmio_addr = bar0 & 0xffffFFF0;
                        serial_outs("MMIO: 0x");
                        serial_outi64(mmio_addr, BASE_HEX);
                        serial_outs("\n");
                    }
                }
            }
        }
    }
}
