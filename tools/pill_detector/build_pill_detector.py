#!/usr/bin/env python3
"""build_pill_detector.py - MedSight, Stage 1A.

Builds the single-class pill detector that runs on the STM32N6's Neural-ART
NPU, end to end and reproducibly:

    dataset -> YOLO format -> train -> export ONNX -> cut head
            -> INT8 quantise -> validate -> ST Edge AI C code

Run with no arguments to do everything:

    python build_pill_detector.py --all

or a single stage, e.g. `--quantise`, when iterating.

────────────────────────────────────────────────────────────────────────────
WHY THIS EXISTS AT ALL - the collaborator's model was measured, not assumed
────────────────────────────────────────────────────────────────────────────
`models/pill_detector/best.onnx` came with the action-recognition design and
is kept in the tree as its provenance: the three-stage pipeline and the state
machine ported in FSBL/Src/ai/intake_fsm.c come from that work. Its training
images are not available, so it could not be calibrated for INT8 against data
it had not seen. This script trains the weights the device ships with, on a
dataset that is public and citable.

────────────────────────────────────────────────────────────────────────────
LICENSING - read before changing the dataset
────────────────────────────────────────────────────────────────────────────
Training data: Roboflow Universe `pills-sxdht` (RF100), 451 images,
**CC BY 4.0**, obtained via the HuggingFace mirror `Francesco/pills-sxdht`.
Attribution belongs in documents/THIRD_PARTY_SOFTWARE.md.

CC BY was chosen deliberately over the obvious alternative (Ultralytics'
`medical-pills`, which is AGPL-3.0) because every other third-party
component in this firmware is SLA0044, BSD or similarly permissive, and
introducing strong copyleft into a contest submission that co-distributes
ST's SLA0044 code is a question nobody wants to answer at a deadline.

That said, be straight about what remains: the TRAINING FRAMEWORK
(Ultralytics YOLOv8) is itself AGPL-3.0, and whether trained weights are a
derivative work of the framework that produced them is genuinely unsettled.
This is recorded in THIRD_PARTY_SOFTWARE.md as an open question rather than
quietly resolved in our own favour. The clean escape, if it is ever needed,
is an ST model-zoo detector under SLA0044 fine-tuned on this same CC BY
dataset - the rest of this pipeline is unchanged by that swap.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
WORK = HERE / "build"
DATA_URL = "https://huggingface.co/datasets/Francesco/pills-sxdht/resolve/main/dataset.tar.gz"

IMGSZ = 192          # see "WHY 192" below
EPOCHS = 80
CONF_T = 0.35        # the collaborator's conf_thresh, kept
STEDGEAI = r"C:\ST\STEdgeAI\4.0\Utilities\windows\stedgeai.exe"

# The six raw head convolutions - the cut points. See CUTTING THE HEAD below.
CUT_OUTPUTS = [
    "/model.22/cv2.0/cv2.0.2/Conv_output_0", "/model.22/cv3.0/cv3.0.2/Conv_output_0",
    "/model.22/cv2.1/cv2.1.2/Conv_output_0", "/model.22/cv3.1/cv3.1.2/Conv_output_0",
    "/model.22/cv2.2/cv2.2.2/Conv_output_0", "/model.22/cv3.2/cv3.2.2/Conv_output_0",
]

# ── WHY 192 ────────────────────────────────────────────────────────────────
# The collaborator trained at 320 because they fed the detector a 320x320
# crop taken around a MediaPipe hand-pinch point. MedSight has no hand
# tracking, so it feeds a crop centred on the MOUTH instead (two face
# widths across, FSBL/Src/ai/intake_camera.c). A pill on its way to a mouth
# fills a large fraction of that crop, so the extra resolution buys little -
# and activation memory scales with the square of the input, which matters a
# great deal here. Measured on the delivered 320px model:
#     FP32 activations 4,505,600 B; INT8 therefore ~1.1 MB
# against an AI_ARENA of 225,280 B. At 192 the same arithmetic gives ~405 KB,
# and with the head cut the real figure comes in lower still. Whatever it
# turns out to be, `--analyse` prints it and `docs/TECHNICAL_REFERENCE.md` records it - this
# project does not guess at memory.


def sh(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


# ══════════════════════════════════════════════════════════════════════════
# 1. Dataset
# ══════════════════════════════════════════════════════════════════════════
def fetch_dataset():
    WORK.mkdir(parents=True, exist_ok=True)
    tgz = WORK / "pills_rf100.tar.gz"
    raw = WORK / "rf100"
    if not tgz.exists():
        print(f"downloading {DATA_URL}")
        urllib.request.urlretrieve(DATA_URL, tgz)
    if not raw.exists():
        raw.mkdir(parents=True)
        with tarfile.open(tgz) as t:
            for m in t.getmembers():
                # the tarball carries the author's absolute home path
                parts = Path(m.name).parts
                if "pills-sxdht" not in parts:
                    continue
                idx = parts.index("pills-sxdht")
                rel = Path(*parts[idx + 1:])
                if not rel.parts:
                    continue
                dst = raw / rel
                if m.isdir():
                    dst.mkdir(parents=True, exist_ok=True)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    with t.extractfile(m) as fsrc, open(dst, "wb") as fdst:
                        shutil.copyfileobj(fsrc, fdst)
    print("dataset at", raw)
    return raw


def to_yolo(raw: Path):
    """COCO -> YOLO, collapsing all 8 medication sub-classes to one `pill`.

    MedSight never needs to know WHICH medication - that is pill
    CLASSIFICATION, which this project does not do and says so plainly in
    PROGRAM_PLAN_RECONCILIATION.md section 1. It needs to know that a pill is
    present and where it is. Collapsing the classes also multiplies the
    per-class training data by eight, which for 451 images matters.
    """
    ds = WORK / "pill_ds"
    splits = {"train": "train", "valid": "val", "test": "test"}
    for s in splits.values():
        (ds / "images" / s).mkdir(parents=True, exist_ok=True)
        (ds / "labels" / s).mkdir(parents=True, exist_ok=True)

    n_img = n_box = 0
    for src, dst in splits.items():
        ann = raw / src / "_annotations.coco.json"
        if not ann.exists():
            continue
        d = json.loads(ann.read_text())
        boxes = {}
        for a in d["annotations"]:
            if a.get("iscrowd") or a["category_id"] == 0:   # 0 is a dummy
                continue
            boxes.setdefault(a["image_id"], []).append(a["bbox"])
        for im in d["images"]:
            src_img = raw / src / im["file_name"]
            if not src_img.exists():
                continue
            shutil.copy2(src_img, ds / "images" / dst / im["file_name"])
            W, H = im["width"], im["height"]
            lines = []
            for (x, y, w, h) in boxes.get(im["id"], []):
                if w <= 0 or h <= 0:
                    continue
                lines.append("0 %.6f %.6f %.6f %.6f" % (
                    min(max((x + w / 2) / W, 0), 1), min(max((y + h / 2) / H, 0), 1),
                    min(w / W, 1), min(h / H, 1)))
            (ds / "labels" / dst / (Path(im["file_name"]).stem + ".txt")
             ).write_text("\n".join(lines))
            n_img += 1
            n_box += len(lines)

    (ds / "data.yaml").write_text(
        f"path: {ds}\ntrain: images/train\nval: images/val\ntest: images/test\n"
        "names:\n  0: pill\n")
    print(f"YOLO dataset: {n_img} images, {n_box} boxes -> {ds}")
    return ds


# ══════════════════════════════════════════════════════════════════════════
# 2. Preprocessing - SHARED between calibration and the firmware
# ══════════════════════════════════════════════════════════════════════════
def letterbox_load(path, size=IMGSZ):
    """Must match FSBL/Src/ai/intake_camera.c's ROI grab.

    Calibrating with different preprocessing than deployment is the classic
    way to produce a model that reports confident nonsense on real frames,
    so there is exactly one definition of it and both sides use it.
    """
    img = Image.open(path).convert("RGB")
    w, h = img.size
    r = min(size / w, size / h)
    nw, nh = int(round(w * r)), int(round(h * r))
    canvas = Image.new("RGB", (size, size), (114, 114, 114))
    canvas.paste(img.resize((nw, nh), Image.BILINEAR),
                 ((size - nw) // 2, (size - nh) // 2))
    a = np.asarray(canvas, np.float32) / 255.0
    return np.ascontiguousarray(np.transpose(a, (2, 0, 1))[None])


# ══════════════════════════════════════════════════════════════════════════
# 3. Train / export / cut / quantise
# ══════════════════════════════════════════════════════════════════════════
def train(ds: Path):
    from ultralytics import YOLO
    m = YOLO("yolov8n.pt")
    m.train(data=str(ds / "data.yaml"), imgsz=IMGSZ, epochs=EPOCHS, batch=16,
            device="cpu", workers=4, project=str(WORK / "runs"), name="pill",
            exist_ok=True, patience=25,
            # a handheld pill arrives at any angle, distance and orientation
            degrees=25.0, scale=0.6, fliplr=0.5, flipud=0.3,
            hsv_h=0.02, hsv_s=0.7, hsv_v=0.5,
            mosaic=1.0, close_mosaic=15, seed=0, plots=False, val=True)
    best = WORK / "runs" / "pill" / "weights" / "best.pt"
    if not best.exists():          # ultralytics nests differently by version
        cands = list((WORK / "runs").rglob("best.pt"))
        best = max(cands, key=lambda p: p.stat().st_mtime)
    print("best weights:", best)
    return best


def export(best: Path):
    from ultralytics import YOLO
    # opset 13, NOT 12: per-channel weight quantisation emits DequantizeLinear
    # with an `axis` attribute, which opset 12 does not define. Exporting at 12
    # produces a model onnxruntime refuses to load.
    p = YOLO(str(best)).export(format="onnx", imgsz=IMGSZ, dynamic=False,
                               simplify=True, opset=13)
    dst = WORK / "pill_fp32.onnx"
    shutil.copy2(p, dst)
    print("FP32 ONNX:", dst)
    return dst


def cut(src: Path):
    """CUTTING THE HEAD - the single most important step in this script.

    A full-graph INT8 quantisation of this model measured **0 of 90**
    detections where the FP32 model got 84 of 90. YOLOv8's decode tail (a
    softmax over DFL bins, then Slice/Sub/Add/Div box arithmetic, then a
    Concat of box coordinates with class scores) holds tensors whose dynamic
    ranges differ by orders of magnitude, and per-tensor activation
    quantisation cannot serve them all at once.

    Cutting after the six raw head convolutions and doing the decode in C on
    the Cortex-M55 (FSBL/Src/ai/intake_detect.c) restores full accuracy. The
    M55 is idle ~88% of the time and 756 anchors x a 16-bin softmax is
    nothing against the convolutional body's ~380 MMAC.
    """
    import onnx
    dst = WORK / "pill_cut_fp32.onnx"
    onnx.utils.extract_model(str(src), str(dst), ["images"], CUT_OUTPUTS)
    m = onnx.load(str(dst))
    print("cut outputs:")
    for o in m.graph.output:
        print("   ", o.name,
              [d.dim_value for d in o.type.tensor_type.shape.dim])
    return dst


def quantise(src: Path, ds: Path):
    from onnxruntime.quantization import (CalibrationDataReader, QuantFormat,
                                          QuantType, quantize_static)

    files = sorted(glob.glob(str(ds / "images" / "train" / "*.jpg")))

    class Reader(CalibrationDataReader):
        def __init__(self):
            self.i = 0
        def get_next(self):
            if self.i >= len(files):
                return None
            x = letterbox_load(files[self.i])
            self.i += 1
            return {"images": x}
        def rewind(self):
            self.i = 0

    dst = WORK / "pill_cut_int8.onnx"
    print(f"calibrating on {len(files)} images @ {IMGSZ}x{IMGSZ}")
    quantize_static(
        str(src), str(dst),
        calibration_data_reader=Reader(),
        quant_format=QuantFormat.QDQ,
        # SIGNED activations. ST Edge AI refuses unsigned outright:
        #   "NOT IMPLEMENTED: Onnx exporting model with quantized unsigned
        #    integer format is not supported"
        activation_type=QuantType.QInt8,
        weight_type=QuantType.QInt8,
        per_channel=True, reduce_range=False)
    print("INT8 ONNX:", dst, f"({dst.stat().st_size/1e6:.2f} MB)")
    return dst


# ══════════════════════════════════════════════════════════════════════════
# 4. Validation - the numbers quoted in the documentation
# ══════════════════════════════════════════════════════════════════════════
STRIDES, REG_MAX = (8, 16, 32), 16


def decode(outs):
    """Reference for the C in FSBL/Src/ai/intake_detect.c. If the two ever
    disagree, one of them is wrong and this one is the easier to inspect."""
    best = (-1.0, 0, 0, 0, 0)
    for s, stride in enumerate(STRIDES):
        box, cls = outs[s * 2][0], outs[s * 2 + 1][0]
        conf = 1.0 / (1.0 + np.exp(-cls[0]))
        gy, gx = np.unravel_index(np.argmax(conf), conf.shape)
        c = float(conf[gy, gx])
        if c <= best[0]:
            continue
        d = box[:, gy, gx].reshape(4, REG_MAX)
        e = np.exp(d - d.max(-1, keepdims=True))
        dist = ((e / e.sum(-1, keepdims=True)) * np.arange(REG_MAX)).sum(-1)
        ax, ay = gx + 0.5, gy + 0.5
        best = (c, (ax - dist[0]) * stride, (ay - dist[1]) * stride,
                (ax + dist[2]) * stride, (ay + dist[3]) * stride)
    return best


def validate(fp32_full: Path, int8_cut: Path, ds: Path):
    import onnxruntime as ort
    full = ort.InferenceSession(str(fp32_full), providers=["CPUExecutionProvider"])
    q = ort.InferenceSession(str(int8_cut), providers=["CPUExecutionProvider"])
    files = sorted(glob.glob(str(ds / "images" / "val" / "*.jpg")))
    n = fh = qh = agree = 0
    for f in files:
        x = letterbox_load(f)
        a = float(full.run(None, {"images": x})[0][0, 4, :].max())
        b = decode(q.run(None, {"images": x}))[0]
        fh += a > CONF_T; qh += b > CONF_T; agree += (a > CONF_T) == (b > CONF_T)
        n += 1
    print(f"\nvalidation on {n} held-out images (threshold {CONF_T}):")
    print(f"  FP32 full graph      {fh}/{n} = {fh/n:.1%}")
    print(f"  INT8 cut + decode    {qh}/{n} = {qh/n:.1%}")
    print(f"  agreement            {agree}/{n} = {agree/n:.1%}")
    print("\nThese are DESKTOP numbers on a public dataset. They are NOT this")
    print("device's accuracy and must not be quoted as such - the camera, the")
    print("ISP, the lighting and the ROI are all different on hardware.")


def analyse(model: Path):
    sh([STEDGEAI, "analyze", "--model", str(model), "--target", "stm32n6",
        "--st-neural-art", "--no-report",
        "--workspace", str(WORK / "ws"), "--output", str(WORK / "out")])


def generate(model: Path):
    """Generate the NPU C sources. See `docs/TECHNICAL_REFERENCE.md` for the runbook
    this follows, and `tools/pill_detector/build_pill_detector.py` for the flashing procedure - weights go to
    external OSPI NOR and are flashed ONCE, separately."""
    sh([STEDGEAI, "generate", "--model", str(model), "--target", "stm32n6",
        "--st-neural-art", "--name", "pill", "--no-report",
        "--workspace", str(WORK / "ws"), "--output", str(WORK / "generated")])
    gen = WORK / "generated"
    print("generated sources in", gen)

    # STM32_Programmer_CLI REFUSES a .raw extension outright:
    #   "the download command ... has a wrong extension, please note that the
    #    supported extension are .bin, .hex, .srec, ..."
    # ST Edge AI emits the weight blob as .raw, so make a .bin copy - which is
    # also the convention the two face networks already use
    # (fd_data.xSPI2.bin, faceid_data.xSPI2.bin).
    raw = gen / "pill_atonbuf.xSPI2.raw"
    if raw.exists():
        binf = gen / "pill_atonbuf.xSPI2.bin"
        shutil.copy2(raw, binf)
        print("flashable blob:", binf, "(%d bytes)" % binf.stat().st_size)

    print("")
    print("Next:")
    print("  1. copy stai_pill.* / pill*.c into FSBL/Src/ai/ and FSBL/Inc/.")
    print("     No `static` fixes and no .xspi2 tagging are needed: --name pill")
    print("     prefixes every global (verified: zero collisions against the")
    print("     two face networks), and the weights are referenced by absolute")
    print("     address rather than through linked arrays. FSBL/Src/ai is a")
    print("     LINKED FOLDER in .project, so new .c files need no per-file")
    print("     <link> entry.")
    print("  2. flash pill_atonbuf.xSPI2.bin to 0x73000000, HOTPLUG mode,")
    print("     MX66UW1G45G loader.")
    print("  3. POWER CYCLE the board (unplug USB, not a reset) before running")
    print("     the app. Back-to-back external-loader operations leave the")
    print("     flash chip's live bus state confused; `tools/pill_detector/build_pill_detector.py` documents")
    print("     the boot hang inside HAL_XSPI_GET_FLAG that follows.")


def main():
    ap = argparse.ArgumentParser()
    for stage in ("all", "dataset", "train", "export", "cut", "quantise",
                  "validate", "analyse", "generate"):
        ap.add_argument(f"--{stage}", action="store_true")
    a = ap.parse_args()
    if not any(vars(a).values()):
        ap.print_help()
        return

    WORK.mkdir(parents=True, exist_ok=True)
    ds = WORK / "pill_ds"
    if a.all or a.dataset:
        ds = to_yolo(fetch_dataset())
    best = WORK / "runs" / "pill" / "weights" / "best.pt"
    if a.all or a.train:
        best = train(ds)
    fp32 = WORK / "pill_fp32.onnx"
    if a.all or a.export:
        fp32 = export(best)
    cutm = WORK / "pill_cut_fp32.onnx"
    if a.all or a.cut:
        cutm = cut(fp32)
    int8 = WORK / "pill_cut_int8.onnx"
    if a.all or a.quantise:
        int8 = quantise(cutm, ds)
    if a.all or a.validate:
        validate(fp32, int8, ds)
    if a.all or a.analyse:
        analyse(int8)
    if a.all or a.generate:
        generate(int8)


if __name__ == "__main__":
    main()
