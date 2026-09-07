/*
 * =============================================================================
 * STM32G474RETx_APP.ld - Robot Controller Board application linker script
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * PROPOSED memory partitioning - modeled directly on URTC's own proven
 * bootloader+app+backup-slot split (src/F303-master/STM32F303CCTx_APP.ld in
 * the sibling URTC repo), scaled up for this chip's larger 512 KB flash /
 * 128 KB RAM. Not yet verified against real hardware - see
 * docs/COMPILE_STM32G474.TXT and HYDRA-UMC/docs/architecture.md section 3.
 *
 * STM32G474RET6: LQFP-64, 512 KB flash (dual-bank capable, treated as one
 * flat 512 KB region here), 128 KB SRAM (80 KB SRAM1 + 16 KB SRAM2,
 * contiguous - treated as one flat 96 KB region below; 32 KB CCM SRAM at a
 * separate address, 0x10000000, deliberately left unused/reserved for a
 * future fast-path use, not mapped by this script).
 *
 * Flash layout (512 KB = 0x80000 total):
 *   0x08000000  Bootloader        32 KB  (0x8000)  - see boot/STM32G474RETx_BOOTLOADER.ld
 *   0x08008000  Metadata           4 KB  (0x1000)  - version/HWID/CRC record, mirrors URTC's own
 *   0x08009000  App main slot    236 KB (0x3B000)  - THIS script targets this slot
 *   0x08044000  App backup slot  236 KB (0x3B000)  - CAN-OTA staging area before verified copy-in
 *   0x0807F000  Reserved           4 KB  (0x1000)
 */

MEMORY
{
  FLASH_APP (rx) : ORIGIN = 0x08009000, LENGTH = 236K
  RAM (rwx)       : ORIGIN = 0x20000000, LENGTH = 96K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x400;
_Min_Stack_Size = 0x800;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH_APP

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH_APP

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH_APP

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH_APP
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH_APP
  .preinit_array : { PROVIDE_HIDDEN (__preinit_array_start = .); KEEP (*(.preinit_array*)); PROVIDE_HIDDEN (__preinit_array_end = .); } >FLASH_APP
  .init_array : { PROVIDE_HIDDEN (__init_array_start = .); KEEP (*(SORT(.init_array.*))); KEEP (*(.init_array*)); PROVIDE_HIDDEN (__init_array_end = .); } >FLASH_APP
  .fini_array : { PROVIDE_HIDDEN (__fini_array_start = .); KEEP (*(SORT(.fini_array.*))); KEEP (*(.fini_array*)); PROVIDE_HIDDEN (__fini_array_end = .); } >FLASH_APP

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH_APP

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
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
