# Licensing

## Is this open source?

**Yes.** MedSight's own code, documentation and CAD are **Apache-2.0**. Read,
build, change, redistribute and sell, keeping the copyright notice and stating
what was changed.

## The repository is multi-licensed

Each component carries the licence its own rights holder grants. `LICENSE` at
the repository root says so and points here.

| What | Licence | Open source |
|---|---|---|
| MedSight firmware, documentation, CAD, tools | Apache-2.0 | Yes |
| Pill detector weights and training pipeline | AGPL-3.0 | Yes |
| µT-Kernel 3.0 BSP2 | T-License 2.1 / 2.2 | Yes |
| CMSIS, STM32N6xx device layer | Apache-2.0 | Yes |
| STM32N6xx HAL/LL, board support package | BSD-3-Clause | Yes |
| MediaPipe hand landmark model | Apache-2.0 | Yes |
| FatFs R0.15 | FatFs licence | Yes |
| DejaVu Sans glyph data | Bitstream Vera / DejaVu | Yes |
| **ST Edge AI runtime, ISP library, the two face models** | **ST SLA0044** | **No** |

**SLA0044 is the only component that is not open source.** It is ST's code for
driving their own accelerator and camera, free to redistribute for use on ST
devices, and every STM32 AI project carries it.

## The pill detector and AGPL-3.0

The detector's weights were trained by this project with Ultralytics YOLOv8,
which is AGPL-3.0, and Ultralytics states that models trained with it are
AGPL-3.0 too.

**This project complies with that.** The complete corresponding source is
published in `tools/pill_detector/`: the training script, the dataset identity
and licence, the trained weights, the exported model before and after the head
was cut, and the accelerator build configuration. The network clause never
applies, because the device has no network.

**One incompatibility is real.** AGPL-3.0 and ST's SLA0044 cannot both govern a
single distributed binary, so no linked binary is distributed: the submission
conveys source and the recipient builds it. Setting
`MEDSIGHT_ACTION_RECOGNITION` to 0 is an AGPL-free configuration, verified by
symbol count.

## Per-file map

| Path | Licence |
|---|---|
| `firmware/FSBL/Src/`, `firmware/FSBL/Inc/`, except the AI files below | Apache-2.0 |
| `firmware/FSBL/Src/ai/ai_vision.c`, `intake*.c`, `npu_init.c` | Apache-2.0 |
| `firmware/FSBL/Src/ai/pill.c`, `stai_pill.c`, and the pill weight blob | AGPL-3.0 |
| `firmware/FSBL/Src/ai/hand.c`, `stai_hand.c`, and the hand weight blob | Apache-2.0 |
| `firmware/FSBL/Src/ai/fd.c`, `faceid.c`, `stai_fd.c`, `stai_faceid.c`, and their weight blobs | ST SLA0044 |
| `firmware/FSBL/Src/ai/ll_aton*.c` and the runtime archive | ST SLA0044 |
| `firmware/FSBL/mtk3_bsp2/` | T-License 2.1 / 2.2 |
| `firmware/Drivers/CMSIS/` | Apache-2.0 |
| `firmware/Drivers/STM32N6xx_HAL_Driver/`, `firmware/Drivers/BSP/` | BSD-3-Clause |
| `firmware/Middlewares/ST/STM32_ISP_Library/` | ST SLA0044 |
| `firmware/Middlewares/Third_Party/FatFs/` | FatFs licence |
| `hardware/cad/`, `docs/`, the site, `tools/` except the pill detector | Apache-2.0 |
| `tools/pill_detector/` | AGPL-3.0 |

**Not in this repository:** ST's prebuilt first-stage bootloader, used only for
boot from external flash. It is ST's binary, downloaded from ST directly; the
handbook gives the link and a fingerprint.
