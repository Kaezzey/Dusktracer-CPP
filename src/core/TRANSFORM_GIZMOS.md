# Transform controls

Select an object in the Raster viewport or Outliner. **W** selects Move,
**E** Rotate, **R** Scale, **Q** selection, and **Space** cycles transform tools.
The toolbar exposes the same modes. Shortcuts are suspended during text entry,
active controls and RMB camera navigation. Fly with **RMB + WASD**, with E/Q
moving vertically while RMB is held.

Move uses RGB X/Y/Z arrows, square XY/XZ/YZ plane handles, and a white center
for movement parallel to the camera. Rotate uses three camera-facing quarter
arcs, a full active ring, and a live angle readout. Scale uses cube-ended axes,
triangular plane handles, and a white center cube for proportional scaling.
Hovered/dragged controls turn yellow. Axis/plane drags stay constrained to the
selected components. Handle size and picking tolerance use displayed pixels,
including letterboxed viewports and render-resolution matching.

**World / Local** changes move and rotation axes. Scale follows local axes:
world nonuniform scaling would introduce shear that Dusk's TRS cannot represent.
**Snap** enables the adjacent increment; **Ctrl** temporarily snaps a drag.
Translation uses scene units, rotation degrees, and scale relative factors.
Each completed drag is one undo action. **Escape** cancels and restores the
original transform. Releasing outside the viewport ends the drag.

Visual and interaction conventions follow Epic's
[UE5 actor transformation documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/transforming-actors-in-unreal-engine?application_version=5.6).
Dusk keeps its Y-up, right-handed coordinate system.

`editor_transform_gizmo.inl` owns projected handle geometry, finite picking,
drawing, drag constraints and undo. Rotation composes matrices before converting
to Euler XYZ; angle tracking unwraps beyond 180 degrees. Edge-on rotation arcs
use a projected tangent when ray/plane intersections become unstable.
`editor_transform_gizmos` tests constraints, snapping, cancel/undo, local axes,
distance-independent display size, rotation near gimbal lock and GL screenshots.
