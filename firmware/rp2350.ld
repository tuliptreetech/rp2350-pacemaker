/* RP2350 Hazard3 phase-1 firmware layout: code/rodata execute-in-place from
 * flash at 0x10000000; data/bss in main SRAM; stack at the top of the striped
 * SRAM region. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .data : ALIGN(4) {
        __data_start = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
