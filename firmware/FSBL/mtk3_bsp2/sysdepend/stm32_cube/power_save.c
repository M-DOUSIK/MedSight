/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0 BSP 2.0
 *
 *    Copyright (C) 2023-2024 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2024/02.
 *
 *----------------------------------------------------------------------
 */

#include <sys/machine.h>
#ifdef MTKBSP_STM32CUBE

#include <tk/tkernel.h>
#include <kernel.h>

/*
 *	power_save.c (STM32Cube)
 *	Power-Saving Function
 */

/*
 * MedSight modification (vendored-BSP change #4 - see
 * documents/THIRD_PARTY_SOFTWARE.md's µT-Kernel modification table).
 * No tk_* API signature or semantic is changed; low_pow() is a BSP-supplied
 * power hook, not part of the µT-Kernel 3.0 API specification.
 *
 * Upstream ships low_pow() empty, so the dispatcher's idle loop
 * (dispatch.S, l_dispatch_110) spins the Cortex-M55 at full clock whenever no
 * task is runnable. The body lives in this project's own ms_osal.c - as
 * ms_osal_clean_dcache() already does - so the CMSIS
 * dependency and the cycle accounting stay outside the vendored tree and the
 * diff against upstream stays a single call. See ms_osal.c for the WFI/BASEPRI
 * reasoning and the documented one-line fallback.
 */
IMPORT void ms_osal_low_power_idle( void );

EXPORT void low_pow( void )
{
	ms_osal_low_power_idle();
}

/*
 * Move to suspend mode
 */
EXPORT void off_pow( void )
{
}


#endif /* MTKBSP_STM32CUBE */
