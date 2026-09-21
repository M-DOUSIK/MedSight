# tools/pill_detector/ - the complete corresponding source

`build_pill_detector.py` reproduces the whole path that produced the pill
detector shipped in this firmware: dataset fetch, YOLO conversion, training,
ONNX export, head cut, INT8 quantisation, validation, and ST Edge AI
generation. The `.json` and `.mpool` files are the memory-pool profiles the
Neural-ART compiler was driven with.

**This directory exists for a licence reason as well as a practical one.**
The detector's trained weights were produced by this project, using
Ultralytics YOLOv8, which is AGPL-3.0. AGPL-3.0 asks for the complete
corresponding source of the derivative work, including the weights. This is
it, and publishing it was going to happen anyway.

`../../docs/LICENSING.md` section 3 is the full resolution of that question,
including what it does **not** resolve.

Training data: the "pills" dataset, Roboflow Universe (`pills-sxdht`, RF100),
451 images, **CC BY 4.0**, obtained via the HuggingFace mirror
`Francesco/pills-sxdht`.
