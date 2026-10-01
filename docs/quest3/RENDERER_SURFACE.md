# Renderer Surface — Halo PC → Quest 3

Ground truth from a live run of the translated engine on the macOS host
(`HALO_RENDER_SURFACE=1 ./halo-host ../../game --frames 60`). Boot + first
minute of gameplay. This is the surface the Quest 3 renderer must cover.

Indices below are the D3D9 `D3DRENDERSTATETYPE` numbers as defined in the
DirectX 9 SDK (`d3d9types.h`) — verified against the host's own state→Metal
mapping (`d3d9_render.inc`, `metalrenderer.m`). Values are the distinct
values the game actually sets.

## Decision: Vulkan

Vulkan, not GLES 3.2. Three reasons:

1. **Both programmable stages are required.** The game creates real D3D9
   vertex shaders (vs_1_1–vs_2_0, 64 created) and pixel shaders
   (ps_2_0/ps_2_x, 469 created). Translating to SPIR-V (DXBC/DXSO→SPIR-V)
   is offline and well-trodden. GLES would force the same translation to
   GLSL ES 3.0, where D3D's `tex2Dproj`, dependent-read clamp, and the
   relative-constant VS addressing (Halo's `c[-const]`) are awkward and
   partly unsupported. The existing host already solves this with
   MojoShader→MSL; a SPIR-V target is the same class of work with no
   runtime-shader-compile penalty on Adreno.
2. **Explicit pipeline state matches the D3D9 surface 1:1.** The game's
   fixed-function state maps directly onto Vulkan pipeline sub-states
   (`VkPipelineColorBlendAttachmentState`, `VkPipelineDepthStencilState`).
   On GLES these are scattered `glBlendFunc`/`glStencilOp` calls with less
   control over the depth format.
3. **Depth precision.** The game needs depth+stencil. Vulkan gives
   `VK_FORMAT_D24_UNORM_S8_UINT` directly; GLES 3.2's `DEPTH24_STENCIL8`
   works but the host's existing path (Metal) uses
   `Depth32Float_Stencil8`, which is closer to Vulkan's explicit model.

Cost: more code than GLES. Mitigation: the macOS Metal backend
(`d3d9_render.inc` `draw_state`, `metalrenderer.m`) is a complete reference
for this exact surface. The Vulkan backend is a re-skin of the same
state-struct→API encoding, not a new design.

## Textures created by the game

| D3D9 format | value | count (boot+1 min) | notes |
|---|---|---|---|
| `D3DFMT_X8R8G8B8` | 22 | 33 | main textures |
| `D3DFMT_A8R8G8B8` | 21 | | alpha |
| `D3DFMT_A4R4G4B4` | 26 | | 16-bit |
| `D3DFMT_DXT1` | 'DXT1' | | compressed, 1-bit alpha |
| `D3DFMT_DXT2` | 'DXT2' | | compressed, explicit alpha |
| `D3DFMT_DXT4` | 'DXT4' | | compressed, interpolated alpha |

Compressed BC1–BC3 (`DXT1`/`DXT2`/`DXT3`/`DXT4`/`DXT5`) are mandatory on
Quest 3 (Adreno HW). Uncompressed formats map to `R8G8B8A8`/`B8G8R8A8`
`VK_FORMAT`. The existing host already decodes these to BGRA (`texture_decode.h`
/ `texture_decode.c` — `HALO_TEXFMT_DXT1/3/5`).

## Render states the game sets

Verified against the host's `draw_state.rs[]` mapping (`d3d9_render.inc`,
`d3d9.c`, `metalrenderer.m`). Indices are the D3D9 `D3DRENDERSTATETYPE`
values the game actually passes to `SetRenderState`.
| idx | D3D9 name | values seen | host meaning |
|---|---|---|---|
| 7 | `ZENABLE` | 0, 1 | depth test |
| 8 | `FILLMODE` | 3 | SOLID only |
| 9 | `SHADEMODE` | 2 | GOURAUD only |
| 14 | `ZWRITEENABLE` | 0, 1 | depth write |
| 15 | `ALPHATESTENABLE` | 0, 1 | alpha-test variant |
| 16 | (unused) | 0 | |
| 19 | `SRCBLEND` | 2, 5, 7, 9 | D3DBLEND factors |
| 20 | `DESTBLEND` | 1, 2, 6 | D3DBLEND factors |
| 22 | `CULLMODE` | 3, 1 | NONE / CW |
| 23 | `ZFUNC` | 4, 3 | LESS_EQUAL / LESS |
| 24 | `ALPHAREF` | 0, 1, 127 | |
| 25 | `ALPHAFUNC` | 5 | |
| 26 | (unused) | 0 | |
| 27 | `ALPHABLENDENABLE` | 0, 1 | **the blend gate** |
| 28 | `FOGENABLE` | 0, 1 | radial fog |
| 29 | (unused) | 0 | |
| 34 | `FOGCOLOR` | 0 | |
| 35 | `FOGTABLEMODE` | 0 | radial fog requires 0 |
| 36 | `FOGSTART` | 0, float | |
| 37 | `FOGEND` | 0, float | |
| 38 | `FOGDENSITY` | 0 | |
| 48 | (unused) | 1 | |
| 52 | `STENCILENABLE` | 0, 1 | |
| 53 | `STENCILFAIL` | 1 | KEEP |
| 54 | `STENCILZFAIL` | 1 | KEEP |
| 55 | `STENCILPASS` | 1, 3 | KEEP / REPLACE |
| 56 | `STENCILFUNC` | 8, 3 | |
| 57 | `STENCILREF` | 0, 2 | |
| 58 | `STENCILREADMASK` | 0, 1 | |
| 59 | `STENCILWRITEMASK` | 0, 2 | |
| 60 | `TEXTUREFACTOR` | 0, 0xFFFFFFFF | D3DTA_TFACTOR value |
| 137 | `LIGHTING` | 0 | fixed lighting off |
| 139 | `AMBIENT` | (set via material) | ambient colour |
| 140 | `FOGVERTEXMODE` | 0, 3 | radial fog mode |
| 168 | `COLORWRITEENABLE` | 15, 7, 8, 0 | RGBA / RGB / A / none |
| 171 | `BLENDOP` | 1 | ADD |

Indices 128–205 are also written (mostly with their default 0); the table
lists only states with non-default values or active host use. The host's
`draw_state.rs[]` array is indexed by these D3D9 numbers verbatim
(`d3d9_render.inc:588-592` names each). No sRGB-write (rs[194]=0), no
scissor-test (rs[174] never set), no two-sided stencil (rs[185] never set),
no separate-alpha-blend in the gameplay path (rs[206]=0; the game's
`SEPARATEALPHABLENDENABLE` writes are all 0).

## Texture-stage states (`SetTextureStageState`, 5 stages used: 0–4)

`ts[][1]` = `D3DTOP` (color op), `ts[][2]` = `D3DTA_*` arg1, `ts[][3]` = arg2,
`ts[][4]` = alpha op, `ts[][5/6]` = alpha args. The game's ops:
`SELECTARG1(1)`, `MODULATE(2)`, `ADD(4)`, `SUBTRACT(7)`, `ADDSELECTED? (10)`,
`MODULATEALPHA_ADDCOLOR? (25)`, `SELECTARGEEK (32)`; args: `TEXTURE(2)`,
`DIFFUSE(4)`, `CURRENT(8)`, `TFACTOR(16)`, `SPECULAR?`, `TEMPORARY(35)`,
`SELECTARGEEK(32)`, `ALPHARAW(48)`. → the classic D3D fixed combiner, 4
stages (the existing host `mr_program_state.fixed_stages[8]`).

## Sampler states (`SetSamplerState`, 4 stages: 0–3)

types 1/2/3 = `MINFILTER`/`MAGFILTER`/`MIPFILTER`: POINT(1), LINEAR(2),
? (3=none used), ANISOTROPIC(4). types 5/6/7 = `MIPMAPLODBIAS`/`MAXMIPLEVEL`/
`MIPFILTER`-class + addressing 5/6/7 WRAP(1), CLAMP(2). → standard `VkSampler`
set (wrap/clamp × point/linear).

## Vertex formats

| FVF | decode | path |
|---|---|---|
| `0x144` | `XYZ(0)`? + `DIFFUSE(0x100)` + `TEXCOORD1(0x40)`-family | fixed pipeline |
| `0x1c4` | `XYZ` + `BLENDDIFFUSE`/`SPECULAR` + `TEXCOORD` | lit |
| `0` | custom `IDirect3DVertexDeclaration9` | 286 created |

`CreateVertexDeclaration` already parses D3D9 declaration elements into
`MR_DECL_*` streams on the host (`d3d9.c` case 86, `metalrenderer.m`
vertex-attribute matching). Vulkan mirrors the same `VkVertexInputBinding`/
`Attribute` encoding.

## Primitives

Only **TRIANGLE_LIST** and **TRIANGLE_STRIP**, via
`DrawPrimitive`/`DrawIndexedPrimitive` (and the UP variants). No points,
lines, or patches. → `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`/`TRIANGLE_STRIP`.

## Shader surface

- **Vertex shaders:** vs_1_1–vs_2_0 (64 created). Halo ships
  **relative-constant** addressing (`c[-const]`); the existing
  `metalshader.c` patches a legacy `ctab20` metadata variant so MojoShader
  parses them. The SPIR-V path (DirectXShaderCompiler, or glslang via a D3D
  front-end) needs the same preprocessing.
- **Pixel shaders:** ps_2_0/ps_2_x (469 created). Up to 4 `tex2D` samples,
  `tex2Dproj`, `tex2dbias`, `def` constants, `dp3`/`mad` arithmetic. **No
  ps_3_0**, no `tex3D`, no pixel-shader cube sampling (cubes are sampled via
  fixed stages). ps_2_x is the well-supported subset.
- **Constants:** `SetVertexShaderConstantF` (256 regs) /
  `SetPixelShaderConstantF` (224 regs) — float4 arrays. The host packs them
  into a uniform buffer at draw time (`packed_float4_count * 16`).
- **Alpha test + vertex fog injection:** the existing host injects D3D's
  post-shader alpha-test (`alpha_test_ref`/`alpha_test_func`) and radial
  vertex-fog into the FS source (`inject_pixel_extras`). The Vulkan path
  must replicate these D3D fixed-function behaviours (they are NOT in the
  shader bytecode).

## Fog

Game uses **vertex fog** (fog computed in the VS, interpolated). The host
rewrites the VS to emit a fog varying and mixes toward `FOGCOLOR` in the FS
(`halo_radial_fog_terms`). Vulkan: same VS varying + FS `mix`.

## Not used (skip)

Points/lines/patches, `DrawRectPatch`/`DrawTriPatch`, clip planes (only
rs[35]), MSAA, YUV/palette textures, volume textures in draws, sRGB-write
path, two-sided stencil in the gameplay path (rs[185] not set), software
vertex processing (set but host always does HW).

## Summary: what Vulkan must cover

- 1 swapchain: `X8R8G8B8`, no MSAA, `D3DSWAPEFFECT_COPY`.
- 3 uncompressed + 3 compressed texture formats; 1 depth-stencil
  (`VK_FORMAT_D24_UNORM_S8_UINT`).
- ~35 render states → 6 pipeline sub-states.
- 4 fixed-function texture-combine stages (0–3) + up to 4 sampler stages.
- 3 FVF layouts + 286 custom vertex declarations.
- vs_1_1–vs_2_0 + ps_2_0/2_x (relative-constant preprocessing), fog
  varying, alpha-test + vertex-fog injection.
- TRIANGLE_LIST / TRIANGLE_STRIP, indexed and user-pointer draws.

The macOS Metal backend exercises every item above and is the reference
implementation for the Vulkan port.
