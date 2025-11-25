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
        src/kernel/kern_asmstubs.c)
