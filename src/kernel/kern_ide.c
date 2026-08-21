#include "../include/kern_ide.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_profile.h"

u0 ide_init() {
    serial_outsl("IDE: Initializing Primary Bus...");
    if (ide_detect(IDE_DRIVE_MASTER)) {
        serial_outsl("IDE: Primary Master drive detected!");
    } else {
        serial_outsl("IDE: No Primary Master drive found.");
    }
    if (ide_detect(IDE_DRIVE_SLAVE)) {
        serial_outsl("IDE: Primary Slave drive detected!");
    } else {
        serial_outsl("IDE: No Primary Slave drive found.");
    }
}

bool ide_detect(u8 drive_sel) {
    // 1. Select the drive on the primary bus
    outb(IDE_DRIVE_SEL, drive_sel);

    // Short I/O delay
    for (volatile int d = 0; d < 1000; d++) asm volatile("pause");

    // 2. Clear sector count and LBA registers (send 0)
    outb(IDE_SEC_COUNT, 0);
    outb(IDE_LBA_LOW, 0);
    outb(IDE_LBA_MID, 0);
    outb(IDE_LBA_HIGH, 0);

    // 3. Send IDENTIFY command
    outb(IDE_COMMAND, 0xEC);

    // 4. Check status — if 0 or 0xFF (floating bus), no IDE drive exists
    u8 status = inb(IDE_STATUS);
    if (status == 0 || status == 0xFF) {
        return false; // Drive does not exist / floating bus
    }

    // 5. Wait for BSY to clear (with timeout)
    u32 timeout = 100000;
    while ((inb(IDE_STATUS) & IDE_STATUS_BSY) && --timeout) {
        asm volatile("pause");
    }
    if (timeout == 0) return false;

    // 6. Check if LBA mid/high are non-zero (implies ATAPI, not ATA)
    if (inb(IDE_LBA_MID) != 0 || inb(IDE_LBA_HIGH) != 0) {
        return false; // Not a standard ATA drive
    }

    // 7. Wait for DRQ or ERR (with timeout)
    timeout = 100000;
    while (timeout--) {
        status = inb(IDE_STATUS);
        if (status == 0 || status == 0xFF || (status & IDE_STATUS_ERR)) return false;
        if (status & IDE_STATUS_DRQ) break;
        asm volatile("pause");
    }
    if (timeout == 0) return false;

    // 8. Read 256 words (512 bytes) of identification data to clear the buffer
    for (int i = 0; i < 256; i++) {
        inw(IDE_DATA);
    }

    return true;
}

u0 ide_read_sectors(u8 drive_sel, u32 lba, u8 count, u8 *buffer) {
    PROFILE_SCOPE("ide:read_sectors");
    u64 irq = save_irq_and_disable();

    // 1. Select Drive and send upper 4 bits of LBA
    outb(IDE_DRIVE_SEL, drive_sel | ((lba >> 24) & 0x0F));

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
        // Wait for BSY to clear and DRQ to be set (with timeout)
        u32 timeout = 100000;
        while (timeout--) {
            u8 status = inb(IDE_STATUS);
            if (status == 0xFF) break;
            if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) break;
            asm volatile("pause");
        }
        if (timeout == 0) {
            mem_set(buffer, 0, count * 512);
            restore_irq(irq);
            return;
        }

        // Read 256 words (512 bytes) using rep insw
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

u0 ide_write_sectors(u8 drive_sel, u32 lba, u8 count, u8 *buffer) {
    u64 irq = save_irq_and_disable();

    // 1. Select Drive and send upper 4 bits of LBA
    outb(IDE_DRIVE_SEL, drive_sel | ((lba >> 24) & 0x0F));

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
        // Wait for BSY to clear and DRQ to be set (with timeout)
        u32 timeout = 100000;
        while (timeout--) {
            u8 status = inb(IDE_STATUS);
            if (status == 0xFF) break;
            if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) break;
            asm volatile("pause");
        }
        if (timeout == 0) {
            restore_irq(irq);
            return;
        }

        // Write 256 words (512 bytes) using rep outsw
        u32 words = 256;
        asm volatile (
            "rep outsw"
            : "+S"(ptr), "+c"(words)
            : "d"(IDE_DATA)
            : "memory"
        );
    }

    // Wait for BSY to clear (with timeout)
    u32 timeout = 100000;
    while ((inb(IDE_STATUS) & IDE_STATUS_BSY) && --timeout) asm volatile("pause");

    restore_irq(irq);
}

