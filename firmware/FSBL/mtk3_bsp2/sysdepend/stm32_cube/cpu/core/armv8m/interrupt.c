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
 *	interrupt.c (ARMv8-M)
 *	Interrupt control
 */

#include <tk/tkernel.h>
#include <kernel.h>
#include "sysdepend.h"
#include "cpu_status.h"
#include <stdio.h>

/* MedSight: D-cache maintenance helper, FSBL/Src/ms_osal.c.
 * See the call site at the end of knl_init_interrupt(). */
IMPORT void ms_osal_clean_dcache(void *addr, UW size);

/* HLL Interrupt Handler Table */
LOCAL UW hllint_tbl[sizeof(UW)*N_INTVEC];

/* ------------------------------------------------------------------------ */
/*
 * HLL(High level programming language) Interrupt Handler
 */
EXPORT void knl_hll_inthdr(void)
{
	FP	inthdr;
	UW	intno;

	ENTER_TASK_INDEPENDENT;

	intno	= knl_get_ipsr() - 16;
	inthdr	= (FP)hllint_tbl[intno];

	(*inthdr)(intno);

	LEAVE_TASK_INDEPENDENT;
}

/* ------------------------------------------------------------------------ */
/*
 * System-timer Interrupt handler
 */
EXPORT void knl_systim_inthdr(void)
{
	ENTER_TASK_INDEPENDENT;

	knl_timer_handler();

	LEAVE_TASK_INDEPENDENT;
}

/* ----------------------------------------------------------------------- */
/*
 * Set interrupt handler (Used in tk_def_int())
 */
EXPORT ER knl_define_inthdr( INT intno, ATR intatr, FP inthdr )
{
	volatile FP	*intvet;

	if(inthdr != NULL) {
		if ( (intatr & TA_HLNG) != 0 ) {
			hllint_tbl[intno] = (UW)inthdr;
			inthdr = knl_hll_inthdr;
		}		
	} else 	{	/* Clear interrupt handler */
		inthdr = (FP)knl_exctbl_o[N_SYSVEC + intno];
	}
	intvet = (FP*)(knl_exctbl + N_SYSVEC);
	intvet[intno] = inthdr;

	return E_OK;
}

/* ----------------------------------------------------------------------- */
/*
 * Return interrupt handler (Used in tk_ret_int())
 */
EXPORT void knl_return_inthdr(void)
{
	/* No processing in ARM. */
	return;
}

void knl_default_handler(void)
{
	/* MedSight fix: this STRONG definition overrides the WEAK
	 * one in exc_hdr.c, so it - not exc_hdr.c's - is what lands in the
	 * link. It printed via tm_printf(), which became a silent no-op when
	 * USE_TMONITOR was set to 0 (see config.h), leaving an undefined
	 * exception indistinguishable from any other silent hang. Routed
	 * through this project's own working printf(), same as every handler
	 * in exc_hdr.c. */
	printf("[MS_DIAG] FAULT: Undefined Exception (knl_default_handler), IPSR=%lu\r\n",
	       (unsigned long)(knl_get_ipsr() & 0x1FFu));
	while(1);
}

/* ------------------------------------------------------------------------ */
/*
 * Interrupt initialize
 */
EXPORT ER knl_init_interrupt( void )
{
	/* Set Exception handler */
	knl_exctbl[2]	= (UW)knl_nmi_handler;		/* 2: NMI Handler */
	knl_exctbl[3]	= (UW)knl_hardfault_handler;	/* 3: Hard Fault Handler */
	knl_exctbl[4]	= (UW)knl_memmanage_handler;	/* 4: MPU Fault Handler */
	knl_exctbl[5]	= (UW)knl_busfault_handler;	/* 5: Bus Fault Handler */
	knl_exctbl[6]	= (UW)knl_usagefault_handler;	/* 6: Usage Fault Handler */

	knl_exctbl[11]	= (UW)knl_svcall_handler;	/* 11: Svcall */
	knl_exctbl[12]	= (UW)knl_debugmon_handler;	/* 12: Debug Monitor Handler */

	knl_exctbl[14]	= (UW)knl_dispatch_entry;	/* 14: Pend SV */
	knl_exctbl[15]	= (UW)knl_systim_inthdr;	/* 15: Systick */

	/* MedSight fix: the fourteen stores above went into normal,
	 * write-back cacheable AXI SRAM that VTOR already points at. Clean them
	 * to the point of coherency before any of these handlers can be taken -
	 * without this, the CPU's exception-entry vector fetch reads stale
	 * physical memory and no exception is ever delivered. See sys_start.c's
	 * matching call and ms_osal.c's ms_osal_clean_dcache() for the full
	 * reasoning (this BSP's own reference project runs with both CPU caches
	 * disabled, so it never needed any of this; this project needs them for
	 * the camera/NPU pipeline). */
	ms_osal_clean_dcache(knl_exctbl, (UW)(sizeof(UW) * (N_SYSVEC + N_INTVEC)));

	return E_OK;
}

#endif	/* defined(MTKBSP_STM32CUBE) && defined(MTKBSP_CPU_CORE_ARMV8M) */