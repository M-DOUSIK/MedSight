# MedSight

**An offline medication dispenser and adherence recorder.**
Most dispensers release pills and assume the rest. This one measures who took
them and how many came out.

Built on µT-Kernel 3.0 and an STM32N6570-DK, with no radio of any kind in this
build - connectivity is planned work, not a rejected idea.

**TRON Programming Contest 2026 · RTOS Application category ·
Entry 54916**

🔗 **[m-dousik.github.io/MedSight](https://m-dousik.github.io/MedSight/)** - the
project site, with the device running.

---

A carer enrols a patient once - face, name, dose size and the times of day they
take it - behind a passcode. From then on the device recognises the patient by
face, reminds them when a dose is due, turns a stepper-driven turntable while an
infrared sensor counts each pill physically leaving it, runs action recognition
over the confirmation window, and writes what happened to a local SD card.

**It counts pills leaving the turntable and corroborates a swallowing gesture. It
cannot prove that a medication was ingested.** No device of this class can. The
action recognition is corroboration only, by design, and the button confirms
every dose. [`docs/MEDSIGHT_HANDBOOK.md`](docs/MEDSIGHT_HANDBOOK.md) is the full account
and is the document to read first.

---

## Where to start

| If you are... | Read |
|---|---|
| **new to this** | [`docs/MEDSIGHT_HANDBOOK.md`](docs/MEDSIGHT_HANDBOOK.md) - what the device is, how it works, how to run it, how to build and flash it, and what comes next. Also the copy printed and shipped in the box: [print edition PDF](pdf/MedSight_Handbook_PRINT.pdf) |
| **looking at the hardware** | [`docs/HARDWARE_SPEC.md`](docs/HARDWARE_SPEC.md) - annotated parts, wiring, bill of materials, specifications |
| **evaluating the engineering** | [`docs/TECHNICAL_REFERENCE.md`](docs/TECHNICAL_REFERENCE.md) - software, AI, memory, and every measured number with how it was obtained |
| **checking provenance** | [`docs/THIRD_PARTY_SOFTWARE.md`](docs/THIRD_PARTY_SOFTWARE.md) and [`docs/LICENSING.md`](docs/LICENSING.md) |
| **holding a device whose mechanism will not move** | [`docs/MEDSIGHT_HANDBOOK.md`](docs/MEDSIGHT_HANDBOOK.md) 8.9. One macro and a rebuild runs everything except the physical dispense, with no motor, driver or sensor attached |

**Six documents.** The handbook is also a print edition:
[`pdf/MedSight_Handbook_PRINT.pdf`](pdf/MedSight_Handbook_PRINT.pdf).

## The numbers

Every figure is measured on the real board and named with how it was obtained
in [`docs/TECHNICAL_REFERENCE.md`](docs/TECHNICAL_REFERENCE.md). None
is an estimate.

| | Measured |
|---|---|
| **CPU idle** | **~89.6%** of wall-clock time asleep in `WFI`, waking ~980×/s. The vendored BSP ships `low_pow()` **empty** |
| **Face recognition** | **209 ms** end to end - detector, embedder and the RTOS IPC between them - **identical to the millisecond across four captures** with different faces and confidences |
| **Memory footprint** | `.text` 561,324 · `.rodata` 334,456 · `.data` 684 · `.bss` 656,488 - 85.6% of the code region, 63.0% of the data region |
| **Pill detector** | 8-bit integer on the NPU with the decode on the Cortex-M55. The quantisation matches floating point on the validation set; at the deployment distance per-frame detection is about 7%, so it corroborates rather than decides |
| **IR pill counting** | real pulses **17-46 ms**, chatter 0-1 ms, floor set at 8 ms - all measured before any threshold was chosen |
| **Dispense counting** | 4 trials, all 13 pills counted by the sensor rather than by the motor. A small sample, and not a reliability figure: the mechanism has miscounted and jammed outside these trials |
| **µT-Kernel surface** | tasks, event flags, alarm handlers, a message buffer, one mutex and the idle hook. Every call lives in `ms_osal.c`; the vendored kernel is unmodified except for six files, none of which implements a system call |
| **The assembled unit** | **165 x 169 x 204 mm** and **950 g** with the turntable empty, tape measure and kitchen scales against the built device. 15 mm of the depth is the collection drawer standing proud of the body |

The 209 ms is the same on every capture: four images, four detector
confidences, one timing. The Neural-ART runtime executes a fixed epoch schedule
for a fixed input shape, so the cost does not depend on image content, which is
why the display holds still for the capture window instead of showing a
spinner.

## What makes this a µT-Kernel application, specifically

The category is judged on real-time performance, power saving and memory
footprint. The answer is not "we used an RTOS":

| Primitive | What it does here |
|---|---|
| `tk_cre_tsk` | **Six tasks**, priorities derived rate-monotonically. The AI task sits *below* the interface deliberately, which is what keeps touch and the physical button alive while the accelerator works |
| `tk_cre_flg`, `tk_wai_flg` | **Inference is dispatched through an event flag.** One blocking call distinguishes face found, no face, and no answer at all. A queue, a semaphore or a mutex cannot say that |
| `tk_cre_alm`, `tk_sta_alm` | **Dose scheduling is an alarm handler**, not a task polling the clock. Polling would wake 8,640 times a day in order to act four times. The handler sets one bit and returns; every line of real work is done by a task |
| `tk_cre_mbf`, `tk_cre_mtx` | The logger's queue, so no caller ever waits on an SD write, and the firmware's one mutex: the clock, which has two readers |
| `low_pow()` | The vendored board support package ships its idle hook **empty**. Ours executes `WFI` and accounts for the cycles, which is where the 89.6% comes from |

**A single file reaches the kernel.** Every call above lives in `ms_osal.c`, behind
an abstraction the rest of the firmware was written against, so the kernel's
use is deliberate and in one place rather than scattered. Two idioms were
evaluated and deliberately not adopted, with the reasoning recorded: a
fixed-size memory pool, and an event flag for the confirm state. Neither would
have done any work here.

## The AI, and which models are ours

| Network | Decides | Origin | Licence |
|---|---|---|---|
| CenterFace | Detects a face and emits five landmarks with it, both mouth corners among them | STMicroelectronics | SLA0044 |
| MobileFaceNet | Encodes the face crop as a 128-dimension embedding for cosine matching | STMicroelectronics | SLA0044 |
| MediaPipe hand landmarks | 21 keypoints, giving the fingertip position during the confirmation window | Google. **Not ours** - we selected, quantised, compiled and integrated it; we did not train it | Apache-2.0 |
| **Pill detector** | Single-class detection in a mouth-centred region. Corroboration only | **Trained by this project** | AGPL-3.0, from the training framework, [resolved](docs/LICENSING.md) |

Everything around the face networks is this project's code: the frame snapshot,
the CenterFace box decode, L2 normalisation, int8 quantisation, cosine gallery
matching and SD persistence.

The pill detector was trained here on a public pill dataset, exported, and
adapted to the accelerator: the convolutional body runs as 8-bit integer on the
NPU and the YOLO decode runs on the Cortex-M55. On the validation set the 8-bit
model matches the floating-point one it was trained from, but at the deployment
distance a pill is about an 8-pixel object and per-frame detection is around
7%, so it corroborates the hand geometry rather than deciding on its own.

It carries **AGPL-3.0** because Ultralytics YOLOv8 trained it and that licence
follows the weights, so the whole training pipeline is published with them in
[`tools/pill_detector/`](tools/pill_detector/).

## Data handling

**This build has no radio.** Wi-Fi, Ethernet and BLE are never initialised,
and no code path can transmit anything off-device.

**Connectivity is planned work**: a carer alert over Wi-Fi when a dose is
missed, and a patient-worn band that signals when a dose is due. The handbook's
last part sets out what that has to answer first: transport security, what the
message carries, where the data comes to rest, provisioning, and what happens
when the link is down.

**What follows from having no radio today:** patient names, face embeddings
and the adherence log cannot leave the SD card over a network, because there is
no network. That is a consequence of the scope decision rather than its motive,
so it changes when the radio arrives.

**The unit as shipped has no card fitted.** Its slot is on the board, inside
the housing, and the card is supplied alongside rather than installed. Until it
is fitted the home screen says so in those words, nothing is recorded, and the
enrolled gallery and the carer passcode last only until the power is cut.
Everything else runs exactly as described.

Face embeddings are never written to UART or to any log; only names, gallery
slot indices, dose counts, schedule times and match confidences are. That rule
was checked against a captured UART log from the noisiest build that exists,
not only by reading the source.

The patient record is five fields: a validity flag, a name, a 128-byte
embedding, a dose size, and up to four dose times. No phone number, no address,
no date of birth, no medical history. What is stored for face recognition is a
quantised embedding, **not the enrolment photograph** - the camera frame that
produced it is never written to the card.

Full posture, including the gaps, in
[`docs/MEDSIGHT_HANDBOOK.md`](docs/MEDSIGHT_HANDBOOK.md).

## Building it

This firmware starts from ST's STM32N6 example application and replaces its
core: µT-Kernel 3.0 is the RTOS, and the OS abstraction layer, the interface,
the state machine, the scheduler, the logger and the dispenser driver are
original MedSight code. Every third-party component and its licence is
inventoried in [`docs/THIRD_PARTY_SOFTWARE.md`](docs/THIRD_PARTY_SOFTWARE.md).

**STM32CubeIDE 2.1.1** or newer. There is **no `.ioc` file** - every peripheral
is configured in code rather than generated, so there is nothing to
regenerate.

```
firmware/STM32CubeIDE/FSBL     <- import this, build Debug
```

Expect **0 errors, 0 warnings**. Both `MEDSIGHT_PHYSICAL_DISPENSER` 1 and 0
build clean; `0` runs the whole application with **no dispensing hardware
attached**, which is a supported configuration rather than a debug path. That
branch is the earlier on-screen dispense kept byte-for-byte, and it has been
run on the assembled board rather than only compiled.

> **Building is not enough to get a working device.** The NPU weights live in
> external flash and the linker marks those regions `(NOLOAD)`, so the
> application build never writes them. Flash the five images in
> [`weights/`](weights/) once per board, then power-cycle. Without them the
> firmware runs and both AI features return nonsense, **with nothing in the log
> to say why.**

Complete instructions, every command of which was actually run:
[`docs/MEDSIGHT_HANDBOOK.md`](docs/MEDSIGHT_HANDBOOK.md) Part 3.

## Licence - is this open source?

**Yes.** MedSight's own code, documentation and CAD are released under
**Apache-2.0**, an OSI-approved open-source licence: read it, build it, change
it, ship it, including commercially, keeping the notice.

The repository is more a **shelf** than a **book** - every item carries its own
licence, and one of them is not open source: ST's AI runtime, camera ISP and
the two face models are under **ST SLA0044**, which permits free redistribution
for use on ST hardware but is not an open-source licence. Every STM32 AI project
carries that, and it blocks nothing.

**MedSight's own code is Apache-2.0.** The repository is **not licensed as a
single work** - it vendors µT-Kernel under T-License, ST components under
BSD-3-Clause and SLA0044, a Google model under Apache-2.0, and FatFs under its
own licence.

[`docs/LICENSING.md`](docs/LICENSING.md) is the per-file map, and it also
**resolves the AGPL-3.0 question** on the pill detector rather than leaving it
open: the project adopts Ultralytics' own stated reading, complies with it for
the detector's artefacts, licenses its own code permissively so the question
cannot bind anything else, and names the one residual incompatibility along with
the build-verified configuration that avoids it.

## Layout

```
MedSight/
  index.html, site.css   the project site (GitHub Pages serves this)
  media/                 the hero loop and its poster
  firmware/              the complete firmware - build this
  hardware/cad/          STL files for the three printed assemblies
  weights/               NPU weight blobs, addresses and fingerprints
  docs/                  everything above, and the engineering record
  pdf/                   the handbook, print edition
  research/              the evidence base, with licences
  tools/                 the model training pipeline and asset generators
```

**The handbook is also a print edition**, laid out to be read rather than
filed: [`pdf/MedSight_Handbook_PRINT.pdf`](pdf/MedSight_Handbook_PRINT.pdf).

## Where it stands

It is a working prototype, and four things are not finished. Each has a plan,
and the handbook's last part sets all of them out properly.

- **One turntable, so one medication.** Next: the stacked dispensing modules already drawn
  in the hardware specification, plus a drug field in the patient record. The
  interface draws the extra slots today, greyed out.
- **It cannot see how full the turntable is.** Next, and the cheapest of these: a
  carer-entered fill count, decremented by *counted* pills, shown as a low
  stock warning on screens that already exist.
- **A missed dose cannot leave the room**, because there is no radio in this
  build. Next, and the top of the list: a carer alert over Wi-Fi and a
  patient-worn band, with the security design that has to come with them.
- **The housing does not lock.** Next: a lockable enclosure, tamper detection
  and a vibration sensor, so the log can say when the unit was disturbed.

And one thing to be clear about rather than plan around: the device counts
pills out and corroborates a swallowing gesture. It cannot prove somebody
swallowed, and no device of this class can, which is why the patient's button
press stays the confirming action.

## Credit

MedSight was built by five of us.

| | |
|---|---|
| **Dousik Manokaran** (applicant) | The firmware: the µT-Kernel migration, the six-task architecture and its priorities, the four networks on the accelerator, and the documentation |
| **Howard Nikhil** | The physical dispensing hardware: the turntable, the infrared pill counter, and the bench work the counting design rests on |
| **Gaurav V R** and **Dillimaran K** | The three-stage intake pipeline - detection, geometry, temporal decision - and the guarded state machine that refuses to certify an intake it cannot support |
| **Abirami M** | The visual identity: the mascot, and the artwork all twenty-two screens are cut from |
| **Dr. G. Santhanamari** and **Dr. D. Selvakumar** | Supervision, and two roadmap items: the vibration sensor for tamper detection, and the patient-worn alert band |

Our thanks to the TRON Forum for the opportunity, and for the STM32N6570-DK
board that made the second round possible.

µT-Kernel 3.0 is (c) TRON Forum and Ken Sakamura, under T-License 2.1 /
2.2, and the STM32N6570-DK was supplied by the TRON Forum for the second round.

Third-party components and their licences are inventoried in
[`docs/THIRD_PARTY_SOFTWARE.md`](docs/THIRD_PARTY_SOFTWARE.md).
