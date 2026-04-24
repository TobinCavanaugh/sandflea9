cmake_minimum_required(VERSION 3.28)
project(sandflea9 C)

set(CMAKE_C_STANDARD 11)

add_executable(sandflea9 src/kernel/main.c
        src/kernel/kern_interrupts.c
        src/include/kern_interrupts.h
        src/include/kern_asmstubs.h
        src/kernel/kern_serial.c
        src/include/kern_serial.h
        src/kernel/kern_vmm.c
        src/include/kern_vmm.h
        src/kernel/kern_asmstubs.c
        src/kernel/kern_screen.c
        src/include/kern_screen.h
        src/kernel/kern_keyboard.c
        src/include/kern_keyboard.h
        src/util/util_str.c
        src/util/util_str.h
        src/include/stbsupport.h
        src/kernel/stbsupport.c
        src/include/limine_requests.h
        src/kernel/kern_pci.c
        src/include/kern_pci.h
        src/kernel/kern_ext2.c
        src/include/kern_ext2.h
        src/kernel/kern_fs.c
        src/include/kern_fs.h
        src/kernel/kern_sched.c
        src/include/kern_sched.h
        src/util/util_cmd.c
        src/include/util_cmd.h
        src/kernel/kern_terminal.c
        src/include/kern_terminal.h
        src/kernel/kern_tests.c
        src/include/kern_tests.h
        src/kernel/ssfn.c
        src/kernel/kern_ide.c
        src/include/kern_ide.h
)
