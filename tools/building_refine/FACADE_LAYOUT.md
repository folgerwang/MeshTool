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

### Restore a damaged roof from the archived capture

When a generated roof is worse than the archived original, a reviewed profile
can bind both geometry hashes and a world-height transition. The restoration
clips triangles at that height, interpolates texture coordinates, and retains
the current lower facades. Explicit source-mesh exclusions in the profile must
be justified by visual inspection; building 278 excludes one isolated floating
triangle above its actual rooftop structures.

```powershell
python tools/building_refine/restore_captured_roof.py previous/city.mtscene tools/building_refine/building_profiles/building_278_roof.json repaired_city
python -m unittest discover -s tools/building_refine -p test_restore_captured_roof.py
```

This is a local restoration of an existing scan, not a closed-shell rebuild.
Reports retain boundary-edge counts and compare total open-edge length because
clipping subdivides edges. New nonmanifold/degenerate geometry or greater open
boundary length is rejected; a closed input must remain closed. Saved-scene
checks verify the repaired geometry, other objects and route. The active facade
catalog is clipped as well, with restored roof meshes recorded separately.

### Fit complete building envelopes

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

For a tree-free city, remove all tree-class meshes from every model variant:

```powershell
python tools/building_refine/remove_trees.py previous/city.mtscene tree_free_city
python -m unittest discover -s tools/building_refine -p test_remove_trees.py
```

The new scene retains ground surfaces, buildings, texture payloads, object IDs
and any saved route. `tree_removal.json` records removed objects and verifies
zero remaining tree meshes after reloading the saved scene. This does not
remove canopy photographs from an unprocessed capture; use the clean-paving
composition workflow first for those scenes.

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

The 511 profile uses `column_groups` for wide glazing and narrow service windows
on the same facade. Each group has its own measured columns, width, optional
height and vertical offset, while sharing the fitted floor spacing. Optional
`floor_fit_faces` selects reviewed facades with reliable full-height windows so
small offset openings do not bias that spacing. Overlapping groups are rejected.
Finishing preserves these reviewed motif positions instead of rectifying them
again. The profile covers seven observed upper rows; unobserved ground-floor
openings are not extrapolated.

### Measured stepped facades for building 218

`building_profiles/building_218_stepped.json` binds the archived capture geometry
to a measured courtyard envelope, roof levels, 104 windows and four reviewed doors.
The automatic flat-roof fit rejects this capture; the measured profile retains
the courtyard, rear setback, narrow eastern facade wing and major roof blocks.
Small roof equipment and classical ornament are simplified. A second facade review adds
24 omitted openings on the ground-floor frontage, lower side wall, upper courtyard,
tower attic and roof-level service walls. Unobserved lower courtyard rows remain blank.

`repair_stepped_building.py` accepts optional `facades` entries containing an
exact facade frame, capture PNG hash, metric opening bounds, rectangular or
arched shape, and measured mullions. Unlisted faces receive no guessed windows.
It rejects frame/image mismatches, overlapping slots, changed opening positions,
obstructed cutouts and nonplanar walls. `facade_details: true` adds inferred sills,
surrounds and courses while preserving the reviewed slots. Clean wall materials
remove the photographed surface fragments; roof imagery is retained.

The latest 218 profile regularizes the captured motifs into exact repeated
rows and columns, with matching opening sizes within each group. The broad
front glazing uses three columns and five rows; the central front uses five
columns and three rows. `layout_regularization`
records the groups and maximum adjustment from the prior measured bounds.
`window_style` sets a local frame width and frame, mullion, glazing and reveal
RGB colors without changing the defaults used by other buildings. The 218
style uses 0.16 m pale frames, consistent pane divisions and dark blue glass
to keep the window layout visible at the building overview scale. Four captured
entrances have opaque paneled doors with glazed transoms above them; panel and
transom subdivisions are inferred. Reviewed doors must meet the building base
or an evidenced landing. The two added front transoms raise the window count
from 102 to 104. `door_leaf_rgb` controls their opaque leaf finish.

Validation checks actual installed frame, glazing and door triangles against
every opening outline, with a 2 mm edge tolerance and at most 0.001 square meters
of uncovered area. Prototype presence alone does not establish that a hole is
filled. Cutout obstruction, closed topology and planar walls are checked too.

```powershell
python tools/building_refine/repair_stepped_building.py build/city-building-218-regular-v4-20261022/city.mtscene tools/building_refine/building_profiles/building_218_stepped.json build/city-building-218-openings-v5-20261022
python -m unittest discover -s tools/building_refine -p test_repair_stepped_building.py
```

### City artifact repair

`repair_city_artifacts.py` audits every active building, including installed
opening coverage on closed envelopes. Damaged buildings are fitted against the
archived capture using orthogonal, polygonal and stepped candidates. Roof support,
footprint agreement and side silhouettes gate replacements. Unsupported partial
captures remain unchanged and are listed as `needs_measured_review`.

Accepted buildings receive clean planar wall finishes, capture-supported opening
layouts, pale frames, dark glazing and clipped facade trim. The generator removes
collapsed, zero-area finishing triangles without moving vertices or UVs. A repair
also skips decorative trim pieces that collapse at float32 export precision;
the window and door assemblies remain mandatory. A repair
must still pass complete topology, opening coverage and planar-wall checks after
serialization. Captured entrances need ground support; no doors are guessed on
unobserved facades. Building 278's individually restored captured roof is protected
from generic envelope replacement.

The output includes `audit_before.json`, per-building captures and assets,
`repair_report.json`, `manifest.json` and the installed `city.mtscene`. The report
distinguishes repaired buildings from unchanged buildings and failed fits.
Archived captures, unselected objects, terrain, routes and existing texture
payloads are verified unchanged. The saved city is checked against the in-memory
result before completion.

```powershell
python tools/building_refine/repair_city_artifacts.py build/city-building-218-openings-v5-20261022/city.mtscene build/city-artifact-repairs-20261022
python -m unittest discover -s tools/building_refine -p test_repair_city_artifacts.py
```

```powershell
python -m unittest discover -s tools/building_refine -p test_city_iteration.py
```
