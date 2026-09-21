/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32n6xx_it.c
  * @brief   Interrupt Service Routines.
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
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32n6xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ui/anime_ui.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
extern DCMIPP_HandleTypeDef hdcmipp;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Secure fault.
  */
void SecureFault_Handler(void)
{
  /* USER CODE BEGIN SecureFault_IRQn 0 */

  /* USER CODE END SecureFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_SecureFault_IRQn 0 */
    /* USER CODE END W1_SecureFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  * SVC_Handler is owned by the FreeRTOS port (port.c).
  * FreeRTOSConfig.h maps vPortSVCHandler -> SVC_Handler.
  * Do NOT define a body here â€” the port's implementation handles task switching.
  */
/* SVC_Handler â€” implemented in FreeRTOS port.c via #define vPortSVCHandler SVC_Handler */

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  * PendSV_Handler is owned by the FreeRTOS port (port.c).
  * FreeRTOSConfig.h maps xPortPendSVHandler -> PendSV_Handler.
  * Do NOT define a body here â€” the port's implementation handles context switches.
  */
/* PendSV_Handler â€” implemented in FreeRTOS port.c via #define xPortPendSVHandler PendSV_Handler */

/**
  * @brief This function handles System tick timer.
  * SysTick_Handler is defined in ms_osal.c (µT-Kernel backend).
  *   It runs only during the pre-kernel, bare-metal phase of main() - the
  *   moment knl_start_mtkernel() relocates and reprograms the vector table,
  *   µT-Kernel's own knl_systim_inthdr() takes over the SysTick vector, so
  *   this function is never invoked again after that point (see
  *   the "HAL tick bridge" note in ms_osal.c for the post-kernel
  *   half of this story - a tk_cre_cyc cyclic handler in ms_osal.c).
  *   Do NOT define a body here - duplicate symbol causes linker error.
  */
/* SysTick_Handler â€” owned by ms_osal.c (µT-Kernel backend, pre-kernel phase only) */

void CSI_IRQHandler(void)
{
  HAL_DCMIPP_CSI_IRQHandler(&hdcmipp);
}

void DCMIPP_IRQHandler(void)
{
  HAL_DCMIPP_IRQHandler(&hdcmipp);
}

/******************************************************************************/
/* STM32N6xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32n6xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/**
  * @brief  DMA2D global interrupt handler.
  *         Routes to HAL, which then fires HAL_DMA2D_XferCpltCallback
  *         (defined below) to release the s_dma2d_busy flag in anime_ui.
  *         Priority 8 â€” lower than DCMIPP (priority 5) so the camera
  *         pipeline is never pre-empted by the mascot blending operation.
  */
void DMA2D_IRQHandler(void)
{
  extern DMA2D_HandleTypeDef hdma2d;
  HAL_DMA2D_IRQHandler(&hdma2d);
}

/**
  * @brief  HAL DMA2D transfer-complete weak-override callback.
  *         Clears the busy flag in anime_ui so the next frame can be blended.
  */
void HAL_DMA2D_XferCpltCallback(DMA2D_HandleTypeDef *hdma2d)
{
  UNUSED(hdma2d);
  anime_ui_dma2d_cplt_cb();
}

/**
  * @brief  EXTI line 0 - the IR pill counter on PD0 (Arduino D2).
  *
  *         This is the first EXTI in the project; before it nothing
  *         in FSBL/ configured a GPIO interrupt at all, so line 0 is
  *         uncontended. The vector reaches here the same way DCMIPP's and
  *         DMA2D's do: mtk3_bsp2's sys_start.c copies the startup vector
  *         table verbatim into knl_exctbl before repointing VTOR at it, so
  *         the weak EXTI0_IRQHandler in startup_stm32n657x0hxq_fsbl.s is
  *         overridden by this strong definition exactly as it would be on
  *         bare metal.
  *
  *         Both edges are enabled, because on a ramp the DURATION of a break
  *         is the information, not just its occurrence. The work is four
  *         lines in dispenser.c - no printf, no tk_*, no osal_* - per
  *         a measured record.
  */
void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0);
}

/**
  * @brief  HAL GPIO EXTI weak-override callbacks. The HAL splits rising from
  *         falling on this part, which saves the ISR a GPIO read to work out
  *         which edge it just took.
  */
void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_0) {
    extern void dispenser_exti_rising(void);
    dispenser_exti_rising();
  }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_0) {
    extern void dispenser_exti_falling(void);
    dispenser_exti_falling();
  }
}

/* USER CODE END 1 */
