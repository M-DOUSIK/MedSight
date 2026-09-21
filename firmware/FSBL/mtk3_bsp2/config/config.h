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

#ifndef _MTKBSP_TK_CONFIG_
#define _MTKBSP_TK_CONFIG_
/*
 *	config.h
 *	User Configuration Definition
 */
/*---------------------------------------------------------------------- */
/* SYSCONF: micro T-Kernel system configuration
 */

#define	CNF_SYSTEMAREA_TOP	0	/* 0: Use system default address */
/* MedSight fix: 0 ("use system default") makes sys_start.c use
 * INTERNAL_RAM_END, which sysdepend/stm32_cube/cpu/stm32n6/sysdef.h defines
 * as the end of the *entire physical* AXI SRAM0 bank (0x341FFF00) - the
 * full 2047KB, not this project's own linker-script "RAM" region, which is
 * only the first 1023KB of that same bank (STM32N657X0HXQ_AXISRAM2_fsbl.ld:
 * RAM ORIGIN=0x34000400 LENGTH=1023K, ending at 0x34100000). The bytes in
 * between (0x34100000-0x34180400) and this project's own "ROM" linker
 * region for.text/.rodata (0x34180400-0x34200000, ending exactly where the
 * camera framebuffer at 0x34200000 begins) are NOT free memory in this
 * project - they hold running code. Left at the vendored default, µT-Kernel's
 * dynamic allocator (imalloc, backing tk_cre_tsk's stacks, tk_cre_mbf's
 * buffers, tk_cre_cyc's control blocks) would hand out memory inside that
 * "ROM" range the first time anything is created in usermain(), corrupting
 * this project's own code with zero warning and no UART output - exactly
 * the silent hang into Default_Handler this fix addresses. Pinning
 * CNF_SYSTEMAREA_END to this project's actual RAM-region end keeps the
 * kernel's heap inside memory this project's linker script actually reserves
 * for dynamic use.
 *
 * This constant and the linker script's MEMORY block are
 * ONE decision and must always move together.
 *
 * ROM and RAM are swapped so the loadable image sits where ST's
 * two-stage boot loader expects it (see the linker script's own note on why).
 * RAM is now 0x34100000-0x341FFFFF, so the kernel's heap ceiling follows it
 * to 0x34200000 - still exactly where the camera framebuffer begins, which is
 * the real physical boundary this value has always been tracking.
 *
 * Changing the linker script and leaving this at 0x34100000 produced a silent
 * hang on hardware, and it is worth recording precisely because it looks like
 * nothing:.bss moved up with the RAM region, so _end landed at 0x341A3890,
 * ABOVE the old ceiling. knl_lowmem_top then exceeded knl_lowmem_limit, the
 * kernel heap had negative size, every tk_cre_* in usermain() failed, and the
 * scheduler started with no tasks. The board printed its whole pre-kernel
 * boot log and then stopped between "sleep clocks:" and "task_camera_isp:
 * started." with no fault, no Error_Handler and nothing on UART. */
#define CNF_SYSTEMAREA_END	0x34200000

#define	CNF_MAX_TSKPRI		32	/* Task Max priority */

/* MedSight fix: the vendored default (10 ms) is a 10x
 * regression against the FreeRTOS build this project was written on, in two
 * compounding ways:
 *   1. tk_dly_tsk() granularity. main.c's camera/ISP task sleeps
 *      osal_delay_ms(1) per iteration and the UI task osal_delay_ms(10);
 *      at a 10 ms tick both round up to the next tick boundary, so the ISP
 *      loop ran at a tenth of its intended rate and touch polling at half.
 *   2. ms_osal.c's HAL tick bridge. HAL_IncTick() adds ONE to uwTick per
 *      call, so firing it every 10 ms made HAL_GetTick() advance 1 ms per
 *      10 ms of real time - HAL's whole millisecond clock ran 10x SLOW, not
 *      merely 10x coarse. Every HAL_GetTick()-based deadline in the UI
 *      stretched by 10x: the mascot's 250 ms animation frame gate
 *      (anime_ui.h LUMIO_FRAME_MS) became 2.5 s, so 0.4 FPS instead of 4,
 *      and state_machine.c's 500 ms touch dwell guard became 5 s - which is
 *      what "the buttons feel unresponsive" actually was.
 * 1 ms matches what FreeRTOS's tick did, is within this port's own
 * MIN_TIMER_PERIOD..MAX_TIMER_PERIOD (1..50, see
 * include/sys/sysdepend/stm32_cube/cpu/stm32n6/sysdef.h), and costs the same
 * 1 kHz timer ISR the FreeRTOS build already paid for. ms_osal.c's
 * OSAL_HAL_TICK_PERIOD_MS is derived from this macro so the two can no
 * longer drift apart. */
#define CNF_TIMER_PERIOD	1	/* System timer period */

/* Maximum number of kernel objects */
#define CNF_MAX_TSKID		32	/* Task */
#define CNF_MAX_SEMID		16	/* Semaphore */
#define CNF_MAX_FLGID		16	/* Event flag */
#define CNF_MAX_MBXID		8	/* Mailbox*/
#define CNF_MAX_MTXID		4	/* Mutex */
#define CNF_MAX_MBFID		8	/* Message buffer */
#define CNF_MAX_MPLID		4	/* Memory pool */
#define CNF_MAX_MPFID		8	/* Fixed size memory pool */
#define CNF_MAX_CYCID		4	/* Cyclic handler */
#define CNF_MAX_ALMID		8	/* Alarm handler */

/* Device configuration */
#define CNF_MAX_REGDEV		(8)	/* Max registered device */
#define CNF_MAX_OPNDEV		(16)	/* Max open device */
#define CNF_MAX_REQDEV		(16)	/* Max request device */
#define CNF_DEVT_MBFSZ0		(-1)	/* message buffer size for event notification */
#define CNF_DEVT_MBFSZ1		(-1)	/* message max size for event notification */

/* Version Number */
#define CNF_VER_MAKER		0
#define CNF_VER_PRID		0
#define CNF_VER_PRVER		3
#define CNF_VER_PRNO1		0
#define CNF_VER_PRNO2		0
#define CNF_VER_PRNO3		0
#define CNF_VER_PRNO4		0


/*---------------------------------------------------------------------- */
/* Backwards compatible api support 
 *      micro T-Kernel2.0 API support (Rendezvous)
 */
#define USE_LEGACY_API		(0)	/* 1: Valid  0: Invalid */
#define CNF_MAX_PORID		(0)	/* Maximum number of Rendezvous */


/*---------------------------------------------------------------------- */
/* Stack size definition
 */
#define CNF_EXC_STACK_SIZE	(0)	/* Exception stack size */
#define	CNF_TMP_STACK_SIZE	(256)	/* Temporary stack size */


/*---------------------------------------------------------------------- */
/* System function selection
 *  1: Use function.  0: No use function.
 */
#define USE_NOINIT		(0)	/* Use zero-clear bss section */
#define USE_IMALLOC		(1)	/* Use dynamic memory allocation */
#define USE_SHUTDOWN		(1)	/* Use System shutdown */
#define USE_STATIC_IVT		(0)	/* Use static interrupt vector table */


/*---------------------------------------------------------------------- */
/* Check API parameter
 *   1: Check parameter  0: Do not check parameter
 */
#define CHK_NOSPT		(1)	/* Check unsupported function (E_NOSPT) */
#define CHK_RSATR		(1)	/* Check reservation attribute error (E_RSATR) */
#define CHK_PAR			(1)	/* Check parameter (E_PAR) */
#define CHK_ID			(1)	/* Check object ID range (E_ID) */
#define CHK_OACV		(1)	/* Check Object Access Violation (E_OACV) */
#define CHK_CTX			(1)	/* Check whether task-independent part is running (E_CTX) */
#define CHK_CTX1		(1)	/* Check dispatch disable part */
#define CHK_CTX2		(1)	/* Check task independent part */
#define CHK_SELF		(1)	/* Check if its own task is specified (E_OBJ) */

#define	CHK_TKERNEL_CONST	(1)	/* Check const-type parameter */

/*---------------------------------------------------------------------- */
/* User initialization program (UserInit)
 *
 */
#define	USE_USERINIT		(0)	/*  1: Use UserInit  0: Do not use UserInit */
#define RI_USERINIT		(0)	/* UserInit start address */


/*---------------------------------------------------------------------- */
/* Debugger support function
 *   1: Valid  0: Invalid
 */
#define USE_DBGSPT		(0)	/* Use mT-Kernel/DS */
#define USE_OBJECT_NAME		(0)	/* Use DS object name */

#define OBJECT_NAME_LENGTH	(8)	/* DS Object name length */

/*---------------------------------------------------------------------- */
/* Use T-Monitor Compatible API Library  & Message to terminal.
 *  1: Valid  0: Invalid
 */
/* MedSight fix: the vendored default (1) makes knl_main()'s
 * very first step, libtm_init(), call tm_com_init() -
 * sysdepend/stm32_cube/lib/libtm/discovery_stm32n657/tm_com.c - which
 * directly re-writes USART1's CR1/CR2/CR3/BRR registers by raw address
 * (0x52001000, hardcoded for USART1) with a baud-rate divisor (0x022C)
 * computed for whatever clock tree the reference camera sample runs at,
 * not this project's own SystemClock_Config(). This project already has a
 * fully working, HAL-managed USART1 (BSP_COM_Init()) that every
 * printf() in this codebase - including every one of this project's own
 * boot-sequence diagnostic prints - depends on. Confirmed by observation:
 * UART output printed cleanly right up through the last line before
 * knl_main() was called, then went silent immediately once knl_main()
 * (and therefore libtm_init()) ran - while the board kept running, not
 * hanging (visible LCD activity continued). T-Monitor's own interactive
 * console (tm_getchar()/tm_getline(), which additionally contain BLOCKING
 * reads on that same UART - see mtkernel/lib/libtm/libtm.c) is a feature
 * this project has no use for and never intentionally invokes. Disabling
 * it here removes both the silent UART reconfiguration and the latent
 * blocking-read hazard in one step. */
#define	USE_TMONITOR		(0)	/* T-Monitor API */
#define USE_SYSTEM_MESSAGE	(1)	/* System Message */
#define USE_EXCEPTION_DBG_MSG	(1)	/* Excepttion debug message */
#define USE_TASK_DBG_MSG	(0)	/* Tsak debug message */

/*---------------------------------------------------------------------- */
/* Use Co-Processor.
 *  1: Valid  0: Invalid
 */
#define	USE_FPU			(1)	/* Use FPU */
#define	USE_DSP			(0)	/* Use DSP */

#define	ALWAYS_FPU_ATR		(1)	/* Always set the TA_FPU attribute on all tasks */

/*---------------------------------------------------------------------- */
/* Use Physical timer.
 *  1: Valid  0: Invalid
 */
#define USE_PTMR		(0)	/* Use Physical timer */

/*---------------------------------------------------------------------- */
/* Use Sample device driver.
 *  1: Valid  0: Invalid
 */
#define USE_SDEV_DRV		(0)	/* Use Sample device driver */

/*---------------------------------------------------------------------- */
/*
 *	Use Standard C include file
 */
#define USE_STDINC_STDDEF	(1)	/* Use <stddef.h> */

#define USE_STDINC_STDINT	(1)	/* Use <stdint.h> */

/*---------------------------------------------------------------------- */
/*
 *	BSP depended Definition
 */
#include "config_bsp.h"

/*---------------------------------------------------------------------- */
/*
 *	Use function Definition
 */

#include <mtkernel/config/config_func.h>

#endif /* _MTKBSP_TK_CONFIG_ */
