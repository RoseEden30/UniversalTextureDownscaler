# UniversalTextureDownscaler

Drops the largest mip levels of textures at creation time to cut VRAM usage,
without touching any files on disk. Same idea as
[TextureDownscaler](https://github.com/RoseEden30/TextureDownscaler) (the
Skyrim SKSE plugin this is named after), generalized to any D3D11, D3D12 or
Vulkan game, with no per-game or per-file rules.

Avoid it in online games with active anti-cheat. It hooks the graphics API
the same way some cheat tools do, and anti-cheat software generally flags
any unauthorized DLL regardless of what it actually does.

## How it works

- **D3D11 / D3D12**: a proxy DLL. Dropped next to a game's exe under the real
  API's name (`d3d11.dll` or `d3d12.dll`), it loads the real system DLL and
  patches the vtable methods that create, copy and transition textures. It
  shrinks the desc on creation, then remaps subresource indices and mip
  ranges everywhere else (copies, barriers, views, D3D12's Enhanced Barriers
  included).
- **Vulkan**: a real Vulkan layer, not a proxy. Many Vulkan games
  link directly against vulkan-1.dll exports a proxy can't safely forward.

All three share the same mip-skip math and candidate filter (`src/Common.h`
plus each backend's own rejection checks). Only plain sampled 2D textures
with more than one mip, a single array layer, no render-target/UAV/storage
usage, and no sharing/sparse flags are touched. Anything the API refuses at
the reduced size falls back to the original, full-size resource.

## Tested on

| Game | Engine | API |
| --- | --- | --- |
| Skyrim Special Edition | Creation Engine | D3D11 |
| Dark Souls III | FromSoftware proprietary | D3D11 |
| Persona 5 Royal | P-Studio proprietary | D3D11 |
| The Blood of Dawnwalker | Unreal Engine 5 | D3D12 |
| Beast of Reincarnation | Unreal Engine 5 | D3D12 |
| Indiana Jones and the Great Circle | id Tech 7 | Vulkan |

### Known limitations

- **Virtual-texturing tile pools** (common in UE5 titles): tiles are small
  and fixed-size by design, so `MaxSize` has nothing to reduce on them.
- **HITMAN 3 (Glacier engine)**: reduction runs cleanly but causes visible
  texture corruption. Likely Glacier's own cached texture metadata going
  stale, unrelated to what the resource reports. Out of scope.
- **Runtime mip generation on Vulkan**: an engine that uploads only the top
  mip and blits the rest of the chain from it loses its seed level when that
  top mip is the one dropped. D3D11 rejects such textures up front on
  `MISC_GENERATE_MIPS`, and the D3D12 equivalent needs a UAV or render-target
  flag that is already rejected, but Vulkan's blit path needs neither. The
  layer logs a warning when it sees one rather than excluding it, since no
  game tested has hit this.

## Building

MSVC + CMake (x64), plus the Vulkan SDK if you want the Vulkan layer target
(skipped with a warning, not an error, if the SDK isn't found).

```
cmake -B build -A x64
cmake --build build --config RelWithDebInfo
```

D3D11 and D3D12 build as one binary, `build/RelWithDebInfo/d3d12.dll`.
Deploy it under whichever name (`d3d11.dll` or `d3d12.dll`) the target game
needs. `.def` files are regenerated from whatever real `d3d11.dll`/
`d3d12.dll` is installed on the build machine (`scripts/generate_def.ps1`).
The Vulkan layer builds as `UniversalTextureDownscaler_Vulkan.dll` + `.json`.

`UniversalTextureDownscalerInstaller/` (.NET 10/WinForms) embeds those binaries and:

- detects a game's API from its exe's PE import table, including
  delay-loaded imports;
- deploys D3D11/D3D12 next to the game's exe, no admin needed;
- for Vulkan, puts the DLL and manifest in `%PROGRAMDATA%\UniversalTextureDownscaler\`
  and registers the layer under HKLM. The ini and log still sit
  next to the game's exe, and the layer does nothing in a Vulkan app that has
  no ini beside it, so only games you set up are touched;
- won't overwrite or delete a `d3d11.dll`/`d3d12.dll` that isn't its own
  (it checks the version resource), since other mods use those names too.

Build with `dotnet build -c Release` after the CMake build above, or
`dotnet publish -c Release` for a standalone single-file exe.

## Deploying by hand

- **D3D11/D3D12**: copy the build output to the game's folder, named
  `d3d11.dll` or `d3d12.dll` to match the game's API. Check first whether a
  file under that name already belongs to another mod.
- **Vulkan**: the layer is registered once and seen by every Vulkan app, but
  it only touches games that have an `UniversalTextureDownscaler.ini` next to
  their exe. Copy `UniversalTextureDownscaler_Vulkan.dll` and its `.json` to a
  folder you'll keep (not the game's), then either
  - add a `DWORD` named after the manifest's full path, set to `0`, under
    `HKEY_LOCAL_MACHINE\Software\Khronos\Vulkan\ImplicitLayers`; or
  - enable it for one launch: set `VK_LAYER_PATH` (the manifest's folder) and
    `VK_INSTANCE_LAYERS=VK_LAYER_universaltexturedownscaler`.

  Drop a copy of the ini in each Vulkan game you want reduced. A game without
  one is left alone.

## Configuration

`UniversalTextureDownscaler.ini`, next to the game's exe (for the Vulkan
layer, that's still each game's own folder even though the DLL is shared),
`[Settings]` section:

- `Enabled` (0/1, default 1)
- `MaxSize` (default 2048): textures larger than this get their top mips
  dropped
- `Verbose` (0/1, default 0): logs every reduced texture and every multi-mip
  candidate that got rejected, and why. Off by default so the log stays a
  few lines even in a long session; turn on to diagnose a specific game
- `FakeVramBudgetMB` (default 0, disabled): caps the VRAM budget the game
  itself sees, for testing how its streaming reacts to less VRAM than the
  card has. Doesn't change how much VRAM is actually available. Whether a
  game actually reacts depends on the engine: tested on Indiana Jones and
  the Great Circle with a 4096 MB cap, the game's own overlay showed the
  fake budget but kept using ~6900 MB anyway, so this technique doesn't
  prove reduced mips would let that particular game fit a smaller card

A `UniversalTextureDownscaler_<API>.log` appears next to the exe (same rule
as the ini above) once the game runs. Its periodic summary line's "MB saved"
is a running total since launch, not subtracting textures since destroyed,
shown alongside the actual current VRAM usage/budget. That figure is
queried live from DXGI in all three backends, the Vulkan layer included, so
it reports the whole adapter rather than one API's own allocations.

Setting `DISABLE_UNIVERSALTEXTUREDOWNSCALER=1` in a Vulkan game's environment
turns the layer off for that one launch without unregistering it.

## Credits

- [TextureDownscaler](https://github.com/RoseEden30/TextureDownscaler): the
  Skyrim SKSE plugin this is generalized from.
- [ReShade](https://github.com/crosire/reshade): its real source was studied
  for the Vulkan layer bootstrap, the Unreal Engine bootstrap-exe
  redirect (RCDATA resource #201), and the Vulkan registry registration
  approach.
