@echo off

@REM call SSFN_tools/fontbuild.bat
call wb.bat

echo Running with qemu
echo --- Running ---
@REM qemu-system-i386.exe -cdrom sandfleaOS.iso -m 2G -monitor stdio -accel whpx
@REM qemu-system-i386.exe -cdrom sandfleaOS.iso -m 2G -qmp tcp:localhost:4444,server,nowait -serial tcp:localhost:5555,server,nowait -d guest_errors,unimp
@REM -accel whpx
@REM -drive if=pflash,format=raw,unit=0,readonly,file=OVMF-pure-efi.fd
@REM -drive if=pflash,format=raw,unit=1,file=OVMF_VARS-pure-efi.fd

@REM qemu-system-x86_64.exe -cdrom sandfleaOS.iso -m 2G -bios ovmf/DEBUGX64_OVMF.fd -serial stdio -s \
@REM -device pci-serial,chardev=myserial -chardev stdio,id=myserial


@REM Newline ^^

@REM qemu-system-x86_64.exe -cdrom sandfleaOS.iso -m 2G -bios ovmf/DEBUGX64_OVMF.fd -serial stdio -s \
@REM     -device pci-serial,chardev=myserial \
@REM     -chardev socket,id=myserial,host=localhost,port=4555,server=on,wait=off

@REM qemu-system-x86_64.exe -cdrom sandfleaOS.iso -m 2G -bios ovmf/DEBUGX64_OVMF.fd -serial stdio -s \
@REM -device pci-serial,chardev=myserial \
@REM -chardev pipe,id=myserial,path=\\.\pipe\sandfleadebug

break > serial_output.log

qemu-system-x86_64.exe -cdrom sandfleaOS.iso ^
    -m 2G ^
    -bios ovmf/DEBUGX64_OVMF.fd ^
    -serial stdio -s ^
    -device pci-serial,chardev=myserial ^
    -chardev file,id=myserial,path=serial_output.log
