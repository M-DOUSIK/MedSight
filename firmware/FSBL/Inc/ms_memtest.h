/* ms_memtest.h - proving the AI arena is real memory
 *
 * The linker script now reserves a named region, AI_ARENA, covering the
 * contiguous SRAM above everything the two NPU networks actually address
 * (the technical reference has the derivation). A region in a linker script
 * is a claim, not a fact: nothing in the build fails if the addresses are
 * wrong, unpowered, or aliased onto something else, because the section is
 * NOLOAD and nothing has ever written to it.
 *
 * This module writes to it and reads it back, once, from a cold boot, and
 * says over UART how many bytes passed. That is the difference between
 * "2.4 MB is unclaimed" - which is worth very little - and "220 KB is
 * claimed, addressable and proven", which future work can build on.
 *
 * ORDERING MATTERS, and it is the trap in this whole exercise.
 * AXISRAM5 and AXISRAM6 are NOT powered at reset. FSBL's
 * stm32n6xx_hal_msp.c only enables AXISRAM3 and AXISRAM4 (clock +
 * HAL_RAMCFG_EnableAXISRAM); the other two are brought up by
 * SystemInit_POST() inside Src/ai/npu_init.c, which runs from
 * aiPreInitialize() on the AI task. The arena lives at 0x34388000, in
 * AXISRAM6. So this self-test MUST run after ai_vision_wait_init() has
 * returned true, and state_machine_init() is where it is called from.
 * Running it earlier does not fault - it reads back zeros or garbage from
 * an unclocked bank, which is exactly the kind of result that would get
 * written into a document as fact.
 */
#ifndef MS_MEMTEST_H
#define MS_MEMTEST_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gated so it can be compiled out entirely once the arena has been proven on
 * a given board. Left ON by default, because the whole point
 * is that the hardware run reports the byte count. */
#ifndef MEDSIGHT_ARENA_SELFTEST
#define MEDSIGHT_ARENA_SELFTEST 1
#endif

/**
 * @brief  Pattern-test the whole of AI_ARENA and report over UART.
 *
 * Three passes over the region, each write-then-verify, so a stuck bit, an
 * address aliased back onto a lower bank, and a bank that is simply not
 * clocked all produce different failures rather than the same zero:
 *   1. 0xA5A5A5A5   - every bit toggled one way
 *   2. 0x5A5A5A5A   - and the other
 *   3. address-in-address - the only pattern that catches aliasing, where a
 *      write to X lands somewhere else and both reads still "pass"
 *
 * Prints one PASS line with the byte count, or a FAIL line naming the first
 * bad word, its address, what was written and what came back. Both are
 * unconditional printf, not MS_DBG_PRINTF: this line IS the deliverable.
 *
 * @return true if every word of the arena held every pattern.
 */
bool ms_memtest_arena(void);

/** Start address of AI_ARENA (linker symbol `_ai_arena_start`). */
uintptr_t ms_memtest_arena_start(void);
/** One past the last usable byte (linker symbol `_ai_arena_end`). */
uintptr_t ms_memtest_arena_end(void);
/** Size of AI_ARENA in bytes. */
uint32_t  ms_memtest_arena_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MS_MEMTEST_H */
