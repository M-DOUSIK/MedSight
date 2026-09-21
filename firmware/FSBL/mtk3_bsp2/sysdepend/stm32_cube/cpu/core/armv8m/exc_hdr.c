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
 *	exc_hdr.c (ARMv8-M)
 *	Exception handler
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <kernel.h>
#include "sysdepend.h"
#include "cpu_status.h"

/* TEMPORARY diagnostic instrumentation, from MedSight's bring-up:
 * every handler below used to gate its debug message behind
 * "USE_EXCEPTION_DBG_MSG && USE_TMONITOR" - since USE_TMONITOR is now 0
 * (see mtk3_bsp2/config/config.h's own comment on why), EVERY ONE of these
 * fault handlers was completely silent, printing nothing before its
 * while(1). Any real fault taken anywhere in this investigation
 * would look EXACTLY like every other silent hang already seen. Routed
 * through this project's own working printf()/UART instead, unconditionally,
 * so a real fault can no longer hide. Remove once the boot hang is
 * root-caused. */
#include <stdio.h>
#define EXCEPTION_DBG_MSG(a)	printf("[MS_DIAG] FAULT: " a)

/*
 * NMI handler
 */
WEAK_FUNC EXPORT void knl_nmi_handler(void)
{
	EXCEPTION_DBG_MSG("NMI\n");
	while(1);
}

/*
 * Hard fault handler
 */
WEAK_FUNC EXPORT void knl_hardfault_handler(void)
{
	UW	hfsr, cfsr;
	ID	ctskid;

	hfsr	= *(_UW *)SCB_HFSR;
	if(knl_ctxtsk != NULL) {
		ctskid = knl_ctxtsk->tskid;
	} else {
		ctskid = 0;
	}

	if(hfsr & 0x40000000) {
		cfsr = *(_UW*)SCB_CFSR;
		printf("[MS_DIAG] FAULT: *** Hard fault ***  ctxtsk:%d  HFSR:%lx  CFSR:%lx\r\n",
		       (int)ctskid, (unsigned long)hfsr, (unsigned long)cfsr);
	} else {
		printf("[MS_DIAG] FAULT: *** Hard fault ***  ctxtsk:%d  HFSR:%lx\r\n",
		       (int)ctskid, (unsigned long)hfsr);
	}
	while(1);
}

/*
 * MPU Fault Handler
 */
WEAK_FUNC EXPORT void knl_memmanage_handler(void)
{
	printf("[MS_DIAG] FAULT: MPU Fault, CFSR=%08lx MMFAR=%08lx\r\n",
	       (unsigned long)(*(volatile UW*)SCB_CFSR), (unsigned long)(*(volatile UW*)SCB_MMFAR));
	while(1);
}

/*
 * Bus Fault Handler
 */
WEAK_FUNC EXPORT void knl_busfault_handler(void)
{
	printf("[MS_DIAG] FAULT: Bus Fault, CFSR=%08lx BFAR=%08lx\r\n",
	       (unsigned long)(*(volatile UW*)SCB_CFSR), (unsigned long)(*(volatile UW*)SCB_BFAR));
	while(1);
}

/*
 * Usage Fault Handler
 */
//WEAK_FUNC EXPORT void knl_usagefault_handler(void)
EXPORT void knl_usagefault_handler(void)
{
	printf("[MS_DIAG] FAULT: Usage Fault, CFSR=%08lx\r\n", (unsigned long)(*(volatile UW*)SCB_CFSR));
	while(1);
}

/*
 * Svcall
 */
WEAK_FUNC EXPORT void knl_svcall_handler(void)
{
	EXCEPTION_DBG_MSG("SVCall\n");
	while(1);
}

/*
 * Debug Monitor
 */
WEAK_FUNC EXPORT void knl_debugmon_handler(void)
{
	EXCEPTION_DBG_MSG("Debug Monitor\n");
	while(1);
}

/*
 * Default Handler
 */
WEAK_FUNC EXPORT void knl_default_handler(void)
{
	INT	i;
	_UW	*icpr;

	icpr = (_UW*)NVIC_ICPR_BASE;

	printf("[MS_DIAG] FAULT: Undefined Exception, ICPR: ");
	for(i=0; i < 8; i++) {
		printf("%08lx ", (unsigned long)*icpr++);
	}
	printf("\r\n");
	while(1);
}

#endif /* defined(MTKBSP_STM32CUBE) && defined(MTKBSP_CPU_CORE_ARMV8M) */
