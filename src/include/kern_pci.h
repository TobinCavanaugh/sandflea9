//
// Created by tobin on 2025-11-30.
//

#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include "dialect.h"
#include "kern_asmstubs.h"
#include "kern_serial.h"


//@formatter:off
#define PCI_CLASS_OLD        0x00
#define PCI_CLASS_STORAGE    0x01
#define PCI_CLASS_NETWORK    0x02
#define PCI_CLASS_DISPLAY    0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MEMORY     0x05
#define PCI_CLASS_BRIDGE     0x06
#define PCI_CLASS_COMM       0x07
#define PCI_CLASS_BASE       0x08
#define PCI_CLASS_INPUT      0x09
//@formatter:on

#define PCI_SUBCLASS_UART 0x00

typedef struct pci_device {
    u8 bus, slot, func;
    u16 vendor_id, device_id;
    u8 class_code, subclass, prog_if;

    u32 bars[6];
    u8 irq_line;

    struct pci_device *next;
} pci_device_t;

//@formatter:off
pci_device_t * pci_init_system();
pci_device_t *pci_get_device(u8 class_code, u8 subclass);
u0 pci_enable_device_io(pci_device_t *dev);
u0 pci_init_uart_port(u32 port);

u0 pci_serial_putc(const pci_device_t *dev, u8 c);
u0 pci_serial_puts(const pci_device_t *dev, const char * str);
u0 pci_serial_putsl(const pci_device_t *dev, const char * str);
//@formatter:on


#endif //KERNEL_PCI_H
