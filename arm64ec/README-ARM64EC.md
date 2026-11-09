# OptiScaler — ARM64EC build

This is the **ARM64EC** build of OptiScaler. Use it with an **x64/x86_64 game**
running on:

- **Linux ARM64 with Proton/Wine ARM64EC** (Proton 11 ARM64 or a Wine build with
  ARM64EC support, e.g. on Snapdragon X Elite/Adreno hardware), or
- **Windows 11 on ARM** (x64 game under emulation, ARM64EC system DLLs), or
- **Winlator on Android** with an ARM64EC Wine build (see below).

An x64 or ARM64EC process can load both x64 and ARM64EC modules. The OptiScaler
proxy is ARM64EC so its Detours hooks can operate on the ARM64EC system
`dxgi.dll`/`d3d12.dll`; bundling the x64 proxy on such a setup causes
`STATUS_ILLEGAL_INSTRUCTION` crashes or refused hooks. The x64 files in the
`OptiScaler/` folder (XeSS, AMD FidelityFX, D3D12Core) are still x64 and run
under emulation; this is expected.

## Installation (Winlator on Android)

Tested on Adreno 740 with the Winlator cmod fork, `proton-11.0-1-arm64ec`,
FEXCore, Turnip, DXVK 2.3.1 and vkd3d-proton 3.0.1.

1. Use a container with an ARM64EC Wine build and DXVK + vkd3d as the wrapper.
2. Extract **all** files into the directory containing the real game executable.
3. Rename `OptiScaler.dll` to `dxgi.dll`.
4. Choose an upscaler in `OptiScaler.ini` with `Dx11Upscaler`, `Dx12Upscaler` or
   `VulkanUpscaler`. For Vulkan games also set `Vulkan=true` and
   `VulkanExtensionSpoofing=true`.
5. Start the game and turn on DLSS in its graphics settings. OptiScaler only runs
   when the game asks for DLSS.
6. Press `O` for the overlay, using a controls profile or on-screen keyboard that
   has an `O` key.

If OptiScaler doesn't load, add `WINEDLLOVERRIDES=dxgi=n,b` to the container's
environment variables.

## Installation (Proton on ARM64 Linux)

1. Use an ARM64 Proton build. The Steam client itself must be the ARM64 client;
   an x86_64 Steam running under FEX cannot use an ARM64 Proton build.
2. Extract **all** files into the directory containing the real game executable.
   For Unreal Engine games this is usually `<Game>/Binaries/Win64/`, not the game
   root and not `Engine/`.
3. Rename `OptiScaler.dll` to a proxy name. `dxgi.dll` is the recommended
   default. `winmm.dll` or `version.dll` are the best fallbacks, especially for
   Vulkan-only games.
4. Add this Steam launch option:

   ```
   WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```

   For a non-Steam game / manual prefix:

   ```
   WINEDLLOVERRIDES="dxgi=n,b" wine /path/to/game.exe
   ```

   Without the override, Wine loads its builtin `dxgi` and OptiScaler is never
   loaded.
5. Start the game and press `O` for the OptiScaler overlay. File logging is off
   by default; enable `[Log] LogToFile=true` in `OptiScaler.ini` if you need
   `OptiScaler.log` (it is written next to the proxy).

You can also use `install_proton.sh` from this directory:

```
bash install_proton.sh /path/to/Game/Binaries/Win64 dxgi.dll
```

It copies the runtime files and renames the proxy for you, then prints the
launch option.

## Verified / expected feature support

The ARM64EC port fixes the hooking layer. It does not add GPU capabilities that
the driver lacks:

- **FSR 2.2.1 and FSR 3.1.2** are compiled natively into the ARM64EC proxy and
  are the realistic outputs on Adreno/Mali class hardware.
- **SGSR 1 and SGSR 2** are compiled natively into the proxy for DX11, DX12 and
  Vulkan (`sgsr1`, `sgsr2`). SGSR 1 only uses the current frame, so expect more
  aliasing and shimmer than from SGSR 2 or FSR.
- **DLSS** and **FSR4** depend on vendor runtimes/GPUs. On Adreno, DLSS is
  generally not detected and FSR4 is unavailable.
- **XeSS** runs on DX12 on Adreno 740 with `Dx12Upscaler=xess`, but at about
  7 fps, so `auto` doesn't pick it there. XeSS on DX11 doesn't work: `xess` is
  Intel only, and `xess_12` needs D3D11 to D3D12 texture sharing, which fails
  with DXVK 2.3.1 and vkd3d-proton 3.0.1.
- **Frame generation** is not known to work on Adreno in public reports.
- The x64 vendor DLLs in `OptiScaler/` (XeSS, AMD FidelityFX, D3D12Core) load,
  but all their work runs under x64 emulation. Do not mix in pure ARM64 copies:
  an ARM64EC process cannot load pure ARM64 modules.
- x64 ASI plugins (including OptiPatcher) and x64 ReShade/SpecialK are not
  included. They can load into an ARM64EC process but their hooks are not
  guaranteed and are not part of a supported ARM64EC configuration.

## Troubleshooting

- **No `OptiScaler.log`**: file logging is disabled by default
  (`[Log] LogToFile=auto` resolves to false). Set `[Log] LogToFile=true` and
  `[Log] LogLevel=0`, then restart. The log is written next to the proxy.
- **No overlay**: check the proxy name, the game executable directory, and
  `WINEDLLOVERRIDES`. `WINEDEBUG=+loaddll PROTON_LOG=1` shows the DLL load.
- **`STATUS_ILLEGAL_INSTRUCTION` (0xC000001D)**: an x64 Detours hook hit ARM64EC
  code, or an old x64 OptiScaler proxy is being used. Use this ARM64EC build.
- **`ERROR_BAD_EXE_FORMAT` (0xC1)**: a pure ARM64 process is loading an ARM64EC
  module. This build is for x64 games (emulated) on ARM64 Windows/Proton.
- **`ERROR_INVALID_BLOCK` (9) from `DetourAttach`**: the target function is too
  small; try `DxgiFactoryWrapping=true` as a diagnostic workaround.
- **Overlay works but the upscaler does nothing**: confirm the game's actual
  input API and select an output backend that works on your GPU (FSR2/FSR3.1).
- **Game ships its own `ucrtbase.dll`, `vcruntime140.dll` or `msvcp140.dll`**:
  no action needed. The ARM64EC proxy links the MSVC runtime statically, so it
  does not load these x64 DLLs. Builds before 2026-11-09 used the dynamic
  runtime and hung or crashed in such games unless those DLLs were moved away.

## Proxy names

Pre-made ARM64EC copies of `dxgi.dll` and `winmm.dll` are in `proxy-names/`.
They are byte-identical to `OptiScaler.dll`; copy the one you need into the game
folder (do not keep more than one proxy copy active).

`OptiScaler.dll` can be renamed to any of the names OptiScaler exports:
`dxgi.dll`, `winmm.dll`, `version.dll`, `d3d12.dll`, `dbghelp.dll`,
`wininet.dll`, `winhttp.dll`, or used as `OptiScaler.asi` with an ASI loader.
Keep only **one** renamed copy in the game folder.

## Licensing

OptiScaler is licensed under GPL-3.0; see `Licenses/OptiScaler-GPL-3.0.txt`.
Source: https://github.com/optiscaler/OptiScaler

This ARM64EC build is an unofficial port of the upstream project. Do not use it
with online games; anti-cheat systems may flag it.
