# GPU vertex programs: design for the port

Status: design. Nothing in this note ships until the bench passes it. The new path is opt-in at first (`RECOMP_GPU_VSH=1`) and becomes the default only after it renders the same as the CPU path in menus, briefing, cutscenes, HUD and flight.

## Why

Measured on the dev PC (6800H / 680M) at 858a0c0, the pushbuffer thread is 100% busy and is what limits fps (~155 in missions). The CPU vertex-program interpreter (`vsh_run_dec` + `run_mac`) takes ~16–23% of that thread. Attribute fetch, the post-transform conversion and the clipper add ~20% more. A GPU vertex shader removes all of that from the bottleneck thread for program batches.

## What the CPU path does today (toolkit v0.16.0-24-g342f22a)

- **Program and constants:** `nv2a_vsh_interp.c` holds the program (136 slots × 4 dwords) and 192 constants (D3D `c0` = `c[96]`). Both are written word by word from the pushbuffer, and there is no dirty tracking.
- **Per vertex (`transform_vertex`):**
  - It fetches 16 inputs: array attributes through `fetch_attr`, absent ones from `imm_attr` (default 0,0,0,1).
  - It runs the program. `oPos` is already in screen space (x, y pixels; z depth; w = clip w).
  - It applies the post-fixes: `pos.w == +0` becomes 1; if oD0 was never written, `d0.rgb = 1` (and `a = 1` if it was 0); d0 and d1 are clamped to [0,1]; `pos.xy *= aa_sx, aa_sy`.
- **Per triangle:** near-plane clipping at `w = 1e-3` and `z = c[59].z * w`, then reject (w ≤ 0, zero area), cull with exact strip parity, and `backend_from_xf`:
  - `rhw = 1/w` and diffuse packed to 8 bits
  - `uv = tex0.xy × (tex width, height)` when texture 0's format is unnormalised
  - `fog = fog_factor(fog.x)`
- **Back end VS:** screen to clip with `(x/surf.x·2−1, 1−y/surf.y·2, saturate(z/16777215), 1) · w`.

## Target path

1. **Executor (program batches, no `CXT_WRITE`, no VSH debug modes):**
   - It gathers the unique vertices' raw inputs, only the attributes the program reads, as float4 each.
   - It reuses the index hand-off from 7a59c34, but builds indices straight from the primitive with no clipping or culling. Strips are expanded with odd triangles flipped, so D3D culling sees the right winding.
   - It snapshots the 192 constants when they have changed since the last batch, using a dirty flag set by the constant methods.
   - It hands over the program identity as a hash of the decoded slots from the start slot to FINAL.
2. **Translator (new file, `src/rbx_vsh_hlsl.c`):** decoded program → HLSL vertex shader, cached by program hash and compiled through `rbx_compile` (already disk-cached).
   - MAC: mov mul add mad dp3 dph dp4 dst min max slt sge arl. ILU: mov rcp rcc rsq exp log lit, with the exact edge cases listed in the toolkit summary.
   - Paired MAC+ILU, where the ILU temp write goes to R1 and both read before writing.
   - `a0`-relative constant reads, with out-of-range reads returning `c[0]`. R12 aliases oPos.
   - Epilogue, the same as the CPU post-fixes: w fix, default diffuse (the shader knows statically whether oD0 is written), clamps, AA scale.
   - Then `backend_from_xf`'s conversions: rhw, uv scale, and fog factor (fog mode passed in a cbuffer).
   - Then the existing screen→clip mapping, without `saturate`. The rasterizer state turns depth clip off for this path, so the far plane clamps as before, and the GPU clips at the near plane in place of the CPU clipper.
   - It writes the same outputs the RC pixel shader reads today (COLOR0, v0 float, d1, tc[4], fog), so the combiner shaders are unchanged.
3. **Back end:** one input layout of up to 16 float4 streams (or one interleaved buffer), a constant buffer of 192 float4 (3 KB, updated only when dirty), and `DrawIndexed`. Rasterizer culling comes from the NV2A cull state.

## What stays on the CPU

- Batches with `CXT_WRITE` (the program writes constants), `RECOMP_VSH_DUMP/VERIFY/NODECODE`, fixed-function batches (a second, smaller translator later) and screen-space batches.
- Heuristics that read transformed vertices:
  - The black-V0 skip and `t1 ← t0` move into the shader (`t1 ← t0` is per vertex), or are decided by running the CPU interpreter on the batch's first vertex only.
  - The zpass and xf bounds counters become debug-only on this path.

## Stages (one commit each, each A/B-able)

0. GPU clipping and culling (`RECOMP_GPU_CLIP=1`): the CPU still transforms, but the indexed hand-off goes over unclipped and unculled. The back end's vertex shaders write the two near planes as `SV_ClipDistance0`, keep w's sign, leave z unclamped (depth clip off, so the rasterizer clamps per pixel), and rasterizer states cull with the NV2A state. This is the part of step 2 the translated shaders need anyway, and it removes the clipper (~17% of the thread) on its own.
Shipped first instead of the translator (stage 1 below): hand-written HLSL for the 5 known programs (4 shaders; 892C and E031 are the same words) behind `RECOMP_GPU_VSH=1`, sharing `patches/rbx_gvs.h` between the executor and the back end, with `RECOMP_GPU_VSH_VERIFY=N` doing the stream-out comparison from stage 3. The translator is only needed if more programs turn up.

1. Translator plus a host-side test that runs the CPU interpreter and an HLSL-equivalent C rendition of the generated code over recorded programs and inputs. A program dump (`RECOMP_VSH_DUMP`) from the bench supplies the corpus.
2. Executor gathers raw inputs and constants; back end draws program batches with the generated VS behind `RECOMP_GPU_VSH=1`.
3. Bench: visual check with per-draw position diffs (`RECOMP_GPU_VSH_VERIFY=1` transforms one vertex per draw on the CPU and compares it with a GPU stream-out readback), then fps and profile.
4. Default on once stage 3 is clean on AMD, NVIDIA and Intel.

## Needs from the bench / the tester

- A `RECOMP_VSH_DUMP` capture covering the menu, briefing and one mission. This gives the programs, constants and a few inputs per program for the translator tests.
- The toolkit's `nv2a_vsh_interp.c` and `.h` (the version). The translator must match its decode exactly, so it should be patched in from this repo like `rbx_present.cmake`, or copied into `patches/`.
