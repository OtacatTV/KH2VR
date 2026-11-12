# OpenXR headers

openxr.h, openxr_platform.h, openxr_platform_defines.h from the Khronos OpenXR SDK 1.1.65 (Apache-2.0, see LICENSE).
Copied 2026-11-10 from a local OpenXR-SDK checkout. The loader DLL is not vendored: the probe loads the one shipped with the
active runtime (SteamVR: steamapps\common\SteamVR\bin\win64\openxr_loader.dll, found via the registry's ActiveRuntime).
