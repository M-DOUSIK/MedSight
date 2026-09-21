# NPU weights - what to flash, where, and why the firmware is useless without it

Everything in this directory is **NPU model data that the firmware build does
not contain**. The linker marks the external NOR regions `(NOLOAD)` on purpose,
so building and flashing the application never writes any of it.

That means a fresh clone can compile a perfectly good `.elf`, flash it, and get
a device whose face recognition and action recognition both produce garbage -
with nothing in any log to say why. **Flash these once per board.**

## The five images

| file | address | bytes | what it is |
|---|---|---|---|
| `fd_data.xSPI2.bin` | `0x70380000` | 2,030,881 | CenterFace - face detector constant pools |
| `ec_blobs.xSPI2.bin` | `0x71000000` | 296,832 | NPU epoch-controller microcode, **both** face networks |
| `faceid_data.xSPI2.bin` | `0x72000000` | 1,092,129 | MobileFaceNet - face embedder constant pools |
| `hand_atonbuf.xSPI2.bin` | `0x73000000` | 3,221,233 | MediaPipe hand landmarks, decides a dose |
| `pill_atonbuf.xSPI2.bin` | `0x73400000` | 3,049,169 | YOLOv8n pill detector, corroboration only |

SHA-256 prefixes, so a mis-copied file is caught before it becomes a debugging
session:

```
8076b74352f3c323  ec_blobs.xSPI2.bin
68a41c8a030afde3  faceid_data.xSPI2.bin
44f72b41a1f43183  fd_data.xSPI2.bin
a0cb24315860ddae  hand_atonbuf.xSPI2.bin
e1359bb6add1268f  pill_atonbuf.xSPI2.bin
```

## Flashing

```powershell
$cli    = "C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\STM32_Programmer_CLI.exe"
$loader = "C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr"

& $cli -c port=SWD mode=UR -el $loader -w "weights\fd_data.xSPI2.bin"        0x70380000
& $cli -c port=SWD mode=UR -el $loader -w "weights\ec_blobs.xSPI2.bin"       0x71000000
& $cli -c port=SWD mode=UR -el $loader -w "weights\faceid_data.xSPI2.bin"    0x72000000
& $cli -c port=SWD mode=UR -el $loader -w "weights\hand_atonbuf.xSPI2.bin"   0x73000000
& $cli -c port=SWD mode=UR -el $loader -w "weights\pill_atonbuf.xSPI2.bin"   0x73400000
```

### `mode=UR`, not `mode=HOTPLUG`

This page said `mode=HOTPLUG` for two years, and it was right for two years:
the only writes happened before any application existed on the board, and
HOTPLUG avoids holding the MCU in reset while the external loader talks to the
flash chip.

Once a bootable MedSight image went into external flash, that changed. HOTPLUG needs a live, responsive core to attach to - and with
MedSight **running**, the erase fails outright, because the application has
XSPI2 memory-mapped for these very weights while the loader wants the same bus
in indirect mode.

**Use `mode=UR` for writes to external flash.** It holds the target in reset,
so it does not care what the application was doing. The commands above have
been updated.

### Then POWER-CYCLE. Unplug the USB cable.

Not a reset, not the IDE's stop button - the cable, out, for a few seconds.

Back-to-back external-loader operations leave the flash chip's live bus state
confused, and the symptom is a boot that hangs where XSPI is first touched:
the log stops right after `ai_vision_init: DEBUG build (-O0).`, before
`HAL_CACHEAXI_Enable`, and the LCD holds a stale frame. The flash *contents*
are fine; only the peripheral's bus state needs clearing.

**This applies to reads as well as writes.** A `-r32` verification read leaves
the bus in the same state a write does, and a read is exactly when it is most
tempting to leave the board running.

### Verifying

```powershell
& $cli -c port=SWD mode=UR -el $loader -r32 0x73000000 2
```

Compare against the file's own first words, then power-cycle again:

```bash
python -c "import struct;print('0x%08X 0x%08X' % struct.unpack('<II', open('weights/hand_atonbuf.xSPI2.bin','rb').read(8)))"
```

`0x73000000` should read `0xB317E9A4`, `0x73400000` should read `0xF0010DF7`.

## Confirming it worked

A correct boot says, in this order:

```
ai_vision_init: face detector + embedder ready.
ARENA SELFTEST PASS: 225280 bytes writable and readable
intake: hand landmark model ready (MediaPipe 224x224 INT8, 21 keypoints, ...)
intake: pill detector ready (YOLOv8n 160x160 INT8, head cut, decode on CPU).
```

A missing or wrong image usually does **not** announce itself as a flash
error - it shows up as a network that initialises and then returns nonsense.
If detection runs but the numbers are meaningless, suspect a weight region
before suspecting the code.

## Where these come from

* **`fd_data`, `faceid_data`, `ec_blobs`** - generated once and unchanged
  since. Byte-identical across every build that used them, which is how the
  copies here were validated. The epoch-controller data's source of
  truth is the tracked headers `FSBL/Inc/fd_ecblobs.h`, `faceid_ecblobs.h` and
  `network_ecblobs.h`; `ec_blobs.xSPI2.bin` is a packaging of the same content
  for the programmer.
* **`hand_atonbuf`** - ST model zoo `pose_estimation/handlandmarks`,
  Apache-2.0, regenerated against `medsight_hand.mpool`.
* **`pill_atonbuf`** - the hard-negative retrain, regenerated against
  `medsight_pill2.mpool` so it does not collide with the hand model.

Regenerating either model is in `tools/pill_detector/build_pill_detector.py`. Note
that a regenerated model changes its INT8 quantisation scales, which live in
`Inc/stai_*.h` - **so a weights change always requires a firmware rebuild**,
even though the firmware does not contain the weights.

## Why this directory exists at all

These blobs used to live only inside `firmware/STM32CubeIDE/FSBL/Debug/
weights_flash/`, i.e. inside build output. When build output stopped being
tracked, that silently removed the only copies of the face-network weights
from the repository. The check that authorised the removal looked for
hand-written source and found none, which was the wrong question: not
hand-written is not the same as regenerable, and for the face networks there
is no regeneration path in this repository at all.

Irreplaceable binary assets do not belong in a directory that a `.gitignore`
rule can sweep away.
