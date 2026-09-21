/**
  ******************************************************************************
  * @file    hand.h
  * @author  STEdgeAI
  * @date    2026-09-11 00:16:26
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
#ifndef LL_ATON_HAND_H
#define LL_ATON_HAND_H

/******************************************************************************/
#define LL_ATON_HAND_C_MODEL_NAME        "hand"
#define LL_ATON_HAND_ORIGIN_MODEL_NAME   "handlandmarks_full_224_int8"

/************************** USER ALLOCATED IOs ********************************/
// No user allocated inputs
// No user allocated outputs

/************************** INPUTS ********************************************/
#define LL_ATON_HAND_IN_NUM        (1)    // Total number of input buffers
// Input buffer 1 -- Input_0_out_0
#define LL_ATON_HAND_IN_1_ALIGNMENT   (32)
#define LL_ATON_HAND_IN_1_SIZE_BYTES  (150528)

/************************** OUTPUTS *******************************************/
#define LL_ATON_HAND_OUT_NUM        (4)    // Total number of output buffers
// Output buffer 1 -- Dequantize_227_out_0
#define LL_ATON_HAND_OUT_1_ALIGNMENT   (32)
#define LL_ATON_HAND_OUT_1_SIZE_BYTES  (4)
// Output buffer 2 -- Dequantize_233_out_0
#define LL_ATON_HAND_OUT_2_ALIGNMENT   (32)
#define LL_ATON_HAND_OUT_2_SIZE_BYTES  (252)
// Output buffer 3 -- Dequantize_221_out_0
#define LL_ATON_HAND_OUT_3_ALIGNMENT   (32)
#define LL_ATON_HAND_OUT_3_SIZE_BYTES  (4)
// Output buffer 4 -- Dequantize_230_out_0
#define LL_ATON_HAND_OUT_4_ALIGNMENT   (32)
#define LL_ATON_HAND_OUT_4_SIZE_BYTES  (252)

#endif /* LL_ATON_HAND_H */
