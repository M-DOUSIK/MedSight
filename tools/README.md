# tools/

What a recipient needs to reproduce what this project built. Nothing here is
required to compile the firmware - the UI assets are pre-generated and
committed, so a clone builds and flashes as-is.

| | |
|---|---|
| `pill_detector/` | the complete training and export pipeline for the pill detector we trained |
| `gen_ui_assets.py` | cuts the designer's artwork into sprites and fonts, producing `ui_assets.h` and `ui_assets_data.inc` |
| `img2sprite.py` | the single-image converter underneath it |
