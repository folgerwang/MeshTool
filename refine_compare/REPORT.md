# Refine Buildings: before / after comparison

Scene: the downtown San Francisco capture from 2026-10-02 (`last_input.mtscene`
with its planet-scale overview group removed), segmented headlessly, then
refined with `MeshTool --refine` (SAM2 outlines, CLIP glass). Renders of both
scenes use identical cameras (`--shots --frame/--box`).

Images in this folder:
- `building_NNN.png` - one sheet per refined building: captured (left) vs
  refined (right), two opposite views, with its numbers in the title bar.
- `cmp_*.png` - wider before/after views.
- `rejected_building_037.png` - a large building refinement left alone.
- `topdown_objects.png` (in the scratch report folder) - object map; `*` marks
  refined buildings.

## 1. What refinement did

| | value |
|---|---|
| buildings in scene | 294 (+ 126 trees, 159 cars) |
| replaced by a model | **27 (9%)**, 39 482 m² of 163 094 m² footprint (24%) |
| facade fragments absorbed into them | 10 |
| triangles on those buildings | 197 762 -> 26 924 (7.3x fewer) |
| glass facades detected | 11 buildings, 6 233 m² on the 134 m tower |
| run time | 96 s (RTX 5090) |

Why the other 267 kept their captured mesh (footprint bins):

| reason | <100 m² | 100-400 | 400-1500 | >1500 m² |
|---|---|---|---|---|
| too small / no clear outline | 168 | 16 | 0 | 1 |
| at the edge of the capture | 30 | 9 | 8 | 6 |
| walls do not match captured facades (<65%) | 1 | 2 | 11 | 9 |
| outline / height misfit | 0 | 2 | 1 | 0 |
| blank walls (>N m² with nothing captured) | 0 | 0 | 0 | 3 |
| **refined** | 3 | 2 | 7 | 15 |

The 20 "walls do not match" rejections of large buildings are the biggest
untapped group; 11 of them scored 51-63%, just under the 65% threshold.
Largest buildings not refined (not at the edge): 039 (12 755 m²), 082 (11 970),
139 (6 224), 117 (4 782), 077 (3 369), 168, 192, 150, 073, 211, 088, 037.

## 2. Geometric accuracy of the 27 models (independent of the refiner's own checks)

Surfaces sampled at 0.15 m; distances are point-to-nearest-sample.

- **Captured shape the model misses** (capture -> model): median 0.09-0.36 m,
  90th percentile 0.2-2.5 m per building. This is detail a planar model cannot
  hold: setbacks, balconies, folded glass facades (158: p90 1.9 m), roof
  clutter. 5-21% of the captured surface is more than 1 m from the model on
  the worst buildings (185, 158, 226, 048).
- **Model surface not on any capture** (model -> capture): 19% of model area
  on average is more than 1 m from its own building's capture. Of that, 6% is
  walls enclosed inside the model (steps between roof parts, invisible) and
  13% is visible wall; **10 of those 13 points are wall standing against
  another building object** (party walls). Extreme cases: 091 (39% of its
  surface is wall against 088/110/100), 014 (38%, against 004), 136 (26%).
  These walls get fill colour because nothing was captured there.
- Roof surfaces are accurate everywhere (invented roof area ~0%).

## 3. Merging neighbours: what the data says

You suggested merging building objects that are physically one building.
Measured on the top-down object map (0.25 m):

- 233 adjacent building pairs share >= 2 m of boundary.
- **Split kinds.** Of 91 straight shared boundaries (>= 8 m, RMS < 0.6 m),
  80 run at 30-45° to north - along the street grid - and only 3 along
  Google Earth's N-S/E-W tile grid. Tile cuts therefore do not split roofs
  here (the segmenter stitches roofs across tiles). What tiles do split off
  are **facades**: wall pieces cut from their roof by a tile edge become their
  own objects. 145 of the 294 building objects are such slivers (mean width
  < 2.5 m, >= 20% of their outline against a building); they are 168 of the
  "too small" rejections.
- **Segmentation over-splits** show as a flat boundary: 43 pairs have a roof
  step < 1.5 m along > 60% of a >= 5 m boundary (e.g. 205|233, 045|053,
  136|153, 284|294 at 0.0-0.1 m step). 10 of them have one side refined and
  the other not.
- **Real neighbours** share long, straight boundaries with a big roof step
  (073|039: 35 m, 39 m step; 158|150: 75 m, 83 m step). Boundary length alone
  cannot tell them from parts of one building.

Experiment (`merge_buildings.py`): slivers absorbed into their neighbour, plus
pairs merged when the shared boundary was >= 8 m and >= 25% of the smaller
outline. 294 -> 149 objects, then refined again:

| | original | merged |
|---|---|---|
| buildings refined | 27 | 14 |
| footprint refined | 39 482 m² | 25 566 m² |

Sliver absorption gained 6 buildings (077, 078, 103, 127, 205, 258 - several
were "no clear outline" before). The boundary-length rule lost 19: it glued
towers to their podiums and neighbours (158 into 150, 247 into 226, 091+088+110),
and the merged objects then failed the blank-wall check (158: 4 810 m² of
walls with nothing captured) or the wall-fit check.

## 4. Recommendations for the model

1. **Absorb facade slivers before modelling, not after.** Width < 2.5 m and
   >= 20% of the outline against one building -> that building. Removes ~145
   bogus objects and gave +6 refined buildings here. This is the tile-split
   fix.
2. **Merge roof parts only on roof continuity**, never on boundary length:
   step < 1.5 m along > 60% of a >= 5 m boundary. Keeps towers off podiums
   (step 40-110 m) and real neighbours apart; targets the 43 over-split pairs.
3. **Treat party walls explicitly.** Walls facing another building object are
   10% of model area on average (39% on 091): either drop them, or texture
   them from the neighbour and exclude them from the blank-wall measure. The
   blank-wall rejection currently penalises exactly the buildings that sit in
   a block.
4. **Revisit the 65% wall-fidelity threshold** on the 11 large buildings at
   51-63%, with their party-wall area excluded from the denominator first;
   the score may already pass once walls against neighbours are not counted.
5. Edge-of-capture (53) and planar-model detail loss (p90 up to 2.5 m) are
   inherent; the first needs more captures, the second is accepted (closer
   captures bring finer tiles; note the 8 cm bake resolution caps that).

## 5. Clean scene (`--clean`, hidden surfaces `--cull`)

Same scene, Refine Buildings with Clean scene on. Sheets in `clean/`
(captured+segmented left, clean right): `street2`, `tower158` show the streets,
`lowangle` the spot where the missing-texture checker was. Close-ups with 2x
detail crops in `clean/detail/`: `cars_street`, `cars_topdown` (parked cars and
buses -> road), `trees_sidewalk`, `tall_trees` (ground filled under trees),
`wallbase_019`, `wallbase_048`, `tower158_base` (refined walls meeting the
terrain), `checker_spot` (missing-texture checker), `road_topdown`.

Known leftovers visible in the close-ups: kerb-side lumps that the segmenter
fused into a building object (e.g. building_066, 33 m long) stay; under dense
trees LaMa continues the nearest surface (a brick sidewalk, a red bus lane), which
is plausible paving but not the true ground; `road_oblique` looks into a building
interior and is not useful. The geometric fill is kept as a fallback (`--no-lama`
or no GPU) and is visibly worse: it smears the rim colour and erases markings.

| | value |
|---|---|
| cars removed | 286 objects: 156 labelled car + 130 vehicle-sized "buildings"/"trees" (buses, vans) relabelled; 2 133 m² of surface filled |
| trees kept | 101 (5 342 m² of ground filled under them) |
| surface clutter flattened | 1 677 blobs, 3 307 m² (raised blobs under 60 m²: people, bins, parked-car rows left in a ground class) |
| wall/roof pieces labelled ground, removed | 139 m² |
| terrain mesh | 1 040 522 triangles, 3 meshes (ground, road, plants), one 2048² page |
| captured ground/road/car meshes replaced | 8 049 |
| placeholder-textured triangles | 689 471 (46%): drawn behind textured twins, recoloured; 32 859 at ground level dropped |
| hidden surfaces removed | 371 253 of 1 545 025 triangles (24%) |
| hole fill (orthophoto) | LaMa image inpainting, 1.48 M px in 5.3 s on the GPU: lane markings, kerbs and paving continue across filled areas |
| placeholder colour left on the visible terrain texture | yellow 14 m², near-black 84 m² (was 118 / 769) |
| terrain height spikes (> 3 m above local median) | 11 vertices of 521 825 |
| buildings refined | 28 of 189 (walls set onto the terrain) |
| run time | 96 s (RTX 5090) |

Why the other 267 kept their captured mesh (footprint bins):

| reason | <100 m² | 100-400 | 400-1500 | >1500 m² |
|---|---|---|---|---|
| too small / no clear outline | 168 | 16 | 0 | 1 |
| at the edge of the capture | 30 | 9 | 8 | 6 |
| walls do not match captured facades (<65%) | 1 | 2 | 11 | 9 |
| outline / height misfit | 0 | 2 | 1 | 0 |
| blank walls (>N m² with nothing captured) | 0 | 0 | 0 | 3 |
| **refined** | 3 | 2 | 7 | 15 |

The 20 "walls do not match" rejections of large buildings are the biggest
untapped group; 11 of them scored 51-63%, just under the 65% threshold.
Largest buildings not refined (not at the edge): 039 (12 755 m²), 082 (11 970),
139 (6 224), 117 (4 782), 077 (3 369), 168, 192, 150, 073, 211, 088, 037.

## 2. Geometric accuracy of the 27 models (independent of the refiner's own checks)

Surfaces sampled at 0.15 m; distances are point-to-nearest-sample.

- **Captured shape the model misses** (capture -> model): median 0.09-0.36 m,
  90th percentile 0.2-2.5 m per building. This is detail a planar model cannot
  hold: setbacks, balconies, folded glass facades (158: p90 1.9 m), roof
  clutter. 5-21% of the captured surface is more than 1 m from the model on
  the worst buildings (185, 158, 226, 048).
- **Model surface not on any capture** (model -> capture): 19% of model area
  on average is more than 1 m from its own building's capture. Of that, 6% is
  walls enclosed inside the model (steps between roof parts, invisible) and
  13% is visible wall; **10 of those 13 points are wall standing against
  another building object** (party walls). Extreme cases: 091 (39% of its
  surface is wall against 088/110/100), 014 (38%, against 004), 136 (26%).
  These walls get fill colour because nothing was captured there.
- Roof surfaces are accurate everywhere (invented roof area ~0%).

## 3. Merging neighbours: what the data says

You suggested merging building objects that are physically one building.
Measured on the top-down object map (0.25 m):

- 233 adjacent building pairs share >= 2 m of boundary.
- **Split kinds.** Of 91 straight shared boundaries (>= 8 m, RMS < 0.6 m),
  80 run at 30-45° to north - along the street grid - and only 3 along
  Google Earth's N-S/E-W tile grid. Tile cuts therefore do not split roofs
  here (the segmenter stitches roofs across tiles). What tiles do split off
  are **facades**: wall pieces cut from their roof by a tile edge become their
  own objects. 145 of the 294 building objects are such slivers (mean width
  < 2.5 m, >= 20% of their outline against a building); they are 168 of the
  "too small" rejections.
- **Segmentation over-splits** show as a flat boundary: 43 pairs have a roof
  step < 1.5 m along > 60% of a >= 5 m boundary (e.g. 205|233, 045|053,
  136|153, 284|294 at 0.0-0.1 m step). 10 of them have one side refined and
  the other not.
- **Real neighbours** share long, straight boundaries with a big roof step
  (073|039: 35 m, 39 m step; 158|150: 75 m, 83 m step). Boundary length alone
  cannot tell them from parts of one building.

Experiment (`merge_buildings.py`): slivers absorbed into their neighbour, plus
pairs merged when the shared boundary was >= 8 m and >= 25% of the smaller
outline. 294 -> 149 objects, then refined again:

| | original | merged |
|---|---|---|
| buildings refined | 27 | 14 |
| footprint refined | 39 482 m² | 25 566 m² |

Sliver absorption gained 6 buildings (077, 078, 103, 127, 205, 258 - several
were "no clear outline" before). The boundary-length rule lost 19: it glued
towers to their podiums and neighbours (158 into 150, 247 into 226, 091+088+110),
and the merged objects then failed the blank-wall check (158: 4 810 m² of
walls with nothing captured) or the wall-fit check.

## 4. Recommendations for the model

1. **Absorb facade slivers before modelling, not after.** Width < 2.5 m and
   >= 20% of the outline against one building -> that building. Removes ~145
   bogus objects and gave +6 refined buildings here. This is the tile-split
   fix.
2. **Merge roof parts only on roof continuity**, never on boundary length:
   step < 1.5 m along > 60% of a >= 5 m boundary. Keeps towers off podiums
   (step 40-110 m) and real neighbours apart; targets the 43 over-split pairs.
3. **Treat party walls explicitly.** Walls facing another building object are
   10% of model area on average (39% on 091): either drop them, or texture
   them from the neighbour and exclude them from the blank-wall measure. The
   blank-wall rejection currently penalises exactly the buildings that sit in
   a block.
4. **Revisit the 65% wall-fidelity threshold** on the 11 large buildings at
   51-63%, with their party-wall area excluded from the denominator first;
   the score may already pass once walls against neighbours are not counted.
5. Edge-of-capture (53) and planar-model detail loss (p90 up to 2.5 m) are
   inherent; the first needs more captures, the second is accepted (closer
   captures bring finer tiles; note the 8 cm bake resolution caps that).

## 5. Clean scene (`--clean`, hidden surfaces `--cull`)

Same scene, Refine Buildings with Clean scene on. Sheets in `clean/`
(captured+segmented left, clean right): `street2`, `tower158` show the streets,
`lowangle` the spot where the missing-texture checker was. Close-ups with 2x
detail crops in `clean/detail/`: `cars_street`, `cars_topdown` (parked cars and
buses -> road), `trees_sidewalk`, `tall_trees` (ground filled under trees),
`wallbase_019`, `wallbase_048`, `tower158_base` (refined walls meeting the
terrain), `checker_spot` (missing-texture checker), `road_topdown`.

Known leftovers visible in the close-ups: kerb-side lumps that the segmenter
fused into a building object (e.g. building_066, 33 m long) stay; filled ground
under dense trees is a smooth colour, not paving detail; `road_oblique` looks
into a building interior and is not useful.

| | value |
|---|---|
| cars removed | 156 objects (800 m² of road filled) |
| trees kept | 126 (5 644 m² of ground filled under them) |
| surface clutter flattened | 1 503 blobs, 1 264 m² |
| wall/roof pieces labelled ground, removed | 139 m² |
| terrain mesh | 1 040 522 triangles, 3 meshes (ground, road, plants), one 2048² page |
| captured ground/road/car meshes replaced | 7 432 |
| placeholder-textured triangles | 689 471 (46%): drawn behind textured twins; 32 859 at ground level dropped |
| hidden surfaces removed | 376 006 of 1 559 837 triangles (24%) |
| placeholder colour left in the terrain texture | yellow 14 m², near-black 23 m² (was 118 / 769) |
| terrain height spikes (> 3 m above local median) | 12 vertices of 521 825 |
| buildings refined | 28 of 294 (walls set onto the terrain) |
| run time | 132 s |

What it took to get there (all in `tools/building_refine`):
- the captured ground is ~46% duplicated by Google Earth's dark overlay pass
  (coincident triangles, near-black texture) - the z-fighting in the viewer;
  textured twins now win every tie and the overlays are culled;
- tiles whose imagery had not streamed in carry a yellow/black checker; they
  are treated like the overlay and recoloured from neighbours;
- a median filter on terrain heights straddled facades and lifted the ground by
  4-10 m along them (spikes, raised footprints): removed;
- DXT5 textures were never decoded by the Python tools (sampled black): fixed.

## Reproduce

```
python scratch/cmp/city_only.py last_input.mtscene city_input.mtscene   # drop overview group
MeshTool --segment city_input.mtscene segmented.mtscene
MeshTool --refine  segmented.mtscene refined.mtscene
python scratch/cmp/rejections.py; geometry_error.py; invented.py; adjacency.py; merge_buildings.py
```
(`tools/building_refine` needs `numba` installed; CLIP needs the full
`openai/clip-vit-large-patch14` snapshot in the Hugging Face cache - the
scripts run offline and do not download it.)
