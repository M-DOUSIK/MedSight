# Technical reference

MedSight, entry 54916. How the firmware is built, and every measured number.

| Part | |
|---|---|
| A | The RTOS: tasks, priorities, the kernel primitives used and rejected |
| B | Software structure: modules, the OS abstraction boundary, the state machine |
| C | The AI pipeline: four networks, what each decides |
| D | Memory: the regions, and what the accelerator can reach |
| E | Every measured number, and how it was obtained |

---

# Part A. The RTOS

## A1. Tasks

Six tasks. Priorities were derived rate-monotonically, shortest period first,
with each number justified rather than inherited. Priority 5 is highest.

| Priority | Task | Period | Why it sits there |
|---|---|---|---|
| 5 | Camera / ISP | 1 ms | The only task tied to external hardware timing. It must consume each frame's statistics before the next frame arrives, about every 33 ms at 30 fps. Missing it degrades exposure and white balance visibly |
| 4 | User interface | 10 ms | Touch-to-response budget. A 10 ms poll plus a 4 fps animation gate stays an order of magnitude inside the roughly 100 ms at which input lag becomes noticeable |
| 3 | AI | on demand | **No deadline.** Hundreds of milliseconds of accelerator work per request, a few times per session, in response to a button press the user already expects to take a moment. Deliberately below the interface so it is preemptible, which is what keeps touch alive during inference. Above the logger because a person is waiting on its result and nobody waits on a log line |
| 2 | SD logger | event-driven | Tolerates seconds of latency by construction. The asynchronous queue exists so no caller ever waits on a 10 to 50 ms card write |
| 1 | Heartbeat | 500 ms | No deadline at all. Deliberately lowest, so "the LED stopped blinking" means "something above me is starving the system", which is exactly the signal it should carry |
| 1 | Alert | event-driven | Sleeps until a pattern is requested, then toggles a pin through a few hundred milliseconds of tone. Shares the lowest level deliberately: a buzzer that delays the camera task to finish a beep is a defect, and a beep arriving 5 ms late is not perceptible |

The application uses 1 as lowest; the abstraction layer inverts this onto the
kernel's opposite scale. Only the ordering is load-bearing, and the absolute
numbers leave headroom on both sides of the kernel's maximum.

## A2. The primitives that are used, and why each is the right one

**Inference is dispatched through an event flag.** The interface's wait has
three outcomes to distinguish in one blocking call: face found, no face, or the
AI task never answered. That is what an event flag expresses and what a queue, a
semaphore or a mutex cannot. The behavioural payoff is real: touch and the
physical button stay alive during a capture, where before the whole interface
task blocked inside the accelerator for seconds.

Action recognition later earned the same primitive again, because the AI task
must block on *a capture request or an intake request* in one call.

**Dose scheduling is an alarm handler**, one-shot, re-armed for the window's
closing edge and then for the next dose. A task polling the clock would wake
8,640 times a day to act four times, and would spend the whole power figure
doing it. The handler runs in handler context, so it sets one bit in an event
flag and returns; every line of actual work is done by a task.

**Deferred object creation bridges a real kernel constraint.** Kernel objects
can only be created once the kernel is running, but every creation call in this
codebase happens before the scheduler starts. The abstraction layer defers real
creation to its own entry point, and callers never see it.

## A3. Two primitives evaluated and deliberately not adopted

**A fixed-size memory pool.** This firmware has no fixed-size runtime allocation
site at all, so adopting one would have meant inventing an allocation in order
to have something to pool.

**An event flag for the dose-confirmation state.** All three of its conditions
are produced by the task that would wait on them.

Neither primitive would have done any work here, and a primitive that does no
work is harder to read than one that is absent.

## A4. Three things that deliberately did not get a task

| | Why not |
|---|---|
| **Scheduled dosing** | Its work is an alarm handler plus a few lines of reaction in the interface task, which already runs every 10 ms and already owns every screen the reaction touches |
| **Action recognition** | Runs on the existing AI task, woken by a fourth bit on the same event flag. A second AI task would have turned the framebuffer ownership rule into a race instead of an invariant |
| **The dispenser** | Runs synchronously on the interface task, because the interface has nothing else to do while a dose is delivered and the progress bar it draws is driven by the same callback. The part that must be asynchronous already is: it is an interrupt handler |

**The buzzer did get a task**, and the contrast is the point: its work is a
sequence of timed waits. Driving it from the 10 ms interface task would either
block every screen for the length of a beep or smear the pattern across
iterations, and the shape of the tone is exactly what distinguishes a polite
patient reminder from an insistent carer alert.

## A5. Power

The vendored board package ships its idle hook empty, so the kernel's dispatcher
spun the processor at full clock whenever no task was runnable, which in this
application is most of the time. It now forwards to this project's own
low-power idle, which executes a race-free sleep instruction and accumulates the
cycles spent asleep.

**Measured: about 89.6% of wall-clock time asleep, waking about 980 times a
second.**

Two faults sit behind that number and both generalise.

**Sleep hung the board, and the interrupt mask was why.** The idle path runs
with a priority mask set, and the system timer's priority is the same value. A
wake-up event must be an exception that would preempt the current execution
priority, and the architecture excludes one masking register from that judgement
but not the other, so the timer could not wake the core and a plain sleep
instruction froze the system the instant it first had nothing to run.

**Putting a processor to sleep is a system-wide change, not a power tweak.** The
sleep state stops the clock of every peripheral, bus and memory whose low-power
enable bit is clear. The framebuffer lives in banks that were affected, so every
idle tick cost the display controller either its own clock, the bus clock, or
the memory it was reading. The panel greyed out while every register the
processor could read said the display was healthy, because the processor only
reads when it is awake. The measurement that isolated it removed the sleep
instruction and changed nothing else.

## A6. The modification surface

**Six vendored kernel files differ from upstream, and every file that implements
a system call is byte-identical.** No kernel call appears anywhere outside this
project's own abstraction layer. The full table with the reason for each change
is in `THIRD_PARTY_SOFTWARE.md` section 4.

---

# Part B. Software structure

## B1. The abstraction boundary

Every application call reaches the kernel through one file. The risk it was
introduced against was explicit: the kernel API might differ enough from the
previous RTOS that the mapping breaks something subtle. Isolating that to one
file kept the blast radius to one file, and when the migration happened, it did.

## B2. Frame buffer ownership

There is **one** framebuffer, and three things write to it: the camera's DMA,
the interface's drawing code, and the accelerator, because the generated network
code places its working memory overlapping the same address.

A second framebuffer was tried, so the interface could keep drawing during
inference. A marker written into it came back overwritten, and rather than work
around that by shrinking buffers or relocating the accelerator's memory pools,
the single buffer was kept. Extracting every address literal from the generated
networks then confirmed why: the second buffer sat entirely inside the
accelerator's working region. There is arithmetic behind that
rather than one observation.

**What ships instead:** the display layer is switched off for the capture
window, so the panel shows a flat colour instead of a frame being visibly
scribbled over. It reads as a deliberate pause rather than a fault.

## B3. The dispenser driver

**One atomic store per step.** The half-step table holds fully-formed register
words rather than per-pin booleans, so advancing the motor is a single 32-bit
write. There is no read-modify-write, so an interrupt landing mid-step cannot
leave two coils energised in a combination the table never contains. The coils
are switched off on every exit path including error paths, so a stepper is never
left drawing holding current after a failed dispense.

**The interrupt timestamps, the task decides.** Both signal edges are timestamped
in the interrupt handler. Breaks shorter than the measured chatter floor are
discarded there; surviving durations go into a 16-entry ring drained from task
context. Nothing prints from the handler, nothing allocates, and the counting
decision lives where it can block safely.

**One bound, on the right quantity.** The original design had a stall bound of
1500 ms on how long the sensor may stay asserted, plus an absolute cap on the whole
dispense. The first standalone run produced legitimate single-pill breaks of 852
and 1210 ms: 290 ms of margin on a medication device, where tripping the bound
means refusing a dose that was being delivered correctly. The absolute cap
measured the wrong thing too, punishing a slow dose rather than a stopped one.

Both are gone. One bound remains, 25 seconds since the **last counted pill**,
restarted by every pill. A working mechanism runs as long as it needs; a stopped
one is caught within one window, including the pill-stalled-at-the-sensor case, which
simply stops producing counts.

**A jam is not a system fault.** The error handler is not called anywhere in the
dispenser. A jam is a normal outcome of a mechanical device, and both failure
outcomes have screens, audit entries and recovery paths.

## B4. The audit log

Written once, after the outcome is known:

```
DISPENSE: <patient> <n> requested, <m> counted (<result>)
```

Logging the intent beforehand and the result afterwards would leave a record
reading as two claims about one event, so the line is written only once.

## B5. The interface

22 screens drawn from one visual system: one frame, one title bar treatment, one
button primitive, one two-button layout, four anti-aliased proportional
typefaces, and colour used as identity, so one colour means one medication
everywhere it appears.

**Three design decisions worth recording:**

A stepped date entry was replaced by typed digits. The stepper made an invalid
date impossible to enter and was miserable: setting a year, day and time from
the power-on default is on the order of a hundred taps. A carer setting the
clock already knows the date; the job is to let them type it. Twelve digits and
a confirm is thirteen taps, and validation is one call at the end. The
dose-times screen went the same way, and the device's own log showed why: that
screen was opened and saved six times in one session when it used a 15-minute
stepper.

Stepping survives on exactly one screen, dose size from 1 to 10, where every
value is at most five taps away and the control shows the whole range
implicitly. That is what stepping is good at.

**The delete confirmation times out to KEEP.** Every other timeout in this
interface goes forward; this one goes back, because that is the only correct
default for something that cannot be undone.

---

# Part C. The AI pipeline

## C1. The four networks

| Stage | Model | Decides |
|---|---|---|
| Face detection | CenterFace, INT8 | Who is at the device. Also supplies both mouth corners |
| Face embedding | MobileFaceNet, INT8 | Whether they are an enrolled patient |
| Hand | MediaPipe hand landmarks, 224x224 INT8, 21 keypoints | Whether a hand reached the mouth |
| Pill | Single-class detector, 160x160 INT8, head removed | Corroboration only. Never creates a detection |

## C2. Face recognition

CenterFace locates a face; MobileFaceNet turns the crop into a 128-dimension
fingerprint; the fingerprint is matched by cosine similarity against the
enrolled gallery at a threshold of 0.65. The gallery is a fixed-size array
sized by `MAX_PATIENTS`, 10 in this build: the bound is RAM, not the matching,
which is a linear scan over 128-byte embeddings and costs nothing worth
measuring at this scale.

Everything around those two networks is this project's own code: the frame
snapshot, the detection box decode, normalisation, 8-bit quantisation of the
fingerprint, the cosine matching, and persistence to the card.

**The threshold was deliberately not moved blind**, because loosening it trades
a recoverable false rejection for a false acceptance.

## C3. The intake pipeline

```
mouth landmarks, free, from the face capture that just happened
  -> region of interest, 1.5 face widths, centred on the mouth
  -> hand landmark model -> presence and 21 keypoints
  -> fingertip = midpoint of thumb tip and index tip
  -> distance to mouth measured in FACE WIDTHS, not pixels
  -> three consecutive frames -> dose observed
```

**The mouth costs nothing.** The face detector already emits both mouth corners
on a tensor the firmware defines and had never read. That stage is a decode, not
an inference.

**Measuring in face widths rather than pixels** makes the geometry independent
of how far the patient is from the camera.

**A privacy consequence follows from the design and is a benefit rather than a
compromise.** The face detector gives two mouth corners and no lip contour, so
the device cannot and does not analyse facial expression. It measures one
distance.

## C4. Why the hand decides and the pill corroborates

The pill detector was intended to carry the decision. It cannot: at the
deployment region of interest a pill is about an 8-pixel object, and measured
per-frame detection of a 12 mm object is about **7%**. A detector firing on 7%
of frames cannot support a decision that needs three consecutive detections.

Before the hand model, four rounds of skin-tone heuristics were tried and none
could separate **a moving ear** from **a hand**, because at that level of
description they are the same thing: skin-coloured, solid, moving. A model that
knows what a hand is settled it in one step.

Measured discrimination:

| Input | Hand presence |
|---|---|
| A face | 0.0078 |
| 400 random images | never above 0.035 |
| An ear, waved deliberately | does not fire |
| A hand at the mouth | about 0.50, and confirms |

## C5. The pill detector we trained

The detector was trained for this device on a public pill dataset, then
exported and adapted to the accelerator.

The adaptation is the part worth knowing about. The exported graph's detection
head does not survive 8-bit quantisation, so the graph is cut after the six raw
head convolutions: the convolutional body runs as 8-bit integer on the NPU, and
the decode runs on the Cortex-M55, which is idle most of the time anyway. On
held-out footage the result matches the floating-point model it was trained
from.

## C6. Corroboration, never replacement

The verdict goes into the audit log as evidence. The confirmation button remains
the confirming action for every dose, so a model failure can never mean a dose
that cannot be confirmed. The whole feature sits behind a build switch so it
remains removable.

The device ships with the simplified intake rule rather than the full guarded
state machine, because the guarded machine needs three consecutive detections to
leave its search state and the measured 7% per-frame rate makes that lock
essentially unreachable. An unreachable guard protects nothing. The audit wording
changes with the mode: the simple rule logs *"pill reached mouth"*, never
*"gesture confirmed"*, because reaching the mouth is precisely what was observed.

---

# Part D. Memory

## D1. Footprint

| Section | Bytes |
|---|---|
| Code | 561,324 |
| Read-only data | 334,456 |
| Initialised data | 684 |
| Zero-initialised data | 656,488 |

**85.6% of the code region** used, 147 KB free. **63.0% of the data region**
used, 378 KB free.

Hand-written code: about **11,800 lines across 28 files**, excluding all
vendored code and generated assets.

## D2. What the accelerator can reach, and the fault that taught it

The first time a network ran, the accelerator crashed the system immediately
with a bus fault.

**The cause was a bus-matrix fact, not a bug.** The generated code places model
weights as constant arrays in the read-only data section, and this project's
linker script put that section in a RAM bank the accelerator's data masters
**physically have no connection to**. It could not read from that region at all,
so fetching the weights faulted.

The fix was to tag the weight arrays into a section mapped to external NOR
flash, which the accelerator's interface can reach directly.

**The generalisable part:** on a part with an accelerator, "the memory exists
and the address is valid" and "this master can reach it" are different
questions, and only the bus matrix answers the second.

## D3. Regions

| Region | What |
|---|---|
| On-chip SRAM, about 4.2 MB in six banks | Code, data, the framebuffer, and a 220 KB accelerator-reachable arena |
| The arena | 220 KB, claimed as a named linker region and **pattern-tested from a cold boot** before anything was put in it |
| Face network working memory | One contiguous block that overlaps the framebuffer, which is why the display is blanked during a capture |
| Hand model working memory | 1,197,952 bytes: about 220 KB in the arena, about 978 KB spilled to external PSRAM |
| Pill detector | Relocated entirely to PSRAM so the two intake networks can coexist |
| External NOR flash | Four weight regions, flashed separately and once |

## D4. Why the weights are a separate flashing step

The linker marks the external flash regions as not-to-be-loaded, deliberately,
so building and flashing the application never writes them. That makes the
every-edit build fast, because the development cycle only ever writes the
internal-RAM application.

The cost, which bites every newcomer: **a fresh
clone compiles a perfectly good binary, flashes it, and produces a device whose
AI outputs are nonsense, with nothing in any log to say why.** The handbook's
flashing section opens with that warning for exactly this reason.

## D5. Boot

The processor has **no internal flash**. Its boot ROM copies a first-stage
loader from external flash into RAM, and that copy is capped at **512 KB**. This
firmware is about 880 KB, so it can never be the first stage: it is signed as a
second-stage image and loaded by ST's own small loader.

---

# Part E. The measurements

Every number here was taken on the real board. Where a figure names a count of
trials, that count is the real one.

## E1. The RTOS

| Quantity | Value | How |
|---|---|---|
| CPU idle | **89.6%** of wall-clock time asleep | Cycle counts accumulated across each sleep, read out from a task on a 10-second period. Read from a task, never from a handler, for the reason in E5 |
| Wake rate | about 980 per second | Same counter |
| Kernel tick | 1 ms | Inside this port's own declared range |
| Tasks | 6 | Part A |
| Kernel files modified | **6 of about 230** | Full recursive comparison against pristine upstream |
| Files implementing a system call that were modified | **0** | The entire kernel directory is byte-identical |
| Kernel calls outside the abstraction layer | **0** | Verified by search |
| Linked objects | 321 | About 234 of them are kernel files, most compiling to empty translation units |

**What the idle figure is not.** It is a duty cycle, not a power measurement.
The board supports instrumented power measurement and it was never wired up. **No
figure in watts is claimed anywhere in this submission**, because none was
measured.

## E2. Vision

| Quantity | Value |
|---|---|
| Face recognition, end to end | **209 ms**, identical to the millisecond across four captures with different faces and confidences |
| Failed capture | 1111 ms, three detector passes and two waits |
| Hand landmark inference | **285 to 302 ms** |
| Pill detector, floating point | 84 of 90 |
| Pill detector, 8-bit, head attached | **0 of 90** |
| Pill detector, 8-bit, head removed, decode on the processor | **85 of 90** |
| Per-frame detection of a 12 mm object at the deployment region | about **7%** |
| Face match threshold | 0.65 cosine |

**The 209 ms invariance is the interesting part.** Four captures, four images,
four different detector confidences, and the same figure every time. The
accelerator runs a fixed schedule for a fixed input shape, so cost does not
depend on image content. That is what makes it defensible to hold the display
still for the capture window rather than showing an indeterminate spinner.

**The number that matters most for a demonstration.** Match scores of **57 to 88
for the same person** were measured under varying illumination with enrolment
taken under different light again, rejecting them about one time in three. That
is a property of one-shot face recognition, not a device defect, and the retry
screen rescued every case. Enrol in the light the device will be used in.

## E3. Power during an intake watch

Idle falls to **7 to 50% during a watch**, against about 88% either side. Quoted
alone that looks alarming, so the duty cycle is quoted with it:

```
4 doses x 30 s = 120 s of watch, per 86,400 s day = 0.14% of the day
day average = 0.9986 x 88% + 0.0014 x 10% = 87.9%
```

The daily average moves from about 88.0% to about **87.9%**. The headline figure
is a statement about the device at rest, and the device is at rest 99.86% of the
time. What would change that conclusion is the watch running when nobody is
being dispensed to, which is why it is ended on every exit from the confirm
screen and has its own 30-second timeout.

## E4. Dispensing

Taken on the bench with a purpose-written break-duration histogram tool,
**before** the driver was written:

| Quantity | Value | Samples |
|---|---|---|
| Beam idle level | HIGH | 7 separate boots |
| Real pill breaks | **17 to 46 ms** | bench |
| Contact chatter | 0 to 1 ms | bench |
| Discrimination floor chosen | **8 ms** | above every chatter event observed, and less than half the shortest real break |
| Longest legitimate break observed | **1210 ms** | the figure that removed the 1500 ms stall bound |
| Half-step period | 2 ms | smooth and silent under turntable load |

**The 17 to 46 ms figure saved the design.** Pills slide down a ramp rather than
free-falling; a free-fall past the sensor would have been a few milliseconds,
and a driver sized for that number would have discarded every real pill.

**Reliability:**

| Run | Requested | Counted |
|---|---|---|
| 1 | 5 | 5 |
| 2 | 3 | 3 |
| 3 | 3 | 3 |
| 4 | 2 | 2 |
| **Total** | **13** | **13** |

Four trials is a small sample and these four were clean. It is not a
reliability figure: the mechanism has miscounted and jammed outside them.

Plus one complete dispense on battery alone, no laptop, no debugger.

Four trials is what was run, and four is what is claimed.

## E5. The interface

| Quantity | Value |
|---|---|
| Framebuffer | 800 x 480, one only |
| Screens | 22 |
| Confirm button | 330 x 200 px |
| Keyboard keys | 70 x 60 px |
| Numeric keypad | 12 keys at 128 x 66 px |
| Carer mode gesture | 5 taps within 3 seconds |
| Passcode rate limit | 5 wrong attempts, then 30 s, held in RAM |
| Body text contrast | 17:1 |

The palette was measured against WCAG AA and darkened where it fell under
the 3:1 floor for large text, so every colour on screen now clears it.

That establishes the palette meets a published standard. It does not establish
that the interface works for the people it is for: no person with dementia has
used this device yet, and that trial is the next piece of work on the
interface.

## E6. What the bench taught, and what it changed in the code

Each of these is a measurement that settled a question, and each one is why
some line in the firmware reads the way it does.

**A verification has to distinguish the outcomes it cares about.** A flash
write was being verified by reading four bytes of a magic number that both
candidate images happened to share, so it could only ever report success. It
now verifies the whole file and fingerprints the entry point.

**Instrumentation on a hot path changes what it measures.** A print call on the
dispatcher path made the system timer starve the context switcher. It is why
nothing in this firmware prints from handler context, and why the idle
accounting in E1 is read out from a task rather than from where it is
collected.

**Reading a register back proves nothing about the pin.** An early dispenser
wiring used analog-header pins. The motor did not move and the driver's channel
LEDs never lit, but all four pins read back HIGH through the input register, so
every software check said the processor was driving them. A bare jumper from
the board's 3.3 V pin to a driver input lit the LED immediately, which
eliminated the driver, the grounds and the wiring in one step. The input
register reading back your value proves the latch took it, and nothing about
whether the pad drives anything external.

**One measurement can settle several questions at once.** Three hypotheses
about the flash controller's state differing between boot paths were settled
together by a register dump showing the controller bit-for-bit identical on
both paths. That moved the search from the controller to the chip, which is
where the answer was: ST's external loader leaves the flash in a high-speed
mode, and that is a property of the chip, surviving any reset on the processor
side.

**A constant that tracks a linker region has to move with it.** When the
linker regions moved for standalone boot, a kernel configuration constant
tracking the same boundary did not, so the kernel heap ended up with negative
size and the scheduler started with no tasks. The board printed its entire
pre-kernel log and stopped, with no fault and nothing on the console. The two
are tied together now, and the linker script says so at the definition.

**A discarded return value hides for as long as you let it.** A filesystem
driver call tested a state the hardware layer never assigns to that field, so
the condition was always true and the call returned "not ready" for every
command it was given. It stayed invisible because the filesystem calls it at
the very end of closing a file, after everything is already written, and
because two call sites above it discarded the result. Every one of those call
sites checks now.

**Debug and release laid the accelerator weights out in opposite order.** The
weight blobs are constant arrays in one literal section, so their internal order
is the compiler's emission order, which is not the same at different
optimisation levels. Every blob address the release binary computed pointed at a
different blob's bytes. Fixed by pinning the emission order; one flashed image
now serves both builds.

**A cold boot is not the same reset as a flash-and-run.** The power
controller's supply-valid bits live in the always-on domain and survive a
system reset, so a debug cycle inherits I/O domain settings that a genuine
power cycle does not provide. Every supply domain is now declared valid at
boot, unconditionally, before a single GPIO is touched.

**A check written when a resource was unowned ages like any other code.** The
arena memory self-test was written while nothing owned the arena. Once a
network was loaded there, the same test would have pattern-written over it, so
it now runs in exactly one window and nowhere else.

## E7. Not measured yet, and how each would be

Every number above is on the real board. These are the ones still to take, so
that no figure here is mistaken for one of them.

| Not yet measured | What it takes |
|---|---|
| Power in watts. The 89.6% is a duty cycle | wiring up the board's own instrumented measurement |
| The buzzer's output, confirmed so far by ear | a scope trace on the GPIO and a recording |
| A deliberately induced jam, and a genuinely empty turntable | both are reasoned from measured break durations; both want a bench run |
| The gallery-full path | eleven enrolments |
| The clock across a genuine power loss | pulling the supply, not resetting |
| Anything with a real user | the trial that is next on the interface work |
