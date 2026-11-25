# Native widescreen

F1 → **Widescreen**: `Off / 4:3`, `Auto / Match window` (default), `16:9`, `16:10`, `21:9`. Saved as `widescreen=` in
`settings.ini` (`[video]`, 0–4). Environment override: `RECOMP_RBX_WIDESCREEN=off|auto|16:9|16:10|21:9`, or any
other shape as `w:h` / a plain ratio (`32:9`, `2.4`), which the renderer uses directly.

## What it does

The game still draws in its own 640×480 coordinates; only the D3D11 back end (`src/rbx_d3d11.c`) changes.

- **Wider targets.** Surfaces of the game's full-screen size (640×480 logical, any guest AA) get `ex` extra pixels
  on each side: the texture is `(w + 2·ex) × h` at the render scale. Depth buffers for them match. Other surfaces
  (offscreen passes, textures rendered by the game) keep their size.
- **Hor+ 3D.** Every vertex shader multiplies clip-space x by `1 − wsq` with `wsq = 1 − w / (w + 2·ex)`. The 4:3
  view keeps its exact shape in the middle, and geometry the game sends past the left and right screen edges is now
  drawn in the side areas instead of being clipped. The vertical view is unchanged.
- **HUD, menus, movies, text** go through the same squeeze, so they stay 4:3 in the centre (pillarboxed), never
  stretched.
- **Full-screen passes stretch.** A flat (one `w`), full-width batch that is untextured or only samples render
  targets (fades, flashes, cinematic bars, frame copies, post passes) is drawn over the whole width (`wsq = 0`).
- **Screen-space sampling lines up.** A squeezed draw that samples a wide target maps its `u` to that target's
  centred 4:3 (`uvs[].w` / `uofs`), so effects that read the frame at screen positions stay aligned.
- **Clears** of the full width cover the side areas; smaller clear rectangles land in the centred 4:3.
- **Guest memory** keeps the game's 640×480: readback squeezes the wide frame into it (the guest-sized `down`
  copy), and seeding a target from guest memory stretches it back.
- **Window.** The shared frame is the wide texture and the display size is its logical shape, so the presenter
  letterboxes to the wide aspect instead of 4:3. `Auto` follows the window's client shape (the monitor's in
  fullscreen) once a resize has been still for 300 ms; a 4:3 or narrower window means off.

Changing the setting rebuilds the render targets at the next flip, the same way a render-scale change does.
With `Off` every new term is an identity (no extra pixels, `wsq = 0`, no `u` offset).

## Known limits

- **Edge pop-in.** The game culls objects against its own 4:3 view, so small objects (enemies, props) can appear
  late at the far left and right; large meshes (terrain, sky, buildings that reach into the 4:3 view) fill the sides.
  Fixing that needs the game's projection or culling aspect patched (look for `1.3333334f`, `0x3FAAAAAB`, in the
  XBE's data or the camera setup).
- **HUD is centred, not edge-anchored.** Radar and gauges stay inside the centred 4:3 rather than moving to the
  screen corners.
- **Heuristic for full-screen passes.** A full-width, screen-flat, textured quad that samples ordinary textures is
  treated as 4:3 art and pillarboxed. A pass that reads a CPU copy of the frame from guest memory (not a render
  target) would be pillarboxed too.

## What to check (frame dumps)

Run with `RECOMP_RBX_WIDESCREEN=16:9` (and once with `21:9`) and `RECOMP_GPU_PRESDUMP=<dir>\ws:60`.

1. Title and main menu: menu art and text 4:3 and centred with black side bars, nothing stretched.
2. Intro movie: 4:3, centred.
3. Ch1 in mission: world continues into the side areas (terrain, sky), HUD centred and undistorted, round
   things round. Note any objects popping in at the side edges.
4. Fades and the START pause menu: the fade/dim covers the whole width, not only the middle.
5. Versus split screen: both halves wide, each camera on its own half.
6. With `Off`: frames identical to the current build.

The log line `[RBX DX11] video: render scale …, widescreen 1.333 (aspect 1.778)` confirms the mode;
`RECOMP_GPU_LOGRT=1` prints `wide+N` per target.
