# OptiScaler on ARM64EC under Proton/Wine ARM64

Research summary, 2026-11-04.

## Key conclusions

1. **What "Proton with ARM64EC" means.** It is an ARM64 build of Proton/Wine
   whose host-side modules (`dxgi.dll`, `d3d12.dll`, …) are ARM64EC or ARM64X,
   with FEX emulating only the x86-64 game code. It is not x86_64 Proton running
   under Box64/FEX. Proton 11 supports this with
   `configure.sh --target-arch=arm64`; the resulting build cannot be used in x86
   Steam under FEX.

2. **An x64 game process can load an ARM64EC DLL.** Microsoft's ARM64EC
   interoperability matrix allows an x64 or ARM64EC process to load both x64 and
   ARM64EC binaries; only a pure ARM64 process is restricted to ARM64. Proton 11
   ARM64 relies on this: its `dxgi.dll`/`d3d12.dll` are ARM64EC while games are
   x64.

3. **Only the OptiScaler proxy must be ARM64EC.** `OptiScaler.ini` is data and
   sits next to the renamed proxy. The `OptiScaler/` folder may contain x64
   XeSS, FidelityFX, fakenvapi, Nukem, Streamline and D3D12Core DLLs; they load
   and run under emulation. Pure ARM64 copies cannot load. The static Detours,
   FreeType and FSR libraries linked into the proxy must be ARM64EC; the current
   build uses native ARM64EC builds of all of them.

4. **Installation on Proton.** Put the files next to the real game executable
   (UE: `Binaries/Win64`), rename `OptiScaler.dll` to `dxgi.dll`, and launch with
   `WINEDLLOVERRIDES="dxgi=n,b" %command%`. Without the override Wine loads its
   builtin dxgi and OptiScaler never loads. `winmm.dll` and `version.dll` are the
   best fallbacks; `d3d12.dll` overlaps vkd3d-proton and is riskier.

5. **Expected support.** Public reports (OptiScaler issue #1074, Sloptiscaler,
   Armada OS) show ARM64EC OptiScaler working with an ARM64EC-capable Detours,
   using XeSS inputs and FSR3 output. DLSS/NGX has not been made to work on
   Snapdragon/Adreno; FSR4 is unavailable there; XeSS and FSR2.1/2.2 can be
   unreliable. The ARM64EC build fixes the hooking layer, not GPU feature
   availability.

## Recommended packaging

- Ship a separate ARM64EC archive: the ARM64EC proxy, `OptiScaler.ini`, the
  installer scripts, `Licenses/` (including GPL-3.0), and an `OptiScaler/`
  folder with the tested x64 vendor DLLs.
- Keep the files next to the game executable and preserve case (`OptiScaler`,
  `D3D12_OptiScaler`).
- Do not bundle x64 ASI/OptiPatcher/ReShade by default.
- Do not include `.lib`, PDB or object files.

## Detours note

The proxy links a current Detours master. Detours' ARM64EC support detects
ARM64EC code with `RtlIsEcCode`, copies ARM64 instructions for trampolines, and
allocates EC-code trampolines with `VirtualAlloc2(MEM_EXTENDED_PARAMETER_EC_CODE)`.
It supports ARM64EC targets when the detour function is also ARM64EC (the whole
OptiScaler proxy is). Older Detours builds caused `STATUS_ILLEGAL_INSTRUCTION`
when hooking ARM64EC modules.

## Sources

- https://learn.microsoft.com/en-us/windows/arm/arm64ec
- https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi
- https://list.winehq.org/hyperkitty/list/wine-releases@list.winehq.org/thread/EBI7GVJAF5SWZAHTRXPLVVJ7TUQWVA2Q/
- https://github.com/ValveSoftware/Proton/blob/proton_11.0/README.md
- https://github.com/ValveSoftware/Proton/issues/10173
- https://github.com/optiscaler/OptiScaler/issues/1074
- https://github.com/microsoft/Detours
- https://github.com/justradical/Sloptiscaler/releases/tag/finally
- https://github.com/armada-os/armada/releases/tag/20261010
- https://wiki.fex-emu.com/index.php?title=Development:ARM64EC

## Building from source

The ARM64EC build needs one change inside the `external/unordered_dense`
submodule, which is not recorded by this repository. Apply it after cloning:

```
git submodule update --init --recursive
git -C external/unordered_dense apply ../../arm64ec/patches/unordered_dense-arm64ec.patch
```

Then build on Windows with Visual Studio 2022 and the ARM64EC tools:

```
powershell -NoProfile -ExecutionPolicy Bypass -File arm64ec\build-optiscaler.ps1
```
