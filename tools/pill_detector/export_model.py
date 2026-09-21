import shutil
from pathlib import Path
from ultralytics import YOLO

# Source model from your training output run
SOURCE_BEST = Path("training/runs/detect/runs/detect/pill_yolov8n/weights/best.pt")
TARGET_DIR = Path("models/pill_detector")
TARGET_BEST = TARGET_DIR / "best.pt"

def main():
    TARGET_DIR.mkdir(parents=True, exist_ok=True)
    
    if not SOURCE_BEST.exists():
        print(f"[ERROR] Source model not found at: {SOURCE_BEST.resolve()}")
        return

    # Copy weights
    shutil.copy2(SOURCE_BEST, TARGET_BEST)
    print(f"[SUCCESS] Copied best weights to: {TARGET_BEST.resolve()}")

    # Export to ONNX for STM32N6 / ST Edge AI
    print("[INFO] Exporting to ONNX (320x320, opset 12)...")
    model = YOLO(TARGET_BEST)
    onnx_path = model.export(
        format="onnx",
        imgsz=320,
        dynamic=False,    # Fixed shape for NPU RAM allocation
        simplify=True,    # Simplify operators for ST Edge AI
        opset=12          # Standard opset
    )
    print(f"[SUCCESS] ONNX model created at: {onnx_path}")

if __name__ == "__main__":
    main()