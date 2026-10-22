# Inspected build and v2 changes

Analyzed locally on 2026-10-22. Source of evidence: the supplied executable, its disassembly, the v1 sources, the installed INI and TS3Mouse.log. No guessed offsets from the 2010 game were used.

- SHA-256: `f87941b418a56b935a5e8372c11720304410d204a8cabd1d9970becfceff9722`
- AMD64, PE timestamp `6ABF29B6`, image size `BDC000`.
- Preferred image base `140000000`; all locations below are **RVAs**, rebased at runtime.

## Why v1 could report success without fixing aiming

The existing log showed three successful installations and a camera-mode-6 activation, including an enabled run with sensitivity 1.08. The final run was disabled (`Enabled=0`). Therefore the disabled INI was not a sufficient explanation for all prior failures.

The v1 log was emitted even for a zero-displacement sample. Its fixture checked synthetic camera-angle replacement, not whether the actual aiming reticle consumed those camera angles.

`CTetherWTVC` calls its input handler at `21ABC1`. In mode 6 it later calls the targeting camera path via `200710`, which consults `TargetSelectDriver` and its reticle direction (`079D80` / `081330`). At `21B047` and `21B09C`, camera angles are reconstructed from the resulting direction. Altering the earlier camera integration is not sufficient to replace reticle input.

The reticle independently reads mapper axes at `085997` / `0859A8`. It dispatches by target mode (`target+208`):

| Mode | Input handler | v2 |
| --- | --- | --- |
| 0, 2, 3 | `0402E0` via CALL `0859F6` | Direct angular mouse input |
| 1 | `03FF60` via CALL `0859E3` | Native distance/trajectory behavior |
| Other | `040810` via CALL `0859D0` | Native specialized behavior |

The normal handler has separate axis deadzones (`target+180`, `target+1B4`) and builds/clamps velocities (`+168`, `+194`). The yaw velocity is multiplied by global frame time at `08602B` / `086037` before constructing the yaw rotation matrix. Pitch (`+18C`) is passed directly to the pitch rotation matrix at `086052`. This establishes the integration boundary used by v2.

## Exact axis snapping

Immediately after `IDirectInputDevice8::GetDeviceState(20, ...)`, the game copies signed counts into `manager+DC` and `manager+E0`. Instructions `27B932..27B962` implement:

```cpp
if (3 * abs(dx) < abs(dy)) dx = 0;
else if (3 * abs(dy) < abs(dx)) dy = 0;
```

This matches the user's slow horizontal/vertical-only movement report. v2 writes `E9 1B 00 00 00 90` at `27B943`, jumping to `27B963` after both discard arms. The overwritten six bytes contain complete instructions. The following button/wheel processing is preserved.

`tests/game_snap_test.py` reads these exact instructions from the hash-verified EXE, executes them with controlled counts, reproduces `(4,1) -> (4,0)` and `(1,4) -> (0,4)`, then applies the same jump and verifies positive/negative shallow diagonals. It does not infer success from a reimplementation of the filter.

The keyboard driver at `27AD90` also clamps mouse counts into virtual axes and uses a frame-dependent factor. The direct hooks read counts before this transformation and bypass it for their supported camera/reticle paths. Other consumers benefit from the axis-discard bypass only.

## Reticle hook

`0AF528` calls `0858D0` with RCX = target driver and RDX = mapper. Its wrapper supplies a thread-local scope. The replacement at `0859F6` receives RCX = target, XMM1/2 = native axes, R9D = driver flags, returns bool in AL.

After checking the original look-permission bit `0x800`, focus, mouse identity, mapper bindings and sample age, it writes:

```cpp
yawVelocity = radians(rawDx * degreesPerCountX) / nativeFrameTime;
pitch = clamp(oldPitch + radians(rawDy * degreesPerCountY), nativeMin, nativeMax);
verticalVelocity = 0;
```

The division cancels the **same** later native frame-time multiplication; it does not turn counts into a capped stick. A fresh zero-count sample sets yaw velocity to zero. The bool result marks manual movement only for nonzero counts, preserving native automatic target tracking while idle. Native pitch limits at `+1A0/+1A4` remain intact. Native locks/permissions can still prevent movement. v2 stops replacing mode-6 camera angles to avoid competing with the reticle-driven camera.

Mode 1 also modifies trajectory/distance fields and recenters pitch; it is deliberately not treated as an ordinary yaw/pitch controller. No direct-input claim is made for that mode or independent minigame controllers.

## Device routing

v1 rejected any non-keyboard driver with nonzero right-stick fields. However `27BA7A` / `27BA7D` can copy keyboard mouse axes into the selected gamepad object. This can make mouse input look like a physical stick and disable the old hook during movement. This is a code-proven possible failure path; the old log does not establish whether that routing was active in the user's run.

v2 wraps the driver-update call at `27BA1B`, records physical axes before the manager's copy, and uses that record while traversing the mapper's keyboard fallback. A genuinely moving physical stick or an unobserved driver keeps native handling. The fixed table holds the 11 drivers visited by the native update loop. Polling resets that table each input update.

## Patching and verification limits

Six rel32 CALLs target nearby absolute jump relays. The seventh patch is the axis-filter jump. Every original site is checked, then all allocations and protection changes complete before any code is changed. Shared code-page protections are restored in reverse order. Installation occurs during the first forwarded DirectInput8Create; injection during gameplay and hot unloading are unsupported.

The full EXE hash and PE identity guard the offsets. All changes are in process memory. The proxy loads the real system dinput8 by absolute path and performs no hook installation under DllMain's loader lock.

Verified here: build, portable tests, SHA/site identities, actual original/patched axis-filter execution. The Windows harness was built but Wine execution was blocked by the sandbox's socket restrictions. No in-game execution, physical-mouse feel, or claim of universal controller coverage is established by these tests.
