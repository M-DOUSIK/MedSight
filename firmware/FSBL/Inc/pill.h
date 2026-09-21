/**
  ******************************************************************************
  * @file    pill.h
  * @author  STEdgeAI
  * @date    2026-09-11 00:46:13
  * @brief   Minimal description of the generated c-implemention of the network
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
#ifndef LL_ATON_PILL_H
#define LL_ATON_PILL_H

/******************************************************************************/
#define LL_ATON_PILL_C_MODEL_NAME        "pill"
#define LL_ATON_PILL_ORIGIN_MODEL_NAME   "pill_cut_int8_160_hn"

/************************** USER ALLOCATED IOs ********************************/
// No user allocated inputs
// No user allocated outputs

/************************** INPUTS ********************************************/
#define LL_ATON_PILL_IN_NUM        (1)    // Total number of input buffers
// Input buffer 1 -- Input_0_out_0
#define LL_ATON_PILL_IN_1_ALIGNMENT   (32)
#define LL_ATON_PILL_IN_1_SIZE_BYTES  (76800)

/************************** OUTPUTS *******************************************/
#define LL_ATON_PILL_OUT_NUM        (6)    // Total number of output buffers
// Output buffer 1 -- Quantize_477_out_0
#define LL_ATON_PILL_OUT_1_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_1_SIZE_BYTES  (25600)
// Output buffer 2 -- Quantize_495_out_0
#define LL_ATON_PILL_OUT_2_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_2_SIZE_BYTES  (400)
// Output buffer 3 -- Quantize_441_out_0
#define LL_ATON_PILL_OUT_3_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_3_SIZE_BYTES  (6400)
// Output buffer 4 -- Quantize_459_out_0
#define LL_ATON_PILL_OUT_4_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_4_SIZE_BYTES  (100)
// Output buffer 5 -- Quantize_405_out_0
#define LL_ATON_PILL_OUT_5_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_5_SIZE_BYTES  (1600)
// Output buffer 6 -- Quantize_423_out_0
#define LL_ATON_PILL_OUT_6_ALIGNMENT   (32)
#define LL_ATON_PILL_OUT_6_SIZE_BYTES  (25)

#endif /* LL_ATON_PILL_H */
