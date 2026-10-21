# sonmat-hook-ue4

A part of 손맛 햅틱 / Sonmat Haptics (`sonmat`): dshaptics_ue4.dll.

Hook for Unreal Engine 4 games with Unreal's own audio (Lies of P): FAudioDevice::AddNewActiveSound found by byte patterns or addresses in sigs_ue4.txt beside the DLL (the app writes it from the game's profile).

The app does not carry this in its installer for good: it fetches the release zip that `sonmat-games/parts.json` names
(address + SHA-256) into `<app>/parts/hook-ue4/` when a game needs it (`sonmat/tools/partfetch.py`). A change here reaches
players when a new tag has been released and `parts.json` points at it.

Build: Visual Studio C++ tools (x64), MinHook in minhook\ at the commit the workflow pins: build.bat dshaptics_ue4. Every push to main builds it on GitHub (artifact); a tag `v*` makes the release.
