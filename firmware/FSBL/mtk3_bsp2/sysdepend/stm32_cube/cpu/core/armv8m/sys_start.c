/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0 BSP 2.0
 *
 *    Copyright (C) 2025 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2025/03.
 *
 *----------------------------------------------------------------------
 */

#include <sys/machine.h>
#if defined(MTKBSP_STM32CUBE) && defined(MTKBSP_CPU_CORE_ARMV8M)

/*
 *	sys_start.c (STM32Cube & ARMv8-M)
 *	Kernel start routine 
 */
#include <tk/tkernel.h>
#include <kernel.h>
#include <sysdepend/stm32_cube/halif.h>
#include "sysdepend.h"

/* Exception handler table (RAM) */
EXPORT UW knl_exctbl[sizeof(UW)*(N_SYSVEC + N_INTVEC)]
	__attribute__((section(".mtk_exctbl"))) __attribute__ ((aligned(EXCTBL_ALIGN)));


EXPORT UW knl_sysclk;		// System clock frequency
EXPORT UW *knl_exctbl_o;	// Exception handler table (Origin)

EXPORT void		*knl_lowmem_top;	// Head of area (Low address)
EXPORT void		*knl_lowmem_limit;	// End of area (High address)
IMPORT const void	*_end;

/* MedSight project helper (FSBL/Src/ms_osal.c) - see call site below. */
IMPORT void ms_osal_clean_dcache(void *addr, UW size);

#if USE_STATIC_SYS_MEM
EXPORT UW knl_system_mem[SYSTEM_MEM_SIZE/sizeof(UW)] __attribute__((section(".mtk_sysmem")));
#endif

#if USE_DEBUG_SYSMEMINFO
EXPORT void		*knl_sysmem_top	= 0;
EXPORT void		*knl_sysmem_end	= 0;
#endif

EXPORT void knl_start_mtkernel(void)
{
	UW	*src, *top;
	UW	reg;
	INT	i;

	disint();		// Disable Interrupt

	knl_startup_hw();

	/* Copy exception handler (ROM -> RAM) */
	src = knl_exctbl_o = (UW*)in_w(SCB_VTOR);
	top = (UW*)knl_exctbl;
	for(i=0; i < (N_SYSVEC + N_INTVEC); i++) {
		*top++ = *src++;
	}
	out_w(SCB_VTOR, (UW)knl_exctbl);

	/* MedSight fix: this vector table was just built with
	 * ordinary stores into normal, write-back cacheable AXI SRAM, and VTOR
	 * now points at it - with no cache maintenance anywhere in this BSP.
	 * The reference project this BSP was vendored from runs with both CPU
	 * caches disabled (its main.c has SCB_EnableICache()/SCB_EnableDCache()
	 * commented out), so it never had to care; THIS project enables both
	 * (main.c - the camera/NPU pipeline needs them). Clean
	 * the table to the point of coherency so the CPU's exception-entry
	 * vector fetch cannot see stale physical memory. See ms_osal.c's
	 * ms_osal_clean_dcache() comment for the full reasoning. */
	ms_osal_clean_dcache(knl_exctbl, (UW)sizeof(knl_exctbl));

	/* Configure exception priorities */
	reg = *(_UW*)SCB_AIRCR;
	reg = (reg & (~AIRCR_PRIGROUP3)) | AIRCR_PRIGROUP0;	// PRIGRP:SUBPRI = 4: 4
	*(_UW*)SCB_AIRCR = (reg & 0x0000FFFF) | AIRCR_VECTKEY;

	/* Enable UsageFault & BusFault & MemFault */
	out_w(SCB_SHCSR, SHCSR_USGFAULTENA | SHCSR_BUSFAULTENA | SHCSR_MEMFAULTENA);

	out_w(SCB_SHPR2, SCB_SHPR2_VAL);		// SVC pri = 0
	out_w(SCB_SHPR3, SCB_SHPR3_VAL);		// SysTick = 1, PendSV = 15

	knl_sysclk	= SystemCoreClock;		// Get System clock frequency

#if USE_IMALLOC
#if USE_STATIC_SYS_MEM
	knl_lowmem_top = knl_system_mem;
	knl_lowmem_limit = &knl_system_mem[SYSTEM_MEM_SIZE/sizeof(UW)];
#else
	/* Set System memory area */
	if(INTERNAL_RAM_START > SYSTEMAREA_TOP) {
		knl_lowmem_top = (UW*)INTERNAL_RAM_START;
	} else {
		knl_lowmem_top = (UW*)SYSTEMAREA_TOP;
	}
	if((UW)knl_lowmem_top < (UW)&_end) {
		knl_lowmem_top = (UW*)&_end;
	}

	/* MedSight fix: newlib's own heap (_sbrk(), used internally
	 * by printf() to lazily allocate stdio buffers on first use - see
	 * Application/User/sysmem.c) ALSO starts at &_end and was completely
	 * uncoordinated with this kernel heap starting at the exact same
	 * address - the first printf() call anywhere after this point would
	 * silently corrupt whatever this allocator had just placed here.
	 * Reserving the same NEWLIB_HEAP_RESERVE gap sysmem.c now caps its own
	 * heap to keeps the two non-overlapping. Both sides must agree on this
	 * constant - see sysmem.c's own comment on NEWLIB_HEAP_RESERVE. */
#define MS_NEWLIB_HEAP_RESERVE (8u * 1024u)
	knl_lowmem_top = (void*)((UW)knl_lowmem_top + MS_NEWLIB_HEAP_RESERVE);

	if((SYSTEMAREA_END != 0) && (INTERNAL_RAM_END > CNF_SYSTEMAREA_END)) {
		knl_lowmem_limit = (UW*)(SYSTEMAREA_END - EXC_STACK_SIZE);
	} else {
		knl_lowmem_limit = (UW*)(INTERNAL_RAM_END - EXC_STACK_SIZE);
	}
#endif

#if USE_DEBUG_SYSMEMINFO
	knl_sysmem_top	= knl_lowmem_top;
	knl_sysmem_end	= knl_lowmem_limit;
#endif	// USE_DEBUG_MEMINFO
#endif	// USE_IMALLOC

	/* Temporarily disable stack pointer protection */
	// set_msplim((uint32_t)INTERNAL_RAM_START);
	Asm ("msr msplim, %0" : : "r" ((uint32_t)INTERNAL_RAM_START));

	/* Startup Kernel */
	knl_main();		// *** No return ****/
	while(1);		// guard - infinite loops
}

#endif	/* defined(MTKBSP_STM32CUBE) && defined(MTKBSP_CPU_CORE_ARMV8M) */
