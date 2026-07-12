#ifndef KERN_IDE_H
#define KERN_IDE_H

#include "dialect.h"

// Primary IDE Bus Ports
#define IDE_DATA        0x1F0
#define IDE_ERROR       0x1F1
#define IDE_FEATURES    0x1F1
#define IDE_SEC_COUNT   0x1F2
#define IDE_LBA_LOW     0x1F3
#define IDE_LBA_MID     0x1F4
#define IDE_LBA_HIGH    0x1F5
#define IDE_DRIVE_SEL   0x1F6
#define IDE_COMMAND     0x1F7
#define IDE_STATUS      0x1F7

// Status Register Bits
#define IDE_STATUS_BSY  0x80  // Busy
#define IDE_STATUS_DRDY 0x40  // Drive Ready
#define IDE_STATUS_DRQ  0x08  // Data Request
#define IDE_STATUS_ERR  0x01  // Error

u0 ide_init();
bool ide_detect();
u0 ide_read_sectors(u32 lba, u8 count, u8 *buffer);
u0 ide_write_sectors(u32 lba, u8 count, u8 *buffer);

#endif //KERN_IDE_H
