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

qemu-system-x86_64.exe -cdrom sandfleaOS.iso -m 2G -bios ovmf/DEBUGX64_OVMF.fd -serial stdio -s