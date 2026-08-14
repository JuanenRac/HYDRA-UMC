/*
 * =============================================================================
 * STM32H745ZITx_CM4_APP.ld - Kinematic Brain, Cortex-M4 application linker script
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * See ../CM7/STM32H745ZITx_CM7_APP.ld's own header comment for the shared
 * partitioning philosophy - this is the SAME split applied to Flash Bank 2
 * (the CM4 core's own 1 MB bank) instead of Bank 1.
 *
 * CM4 RAM: D2-domain SRAM1+SRAM2+SRAM3 (128+128+32 = 288 KB, contiguous
 * from 0x30000000) - separate from CM7's own D1 AXI SRAM, by design (each
 * core gets its own RAM so neither can starve the other; see
 * docs/architecture.md's own note on this chip's real-time motion (CM7) vs
 * FDCAN/IPC (CM4) split). SRAM4 (D3 domain, 64 KB @ 0x38000000) is
 * deliberately left unmapped here, reserved for a future CM7<->CM4
 * shared-memory region (IPC mailbox) once that's actually designed - not
 * needed for this skeleton's own GPIO-toggle smoke test.
 *
 * Flash Bank 2 layout (1 MB = 0x100000 total):
 *   0x08100000  Bootloader        64 KB (0x10000) - see boot/STM32H745ZITx_CM4_BOOTLOADER.ld
 *   0x08110000  Metadata           4 KB (0x1000)
 *   0x08111000  App main slot    476 KB (0x77000) - THIS script targets this slot
 *   0x08188000  App backup slot  476 KB (0x77000)
 *   0x081FF000  Reserved           4 KB (0x1000)
 */

MEMORY
{
  FLASH_APP (rx) : ORIGIN = 0x08111000, LENGTH = 476K
  RAM (rwx)       : ORIGIN = 0x30000000, LENGTH = 288K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x1000;

SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH_APP

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*)
    *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP (*(.init)) KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH_APP

  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH_APP
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH_APP
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH_APP
  .preinit_array : { PROVIDE_HIDDEN (__preinit_array_start = .); KEEP (*(.preinit_array*)); PROVIDE_HIDDEN (__preinit_array_end = .); } >FLASH_APP
  .init_array : { PROVIDE_HIDDEN (__init_array_start = .); KEEP (*(SORT(.init_array.*))); KEEP (*(.init_array*)); PROVIDE_HIDDEN (__init_array_end = .); } >FLASH_APP
  .fini_array : { PROVIDE_HIDDEN (__fini_array_start = .); KEEP (*(SORT(.fini_array.*))); KEEP (*(.fini_array*)); PROVIDE_HIDDEN (__fini_array_end = .); } >FLASH_APP

  _sidata = LOADADDR(.data);
  .data : { . = ALIGN(4); _sdata = .; *(.data) *(.data*) . = ALIGN(4); _edata = .; } >RAM AT> FLASH_APP

  . = ALIGN(4);
  .bss :
  {
    _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .; __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ : { libc.a ( * ) libm.a ( * ) libgcc.a ( * ) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
