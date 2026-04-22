@echo off

@REM Build the project
call wb.bat

echo Running with qemu (Optimized with IDE Drive)
echo --- Running ---

break > serial_output.log

@REM Performance Tweaks:
@REM 1. -accel whpx: Use Windows Hypervisor Platform (HW Acceleration)
@REM 2. -display sdl: Faster than the default GTK on Windows
@REM 3. -vga std: Reliable display driver
@REM 4. -drive ...: Attaching our disk image as a physical IDE drive

qemu-system-x86_64.exe -cdrom sandfleaOS.iso ^
    -m 2G ^
    -bios ovmf/DEBUGX64_OVMF.fd ^
    -accel whpx ^
    -display sdl ^
    -vga std ^
    -drive file=disk.img,format=raw,index=0,media=disk ^
    -serial stdio ^
    -device pci-serial,chardev=myserial ^
    -chardev file,id=myserial,path=pci_serial_output.log
