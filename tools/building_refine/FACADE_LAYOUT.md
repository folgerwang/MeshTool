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

## Architectural detail pass on an existing facade

`facade_details.py` adds inferred projecting window sills, shallow U-shaped
surrounds, floor courses, base trim and cornices to an existing isolated building
asset. It uses the catalog's actual wall triangles, skips blank/nonplanar faces,
and clips every detail away from openings, signs and other trim. The existing
meshes, roof, window slots and texture payloads are retained exactly. These trim
styles and depths are design inferences, not newly detected capture evidence.

The base directory must contain a manifest, iterations report and matching
`building_NAME.mtscene` asset from the repair/install workflow. Use new output
directories for each pass:

```powershell
python tools/building_refine/facade_details.py previous_city detail_trials --building building_434
python tools/building_refine/install_building_repairs.py previous_city/city.mtscene detail_trials detailed_city
python -m unittest discover -s tools/building_refine -p test_facade_details.py
```

`facade_details.json` records inferred detail counts, exact mesh preservation,
trim/opening clearance, wall planarity and closed topology. The exported asset
is reloaded and checked before installation. Reapplying details to an already
detailed catalog is rejected; start a new pass from the undetailed asset.

## Capture-scored city iteration

`iterate_building_shells.py` ranks buildings with mesh defects and evaluates
three envelope fits per building against its archived capture. Acceptance
requires a closed mesh, at least 0.90 footprint IoU, 0.87 roof-height agreement
within 1.5 m, and bounded silhouette changes from four sides. Sloped roofs and
fits with insufficient evidence retain the previous model. Accepted walls use
the capture-derived facade procedure above, including real opening cuts.

```powershell
python tools/building_refine/iterate_building_shells.py source.mtscene previous/manifest.json building_trials --limit 633
python tools/building_refine/terrain_scene.py source.mtscene terrain_pass --resolution 0.5 --step 2
python tools/building_refine/iterate_roads.py source.mtscene terrain_pass/0_0_terrain.npz road_evidence
python tools/building_refine/compose_iteration.py previous/city.mtscene terrain_pass road_evidence next_city --assets building_trials
python tools/building_refine/validate_city_iteration.py next_city/opening_validation.json building_trials
```

Each output directory must be new. Road evidence and composition currently
support one captured group. The road loop compares three gap-repair sizes,
penalizes ragged boundaries, fits bounded cubic boundary curves, and recovers
red, green, yellow and white paint from height-validated ground pixels at
0.25 m/pixel. Short colored-lane gaps are inferred only along observed strokes.
Scores measure agreement with the capture segmentation, not surveyed accuracy.
`roads/road_splines.json` exports sampled world-space centerline curves; the
saved driving route is preserved. Occluded/faded markings can remain incomplete.

Cars and captured trees are absent from the new scene. Procedural paving
removes residual vehicle/canopy photographs on the ground. Tree regions are
split into crown placements, grounded, and checked against building pixels;
implausible or unsupported labels are reported without invented replacements.
`trees/tree_instances.json` records prototype IDs and placement transforms.
`trees/trees.gltf` and `trees.bin` contain native shared-mesh instances, while
the MeshTool v7 preview expands the same placements into compatible meshes.
Trees are broadleaf approximations; their species is not inferred.

Vehicle-shaped geometry is not sufficient evidence to remove a building label.
For visually confirmed mislabeled vehicles, `compose_iteration.py` accepts
`--vehicle-review scene_profiles/vehicle_labels_20261021.json` (resolve this path
relative to `tools/building_refine`). Every listed object must match the profile's
exact geometry fingerprint before any is removed. The supplied review records
six vehicles from the matching city; pavement-shaped false positives are retained.

`validation.json`, `building_iterations.json`, and the opening check report
record accepted/rejected fits and preservation checks. Rejected buildings can
still contain earlier capture defects. Texture compaction removes unused and
duplicate payloads without changing retained material images.

### Targeted flat-wall repairs

For predominantly flat-roof buildings with missing captured walls, use the
original capture scene and explicitly select the objects:

```powershell
python tools/building_refine/iterate_building_shells.py original.mtscene previous/manifest.json planar_trials --only building_435,building_513 --planar
python tools/building_refine/install_building_repairs.py previous/city.mtscene planar_trials repaired_city
```

This mode fits continuous orthogonal wall planes from roof outlines and surviving
wall normals. It requires at least 80% flat roof support and checks three fit
tolerances. Acceptance requires at least 92% observed roof coverage, 90% height
agreement within 1.5 m, and at most 5% excess footprint. Small rooftop equipment
can be simplified; genuinely sloping main roofs are rejected. Side silhouettes
remain recorded, but missing captured walls may be completed when the roof
supports them. Completed unseen surfaces are inferred, not surveyed.

The installer verifies the original capture fingerprint, closed shell, opening
clearance, and wall planarity before replacing the selected models. It preserves
original capture variants and checks that other objects, their material payloads,
and the driving route remain unchanged. Each output directory must be new.

For triangular footprints, oblique facades, and complex recessed outlines,
use `--polygonal` instead of `--planar`. It follows simplified, capture-supported
roof boundaries without requiring perpendicular wall directions. Each boundary
segment still generates a continuous vertical wall plane. Substantial courtyards
are retained, and the same roof-coverage, height, closed-shell, and opening
checks apply. This avoids rejecting a valid angled building or forcing its
outline into a rectangle. The trial report records `boundary_mode` for review.

### Regular window motifs on an approved envelope

`repair_window_pattern.py` refines window positions while retaining an approved
building shape. A reviewed profile binds the original geometry, prior facade
catalog, roof layers, and capture images by hash. It fits common floor elevations
with one vote per facade/row and fits repeated single or paired window bays from
reviewed column observations. Missing motif slots are inferred inside the
supported facade; slots crossing an architectural corner are omitted. Ground
floor and blank-wall windows are not extrapolated.

```powershell
python tools/building_refine/repair_window_pattern.py original.mtscene previous/city.mtscene tools/building_refine/scene_profiles/building_434_window_pattern.json window_trials
python tools/building_refine/install_building_repairs.py previous/city.mtscene window_trials next_city
python -m unittest discover -s tools/building_refine -p test_regular_window_layout.py
```

The supplied 434 profile is specific to its approved polygonal repair. Facades
are matched by geometric frame because polygon serialization may reorder their
indices. Roof geometry, UVs, and images are retained exactly. Validation checks
the approved wall planes, exact fitted slots, closed topology, and clear wall
cutouts. `pattern_fit.json` records fitted spacings, observation residuals,
skipped corner slots, and inferred repetition. This is a reviewed geometric
fit, not a claim that every recovered window was visible in the capture.

```powershell
python -m unittest discover -s tools/building_refine -p test_city_iteration.py
```
