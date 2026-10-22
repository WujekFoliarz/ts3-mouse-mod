# TS3Mouse v2

C++17 mouse-input mod for the inspected **x64 Toy Story 3 Complete Edition** executable. This does not support the original 2010 executable or arbitrary updates. Build identity and hook instructions are checked before any patch is written.

The game explicitly deletes the smaller mouse axis at a ratio greater than 3:1. v2 bypasses that filter for every mouse consumer. It also replaces the normal TargetSelectDriver angular aiming input with displacement-based yaw/pitch, bypassing its stick deadzones, acceleration and speed cap. The previous version changed camera angles but left this separate reticle input path intact.

Normal angular reticle modes 0/2/3 and eligible non-aiming tether cameras have direct displacement input. Specialized distance/trajectory modes 1 and 4+, Alley Toss and other independent minigame controllers retain their native input processing beyond the shared axis-snap fix. Native pitch limits, camera permissions and automatic target tracking while the mouse is idle remain in effect. This is not a universal replacement of every camera/minigame controller.

## Build

Linux with CMake and MinGW-w64:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Windows, Visual Studio x64 developer terminal:

```bat
cmake -S . -B build-vs -A x64
cmake --build build-vs --config Release
```

The output is `build/dinput8.dll` (Visual Studio: `build-vs/Release/dinput8.dll`). The DLL statically links its C++ runtime. No additional hook library is required.

## Install / remove

1. Close the game. Back up any existing `dinput8.dll`, `TS3Mouse.ini` and `TS3Mouse.log` beside its executable. This proxy does not chain another mod loader.
2. Copy the built `dinput8.dll` and the supplied `TS3Mouse.ini` beside `Toy Story 3 Complete Edition.exe`.
3. On Proton, use this Steam launch option:

   ```sh
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```

4. Restart the game. `TS3Mouse.log` should contain `TS3Mouse v2 startup` and `Installed 6 hooks + axis-snap bypass`.

`Enabled=1` is required. `SensitivityX/Y` are degrees per mouse count for the direct paths. Start at `0.08`. `AimingOnly=1` disables the additional non-aiming camera integration; the shared axis-snap fix still applies. `InvertX=1` and `InvertY=0` are the defaults, including when the INI or keys are missing. Toggle either between `0` and `1` to reverse that axis. They control the direct paths independently of the game's inversion setting. Restart after editing.

The existing installation was backed up to `backups/v1/`. The deployed v2 INI retains the existing `AimingOnly=1` preference and enables the mod. No executable or asset is changed on disk. To uninstall, close the game and remove the mod DLL/INI or restore your backup. Remove the launch override if no other mod needs it.

## Check the result

In ordinary aiming, move in a slow, shallow diagonal, then stop. Both axes should respond, without accumulated stick turning after stopping. Compare equal mouse travel at slow and fast speeds away from pitch limits and automatic target locks.

Logs distinguish these stages:

- `DirectInput movement captured`: the mouse polling hook received real movement.
- `TargetSelect input reached mode ...`: identifies the aiming handler in use.
- `Direct reticle movement: mode ...`: nonzero counts reached the new angular reticle hook. The old camera-only activation message did not establish this.
- `specialized native handler`: that aiming mode uses its original specialized controller.
- `awaiting eligible mouse sample`: a focus, mapper, device or freshness check temporarily kept native input; later direct-movement messages confirm recovery.

If it still snaps, keep the new log and note the exact weapon/scene. The absence of `Direct reticle movement` during ordinary aiming is useful diagnostic evidence, not proof the hook works.

## Validation

```sh
cmake -S . -B build-tests -DTS3_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
python3 tools/verify_game.py '/path/to/Toy Story 3 Complete Edition.exe'
python3 tests/game_snap_test.py '/path/to/Toy Story 3 Complete Edition.exe'
```

The last test requires Linux x86-64 and executes just the verified EXE's mouse-axis filter in an isolated fixture. It reproduces the original lost diagonal and checks the actual replacement jump. It does not launch the game.

Validation completed for v2: MinGW Release DLL/test builds, exact executable/hash/site verification, portable frame-rate/diagonal/stop/focus/pitch-limit tests, and execution of the original/patched axis-filter instructions.

The Windows `hook_harness.exe` was compiled with `-DTS3_BUILD_TESTS=ON`, but **could not be run in this session**: the sandbox prevents Wine's socket bind (`Operation not permitted`). On Windows, run it from the build directory alongside `dinput8.dll`; on Linux, use Wine outside that restricted session. It tests patch relays, native fallbacks, reticle integration, physical-stick arbitration, shared-page protection restoration, and DLL forwarding. It creates a temporary focus window. This is a fixture, not an in-game test.

**In-game feel and Windows hook execution still need verification.** See [the analysis notes](docs/ANALYSIS.md) for the disassembly evidence and limits.
