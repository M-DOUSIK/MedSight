  /**
  ******************************************************************************
  * @file    npu_init.c
  * @author  GPM/AIS Application Team
  * @brief   Collection of functions to perform main configurations in main.c
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#include <string.h>     // Used for memset
#include <stdio.h>      // AiPreInitialize() boot tracing
#include <stdbool.h>

/* A diagnostic, kept and gated OFF rather than deleted - the same
 * treatment MS_BOOT_LED_CHECKPOINTS and MS_DISPLAY_WATCH get in main.c.
 *
 * Standalone boot failed for ten rounds inside aiPreInitialize(), between
 * "ai_vision_init: DEBUG build (-O0)." and "HAL_CACHEAXI_Enable returned 0",
 * and printing which call was responsible is what eventually ended the
 * guessing. It also prints the XSPI register state, which is what proved the
 * controller was IDENTICAL in both boot paths and killed nine theories at
 * once. Set this to 1 if external-memory bring-up ever misbehaves again; it
 * is the right first tool and it costs nothing switched off. */
#ifndef MS_AIPRE_TRACE
#define MS_AIPRE_TRACE 0
#endif
#if MS_AIPRE_TRACE
#define AIPRE(...)  do { printf("aiPre: " __VA_ARGS__); printf("\r\n"); } while (0)
#else
#define AIPRE(...)  do { } while (0)
#endif

#include "npu_init.h"
#include "app_config.h"
#include "npu_cache.h"  // Used in NPU_config
#include "stm32n6570_discovery_xspi.h"

#define USE_NPU_CACHE



static uint32_t Get_RISAF_Max_Addr(RISAF_TypeDef *risaf)
{
  uint32_t max_addr = 0U;
  if      ((risaf == RISAF1_S)  || (risaf == RISAF1_NS))  {max_addr = RISAF1_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF2_S)  || (risaf == RISAF2_NS))  {max_addr = RISAF2_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF3_S)  || (risaf == RISAF3_NS))  {max_addr = RISAF3_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF4_S)  || (risaf == RISAF4_NS))  {max_addr = RISAF4_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF5_S)  || (risaf == RISAF5_NS))  {max_addr = RISAF5_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF6_S)  || (risaf == RISAF6_NS))  {max_addr = RISAF6_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF7_S)  || (risaf == RISAF7_NS))  {max_addr = RISAF7_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF8_S)  || (risaf == RISAF8_NS))  {max_addr = RISAF8_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF9_S)  || (risaf == RISAF9_NS))  {max_addr = RISAF9_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF11_S) || (risaf == RISAF11_NS)) {max_addr = RISAF11_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF12_S) || (risaf == RISAF12_NS)) {max_addr = RISAF12_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF13_S) || (risaf == RISAF13_NS)) {max_addr = RISAF13_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF14_S) || (risaf == RISAF14_NS)) {max_addr = RISAF14_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF15_S) || (risaf == RISAF15_NS)) {max_addr = RISAF15_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF21_S) || (risaf == RISAF21_NS)) {max_addr = RISAF21_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF22_S) || (risaf == RISAF22_NS)) {max_addr = RISAF22_LIMIT_ADDRESS_SPACE_SIZE;}
  else if ((risaf == RISAF23_S) || (risaf == RISAF23_NS)) {max_addr = RISAF23_LIMIT_ADDRESS_SPACE_SIZE;}
  return max_addr;
}

static void Set_RISAF_Default(RISAF_TypeDef *risaf)
{
  RISAF_BaseRegionConfig_t risaf_conf;  
  risaf_conf.StartAddress = 0x0;
  risaf_conf.EndAddress   = Get_RISAF_Max_Addr(risaf); /* as the default config */
  risaf_conf.Filtering    = RISAF_FILTER_ENABLE; // Base region enable (otherwise access control is secure, privileged, trusted domain CID = 1)
  risaf_conf.PrivWhitelist  = RIF_CID_NONE; // apps running in all compartments can access to region in priv/unpriv mode
  risaf_conf.ReadWhitelist  = RIF_CID_MASK; // apps running in all compartments can R in this region
  risaf_conf.WriteWhitelist = RIF_CID_MASK; // apps running in all compartments can W in this region
  // Configure 2 regions with this config, fully overlapping, one for secure one for non secure accesses:
  risaf_conf.Secure = RIF_ATTRIBUTE_SEC;    // Only secure requests can access this region
  HAL_RIF_RISAF_ConfigBaseRegion(risaf, 0, &risaf_conf);
  risaf_conf.Secure = RIF_ATTRIBUTE_NSEC;    // Only non-secure requests can access this region
  HAL_RIF_RISAF_ConfigBaseRegion(risaf, 1, &risaf_conf);
}


void Set_CLK_Sleep_Mode(void)
{
  /* Leave clocks enabled in Low Power modes */
  // Low-power clock enable misc
  __HAL_RCC_DBG_CLK_SLEEP_ENABLE();
  __HAL_RCC_XSPIPHYCOMP_CLK_SLEEP_ENABLE();
  
  // Low-power clock enable for memories
  __HAL_RCC_AXISRAM1_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM2_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM3_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_FLEXRAM_MEM_CLK_SLEEP_ENABLE();
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_SLEEP_ENABLE();
  // LP clock AHB1: None
  // LP clock AHB2: None
  // LP clock AHB3
  __HAL_RCC_RIFSC_CLK_SLEEP_ENABLE();
  __HAL_RCC_RISAF_CLK_SLEEP_ENABLE();
  __HAL_RCC_IAC_CLK_SLEEP_ENABLE();
  // LP clock AHB4: None
  // LP clocks AHB5
  __HAL_RCC_XSPI1_CLK_SLEEP_ENABLE();
  __HAL_RCC_XSPI2_CLK_SLEEP_ENABLE();
  __HAL_RCC_CACHEAXI_CLK_SLEEP_ENABLE();
  __HAL_RCC_NPU_CLK_SLEEP_ENABLE();
  // LP clocks APB1: None
  // LP clocks APB2
  __HAL_RCC_USART1_CLK_SLEEP_ENABLE();
  // LP clocks APB4: None
  // LP clocks APB5: None
}


#if defined(LL_ATON_PLATFORM)
void NPU_Config(void)
{
  // Enable NPU
  __HAL_RCC_NPU_CLK_ENABLE();
  __HAL_RCC_NPU_FORCE_RESET();
  __HAL_RCC_NPU_RELEASE_RESET();
  // Enable Cache-AXI
  __HAL_RCC_CACHEAXI_CLK_ENABLE();
  __HAL_RCC_CACHEAXI_FORCE_RESET();
  __HAL_RCC_CACHEAXI_RELEASE_RESET();
  
  // __HAL_RCC_CACHEAXI_CLK_SLEEP_DISABLE();
  // __HAL_RCC_NPU_CLK_SLEEP_DISABLE();
  // __HAL_RCC_RAMCFG_CLK_SLEEP_DISABLE();
  
#ifdef USE_NPU_CACHE
   npu_cache_enable(); // Useless: already enabled by init
#else
   npu_cache_disable();
#endif

  RIMC_MasterConfig_t master_conf;
  /* Enable Secure access for NPU */
  master_conf.MasterCID = RIF_CID_1;    // Master CID = 1
  master_conf.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV; // Priviledged secure
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &master_conf);  
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU, RIF_ATTRIBUTE_PRIV | RIF_ATTRIBUTE_SEC);
}
#endif

void RISAF_Config(void)
{
  /*
  *  Note: before to set a risaf for a given IP, the IP
  *        should be clocked.
  */
  Set_RISAF_Default(RISAF2_S);          /* SRAM1_AXI */
  Set_RISAF_Default(RISAF3_S);          /* SRAM2_AXI */

#if defined(LL_ATON_PLATFORM)
  Set_RISAF_Default(RISAF4_S);          /* NPU MST0 */
  Set_RISAF_Default(RISAF5_S);          /* NPU MST1 */
#endif
  
  Set_RISAF_Default(RISAF6_S);          /* SRAM3,4,5,6_AXI */
  Set_RISAF_Default(RISAF7_S);          /* FLEXMEM */
  
#if defined(LL_ATON_PLATFORM)
#ifdef USE_NPU_CACHE
  Set_RISAF_Default(RISAF8_S);          /* NPU_CACHE */
  Set_RISAF_Default(RISAF15_S);         /* NPU_CACHE config */
#endif  
#endif
  
  // Set_RISAF_Default(RISAF9_S);       /* VENC */
  

#if (USE_EXTERNAL_RAM)
  Set_RISAF_Default(RISAF11_S);         /* OCTOSPI1 0x9000 0000 */
#endif
  Set_RISAF_Default(RISAF12_S);         /* OCTOSPI2 0x7000 0000 */
  // Set_RISAF_Default(RISAF13_S);      /* OCTOSPI3 0x8000 0000 */
  
}



void SystemInit_POST(void)
{  
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_CRC_CLK_ENABLE();
   
  /* Enable NPU RAMs (4x448KB) + CACHEAXI using HAL macros */
  __HAL_RCC_AXISRAM1_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM2_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM3_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM4_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM5_MEM_CLK_ENABLE();
  __HAL_RCC_AXISRAM6_MEM_CLK_ENABLE();
  __HAL_RCC_CACHEAXIRAM_MEM_CLK_ENABLE();
  
  __HAL_RCC_RAMCFG_CLK_ENABLE();
  RAMCFG_HandleTypeDef hramcfg = {0};
  hramcfg.Instance = RAMCFG_SRAM2_AXI; HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance = RAMCFG_SRAM3_AXI; HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance = RAMCFG_SRAM4_AXI; HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance = RAMCFG_SRAM5_AXI; HAL_RAMCFG_EnableAXISRAM(&hramcfg);
  hramcfg.Instance = RAMCFG_SRAM6_AXI; HAL_RAMCFG_EnableAXISRAM(&hramcfg);

  // Set low-power mode for clocks
  Set_CLK_Sleep_Mode();

  /* Allow caches to be activated. Default value is 1, but the current boot sets it to 0 */
  MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_DCACTIVE_Msk | MEMSYSCTL_MSCR_ICACTIVE_Msk;
}


void aiPreInitialize(void)
{
    AIPRE("enter");
    SystemInit_POST();
    AIPRE("SystemInit_POST done");

    /* ── Undo the boot ROM's compartmentalisation ─────────────
     *
     * UM/community "STM32N6 boot ROM explained": the boot ROM handles its own
     * data in AXI SRAM2 by configuring RISAF2, and "configures seven regions
     * inside RISAF2 and dynamically sets each region depending on the boot ROM
     * code phase being executed". Those regions are still configured when it
     * hands over - the ROM does not put them back.
     *
     * This project deliberately skipped RISAF_Config(), with the comment
     * "to avoid accidental compartmentalization, matching the ST official
     * example". That is correct for DEVELOPMENT boot, where the debugger
     * loads us into SRAM, RISAF is at its permissive reset defaults, and
     * reconfiguring it could only narrow access that is already open.
     *
     * Booting from external flash it is exactly backwards: the ROM and the
     * first-stage loader have already narrowed things, and RISAF_Config() is
     * the function that opens them back up - RISAF12_S is OCTOSPI2 at
     * 0x70000000, the NOR holding the weights, and RISAF11_S is OCTOSPI1.
     * BFAR=0x71026FC0 is a FILTERED address, not an absent one.
     *
     * Before the XSPI work, not after, because the filters have to be open
     * before anything touches the flash. Clocks first: RISAF cannot be
     * configured for an IP that is not clocked. */
    __HAL_RCC_RIFSC_CLK_ENABLE();
    __HAL_RCC_RISAF_CLK_ENABLE();
    __HAL_RCC_XSPIM_CLK_ENABLE();
    __HAL_RCC_XSPI1_CLK_ENABLE();
    __HAL_RCC_XSPI2_CLK_ENABLE();
    /* NOT the whole RISAF_Config(). Calling it hangs on this board even in
     * development boot - it walks every RISAF instance including ones whose
     * IP is not clocked here - which is the other half of why this project
     * had it switched off. Only the two external-memory filters matter for
     * reaching the flash, so only those are touched, each announced so a hang
     * names itself instead of going quiet. */
#if (USE_EXTERNAL_RAM)
    AIPRE("RISAF11 (OCTOSPI1)...");
    Set_RISAF_Default(RISAF11_S);
    AIPRE("RISAF11 done");
#endif
    /* ── Stop guessing, print the difference ──────────────────
     *
     * Nine single-variable experiments have each been a clean negative, all
     * chasing the same question: what does the first-stage loader leave
     * behind that a peripheral reset does not clear? Guessing at that has
     * cost hours. These registers answer it directly - dump them in BOTH boot
     * paths and diff. XSPI1 is dumped alongside XSPI2 deliberately: it is the
     * control, because the PSRAM on XSPI1 initialises fine in both boots
     * while the NOR on XSPI2 only works from the debugger.
     *
     * Clocks are enabled above, so these reads cannot hang the way an earlier
     * probe did. */
    AIPRE("regs XSPI2 CR=%08lX DCR1=%08lX DCR2=%08lX DCR3=%08lX DCR4=%08lX",
          (unsigned long)XSPI2->CR,  (unsigned long)XSPI2->DCR1,
          (unsigned long)XSPI2->DCR2,(unsigned long)XSPI2->DCR3,
          (unsigned long)XSPI2->DCR4);
    AIPRE("regs XSPI2 CCR=%08lX TCR=%08lX IR=%08lX SR=%08lX",
          (unsigned long)XSPI2->CCR, (unsigned long)XSPI2->TCR,
          (unsigned long)XSPI2->IR,  (unsigned long)XSPI2->SR);
    AIPRE("regs XSPI1 CR=%08lX DCR1=%08lX SR=%08lX  (control: this one works)",
          (unsigned long)XSPI1->CR,  (unsigned long)XSPI1->DCR1,
          (unsigned long)XSPI1->SR);
    AIPRE("regs XSPIM CR=%08lX", (unsigned long)XSPIM->CR);

    AIPRE("RISAF12 (OCTOSPI2, the NOR)...");
    Set_RISAF_Default(RISAF12_S);
    AIPRE("RISAF12 done");

    /* The FaceID embedder's compiled network (faceid.c) uses
     * the board's external Hexadeca-SPI PSRAM as scratch activation space
     * at physical address 0x90000000 (its "hyperRAM" memory pool per
     * faceid.c's generated header comment: "index=4 file postfix=xSPI1
     * name=hyperRAM offset=0x90000000... size=16777208"). Nothing in this
     * project previously brought that chip up - the detector (fd.c) never
     * needed it, which is why fd worked before this was added but faceid
     * hung/crashed the instant its epoch blocks touched that address.
     * Sequence matches PeleAB's own working platform.c exactly (their only
     * documented, proven-correct init order for this exact board/chip). */
    /* ── Hand the XSPI back to us before we configure it ──────
     *
     * In development boot the debugger loads this firmware straight into SRAM
     * and nothing has touched XSPI, so the BSP starts from a clean peripheral.
     * Booting from external flash is different: the first-stage loader has
     * ALREADY put XSPI2 into memory-mapped mode - that is how it read this
     * application out of flash in the first place - and the BSP's own software
     * state here is fresh zeroes that know nothing about it. Re-running
     * BSP_XSPI_NOR_Init() against a peripheral already in memory-mapped mode
     * hangs inside the HAL waiting on a busy flag, and it hangs exactly where
     * weights/README.md says to expect trouble: after
     * "ai_vision_init: DEBUG build (-O0)." and before "HAL_CACHEAXI_Enable".
     *
     * A peripheral reset is the honest fix rather than trying to unwind a
     * configuration we did not make. It costs microseconds, it is idempotent,
     * and it makes both boot paths start from the same known state instead of
     * one of them depending on what a loader happened to leave behind.
     *
     * Safe to do here: this code runs from SRAM, never from the flash being
     * reset, and no weights have been read yet. XSPIM is the shared manager
     * for both ports, so it resets first. */
    /* PSRAM first, NOR second - the original order, restored.
     *
     * The reverse was tried, on the theory that XSPI_RAM_MspInit()
     * resets the shared XSPI I/O Manager while XSPI_NOR_MspInit() does not,
     * so the NOR always inherits XSPIM state it did not choose. Tested on
     * hardware: the NOR failed identically when it went first, so ordering is
     * not the cause and the original sequence stands. */
    {
        int32_t r;
        AIPRE("BSP_XSPI_RAM_Init...");
        r = BSP_XSPI_RAM_Init(0);
        AIPRE("BSP_XSPI_RAM_Init -> %ld", (long)r);

        AIPRE("BSP_XSPI_RAM_EnableMemoryMappedMode...");
        r = BSP_XSPI_RAM_EnableMemoryMappedMode(0);
        AIPRE("BSP_XSPI_RAM_EnableMemoryMappedMode -> %ld", (long)r);
        (void)r;   /* only read by the trace, which gates off */
    }

    /* NPU weights are placed into OCTOSPI2 by the linker. Initialize the NOR flash. */
    BSP_XSPI_NOR_Init_t NOR_Init;
    NOR_Init.InterfaceMode = BSP_XSPI_NOR_OPI_MODE;
    NOR_Init.TransferRate = BSP_XSPI_NOR_DTR_TRANSFER;
    /* ── Do not re-initialise a NOR that is already mapped ────
     *
     * Booting from external flash, the first-stage loader has ALREADY put
     * XSPI2 into memory-mapped mode and - this is the part that matters -
     * switched the MX66UW1G45G itself into Octal-DTR mode to do it. That is a
     * property of the CHIP, not the controller, and it persists until a power
     * cycle. BSP_XSPI_NOR_Init() begins by talking single-SPI to identify and
     * configure the device, so against a chip that now only answers OPI it
     * fails: -5 from both calls, the NOR never gets mapped, and the first NPU
     * weight read bus-faults at BFAR=0x71026FC0, inside ec_blobs.
     *
     * Resetting the XSPI controller does not help and was tried: it clears the
     * controller but cannot put the flash chip back into SPI mode, so the
     * mismatch survives. The honest move is to notice that the mapping we were
     * about to create already exists and leave it alone. FMODE == 0b11 with
     * the peripheral enabled IS memory-mapped mode; it is a plain register
     * read and cannot fault.
     *
     * In development boot none of this applies - the debugger loads us into
     * SRAM, nothing has touched XSPI2, FMODE is clear, and the branch below
     * runs exactly the code that has always run. */
    {
        int32_t r;

        /* NO "is it already mapped?" CHECK HERE, and that absence is
         * deliberate. An earlier attempt read XSPI2->CR to detect the boot
         * loader's existing memory-mapped configuration and skip this init.
         * It never fired in standalone boot - by the time we run, our own
         * clock setup has already torn the loader's mapping down - and it
         * BROKE development boot outright, because there XSPI2's peripheral
         * clock is still gated and reading the register of an unclocked
         * peripheral hangs the bus. The boot stopped dead between
         * "BSP_XSPI_RAM_EnableMemoryMappedMode -> 0" and the next line, with
         * no fault and no output. Probing a peripheral you have not clocked is
         * not a cheap check; it is an expensive one that usually gets away
         * with it. */
        {
            /* Retry, because the first attempt is not wasted work.
             *
             * Booting from external flash, the loader leaves the MX66UW1G45G
             * in Octal-DTR - a property of the CHIP that survives any
             * controller reset - and BSP_XSPI_NOR_Init() opens by talking
             * single-SPI to a device that no longer answers it, returning -5
             * (BSP_ERROR_COMPONENT_FAILURE). But its XSPI_NOR_ResetMemory()
             * step issues reset commands in SPI, OPI-STR and OPI-DTR before
             * that failure, so the chip very likely IS back in SPI mode by the
             * time the call returns. A second attempt then starts from the
             * same clean state development boot always enjoys.
             *
             * Bounded at three, DeInit between attempts so the BSP's own
             * context does not go stale, and every result printed - a silent
             * retry loop that sometimes works is worse than no retry at all.
             * In development boot the first attempt succeeds and the loop
             * exits immediately, so that path is unchanged. */
            r = BSP_ERROR_COMPONENT_FAILURE;
            for (int attempt = 1; (attempt <= 3) && (r != BSP_ERROR_NONE); attempt++) {
                if (attempt > 1) {
                    (void)BSP_XSPI_NOR_DeInit(0);
                    HAL_Delay(10);
                }
                AIPRE("BSP_XSPI_NOR_Init attempt %d...", attempt);
                r = BSP_XSPI_NOR_Init(0, &NOR_Init);
                AIPRE("BSP_XSPI_NOR_Init attempt %d -> %ld", attempt, (long)r);
            }

            AIPRE("BSP_XSPI_NOR_EnableMemoryMappedMode...");
            r = BSP_XSPI_NOR_EnableMemoryMappedMode(0);
            AIPRE("BSP_XSPI_NOR_EnableMemoryMappedMode -> %ld", (long)r);
        }
    }

#if defined(LL_ATON_PLATFORM)
    AIPRE("RIFSC clk...");
    __HAL_RCC_RIFSC_CLK_ENABLE(); // Ensure RIFSC is active before configuring RIMC
    AIPRE("NPU_Config...");
    NPU_Config();
    AIPRE("NPU_Config done");
#endif
    AIPRE("leave");
  
  /* RISAF_Config() now runs at the TOP of this function - see the note
     there. It used to be skipped entirely, which is right for development
     boot and wrong for booting from flash. */
}
