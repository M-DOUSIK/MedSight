# The plan against what was built

MedSight, entry 54916. Compared against the Program Plan submitted 31 March 2026
for the first screening round.

The plan described a **camera-only device**: a user holds a medication packet up
to the camera, a CNN classifies it, and the result is checked against a stored
schedule. What was built dispenses, counts, recognises the patient and
corroborates that the dose reached their mouth.

---

## Summary

| Promised | Status |
|---|---|
| Fully offline operation | **Delivered** |
| On-device NPU inference | **Delivered**, four networks rather than one |
| Caregiver notification flag in non-volatile memory | **Exceeded**, a full append-only event log readable on the device |
| Multi-patient support, listed as extensibility | **Delivered** as a core feature |
| RTOS task architecture, four tasks sketched | **Exceeded**, six tasks with priorities derived rate-monotonically |
| Schedule validation | **Delivered** |
| Audio and visual alert feedback | **Delivered**, five buzzer patterns and on-screen alerts |
| Open source commitment | **Kept**, Apache-2.0 |
| **On-device CNN pill classification** | **Not built.** See below |

| Not promised, delivered anyway |
|---|
| Face recognition and patient identity verification |
| Physical dispensing, with a counted pill sensor |
| Action recognition, two further networks |
| Carer mode behind a passcode, with on-device log review |
| Boot from external flash, running on battery |
| The µT-Kernel migration itself |

---

## The one thing that was promised and not built

The plan committed to *"on-device CNN-based pill/packet visual classification
using the board's NPU"*. That does not exist, and no amount of framing makes it
exist.

**What replaced it: the turntable.** One medication per turntable, separated
physically, which is foolproof in a way vision is not. A vision classifier can
misread a pill; a physical separation cannot.

**What the freed capacity went to** is the question the plan never asked: *did
the patient actually take it?* That is what face recognition and action
recognition answer.

The adherence chain the device implements is five measured links where the plan
offered one inferred one:

1. A carer loads one known medication onto the turntable.
2. The mechanism dispenses a counted dose of it.
3. Face recognition establishes who is taking it.
4. The schedule establishes that it is the right time.
5. The camera establishes that it went towards the mouth.

**One residual risk is unmitigated.** A turntable loaded with the wrong drug
produces a correct, confident, entirely wrong record. Nothing the device does
catches that.

**The pill detector is not a classifier.** It finds *a* pill; it does not
identify *which* medication. That is a narrower claim than the plan made, and it
is made narrowly.

---

## Physical dispensing was added, not restored

The plan contained no dispensing hardware at all. An intermediate revision of
this project's own planning expanded to a stacked-module mechanism, then cut it
entirely as not achievable in the time available, then built one module when the
team's hardware work made it possible.

So the device that ships is **scope added beyond the plan**, not a promise
recovered. The plan asked for a camera that identifies a pill; what arrived is a
machine that dispenses one, counts it out, watches it go to a mouth, and writes
down what happened.

The simulated dispense path was kept working throughout, which is why
`MEDSIGHT_PHYSICAL_DISPENSER` set to 0 remains a supported configuration rather
than dead code.

---

## Is the submission in line with the plan?

Three parts, and the first is a concession.

**One promised core function was not built.** On-device pill classification does
not exist.

**Everything else in the plan was delivered, and most of it exceeded.** Offline
operation, NPU inference, schedule validation, the caregiver record,
multi-patient support, the task architecture and the alert feedback are all
present, and four of those are materially more than the plan described.

**The substitution stands on its own terms.** The plan's classifier answered
*which pill is this?*, and the turntable answers that by construction before the
pill is ever dispensed. The harder and more clinically meaningful question, *did
the patient take it?*, was not in the plan at all and is what the delivered
device addresses.
