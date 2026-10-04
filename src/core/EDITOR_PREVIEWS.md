# Editor layout and previews

The Outliner groups objects and lights and filters by name. Selecting a row opens
its Properties; Delete acts on that selection, with undo/redo. Camera, render
quality, output, denoising, and caustics guidance live in Render Settings.
The Content Drawer has separate Textures, Meshes, and Materials tabs.

The material graph has a fixed 1024 x 1024 sphere preview while dragging and at
rest. Drag orbits a perspective camera around a stationary sphere; horizontal
movement follows world Y without roll. Scroll dollies the camera and Reset
restores the view. Studio lights, UVs, and tangent-space normals stay attached to
their world-space frames. Orbiting changes only editor preview state.

The interactive sphere reuses the raster viewport's GPU material evaluator and
texture cache. A fullscreen pass intersects camera rays with an analytic sphere,
so its outline has no mesh facets. The shader and output texture are reused;
only camera uniforms change during orbit. Settled previews redraw only after an
edit or texture upload. Rendering restores the caller's OpenGL state. This is a
studio material inspection view with the same direct-lighting approximations as
the raster viewport, not the final path tracer (see its limits below).

Three bounded, joined workers serve material thumbnails, texture thumbnails,
and CPU sphere fallbacks separately. Graphs beyond the GPU evaluator's limits
(such as more than 12 distinct textures) use the CPU fallback at the same fixed
resolution; those unusually large graphs can orbit more slowly. Each CPU job
owns a scene/material snapshot, and revision checks reject stale results.
OpenGL uploads remain on the UI thread. Only visible drawer cells request asset
thumbnails.

Disposable files in `.dusk-cache/previews` store reduced texture sources (at most
1024 pixels per side for thumbnails/spheres, 2048 for the raster viewport) and settled 128-pixel material thumbnails. File size and
modification time invalidate texture data; graph surface keys, material parameters,
and referenced texture stamps invalidate material thumbnails. Each asset replaces
its cache file on edits. Invalid/truncated files regenerate; unwritable caches
fall back to normal decoding. The final renderer still reads original textures.
An eight-image LRU per preview worker retains decoded sources between edits.
HDR preview sources retain their original float representation.

`DuskGraphIntegrationTests` exercises production ImGui dragging, independent worker
scheduling, fixed-resolution dragging, GPU UV/normal orientation, camera framing,
GL state restoration, cache corruption/invalidation, and Outliner actions.
It reports completed GPU orbit timings and writes material-editor and Outliner
screenshots in its isolated output folder.
An optional fourth argument profiles a real texture without modifying it:

```powershell
& .\build\Release\DuskGraphIntegrationTests.exe build/graph-integration profile 'C:/path/to/texture.png'
```

For the project's 53 MB chest normal map, measured preview creation took about
778 ms cold and 5.7 ms from disk cache on the development machine. These are
individual asset timings, not a measurement of complete application startup.

The scene viewport now evaluates materials on model UVs with cached shadows;
see [Raster preview](RASTER_PREVIEW.md) for its rendering and caching contract.
