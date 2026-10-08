/* RP2350 Hazard3 (RISC-V) startup. The emulator boots the real mask bootrom
 * (OTP BOOT_ARCH = riscv), which only launches a flash image that carries a
 * picobin IMAGE_DEF block in its first 4 KB. With no ENTRY_POINT item the ROM
 * enters a RISC-V image at its first byte, so _start stays at 0x10000000 and
 * the block sits just after this code. */
.section .text.start
.option norelax
.global _start
_start:
    /* Trap anything unexpected into a visible parking loop. */
    la t0, _trap
    csrw mtvec, t0

    la sp, __stack_top

    /* Zero .bss */
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:
    /* Copy .data from flash to RAM */
    la t0, __data_load
    la t1, __data_start
    la t2, __data_end
3:  bgeu t1, t2, 4f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 3b
4:
    call main
_trap:
    j _trap

/* Minimal IMAGE_DEF (RP2350 datasheet 5.9.5, pico-sdk embedded_start_block):
 * EXE | security S | CPU RISC-V | chip RP2350, in a block that links to itself. */
.p2align 2
_image_def:
    .word 0xffffded3        /* PICOBIN_BLOCK_MARKER_START */
    .byte 0x42              /* PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE */
    .byte 0x01              /* item size in words */
    .hword 0x1121           /* EXE | EXE_SECURITY_S | EXE_CPU_RISCV | EXE_CHIP_RP2350 */
    .byte 0xff              /* PICOBIN_BLOCK_ITEM_2BS_LAST */
    .hword 0x0001           /* size of the items before it, in words */
    .byte 0x00
    .word 0x00000000        /* relative link to the next block: 0 = this one */
    .word 0xab123579        /* PICOBIN_BLOCK_MARKER_END */
