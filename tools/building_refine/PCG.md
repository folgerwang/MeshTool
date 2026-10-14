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
shows the refined versions by default. Existing scene versions 1–4 still load;
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

The PCG dialog's **Fit to original with multiple cameras** option runs 1–6
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
