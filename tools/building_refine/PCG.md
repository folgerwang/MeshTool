# PCG refinement in MeshTool

After **Tools > Segment Scene (AI)** completes, click a building in the viewport.
In the Selected Object panel choose **Improve selected building (PCG)...**, then
**Apply PCG**. The dialog names the target. For a batch, use **Tools > Improve
Buildings (PCG)...** and choose **All segmented buildings**.

The operation fits the capture's footprint, wall planes and roof parts, retains
projected captured textures, then adds metric facade trim, floor bands, coping,
rainwater hardware on shorter buildings, and rooftop HVAC on usable flat roofs.
Generated opaque finishes use Cook-Torrance GGX shading; glass keeps its separate
translucent, Fresnel reflection pass and interior backing. Glass detection requires
the existing CLIP dependencies and weights. An unavailable classifier is reported
in the refinement log and leaves the facade opaque rather than guessing.

Single-building operations use exact batch/group/object indices, not names, and
disable scene-wide cleaning/culling. Neighboring objects retain their geometry.
Fits that fail the replacement checks use captured-surface PBR and detail refinement. Reapplying
PCG replaces previous details; a rejected fit retains the previous result.
Details are consolidated into at most three meshes per building by finish. This
capture renderer does not yet use the RealWorld generator's GPU detail instancing.

The result opens in the editor. Use **File > Save Scene** to keep it. To see the
materials, turn off **Colour by class** or use **Actual** on a selected object.

Headless use:

```text
MeshTool_PCG.exe --refine in.mtscene out.mtscene --pcg --target 0:0:12
MeshTool_PCG.exe --refine in.mtscene out.mtscene --pcg
python tools/building_refine/test_pcg.py -v
```

`--no-sam` uses height-derived outlines; `--no-glass` disables CLIP detection.
`--clean` and `--cull` are scene-wide batch options and cannot be combined with
`--target` or `--only`.

## Compare original and refined models

After successful refinement, select the building and use **Model: Original /
Refined** in Selected Object, or the **Original model / Refined model** entries
in its right-click menu. The camera stays fixed; only one version is drawn and
pickable. **All originals / All refined** in the Objects panel switches the
whole batch. These controls also switch to material view for comparison.

Both versions are embedded by Save Scene in `.mtscene` version 5. Reopening
shows the refined versions by default. Existing scene versions 1ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã¢â‚¬Å“4 still load;
refine with this build to record a comparison pair. Rejected replacement fits use a conservative PCG fallback: the captured shell and
textures stay intact, with PBR finishes, welded surface-following facade bands, and roof equipment on
verified flat captured patches. Short isolated facade runs are excluded; openings
are retained. Details are bounded and consolidated by finish. Existing pairs stay unchanged if a repeated fit fails. Reapplying refinement retains the first recorded original.
Mesh export uses the currently displayed versions. Successfully resegmenting
a scene replaces the old object identities and their comparison history.

For an explicitly glass-clad building, enable **Reflective glass facade** in the refinement dialog (CLI: --reflective-glass). This overrides automatic detection on refined wall triangles while retaining opaque roofs, trim, and the original comparison model. Reflection defaults to 0.95; adjust it under Glass facades. The shader uses an analytic lighting environment, not reflections of neighboring scene objects.

## Tiled window facade replacement

PCG defaults to **Replace walls with tiled windows** in the editor. It first
recovers continuous facade planes from area-weighted wall directions and depth
clusters. Repeated windows cross internal scan-fragment boundaries instead of
being cropped separately inside every triangle patch. Strongly orthogonal
buildings use a shared set of wall directions; setbacks retain separate planes.
Supported walls are rebuilt with facade modules: fine-grain panel
textures, metal frames, glass recessed 14 cm, and dark interior backing.
Replaced wall triangles are absent from the refined version. Roofs and
unsupported surfaces retain their captured geometry. The full original is saved
for comparison. Disable this option to use the earlier trim-only workflow.

The facade is a procedural design, not a recovery of the building's real window
layout. Small capture cracks are closed and noisy boundaries simplified; larger
openings and disconnected wings remain separate. Wall positions are regularized
within bounded depth clusters instead of following every captured wrinkle.
Distorted roofs and larger missing surfaces are not reconstructed by this step.
Meshes are batched by material and shared textures are reused when reapplied.
This path does not need SAM/CLIP and skips scene cleanup. CLI: `--tiled-facade`.

### Capture-derived repetition

Before tiling, the generator projects the largest wall patches from the original
textures and estimates repeated spacing with autocorrelation. Contrast, coverage,
cycle count, and agreement checks reject weak patterns. Detected floor and bay
spacing drive the module grid. Multi-floor bands require agreement from at least
two facade patches; floor subdivision is explicitly reported as inferred. The
stronger spandrel repeats at that group cadence. Unknown spacing uses documented
defaults (1.6 m bay, 3.4 m floor), and the log states which values were detected,
inferred, or defaulted. Original textures, rather than generated tiles, drive
reapplication.

Fitted sill height adjusts ordinary rows while retaining the stronger band at
the detected multi-floor cadence. That band's datum comes from its own captured
phase, so window-height optimization cannot erase the grouped-floor design.

### Wall color and finish

Tiled panel color is estimated from bright structural regions of the original
facade, excluding dark glazing, clipped highlights, and glass-heavy shadow
patches when clearer cladding samples exist. This is an albedo estimate, not
a measured material identification. The default **Painted concrete** finish
uses nonmetallic, rough PBR shading and fine paint grain. **Brushed metal**
uses metallic shading with a directional grain texture. Select **Wall finish**
in the PCG dialog, or pass `--wall-finish painted-concrete` / `--wall-finish metal`.
Glass and metal window frames keep their own materials. Reapply regenerates
from the original and reuses matching texture atlases rather than stacking them.

Each generated glass pane lies on its recovered facade plane. When no supported
planes can be recovered, the older captured-surface method remains the fallback.

Internal diagonal mesh joins are stitched before window layout when neighboring
patches have compatible normals and bounded fit error. Vertical corners and
open boundaries remain separate. Geometry is mapped back onto the captured
surface to avoid introducing cracks. Each generated glass pane is planar, so
a warped source quad cannot split its reflection diagonally.

### Capture-typed modules, base, crown and roof

Each wall patch is projected from the original textures and every bay column
is classified from its captured luminance against the building's cladding
estimate: dark, blue columns become **glazed** curtain-wall modules (near
full-bay panes over a slim spandrel); bright columns become **clad** modules
(punched windows between wide piers); a lone clad bay between glazing becomes
a solid, slightly proud **pier**. A patch with little column contrast is one
class. Uncovered columns follow their nearest sampled neighbour; without any
capture every column is clad, as before. The log reports the module counts.

The lowest full-height module row on the building base becomes a storefront
(low sill, transom); a short partial row beneath it is a solid plinth. The
widest ground-level patch with at least three storefront bays gets an
entrance: recessed door leaves under a projecting metal canopy. Partial top
rows shorter than 70% of a floor are solid parapets, and every patch top edge
carries a proud coping strip. Non-wall captured triangles more than 2 m above
the wall base (roof caps, setbacks, roof clutter) keep their geometry but are
re-surfaced with a generated roof membrane texture (1 m seams, 4 m repeat)
using the roof PBR finish; fragments near the ground stay captured. The
generated atlas is now four textures (panel, panes, backing, roof).

### Bay fitting, corner posts and patch orientation

Each wall patch takes an integer number of bays (its width divided by the
nearest whole count of nominal bays), so a patch never ends in a sliver
window and every interior window in a patch has the same width. The first and
last bay of a patch keep a 25 cm solid corner post, so glazing never runs
into a corner or an open edge. Each patch is turned outward by ray parity:
horizontal rays leave its largest triangles along the patch normal, and an
odd number of crossings of the building's other surfaces means that side is
inside. Duplicate capture triangles are removed before counting. This keeps
recessed slots facing into the slot and corrects wrongly wound capture faces,
either of which otherwise places the dark backing in front of the wall. A
patch whose rays give no majority keeps the centre-outward rule. The log
reports how many patches were turned.

### Iterative multi-camera fitting

The PCG dialog's **Fit to original with multiple cameras** option runs 1ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã¢â‚¬Å“6
iterations (default 3). It renders eight fixed 512-pixel orthographic views of the saved
original and the actual generated geometry. Four alternating views drive a
bounded coordinate search over window sill/jamb proportions, bay spacing, and
horizontal/vertical grid offsets, with progressively smaller steps. Detected
per-face bright-band phases can anchor the window grid to the capture. Panel and glass colors
are seeded from original texture samples in those four views, including separate
palettes for differently lit facade directions. A shared horizontal grid is
also tried across neighboring wall patches. Recovered facade planes and detected
floor spacing stay fixed during the search.

The other four views are used only for the final acceptance check. The objective
combines multiscale unlit RGB error, local contrast/structure agreement (weight
0.75), silhouette IoU, and depth error. The structural term penalizes shifted
window rows even when average color is unchanged. A candidate
must improve the training views, not worsen the mean validation score, and pass
per-view shape/depth/regression limits. On regularized facades the relative
limits allow up to 0.01 silhouette IoU loss and 0.65 m mean depth increase;
surface-following fallback keeps the tighter 0.002 / 0.05 m limits. Otherwise the normal tiled baseline is
kept. Insufficient textured wall coverage skips the fit. This is bounded local
optimization, not proof of the real architectural layout or a global optimum;
unlit texture comparison does not evaluate dynamic PBR lighting/reflections.
The viewport glass shader uses Fresnel-weighted reflections, so fitted pane
colors remain visible head-on instead of being overwhelmed by an artificial
minimum sky reflection.

Each run writes `camera_fit/<batch>_<group>_<object>/fit.json` and `cameras.png`
in the app's refinement work directory. The sheet shows capture, baseline, and
best candidate; `accepted` in the report says whether that candidate was used.
Original meshes and textures remain embedded, and reapplying starts from them.
CLI input and output paths must differ, including links to the same file.

```powershell
python tools/building_refine/refine.py original.mtscene fitted.mtscene --tiled-facade --auto-fit --fit-iterations 3 --target 0:0:175 --debug-dir fit_reports
# The native --refine command also accepts --auto-fit and --fit-iterations.
```

When the surrounding scene is available, base recovery reads nearby road,
ground and building triangles without changing their objects. Coplanar lower
wall fragments support the reconstruction; a well-supported local ground level
and a facade span present at three higher elevations allow bounded downward
continuation. Separate wings remain separate, and absent ground evidence leaves
the base unchanged. Recovered lower walls include inferred geometry, not a new
capture of entrances or storefront details. The recovery parameters record
donor object IDs, observed area, inferred completion area and ground support.

For context-completed convex towers, the upper captured wall outline can form
a continuous facade with joined corners. This requires at least 85% of the
upper wall area to lie within two metres of the simplified outline. Captured
rooftops remain; the closed shell replaces lower scan fragments, including tiny
degenerate triangles that otherwise remain as floating specks. Unsupported or
concave outlines retain the patch-based path. Near-parallel overlapping patches
are assigned to the stronger wall; narrow tails are removed before windowing.

Window rows can use a repeated `row_offsets` sequence in
fractions of a bay: `[0, 0.5]` alternates half-window offsets, while `[0, 0, 0.5]`
uses a three-floor cycle. Row indices use the floor datum rather than restarting
at each mesh patch. Boundary windows remain clipped to the facade. Per-face
capture detection requires three observed cycles and consistent phase; missing
or low-contrast pixels do not justify staggering. Multi-camera fitting compares
aligned, alternating and three-row layouts, with held-out validation before a
candidate replaces the current layout.

This uses virtual camera captures of existing data. It does not acquire new
Google Earth imagery. Multiple iterations
rebuild and compare real window meshes; large, fragmented buildings take longer.


## Shared building rules

`building_rules.py` is the shared construction implementation for both the city
pipeline (`capture_tiles.py`) and the focused facade/roof pipeline. Do not fork
window, arch, extrusion, beam or roof-coping construction into building-specific
copies. Both exports identify the shared ruleset version.

Apply these rules to every building:
- Preserve the original capture and generate a separate replacement asset.
- Recover each building's own opening layout, including alternating row phases.
- Cut whole openings from walls; insert frame, reveal, mullion and glazing parts.
- Treat fascia/cornice ornaments as masonry, not dark window detections.
- Restrict doors to the overall building base, with observed support.
- Separate captured wall finish from procedural opening geometry.
- Model observed roof slopes and raised coping as geometry; do not flatten them.
- Keep inferred dimensions distinct from observed measurements.
- Check closed structural envelopes and compare renders against the capture.

Measured facade values belong in `building_profiles/`, not in shared geometry
rules. The current building_080 profile is a calibrated example, not a style to
copy onto unrelated buildings. Roof feature detection for other buildings remains
incomplete; do not claim this example has automatically refined every roof.


## Persistent generation config

`building_rules.json` is the versioned shared config for future captures. Both
`city_rules.py` and `landmark_facade.py` accept `--rules PATH`; the default is the
config beside the scripts. Overrides may specify only changed fields. Unknown
keys, invalid sizes and disabled source-preservation checks are rejected.

The config controls detection, pattern recovery, door placement/evidence,
window geometry, glass assignment, captured wall-material cleanup, roof sampling,
roof-frame defaults and validation. Keep measured building layouts in
`building_profiles/`: they are evidence for an individual building, not global
style rules. The current city generator uses the repaired, capture-registered
scene as input, the protected source capture for roofs, and a calibrated profile
asset where one exists. Sub-metre roof features still require further fitting.

Every export writes `effective_rules.json` and records its SHA-256 in the manifest.
Keep that snapshot with the exported scene when changing the global config.
Example:

```powershell
python tools/building_refine/city_rules.py REFERENCE.mtscene SOURCE.mtscene CALIBRATED_BUILDING.mtscene NEW_OUTPUT_DIR --rules tools/building_refine/building_rules.json
```

Calibrated profiles are bound to `profiles.source_capture_sha256`. Reusing this
config with a different capture runs the generic evidence-based rules; it does
not apply this city's building_080 profile or building_367 exception merely
because segmentation reused those names.

## Individual facade review

`window_members` in the shared config controls extraction of internal window
members. A member must be a narrow, long light strip with darker glazing on both
sides. Its position is measured within that captured opening; it is not a fixed
two-pane or four-pane preset. Members are opaque geometry in front of native
glass. Small scratches and broad reflections are rejected conservatively.

`building_profiles/<name>_facades.json` can override opening rectangles on a
reviewed face. Overrides are bound to the SHA-256 of the decoded, registered
face image and are ignored on other imagery. The current 090 and 231 overrides
correct selected facade layouts only; they do not certify those whole buildings.

After generation, run:

```powershell
python tools/building_refine/building_review.py OUTPUT_DIR SOURCE.mtscene
```

This writes an individual capture scene, PCG scene, bounds, measured profile and
facade comparison sheet for each building, plus `review_index.json`. Source
isolation uses the matching segmentation label; neighboring fragments can still
hold additional evidence. Automatic measurements are explicitly pending visual
review. No-window detections are listed for follow-up, not counted as successful
reconstructions. Review roof features and missing capture areas separately.

### Capture-building-v4 refinement

Roofs are partitioned into closed fitted plane regions. Level changes use
vertical risers instead of heightfield ramps. The config controls 0.25 m source
sampling, 0.5 m fitting, minimum region area, boundary simplification and narrow
construction joints. The closed shell below remains the safety envelope.
Vertical risers use adjacent sampled colors because top-down capture does not
provide their wall texture. This is an approximation, not recovered cladding.

Window template recovery requires compatible tone/contrast and at least two
observed openings supporting its row or column. Frame color comes from captured
opening perimeters. Wall cleanup now keeps a narrow margin to retain more of the
captured masonry and trim. Layout overrides may also bind to an exact bottom
image strip, allowing a measured layout to survive a changed eave crop without
being applied to a different capture. Roof glazing, thin ornament, missing wall
imagery and imperfect segmentation still require individual review.

## Per-building iteration loop

Keep a persistent ledger instead of treating a completed city export as approval:

```powershell
python tools/building_refine/iteration_loop.py scan BASE_CITY_DIR NEW_LEDGER_DIR
python tools/building_refine/city_rules.py REFERENCE SOURCE CALIBRATED_ASSET CANDIDATE_DIR --only building_469
python tools/building_refine/iteration_render.py BASE_CITY_DIR CANDIDATE_DIR REVIEW_DIR --only building_469
python tools/building_refine/iteration_loop.py record LEDGER_DIR/iterations.json building_469 CANDIDATE_DIR/city.mtscene needs_more_work "Specific observed mismatch" REVIEW_DIR/building_469/comparison.jpg
```

Repeat the candidate/render/review steps after correcting the diagnosed issue.
`register` records a full-city candidate without approving any building. The
ledger retains failed iterations, scene hashes, comparison-image hashes, open
roof tasks and individual review notes. Missing-glazing area is a prioritization
heuristic, not a similarity score or proof that a proposed window is correct.

The curtain-wall method uses repeated observed floor bands and, where resolved,
vertical members. Without reliable narrow members it records an unresolved
strip proposal; production replacement is disabled by default so captured
subdivisions are not erased. A measured profile can resolve a restricted region
without replacing unrelated parts of the face. Local band checks avoid placing
glass on occluding foreground surfaces. Glass openings follow locally measured
floor lines; curtain frames use separate widths from masonry window frames.
Material cleanup preserves dark metal and the narrow strips between glass bays.
The calibrated landmark profile still takes priority over automatic detection.

For the current capture, `scenes/building_iterations_20261016/iterations.json`
tracks all 148 building objects. Never mark its pending entries complete merely
because the batch ran. Roof and faÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â§ade reviews are separate outstanding tasks.

### Roof and envelope review (v6)

`envelope_repair.py` fills scan-gap notches only where captured upper surfaces
support the added area. It retains neighboring footprints and unsupported
courtyards. Single-level envelopes may also use a measured `*_envelope.json`
profile bound to the source capture SHA256. Building 020 uses its observed
cornice outline; its unobserved roof interior remains an inference.

Roof texture projection groups triangles by plane and height. Combining several
roof levels into one projection loses low decks outside the projection band.
Vertical roof risers use captured faÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â§ade imagery where available. Existing
touching-shell edges do not suppress roof fitting unless fitting worsens the
topology audit. Plane support and source-coverage failures remain explicit.

Render all buildings with two oblique views and one overhead view:

```powershell
python tools/building_refine/iteration_render.py BASE_CITY CANDIDATE REVIEW_DIR --roof --no-before --workers 3
```

Optional `--capture-only` prepares original images while generation runs;
`--reuse-captures` reuses them only when scene hash, camera bounds and views
match. Raw PNGs are unchanged; comparison sheets share a crop across variants.
Review every building before recording its result. Do not treat capture-only
sheets, generated meshes, coverage numbers, or old reviews as latest approval.
Open construction frames and fragment-only captures require separate treatment;
absence of roof evidence is not evidence for a finished flat roof.

Reviewed classification corrections live in `source_classification.json` and
apply only when its source SHA256 matches. Seven inspected street/vegetation
labels in this capture are omitted from the generated building layer; their
source meshes and object metadata are retained. This is an explicit reviewed
override, not an automatic vegetation classifier. Empty candidate comparison
panels are labeled as classification corrections, not rendered buildings.

Capture-bound `*_structure.json` profiles support measured open construction
assemblies (currently 008): retaining ring, deck and I-beams without a roof cap.
Beam heights are sampled from the original capture. Excavation depth and small
joints are not reconstructed. These profiles do not generalize to other captures.

Review history stores each building's geometry/material/texture content hash.
An unchanged building can retain its review across city exports; a changed one
must be rendered and reviewed again. `review_gallery.py` shows the matching
review evidence. Generation and screenshot capture run locally without a chat
assistant; visual diagnosis and rule edits are currently agent/manual steps,
not an autonomous optimization loop in MeshTool.

`building_profiles/building_groups.json` records reviewed source-label groups
for the exact capture hash. Members are rebuilt as one envelope before roof
fitting and facade extraction; secondary labels retain metadata and report
`combined_into_building`, without duplicate geometry. Comparison exports use
all source members. The first reviewed grouping is 334 + 346. For these explicit
groups, neighbors more than 12 m below the group's cap cannot carve a notch
through its upper roof. This does not automatically establish ownership of
every adjacent building or reconstruct intersections with lower podiums.

Roof-supported recovery may exceed the ordinary area-growth limit when one
connected captured roof covers at least 90% of both the old and proposed
footprint, up to the configured fragment area ratio. It does not use a convex
hull to bridge disconnected roofs. Courtyard and topology checks still apply.

Road finishing uses `road_finish.json`: lighter asphalt RGB, cubic spline
smoothing error, maximum edge displacement and minimum triangle-area ratio.
`road_scene.rebuild` applies it automatically. To update an existing scene:
`python tools/building_refine/road_finish.py INPUT.mtscene NEW_DIRECTORY`.
Only shared road/ground boundary vertices move, on both meshes together;
scene-edge endpoints stay fixed and local damping prevents triangle inversion.
The existing tessellation approximates the spline; this is not a new curb mesh.
Road paint remains in the atlas, with its pigments excluded from asphalt lifting.

### Ground-floor display windows

`storefront.py` detects wide ground-floor display bays independently of repeated upper-floor windows. The `storefront` section in `building_rules.json` controls size, sill, contrast and texture constraints. Detection requires internal mullions in both directions to avoid treating plain garage panels or shadows as glass. Dark and light frame patterns are supported.

Capture-bound facade profiles may specify `storefront_rects_px` and `storefront_members` for measured grids. Profiles are guarded by the capture image hash. These openings use recessed glass and dark frames sampled from their capture; wall piers remain wall material. Building 334 face 010 is the first visually checked example. Adjacent bays and door interiors are not inferred by this override.

### Capture iteration v7

Window members are measured in both contrast polarities when
`window_members.detect_dark_members` is enabled. With
`materials.sample_measured_frame_color`, the measured bars supply their frame
color instead of the adjacent wall. Flat glass, short scratches and broad shadow
bands do not qualify as internal members. Arched member intersections are snapped
to a 1 mm construction tolerance before extrusion to prevent degenerate triangles.
Original wall material projections and per-face window locations remain capture-derived.

Roof fitting reports zero support when no plane can be measured; a median height
is not accepted as evidence of a flat roof. `city_rules.py --fallback <previous-city-directory>`
retains the previous building when a generated candidate fails the topology checks.
Its rejected catalog is saved separately and the manifest records the rejection.
These checks do not certify visual similarity. Review each building from two
oblique directions and overhead before accepting the candidate.

Preserved calibrated facades now receive a separate roof pass. It detects a missing
main deck from the captured horizontal roof footprint and dominant measured height,
preserves courtyard holes, and projects captured roof materials. The `roofs.deck_*`
settings control this recovery. Low-coverage material projections are rejected.

Assembly profiles can mark thin rooftop patches as `captured_roof_attachment`.
This retains the validated parent building and attaches the captured roof surface;
it does not extrude the patch to ground. Such attachment geometry is explicitly
reported as retained capture, rather than a newly fitted PCG roof.

Use `merge_city_pass.py --preserve-window-layout` when accepting an iteration.
`iteration_acceptance` in the shared config controls the window-loss review guard.
A large count drop retains the previous building pending layout review; it does
not certify that either layout matches the capture.

### Capture-supported glass finish

The `captured_glazing` rules transfer original pane imagery onto existing PCG glass after window construction. Source objects are restricted to the building and its declared parts. Native glass material, frame geometry, opening locations, wall textures and roof geometry remain intact. Connected panes require at least 98% observed pixels for every triangle; unsupported panes retain their prior finish. This retains baked capture reflections, not physically recovered albedo. It does not complete missing facades.

`captured_glazing.py BASE_DIRECTORY SOURCE_SCENE NEW_OUTPUT --only building_334,building_001` applies the same pass to an existing validated city without rebuilding its envelopes. Its per-face evidence masks and manifest distinguish observed coverage from inferred fill. `city_rules.py` applies this pass to newly generated buildings and forwards repaired-envelope observation masks into opening confidence checks.

### Bounded facade iteration

`facade_rectification` aligns supported window lintels/sills within each row and height class. Bay widths, horizontal phases, doors and measured storefronts remain fixed. A piecewise affine texture warp follows the geometry, with fixed facade borders and rejection of local folds/stretch. Unknown wall coverage is not completed by this step.

`iterate_facade.py BASE NEW_OUTPUT --building NAME --face ID` reuses the established opening catalog and replaces only that facade assembly. It checks unchanged opening count, explicit frame geometry and shell topology before saving. A rejected correction writes a rejection record without a city scene. Each generated window has a separate outer frame, optional internal mullions, native glass and a closed recessed interior backing. Backing dimensions are shared rules and inferred, not measured room depths.

`iterate_facade.py ... --flush-windows` applies the shared `windows.flush_recess_m` rule to the selected facade while preserving its opening catalog. The wall and outer frame front remain coplanar; native glass and internal bars sit within the shallow recess. This is an explicit surface correction, not depth estimated from a single capture image. Per-instance `surface_mode` records the selected construction.

Clean PCG windows are now the default (`captured_glazing.enabled=false`). Frames and internal bars retain capture-derived dimensions/colors, while glass uses a clean native material rather than photographic pane imagery. The original capture remains the layout reference. A face tagged `window_finish=procedural` is protected from the captured-glazing transfer pass.

`window_shape` validates inferred arches against a straight captured lintel and at least three similar rectangular row peers. It changes only the shape, retains bounds and excludes manually measured/storefront openings. True arches, isolated arches and weak lintel evidence are left unchanged. Shape changes are recorded per facade; repeated review stops when no additional changes meet the evidence thresholds.


### Local AI facade iteration (2026-10-19)

`python tools/building_refine/ai_city_iteration.py BASE SOURCE OUTPUT` uses cached LaMa on CUDA and same-row template recovery. Rules are in `building_rules.json` under `ai_facade`: 55% minimum observed coverage, 1 m maximum gap radius, 12 cm opening padding. Source captures are read-only and SHA256 checked. Only masked wall material gaps change; geometry, roofs, frames, glass and routes remain unchanged. DXT compression can affect nearby texels.

Per-building `ai_review.json` records inferred window proposals; these are NOT accepted PCG geometry. Large unsupported areas and missing facade catalogs are skipped. Before/after images and inference masks make the model changes reviewable. Captured PNGs are retained for subsequent passes.

Run result: 148 object records inspected; 136 catalogued buildings processed; 120 buildings / 792 faces repaired; 332 proposed windows. Three rendered objects lacked catalogs and nine records had no separate geometry. Geometry/routes verified unchanged; 171 tests passed. Native comparisons reviewed for 334, 066 and 154. Changes are small gap repairs, not a complete reconstruction. Building 334 still has a large blank facade in both baseline and candidate; this requires facade geometry/detection work beyond texture inpainting.

Review: `scenes/city_ai_iteration_20261019/review.html`; road-finished scene: `scenes/city_ai_iteration_final_20261019/city.mtscene`.


### Capture-validated window installation

`validate_window_candidates.py BASE SOURCE OUTPUT` independently projects the original building family, detects observed openings, and accepts proposals only with at least 0.5 intersection-over-union and 90% original-pixel coverage. Rules are in `window_candidate_validation`. Doors are excluded. Unsupported candidates are recorded, not installed.

`iterate_facade.run(... additions=..., scene=..., manifest=...)` supports city-wide in-memory processing. Addition mode cuts new wall holes and keeps existing frame/glass assemblies byte-for-byte; only new window components are installed. Existing window rectification is disabled during additive patches. Topology regression rejects the patch transactionally. Original captures and unrelated scene objects are preserved.

2026-10-19: all 332 proposals validated; six installed on 334 (2), 154, 005, 327, 487. 325 had no matching observed opening; one overlapped an existing opening. Full-facade regeneration was rejected after visual review; selective installation fixed that regression. The broad blank end wall on 334 has only 16.8% matching capture coverage and remains unresolved, not falsely approved. Review `scenes/city_validated_windows_selective_v2_20261019/review.html`; road-finished scene `scenes/city_validated_windows_selective_roads_20261019/city.mtscene`.


### Surrounding-capture completion

`complete_surrounding_facade.py BASE SOURCE OUTPUT --building NAME --target ID --donor ID --bay START END` uses a captured same-building motif when direct target capture is missing. Floor registration must match; donor coverage must exceed 80%. The motif repeats at its original metre scale, with horizontal phase selected against observed edge pixels. Observed target pixels are restored exactly before encoding. Original facade PNGs remain unchanged; synthesized images use distinct inferred names.

Only mostly unobserved openings are installed, with native frames and glass; existing windows remain unchanged. New instances are marked inferred with donor face provenance. No doors are inferred. Rules live in `surrounding_facade_completion`; per-run donor, motif interval and phase are recorded in `inference.json`. This is an inferred continuation, not direct validation of the missing design.

334 trial: face 1 completed from face 20, motif 3.84–6.40 m, 67 new windows, no topology regression. Reviewed three native views. Original base strip remains plain. Review `scenes/city_surrounding_completion_20261019/review.html`; road-finished scene `scenes/city_surrounding_completion_roads_20261019/city.mtscene`.
