# The printed parts

Three printed assemblies, designed in Autodesk Fusion.

Each part is here twice: an **STL** to print from, and a **STEP** to open and
change. The STL is a triangle mesh and cannot meaningfully be edited; the STEP
carries the solid geometry.

| Part | Print | Edit | Size (STL bounding box) | Triangles |
|---|---|---|---|---|
| Board enclosure | `board_enclosure.stl` | `board_enclosure.step` | **164.24 × 153.00 × 151.99 mm** | 10,766 |
| Pill dispenser mechanism | `pill_dispenser.stl` | `pill_dispenser.step` | **179.24 × 92.00 × 50.00 mm** | 5,136 |
| Structural pillars, 8 off | `structural_pillars.stl` | `structural_pillars.step` | **180.00 × 100.00 × 10.00 mm** | 96 |

`HARDWARE_DESIGN_SHEET.pdf` is a one-page annotated summary of all three.

Numbered callouts for every sub-part, and what each one does, are in
[`../../docs/HARDWARE_SPEC.md`](../../docs/HARDWARE_SPEC.md).

Printed FDM in PLA at 20% infill.

## Licensing

Original designs by this project, under **Apache-2.0**, the same as the rest of
MedSight's own work. See [`../../docs/LICENSING.md`](../../docs/LICENSING.md).
