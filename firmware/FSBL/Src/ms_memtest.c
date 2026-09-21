/* ms_memtest.c - see ms_memtest.h for why this exists. */

#include "ms_memtest.h"
#include "stm32n6xx_hal.h"
#include <stdio.h>

/* Provided by STM32N657X0HXQ_AXISRAM2_fsbl.ld's.ai_arena section. Declared
 * as arrays rather than pointers: a linker symbol has no storage, so taking
 * its ADDRESS is the only correct way to read it. */
extern uint8_t _ai_arena_start[];
extern uint8_t _ai_arena_end[];

uintptr_t ms_memtest_arena_start(void) { return (uintptr_t)_ai_arena_start; }
uintptr_t ms_memtest_arena_end(void)   { return (uintptr_t)_ai_arena_end;   }
uint32_t  ms_memtest_arena_size(void)
{
    return (uint32_t)((uintptr_t)_ai_arena_end - (uintptr_t)_ai_arena_start);
}

#if MEDSIGHT_ARENA_SELFTEST

/* One write-then-verify sweep. `pattern_fn` turns a word index into the value
 * that index should hold, which is what lets the address-in-address pass
 * share this code with the two constant patterns. */
static bool sweep(volatile uint32_t *base, uint32_t words,
                  uint32_t (*pattern_fn)(uint32_t idx, uintptr_t addr),
                  const char *name)
{
    for (uint32_t i = 0; i < words; i++) {
        base[i] = pattern_fn(i, (uintptr_t)&base[i]);
    }

    /* The arena is ordinary cacheable memory as far as the CPU is concerned,
     * so a write-back cache could satisfy the read from the line that is
     * still sitting in L1 and never touch the RAM at all - which would make
     * an unpowered bank pass. Clean+invalidate between the write and the
     * read so every load below is a real bus access.
     *
     * This is the same asymmetry the sleep-clock note is about:
     * a measurement taken by the CPU can be structurally unable to see the
     * thing being measured. */
    SCB_CleanInvalidateDCache_by_Addr((void *)base, (int32_t)(words * 4u));

    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = pattern_fn(i, (uintptr_t)&base[i]);
        uint32_t got  = base[i];
        if (got != want) {
            printf("ARENA SELFTEST FAIL: pass '%s' at 0x%08lX - "
                   "wrote %08lX, read %08lX (word %lu of %lu)\r\n",
                   name, (unsigned long)(uintptr_t)&base[i],
                   (unsigned long)want, (unsigned long)got,
                   (unsigned long)i, (unsigned long)words);
            return false;
        }
    }
    return true;
}

static uint32_t pat_a5(uint32_t idx, uintptr_t addr)
{
    (void)idx; (void)addr; return 0xA5A5A5A5u;
}
static uint32_t pat_5a(uint32_t idx, uintptr_t addr)
{
    (void)idx; (void)addr; return 0x5A5A5A5Au;
}
/* The address of the word IS the value. If a write to 0x343A0000 actually
 * landed at 0x34380000 because a bank is aliased or short-decoded, both
 * constant passes above still read back the constant and report success;
 * this one reads back the wrong address and says so. */
static uint32_t pat_addr(uint32_t idx, uintptr_t addr)
{
    (void)idx; return (uint32_t)addr;
}

bool ms_memtest_arena(void)
{
    volatile uint32_t *base  = (volatile uint32_t *)ms_memtest_arena_start();
    uint32_t           bytes = ms_memtest_arena_size();
    uint32_t           words = bytes / 4u;

    if (words == 0u) {
        printf("ARENA SELFTEST: arena is empty (0 bytes) - nothing to test.\r\n");
        return false;
    }

    printf("ARENA SELFTEST: testing 0x%08lX-0x%08lX (%lu bytes)...\r\n",
           (unsigned long)ms_memtest_arena_start(),
           (unsigned long)(ms_memtest_arena_end() - 1u),
           (unsigned long)bytes);

    uint32_t t0 = HAL_GetTick();
    bool ok = sweep(base, words, pat_a5,   "A5A5A5A5")
           && sweep(base, words, pat_5a,   "5A5A5A5A")
           && sweep(base, words, pat_addr, "address-in-address");
    uint32_t dt = HAL_GetTick() - t0;

    if (ok) {
        printf("ARENA SELFTEST PASS: %lu bytes writable and readable "
               "(3 patterns, %lums).\r\n", (unsigned long)bytes,
               (unsigned long)dt);
        /* Leave it as it was found. Not strictly required - nothing reads
         * this memory - but a future model's first run should not inherit
         * an address-shaped pattern that could be mistaken for its own
         * output. */
        for (uint32_t i = 0; i < words; i++) {
            base[i] = 0u;
        }
        SCB_CleanInvalidateDCache_by_Addr((void *)base, (int32_t)(words * 4u));
    } else {
        printf("ARENA SELFTEST FAIL: the AI_ARENA region in the linker "
               "script does not describe usable memory. Do NOT record it as "
               "available. Check that aiPreInitialize() has run (AXISRAM5/6 "
               "are unpowered before it) - see the technical reference.\r\n");
    }
    return ok;
}

#else  /* !MEDSIGHT_ARENA_SELFTEST */

bool ms_memtest_arena(void) { return true; }

#endif
