OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0xffffffff80000000;

    kernel_start_marker = .;

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        KEEP(*(.data))   /* This is good, keep it */
    }

    . = ALIGN(4096);
    .limine_requests_start : {
        KEEP(*(.limine_requests_start))
    }

    .limine_requests : {
        KEEP(*(.limine_requests))
    }

    .limine_requests_end : {
        KEEP(*(.limine_requests_end))
    }

    . = ALIGN(4096);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    kernel_end_marker = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note*)
    }
}
