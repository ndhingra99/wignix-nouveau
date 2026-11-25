# Vulkan on Fermi (Quadro 2000M, GF106, NVC3): plan and notes

Target: first **Vulkan 1.0**, then **1.2 if it can be reached**, with
concessions where the hardware needs them, on the ThinkPad W520's Quadro
2000M, by porting Mesa's NVK to Fermi. Everything runs at the W520's
0.83 V default. This file is the plan, the map of the code involved, and
the running notes for the Vulkan work.

Source tags: **[open]** published code and headers (Mesa, nouveau, the
NVIDIA class headers shipped in Mesa), **[w520]** measured on the W520.

Paths below are relative to a Mesa checkout unless they start with
`kernel:`, which means the nouveau tree under `drivers/gpu/drm/nouveau`.
The matching Mesa and kernel patches are in `patches/`.

## 0. Status (keep this current)

Results below were measured on the W520 on 2026-11-13/14 and re-run in
full on 2026-11-24 after the rebase onto Mesa 26.2.7: every offscreen test
(vkclear, vkds, vkbuf, vkcomp, vkimg, vkmisc, vksi, vkgfx, vkgfx2) passes
with no kernel messages. 2026-11-24 environment:

- W520: installed system (not the live disc), coreboot discrete-only,
  kernel 7.3.1_24. `/tmp` is mounted noexec there, so the
  test driver and tests live in the test directory (ICD file and `t.sh` point
  there). The packaged kernel carries the copy-engine fix: `vkds` B2-B4
  pass without any test module.
- Mesa 26.2.7: upstream changed nothing under `src/nouveau` since 26.2.6,
  so `patches/nvk-fermi.diff` applied unchanged; it is now generated
  against 26.2.7. Working tree
  the Mesa 26.2.7 build tree` (26.2.7 + every package
  patch + the NVK diff); the old 26.2.6 tree `mesa-dbg` is kept.
- the x86_64 build chroot set up again (deps through
  `xbps-src -f patch mesa`); `tools/buildvk.sh` builds in about 3 min.
- The feature list and API version are still Kepler's claims (1.2.354).

| what | state |
| --- | --- |
| `vulkaninfo` | completes: `Quadro 2000M (NVK GF106)`, driver NVK |
| render-pass clear + read-back (`tests/vkclear.c`) | correct, 4096/4096 pixels |
| triangle (`vkclear tri`) | correct, 2016 + 2080 pixels |
| texture upload round trip (`tests/vkds.c` A) | exact, 0 bytes differ |
| D24S8 clear, depth read-back (`vkds` B1) | correct (0x800000, stencil byte 0x5a) |
| `vkcube` in an X window | correct on 2026-11-24 with Mesa 26.2.7 and that day's changes (`probe/vkcube-2627.png`); earlier: capture on 11-12 (`probe/vkcube-3.png`); on 11-13 with compute, the copy-engine fix and the converted launches it ran 45 s with no kernel message and the picture on the W520 was judged correct |
| stencil aspect to/from a buffer, single-aspect writes into D24S8 (`vkds` B2-B4) | correct on the copy engine with the fixed firmware image (linux-sandy rev 16 on); since 2026-11-24 on the packaged kernel 7.3.1_24, no test module |
| compute dispatch (`tests/vkcomp.c` D1) | works: a shader writing a storage buffer, 64 to 3 840 000 invocations (D1); a 4x4x2 group on a 3D grid with shared memory, a barrier, a uniform buffer and `gl_NumWorkGroups` (D2); the same through `vkCmdDispatchIndirect`, also with a zero count (D3, D4); 0 words wrong in all |
| `vkCmdFillBuffer`, `vkCmdUpdateBuffer`, `vkCmdCopyBuffer`, events (`tests/vkbuf.c` C1-C4) | correct |
| timestamp queries, compute-invocation statistics, `vkCmdCopyQueryPoolResults`, conditional rendering (`tests/vkmisc.c` M1-M4) | correct |
| arrays of descriptors indexed at run time: sampled images, separate textures and samplers, array textures, `textureSize`, `texelFetch`, storage images, uniform and storage buffers (`tests/vkdyn.c` Y1-Y8, compute and `VKDYN_FRAG=1` fragment) | correct, 0 of 28 words wrong in both stages (2026-11-24) |
| texture and sampler units per stage | 9 + 9 in use at once in `vkdyn`; 32 textures and 16 samplers are what the binding limits allow; more than 16 textures untested |
| storage buffers | work from a compute shader (D1); from graphics stages untested |
| textures sampled in a compute shader (`tests/vkimg.c` I1) | correct, 0 bytes wrong |
| storage images (`tests/vksi.c`: 17 formats x 2D, 2D array, 3D; store, load, copy cross-check) | all 51 cases correct; atomics (`vkgfx2` H5) and texel buffers (H6) correct; multisampled storage images untested |
| fragment-stage storage buffer and image writes, geometry shader, tessellation, indexed/instanced/indirect draws (`tests/vkgfx.c` G1-G4) | correct |
| 4x multisampling with resolve, per-sample `texelFetch`, blits, transform feedback (`tests/vkgfx2.c` H2-H4) | correct |
| multiview (H1) | correct at every position among the other tests (`VKGFX2_MV_AT=0..6`) |
| draws and compute dispatches mixed in one program | correct after three fixes for state the two classes share (section 7): draw parameters, root table size, first bind |
| Dawn's required features (`tests/vkdawn.c`, 2026-11-24) | all correct: robust storage/uniform/vertex access, cube arrays, sample-rate shading, independent blend, depth bias clamp, 32-bit indices, push constants after compute work (Fermi compute shaders use no constant buffers, section 7) |
| WebGPU through Dawn in Helium 0.22 (`tests/webgpu/*.html`, 2026-11-24) | headless: adapter `nvidia`/`fermi` for core and compatibility; compute, render with a uniform buffer and a textured draw all correct. On screen: a canvas presented at 60 fps, correct picture (`probe/helium-webgpu-canvas.png`) |
| feature list / API version | Vulkan 1.1 with an explicit Fermi list of extensions and features, real per-stage unit limits (2026-11-24, section 7) |
| conformance suite | not run |
| package | `srcpkgs/mesa/patches/nvk-1-fermi-gf106.patch`, mesa 26.2.7_3; `w520-modifications` 1_7 with the Helium flags; committed, packages not built yet |

Order of work agreed: go as far as possible towards 1.2
first, fix the version and feature claims last. Then: solve every known gap first, then talk conformance.
Target since 2026-11-24 : an honest Vulkan 1.1 for WebGPU in
Helium/Chromium (Dawn needs 1.1 and the features listed in section 7,
2026-11-24 entry). Known gaps left: NVK's compute-based image copies
trapping; the 2D engine for blits (blits work through the 3D path);
multisampled storage images; tests for the features Dawn requires that
are claimed but untested (BC formats, cube arrays, sample-rate shading,
independent blend, robust buffer access, depth bias clamp); then the
honest feature list and API version.

What exists in the working tree that is only for bring-up and has to
go or be cleaned before packaging: the `NVK_FERMI_DEBUG` prints in
`nvk_physical_device.c` (class list) and `nvk_cmd_copy.c` (copy engine
addresses, removed 11-13 together with the `NVK_FERMI_LOWVA` switch
once the copy engine worked).

## 1. Starting point

Sections 1 to 4 are the plan as written at the start (2026-11-13, target
Vulkan 1.0, copy engine assumed unusable). The target is now 1.2, the copy
engine works, and optional stages (geometry, tessellation, transform
feedback, multiview) are implemented: section 0 and the phase checklists
are the current state, sections 7 and 9 say what changed and why.

- Nobody ships Vulkan on Fermi. NVIDIA declined in 2016; NVK is
  conformant from Kepler (Mesa 25.2, Collabora) and rejects older chips.
  Collabora's stated blockers: no bindless textures on Fermi, and the
  copy engine differs.
- On the W520 **[w520]**: `vulkaninfo` lists only llvmpipe; with
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nouveau_icd.x86_64.json`
  the loader reports "Failed to detect any valid GPUs".
  `libvulkan_nouveau.so`, `vulkaninfo` and `vkcube` are installed.
- The kernel logs `drm: failed to create ce channel, -22` on this card
  (`kernel:nouveau_drm.c:354`): no copy-engine channel.

## 2. What Vulkan 1.0 demands that matters here

Mandatory in 1.0: graphics and compute pipelines, vertex + fragment +
compute stages, uniform and storage buffers, sampled images, separate
samplers, storage images, input attachments, the copy/blit/clear/resolve
commands, queries (occlusion), and the limits table. Optional, and to
be left off: geometry and tessellation stages, sparse memory, dynamic
indexing of descriptor arrays, anisotropy, multi-viewport, 64-bit
types, most compressed formats beyond BC.

Limits that Fermi's 16 texture units and 16 sampler units per stage
(`src/gallium/drivers/nouveau/nvc0/nvc0_screen.c:141`) have to meet:

| limit | 1.0 minimum | Fermi |
| --- | --- | --- |
| maxPerStageDescriptorSamplers | 16 | 16 sampler units |
| maxPerStageDescriptorSampledImages | 16 | 16 texture units |
| maxPerStageDescriptorStorageImages | 4 | no image units: memory access by address (section 5.3) |
| maxPerStageDescriptorUniformBuffers | 12 | constant buffer slots per stage (nvc0 uses 16, some reserved) |
| maxPerStageDescriptorStorageBuffers | 4 | by address, as nvc0 |
| maxPerStageDescriptorInputAttachments | 4 | count against the 16 texture units |
| maxBoundDescriptorSets | 4 | software |

So the minimum fits exactly, with nothing to spare on textures: input
attachments and any driver-internal texture use share the 16 units.

## 3. What already exists

### 3.1 Kernel

- NVK's memory interface (`VM_INIT`, `VM_BIND`, `EXEC`) is registered
  for all chips (`kernel:nouveau_drm.c:1320-1327`), and the map path
  takes Fermi's address-space class:
  `kernel:nouveau_uvmm.c:147-150` (`NVIF_CLASS_VMM_GF100`, shared with
  Kepler). `EXEC` needs a GPFIFO channel of NV50 class or later
  (`kernel:nouveau_exec.c:382`).
- Sparse page tables exist from GM20x only
  (`kernel:nvkm/subdev/mmu/vmmgm200.c:28-63`;
  `kernel:nvkm/subdev/mmu/vmmgf100.c:242`). Not needed for 1.0.
- Unknown until tried: whether `VM_INIT` + `EXEC` actually run on NVC3.
  Kepler uses the same paths, so the expectation is yes.

### 3.2 Compiler (NAK, `src/nouveau/compiler`)

- `nak/sm20.rs` (Collabora 2025): encoder for shader models 20 to 31.
  `ShaderModel20::new` accepts `sm >= 20 && sm < 32` (line 24);
  `encode_sm20_shader` (line 3090) is the Fermi path,
  `encode_sm30_shader` (line 3152) the Kepler A one.
  `nak/ir.rs:9583-9597`: `is_fermi()` = 20..29, `is_kepler_a()` = 30..31.
- Textures by unit already exist in the IR: `TexRef::Bound(u16)`
  (`nak/ir.rs:1973`), encoded for SM20 in every texture op
  (`nak/sm20.rs:1746`, 1785, 1831, 1877, 1912, 1947: unit in bits
  32..40). The bindless form asserts `sm >= 30`; the constant-buffer
  form panics on SM20.
- The NIR side knows the three reference kinds
  (`nak_private.h:146-163`: `BINDLESS`, `BOUND`, `CBUF`) and
  `nak/from_nir.rs:1917` translates `BOUND`. Nothing produces `BOUND`
  today: `nak_nir_lower_tex.c:50` and `:224` choose `BINDLESS` or `CBUF`.
- Shader model per chipset: `winsys/nouveau_device.c:54`
  (`sm_for_chipset`), used at `:531`. To check: what it returns for
  0xc3.
- Not available on SM20: image load/store ops `OpSuLdGa`/`OpSuStGa`
  assert `sm >= 30` (`nak/sm20.rs:2253`, 2291); "SM20 only supports
  global atomics" (`:2500`).

### 3.3 NVK (`src/nouveau/vulkan`, 29k lines)

Sites that already know about Fermi:

- `nvk_device.c:360`: vertex attribute buffer memory for
  `FERMI_A <= cls < MAXWELL_A`; `nvk_cmd_draw.c:648`: programs it
  (`SET_VAB_MEMORY_AREA`).
- `nvk_shader.c:617`: shader alignment 0x40 on Fermi, 0x80 from Kepler;
  `:630`: Kepler-only padding.
- `nvk_queue.c:341`: `SET_OBJECT` for the Fermi M2MF class, with the
  comment "we absolutely do not support Fermi, but if somebody wants to
  toy around with it, this is a must".
- `nvk_queue.c:99-102`, `:123-126`: texture header and sampler pools
  set through `NV9097` methods (the pools exist on Fermi; nvc0 uses the
  same tables).
- Image layout library `src/nouveau/nil`: Fermi branches in
  `tiling.rs:144`, `image.rs:749`, `descriptor.rs:933`, 972, 996
  (texture header format).
- Macros: `src/nouveau/mme` has the "Fermi" macro language
  (`mme_fermi*.c`), which is what Fermi through Maxwell run.
- Class headers: `src/nouveau/headers/nvidia/classes/cl9097.h`,
  `cl9197.h`, `cl9297.h`, `cl9097tex.h`, `cl9039.h` (M2MF), `cl902d.h`
  (2D), `cl90b5.h` (copy), `cl90c0.h`, `cl91c0.h` (compute), `cl906f.h`
  (GPFIFO).

Sites that stop Fermi:

- `nvk_physical_device.c:1503`: `cls_eng3d < KEPLER_A` ->
  `VK_ERROR_INCOMPATIBLE_DRIVER`; `:94` conformance list;
  `:1508` `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER` for unlisted chips.
- `nvk_sampler.c:211`, `:236`: asserts Kepler; cube seam and
  unnormalized-coordinate fields are written with `NVA097` layouts.
- `nvk_cmd_draw.c:386`: `SET_TEXTURE_INSTRUCTION_OPERAND` (Kepler only,
  already guarded).
- Descriptors: `nvk_nir_lower_descriptors.c:1288-1319` builds a combined
  texture/sampler handle for every texture instruction (bindless).
- Engines used by the command code: copy class `NV90B5` in
  `nvk_cmd_copy.c`, `nvk_upload_queue.c`, `nvk_event.c`,
  `nvk_query_pool.c`, `nvk_cmd_buffer.c`, `nvk_cmd_draw.c`; Kepler
  P2MF `NVA040` in `nvk_cmd_copy.c`; compute `NVA0C0` in
  `nvk_cmd_dispatch.c`, `nvk_cmd_indirect.c`, `nvk_queue.c`,
  `nvk_device.c`. The 2D engine (`NV902D`) is not used at all.

### 3.4 The GL driver as the reference for Fermi behaviour

`src/gallium/drivers/nouveau/nvc0` does on Fermi everything the port
needs, in GL terms:

| need | where nvc0 does it |
| --- | --- |
| bind textures and samplers to units | `nvc0_tex.c:478` `nvc0_validate_tic` (`BIND_TIC(s)` at 538), `:622` `nvc0_validate_tsc` (`BIND_TSC(s)` at 674) |
| images without image units | `nvc0_tex.c:1131` `nvc0_validate_suf`; shader side `codegen/nv50_ir_lowering_nvc0.cpp:2421` `processSurfaceCoordsNVC0`, `:2569` `handleSurfaceOpNVC0`, `:1935` `handleSUQ` |
| texture instruction operands on Fermi | `codegen/nv50_ir_lowering_nvc0.cpp:931` `handleTEX`; encoder `codegen/nv50_ir_emit_nvc0.cpp:1321` |
| compute launch (method-driven, 0x90c0) | `nvc0_compute.c:30` setup, `:121-314` state, `:360` input upload, `:381` `nvc0_launch_grid` |
| memory copies and uploads without a copy engine | `nvc0_transfer.c:15` `nvc0_m2mf_transfer_rect`, `:199` `nvc0_m2mf_push_linear`, `:281` `nvc0_m2mf_copy_linear` |
| image copies and scaled blits | `nvc0_surface.c:89` `nvc0_2d_texture_set`, `:164` `nvc0_2d_texture_do_copy`, `:1467` `nvc0_blit_eng2d`, `:1235` `nvc0_blit_3d` |
| clears | `nvc0_surface.c:695` `nvc0_clear`, `:289` `nvc0_clear_render_target` |
| constant buffer upload through the 3D engine | `nvc0_transfer.c:576` `nvc0_cb_bo_push` |
| 3D engine initialisation, macros, fence | `nvc0_screen.c:767` `nvc0_screen_create`, `:509` `nvc0_magic_3d_init`, `:469` `nvc0_graph_set_macro`, `:578` `nvc0_screen_fence_emit` |
| table sizes | `nvc0_screen.h:14` TIC table 2048 entries |

## 4. Design decisions

1. **Port NVK, do not write a new driver.** The kernel interface,
   compiler encoder, image layout and macro language are shared with
   Kepler A. Fermi becomes one more hardware generation inside NVK,
   selected by `cls_eng3d < KEPLER_A` and `sm < 30`.
2. **Descriptors: keep NVK's global image and sampler tables, add a
   per-stage unit map.** A texture instruction names a texture unit and
   a sampler unit; the compiler assigns units to the (set, binding,
   element) pairs a shader uses, at most 16 + 16 per stage, and records
   the map with the shader. At draw time NVK writes `BIND_TIC` /
   `BIND_TSC` for the units whose descriptor changed, taking the table
   indices from the bound sets. A run-time index into a descriptor array
   gets the whole array as consecutive units (section 9).
3. **Storage images and storage buffers by address**, as nvc0 does on
   Fermi: the descriptor carries address, size and format data in a
   constant buffer and the shader computes the address and uses global
   loads and stores.
4. **Copies without the copy engine**: M2MF for buffer-to-buffer,
   buffer-to-image and uploads, the 2D engine for image-to-image and
   blits, the 3D engine for the rest (resolve, depth/stencil).
5. **Compute through the Fermi compute class** with method-driven
   launch.
6. **Everything optional stays off** until 1.0 passes: no geometry or
   tessellation stages, no sparse, no extensions beyond what swapchain
   use needs (`KHR_swapchain`, `KHR_surface` and friends).
7. **Fermi changes are private to our tree** (WigNix patches), same
   rules as the GL work; method names and layouts from the published
   class headers where they exist.

## 5. Phases

Each phase ends with something that runs on the W520.

### Phase 0: build and probe

- [x] Test build with `-Dvulkan-drivers=nouveau` in the 64-bit test
      tree (section 8).
- [x] Lift the gate (`nvk_physical_device.c:1503`), run
      `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vulkaninfo` and `vkcube` on
      the W520 (section 7, 2026-11-13).
- [x] Record what the probe reports: 3D class (9097/9197/9297), M2MF,
      2D, copy and compute classes (`winsys/nouveau_device.c:531-536`),
      `sm_for_chipset(0xc3)`.
- [x] Decide the test set: `vkcube`, `vulkaninfo`, small own tests like
      the GL ones in the kernel tests`, then the conformance suite
      (dEQP-VK, 1.0 mustpass list) once basic rendering works.

### Phase 1: device comes up, first triangle

- [x] Device and queue init on Fermi: contexts for 3D, 2D, M2MF,
      compute; 3D engine initial state compared against
      `nvc0_screen.c:767` and `nvc0_magic_3d_init`.
- [ ] (last, decided last) Features, limits and format tables for Fermi in
      `nvk_physical_device.c` (1.0 only, section 2).
- [x] Shader upload with 0x40 alignment (`nvk_shader.c:617`); vertex
      and fragment shaders without textures compile through
      `encode_sm20_shader` and run.
- [x] Clears and a triangle into an image; read back through a buffer
      copy. Needs the first copy path (phase 3 item, M2MF).

### Phase 2: textures and samplers (the bindless replacement)

- [x] Compiler: produce `NAK_NIR_TEX_REF_TYPE_BOUND` for `sm < 30`
      (`nak_nir_lower_tex.c:50`, `:224`); carry the sampler unit as
      well as the texture unit through `TexRef::Bound` to the encoder
      (nvc0 puts the texture unit and the sampler unit in separate
      fields: `nv50_ir_emit_nvc0.cpp:1321`; `sm20.rs` writes one 8-bit
      field today).
- [x] NVK: unit assignment in `nvk_nir_lower_descriptors.c` in place
      of the combined handle (`:1288-1319`); the map stored with the
      shader.
- [ ] (draw and dispatch done, `vkimg` I1 samples in compute; every
      draw rebinds everything, dirty tracking open) NVK: `BIND_TIC` / `BIND_TSC` at draw and dispatch time from the
      bound descriptor sets, with dirty tracking
      (`nvk_cmd_draw.c`, `nvk_cmd_buffer.c`).
- [ ] (Kepler-only fields skipped; unnormalized coordinates and cube
      seam behaviour still to do) Sampler state for Fermi (`nvk_sampler.c:211`, `:236`): cube seam
      filtering and unnormalized coordinates are Kepler sampler fields.
      On Fermi "normalized coordinates" is a property of the texture
      header, so an image view used with an unnormalized sampler needs
      a second header entry.
- [ ] Texture header pool size within what Fermi accepts (nvc0 uses
      2048 entries).
- [ ] Input attachments as bound textures.

### Phase 3: copies, blits, uploads

- [x] Buffer copies, buffer/image copies, uploads and the upload queue
      on M2MF (`nvk_cmd_copy.c`, `nvk_upload_queue.c`) following
      `nvc0_transfer.c`.
- [x] Copies that rearrange components (stencil aspect, single aspect
      of packed depth/stencil) on the copy engine in the firmware's own
      method layout (`nouveau_copy_rect_fermi_ce`); needs the kernel's
      fixed image (linux-sandy rev 16, section 7).
- [ ] (blits work through the 3D path, `vkgfx2`; 2D engine not used)
      Image copies and blits on the 2D engine following
      `nvc0_surface.c:89-207`, `:1467`; the 3D fallback for what the 2D
      engine cannot do.
- [x] The other users of the copy class: events (`nvk_event.c`), query
      result copies (`nvk_query_pool.c`) (`vkbuf` C4, `vkmisc` M3).
- [x] Multisample resolve (`vkgfx2` H2).

### Phase 4: buffers and images as shader resources

- [ ] (uniform buffers work, `vkcomp` D2; dynamic offsets untested)
      Uniform buffers on the per-stage constant buffer slots; dynamic
      offsets.
- [x] Storage buffers by address, from compute (`vkcomp` D1) and
      fragment shaders (`vkgfx` G1).
- [x] Storage images by address: SM20 variant of
      `nak_nir_lower_image_addrs.c`, 17 formats x 2D/array/3D (`vksi`);
      multisampled storage images untested.
- [ ] (image atomics work, `vkgfx2` H5; buffer atomics untested)
      Atomics limited to what SM20 has (global).

### Phase 5: compute

- [x] Compute class setup and method-driven dispatch following
      `nvc0_compute.c` (`nvk_cmd_dispatch.c` `nvk_cmd_dispatch_fermi`,
      `nvk_push_dispatch_state_init`, `nvk_queue.c` local memory).
- [x] Indirect dispatch: the 3D macro `NVK_MME_DISPATCH_INDIRECT`
      writes the counts to the root table and the invocation counter,
      then `NVK_MME_FERMI_LAUNCH_GRID`, called on the compute
      subchannel with the same three words, launches.
- [x] Compute shaders for SM20: NAK's output runs as is (no header,
      thread and group ids from special registers 0x21.. and 0x25..,
      global stores with 64-bit addresses).

### Phase 6: the rest of 1.0 and conformance

- [ ] (done and tested: timestamps, compute-invocation statistics,
      events, indexed/instanced/indirect draws; open: occlusion queries,
      secondary command buffers) Occlusion and pipeline statistics
      queries, events, secondary command buffers, indirect draws (macros).
- [ ] (D24S8, vkcube's depth buffer, 4x MSAA, 2D/array/3D images
      work; blending, cube images and mipmaps untested)
      Depth/stencil formats and operations, blending, MSAA, cube and
      3D and array images, mipmaps.
- [x] WSI: X11 swapchain, `vkcube` on screen.
- [ ] dEQP-VK 1.0 mustpass on the W520; fix until it passes or the
      failures are understood hardware limits.
- [ ] Package: Fermi patches for `srcpkgs/mesa` (`mesa-vulkan-nouveau`
      subpackage), 64-bit and i686 multilib.
- [ ] First real users: DXVK needs Vulkan 1.3, so it is out of reach;
      vkQuake, zink (needs more than 1.0 in practice), native Vulkan
      1.0 titles.

## 6. Known risks

- The texture limit is exactly the Vulkan minimum; a shader using 16
  sampled images and input attachments together does not fit.
- Unknown hardware behaviour NVK relies on from Kepler up may differ on
  Fermi without any published description; the closed driver never did
  Vulkan on Fermi, so there is nothing to trace for Vulkan-specific
  sequences. GL traces and nvc0 are the references.
- The image-variation problem on this card (the Fermi notes
  section 6) is unsolved and will affect conformance runs that compare
  images.
- Two heavy pieces, storage images by address and descriptors by unit,
  both touch the compiler and the driver at once.
- Vulkan 1.0 alone runs little modern software; the value is the
  platform (and zink, if enough extensions follow later).

## 7. Notes (running)

- 2026-11-13: plan written. Kernel 7.3.1_13 and mesa 26.2.6_9 on the
  W520.
- 2026-11-13, phase 0 **[w520]**: NVK built with the gate at Fermi and
  run on the W520. `vulkaninfo` completes and reports
  `Quadro 2000M (NVK GF106)`, discrete GPU, driver NVK, API 1.2.354
  (the version NVK claims for anything below Maxwell; ours has to say
  1.0). Output in `probe/vulkaninfo-summary-2026-11-13.txt`. So on
  NVC3 the kernel accepts `VM_INIT`, a channel with 3D, 2D, M2MF and
  compute subchannels, and the first `EXEC` with the 3D engine's
  initial state and all macros. No kernel message during these runs.
  What it took, in the order hit (all in `patches/nvk-fermi.diff`):
  1. `DRM_NOUVEAU_CHANNEL_ALLOC` returned `EINVAL`: before Kepler the
     kernel wants two DMA object names in `fb_ctxdma_handle` /
     `tt_ctxdma_handle` and rejects the `~0` + engine selector NVK
     sends (`kernel:nouveau_abi16.c:393-429`). The GL driver passes
     0xbeef0201 / 0xbeef0202 (`gallium/drivers/nouveau/nouveau_screen.c:302`);
     `winsys/nouveau_context.c` now does the same for chipsets below
     0xe0.
  2. `nvk_sampler.c`: cube seam and coordinate normalization fields
     only written from Kepler on.
  3. `nvk_queue.c`: the M2MF `SET_OBJECT` only on queues that have the
     M2MF engine (it asserted on the subchannel mask).
  4. `nvk_cmd_dispatch.c`: the indirect-dispatch macro builds a launch
     descriptor, which NAK refuses for Fermi ("Unsupported shader
     model", `nak/qmd.rs:754`); it is empty on Fermi until phase 5.
  5. `nvk_shader.c`: on Fermi the header start is what gets aligned to
     0x40 (`nvc0_program.c:775-781`), not the first instruction.
- `vkcube` then creates its device, swapchain and pipeline layout, and
  NAK compiles for shader model 20 until the first texture instruction:
  `nak_nir_lower_tex.c:443` has operand layouts for shader model 30 and
  up only. That is the start of phase 2. Fermi's operand order is in
  `codegen/nv50_ir_lowering_nvc0.cpp:931` (`handleTEX`, the branch for
  chipsets below NVE4).
- 2026-11-13, phase 2 started **[w520]**. Probe values: 3D class 9097,
  2D 902d, M2MF 9039, copy 90b5, compute 90c0, chipset 0c3, shader
  model 21 (`NVK_FERMI_DEBUG=1` prints them).
  - Units: NVIDIA's own header gives 8-bit slot fields for both binds
    (`cl9097.h:3612` `BIND_GROUP_TEXTURE_SAMPLER`: slot 11:4, index
    24:12; `:3619` `BIND_GROUP_TEXTURE_HEADER`: slot 8:1, index 30:9),
    and the instruction has 8 bits for the texture unit (bits 32..40)
    but only bits 40..44 for the sampler unit (45 and up are other
    fields: `nak/sm20.rs` `OpTex`). So more than 16 texture units per
    stage look possible, samplers stay at 16. The driver uses 32 and 16
    (`NVK_MAX_TEX_UNITS`, `NVK_MAX_SAMPLER_UNITS`); more than 16
    textures is untested on hardware.
  - Compiler: for `sm < 30` the texture "handle" the driver passes is a
    constant, texture unit in bits 0..7 and sampler unit in bits 8..11
    (`nak_nir_lower_tex.c` `has_bound_tex`, `tex_handle_as_bound`); it
    becomes `NAK_NIR_TEX_REF_TYPE_BOUND` / `TexRef::Bound`, and
    `sm20.rs` writes both fields. Operand order for Fermi is the Kepler
    one without the handle (array index, coordinates, sample, LOD,
    offsets, depth compare), per the comment block in
    `codegen/nv50_ir_lowering_nvc0.cpp:960-975`.
  - Driver: `nvk_nir_lower_descriptors.c` assigns units per shader
    (`tex_unit_for_deref`, `get_tex_unit`) and stores them in
    `struct nvk_cbuf_map` (`tex_units`, `sampler_units`, in
    `nvk_shader.h`; the map is already serialized with the shader).
    Texel fetches get the device's sampler 0 as on Kepler.
    `nvk_cmd_draw.c` `nvk_cmd_flush_gfx_tex_units` binds every unit of
    every bound stage on each draw (no dirty tracking yet), reading the
    descriptor from the bound set or push set
    (`nvk_cmd_buffer_get_tex_unit_desc`). Only constant array indices.
  - Subchannels: before Kepler each one needs `SET_OBJECT`. Without it
    the copy subchannel logged `fifo: PBDMA0: EMPTY_SUBC ... subc 4`.
    `nvk_queue.c` now sets the copy and 2D objects on Fermi.
  - Image copies went through a compute shader (`nvk_cmd_meta.c`
    `nvk_meta_image_copy_compute_supported`), which needs the launch
    descriptor; on Fermi they take the copy-engine path instead. The
    copy class 90b5 exists on this card and accepts NVK's methods
    without a kernel complaint.
  - `vkcube` now runs its 2000 frames with no crash and no kernel
    message. The window is wrong: mostly black with a band of striped
    garbage at the bottom (`probe/vkcube-2.png`). Not yet known which of
    clear, draw, texture upload, or the copy for presentation is wrong.
- 2026-11-13, first correct output **[w520]**. `tests/vkclear.c`
  (offscreen: render-pass clear of a 64x64 RGBA8 image, copy to a
  buffer, read back; `tri` adds a full-screen-half triangle from
  `tri.vert`/`tri.frag`, compiled with `glslangValidator -V` on the
  build host) run through `tools/t.sh` on the W520:
  - With the copy class 90b5 the buffer came back untouched and the
    kernel said nothing: NVK's copy-engine methods do nothing on this
    card. Copies now go through M2MF on Fermi
    (`nvk_cmd_copy.c` `nouveau_copy_rect_m2mf`, following
    `nvc0_transfer.c:15`): tiling mode = x | y << 4 | z << 8 from the
    nil tiling, pitch = row stride, at most 2047 lines per launch, EXEC
    bit 20 plus 0x10 / 0x100 for linear source / destination. No
    component remapping (so no depth/stencil aspect copies yet). Fermi
    transfer queues get the M2MF and 2D engines
    (`nvk_queue_engines_from_queue_flags` now takes the device info).
  - Result: **clear = 4096 of 4096 pixels (255,127,64,255)**, the exact
    clear colour. The 3D engine clear, the image layout and the M2MF
    read-back are right.
  - The triangle does not appear (0 pixels), with no kernel message.
    Next: dump what NAK emits for `tri.vert`/`tri.frag` (`NAK_DEBUG`)
    and compare program binding and shader headers with nvc0.
- The compiler side of the triangle looks right: `NAK_DEBUG=print`
  (`probe/nak-tri.txt`) shows 20 instructions for the vertex shader and
  6 for the fragment shader, which ends in r0..r3 = (0, 1, 0, 1) and
  `exit`. So the missing triangle is in how the draw is set up or
  launched (program binding and headers, viewport/scissor, the draw
  macro, the vertex attribute area), not in code generation. Next
  step: `NVK_DEBUG=push_dump` on `vkclear tri` and a line-by-line
  comparison with what nvc0 sends for a draw.
- 2026-11-13, **first triangle and vkcube** **[w520]**. The missing
  triangle was the upload queue: `nvk_upload_queue.c` sends shader
  code and the texture header / sampler tables to the GPU with the copy
  class, so on Fermi nothing ever arrived. It now uses an M2MF context
  on Fermi (`nvk_upload_queue_uses_m2mf`): linear copies of at most
  128 KiB (`OFFSET_OUT`, `OFFSET_IN`, `LINE_LENGTH_IN` + count 1,
  `EXEC` = bit 20 | 0x10 | 0x100), `SET_OBJECT` once at queue init, and
  fills done as copies from a block holding the value.
  - `vkclear tri`: 2016 triangle pixels + 2080 clear pixels of 4096.
  - `vkcube` renders correctly (`probe/vkcube-3.png`): textured,
    depth-tested, lit cube, presented in an X window, 2000 frames, no
    kernel message. That exercises the bound texture and sampler units,
    uniform buffers, the depth buffer, the draw macros and the M2MF
    presentation copy.
- 2026-11-13, uploads and depth/stencil **[w520]**
  (`tests/vkds.c`):
  - A, texture upload: buffer -> RGBA8 image -> buffer through M2MF,
    0 of 12288 bytes differ.
  - B, D24_UNORM_S8_UINT cleared by a render pass to 0.5 / 0x5a: depth
    reads back 0x800000 on every pixel and the stencil byte is 0x5a.
    The depth aspect of Z24S8 going to a buffer is copied as it is by
    M2MF (the buffer's fourth byte is undefined by the API).
  - Barriers needed the M2MF and 2D subchannels in
    `nvk_cmd_flush_wait_dep` (`WAIT_FOR_IDLE` on those classes).
- The copy engine on Fermi **[open]** **[w520]**:
  - It runs nouveau's own firmware, whose method layout is not the
    published 90b5 one: `kernel:nvkm/engine/ce/fuc/com.fuc` (tiling at
    0x200/0x220, `EXEC` 0x300 with 0x1 = use FORMAT, 0x10/0x100 linear,
    addresses/pitches/counts/FORMAT/constants at 0x30c..0x334; FORMAT
    is the same component-remap word as the 90b5 class). That is why
    NVK's methods did nothing. `nouveau_copy_rect_fermi_ce` in
    `nvk_cmd_copy.c` speaks this layout.
  - `SET_OBJECT` on its subchannel: with engine 0 the methods reach the
    3D engine (`gr: ILLEGAL_CLASS ... class 90b5`); with engine 4 (the
    first copy engine in `gf100_fifo_runl_ctor`) the first launch gets
    the channel disabled (`fifo: ... errored - disabling channel`,
    `channel killed`) with no reason logged. The kernel itself never
    uses this engine on Fermi (`MM: using M2MF for buffer copies`,
    `failed to create ce channel, -22`), so this path may never have
    worked from a normal channel.
  - Until that is solved, only copies that rearrange components fail:
    the stencil aspect to or from a buffer, and writing one aspect of a
    packed depth/stencil image. Plain copies use M2MF.
  - 2026-11-13, looked at with a debug module load
    (`reload.sh "" "fifo=debug,ce=debug"`, kernel 7.3.1_13 rev 15
    module): the engine does run our launch. What the kernel reported
    before the reload was a page fault from PCOPY0
    (`fifo: fault 00 [READ] at 00000000fdf1b000 engine 15 [PCE0] ...
    PT_NOT_PRESENT`, and `[WRITE] at 00000000fdee4000`), where the
    addresses we gave were `0x3e_fdee8000` / `0x3e_fdee4000`
    (`NVK_FERMI_DEBUG=1` prints them): the low 32 bits arrive, the
    high byte does not, although the firmware source stores and
    programs it (`com.fuc` `cmd_exec_set_surface_linear`). After the
    reload the same launch leaves the engine busy until the kernel
    resets it (`ce0: 11000205: busy 1 ... resetting...`). NVK's
    addresses are always above 4 GiB (a bottom-up heap still starts at
    `0x1_0211...`), and the kernel never drives this engine on Fermi,
    so the high-address path of this firmware has probably never run.
  - Which firmware (2026-11-13): NVIDIA's files are used only by the
    graphics engine (`nvc3_fuc409c/d`, `fuc41ac/d`; `gr/gf100.c`
    `NvGrUseFW`). The copy engine always runs nouveau's built-in
    `gf100.fuc3.h`: `ce/gf100.c` sets `.code.data`, so
    `engine/falcon.c:192` never asks for `nouveau/nvc3_fuc104`, and the
    W520 has no such file (the fetcher only produces fuc084/085/086 for
    video and the four graphics files).
  - Where the high byte should go, by the firmware source: source
    low/high/pitch at engine registers 0x808/0x818/0x828, destination
    at 0x80c/0x81c/0x82c, high byte shifted to bits 16..23; x counts
    0x810/0x814, control 0x800 (bit 0 launches). On Fermi the firmware
    never writes the source/destination target registers (0x604,
    0x608; they read 0x110 after reset), where the GT215 build loads
    DMA objects. Both the read and the write fault with a zero high
    byte, so either the high byte's place or the target setup is wrong
    for this chip. Not settled.
  - `tools/mmio.py` reads registers through the PCI resource file
    (root, W520). The kernel resets the engine on the fault, so the
    registers show reset values afterwards, not the failed launch.
    Do not write engine registers from the host: an all-ones write to
    0x104800 launches a copy, and the read-back masks were not
    consistent (host writes look filtered).
  - NVIDIA's module was not mined for its copy-engine program (the
    attempt was stopped; only helper functions returning the register
    bases were seen). The fix below comes from the open envytools
    register docs (`envytools` `pcopy.xml`,
    `falcon.xml`) and experiments.
  - **Solved 2026-11-13.** Test module (`patches/kernel-ce-test.diff`:
    register dump at the engine's fault, run-time port value, image
    loaded from `nouveau/ce_test_code|data`; `tools/fuc2bin.py` makes
    those files from `gf100.fuc3.h`; `tools/ceport.sh <port> <0|1>`
    runs one try). Findings, in order (`probe/ce-run*.txt`):
    - At the fault the firmware *had* written the high byte
      (0x818 = 003e0010, 0x81c = 003e0000): the hardware ignored it.
    - Port values 0, 0x220, 0x330 for 0x604/0x608: no change.
    - With every test allocation below 4 GiB (`NVK_FERMI_LOWVA=1`)
      the copies run without a fault: the engine itself works.
    - Control register bits (0x800): bit 16, 19, 20 no change;
      **bit 10 makes the engine use the high bytes.** envytools
      marks it "UNK10, always set"; nouveau's image never set it.
    - The remaining fault was NVK's own barrier: its "wait for the
      copy engine" is a zero-size 90b5 launch (methods 0x418, 0x41c,
      0x300 = 0x586), which this firmware runs as a raw copy with
      left-over state. It corrupted 576 stencil pixels and the depth
      at low addresses and faulted at high ones. On Fermi the barrier
      now sends the firmware's method 0x304 (write-cache flush)
      (`nvk_cmd_buffer.c`).
    - Result with both: `vkds` B2 0 wrong, B3 0 wrong, B4 depth
      unchanged, no kernel message, addresses at 0x3e_xxxxxxxx.
  - The kernel fix (linux-sandy rev 16, the linux-sandy kernel patch):
    `com.fuc` `cmd_exec` loads 0x600 instead of 0x200 into the
    control word after `cmd_exec_set_format` (Fermi build only), and
    `gf100.fuc3.h` word 0x0ef40200 -> 0x0ef40600 by hand (no falcon
    assembler on the build host; byte 0x4c9 of the code). Launches
    without FORMAT still lack the bit; NVK always sets FORMAT.
  - Open: method 0x304 does not wait for a running copy, so a draw
    right after a copy could race it; every other place that sends
    90b5-class methods on Fermi (list below) has the same stray-copy
    hazard as the barrier had.
  - W520 crashes: twice on 2026-11-13 the machine went down some time
    after register writes from the host (`mmio.py w`); the second
    time the only write was 0 to 0x104604/0x104608 before a faulting
    copy. Cause not established. Since then nothing is written from
    the host; the module sets what needs setting.
- Still using the copy class: events (`nvk_event.c`), query copies
  (`nvk_query_pool.c`), and a few sites in `nvk_cmd_buffer.c` /
  `nvk_cmd_draw.c`. Depth/stencil aspect copies need component
  remapping, which M2MF does not have.
- Target change to 1.2: beyond 1.0 that needs, at least, the 1.1 core
  (multiview, variable pointers storage buffer, protected memory may be
  absent, sampler YCbCr may be absent, 16-bit storage optional) and the
  1.2 core (imageless framebuffer, separate depth/stencil layouts,
  timeline semaphores, host query reset, uniform buffer standard
  layout, subgroup extended types, 8-bit storage optional, descriptor
  indexing optional). NVK already does all of these for Kepler in
  software or with features Fermi shares; multiview is the one to
  check on hardware. To be listed properly once 1.0 rendering works.

- Converting the rest off the 90b5 class (2026-11-13) **[w520]**:
  - Events and timestamps: when the last subchannel is not 3D or
    compute, Fermi uses the 3D engine's report semaphore
    (`nvk_event.c`, `nvk_query_pool.c`). Events tested (C4).
  - Buffer copies always go to `nvk_cmd_m2mf_copy_linear` on Fermi
    (also on queues with the compute bit, where NVK would use a
    compute shader); `vkCmdUpdateBuffer` stages the data and copies
    it with M2MF (the Fermi 3D class has no inline-to-memory);
    fills use the copy engine in the firmware's layout with FORMAT
    nibble 4 = constant 0, four-byte component, no source component
    (`nvk_cmd_fill_memory_ce`). All tested (C1-C3).
  - `INVALIDATE_SHADER_CACHES` (0x021c) and `_NO_WFI` (0x0da4) are not
    methods of the 9097 class (the kernel logged `ILLEGAL_MTHD 0da4`):
    the first becomes `WAIT_FOR_IDLE`, the second is dropped
    (`nvk_cmd_buffer.c`, `nvk_cmd_draw.c`). Whether Fermi needs some
    other cache flush after writing a constant buffer is open.
- Compute on Fermi (2026-11-13) **[w520]**:
  - Engine setup once per queue: processor count (method 0x0758 =
    `tpc_count`), call depth 0x0d64 = 0xf, 0x02a0 = 0x8000, the
    global-to-virtual address map (0x02c4/0x02c8, 256 identity
    entries with read and write), cache split 48K, shared size 0.
    The code address, local/shared windows and the header/sampler
    pools use the same offsets as on Kepler and were already sent.
  - Per dispatch: root table uploaded; each constant buffer by
    0x2380 (size, address) + 0x1694 (bind), then 0x1698 = 0x1000;
    texture/sampler units by `BIND_TEXTURE_HEADER/SAMPLER`; 0x1698
    = 1 (code), program start = offset in the shader heap, local
    memory sizes, shared size, thread count, barrier count, register
    count, 0x0780 = 1, launch id 0, 0x1698 = 0x110, block and grid
    dimensions, `BEGIN_GRID`, 0x0a08, `LAUNCH` 0x1000, 0x0a04,
    `WAIT_REF_COUNT`.
  - What made it run: the engine needs a local-memory area
    (0x0790..0x07a0) even when the shader uses none, because each
    launch declares an 0x800-byte call stack. Without it the launch
    is accepted, nothing runs and nothing is logged.
    `nvk_cmd_dispatch_fermi` asks for at least 0x800 bytes of call
    stack.
  - Constant-buffer and texture bindings of the compute engine alias
    the 3D engine's (GL driver comments), so after a dispatch all 3D
    constant-buffer groups are marked dirty; texture units are
    rebound on every draw anyway.
  - The macro memory is one for the 3D and the compute class. A
    first version loaded the launch macro through the compute
    subchannel at slot 0 and so overwrote the first 3D macro: the
    offscreen tests still passed, `vkcube` drew nothing and the kernel
    logged a shader read at address 0 (`gr: TRAP`, `fault ... [READ]
    at 0000000000000000 ... GPC0`). The macro is now an ordinary
    entry of the `nvk_mme` table, loaded with the others and called
    with `CALL_MME_MACRO` on the compute subchannel. Lesson: run
    `vkcube` after every change to queue setup.
  - `maxComputeWorkGroupCount` x is 65535 on Fermi (the launch
    methods take 16 bits for x and for y).
  - 0x1944 (render-enable override) is sent around internal
    dispatches only, as on Kepler; it is accepted, its effect on
    Fermi is not verified.
  - Not done: textures and storage images in compute shaders are
    untested / not started; the compute-based meta paths (image
    copies, query copies) are still switched off on Fermi.
  - `tools/shot.sh` cannot be trusted while a full-screen program
    runs on the W520: the window capture comes back black and its
    kernel-line filter printed nothing for a run that faulted. Check
    the kernel log after a marker line instead (as `ceport.sh` does).

- Wait-for-copy (2026-11-13) **[w520]**: nouveau's copy firmware has no
  method that waits, but every launch starts by waiting for the copy
  before it. The barrier on the copy subchannel therefore launches a
  four-byte copy inside a scratch allocation (FORMAT 0, both linear)
  and then sends 0x304. Needs no firmware change. Copy tests still
  pass; a test that would fail without the wait does not exist.
- Storage images on Fermi (2026-11-13) **[w520]**:
  - Fermi has neither Kepler's surface instructions (`suclamp`,
    `subfm`, `sueau`, `suldga`, `sustga`: NAK's SM20 encoder asserts)
    nor are the GL driver's bound image slots (`suldb`/`sustp`, eight
    per stage) needed: plain global loads and stores work, as the
    storage-buffer test showed.
  - `nak_nir_lower_image_addrs.c` has an SM20 variant
    (`lower_image_access_sm20`): bounds from the descriptor's clamp
    fields, the byte offset computed with integer operations for
    pitch-linear and block-linear layouts, a 64-bit address, then
    `load_global`/`store_global` of 8, 16 or 32-bit words with
    `nir_format_pack_rgba`/`unpack_rgba` for the format. The
    descriptor is the Kepler one (`nil_su_info`), unchanged.
  - Block-linear byte order inside a 512-byte GOB, measured on the
    GF106 against what M2MF reads back: bit 8 = x5, bit 7 = y2,
    bit 6 = x4, bits 5..4 = y1..y0, bits 3..0 = x3..x0 (x in bytes).
    The order with y2,y1 at bits 7..6 and x4 at bit 5 put half the
    texels in the wrong place.
  - Stores need the format at compile time, so
    `shaderStorageImageWriteWithoutFormat` is off on Fermi.
- NVK's compute-based image copies stay off on Fermi
  (`nvk_meta_image_copy_compute_supported`), state of 2026-11-14
  **[open]** **[w520]**:
  - One cause found and fixed: a shader uploaded through the upload
    queue (M2MF on its own channel) could be launched before its code
    was in place. The queue's time point was signalled when the M2MF
    methods had been taken, not when the copy was done. Seen as a copy
    that did nothing the first time a copy pipeline was created in the
    middle of a command buffer (`vkimg` I5 after I1-I4: wrong twice,
    right once, always right with the three copies in separate
    command buffers). `nvk_upload_queue_m2mf_copy` now ends with
    `WAIT_FOR_IDLE`; after it `vkimg` passed 12 runs out of 12. This
    fix stays in whether or not the compute copies are on.
  - Still open: with the compute copies on, `vkcube` lost its channel
    in 3 starts out of 9 (`fault 00 [READ] at 0000000000000000 ...
    GPC0`), fine in the others. Not reproduced by `vkimg` I4 (linear
    image to ordinary image) or I5 (100x60 BGRA to buffer and to a
    linear image). Next thing to look at: what vkcube's first frames
    do that the tests do not (presentation path, copies between
    draws), and whether another upload path lacks the wait.
  - M2MF and the copy engine do these copies meanwhile; with them
    `vkcube` ran 45 s clean and the picture was judged correct.

- Texture operands on Fermi (2026-11-13) **[w520]**: the first operand
  of a texture instruction holds the handle or array index and the
  coordinates and nothing else; sample index, LOD or bias, offsets and
  the depth reference follow each other in the second operand (GL
  compiler: `texConstraintNVC0`, `handleTEX`, "the sample id has to be
  in the second operand"). The port had packed them Kepler-style, four
  to an operand, which was only right for lookups with coordinates
  alone. Seen as: a 4x resolve that took sample 0 of every pixel
  (`vkgfx2` H2: no half-covered edge pixels). `nak_nir_lower_tex.c`
  now has a Fermi branch. After it: 64 half-red edge pixels, and
  `texelFetch` per sample shows 2 of 4 samples covered. Explicit LOD,
  bias, shadow compare and offsets go through the same branch and are
  not tested individually yet.
- The constant-buffer selector is shared between the 3D and compute
  classes like the bindings are: after a dispatch the next draw's
  macro wrote its parameters into the compute shader's last buffer
  and the draw came out empty (`vkgfx2` H3, first draw after a
  dispatch). `nvk_cmd_dispatch_fermi` ends with
  `NVK_MME_SELECT_CB0`.
- Constant buffers are shared between the 3D and the compute class
  in ways the GL driver's comments only hint at ("aliased"). What was
  measured on the GF106, all with `tests/vkgfx2.c` (2026-11-13/14)
  **[w520]**. None of this is multiview-specific; multiview only made
  it visible because it writes the root table between draws:
  1. After draws, the first constant buffer bound on the compute
     class is read wrongly once: the shader saw garbage in the first
     24 bytes of its root table (`gl_WorkGroupID`, `gl_NumWorkGroups`;
     `VKGFX2_IDS=1` prints them), wrote through a wild address and
     the channel died (`fault ... [WRITE] at 3f_...`). The dispatch
     after it was fine. Fix: `nvk_cmd_dispatch_fermi` first binds and
     flushes a throwaway buffer on compute slot 0.
  2. That throwaway buffer must be as large as the 3D root table:
     afterwards the 3D shaders read zeros from their root table
     beyond the size given in that first compute bind (a blit after a
     dispatch came out black with a 0x100-byte one).
  3. Once the compute class has selected a constant buffer (the
     selector write alone is enough), a draw that is not followed by a
     write to the 3D root table is shaded with the compute side's
     first buffer in place of the root table's first words: the last
     draw of every render pass drew with first instance 0 and view
     index 0 (`VKGFX2_MVX=5 VKGFX2_SEQ=123`: the third quad landed
     on quad 0; marking the throwaway buffer moved it). Fix: on Fermi
     every draw macro ends with a write to the root table's unused
     word (`nvk_mme_fermi_after_draw`).
  4. The 3D root table is rebound to slot 0 of all five shader
     groups and reselected after every dispatch (needed for the first
     draw after a dispatch).
  Tried and without effect: waiting for idle on either class, the 3D
  memory barrier (0x021c), unbinding before binding, binding twice,
  selecting through the 3D class for compute (compute has its own
  selector), restoring the selector with explicit writes.
- `INVALIDATE_SHADER_CACHES` (0x021c) does exist on the Fermi 3D
  class (the GL driver calls it MEM_BARRIER); only `_NO_WFI` (0x0da4)
  does not. The barrier uses 0x021c again.
- Test code that only helped find these was removed again from the
  driver (`TMPDBG` print, `NAK_NOSCHED`, the bisect switches).

### 2026-11-24: rebase, WebGPU target, run-time indexed descriptor arrays **[w520]**

- Rebased onto Mesa 26.2.7 (no upstream change under `src/nouveau`);
  the whole offscreen suite passes on the W520 (kernel 7.3.1_24,
  coreboot).
- The target: WebGPU in Helium/Chromium through Dawn, so an honest
  Vulkan 1.1. Read from the Helium 0.21.3.1 tree
  (`third_party/dawn/src/dawn/native/vulkan`): `BackendVk.h:53`
  requires instance and device 1.1; `PhysicalDeviceVk.cpp:240-271`
  requires robustBufferAccess, textureCompressionBC (or ETC2 + ASTC),
  the four `*ArrayDynamicIndexing` features, fragmentStoresAndAtomics,
  fullDrawIndexUint32; depthBiasClamp, imageCubeArray, independentBlend
  and sampleRateShading only decide core versus compatibility mode;
  the WebGPU minimum limits are checked from line 738. Chromium on
  Linux needs `--enable-unsafe-webgpu --enable-features=Vulkan`
  (`gpu_finch_features.cc:284`). Firefox/LibreWolf 160 (wgpu-hal)
  hides adapters whose conformance version major is 0
  (`wgpu-hal/src/vulkan/adapter.rs:2399`) and has `dom.webgpu.enabled`
  off on Linux; not pursued for now.
- Run-time indexed descriptor arrays implemented (section 9) and
  tested with `tests/vkdyn.c` in compute and fragment shaders. First run:
  only units 0..3 worked; every texture or sampler unit from 4 on read
  as unbound (all zeros). Cause: NVK never set
  `SET_BINDING_CONTROL_TEXTURE` on Fermi (compute 0x0210, 3D 0x2200 +
  16 per stage), and with the default the hardware reaches only the
  first four units. Both are now set to 16 samplers / 32 headers (the
  GL driver's `TEX_LIMITS` 0x54) in `nvk_push_dispatch_state_init` and
  `nvk_push_draw_state_init`. Constant-index shaders with more than
  four units were broken by the same thing before.

### 2026-11-24: Dawn's required features tested (`tests/vkdawn.c`) **[w520]**

Every case runs on NVK and lavapipe in one process
(`VK_ICD_FILENAMES=<nvk>:<lvp>`). Result on the GF106, with the fix
described below in place:

| case | NVK |
| --- | --- |
| Z2 storage reads/writes past a 16-word binding, vertex fetch past the buffer | correct (zeros, nothing outside written) |
| Z2 uniform reads past the binding | correct: the bytes after the binding in the same buffer come back, which `robustBufferAccess` allows (the binding is rounded up to 256 bytes) |
| Z3 cube array, 12 faces | correct |
| Z4 `gl_SampleID`; `minSampleShading` 1.0 = 256 invocations at 4x | correct |
| Z5 independent blend (ADD, MIN) | correct |
| Z6 depth bias clamp | correct (was wrong after any dispatch until the fix below: it reads its depth from a push constant) |
| Z7 indices above 2^24 | correct |
| Z8 limits | none below WebGPU's minimum, but the per-stage descriptor counts are Kepler's (1048576); Fermi binds 16 samplers and 32 textures per stage |
| Z9 push constants in vertex and fragment shaders | correct (were all zero after any dispatch until the fix below) |
| Z1 BC1-BC5 against lavapipe | differ by up to 4/255 (4.6/127 snorm) in a few texels; BC6H and BC7 match. Not judged: needs a reference decoder (decoding is fixed-function, so only the format setup can be wrong) |

**Push constants and the rest of the 3D root table after a dispatch.**
Narrowed down with Z0 (one trivial dispatch) + Z9, a vertex shader that
reads its push constants both through the constant buffer and by address
(`GL_EXT_buffer_reference` on the root table's address):
- The root table's memory holds the right values; the constant buffer
  read returns the compute class's first bound buffer instead (proved by
  filling the compute throwaway buffer with a pattern: the 3D shader read
  the pattern at the push constant offset). Draw parameters are not
  affected visibly because the 3D hardware supplies them.
- Nothing done on the 3D side restores it: rebinding slot 0 of all groups
  (macro or explicit selector), selecting another buffer first,
  `INVALIDATE_SHADER_CACHES` 0x1011 before the draw, a second draw.
  Same with the dispatch in the same command buffer.
- Moving the throwaway to slot 15, making it the 3D root table itself, or
  binding the root table on compute after the launch: 3D still reads a
  compute buffer (the first one bound whose address differs from 3D's).
- Binding the 3D root table at a **new address** before the draw fixes
  it. Doing that with a new upload buffer per command buffer and per
  dispatch makes compute shaders stop writing after a few dozen binds
  (`Z1` from the 5th or 8th format on, no kernel message). Alternating
  between two fixed halves of the queue's CB0, switched by an MME macro at
  the first draw after a dispatch (root writes going to both halves), fixed
  Z9 but a later compute dispatch (after the switch back to the first
  half) wrote nothing and the next submit returned DEVICE_LOST, again with
  no kernel message.
- None of those went in. What settled it: a dispatch that issues no
  constant buffer method on the compute class at all (no select, no load,
  no bind) leaves the 3D class untouched (`noop.comp` with the binds
  skipped, then Z9: correct). Any one of the three is enough to break it;
  moving only the binds away was not.

**Fix: Fermi compute shaders use no constant buffers.**
- `nvk_lower_nir` (`nvk_is_fermi_cs`): no large constants, the constant
  buffer map gets the root table only (`indirect_bind` in
  `nvk_nir_lower_descriptors`), uniform buffers and descriptor sets are
  read through global loads as on the `NVK_DEBUG=nocbuf` path, and
  `lower_fermi_cs_root_load` turns the remaining root table loads into
  global loads from a fixed address; the map ends up empty (the texture
  and sampler units stay in it).
- `nvk_device::fermi_cs_root`: 4 KiB of VRAM, the compute root table's
  one place. `nvk_cmd_dispatch_fermi` writes the table there before
  every launch **through the 3D class** (selector + `LOAD_CONSTANT_BUFFER`,
  as the indirect dispatch macro already wrote its counts), binds
  nothing, and no longer needs the throwaway buffer.
- The 3D class runs with `MODE_METHOD_TRACK_WITH_FILTER`, which drops a
  method that repeats its shadowed value. Texture and sampler unit binds
  are shared between the classes as well, so the first draw of a command
  buffer and the first draw after a dispatch bind constant buffers and
  units with the filter off (`nvk_graphics_state::fermi_rebind`,
  `nvk_cmd_flush_gfx_state`); the selector is pointed back at the 3D root
  table the same way after a dispatch. (The filter alone was not the
  cause of the root table problem: unfiltered rebinds did not help while
  compute still touched its constant buffers.)
- Result: `vkdawn` 0 checks failed on NVK in the full sequence and with
  a dispatch in the same command buffer; `vkdyn` and the nine older tests
  pass, including a uniform buffer in compute (`vkcomp` D2) and indirect
  dispatch.
- Z2 was too strict about uniform reads: under `robustBufferAccess` a
  read past the binding may return any value from the memory bound to the
  buffer. NVK rounds a uniform binding up to 256 bytes (upstream, all
  chips before Turing), so such a read returns the buffer's next bytes.
  The test now accepts that and reports it separately.

### 2026-11-24: Dawn in Helium on the GF106 **[w520]**

Helium 0.22 (Chromium 158) on the W520, headless (no window), the test
driver through `VK_ICD_FILENAMES`, a separate profile in `./nvk-test/prof`:

```
VK_ICD_FILENAMES=./nvk-test/nvk_icd.json \
NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
timeout 45 /usr/lib/helium/helium --headless=new \
  --user-data-dir=./nvk-test/prof --no-first-run \
  --enable-unsafe-webgpu --enable-features=Vulkan \
  --use-vulkan=native --ignore-gpu-blocklist \
  --use-gl=angle --use-angle=gl-egl \
  --enable-logging=stderr --log-level=0 \
  file://./nvk-test/work.html 2> err.txt
grep -o 'WGPU [^"]*' err.txt
```

- Without `--use-angle=...` headless Chromium renders in software and
  Dawn only offers SwiftShader. On Linux Dawn gets the Vulkan backend
  only when the compositor is on Vulkan or WebGPU-over-GL interop is on
  (`webgpu_decoder_impl.cc:1758`), and the adapter must support external
  images (`:1830`; our driver lists the external memory/semaphore/fence
  fd, dma-buf and DRM format modifier extensions, untested on Fermi).
- `tests/webgpu/probe.html`: `requestAdapter()` returns vendor `nvidia`,
  architecture `fermi`, for core and for compatibility.
- `tests/webgpu/work.html` (results checked in the page, printed with
  `console.log`): W1 compute 0 of 1024 words wrong; W2 a triangle in a
  colour from a uniform buffer, 2016 + 2080 pixels as expected; W3 a
  textured draw, 1024 pixels of each of the four texels. No Dawn error,
  no kernel message.
- In a window on the W520's screen (`tools/helium-canvas.sh canvas.html`,
  flags `--enable-unsafe-webgpu --enable-features=Vulkan
  --ignore-gpu-blocklist`, compositor on Vulkan, GL through nouveau):
  `tests/webgpu/canvas.html` presents a rotating triangle to a canvas at
  60 frames per second, 1080 frames in 18 s, correct picture
  (`probe/helium-webgpu-canvas.png`), browser UI correct, no kernel
  message.
- That needed one change. nouveau's GL driver has no memory objects or
  semaphores, so Chromium takes its shared images as not external
  (`shared_context_state.cc:852`) and creates them without external
  memory (`external_vk_image_backing.cc:241`), but still exports their
  memory by fd for Dawn (`:801`). NVK allocates unshared memory as
  kernel-private and cannot export it (the debug build asserts in
  `nvkmd_mem_export_dma_buf`). On Fermi, memory dedicated to an image is
  now always shareable (`nvk_AllocateMemory`).
- vkcube after all of the above: correct (`probe/vkcube-2627.png`).
- A shipped WebGPU game (nfsu2.xyz) in the test window on
  2026-11-24: it played correctly, at 34 to 48 frames per second by the
  page's own report (debug build of the driver); no Dawn error, no kernel
  message.

### 2026-11-24: Vulkan 1.1 feature list, limits, package **[w520]**

- `nvk_get_vk_version`: 1.1 on Fermi. `nvk_fermi_limit_extensions` and
  `nvk_fermi_limit_features` (`nvk_physical_device.c`) cut the extension
  and feature tables down to an explicit list: what the tests or
  Chromium's two devices (Dawn, Skia) have run on the GF106, plus render
  pass, pipeline, state, window system and external handle plumbing that
  does not depend on the chip. Out: descriptor indexing, robustness2,
  pipeline robustness, 8/16/64-bit shader types, subgroups, variable
  pointers, device addresses, dynamic state 3, maintenance 5 and later,
  multisampled storage images, precise occlusion queries.
- Limits: 16 samplers, 32 sampled images, 8 input attachments per stage
  (the bound units); buffers and storage images are by address and keep
  their limits. Known hole: a stage with all 16 samplers in use has no
  unit left for the sampler `texelFetch` needs.
- `samplerYcbcrConversion` stays: Chromium's Vulkan compositor does not
  start without it (`vulkan_device_queue.cc:293`). An H.264 clip through
  the compositor shows correct colours, 0 dropped frames
  (`tests/webgpu/video.html`, `probe/helium-video.png`); the feature has
  no test of its own.
- The GF106 is accepted without `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER`; the
  bring-up print `NVK_FERMI_DEBUG` is gone.
- Dawn with this list: still `core-features-and-limits`; `subgroups`,
  `shader-f16` and the experimental resource table are no longer offered.
- Checked with the trimmed list, debug and release builds of the driver:
  `vkdawn` 0 failed, `vkdyn`, the nine older tests, the headless WebGPU
  page, the canvas at 60 fps without `--ignore-gpu-blocklist`.
- Package: `srcpkgs/mesa/patches/nvk-1-fermi-gf106.patch` (this diff with
  a header), mesa 26.2.7_3; `w520-modifications` 1_7 depends on
  `mesa-vulkan-nouveau` and its Helium flags turn on the Vulkan compositor
  and WebGPU. The patch set applied to pristine 26.2.7 equals the tested
  tree. Not built as packages here (the packaged builds); the release
  configuration was compiled with `.build-vk-rel.sh` (`build-vk-rel`) and
  tested from the release test directory on the W520.
- Open: BC1-BC5 reference decoder; tests for YCbCr conversion, occlusion
  queries, mipmaps, blending; texture unit dirty tracking (every draw
  rebinds its units); Dawn's own test suites.

## 8. Build and test loop

- Build environment: the x86_64 build chroot (under
  an xbps-src tree), with these added from
  `hostdir/binpkgs/sandy` by `xbps-install -r`: `rust`, `rust-std`,
  `rust-bindgen`, `cbindgen`, `llvm22`, `llvm22-devel`, `clang22`,
  `clang22-devel`, `mesa-libclc`, `SPIRV-LLVM-Translator22-devel`,
  `SPIRV-Tools-devel`, `elfutils-devel`. The crate tarballs from
  `hostdir/sources/mesa-26.2.7` go into
  `builddir/mesa-dbg/subprojects/packagecache`.
- `tools/build-vk-inside.sh` is `builddir/mesa-dbg/.build-vk.sh`: a
  Vulkan-only meson build in `build-vk` (debugoptimized, assertions on),
  LLVM 22 through `tools/native-vk.ini`, and
  `BINDGEN_EXTRA_CLANG_ARGS=-resource-dir .../llvm/23/...` because
  bindgen loads libclang 23 and otherwise reads clang 21's headers.
- `tools/buildvk.sh`: sync the working tree
  (the Mesa 26.2.7 build tree/src`) into the 64-bit tree, build, strip, and copy
  `libvulkan_nouveau.so` + an ICD file to the W520's the test directory.
- On the W520: `tools/probe.sh` (vulkaninfo with an ioctl trace),
  `tools/cube.sh` (vkcube and the kernel log lines it caused). Both use
  `VK_ICD_FILENAMES=./nvk-test/nvk_icd.json` and
  `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`; nothing is installed system-wide.
- `patches/nvk-fermi.diff` is regenerated as
  `diff -ru <pristine>/src/nouveau <working>/src/nouveau` (through the
  links `the Mesa source tree` and
  `/tmp/nvkd/b` = the working tree, `diff -ru -x __pycache__`; recreate the links after a reboot
  of the build host).
- `tools/t.sh <program> [args]` (W520): runs a test from the test directory
  against the test driver and prints the kernel log lines it caused.
  It waits a second before reading the log: faults are logged after
  the process is gone, and an earlier version missed them, which made
  the copy engine look like it failed silently.
- `tools/shot.sh` + `tools/xwd2png.py`: run vkcube, dump its window
  with `xwd`, convert to PNG on the build host.
- Tests: `tests/vkclear.c` (clear, triangle; needs `tri.vert.spv`,
  `tri.frag.spv` next to it, built with `glslangValidator -V`),
  `tests/vkds.c` (upload round trip, depth/stencil), `tests/vkbuf.c`
  (fill, update, copy, events), `tests/vkcomp.c` + `fill.comp`
  (compute dispatch; `glslangValidator -V fill.comp -o fill.comp.spv`),
  `tests/vkimg.c` + `tex.comp`, `store.comp`, `load.comp` (images in
  compute, linear images, odd-sized BGRA copies; `VKIMG_MAP=1` prints
  where texels landed, `VKIMG_ONLY5`, `VKIMG_SPLIT`, `VKIMG_NOIMG`
  vary the last test), `tests/vkmisc.c`
  (queries, conditional rendering), `tests/vksi.c` + `gen_si.py`
  (storage image formats; shaders generated into `tests/si`),
  `tests/vkdawn.c` + `tests/dawn/*` (Dawn's required features, NVK
  against lavapipe in one run; switches in the file header),
  `tests/vkdyn.c` + `dyn.comp`, `dyn.vert`, `dyn.frag` (descriptor
  arrays indexed at run time; `VKDYN_FRAG=1` runs the fragment variant;
  checked against lavapipe on the W520),
  `tests/vkgfx.c` and `tests/vkgfx2.c` + `tests/gfx/*` (graphics
  stages, multiview, multisampling, blits, transform feedback, image
  atomics, texel buffers; `MESA_SHADER_CACHE_DISABLE=true` when a
  compiler change is being tested, the cache hides it otherwise);
  build with
  `gcc -O1 -o bin/<name> <name>.c -lvulkan` on the build host.
- Driver debug switches: `NVK_FERMI_DEBUG=1` (classes, copy engine
  addresses), `NAK_DEBUG=print` (compiler output per shader),
  `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` (required: not a listed chip).
- Copy-engine experiments: `tools/ceport.sh`, `tools/fuc2bin.py`,
  `tools/mmio.py` (read only), image variants in `fw/`.
- Kernel side: module swap with the kernel tests/reload.sh
  "<config>" ["<debug>"]` on the W520 as root (stops X); modules for
  the running kernel are built with `./build-ko.sh <kver>`
  in the kernel working tree (tree `b/`) and kept in its `out/`.

## 9. Concessions so far (things Fermi gets that Kepler does not need)

- Texture and sampler references are units assigned at compile time, at
  most 32 textures and 16 samplers per stage. A descriptor array indexed
  at run time gets all its elements as consecutive units
  (`nvk_nir_lower_descriptors.c` `tex_unit_def`); the shader computes
  first unit + index (clamped to the array) and the instruction takes it
  from its first operand: `0xttxsaaaa`, texture unit at bits 23..31,
  sampler unit at 16..22, array index below (GL compiler `handleTEX`,
  `handleTXQ`). On SM20 that is the encoding bit 50 that means bindless
  on Kepler, with the unit fields zero (`nak/sm20.rs`; NAK lowers a
  computed handle to `TexRef::Bindless`, `nak_nir_lower_tex.c`
  `fermi_indirect_units`). Dawn requires this feature, so it is no
  longer optional for us.
- Plain copies go through M2MF. Component rearranging copies use the
  copy engine through nouveau's firmware layout, which needs the fixed
  image of linux-sandy rev 16: on a stock kernel they fault.
- Image copies never take the compute-shader path
  (`nvk_meta_image_copy_compute_supported` is false on Fermi).
- No sparse memory (GM20x page tables), no launch-descriptor compute.
