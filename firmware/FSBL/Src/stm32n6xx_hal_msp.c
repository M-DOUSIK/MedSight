/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32n6xx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
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
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  /* System interrupt init*/

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/**
* @brief DCMIPP MSP Initialization
* This function configures the hardware resources used in this example
* @param hdcmipp: DCMIPP handle pointer
* @retval None
*/
void HAL_DCMIPP_MspInit(DCMIPP_HandleTypeDef* hdcmipp)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  RAMCFG_HandleTypeDef hramcfg  = {0};
  RIMC_MasterConfig_t RIMC_master = {0};
  if(hdcmipp->Instance==DCMIPP)
  {
    /* USER CODE BEGIN DCMIPP_MspInit 0 */
    /*** Enable peripheral clock ***/
    /* Enable DCMIPP clock */
    __HAL_RCC_DCMIPP_CLK_ENABLE();

    __HAL_RCC_DCMIPP_FORCE_RESET();
    __HAL_RCC_DCMIPP_RELEASE_RESET();

    /*** Configure the NVIC for DCMIPP ***/
    /* NVIC configuration for DCMIPP transfer complete interrupt */
    HAL_NVIC_SetPriority(DCMIPP_IRQn, 0x07, 0);
    HAL_NVIC_EnableIRQ(DCMIPP_IRQn);

    /*** Enable peripheral clock ***/
    /* Enable CSI clock */
    __HAL_RCC_CSI_CLK_ENABLE();

    __HAL_RCC_CSI_FORCE_RESET();
    __HAL_RCC_CSI_RELEASE_RESET();

    /*** Configure the NVIC for CSI ***/
    /* NVIC configuration for CSI transfer complete interrupt */
    HAL_NVIC_SetPriority(CSI_IRQn, 0x07, 0);
    HAL_NVIC_EnableIRQ(CSI_IRQn);

    /* USER CODE END DCMIPP_MspInit 0 */
    /* USER CODE BEGIN DCMIPP_MspInit 1 */

    /* DCMIPP Clock Config */
    /* DCMIPP clock configuration */
    /* Typical PCLK is 333 MHz so the PLL1 is configured to provide this clock */
    /* Configure DCMIPP clock to IC17 with PLL1  */
    /* PLL1_VCO Input = HSI_VALUE/PLLM = 64 Mhz / 4 = 16 */
    /* PLL1_VCO Output = PLL3_VCO Input * PLLN = 16 Mhz * 75 = 1200 */
    /* PLLLCDCLK = PLL3_VCO Output/(PLLP1 * PLLP2) = 1200/4 = 300Mhz */
    /* DCMIPP clock frequency = PLLLCDCLK = 300 Mhz */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_DCMIPP;
    PeriphClkInitStruct.DcmippClockSelection = RCC_DCMIPPCLKSOURCE_IC17;
    PeriphClkInitStruct.ICSelection[RCC_IC17].ClockSelection = RCC_ICCLKSOURCE_PLL1;
    PeriphClkInitStruct.ICSelection[RCC_IC17].ClockDivider = 4;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CSI;
    PeriphClkInitStruct.ICSelection[RCC_IC18].ClockSelection = RCC_ICCLKSOURCE_PLL1;
    PeriphClkInitStruct.ICSelection[RCC_IC18].ClockDivider = 60;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* IMX335 Sensor HW Reset */
    /* Enable GPIO clocks */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitTypeDef gpio_init_structure = {0};

    /* Initialize camera NRST pin */
    gpio_init_structure.Pin       = GPIO_PIN_8;
    gpio_init_structure.Pull      = GPIO_NOPULL;
    gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
    gpio_init_structure.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &gpio_init_structure);

    /* Initialize camera EN pin */
    gpio_init_structure.Pin       = GPIO_PIN_2;
    gpio_init_structure.Pull      = GPIO_NOPULL;
    gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
    gpio_init_structure.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOD, &gpio_init_structure);

    /* Camera sensor Power-On sequence */
    /* Assert the camera Enable and NRST pins */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2, GPIO_PIN_RESET);
    HAL_Delay(200);   /* NRST and Enable signals asserted during 200ms */

    /* De-assert the camera STANDBY pin (active high) */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2, GPIO_PIN_SET);
    HAL_Delay(3);     /* NRST de-asserted during 3ms */

    /* SRAM3 and SRAM4 memories clock enable */
    LL_MEM_EnableClock(LL_MEM_AXISRAM3);
    LL_MEM_EnableClock(LL_MEM_AXISRAM4);

    /* Power On AXSRAM3 and AXISRAM4 */
    hramcfg.Instance = RAMCFG_SRAM3_AXI;
    HAL_RAMCFG_EnableAXISRAM(&hramcfg);

    hramcfg.Instance = RAMCFG_SRAM4_AXI;
    HAL_RAMCFG_EnableAXISRAM(&hramcfg);

    __HAL_RCC_RIFSC_CLK_ENABLE();

    RIMC_master.MasterCID = RIF_CID_1;
    RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;

    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &RIMC_master);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
    /* USER CODE END DCMIPP_MspInit 1 */
  }

}

/**
  * @brief  Initialize the BSP LTDC Msp.
  * @param  hltdc  LTDC handle
  * @retval None
  */
void HAL_LTDC_MspInit(LTDC_HandleTypeDef *hltdc)
{
  GPIO_InitTypeDef  gpio_init_structure = {0};
  RCC_PeriphCLKInitTypeDef RCC_PeriphCLKInitStruct = {0};
  RIMC_MasterConfig_t RIMC_master = {0};

  if (hltdc->Instance == LTDC)
  {
    __HAL_RCC_LTDC_CLK_ENABLE();

    __HAL_RCC_LTDC_FORCE_RESET();
    __HAL_RCC_LTDC_RELEASE_RESET();

    /* STM32N6570-DK MB1860A RK050HR18 */
    /* PG0  R0          */
    /* PD9  R1          */
    /* PD15 R2          */
    /* PB4  R3          */
    /* PH4  R4          */
    /* PA15 R5          */
    /* PG11 R6          */
    /* PD8  R7          */
    /* PG12 G0          */
    /* PG1  G1          */
    /* PA1  G2          */
    /* PA0  G3          */
    /* PB15 G4          */
    /* PB12 G5          */
    /* PB11 G6          */
    /* PG8  G7          */
    /* P15  B0          */
    /* PA7  B1          */
    /* PB2  B2          */
    /* PG6  B3          */
    /* PH3  B4          */
    /* PH6  B5          */
    /* PA8  B6          */
    /* PA2  B7          */
    /*                  */
    /* PG13 LCD_DE      */
    /* PQ3  LCD_ONOFF   */
    /* PB14 LCD_HSYNC   */
    /* PE11 PCD_VSYNC   */
    /* PB13 LCD_CLK     */
    /* PQ4  LCD_INT     */
    /* PQ6  LCD_BL_CTRL */
    /* PE1  NRST        */

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOQ_CLK_ENABLE();

    gpio_init_structure.Mode      = GPIO_MODE_AF_PP;
    gpio_init_structure.Pull      = GPIO_NOPULL;
    gpio_init_structure.Speed     = GPIO_SPEED_FREQ_HIGH;

    /* G3, G2, B7, B1, B6, R5 */
    gpio_init_structure.Pin       = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_15;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOA, &gpio_init_structure);

    /* LCD_CLK, LCD_HSYNC B2, R3, G6, G5, G4 */
    gpio_init_structure.Pin       = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_15;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOB, &gpio_init_structure);

    /* R7, R1, R2 */
    gpio_init_structure.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_15;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOD, &gpio_init_structure);

    /* LCD_VSYNC */
    gpio_init_structure.Pin       = GPIO_PIN_11;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOE, &gpio_init_structure);

    /* R0, G1, B3, G7, R6, G0 */
    gpio_init_structure.Pin       = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_6 | GPIO_PIN_8 | GPIO_PIN_11 | GPIO_PIN_12 ;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOG, &gpio_init_structure);

    /* B4, R4, B5 */
    gpio_init_structure.Pin       = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_6;
    gpio_init_structure.Alternate = GPIO_AF14_LCD;
    HAL_GPIO_Init(GPIOH, &gpio_init_structure);

    /* NRST */
    gpio_init_structure.Pin       = GPIO_PIN_1;
    gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOE, &gpio_init_structure);

    /* LCD_ONOFF, LCD_BL_CTRL */
    gpio_init_structure.Pin       = GPIO_PIN_3 | GPIO_PIN_6;
    gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOQ, &gpio_init_structure);

    /* LCD_DE */
    gpio_init_structure.Pin       = GPIO_PIN_13;
    gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOG, &gpio_init_structure);

    HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_3, GPIO_PIN_SET); /* LCD On */ /* PQ3  LCD_ONOFF   */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_SET); /* Display Enable */ /* PG13 LCD_DE      */
    HAL_GPIO_WritePin(GPIOQ, GPIO_PIN_6, GPIO_PIN_SET); /* 100% Brightness */ /* PQ6  LCD_BL_CTRL */

    /* LCD clock configuration */
    /* Typical PCLK is 25 MHz so the PLL1 is configured to provide this clock */
    /* LTDC - PLL1 */
    /* Configure LTDC clock to IC16 with PLL1  */

    /* LCD clock configuration */
    /* Typical PCLK is 25 MHz so the PLL1 is configured to provide this clock */
    /* LCD clock configuration */
    /* PLL1_VCO Input = HSI_VALUE/PLLM = 64 Mhz / 4 = 16 */
    /* PLL1_VCO Output = PLL3_VCO Input * PLLN = 16 Mhz * 75 = 1200 */
    /* PLLLCDCLK = PLL3_VCO Output/(PLLP1 * PLLP2) = 1200/48 = 25Mhz */
    /* LTDC clock frequency = PLLLCDCLK = 25 Mhz */
    RCC_PeriphCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
    RCC_PeriphCLKInitStruct.LtdcClockSelection = RCC_LTDCCLKSOURCE_IC16;
    RCC_PeriphCLKInitStruct.ICSelection[RCC_IC16].ClockSelection = RCC_ICCLKSOURCE_PLL1;
    RCC_PeriphCLKInitStruct.ICSelection[RCC_IC16].ClockDivider = 48;
    if (HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    RIMC_master.MasterCID = RIF_CID_1;
    RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;

    HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC1 , &RIMC_master);
    HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL1 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  }
}

/**
* @brief DCMIPP MSP De-Initialization
* This function freeze the hardware resources used in this example
* @param hdcmipp: DCMIPP handle pointer
* @retval None
*/
void HAL_DCMIPP_MspDeInit(DCMIPP_HandleTypeDef* hdcmipp)
{
  if(hdcmipp->Instance==DCMIPP)
  {
    /* USER CODE BEGIN DCMIPP_MspDeInit 0 */
    __HAL_RCC_DCMIPP_FORCE_RESET();
    __HAL_RCC_DCMIPP_RELEASE_RESET();

    /* Disable NVIC  for DCMIPP transfer complete interrupt */
    HAL_NVIC_DisableIRQ(DCMIPP_IRQn);

    /* Disable DCMIPP clock */
    __HAL_RCC_DCMIPP_CLK_DISABLE();

    __HAL_RCC_CSI_FORCE_RESET();
    __HAL_RCC_CSI_RELEASE_RESET();

    /* Disable NVIC  for DCMIPP transfer complete interrupt */
    HAL_NVIC_DisableIRQ(CSI_IRQn);

    /* Disable DCMIPP clock */
    __HAL_RCC_CSI_CLK_DISABLE();
    /* USER CODE END DCMIPP_MspDeInit 0 */

    /* USER CODE BEGIN DCMIPP_MspDeInit 1 */

    /* USER CODE END DCMIPP_MspDeInit 1 */
  }

}

/* USER CODE BEGIN 1 */

/**
* @brief DMA2D MSP Initialization
* This function configures the hardware resources used in this example
* @param hdma2d: DMA2D handle pointer
* @retval None
*/
void HAL_DMA2D_MspInit(DMA2D_HandleTypeDef* hdma2d)
{
  if(hdma2d->Instance==DMA2D)
  {
    /* Peripheral clock enable */
    __HAL_RCC_DMA2D_CLK_ENABLE();
  }
}

/**
* @brief DMA2D MSP De-Initialization
* This function freeze the hardware resources used in this example
* @param hdma2d: DMA2D handle pointer
* @retval None
*/
void HAL_DMA2D_MspDeInit(DMA2D_HandleTypeDef* hdma2d)
{
  if(hdma2d->Instance==DMA2D)
  {
    /* Peripheral clock disable */
    __HAL_RCC_DMA2D_CLK_DISABLE();
  }
}

/* USER CODE END 1 */

/**
  * @brief SD MSP Initialization
  * This function configures the hardware resources used in this example
  * @param hsd: SD handle pointer
  * @retval None
  */
void HAL_SD_MspInit(SD_HandleTypeDef* hsd)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(hsd->Instance==SDMMC2)
  {
    /* SDMMC2 activation and security configuration (RIFSC) */
    uint32_t RIFSCAddress2 = 0x54024C1C; /* (RIFSC_RIMC_ATTRx) with x = 4 (0xC:Master index of SDMMC2) */
    uint32_t data;
    
    /* set MPRIV, MSEC and MCID=001 (same CID as CPU) */
    *(uint32_t*)RIFSCAddress2 = 0x00000310;
    /* readback data */
    data = *(uint32_t*)RIFSCAddress2;
    (void)data;
    __HAL_RCC_SDMMC2_FORCE_RESET();
    __HAL_RCC_SDMMC2_RELEASE_RESET();
    
    /* Lines below are useful only for DMA transfer */
    /* SDMMC2 Secure Guard RISUP Index is 54 */
    RIFSCAddress2 = 0x54024014; /* RIFSC_RISC_SECCFGRx with x = 1 */
    data = *(uint32_t*)RIFSCAddress2;
    *(uint32_t*)RIFSCAddress2 = data | 0x00400000U;
    /* readback data */
    data = *(uint32_t*)RIFSCAddress2;
    (void)data;

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SDMMC2;
    PeriphClkInitStruct.Sdmmc2ClockSelection = RCC_SDMMC2CLKSOURCE_HCLK;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* Peripheral clock enable */
    __HAL_RCC_SDMMC2_CLK_ENABLE();

    /* Enable VDDIO5 for SDMMC GPIOs */
    HAL_PWREx_EnableVddIO5();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    /**SDMMC2 GPIO Configuration
    PC4     ------> SDMMC2_D0
    PC5     ------> SDMMC2_D1
    PC0     ------> SDMMC2_D2
    PC2     ------> SDMMC2_CK
    PE4     ------> SDMMC2_D3
    PC3     ------> SDMMC2_CMD
    */
    GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC2;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_SDMMC2;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
  }
}

/**
  * @brief SD MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param hsd: SD handle pointer
  * @retval None
  */
void HAL_SD_MspDeInit(SD_HandleTypeDef* hsd)
{
  if(hsd->Instance==SDMMC2)
  {
    /* Peripheral clock disable */
    __HAL_RCC_SDMMC2_CLK_DISABLE();

    /**SDMMC2 GPIO Configuration
    PC4     ------> SDMMC2_D0
    PC5     ------> SDMMC2_D1
    PC0     ------> SDMMC2_D2
    PC2     ------> SDMMC2_CK
    PE4     ------> SDMMC2_D3
    PC3     ------> SDMMC2_CMD
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_0|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5);
    HAL_GPIO_DeInit(GPIOE, GPIO_PIN_4);
  }
}

/**
  * @brief UART MSP Initialization
  * This function configures the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(huart->Instance==USART1)
  {
  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    PeriphClkInitStruct.Usart1ClockSelection = RCC_USART1CLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* Peripheral clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOE_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PE5     ------> USART1_TX
    PE6     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
  }
}

/**
  * @brief UART MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance==USART1)
  {
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PE5     ------> USART1_TX
    PE6     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOE, GPIO_PIN_5|GPIO_PIN_6);
  }
}
