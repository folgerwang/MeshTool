# Facade layout refinement

Use the clean facade path on a fitted PCG shell whose UVs still reference the
capture. It emits shared window prototypes and per-face instances in `tiles.json`;
the current scene renderer expands those instances to meshes.

The city reconstruction workflow exposes this mode with `--procedural-facades`:

```powershell
python tools/building_refine/city_rules.py reference.mtscene capture.mtscene landmark.mtscene output_city --procedural-facades
```

This disables captured wall and glazing transfers for rebuilt facades. Existing
preserved/calibrated buildings follow their established profile policy. Roof
reconstruction keeps its existing materials.

```powershell
python tools/building_refine/capture_tiles.py input.mtscene output_facades --clean-materials --preserve-roofs --semantic-review
```

Each face now includes a `pattern` report with independently fitted row phases,
bay spacing, floor spacing, fit error, supporting observations, and a repeating
row cycle where at least three observations support every cycle class. Missing
bays and outliers do not force a different grid. Template matches require observed
row/column support. Bounded row gaps also require capture tone and contrast, so
a blank section between repeated windows stays blank.

`wall_finish` estimates RGB from covered wall pixels after excluding openings
and colored details. Repeated joints in both directions produce a tile hypothesis
and a synthesized mortar pattern. Other walls get a solid paint/render estimate.
These materials contain no captured image patches. Tile versus stone and paint
versus render remain uncertain; the report records that uncertainty.

Coplanar wall patches with the same affine UV mapping are combined before
window analysis. Texture discontinuities and displaced planes remain separate.
This allows a window spanning several capture triangles to be detected as one
opening. Projection frames are orthonormal even on tilted walls. Pixels outside
the actual wall triangles do not contribute to wall material measurements.

Faces without accepted openings retain their original vertices and translations,
preventing seam damage from flattening or float32 rebasing. Nonplanar patches
record `plane_deviation_m` and keep their triangles rather than cutting planar
holes. Existing native glass and interiors keep their semantic materials and use
sampled procedural colors; `retained_native_parts` records their mesh counts.
These mesh counts are distinct from newly detected window-instance counts.
Use `--preserve-roofs` for facade-only work to retain captured roof detail.

For a complete city containing archived original/refined variants, use:

```powershell
python tools/building_refine/convert_city_facades.py input.mtscene new_city_directory
```

This converts every building's active variant (refined variant 2 when available,
otherwise shared variant 0), retaining archived variant 1 and unrelated objects.
It keeps roofs and native glass/interiors, checks each building's topology, and
records any rejected proposal as a material-only fallback or retained original.
Large batches omit per-face PNGs; render the exported `city.mtscene` for review.
`progress.json`, `building_results.json`, and the final `manifest.json` distinguish
converted buildings, fallbacks, and objects with no active geometry. After fixing
a rejection, retry only those buildings with:

```powershell
python tools/building_refine/convert_city_facades.py new_city_directory --retry-fallbacks
```

Missing tower/podium faces can be rebuilt from a measured stepped envelope:

```powershell
python tools/building_refine/repair_stepped_building.py city.mtscene tools/building_refine/building_profiles/building_188_stepped.json repaired_city_directory
```

The profile is bound to the archived building's geometry hash. Roof footprints
may contain multiple polygons and courtyards. Shared boundary vertices keep
stepped roof/wall joins closed. The command requires zero open edges,
nonmanifold edges, and degenerate triangles before and after window installation.
It replaces only the selected active building, preserving its archived original,
other objects, and route. Window positions come from the projected original
capture; walls are triangulated with actual holes for the framed/recessed glass.
Repeated wide horizontal banks have separate width/aspect limits, while filled
component interiors must still show glazing contrast to reject outlined panels.

Colored wall marks become separate `wall_detail` prototypes and instances, with
their own semantic meshes and sampled flat colors. This conservatively captures
simple signs and painted accents. It does not reconstruct lettering or claim a
mark is a particular object. Dark vents, subtle relief, and complex murals may
remain undetected. The semantic review colors these details magenta.

Doors remain candidates requiring observed pixels at the building base. For a
measured patio/landing, callers can pass `support_levels={face_id: [world_z]}` to
`refine_building`. Elevated doors require that explicit support; a roof setback
alone does not establish access. Door instances record their support elevation.

For a faster image-only iteration, use a rectified capture and its metric pixel
pitch. Every run needs a new output directory:

```powershell
python tools/building_refine/facade_layout.py face_capture.png review_pass_01 --pitch 0.05 --ground-offset 0
```

Inspect `procedural_preview.png` and compare `analysis.json` between passes:
opening counts, recovered counts, spacing errors, row support, wall finish, and
detail candidates. The image-only preview is a flat diagnostic; the scene path
creates framed, recessed window geometry. Use `--ground-offset` for elevated
facades so their lower edge is not mistaken for ground level. The original
capture and previous outputs remain intact.

Validation:

```powershell
python -m unittest discover -s tools/building_refine -p test_facade_layout.py
python -m unittest discover -s tools/building_refine -p test_capture_tiles.py
python -m unittest discover -s tools/building_refine -p test_facade_iteration.py
```
