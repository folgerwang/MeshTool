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

PCG now defaults to **Replace walls with tiled windows** in the editor. This
replaces vertical captured triangles with facade modules: fine-grain panel
textures, metal frames, glass recessed 14 cm, and dark interior backing.
The original wall triangles are absent from the refined version. Roofs and
non-wall triangles retain their captured geometry. The full original is saved
for comparison. Disable this option to use the earlier trim-only workflow.

The facade is a procedural design, not a recovery of the building's real window
layout. Local plane clipping preserves capture openings and overall shape;
existing missing geometry and distorted roofs are not reconstructed by this step.
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
into a corner or an open edge. Patches face outward by the capture's own
triangle winding when at least 85% of the wall area agrees with the
centre-outward rule; this keeps recessed slots and notches facing into the
slot instead of placing the dark backing in front of them. Inconsistently
wound captures fall back to the centre rule. The log names the rule used.

