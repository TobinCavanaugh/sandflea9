@echo off

@REM Build the project
call wb.bat

echo Running with qemu (Optimized with IDE Drive)
echo --- Running ---

break > serial_output.log

qemu-system-x86_64.exe -cdrom sandfleaOS.iso ^
    -m 2G ^
    -bios ovmf/DEBUGX64_OVMF.fd ^
    -display sdl ^
    -vga std ^
    -drive file=disk.img,format=raw,index=0,media=disk ^
    -serial stdio ^
    -accel whpx ^
    -device pci-serial,chardev=myserial ^
    -chardev file,id=myserial,path=pci_serial_output.log
