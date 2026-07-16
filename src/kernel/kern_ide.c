#include "../include/kern_ide.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_profile.h"

u0 ide_init() {
    serial_outsl("IDE: Initializing Primary Bus...");
    if (ide_detect()) {
        serial_outsl("IDE: Primary Master drive detected!");
    } else {
        serial_outsl("IDE: No Primary Master drive found.");
    }
}

bool ide_detect() {
    // 1. Select the master drive on the primary bus
    // 0xA0 = Master, 0xB0 = Slave
    outb(IDE_DRIVE_SEL, 0xA0);

    // 2. Clear sector count and LBA registers (send 0)
    outb(IDE_SEC_COUNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);

    // 3. Send IDENTIFY command
    outb(IDE_COMMAND, 0xEC);

    // 4. Check status
    u8 status = inb(IDE_STATUS);
    if (status == 0) {
        return false; // Drive does not exist
    }

    // 5. Wait for BSY to clear
    while (inb(IDE_STATUS) & IDE_STATUS_BSY);

    // 6. Check if LBA mid/high are non-zero (implies ATAPI, not ATA)
    if (inb(IDE_LBA_MID) != 0 || inb(IDE_LBA_HIGH) != 0) {
        return false; // Not a standard ATA drive
    }

    // 7. Wait for DRQ or ERR
    while (true) {
        status = inb(IDE_STATUS);
        if (status & IDE_STATUS_ERR) return false;
        if (status & IDE_STATUS_DRQ) break;
    }

    // 8. Read 256 words (512 bytes) of identification data to clear the buffer
    for (int i = 0; i < 256; i++) {
        inw(IDE_DATA);
    }

    return true;
}

u0 ide_read_sectors(u32 lba, u8 count, u8 *buffer) {
    PROFILE_SCOPE("ide:read_sectors");
    u64 irq = save_irq_and_disable();

    // 1. Select Drive and send upper 4 bits of LBA
    // 0xE0 = LBA mode + Master
    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));

    // 2. Send sector count
    outb(IDE_SEC_COUNT, count);

    // 3. Send remaining LBA bits
    outb(IDE_LBA_LOW, (u8) lba);
    outb(IDE_LBA_MID, (u8) (lba >> 8));
    outb(IDE_LBA_HIGH, (u8) (lba >> 16));

    // 4. Send READ command (0x20)
    outb(IDE_COMMAND, 0x20);

    u16 *ptr = (u16 *) buffer;

    for (int i = 0; i < count; i++) {
        // Wait for BSY to clear and DRQ to be set
        while (true) {
            u8 status = inb(IDE_STATUS);
            if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) break;
        }

        // Read 256 words (512 bytes) using rep insw — single instruction
        // replaces 256 individual inw calls, reducing VM exits under WHPX
        u32 words = 256;
        asm volatile (
            "rep insw"
            : "+D"(ptr), "+c"(words)
            : "d"(IDE_DATA)
            : "memory"
        );
    }

    restore_irq(irq);
}

u0 ide_write_sectors(u32 lba, u8 count, u8 *buffer) {
    u64 irq = save_irq_and_disable();

    // 1. Select Drive and send upper 4 bits of LBA
    // 0xE0 = LBA mode + Master
    outb(IDE_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));

    // 2. Send sector count
    outb(IDE_SEC_COUNT, count);

    // 3. Send remaining LBA bits
    outb(IDE_LBA_LOW, (u8) lba);
    outb(IDE_LBA_MID, (u8) (lba >> 8));
    outb(IDE_LBA_HIGH, (u8) (lba >> 16));

    // 4. Send WRITE command (0x30)
    outb(IDE_COMMAND, 0x30);

    u16 *ptr = (u16 *) buffer;

    for (int i = 0; i < count; i++) {
        // Wait for BSY to clear and DRQ to be set
        while (true) {
            u8 status = inb(IDE_STATUS);
            if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) break;
        }

        // Write 256 words (512 bytes) using rep outsw — single instruction
        u32 words = 256;
        asm volatile (
            "rep outsw"
            : "+S"(ptr), "+c"(words)
            : "d"(IDE_DATA)
            : "memory"
        );
    }

    // Wait for BSY to clear
    while (inb(IDE_STATUS) & IDE_STATUS_BSY);

    restore_irq(irq);
}

