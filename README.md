# wignix-nouveau

Vulkan on Fermi, and the nvc0 work behind it.

The Quadro 2000M is a Fermi (GF106, NVC3) card. No vendor ships Vulkan
on Fermi — NVIDIA declined in 2016 — and Mesa's NVK driver targets Kepler
and later. This repository holds the port of NVK to Fermi that makes
Vulkan 1.1 run on the card, enough for **WebGPU through Dawn in
Chromium**: on the ThinkPad W520 a WebGPU canvas presents at 60 fps in
Helium and a WebGPU game plays correctly, with the GPU doing the
rendering through Vulkan.

It also holds the tests and screenshots used to check the port, and the
kernel-side copy-engine patch the Vulkan driver depends on.

## What runs

Measured on a ThinkPad W520 (Quadro 2000M, coreboot, kernel 7.3.1):

- `vulkaninfo`: `Quadro 2000M (NVK GF106)`, Vulkan 1.1.
- `vkcube` in an X window: correct, `probe/vkcube-2627.png`.
- WebGPU in Helium through Dawn: compute, render and texture pages
  correct; an on-screen canvas at 60 fps, `probe/helium-webgpu-canvas.png`.
- Offscreen suite (`tests/`): clear and read-back, triangle, texture
  upload, D24S8 depth/stencil, compute dispatch (direct and indirect),
  buffer fill/copy/update, timestamp queries, runtime-indexed descriptor
  arrays, storage images across 17 formats, storage buffers, atomics.

## Layout

| path | what |
| --- | --- |
| `NOTES.md` | the plan, the map of the code involved, and the running notes |
| `patches/nvk-fermi.diff` | the NVK port to Fermi, as one diff against Mesa 26.2.7 |
| `patches/kernel-ce-test.diff` | the copy-engine test image for the kernel side |
| `tests/` | the offscreen Vulkan tests and the WebGPU pages |
| `tools/` | build and run helpers, the MMIO reader, the copy-engine port |
| `probe/` | screenshots and `vulkaninfo` output from the W520 |
| `fw/` | the assembled copy-engine test programs |

The NVK port is also shipped as the `nvk-1-fermi-gf106.patch` of the
`mesa` package in `wignix-packages`, and as a commit in `wignix-mesa`.
It needs the copy-engine fix in `wignix-kernel` (linux-sandy 7.3.1_16 or
later): the engine's control bit that makes it honour address high bytes.

## What the port does

- Texture instructions name bound texture and sampler units (no handles
  before Kepler): units are assigned when a shader is built and bound at
  draw and dispatch time, and runtime-indexed arrays get consecutive
  units and Fermi's indirect form. `SET_BINDING_CONTROL_TEXTURE` allows
  16 samplers and 32 textures per stage.
- Copies and uploads go through M2MF; component-rearranging copies go
  through the copy engine in the firmware's method layout.
- Compute is set up and launched by methods. Compute shaders use no
  constant buffers (on Fermi any constant-buffer method on the compute
  class makes 3D shaders read the compute buffer as their root table);
  the compute root table has a fixed address, written through the 3D class
  and read by address.
- Storage buffers and images are accessed by address (SM20 image
  lowering).
- The 3D class's constant buffers and texture units are bound with the
  method filter off at the first draw of a command buffer and after a
  dispatch.
- Image-dedicated memory is always shareable, so Chromium can export its
  shared images' memory for Dawn.

## Related GL work

The `nvc0` gallium driver on the same card carries occlusion queries
(ZCULL) and a threaded context, and submits on demand instead of after
every draw. Those patches are in `wignix-mesa`.

## License

The tests and tools are under the same terms as Mesa where they build on
it. NVK and the nouveau driver are MIT; see the Mesa tree for notices.
