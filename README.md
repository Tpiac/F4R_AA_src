# F4R Upscaling

Upscaling mod for Fallout 4:

- DLSS
- FSR 3.1.5
- XeSS

This is the combined development repo and builds F4R_Upscaling.dll, DLSS.dll, FSR3.dll, XeSS.dll

- Fallout 4 (OG/NG/AE)
- [F4SE](https://f4se.silverlock.org)
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327)

## Credits

- [fo4test](https://github.com/doodlum/fo4test/tree/upscaler)
- [Community Shaders](https://github.com/community-shaders/skyrim-community-shaders)
- [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline)
- [FidelityFX SDK](https://github.com/alandtse/FidelityFX-SDK-DX11)
- [XeSS SDK](https://github.com/intel/xess)
- [CommonLibF4](https://github.com/LucaDotGit/CommonLibF4)
- [Detours](https://github.com/microsoft/Detours)

## License

Released under [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html) WITH Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source).

## Building

Вependencies:

- A C++ 23 compiler (MSVC 2026)
- [CommonLibF4](https://github.com/LucaDotGit/CommonLibF4)
- [vcpkg](https://github.com/microsoft/vcpkg) (set `VCPKG_ROOT`)
- [spdlog](https://github.com/gabime/spdlog)
- [fmt](https://github.com/fmtlib/fmt)

Build (combined plugin):

$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build --config Release

DLSS standalone:

cmake -B build_dlss -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCOMMONLIB_PLUGIN_NAME=DLSS 
cmake --build build_dlss --config Release

FSR3 standalone:

cmake -B build_fsr3 -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCOMMONLIB_PLUGIN_NAME=FSR3
cmake --build build_fsr3 --config Release

XeSS standalone:

cmake -B build_xess -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCOMMONLIB_PLUGIN_NAME=XeSS
cmake --build build_xess --config Release