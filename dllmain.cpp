// dllmain.cpp
#include "pch.h"
#include "safetyhook.hpp"
#include "Overlay.h"
#include "OptionsDetector.h"
#include "TreasurePolicy.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <initializer_list>

// ============================================================
// GLOBALS
// ============================================================

static HMODULE g_hModule = nullptr;

static safetyhook::MidHook g_modeHook{};
static safetyhook::MidHook g_finalAiHook{};

static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_inOptions{ false };
static std::atomic<bool> g_isTreasure{ false };
static std::atomic<uint32_t> g_lastGameMode{ 0xFFFFFFFFu };

// 0 = Default (vanilla Treasure behaviour; no intervention)
// 1 = Easy
// 2 = Medium
// 3 = Hard
// 4 = Very Hard
// 5 = Ultra Hard
static std::atomic<int> g_difficulty{ 0 };
static std::atomic<int> g_rounds{ 0 }, g_specialFights{ 0 }, g_selectedRow{ 0 };
static safetyhook::MidHook g_specialHook{}, g_roundHook{}, g_initialRoundHook{};

// ============================================================
// TEKKEN 7 OFFSETS / PROVEN RUNTIME CONTRACT
// ============================================================
//
// Mode discriminator:
//
//   TekkenGame-Win64-Shipping.exe + 0x2600BA
//   cmp eax,05
//
// At this point EAX is the game-mode enum.
// Proven:
//
//   Treasure = 0x0B
//   Arcade   = 0x01
//
// Final AI resolver:
//
//   TekkenGame-Win64-Shipping.exe + 0x70D009
//   movsxd rax,r9d
//
// BEFORE this instruction:
//
//   R14D = final base difficulty enum
//   R9D  = final AI profile index
//
// The game then copies its own 0x7E-byte AI profile and later stores:
//
//   battleBlock + 6D8 = R14D
//   battleBlock + 6DC = R9D
//
// Proven Practice mapping:
//
//   Easy       -> base 0 / profile 0
//   Medium     -> base 1 / profile 5
//   Hard       -> base 2 / profile 10
//   Very Hard  -> base 3 / profile 17
//   Ultra Hard -> base 4 / profile 19
//
// Proven Treasure vanilla at this resolver during our test:
//
//   base 1 / profile 0
//
// Forcing 4/19 here produced the expected Ultra-Hard-level CPU behaviour.
// ============================================================

static constexpr uintptr_t OFFSET_MODE_HOOK =
    0x2600BA;

static constexpr uintptr_t OFFSET_FINAL_AI_HOOK =
    0x70D009;

static constexpr uint32_t GAME_MODE_TREASURE =
    0x0B;

static constexpr DWORD INPUT_POLL_MS =
    10;

static constexpr ULONGLONG OPTIONS_SCAN_MS =
    150;

// Small byte checks so a different executable build fails cleanly instead
// of blindly placing a mid-hook at an unrelated instruction.
static constexpr unsigned char MODE_HOOK_BYTES[] =
{
    0x83, 0xF8, 0x05 // cmp eax,05
};

static constexpr unsigned char FINAL_AI_HOOK_BYTES[] =
{
    0x49, 0x63, 0xC1 // movsxd rax,r9d
};

// ============================================================
// DIFFICULTY NAMES / POLICY
// ============================================================

static const wchar_t* GetDifficultyName(
    int difficulty
)
{
    switch (difficulty)
    {
    case 0: return L"Default";
    case 1: return L"Easy";
    case 2: return L"Medium";
    case 3: return L"Hard";
    case 4: return L"Very Hard";
    case 5: return L"Ultra Hard";
    default: return L"Unknown";
    }
}

struct DifficultyPolicy
{
    uint32_t base = 0;
    uint32_t profile = 0;
};

static DifficultyPolicy GetPolicy(
    int difficulty
)
{
    switch (difficulty)
    {
    case 1:
        return { 0, 0 };

    case 2:
        return { 1, 5 };

    case 3:
        return { 2, 10 };

    case 4:
        return { 3, 17 };

    case 5:
        return { 4, 19 };

    case 0:
    default:
        return { 0, 0 };
    }
}

// ============================================================
// PATHS
// ============================================================

static std::wstring GetModuleDirectory()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(
        g_hModule,
        path,
        MAX_PATH
    );

    std::wstring result =
        path;

    const size_t slash =
        result.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
    {
        result.resize(slash + 1);
    }
    else
    {
        result.clear();
    }

    return result;
}

static std::wstring GetLogPath()
{
    return
        GetModuleDirectory() +
        L"T7Difficulty.log";
}

static std::wstring GetIniPath()
{
    return
        GetModuleDirectory() +
        L"T7Difficulty.ini";
}

// ============================================================
// LOG
// ============================================================

static void ResetLog()
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"w"
        ) == 0 &&
        file != nullptr)
    {
        fclose(file);
    }
}

static void Log(
    const wchar_t* text
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        L"%s\n",
        text
    );

    fclose(file);
}

#ifdef _DEBUG

static void LogAddress(
    const wchar_t* name,
    uintptr_t address
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        L"%-24s = 0x%016llX\n",
        name,
        static_cast<unsigned long long>(address)
    );

    fclose(file);
}

static void DebugLogSelection(
    int difficulty
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        L"SELECT -> %d / %s\n",
        difficulty,
        GetDifficultyName(difficulty)
    );

    fclose(file);
}

static void DebugLogOptionsTransition(
    bool inside
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        inside
            ? L"ENTER OPTIONS"
            : L"EXIT OPTIONS"
    );

    fwprintf(
        file,
        L" | detector=%s | pattern=%s | baseline=%d | live=%d\n",
        OptionsDetectorLanguage(),
        OptionsDetectorPattern(),
        OptionsDetectorBaselineCount(),
        OptionsDetectorLastCount()
    );

    fclose(file);
}

static void DebugLogMode(
    uint32_t mode,
    bool treasure
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        L"MODE -> %u / 0x%X | Treasure=%s\n",
        mode,
        mode,
        treasure ? L"YES" : L"NO"
    );

    fclose(file);
}

static void DebugLogFinalAi(
    int difficulty,
    bool treasure,
    uint32_t oldBase,
    uint32_t oldProfile,
    uint32_t newBase,
    uint32_t newProfile,
    bool modified
)
{
    const std::wstring path =
        GetLogPath();

    FILE* file = nullptr;

    if (_wfopen_s(
            &file,
            path.c_str(),
            L"a"
        ) != 0 ||
        file == nullptr)
    {
        return;
    }

    fwprintf(
        file,
        L"FINAL AI | Treasure=%s | Difficulty=%d/%s | base=%u->%u | profile=%u->%u | modified=%s\n",
        treasure ? L"YES" : L"NO",
        difficulty,
        GetDifficultyName(difficulty),
        oldBase,
        newBase,
        oldProfile,
        newProfile,
        modified ? L"YES" : L"NO"
    );

    fclose(file);
}

#endif

// ============================================================
// INI
// ============================================================

static int LoadDifficultyFromIni()
{
    const std::wstring path =
        GetIniPath();

    int value =
        GetPrivateProfileIntW(
            L"Difficulty",
            L"TreasureBattle",
            0,
            path.c_str()
        );

    if (value < 0 ||
        value > 5)
    {
        value = 0;
    }

    return value;
}

static bool SaveDifficultyToIni()
{
    const std::wstring path =
        GetIniPath();

    const int difficulty =
        g_difficulty.load(
            std::memory_order_relaxed
        );

    wchar_t value[16]{};

    swprintf_s(
        value,
        L"%d",
        difficulty
    );

    const BOOL result =
        WritePrivateProfileStringW(
            L"Difficulty",
            L"TreasureBattle",
            value,
            path.c_str()
        );

#ifdef _DEBUG
    if (result)
    {
        const std::wstring logPath =
            GetLogPath();

        FILE* file = nullptr;

        if (_wfopen_s(
                &file,
                logPath.c_str(),
                L"a"
            ) == 0 &&
            file != nullptr)
        {
            fwprintf(
                file,
                L"INI SAVED -> %d / %s\n",
                difficulty,
                GetDifficultyName(difficulty)
            );

            fclose(file);
        }
    }
#endif

    bool saved = result != FALSE;
    for (const auto& option : { std::pair{ L"Rounds", g_rounds.load() },
                               std::pair{ L"SpecialFights", g_specialFights.load() } })
    {
        swprintf_s(value, L"%d", option.second);
        saved = (WritePrivateProfileStringW(L"TreasureBattle", option.first, value, path.c_str()) != FALSE) && saved;
    }
    return saved;
}

// ============================================================
// BUILD/OFFSET VALIDATION
// ============================================================

static bool MemoryMatches(
    uintptr_t address,
    const unsigned char* expected,
    size_t size
)
{
    if (address < 0x10000 ||
        expected == nullptr ||
        size == 0)
    {
        return false;
    }

    __try
    {
        return
            std::memcmp(
                reinterpret_cast<const void*>(address),
                expected,
                size
            ) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ============================================================
// MID-HOOK CALLBACKS
// ============================================================

static void GameModeHook(
    safetyhook::Context& ctx
)
{
    const uint32_t mode =
        static_cast<uint32_t>(ctx.rax);

    const bool treasure =
        mode == GAME_MODE_TREASURE;

    const uint32_t oldMode =
        g_lastGameMode.exchange(
            mode,
            std::memory_order_relaxed
        );

    const bool oldTreasure =
        g_isTreasure.exchange(
            treasure,
            std::memory_order_relaxed
        );

#ifdef _DEBUG
    if (oldMode != mode ||
        oldTreasure != treasure)
    {
        DebugLogMode(
            mode,
            treasure
        );
    }
#else
    (void)oldMode;
    (void)oldTreasure;
#endif
}

static void FinalAiResolverHook(
    safetyhook::Context& ctx
)
{
    const bool treasure =
        g_isTreasure.load(
            std::memory_order_relaxed
        );

    const int difficulty =
        g_difficulty.load(
            std::memory_order_relaxed
        );

    const uint32_t oldBase =
        static_cast<uint32_t>(ctx.r14);

    const uint32_t oldProfile =
        static_cast<uint32_t>(ctx.r9);

    uint32_t newBase =
        oldBase;

    uint32_t newProfile =
        oldProfile;

    bool modified =
        false;

    // Default is intentionally a complete no-op. This preserves Tekken 7's
    // vanilla Treasure rank scaling exactly.
    if (treasure &&
        difficulty != 0)
    {
        const DifficultyPolicy policy =
            GetPolicy(difficulty);

        newBase =
            policy.base;

        newProfile =
            policy.profile;

        // The hook is immediately before "movsxd rax,r9d".
        // Writing the full 64-bit context registers with small positive
        // values exactly reproduces r14d/r9d writes (upper bits become zero).
        ctx.r14 =
            static_cast<uint64_t>(newBase);

        ctx.r9 =
            static_cast<uint64_t>(newProfile);

        modified =
            true;
    }

#ifdef _DEBUG
    DebugLogFinalAi(
        difficulty,
        treasure,
        oldBase,
        oldProfile,
        newBase,
        newProfile,
        modified
    );
#endif
}

// Additional hooks are confined to verified Treasure call sites. No global setter hook.
static bool FilterEncounter(uintptr_t address, int policy)
{
    if (address < 0x10000 || address % alignof(int32_t) != 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        address > reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize - sizeof(int32_t)) return false;
    __try
    {
        auto* type = reinterpret_cast<int32_t*>(address);
        const int old = *type;
        if (!TreasurePolicy::Suppress(policy, old))
        {
            wchar_t message[128]{};
            swprintf_s(message, L"TREASURE special type=%d unchanged | policy=%d", old, policy);
            Log(message);
            return false;
        }
        *type = 0;
        wchar_t message[128]{};
        swprintf_s(message, L"TREASURE special type=%d -> 0 | policy=%d", old, policy);
        Log(message);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Unlike the AI resolver, these sites run while the next battle is being prepared.
// The cached AI-mode flag can still describe the previous battle at this point.
static void SpecialFightHook(safetyhook::Context& ctx)
{
    // This verified entry belongs exclusively to Treasure's next-fight routine.
    FilterEncounter(ctx.rdx, g_specialFights.load());
}

static uint32_t ReadBattleMode(uintptr_t config)
{
    if (config < 0x10000 || config % alignof(uint32_t) != 0) return 0xFFFFFFFFu;
    __try { return *reinterpret_cast<const uint32_t*>(config); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

static void RoundsHook(safetyhook::Context& ctx)
{
    const int configured = g_rounds.load();
    // The initial Treasure path calls 793AA0(config, 0x0B), which writes [config],
    // immediately before this hook. Check the actual setter's RCX, not the AI cache.
    const uint32_t mode = ReadBattleMode(ctx.rcx);
    const uint64_t original = ctx.rdx;
    if (original == 2)
        ctx.rdx = static_cast<uint64_t>(TreasurePolicy::RoundValue(configured, 2, mode));
    wchar_t message[192]{};
    swprintf_s(message, L"TREASURE rounds | configured=%d | configMode=0x%X | cachedAI=%d | %llu -> %llu",
        configured, mode, g_isTreasure.load() ? 1 : 0,
        static_cast<unsigned long long>(original), static_cast<unsigned long long>(ctx.rdx));
    Log(message);
}

static void InstallTreasureHooks(uintptr_t base)
{
    // Verified 2021 NoDVD instruction contexts, including argument setup and call targets.
    static constexpr unsigned char entry[] = {0x40,0x53,0x57,0x41,0x54,0x41,0x57,0x48,0x83,0xEC,0x28,0x48,0x8B,0x1D,0xC6,0x34,0x09,0x03,0x4D,0x8B,0xE0,0x4C,0x8B,0xFA,0x48,0x8B,0xF9};
    static constexpr unsigned char regular[] = {0xBA,2,0,0,0,0x48,0x8B,0xC8,0x48,0x8B,0xD8,0xE8,0xBD,0x12,0xE9,0xFF};
    static constexpr unsigned char initial[] = {0xBA,2,0,0,0,0xC7,0x83,0x78,0x09,0,0,0,0,0,0,0x48,0x8B,0xCB,0xE8,0x52,0x01,0xE9,0xFF};
    static constexpr unsigned char modeSetter[] = {0x89,0x11,0xC3};
    static constexpr unsigned char initialMode[] = {0xBA,0x0B,0,0,0,0x48,0x8B,0xC8,0x48,0x8B,0xD8,0xE8,0x69,0xAB,0x35,0};
    static constexpr unsigned char setter[] = {0x89,0x91,0xA0,0,0,0,0xC3};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->FileHeader.TimeDateStamp != 0x616E792F ||
        !MemoryMatches(base+0x437A60, entry, sizeof(entry)) ||
        !MemoryMatches(base+0x437DD3, regular, sizeof(regular)) ||
        !MemoryMatches(base+0x438F37, initial, sizeof(initial)) ||
        !MemoryMatches(base+0x2C90A0, setter, sizeof(setter)) ||
        !MemoryMatches(base+0x793AA0, modeSetter, sizeof(modeSetter)) ||
        !MemoryMatches(base+0x438F27, initialMode, sizeof(initialMode)))
    {
        Log(L"Treasure extensions disabled: executable/context mismatch. Original difficulty retained.");
        return;
    }
    // Roll back the extension group on failure; original difficulty hooks stay installed.
    g_specialHook = safetyhook::create_mid(reinterpret_cast<void*>(base+0x437A60), SpecialFightHook);
    if (g_specialHook) g_roundHook = safetyhook::create_mid(reinterpret_cast<void*>(base+0x437DDE), RoundsHook);
    if (g_roundHook) g_initialRoundHook = safetyhook::create_mid(reinterpret_cast<void*>(base+0x438F49), RoundsHook);
    if (!g_specialHook || !g_roundHook || !g_initialRoundHook)
    {
        g_initialRoundHook.reset(); g_roundHook.reset(); g_specialHook.reset();
        Log(L"Treasure extensions disabled: hook installation failed. Original difficulty retained.");
        return;
    }
#ifdef _DEBUG
    LogAddress(L"Special encounter hook", base + 0x437A60);
    LogAddress(L"Regular rounds hook", base + 0x437DDE);
    LogAddress(L"Initial rounds hook", base + 0x438F49);
#endif
    Log(L"Treasure extensions installed: special=437A60, rounds=437DDE/438F49.");
}

// ============================================================
// XINPUT
// ============================================================
//
// The project itself can be built as the same XINPUT1_3 transparent proxy
// used by SC6. Separately, this worker loads the real XInput implementation
// by absolute System32 path for our own L1/R1 polling.
// ============================================================

using XInputGetStateFn =
    DWORD(WINAPI*)(
        DWORD,
        XINPUT_STATE*
    );

static HMODULE g_xInputModule =
    nullptr;

static XInputGetStateFn g_xInputGetState =
    nullptr;

static HMODULE LoadSystemLibrary(
    const wchar_t* fileName
)
{
    if (fileName == nullptr)
    {
        return nullptr;
    }

    wchar_t systemDirectory[MAX_PATH]{};

    const UINT length =
        GetSystemDirectoryW(
            systemDirectory,
            MAX_PATH
        );

    if (length == 0 ||
        length >= MAX_PATH)
    {
        return nullptr;
    }

    std::wstring path =
        systemDirectory;

    path += L"\\";
    path += fileName;

    return
        LoadLibraryW(
            path.c_str()
        );
}

static bool InitializeXInput()
{
    const wchar_t* candidates[] =
    {
        L"xinput1_4.dll",
        L"xinput1_3.dll",
        L"xinput9_1_0.dll"
    };

    for (const wchar_t* dll :
         candidates)
    {
        HMODULE xinputModule =
            LoadSystemLibrary(dll);

        if (xinputModule == nullptr)
        {
            continue;
        }

        auto proc =
            reinterpret_cast<XInputGetStateFn>(
                GetProcAddress(
                    xinputModule,
                    "XInputGetState"
                )
            );

        if (proc != nullptr)
        {
            g_xInputModule =
                xinputModule;

            g_xInputGetState =
                proc;

            return true;
        }

        FreeLibrary(xinputModule);
    }

    return false;
}

static void GetShoulderState(
    bool& l1,
    bool& r1,
    bool& l3
)
{
    l1 = false;
    r1 = false;
    l3 = false;

    if (g_xInputGetState == nullptr)
    {
        return;
    }

    for (DWORD id = 0;
         id < XUSER_MAX_COUNT;
         ++id)
    {
        XINPUT_STATE state{};

        if (g_xInputGetState(
                id,
                &state
            ) != ERROR_SUCCESS)
        {
            continue;
        }

        l3 = l3 || (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;

        l1 =
            l1 ||
            (state.Gamepad.wButtons &
             XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;

        r1 =
            r1 ||
            (state.Gamepad.wButtons &
             XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    }
}

// ============================================================
// INPUT / SELECTION
// ============================================================

static bool IsGameForeground()
{
    const HWND foreground =
        GetForegroundWindow();

    if (foreground == nullptr)
    {
        return false;
    }

    DWORD pid =
        0;

    GetWindowThreadProcessId(
        foreground,
        &pid
    );

    return
        pid ==
        GetCurrentProcessId();
}

static void ChangeDifficulty(
    int direction
)
{
    int current =
        g_difficulty.load(
            std::memory_order_relaxed
        );

    current +=
        direction;

    if (current < 0)
    {
        current = 5;
    }
    else if (current > 5)
    {
        current = 0;
    }

    g_difficulty.store(
        current,
        std::memory_order_relaxed
    );

#ifdef _DEBUG
    DebugLogSelection(current);
#endif
}

static void ChangeSelection(int direction)
{
    const int row = g_selectedRow.load();
    if (row == 0) { ChangeDifficulty(direction); return; }
    auto& option = row == 1 ? g_rounds : g_specialFights;
    const int current = option.load();
    if (row == 1) option.store((current + direction + 6) % 6);
    else option.store(TreasurePolicy::NextSpecial(current, direction));
}

static DWORD WINAPI InputThread(
    LPVOID
)
{
    bool oldPrevious = false;
    bool oldNext = false;
    bool oldRow = false;

    while (g_running.load(
        std::memory_order_relaxed
    ))
    {
        bool l1 = false;
        bool r1 = false;
        bool l3 = false;

        GetShoulderState(
            l1,
            r1, l3
        );
        const bool row = l3 || (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;

        const bool keyZ =
            (GetAsyncKeyState('Z') &
             0x8000) != 0;

        const bool keyX =
            (GetAsyncKeyState('X') &
             0x8000) != 0;

        const bool previous =
            l1 || keyZ;

        const bool next =
            r1 || keyX;

        const bool inOptions =
            g_inOptions.load(
                std::memory_order_relaxed
            );

        if (inOptions &&
            IsGameForeground())
        {
            if (row && !oldRow) g_selectedRow.store((g_selectedRow.load() + 1) % 3);
            if (!(previous && next))
            {
                if (previous &&
                    !oldPrevious)
                {
                    ChangeSelection(-1);
                }

                if (next &&
                    !oldNext)
                {
                    ChangeSelection(+1);
                }
            }
        }

        oldRow = row;
        oldPrevious =
            previous;

        oldNext =
            next;

        Sleep(INPUT_POLL_MS);
    }

    return 0;
}

// ============================================================
// MAIN THREAD
// ============================================================

static DWORD WINAPI MainThread(
    LPVOID
)
{
    ResetLog();

    Log(L"==============================================");
    Log(L"T7Difficulty loaded");
    Log(L"==============================================");

#ifdef _DEBUG
    Log(L"Build: DEBUG");
#else
    Log(L"Build: RELEASE");
#endif

    // --------------------------------------------------------
    // INI
    // --------------------------------------------------------

    const auto iniPath = GetIniPath();
    const int rounds = GetPrivateProfileIntW(L"TreasureBattle", L"Rounds", 0, iniPath.c_str());
    const int special = GetPrivateProfileIntW(L"TreasureBattle", L"SpecialFights", 0, iniPath.c_str());
    g_rounds.store(TreasurePolicy::ValidRounds(rounds) ? rounds : 0);
    g_specialFights.store(special >= 0 && special <= 4 ? special : 0);
    const int loadedDifficulty =
        LoadDifficultyFromIni();

    g_difficulty.store(
        loadedDifficulty,
        std::memory_order_relaxed
    );

    {
        wchar_t buffer[128]{};

        swprintf_s(
            buffer,
            L"Difficulty loaded: %d / %s",
            loadedDifficulty,
            GetDifficultyName(loadedDifficulty)
        );

        Log(buffer);
    }

    // --------------------------------------------------------
    // GAME BASE / BACKEND HOOKS
    // --------------------------------------------------------

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(
            GetModuleHandleW(nullptr)
        );

#ifdef _DEBUG
    LogAddress(L"EXE Base", base);
#endif

    if (base == 0)
    {
        Log(L"ERROR: Could not get EXE base.");
        return 0;
    }

    const uintptr_t modeHookAddress =
        base + OFFSET_MODE_HOOK;

    const uintptr_t finalAiHookAddress =
        base + OFFSET_FINAL_AI_HOOK;

#ifdef _DEBUG
    LogAddress(L"Treasure mode hook", modeHookAddress);
    LogAddress(L"Final AI resolver hook", finalAiHookAddress);
#endif

    if (!MemoryMatches(
            modeHookAddress,
            MODE_HOOK_BYTES,
            sizeof(MODE_HOOK_BYTES)
        ))
    {
        Log(L"ERROR: Treasure mode hook signature mismatch. Wrong Tekken 7 executable/version?");
        return 0;
    }

    if (!MemoryMatches(
            finalAiHookAddress,
            FINAL_AI_HOOK_BYTES,
            sizeof(FINAL_AI_HOOK_BYTES)
        ))
    {
        Log(L"ERROR: Final AI hook signature mismatch. Wrong Tekken 7 executable/version?");
        return 0;
    }

    g_modeHook =
        safetyhook::create_mid(
            reinterpret_cast<void*>(modeHookAddress),
            GameModeHook
        );

    if (!g_modeHook)
    {
        Log(L"ERROR: Treasure mode hook FAILED.");
        return 0;
    }

#ifdef _DEBUG
    Log(L"Treasure mode hook installed.");
#endif

    g_finalAiHook =
        safetyhook::create_mid(
            reinterpret_cast<void*>(finalAiHookAddress),
            FinalAiResolverHook
        );

    if (!g_finalAiHook)
    {
        Log(L"ERROR: Final AI resolver hook FAILED.");
        return 0;
    }

#ifdef _DEBUG
    Log(L"Final AI resolver hook installed.");
#endif

    // --------------------------------------------------------
    // XINPUT
    // --------------------------------------------------------

    InstallTreasureHooks(base);

    if (InitializeXInput())
    {
#ifdef _DEBUG
        Log(L"XInput initialized.");
#endif
    }
    else
    {
        Log(L"ERROR: XInput initialization FAILED. Keyboard Z/X still works.");
    }

    // --------------------------------------------------------
    // OPTIONS DETECTOR
    // --------------------------------------------------------

    // The XINPUT proxy can load very early. Give UE4 a moment to finish its
    // initial startup allocations before starting the background UI scan.
    Sleep(3000);

    if (OptionsDetectorInitialize(Log))
    {
#ifdef _DEBUG
        Log(L"Options detector initialized.");
#endif
    }
    else
    {
        Log(L"ERROR: Options detector initialization FAILED.");
    }

    // --------------------------------------------------------
    // OVERLAY
    // --------------------------------------------------------

    if (OverlayInitialize(
            g_hModule,
            &g_inOptions,
            &g_difficulty,
            &g_rounds, &g_specialFights, &g_selectedRow,
            Log
        ))
    {
#ifdef _DEBUG
        Log(L"Overlay hooks installed.");
#endif
    }
    else
    {
        Log(L"ERROR: Overlay initialization FAILED.");
    }

#ifdef _DEBUG
    Log(L"Initialization finished.");
#else
    Log(L"Ready.");
#endif

    HANDLE inputThread =
        CreateThread(
            nullptr,
            0,
            InputThread,
            nullptr,
            0,
            nullptr
        );

    if (inputThread != nullptr)
    {
#ifdef _DEBUG
        Log(L"Input thread started.");
#endif
    }
    else
    {
        Log(L"ERROR: Input thread creation FAILED.");
    }

    // ========================================================
    // OPTIONS STATE LOOP
    // ========================================================

    bool inOptions =
        false;

    ULONGLONG nextOptionsScan =
        0;

    while (g_running.load(
        std::memory_order_relaxed
    ))
    {
        const ULONGLONG now =
            GetTickCount64();

        if (now >= nextOptionsScan)
        {
            const bool newInOptions =
                OptionsDetectorPoll();

            if (newInOptions != inOptions)
            {
                inOptions =
                    newInOptions;

                g_inOptions.store(
                    inOptions,
                    std::memory_order_relaxed
                );

#ifdef _DEBUG
                DebugLogOptionsTransition(inOptions);
#endif

                if (!inOptions)
                {
                    // Same policy as SC6: save on EVERY Options exit.
                    if (!SaveDifficultyToIni())
                    {
                        Log(L"ERROR: Could not save INI.");
                    }
                }
            }

            nextOptionsScan =
                now + OPTIONS_SCAN_MS;
        }

        Sleep(INPUT_POLL_MS);
    }

    g_inOptions.store(
        false,
        std::memory_order_relaxed
    );

    g_isTreasure.store(
        false,
        std::memory_order_relaxed
    );

    if (inputThread != nullptr)
    {
        WaitForSingleObject(
            inputThread,
            1000
        );

        CloseHandle(inputThread);
        inputThread = nullptr;
    }

    OverlayShutdown();
    OptionsDetectorShutdown();

    g_initialRoundHook.reset(); g_roundHook.reset(); g_specialHook.reset();
    g_finalAiHook.reset();
    g_modeHook.reset();

    if (g_xInputModule != nullptr)
    {
        FreeLibrary(g_xInputModule);
        g_xInputModule = nullptr;
        g_xInputGetState = nullptr;
    }

    return 0;
}

// ============================================================
// DLL ENTRY
// ============================================================

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD ulReasonForCall,
    LPVOID
)
{
    switch (ulReasonForCall)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_hModule =
            hModule;

        g_running.store(
            true,
            std::memory_order_relaxed
        );

        g_inOptions.store(
            false,
            std::memory_order_relaxed
        );

        g_isTreasure.store(
            false,
            std::memory_order_relaxed
        );

        DisableThreadLibraryCalls(hModule);

        HANDLE thread =
            CreateThread(
                nullptr,
                0,
                MainThread,
                nullptr,
                0,
                nullptr
            );

        if (thread != nullptr)
        {
            CloseHandle(thread);
        }

        break;
    }

    case DLL_PROCESS_DETACH:
    {
        g_running.store(
            false,
            std::memory_order_relaxed
        );

        break;
    }

    default:
        break;
    }

    return TRUE;
}
