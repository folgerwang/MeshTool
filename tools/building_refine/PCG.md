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
Fits that fail the existing capture-fidelity checks remain unchanged. Reapplying
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
refine with this build to record a comparison pair. Rejected fits do not create
a second model. Reapplying refinement retains the first recorded original.
Mesh export uses the currently displayed versions. Successfully resegmenting
a scene replaces the old object identities and their comparison history.
