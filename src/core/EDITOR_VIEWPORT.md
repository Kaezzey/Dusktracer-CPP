# Editor viewport: navigation and responsiveness

## Navigation

The viewport follows Unreal's editor controls:

| Input | Action |
| --- | --- |
| Right drag | Look around. **W A S D** move, **Q / E** down and up, **Shift** 3× faster |
| Wheel while right-dragging | Change the fly speed (shown at the top of the viewport) |
| Middle drag | Pan: the point under the cursor follows it |
| Alt + left drag | Orbit around the point the view looks at |
| Alt + right drag | Dolly toward that point |
| Wheel | Zoom toward that point |
| **F** | Frame the selection, or the whole scene with nothing selected (also View > Frame Selection) |

Hover **Controls** in the viewport toolbar for the list.

A drag starts only on a press over the viewport image, not its toolbar, and
not over a panel or menu drawn on top of it. From then on it owns the mouse
until its button is released, wherever the pointer goes:

- the cursor is hidden and locked to the window, and raw (unaccelerated)
  mouse motion drives the camera;
- ImGui ignores the mouse (`ImGuiConfigFlags_NoMouse`), so no other panel
  hovers, highlights or takes a click;
- the drag ends when the button is released or the window loses focus, so
  the cursor can never stay captured. GLFW puts the pointer back where the
  drag began.

Before, a right drag kept running only while the viewport stayed hovered or
focused. ImGui focuses a window on a left click only, so once the hidden
pointer drifted over another panel the drag stopped mid-way with the cursor
still captured: the camera stuck. The hidden pointer also kept hovering and
clicking other panels.

Fly movement eases in and out (a 70 ms time constant), and a frame-time hitch
is clamped so it cannot fling the camera. **F** eases the camera to the framed
view over 0.22 s.

`editor_viewport_nav.inl` reads one input snapshot per frame (`read_input`),
so `tests/editor_navigation_regressions.inl` replays drags without a mouse.
That includes the pointer leaving the viewport mid-drag, focus loss, panels
and toolbars that must not start a drag, easing, Shift, wheel speed, pan,
orbit, zoom and framing.

## Windows and layout

- Panels dragged out of the editor window, such as the Material Editor, are
  owned by it, as tool windows are: they stay above the editor when it is
  clicked, minimise with it, and have no taskbar entries of their own.
- The dock layout is saved in `imgui.ini` and restored on launch.
  View > Reset Layout restores the default.
- Panels move by their title bars only, so dragging inside one never moves it.
- Click a number field to type a value; drag it to scrub.
- A status bar shows the project and unsaved changes, the selection, the fly
  speed, render progress and the frame rate.
- While minimised, the editor waits for events instead of drawing. Renders
  keep running.

## Viewport speed

The raster viewport draws every frame, so its cost decides how smooth the
editor feels. Cloud's scene (hair cards, SSS head, one sun) at 1600×900 on an
RTX 4070, GPU time per frame:

| Change | ms |
| --- | --- |
| Before | 15.2 |
| Depth pre-pass | 11.1 |
| Eight-register graph shaders | 4.4 |

- **Depth pre-pass.** A cheap pass (`raster_shaders::depth`) writes the
  nearest surface's depth, testing alpha only. The lit pass then runs with
  `GL_LEQUAL` and no depth writes, so each pixel is shaded once instead of
  once per overlapping hair card. `invariant gl_Position` keeps both passes'
  depths identical.
- **Register file.** The material graph interpreter keeps its values in a
  register array. At 32 entries (any graph's limit) it spills to GPU memory;
  at 8 it stays in registers. Each viewport program is compiled both ways
  (`GRAPH_REGISTERS`), and each material uses the small one when its compiled
  graph needs at most 8 values at once, as typical graphs do.
- The sun shadow lookup projects its centre once and steps its nine taps
  linearly (the sun's projection is orthographic), rather than multiplying
  matrices per tap.

Measure a project with:

```powershell
.\build\Release\DuskGraphIntegrationTests.exe <scratch folder> --raster-bench <Scene.dusk> [width] [height]
```

It prints the cold first frame, still and orbiting frame times, and the cost
with one feature switched off at a time (base colour only, no shadow maps,
Clip instead of Blend alpha, no subsurface).
