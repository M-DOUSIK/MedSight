/*
******************************************************************************
**
** @file: STM32N6XX_AXISRAM2_fsbl.ld
**
** @author: GPM Application Team
**
** @brief: Linker script for STM32N6XX Device from STM32N6 series
**                      512 KBytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used
**
**  Target: STMicroelectronics STM32
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
******************************************************************************
** @attention
**
** Copyright (c) 2024 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
******************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of "RAM" Ram type memory */
_sstack = _estack - _Min_Stack_Size;

_Min_Heap_Size = 0x200; /* required amount of heap */
_Min_Stack_Size = 0x800; /* required amount of stack */

/* ============================================================================
 * Memories definition  -  re-derived (see the technical reference)
 *
 * The 511K/1023K split below was inherited unexamined from the ST
 * DCMIPP_ContinuousMode example this repository was founded on.
 * It claimed ~1.5 MB of the part's ~3.75 MB of AXI SRAM and left a 501 KB
 * hole between the two regions that nothing has ever used.
 *
 * The part's AXI SRAM, from the reference manual and confirmed against the
 * Cube.AI generated memory-pool declarations in Src/ai/fd.c and
 * Src/ai/faceid.c:
 *
 *   AXISRAM1  0x34000000.. 0x340FFFFF   1024K   CPU only  (NPU cannot reach)
 *   AXISRAM2  0x34100000.. 0x341FFFFF   1024K   CPU only  (NPU cannot reach)
 *   AXISRAM3  0x34200000.. 0x3426FFFF    448K   npuRAM3
 *   AXISRAM4  0x34270000.. 0x342DFFFF    448K   npuRAM4
 *   AXISRAM5  0x342E0000.. 0x3434FFFF    448K   npuRAM5
 *   AXISRAM6  0x34350000.. 0x343BFFF7    448K   npuRAM6  (8 bytes short)
 *
 * RAM  keeps AXISRAM1 minus the first 1 KB, which the STM32N6 boot ROM owns.
 *      AXISRAM1 has nothing more to give and this region is only 77% full.
 * ROM  now takes ALL of AXISRAM2 instead of its top half. That is the whole
 *      of the change: the origin moves down by 0x80400 and the length grows
 *      from 511K to 1024K, ending exactly where the framebuffer begins.
 *      Debug was at 90.6% of the old 511K with 48 KB free - enough to be one
 *      feature away from failing to link, which is why it was grown.
 * AI_ARENA is the contiguous SRAM above everything the two NPU networks
 *      actually address. That footprint was established by extracting
 *      every address literal from the generated sources: it is one solid
 *      block, 0x34200000.. 0x34387FFF. Everything above that, up to the top
 *      of AXISRAM6 less a 4 KB guard band, is free and NPU-reachable.
 *      NOT part of RAM: the linker must never place ordinary.bss here, both
 *      because a future model wants it whole and because AXISRAM5/6 are not
 *      powered until aiPreInitialize() runs. See the technical reference.
 * ============================================================================ */
/* ============================================================================
 * ROM AND RAM ARE SWAPPED, so the device can boot without a computer.
 *
 * Sizes and section assignments are unchanged; only the two ORIGINs trade
 * places. ROM (code +.rodata, the part that is actually loaded from flash)
 * moves DOWN to 0x34000400; RAM (.data +.bss, which no loader ever copies)
 * moves UP to 0x34100000.
 *
 * WHY. The STM32N6 boot ROM copies at most 512 KB from external flash, so a
 * 715 KB application cannot be a single FSBL. The supported route is ST's
 * two-stage loader (tools/x-cube-n6-ai-power-measurement/FSBL/ai_fsbl.hex):
 * the ROM loads that 62 KB first stage, and it copies the real application
 * out of flash and jumps to it.
 *
 * That loader reserves 0x34000400.. 0x340FFFFF - SRAM1, 1 MB - for the
 * application, and itself lives up around 0x34180000. With ROM at
 * 0x34100000 our loadable image spanned 0x34100000.. 0x341DB400 and so
 * covered the loader's own code: it overwrote the loader partway through the
 * copy and died before UART came up, which is exactly the silent failure
 * observed on 2026-09-12. Swapping the origins puts the image where the
 * loader expects it and leaves the loader untouched while it works.
 *
 *.bss now sits at 0x34100000 and does overlap where the loader was, which is
 * harmless: startup zeroes.bss AFTER the jump, by which point the loader has
 * done its job and is not coming back.
 *
 * Capacity, measured rather than assumed:
 *   loadable image   873 KB Debug / 688 KB Release   vs 1023 KB   OK
 *.data +.bss     ~672 KB                         vs 1024 KB   OK
 *
 * Nothing else moves. The NPU activation block at 0x34200000, AI_ARENA at
 * 0x34388000 and the framebuffer are absolute addresses and are unaffected.
 * _estack now tops out at 0x34200000, immediately below the activation block.
 * ============================================================================ */
MEMORY
{
  ROM      (xrw)   : ORIGIN = 0x34000400,   LENGTH = 1023K
  RAM      (xrw)   : ORIGIN = 0x34100000,   LENGTH = 1024K
  AI_ARENA (xrw)   : ORIGIN = 0x34388000,   LENGTH = 220K
  OSPI_NOR (rx)    : ORIGIN = 0x71000000,   LENGTH = 64M
}

/* Sections */
SECTIONS
{
  /* The startup code into "RAM" Ram type memory */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >ROM

  /* The program code and other data into "RAM" Ram type memory */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /*.text sections (code) */
    *(.text*)          /*.text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)
    *(.RamFunc)        /*.RamFunc sections */
    *(.RamFunc*)       /*.RamFunc* sections */

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >ROM

  /* Constant data - moved from RAM to ROM.
   *
   * the technical reference considered this move and DECLINED it, with
   * a good reason: "the NPU cannot reach either bank, so moving it buys
   * nothing that matters", and the one lesson learned about.rodata
   * placement is a reason to leave a working arrangement working.
   *
   * The premise changed. A third network's generated tables arrived
   * with their own code, and the RAM region - which holds.rodata,.bss,
   * the heap and the stack - went to 95.9% full, 42 KB free. The earlier
   * words about Debug ROM at 90.6% apply verbatim: that is roughly one
   * feature away from a link failure, and a link failure at 3 a.m. before a
   * deadline is not a good way to discover a linker script nobody re-derived.
   *
   * The move is safe, and specifically it does not re-run the earlier bus
   * fault. That fault was the NPU being unable to READ weights placed in
   * AXISRAM1. The NPU cannot reach AXISRAM1 *or* AXISRAM2, so moving.rodata
   * between them changes nothing the NPU sees - and every model weight in
   * this project lives in external OSPI NOR regardless. What moves here is
   * ordinary CPU-read constant data: sprite tables, fonts, format strings.
   *
   * Measured effect, Debug: RAM 95.9% -> 63.6%, ROM 50.7% -> 75.5%. */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /*.rodata sections (constants, strings, etc.) */
    *(.rodata*)        /*.rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >ROM

  .ARM.extab   (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
   {
     . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >ROM

  .ARM (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
   {
     . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >ROM

  .preinit_array     (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >ROM

  .init_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >ROM

  .fini_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >ROM

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /*.data sections */
    *(.data*)          /*.data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */

  } >RAM AT> ROM

  .noncacheable :
  {
    . = ALIGN(8);
    __snoncacheable = .;/* create symbol for start of section */
    KEEP(*(noncacheable_buffer))
    . = ALIGN(8);
    __enoncacheable = .;  /* create symbol for end of section */
  } > RAM

  .gnu.sgstubs :
  {
    . = ALIGN(4);
    *(.gnu.sgstubs*)   /* Secure Gateway stubs */
    . = ALIGN(4);
  } >ROM

  /* Uninitialized data section into "RAM" Ram type memory */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the.bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* User_heap_stack section, used to check that there is enough "RAM" Ram  type memory left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* ── AI_ARENA ───────────────────────────────────────────────
   * Reserved, addressable, NPU-reachable scratch for a future third model.
   * NOLOAD: nothing is programmed here, it is claimed rather than filled.
   * The two symbols are the contract future work codes against; the
   * arena self-test in ms_memtest.c is the proof that they are real.       */
  .ai_arena (NOLOAD) :
  {
    . = ALIGN(32);
    _ai_arena_start = .;
    KEEP(*(.ai_arena))
    KEEP(*(.ai_arena*))
    . = ALIGN(32);
    _ai_arena_used_end = .;
  } >AI_ARENA
  _ai_arena_end = ORIGIN(AI_ARENA) + LENGTH(AI_ARENA);

  OSPI_NOR (NOLOAD) :
  {
    . = ALIGN(4);
    KEEP(*(.xspi2*))
    . = ALIGN(4);
  } >OSPI_NOR

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
