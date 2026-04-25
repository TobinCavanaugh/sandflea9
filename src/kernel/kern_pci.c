//
// Created by tobin on 2025-11-30.
//

#include "../include/kern_pci.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_vmm.h"

u32 pci_serial_base = 0;

pci_device_t *pci_list_head = null;

u0 pci_register_device(u8 bus, u8 slot, u8 func, u16 vendor, u16 device) {
    pci_device_t *dev = (pci_device_t *) kmalloc(sizeof(pci_device_t)); // TODO This should be on 1024 byte aligned

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;;
    dev->vendor_id = vendor;
    dev->device_id = device;

    u32 class_reg = pci_read_32(bus, slot, func, 0x08);
    dev->class_code = (class_reg >> 24) & 0xFF;
    dev->subclass = (class_reg >> 16) & 0xFF;
    dev->prog_if = (class_reg >> 8) & 0xFF;

    for (i32 i = 0; i < 6; i++) {
        dev->bars[i] = pci_read_32(bus, slot, func, 0x10 + (i * 4));
    }

    dev->next = pci_list_head;
    pci_list_head = dev;
}

pci_device_t *pci_init_system() {
    serial_outsl("Enumerating pci...");

    for (u16 bus = 0; bus < 256; bus++) {

        // TODO Check ports implemented instead of brute forcing them
        for (u8 slot = 0; slot < 32; slot++) {

            u16 vendor_id = pci_read_16(bus, slot, 0, 0x00);
            if (vendor_id == 0xFFff) continue;

            u8 header_type = pci_read_32(bus, slot, 0, 0x0C) >> 16;
            u8 func_count = (header_type & 0x80) ? 8 : 1; // hmmm

            for (u8 func = 0; func < func_count; func++) {
                u16 f_vendor = pci_read_16(bus, slot, func, 0x00);
                if (f_vendor == 0xFFff) continue;

                u16 device = pci_read_16(bus, slot, func, 0x02);
                pci_register_device(bus, slot, func, vendor_id, device);
            }
        }
    }

    return pci_list_head;
}


pci_device_t *pci_get_device(u8 class_code, u8 subclass) {
    pci_device_t *node = pci_list_head;

    // Iterate the LL until we find our device
    while (node) {
        if (node->class_code == class_code && node->subclass == subclass) {
            return node;
        }
        node = node->next;
    }
    return null;
}

u0 pci_enable_device_io(pci_device_t *dev) {
    u32 command_reg = pci_read_32(dev->bus, dev->slot, dev->func, 0x04);
    command_reg |= 0x5; // Set bits 0 and 2 (hmmmm)

    u32 addr = (u32) ((dev->bus << 16) | (dev->slot << 11) | (dev->func << 8) | 0x04 | 0x80000000);
    outl(0xCF8, addr);
    outl(0xCFC, command_reg);
}

u0 pci_serial_putc(const pci_device_t *dev, u8 c) {
    u32 port = dev->bars[0] & 0xFFFFFFFC;

    while ((inb(port + 5) & 0x20) == 0) {
        asm volatile("nop");
    }

    outb(port, c);
}


u0 pci_serial_puts(const pci_device_t *dev, const char *str) {
    i32 i = 0;
    while (str[i] != '\0') {
        pci_serial_putc(dev, str[i++]);
    }
}

u0 pci_serial_putsl(const pci_device_t *dev, const char *str) {
    pci_serial_puts(dev, str);
    pci_serial_puts(dev, "\n");
}

u0 pci_init_uart_port(u32 port) {
    // outb(port + 1, 0x00); // Disable all interrupts
    // outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    // outb(port + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    // outb(port + 1, 0x00); //                  (hi byte)
    // outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    // outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    // outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set

    outb(port + 1, 0x00); // Disable IER (Interrupt Enable Register)
    outb(port + 2, 0x00); // Disable FIFO for testing (keeps it simple)
    outb(port + 3, 0x80); // Enable DLAB
    outb(port + 0, 0x08); // Set divisor to 1 (115200 baud approx)
    // outb(port + 0, 0x01); // Set divisor to 1 (115200 baud approx)
    outb(port + 1, 0x00); // (High byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit

    outb(port + 4, 0x03); // RTS/DSR set, but OUT2 cleared (Interrupts disconnected)
}
