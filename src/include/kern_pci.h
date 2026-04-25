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

typedef volatile struct {
    u32 clb;       // Command list base address (low 32 bits)
    u32 clbu;      // Command list base address (high 32 bits)
    u32 fb;        // FIS base address (low 32 bits)
    u32 fbu;       // FIS base address (high 32 bits)
    u32 is;        // Interrupt status
    u32 ie;        // Interrupt enable
    u32 cmd;       // Command and status
    u32 reserved0; // Reserved
    u32 tfd;       // Task file data
    u32 sig;       // Signature
    u32 ssts;      // SATA status (SCR0: SStatus)
    u32 sctl;      // SATA control (SCR2: SControl)
    u32 serr;      // SATA error (SCR1: SError)
    u32 sact;      // SATA active (SCR3: SActive)
    u32 ci;        // Command issue
    u32 sntf;      // SATA notification (SCR4: SNotification)
    u32 fbs;       // FIS-based switching control
    u32 reserved1[11];
    u32 vendor[4]; // Vendor specific
} ahci_port_t;

typedef volatile struct {
    u32 cap;       // Host capabilities
    u32 ghc;       // Global host control
    u32 is;        // Interrupt status
    u32 pi;        // Ports implemented (bitmask)
    u32 vs;        // Version
    u32 ccc_ctl;   // Command completion coalescing control
    u32 ccc_pts;   // Command completion coalescing ports
    u32 em_loc;    // Enclosure management location
    u32 em_ctl;    // Enclosure management control
    u32 cap2;      // Host capabilities extended
    u32 bohc;      // BIOS/OS handoff control and status
    u8 reserved[0xA0 - 0x2C];
    u8 vendor[0x100 - 0xA0];
    ahci_port_t ports[32]; // The 32 possible SATA ports
} ahci_hba_t;

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
