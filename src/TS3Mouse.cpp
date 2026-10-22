/*
 * TS3Mouse: mouse displacement -> camera angle, for the inspected x64 build.
 *
 * BUILD (Windows, VS developer terminal):
 *   cmake -S . -B build -A x64
 *   cmake --build build --config Release
 * BUILD (Linux, MinGW-w64):
 *   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build
 * INSTALL: put dinput8.dll and TS3Mouse.ini beside the game's EXE. Keep any
 * existing dinput8.dll separately; this proxy does not chain another mod loader.
 * PROTON Steam launch option: WINEDLLOVERRIDES="dinput8=n,b" %command%
 * REMOVE: delete those two files. No executable or asset is modified on disk.
 *
 * v2 HOOKS (RVAs for the SHA-256 below; never use on the 2010 executable):
 * 27B8F7: read DIMOUSESTATE2 before the game's input transformations.
 * 27B943: jump over the 3:1 minor-axis discard (ALL mouse consumers).
 * 27BA1B: record physical driver axes before keyboard-to-gamepad copying.
 * 0AF528: scope TargetSelectDriver input, preserving its gameplay checks.
 * 0859F6: replace normal angular reticle deadzones/acceleration with counts.
 * 21ABC1 / 1FE356: replace non-aiming tether-camera angle integration.
 *
 * Native aiming uses TargetSelectDriver independently of CTetherWTVC. v1 only
 * hooked the latter, leaving reticle snapping/deadzones/acceleration intact.
 * v2 hooks reticle modes 0/2/3; specialized distance/trajectory handlers retain
 * native behavior except the global mouse axis-snap fix. See docs/ANALYSIS.md.
 *
 * CALLs use nearby indirect jump relays, complete instruction boundaries and
 * the Windows x64 ABI. Install during the first DirectInput8Create, before the
 * input update begins. Do not inject mid-game or hot-unload this DLL.
 * Unknown executable hashes or changed hook bytes leave the game unpatched.
 */
#include <windows.h>
#include <algorithm>
#include <unknwn.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>
#include "mouse_input.hpp"

namespace {
using Byte = unsigned char;
constexpr std::uint32_t kTimestamp = 0x6ABF29B6;
constexpr std::uint32_t kImageSize = 0xBDC000;
constexpr char kSha256[] = "f87941b418a56b935a5e8372c11720304410d204a8cabd1d9970becfceff9722";
constexpr std::uintptr_t kCameraInput = 0x1FE0B0;
constexpr std::uintptr_t kNormalize = 0x251E00;
constexpr std::uintptr_t kKeyboardVtable = 0x7BC938;

HMODULE g_self = nullptr, g_systemDinput = nullptr;
std::uintptr_t g_base = 0;
std::wstring g_ini, g_log;
INIT_ONCE g_forwardOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_modOnce = INIT_ONCE_STATIC_INIT;
SRWLOCK g_sampleLock = SRWLOCK_INIT;
ts3::Samples g_samples;
ts3::Samples g_aimSamples;
void* g_sampleDevice = nullptr;
std::atomic<unsigned> g_loggedModes{0};
std::atomic<unsigned> g_loggedAimModes{0};
std::atomic<unsigned> g_seenAimModes{0};
std::atomic<bool> g_loggedPoll{false}, g_loggedAimFallback{false};
struct DriverSample { void* driver = nullptr; float x = 0, y = 0; };
std::array<DriverSample, 11> g_drivers{};
std::size_t g_driverCount = 0;

struct Config {
    bool enabled = true, invertX = true, invertY = false, aimingOnly = false;
    double degreesX = 0.08, degreesY = 0.08;
} g_config;

struct MouseState {
    LONG x, y, wheel;
    BYTE buttons[8];
};
static_assert(sizeof(MouseState) == 20, "DIMOUSESTATE2 ABI mismatch");
static_assert(sizeof(void*) == 8, "Only x64 is supported");

template<class T> T Read(const void* object, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const Byte*>(object) + offset, sizeof(value));
    return value;
}
template<class T> void Write(void* object, std::size_t offset, T value) noexcept {
    std::memcpy(static_cast<Byte*>(object) + offset, &value, sizeof(value));
}

void Log(const char* format, ...) noexcept {
    if (g_log.empty()) return;
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer) - 2, format, args);
    va_end(args);
    if (length < 0) return;
    const auto n = static_cast<std::size_t>(length) < sizeof(buffer) - 2
                 ? static_cast<std::size_t>(length) : sizeof(buffer) - 3;
    buffer[n] = '\r'; buffer[n + 1] = '\n';
    HANDLE file = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, buffer, static_cast<DWORD>(n + 2), &written, nullptr);
        CloseHandle(file);
    }
}

bool ProcessHasFocus() noexcept {
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
}

// This CALL replaces `mov edx,20; call [rax+48h]`. RCX (device) and R8
// (output) are already prepared. RDX is deliberately ignored and restored to 20
// for the real COM method. Buttons, wheel, HRESULT and acquisition remain native.
HRESULT WINAPI PollMouse(void* device, DWORD, MouseState* output) noexcept {
    using GetState = HRESULT (WINAPI*)(void*, DWORD, void*);
    void** vtable = Read<void**>(device, 0);
    const auto getState = reinterpret_cast<GetState>(vtable[9]);
    const HRESULT result = getState(device, sizeof(MouseState), output);
    const bool focused = ProcessHasFocus();
    const ts3::Delta delta = SUCCEEDED(result)
        ? ts3::Delta{static_cast<double>(output->x), static_cast<double>(output->y)}
        : ts3::Delta{};
    AcquireSRWLockExclusive(&g_sampleLock);
    g_sampleDevice = device;
    g_samples.submit(SUCCEEDED(result), focused, delta, GetTickCount64());
    g_aimSamples.submit(SUCCEEDED(result), focused, delta, GetTickCount64());
    g_driverCount = 0;
    ReleaseSRWLockExclusive(&g_sampleLock);
    if (SUCCEEDED(result) && focused && (delta.x != 0 || delta.y != 0) && !g_loggedPoll.exchange(true))
        Log("DirectInput movement captured: %.0f / %.0f counts.", delta.x, delta.y);
    return result;
}

// Capture physical axes BEFORE the manager copies keyboard mouse axes into the
// selected gamepad (RVA 27BA7A). Looking at that gamepad later misidentifies
// mouse movement as physical stick movement and disables the raw camera hook.
void UpdateDriver(void* driver) noexcept {
    auto** vtable = Read<void**>(driver, 0);
    reinterpret_cast<void (*)(void*)>(vtable[0xD8 / 8])(driver);
    AcquireSRWLockExclusive(&g_sampleLock);
    if (g_driverCount < g_drivers.size())
        g_drivers[g_driverCount++] = {driver, Read<float>(driver, 0x70), Read<float>(driver, 0x74)};
    ReleaseSRWLockExclusive(&g_sampleLock);
}

bool PhysicalStickMoving(void* driver) noexcept {
    bool moving = true; // Unobserved drivers fail closed.
    AcquireSRWLockExclusive(&g_sampleLock);
    for (std::size_t i = 0; i < g_driverCount; ++i) {
        if (g_drivers[i].driver == driver) {
            moving = g_drivers[i].x != 0 || g_drivers[i].y != 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_sampleLock);
    return moving;
}

// A mapper may point at the keyboard directly, or at a neutral gamepad with a
// keyboard fallback. A moving physical stick keeps the game's original path.
void* KeyboardForMapper(void* mapper) noexcept {
    if (!mapper) return nullptr;
    if (!Read<BYTE>(mapper, 0x10) || !Read<BYTE>(mapper, 0x154)) return nullptr;
    if (Read<int>(mapper, 0x1C) != 3 || Read<int>(mapper, 0x20) != 4) return nullptr;
    void* driver = Read<void*>(mapper, 8);
    for (int depth = 0; driver && depth < 4; ++depth) {
        if (Read<std::uintptr_t>(driver, 0) == g_base + kKeyboardVtable) return driver;
        if (PhysicalStickMoving(driver)) return nullptr;
        driver = Read<void*>(driver, 0x270);
    }
    return nullptr;
}

bool TakeMouse(void* mapper, ts3::Samples& samples, ts3::Delta& counts) noexcept {
    void* keyboard = KeyboardForMapper(mapper);
    if (!keyboard || !(Read<BYTE>(keyboard, 0x480) || Read<BYTE>(keyboard, 0x482))) return false;
    void* manager = Read<void*>(keyboard, 0x2E8);
    HWND window = manager ? Read<HWND>(manager, 0x238) : nullptr;
    if (!window || !Read<BYTE>(manager, 0x501) || !ProcessHasFocus() ||
        GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(window, GA_ROOT)) return false;
    AcquireSRWLockExclusive(&g_sampleLock);
    const bool available = g_sampleDevice == Read<void*>(manager, 0x220) &&
                           samples.take(GetTickCount64(), counts);
    ReleaseSRWLockExclusive(&g_sampleLock);
    return available;
}

struct AimScope { void* target; void* mapper; };
thread_local AimScope* g_aimScope = nullptr;

void TargetInput(void* target, void* mapper) noexcept {
    const unsigned mode = static_cast<unsigned>(Read<int>(target, 0x208));
    if (mode < 32 && !(g_seenAimModes.fetch_or(1u << mode) & (1u << mode)))
        Log("TargetSelect input reached mode %u (%s).", mode,
            mode == 0 || mode == 2 || mode == 3 ? "direct angular mouse eligible" : "specialized native handler");
    AimScope scope{target, mapper};
    auto* previous = g_aimScope;
    g_aimScope = &scope;
    reinterpret_cast<void (*)(void*, void*)>(g_base + 0x858D0)(target, mapper);
    g_aimScope = previous;
}

// TargetSelectDriver has its OWN input integration. Camera yaw/pitch are not
// the source of its aim. Hook only its normal angular aiming branch (modes
// 0/2/3), leaving distance/trajectory modes 1 and 4+ to their native handlers.
// Windows x64 ABI: RCX=target, XMM1=x, XMM2=y, R9D=driver permissions, AL=bool.
bool AimMouse(void* target, float x, float y, unsigned flags) noexcept {
    const auto original = reinterpret_cast<bool (*)(void*, float, float, unsigned)>(g_base + 0x402E0);
    const float dt = Read<float>(reinterpret_cast<void*>(g_base), 0x95483C);
    const float pitch = Read<float>(target, 0x18C);
    const float minimum = Read<float>(target, 0x1A0), maximum = Read<float>(target, 0x1A4);
    ts3::Delta counts{};
    if (!g_aimScope || g_aimScope->target != target || !(flags & 0x800) ||
        !std::isfinite(dt) || dt <= 0 || !std::isfinite(pitch) ||
        !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
        return original(target, x, y, flags);
    if (!TakeMouse(g_aimScope->mapper, g_aimSamples, counts)) {
        if (!g_loggedAimFallback.exchange(true))
            Log("Reticle awaiting eligible mouse sample (focus/device/mapper); native input used meanwhile.");
        return original(target, x, y, flags);
    }

    const auto angle = ts3::radians(counts, g_config.degreesX, g_config.degreesY,
                                   g_config.invertX, g_config.invertY);
    // RVA 086037 later multiplies yaw velocity by this same dt. Divide here
    // solely to cancel that integration, with no stick speed cap/acceleration.
    // Pitch is already an angle; retain the game's per-mode pitch boundaries.
    const auto step = ts3::aimStep(angle, dt, pitch, minimum, maximum);
    Write(target, 0x168, step.yawVelocity);
    Write(target, 0x194, 0.0f);
    Write(target, 0x18C, step.pitch);
    const bool moved = counts.x != 0 || counts.y != 0;
    if (moved) Write(target, 0x1C8, 0.0f); // native manual-aim reset
    const unsigned mode = static_cast<unsigned>(Read<int>(target, 0x208));
    if (moved && mode < 32 && !(g_loggedAimModes.fetch_or(1u << mode) & (1u << mode)))
        Log("Direct reticle movement: mode %u, counts %.0f / %.0f, radians %.6f / %.6f.",
            mode, counts.x, counts.y, angle.x, angle.y);
    // The caller uses AL to suppress automatic target movement this update.
    // Preserve automatic target tracking when the user is not moving the mouse.
    return moved;
}

struct CameraScope {
    void* camera = nullptr;
    bool raw = false;
    float yaw = 0, pitch = 0;
    float deltaYaw = 0, deltaPitch = 0;
    std::uint32_t priorManualFlag = 0;
};
thread_local CameraScope* g_scope = nullptr;

// Signature verified at the CALL: RCX = CTetherWTVC*, RDX = ControlMapper*.
void CameraInput(void* camera, void* mapper) noexcept {
    CameraScope scope{};
    const int mode = Read<int>(camera, 0x224);
    void* keyboard = KeyboardForMapper(mapper);
    const float dt = Read<float>(camera, 0x54);
    // Mode 6 follows TargetSelectDriver; modifying camera angles there fights
    // its independently integrated reticle. AimMouse handles that source now.
    if (keyboard && !g_config.aimingOnly && mode != 6 &&
        std::isfinite(dt) && dt > 0 && !Read<BYTE>(camera, 0x1C0) &&
        (Read<BYTE>(keyboard, 0x480) || Read<BYTE>(keyboard, 0x482))) {
        void* manager = Read<void*>(keyboard, 0x2E8);
        HWND gameWindow = manager ? Read<HWND>(manager, 0x238) : nullptr;
        if (gameWindow && Read<BYTE>(manager, 0x501) &&
            GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(gameWindow, GA_ROOT)) {
            ts3::Delta counts{};
            AcquireSRWLockExclusive(&g_sampleLock);
            const bool available = g_sampleDevice == Read<void*>(manager, 0x220) &&
                                   g_samples.take(GetTickCount64(), counts);
            ReleaseSRWLockExclusive(&g_sampleLock);
            scope.yaw = Read<float>(camera, 0x24);
            scope.pitch = Read<float>(camera, 0x28);
            if (available && std::isfinite(scope.yaw) && std::isfinite(scope.pitch)) {
                const auto angle = ts3::radians(counts, g_config.degreesX, g_config.degreesY,
                                                g_config.invertX, g_config.invertY);
                scope.raw = true;
                scope.camera = camera;
                // Preserve the game's per-update look locks. Positive pitch is
                // down: the game reconstructs it with atan2(-view.y, horizontal).
                const bool locked = Read<BYTE>(camera, 0x1AE) != 0;
                scope.deltaYaw = locked ? 0.0f : static_cast<float>(angle.x);
                scope.deltaPitch = locked || Read<BYTE>(camera, 0x1AC)
                                 ? 0.0f : static_cast<float>(angle.y);
                scope.priorManualFlag = Read<std::uint32_t>(camera, 0x208) & 2u;
            }
        }
    }
    CameraScope* previous = g_scope;
    g_scope = &scope;
    reinterpret_cast<void (*)(void*, void*)>(g_base + kCameraInput)(camera, mapper);
    g_scope = previous;
}

// The caller has already integrated its virtual stick. Replace both resulting
// angles, rather than adding a second mouse contribution. The remaining native
// instructions wrap yaw and clamp both axes using the current camera settings.
float IntegrateMouse(float nativeYaw) noexcept {
    CameraScope* scope = g_scope;
    if (scope && scope->raw) {
        void* camera = scope->camera;
        Write(camera, 0x28, scope->pitch + scope->deltaPitch);
        Write(camera, 0x2C, scope->deltaYaw);
        auto flags = Read<std::uint32_t>(camera, 0x208);
        flags = (flags & ~2u) | scope->priorManualFlag;
        if (scope->deltaYaw != 0 || scope->deltaPitch != 0) flags |= 2u;
        Write(camera, 0x208, flags);
        nativeYaw = scope->yaw + scope->deltaYaw;
        const unsigned mode = static_cast<unsigned>(Read<int>(camera, 0x224));
        if ((scope->deltaYaw != 0 || scope->deltaPitch != 0) && mode < 32 &&
            !(g_loggedModes.fetch_or(1u << mode) & (1u << mode)))
            Log("Raw mouse reached camera mode %u%s.", mode, mode == 6 ? " (aiming)" : "");
    }
    return reinterpret_cast<float (*)(float)>(g_base + kNormalize)(nativeYaw);
}

double ReadSensitivity(const wchar_t* key) noexcept {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"Mouse", key, L"0.08", value, 64, g_ini.c_str());
    wchar_t* end = nullptr;
    const double number = std::wcstod(value, &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (end == value || !end || *end || !std::isfinite(number) || number <= 0 || number > 10) {
        Log("Invalid sensitivity; using 0.08 degrees/count.");
        return 0.08;
    }
    return number;
}

bool HashExecutable(const wchar_t* path, char (&hex)[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    // Windows 7+ supports algorithm-owned hash storage when pbHashObject is null.
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<Byte, 65536> buffer{};
    DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            ok = false; break;
        }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer.data(), bytes, 0) >= 0;
    }
    std::array<Byte, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (ok) {
        constexpr char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < digest.size(); ++i) {
            hex[2 * i] = digits[digest[i] >> 4];
            hex[2 * i + 1] = digits[digest[i] & 15];
        }
        hex[64] = '\0';
    }
    return ok;
}

struct Patch {
    std::uintptr_t rva;
    std::size_t size;
    std::array<Byte, 8> expected;
    void* destination;
    DWORD oldProtection = 0;
    std::array<Byte, 8> replacement{};
};

Byte* AllocateRelays() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto align = [granularity](std::uintptr_t n) {
        return (n + granularity - 1) & ~(granularity - 1);
    };
    // All CALL sites must reach every relay with a signed rel32.
    constexpr std::uintptr_t reach = 0x7FFF0000;
    const auto low = g_base + 0x27B8F7 > reach ? g_base + 0x27B8F7 - reach : granularity;
    const auto high = g_base + 0x859F6 + reach;
    const auto scan = [&](std::uintptr_t first, std::uintptr_t last) -> Byte* {
        for (auto address = align(first); address + 4096 < last;) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory))) break;
            if (memory.State == MEM_FREE) {
                if (void* allocation = VirtualAlloc(reinterpret_cast<void*>(address), 4096,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return static_cast<Byte*>(allocation);
            }
            const auto next = align(reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (next <= address) break;
            address = next;
        }
        return nullptr;
    };
    Byte* result = scan(g_base + kImageSize, high);
    return result ? result : scan(low, g_base);
}

bool InstallPatches() noexcept {
    std::array<Patch, 7> patches{{
        {0x27B8F7, 8, {0xBA,0x14,0,0,0,0xFF,0x50,0x48}, reinterpret_cast<void*>(&PollMouse)},
        {0x21ABC1, 5, {0xE8,0xEA,0x34,0xFE,0xFF}, reinterpret_cast<void*>(&CameraInput)},
        {0x1FE356, 5, {0xE8,0xA5,0x3A,0x05,0}, reinterpret_cast<void*>(&IntegrateMouse)},
        {0x27BA1B, 6, {0xFF,0x90,0xD8,0,0,0}, reinterpret_cast<void*>(&UpdateDriver)},
        {0x0AF528, 5, {0xE8,0xA3,0x63,0xFD,0xFF}, reinterpret_cast<void*>(&TargetInput)},
        {0x0859F6, 5, {0xE8,0xE5,0xA8,0xFB,0xFF}, reinterpret_cast<void*>(&AimMouse)},
        // Skip the entire 3:1 axis-discard block, without changing mouse
        // buttons/wheel or its raw counts. Applies to ALL mouse consumers.
        {0x27B943, 6, {0x43,0x8D,0x04,0x40,0x3B,0xC2}, nullptr}
    }};
    for (const auto& patch : patches) {
        if (std::memcmp(reinterpret_cast<void*>(g_base + patch.rva), patch.expected.data(), patch.size)) {
            Log("Call site 0x%llX changed; no hooks installed.", static_cast<unsigned long long>(patch.rva));
            return false;
        }
    }
    Byte* relays = AllocateRelays();
    if (!relays) { Log("Cannot allocate reachable call relays; no hooks installed."); return false; }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        auto& patch = patches[i];
        if (!patch.destination) {
            patch.replacement = {0xE9,0x1B,0,0,0,0x90}; // jmp RVA 27B963
            continue;
        }
        Byte* relay = relays + i * 16;
        // jmp qword ptr [rip+0]; absolute destination follows the instruction.
        const Byte jump[] = {0xFF,0x25,0,0,0,0};
        std::memcpy(relay, jump, sizeof(jump));
        std::memcpy(relay + 6, &patch.destination, sizeof(void*));
        const auto distance = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(relay))
                            - static_cast<std::int64_t>(g_base + patch.rva + 5);
        if (distance < INT32_MIN || distance > INT32_MAX) {
            VirtualFree(relays, 0, MEM_RELEASE); return false;
        }
        patch.replacement.fill(0x90);
        patch.replacement[0] = 0xE8;
        const auto displacement = static_cast<std::int32_t>(distance);
        std::memcpy(patch.replacement.data() + 1, &displacement, sizeof(displacement));
    }
    DWORD previous = 0;
    if (!VirtualProtect(relays, 4096, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(relays, 0, MEM_RELEASE); return false;
    }
    FlushInstructionCache(GetCurrentProcess(), relays, 4096);
    std::size_t protectedCount = 0;
    for (auto& patch : patches) {
        if (!VirtualProtect(reinterpret_cast<void*>(g_base + patch.rva), patch.size,
                            PAGE_EXECUTE_READWRITE, &patch.oldProtection)) break;
        ++protectedCount;
    }
    if (protectedCount != patches.size()) {
        // Reverse order is essential: multiple sites share the same page.
        for (std::size_t i = protectedCount; i-- > 0;)
            VirtualProtect(reinterpret_cast<void*>(g_base + patches[i].rva), patches[i].size,
                           patches[i].oldProtection, &previous);
        VirtualFree(relays, 0, MEM_RELEASE);
        Log("Cannot protect hook sites; no hooks installed.");
        return false;
    }
    // Startup-only: the game's first DirectInput8Create has not yet returned.
    // All checks and allocations completed before the first instruction changes.
    for (const auto& patch : patches)
        std::memcpy(reinterpret_cast<void*>(g_base + patch.rva), patch.replacement.data(), patch.size);
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        const auto& patch = *it;
        if (!VirtualProtect(reinterpret_cast<void*>(g_base + patch.rva), patch.size,
                            patch.oldProtection, &previous)) Log("Could not restore code page protection.");
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_base + patch.rva), patch.size);
    }
    // Relays and this module remain resident until process exit.
    return true;
}

BOOL CALLBACK InitializeMod(PINIT_ONCE, PVOID, PVOID*) noexcept {
    try {
        std::array<wchar_t, 32768> path{};
        DWORD length = GetModuleFileNameW(g_self, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return TRUE;
        const std::wstring selfPath(path.data(), length);
        const auto separator = selfPath.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return TRUE;
        const auto directory = selfPath.substr(0, separator + 1);
        g_ini = directory + L"TS3Mouse.ini";
        g_log = directory + L"TS3Mouse.log";
        Log("--- TS3Mouse v2 startup (2026-10-22) ---");
        g_config.enabled = GetPrivateProfileIntW(L"Mouse", L"Enabled", 1, g_ini.c_str()) != 0;
        if (!g_config.enabled) { Log("Disabled in INI; native input active."); return TRUE; }
        g_config.degreesX = ReadSensitivity(L"SensitivityX");
        g_config.degreesY = ReadSensitivity(L"SensitivityY");
        g_config.invertX = GetPrivateProfileIntW(L"Mouse", L"InvertX", 1, g_ini.c_str()) != 0;
        g_config.invertY = GetPrivateProfileIntW(L"Mouse", L"InvertY", 0, g_ini.c_str()) != 0;
        g_config.aimingOnly = GetPrivateProfileIntW(L"Mouse", L"AimingOnly", 0, g_ini.c_str()) != 0;
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return TRUE;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize) {
            Log("Unsupported EXE headers; native input active."); return TRUE;
        }
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        char digest[65]{};
        if (!length || length >= path.size() || !HashExecutable(path.data(), digest)) {
            Log("Cannot verify EXE SHA-256; native input active."); return TRUE;
        }
        if (std::strcmp(digest, kSha256)) {
            Log("Unsupported EXE SHA-256: %s; native input active.", digest); return TRUE;
        }
        if (InstallPatches())
            Log("Installed 6 hooks + axis-snap bypass. Sensitivity %.5f / %.5f degrees/count. Awaiting mouse MOVEMENT in gameplay.",
                g_config.degreesX, g_config.degreesY);
        else Log("Installation failed; native input active.");
    } catch (...) {
        Log("Initialization failed; no further installation attempted.");
    }
    return TRUE;
}

BOOL CALLBACK LoadSystemDinput(PINIT_ONCE, PVOID, PVOID*) noexcept {
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    constexpr wchar_t suffix[] = L"\\dinput8.dll";
    if (length && length + std::size(suffix) <= MAX_PATH) {
        std::memcpy(path + length, suffix, sizeof(suffix));
        g_systemDinput = LoadLibraryW(path); // absolute path prevents proxy recursion
    }
    return TRUE;
}
template<class T> T Forward(const char* name) noexcept {
    InitOnceExecuteOnce(&g_forwardOnce, LoadSystemDinput, nullptr, nullptr);
    const FARPROC address = g_systemDinput ? GetProcAddress(g_systemDinput, name) : nullptr;
    static_assert(sizeof(T) == sizeof(address), "Win64 function pointer ABI");
    T function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}
} // namespace

extern "C" HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE instance, DWORD version,
    REFIID iid, void** output, LPUNKNOWN outer) {
    using Function = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN);
    const auto original = Forward<Function>("DirectInput8Create");
    if (!original) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    InitOnceExecuteOnce(&g_modOnce, InitializeMod, nullptr, nullptr);
    return original(instance, version, iid, output, outer);
}
extern "C" HRESULT WINAPI ProxyDllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI ProxyDllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    using Function = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    const auto original = Forward<Function>("DllGetClassObject");
    return original ? original(clsid, iid, output) : CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI ProxyDllRegisterServer() {
    const auto original = Forward<HRESULT (WINAPI*)()>("DllRegisterServer");
    return original ? original() : E_NOTIMPL;
}
extern "C" HRESULT WINAPI ProxyDllUnregisterServer() {
    const auto original = Forward<HRESULT (WINAPI*)()>("DllUnregisterServer");
    return original ? original() : E_NOTIMPL;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_self = module;
    // No hooks, waits, file I/O or library loading under the loader lock.
    return TRUE;
}
