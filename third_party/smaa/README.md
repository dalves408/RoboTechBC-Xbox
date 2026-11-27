# SMAA

Enhanced Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez, from https://github.com/iryoku/smaa at commit 71c806a838bdd7d517df19192a20f0c61b3ca29d, unchanged.
MIT licence: see LICENSE.txt.

- `SMAA.hlsl`: the shader. `scripts/smaa_hlsl2h.py` strips its comments into `src/rbx_smaa_hlsl.h`.
- `AreaTex.h`, `SearchTex.h`: the precomputed lookup textures, compiled into the game.

The back end (`src/rbx_d3d11.c`, "SMAA") runs SMAA 1x at the Ultra preset on the frame handed to the window.
