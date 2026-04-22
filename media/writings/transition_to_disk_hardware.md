# Transitioning from Initial RAM Disk to Real Hardware Access

Currently, sandfleaOS loads its filesystem as a **Limine Module**. In the hardware world, this is equivalent to an **initrd** (Initial RAM Disk). The bootloader reads the file from the boot media, copies it into RAM, and tells the kernel where it is.

To make this "legit," the kernel needs to talk to a **Storage Controller** (like IDE, AHCI, or NVMe) and read sectors directly from the disk.

## 1. The Strategy: The "Legit" Storage Stack

To move away from baked-in modules, you need to implement three layers:

1.  **PCI Discovery**: You already have this! You can find the storage controller on the PCI bus.
2.  **Hardware Driver**: A driver for the controller (IDE/PATA is the easiest starting point for a hobby OS).
3.  **Disk Abstraction**: A function like `read_sectors(LBA, count, buffer)` that the EXT2 driver calls instead of using `fs_base`.

## 2. QEMU Setup (The Hardware)

Instead of relying on Limine to load the module, we tell QEMU to attach a physical disk to a controller.

### Update `wr.bat`
Modify the QEMU command to include a drive on the IDE bus:
```bash
qemu-system-x86_64.exe ... ^
    -drive file=disk.img,format=raw,index=0,media=disk
```

### Update `build.sh`
You no longer need to copy `disk.img` into `iso_root/`. The ISO will now only contain the kernel; the filesystem lives on its own "physical" device.

## 3. Kernel implementation: The IDE Driver

The "easiest" legit path is **IDE (PATA)**. It uses simple IO ports (0x1F0-0x1F7).

### Step A: Identify the Device
Scan PCI for Class `0x01` (Mass Storage) and Subclass `0x01` (IDE).

### Step B: The `read_sectors` Logic
Instead of `memcpy` from `fs_base`, you talk to the hardware:
1.  **Select Drive**: Outport `0x1F6` with `0xE0` (Master).
2.  **Set LBA**: Outport the sector number to `0x1F3`, `0x1F4`, and `0x1F5`.
3.  **Set Count**: Outport the number of sectors to `0x1F2`.
4.  **Command**: Outport `0x20` (Read with retry) to `0x1F7`.
5.  **Wait**: Poll `0x1F7` until the BSY bit is clear and DRQ bit is set.
6.  **Read Data**: Inport 256 times (for 512 bytes) from `0x1F0`.

## 4. Connecting EXT2 to the Driver

Currently, your EXT2 code uses `get_block_ptr(block_id)` which returns `fs_base + offset`.

### The "Legit" Version:
```c
u0 ext2_read_block(u32 block_id, u8* buffer) {
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = block_id * sectors_per_block;
    
    // Call your new hardware driver
    ide_read_sectors(start_sector, sectors_per_block, buffer);
}
```

## 5. Why this is "Legit"
- **Memory Efficiency**: You only load the blocks you need into RAM, rather than keeping the entire 32MB disk image in memory at all times.
- **Persistence**: If you implement `write_sectors`, changes you make in the OS will actually stay on the `disk.img` after you close QEMU.
- **Hardware Agnostic**: Once the EXT2 driver uses `read_block`, you can swap the IDE driver for an AHCI (SATA) or NVMe driver later without changing any filesystem code.
