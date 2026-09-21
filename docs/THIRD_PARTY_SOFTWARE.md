# Third-party software

MedSight, entry 54916.

---

## 1. Summary

| Layer | Component | Rights holder | Licence |
|---|---|---|---|
| RTOS | µT-Kernel 3.0 BSP2 | TRON Forum / Ken Sakamura | T-License 2.1 / 2.2 |
| CPU support | CMSIS Core (Cortex-M55) | Arm Limited | Apache-2.0 |
| Device support | STM32N6xx CMSIS Device | Arm Limited, STMicroelectronics | Apache-2.0 |
| Peripheral drivers | STM32N6xx HAL / LL | STMicroelectronics | BSD-3-Clause |
| Board support | STM32N6570-DK BSP and components | STMicroelectronics | BSD-3-Clause |
| Camera ISP | STM32 ISP Library, eVision AE/AWB | LACROIX-Impulse, STMicroelectronics | ST SLA0044 |
| NPU runtime | ST Edge AI runtime 1.1.3-262 | STMicroelectronics | ST SLA0044 |
| AI model | CenterFace face detector | STMicroelectronics | ST SLA0044 |
| AI model | MobileFaceNet face embedder | STMicroelectronics | ST SLA0044 |
| AI model | MediaPipe hand landmarks | Google LLC | Apache-2.0 |
| AI model | Pill detector architecture and training framework, YOLOv8n | Ultralytics Inc. | AGPL-3.0 |
| Training data | "pills" dataset, Roboflow Universe | Roboflow contributors | CC BY 4.0 |
| Filesystem | FatFs R0.15 | ChaN | FatFs licence, BSD-style |
| UI font | DejaVu Sans, rasterised into the UI assets | DejaVu authors, Bitstream Inc. | Bitstream Vera / DejaVu |
| C library | newlib-nano, GNU Tools for STM32 14.3.rel1 | FSF, Red Hat and contributors | GPLv3 with Runtime Exception; BSD-style |

**Everything else in the firmware is original MedSight code.** That includes the
OS abstraction layer, the 22-screen user interface and its design system, the
state machine, carer mode, the scheduler and time source, the SD logger and the
block-device layer beneath FatFs, the dispenser driver, the buzzer and alert
task, the accelerator bring-up, and all the vision code around the face
networks: the frame snapshot, the detection box decode, normalisation, 8-bit
quantisation of the face fingerprint, cosine matching against the gallery, and
persistence to the card.

**The pill detector's trained weights were produced by this project.** The
architecture and the framework that trained them are Ultralytics'; the weights
are ours.

**The dispensing mechanism is not third-party.** The printed enclosure, the
turntable and its chute are original designs by this team, and the dispenser
firmware is original code written directly against ST's GPIO driver.

---

## 2. Detail

### 2.1 µT-Kernel 3.0 BSP2

| | |
|---|---|
| **Name** | µT-Kernel 3.0 BSP2, versions 3.00.00 to 3.00.07 across the tree |
| **Rights holder** | TRON Forum; Copyright (C) 2006-2024 by Ken Sakamura |
| **Licence** | T-License 2.1 and 2.2; per-file headers state which |
| **Acquisition** | Cloned from the TRON Forum's public repository at github.com/tron-forum/mtk3bsp2_samples, specifically the `prj_stm32n6_cam` example, which already targets this board. The whole kernel tree was vendored into the firmware unmodified, together with that project's own build configuration |
| **Function** | The real-time kernel: tasks, message buffers, mutexes, event flags, cyclic handlers, alarms, delays and the dispatcher |
| **In the build** | About 234 of the 321 linked objects. The whole tree compiles, including board variants this project never uses; those are guarded end to end and compile to empty translation units |
| **Modified** | Yes, six files. See section 4.1 |

### 2.2 CMSIS and STM32N6xx CMSIS Device

| | |
|---|---|
| **Name** | CMSIS Core for Cortex-M55; STM32N6xx device layer, startup code and vector table |
| **Rights holder** | Arm Limited; Arm Limited and STMicroelectronics |
| **Licence** | Apache-2.0 |
| **Acquisition** | Shipped inside the STM32Cube FW_N6 package, obtained as part of the ST example project this firmware was founded on |
| **Function** | Core definitions and intrinsics, reset and startup code, the interrupt vector table, clock scaffolding. The power-saving code uses the CMSIS sleep and barrier intrinsics and the cycle-counter definitions directly |
| **Modified** | No |

### 2.3 STM32N6xx HAL and LL drivers

| | |
|---|---|
| **Name** | STM32N6xx HAL Driver, 87 source modules |
| **Rights holder** | STMicroelectronics |
| **Licence** | BSD-3-Clause |
| **Acquisition** | STM32Cube FW_N6 package |
| **Function** | All peripheral access: the camera pipeline, display controller, 2D blitter, SD card, touch bus, debug console, real-time clock, external flash, clocks and power domains |
| **Modified** | No |

### 2.4 STM32N6570-DK board support package

| | |
|---|---|
| **Name** | STM32N6570-DK BSP, plus the components `imx335` camera, `gt911` touch, `rk050hr18` display, `mx66uw1g45g` external NOR flash, `aps256xx` PSRAM |
| **Rights holder** | STMicroelectronics |
| **Licence** | BSD-3-Clause |
| **Acquisition** | STM32Cube FW_N6 package |
| **Function** | Board-level initialisation and device drivers |
| **Modified** | Yes, one file. See section 4.2 |

### 2.5 STM32 ISP Library and eVision AE/AWB

| | |
|---|---|
| **Name** | The ISP library sources, plus two binary archives for auto-exposure and auto-white-balance |
| **Rights holder** | LACROIX-Impulse and STMicroelectronics |
| **Licence** | ST SLA0044 |
| **Acquisition** | STM32Cube FW_N6 package |
| **Function** | Runs the camera's auto-exposure and auto-white-balance loops against the image pipeline's hardware statistics |
| **Modified** | No |

### 2.6 ST Edge AI runtime

| | |
|---|---|
| **Name** | ST Edge AI runtime, version 1.1.3-262, plus a precompiled archive for Cortex-M55 |
| **Rights holder** | STMicroelectronics |
| **Licence** | ST SLA0044 |
| **Acquisition** | X-CUBE-AI / ST Edge AI Core, obtained as part of ST's `x-cube-n6-ai-h264-usb-uvc` application package |
| **Function** | Drives the Neural-ART accelerator: microcode loading, buffer and cache management, and the synchronous execution path |
| **Modified** | No |

### 2.7 The two face models

| | |
|---|---|
| **Name** | **CenterFace** face detector and **MobileFaceNet** face embedder, both INT8, compiled to Neural-ART by ST's tooling |
| **Rights holder** | STMicroelectronics |
| **Licence** | ST SLA0044 |
| **Acquisition** | Copied from the model directory of ST's `x-cube-n6-ai-h264-usb-uvc` application, together with its prebuilt weight blobs |
| **Function** | CenterFace locates a face in the camera frame and emits five landmarks including both mouth corners; MobileFaceNet turns the face crop into a 128-dimension fingerprint |
| **Modified** | The generated sources were edited in two mechanical ways only: internal helper functions were made static to resolve duplicate symbols when both models link together, and the large weight arrays were tagged into a section that places them in external flash, which the accelerator can reach. No weights or graph structure were altered |

### 2.8 MediaPipe hand landmark model

| | |
|---|---|
| **Name** | MediaPipe hand landmarks, 224x224, INT8, 21 keypoints |
| **Rights holder** | Google LLC and the MediaPipe Authors |
| **Licence** | **Apache-2.0** |
| **Acquisition** | ST model zoo, which is itself a conversion of Google's MediaPipe hand tracking model |
| **Function** | Decides whether a hand reached the patient's mouth during the confirmation window |
| **Our work on it** | We selected it, quantised it, compiled it to Neural-ART and integrated it. **We did not train it.** Apache-2.0 section 4 requires this attribution to travel with any redistribution, and it does |
| **Known limitation** | In MediaPipe this model never sees a whole scene: a palm detector runs first and hands it a tight crop. We do not ship that palm detector, so the model reports hand presence reliably when the hand is centred and large in its input, which is the case this device needs, and does not localise a hand elsewhere in the frame |

### 2.9 Pill detector

| | |
|---|---|
| **Name** | Single-class pill detector. YOLOv8n architecture, 160x160, INT8, detection head removed |
| **Rights holder** | Architecture and training framework: Ultralytics Inc. **Trained weights: produced by this project.** Generated accelerator sources: STMicroelectronics |
| **Licence** | **AGPL-3.0.** See `LICENSING.md` |
| **Training data** | "pills" dataset, Roboflow Universe, 451 images, **CC BY 4.0**. Attribution: *"pills" dataset, Roboflow Universe, CC BY 4.0* |
| **Acquisition** | Trained by this project. `tools/pill_detector/build_pill_detector.py` reproduces the whole path: dataset fetch, conversion, training, ONNX export, head removal, INT8 quantisation, validation, and accelerator code generation |
| **Function** | Locates a pill in a mouth-centred region of interest, corroborating that a pill was present when a hand reached the mouth |
| **Modified** | The exported graph is cut after the six raw head convolutions and the decode runs on the CPU instead. No weights or graph structure were otherwise altered |

### 2.10 FatFs

| | |
|---|---|
| **Name** | FatFs, generic FAT filesystem module, R0.15 |
| **Rights holder** | ChaN |
| **Licence** | The FatFs licence, a one-clause BSD-style permissive licence. Full text is in the source tree |
| **Acquisition** | Downloaded from the official FatFs site and integrated by hand |
| **Function** | FAT32 filesystem on the microSD card: the event log, the patient gallery, the carer passcode hash |
| **Modified** | No. The block-device layer beneath it is MedSight's own code, not part of FatFs |

### 2.11 Toolchain and C library

| | |
|---|---|
| **Name** | GNU Tools for STM32 14.3.rel1 with newlib-nano |
| **Rights holder** | Free Software Foundation; Red Hat and newlib contributors; packaging by STMicroelectronics |
| **Licence** | GCC under GPLv3 with the Runtime Library Exception; newlib under BSD-style licences. **Neither imposes any licence condition on this firmware** |
| **Acquisition** | Bundled with STM32CubeIDE |
| **Function** | Compiles and links the firmware |

---

## 3. Provision to the organizer

Every component above is publicly and freely obtainable, and none requires a
fee, a registration or an agreement to obtain or to evaluate.

| Component | Availability |
|---|---|
| µT-Kernel 3.0 BSP2 | Public TRON Forum repository, **and vendored into this submission**, so no separate fetch is needed |
| CMSIS, HAL/LL, board support, ISP library, eVision archives | Free from STMicroelectronics. The specific files used are in this submission |
| ST Edge AI runtime and the face models | Free from STMicroelectronics. Generated sources are in the tree; weight blobs are in `weights/` |
| MediaPipe hand landmark model | Free, Apache-2.0. Weight blob in `weights/` |
| Pill detector | Produced by this project. Weights, training script and dataset identity all published |
| FatFs | Free from elm-chan.org. **In the tree** |
| GNU Tools for STM32, newlib | Free with STM32CubeIDE |

**One dependency is not redistributed here.** Boot from external flash uses ST's prebuilt first-stage bootloader
`ai_fsbl.hex`, from ST's X-CUBE-N6-AI-POWER-MEASUREMENT package v1.4.0. It is
ST's binary, not ours, so this project does not redistribute it; the handbook
gives the download and a SHA-256 fingerprint. **Evaluating this program over USB
does not require it**, and the device ships already flashed.

These will remain available to the organizer until at least one week after the
awards ceremony.

---

## 4. Modifications

### 4.1 µT-Kernel: six files

Six files differ from upstream, found by full recursive comparison against the
pristine tree. **Every other file, roughly 230 of them including the entire
kernel directory, is byte-identical.**

| File | Change | Why |
|---|---|---|
| `config/config.h` | System area end set explicitly | The default resolves to the end of the whole physical RAM bank rather than this project's linker region, which let the kernel heap hand a task stack memory overlapping program code |
| `config/config.h` | Timer period 10 ms to 1 ms | At 10 ms the tick bridge made the millisecond counter run ten times slow rather than ten times coarse, stretching every interface deadline tenfold. 1 ms is inside this port's own declared range |
| `config/config.h` | Debug monitor disabled | It reprograms the UART by hardcoded address with a baud divisor for a different clock tree, silently taking over the console this project has used since bring-up |
| `sysdef.h` | Interrupt vector count 196 to 195 | This project's startup file has exactly 195 vectors, verified by measuring the vector table in the linked image. At 196 the kernel's vector copy read one word past the end |
| `sys_start.c` | An 8 KB heap reserve, and a cache clean after the vector table is relocated | The C library and the kernel allocator both started allocating at the same linker symbol with no coordination. Separately, the kernel builds its vector table at run time in cacheable memory with no cache maintenance, because its own reference project runs with caches disabled; this one does not |
| `interrupt.c` | Cache clean after the handler table is written; default handler prints through this project's console | The same cache fix. The print change follows from disabling the debug monitor, which otherwise made an unhandled exception completely silent |
| `exc_hdr.c` | Fault handlers print unconditionally, with register dumps | Fault reporting was gated on the debug monitor, so disabling it turned every fault handler into a silent infinite loop |
| `power_save.c` | The idle hook forwards to this project's own low-power idle | Upstream ships it empty, so the dispatcher spun the processor at full clock whenever no task was runnable, which in this application is most of the time |

### 4.2 One board support file

The external-flash driver in the board support package, BSD-3-Clause, already
inventoried above. Two changes, both bug fixes needed for boot from external
flash:

1. Before issuing its reset sequence, the function now asks the flash chip for
   its status register in Octal-DTR mode. If the chip answers, its current mode
   is adopted rather than reset. ST's own external loader leaves the chip in
   Octal-DTR, and that is a property of the chip rather than the controller, so
   it survives any reset on the processor side. Without this, boot from external
   flash fails.
2. The reset branch had no post-reset delay at all. It now waits the interval
   the component driver already defines.

No API was changed and every caller is unaffected.

### 4.3 The OS API specification was not changed

**No µT-Kernel API specification was changed by this project.**

- Not one kernel function's signature was altered: not its name, parameters,
  return type or error codes. The entire kernel directory, every file that
  implements a system call, is byte-identical to upstream, verified by recursive
  comparison.
- Not one kernel call's semantics were altered. The changes above are confined
  to build-time configuration constants, the boot and interrupt plumbing, fault
  reporting, and the board's own power-management hook. **None of those files
  defines a system call.**
- The application reaches the kernel only through this project's own OS
  abstraction layer. **No kernel call and no kernel header include exists
  anywhere else in the application source**, verified by search.

---

## 5. Rights guarantee

The author warrants that the copyrights and other rights in all software used in
this submission have been handled in accordance with the TRON Programming
Contest Application Rules. Specifically:

- Every third-party component listed in section 2 is used under the licence its
  own rights holder grants, as identified there, and within the scope those
  terms permit.
- No component was obtained by circumventing a licence, and none requires a fee,
  registration or agreement to obtain or to evaluate.
- The µT-Kernel modifications in section 4 are individually documented
  with their reasons and leave the OS API specification unchanged.
- All original MedSight code is this team's own work.
- **No copyrighted or trademarked character, artwork or other third-party
  creative asset appears anywhere** in the firmware, the user interface, the
  asset filenames or the documentation. The on-screen mascot and every interface
  element are original designs by this team's designer.

---
