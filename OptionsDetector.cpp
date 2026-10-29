// OptionsDetector.cpp
#include "pch.h"
#include "OptionsDetector.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{
    // ============================================================
    // TEKKEN 7 - OPTIONS DETECTOR
    // ============================================================
    //
    // Final runtime signature recovered from the live Options UI objects.
    // No localized text is used by the final detector.
    //
    // A live F3 owner contains the internal Scaleform path at +0x228:
    //
    //   mc.List1_Panel_mc.Panel6.OptionName_mc.DT_OptionName_mc
    //
    // Cross-cycle structural fields (two independent Options opens):
    //
    //   owner +000 = exe + 3485EC2
    //   owner +008 -> object whose +000 = exe + 2E9B3A8
    //   owner +010 = 0
    //   owner +0A0 = exe + 3485EC2
    //   owner +0B0 = 0
    //   owner +170 = exe + 2EB9810
    //   owner +178 = FFFFFFFF00000001
    //   owner +190 = 1
    //   owner +198 = 4
    //   owner +1B8 -> object whose +000 = exe + 2EB9848
    //   owner +1C0 -> localized live OptionName text
    //   owner +1E8 = 1
    //   owner +1F0 = 4
    //   owner +1F8 = 0
    //   owner +260 -> object whose +000 = exe + 2EB1758
    //   owner +268 = FFFFFFFF00000001
    //   owner +278 -> object whose +000 = exe + 2E9B3A8
    //   owner +280 = exe + 3485EC2
    //   owner +290 = 0
    //   owner +298 -> object whose +000 = exe + 2EABBE0
    //   owner +2D0 = exe + 3485EC2
    //   owner +2E0 = 0
    //   owner +2E8 = 0
    //
    // We deliberately validate only a strong subset. The internal path is
    // already highly specific; the extra structural checks make stale or
    // unrelated copies extremely unlikely to pass.
    // ============================================================

    constexpr uintptr_t TAG_3485EC2 =
        0x3485EC2;

    constexpr uintptr_t TAG_2EB9810 =
        0x2EB9810;

    constexpr uintptr_t TAG_2E9B3A8 =
        0x2E9B3A8;

    constexpr uintptr_t TAG_2EB9848 =
        0x2EB9848;

    constexpr uintptr_t TAG_2EB1758 =
        0x2EB1758;

    constexpr uintptr_t TAG_2EABBE0 =
        0x2EABBE0;

    constexpr uintptr_t INTERNAL_PATH_OFFSET =
        0x228;

    constexpr char INTERNAL_PATH[] =
        "mc.List1_Panel_mc.Panel6.OptionName_mc.DT_OptionName_mc";

    constexpr size_t INTERNAL_PATH_LENGTH =
        sizeof(INTERNAL_PATH) - 1;

    constexpr ULONGLONG OPTIONS_EXIT_CONFIRM_MS =
        500;

    OptionsDetectorLogFn g_log =
        nullptr;

    uintptr_t g_exeBase =
        0;

    uintptr_t g_minApplicationAddress =
        0;

    uintptr_t g_maxApplicationAddress =
        0;

    uintptr_t g_cachedOwner =
        0;

    bool g_initialized =
        false;

    bool g_inside =
        false;

    int g_lastCount =
        0;

    int g_baselineCount =
        0;

    ULONGLONG g_outsideCandidateSince =
        0;

    constexpr const wchar_t* DETECTOR_FAMILY =
        L"Language-independent";

    constexpr const wchar_t* DETECTOR_PATTERN =
        L"Scaleform Options owner +0x228";

    // ============================================================
    // LOGGING
    // ============================================================

    void LogMessage(
        const wchar_t* text
    )
    {
        if (g_log != nullptr)
        {
            g_log(text);
        }
    }

    // ============================================================
    // MEMORY FILTER
    // ============================================================

    bool IsReadableProtection(
        DWORD protection
    )
    {
        if ((protection & PAGE_GUARD) != 0 ||
            (protection & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD baseProtection =
            protection & 0xFF;

        switch (baseProtection)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    bool IsHeapCandidateProtection(
        DWORD protection
    )
    {
        if ((protection & PAGE_GUARD) != 0 ||
            (protection & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD baseProtection =
            protection & 0xFF;

        switch (baseProtection)
        {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    bool IsCommittedReadableRange(
        uintptr_t address,
        size_t size
    )
    {
        if (address < 0x10000 ||
            size == 0)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi{};

        if (VirtualQuery(
                reinterpret_cast<const void*>(address),
                &mbi,
                sizeof(mbi)
            ) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT ||
            !IsReadableProtection(mbi.Protect))
        {
            return false;
        }

        const uintptr_t regionBase =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);

        const uintptr_t regionEnd =
            regionBase +
            static_cast<uintptr_t>(mbi.RegionSize);

        const uintptr_t requestedEnd =
            address +
            static_cast<uintptr_t>(size);

        return
            requestedEnd >= address &&
            requestedEnd <= regionEnd;
    }

    // ============================================================
    // SAFE READS
    // ============================================================

    bool SafeReadQword(
        uintptr_t address,
        uintptr_t* value
    )
    {
        if (address < 0x10000 ||
            value == nullptr)
        {
            return false;
        }

        __try
        {
            *value =
                *reinterpret_cast<const uintptr_t*>(address);

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeReadWord(
        uintptr_t address,
        wchar_t* value
    )
    {
        if (address < 0x10000 ||
            value == nullptr)
        {
            return false;
        }

        __try
        {
            *value =
                *reinterpret_cast<const wchar_t*>(address);

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeBytesEqual(
        uintptr_t address,
        const char* expected,
        size_t length
    )
    {
        if (address < 0x10000 ||
            expected == nullptr ||
            length == 0)
        {
            return false;
        }

        __try
        {
            return
                std::memcmp(
                    reinterpret_cast<const void*>(address),
                    expected,
                    length
                ) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool PointsToTag(
        uintptr_t pointer,
        uintptr_t tagOffset
    )
    {
        if (pointer < 0x10000)
        {
            return false;
        }

        uintptr_t value =
            0;

        return
            SafeReadQword(pointer, &value) &&
            value == g_exeBase + tagOffset;
    }

    // We intentionally do NOT compare the localized label. We only require
    // owner+1C0 to still point to a plausible live, NUL-terminated UTF-16
    // string. This helps reject stale/freed owner memory after Options closes.
    bool IsPlausibleLiveWideString(
        uintptr_t address
    )
    {
        if (!IsCommittedReadableRange(
                address,
                sizeof(wchar_t)
            ))
        {
            return false;
        }

        int nonZeroCharacters =
            0;

        for (int i = 0;
             i < 64;
             ++i)
        {
            wchar_t ch =
                0;

            if (!SafeReadWord(
                    address +
                    static_cast<uintptr_t>(i) *
                    sizeof(wchar_t),
                    &ch
                ))
            {
                return false;
            }

            if (ch == 0)
            {
                return
                    nonZeroCharacters >= 2;
            }

            ++nonZeroCharacters;
        }

        return false;
    }

    // ============================================================
    // LIVE OPTIONS OWNER VALIDATION
    // ============================================================

    bool IsValidOptionsOwner(
        uintptr_t owner
    )
    {
        if (owner < 0x10000 ||
            g_exeBase == 0)
        {
            return false;
        }

        if (!IsCommittedReadableRange(
                owner,
                0x300
            ))
        {
            return false;
        }

        uintptr_t value =
            0;

        // Strong fixed tag at object start.
        if (!SafeReadQword(owner + 0x000, &value) ||
            value != g_exeBase + TAG_3485EC2)
        {
            return false;
        }

        // Independent nested object family.
        if (!SafeReadQword(owner + 0x008, &value) ||
            !PointsToTag(value, TAG_2E9B3A8))
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x010, &value) ||
            value != 0)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x0A0, &value) ||
            value != g_exeBase + TAG_3485EC2)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x170, &value) ||
            value != g_exeBase + TAG_2EB9810)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x178, &value) ||
            value != static_cast<uintptr_t>(
                0xFFFFFFFF00000001ull
            ))
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x190, &value) ||
            value != 1)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x198, &value) ||
            value != 4)
        {
            return false;
        }

        // The localized OptionName text must still be a live UTF-16 string.
        uintptr_t localizedText =
            0;

        if (!SafeReadQword(
                owner + 0x1C0,
                &localizedText
            ) ||
            !IsPlausibleLiveWideString(
                localizedText
            ))
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x1E8, &value) ||
            value != 1)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x1F0, &value) ||
            value != 4)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x1F8, &value) ||
            value != 0)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x260, &value) ||
            !PointsToTag(value, TAG_2EB1758))
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x278, &value) ||
            !PointsToTag(value, TAG_2E9B3A8))
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x280, &value) ||
            value != g_exeBase + TAG_3485EC2)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x290, &value) ||
            value != 0)
        {
            return false;
        }

        if (!SafeReadQword(owner + 0x298, &value) ||
            !PointsToTag(value, TAG_2EABBE0))
        {
            return false;
        }

        // The non-localized internal path is the final identity check.
        if (!SafeBytesEqual(
                owner + INTERNAL_PATH_OFFSET,
                INTERNAL_PATH,
                INTERNAL_PATH_LENGTH
            ))
        {
            return false;
        }

        return true;
    }

    // ============================================================
    // SCAN ONE HEAP REGION FOR THE INTERNAL PATH
    // ============================================================

    uintptr_t ScanRegionForOwner(
        uintptr_t regionBase,
        size_t regionSize
    )
    {
        if (regionBase < 0x10000 ||
            regionSize < INTERNAL_PATH_LENGTH)
        {
            return 0;
        }

        const auto* begin =
            reinterpret_cast<const unsigned char*>(regionBase);

        const unsigned char firstByte =
            static_cast<unsigned char>(INTERNAL_PATH[0]);

        __try
        {
            size_t offset =
                0;

            while (offset +
                   INTERNAL_PATH_LENGTH <=
                   regionSize)
            {
                const size_t remaining =
                    regionSize - offset;

                const void* found =
                    std::memchr(
                        begin + offset,
                        firstByte,
                        remaining -
                        INTERNAL_PATH_LENGTH +
                        1
                    );

                if (found == nullptr)
                {
                    break;
                }

                const auto* candidate =
                    reinterpret_cast<const unsigned char*>(found);

                const uintptr_t candidateAddress =
                    reinterpret_cast<uintptr_t>(candidate);

                if (std::memcmp(
                        candidate,
                        INTERNAL_PATH,
                        INTERNAL_PATH_LENGTH
                    ) == 0)
                {
                    if (candidateAddress >=
                        INTERNAL_PATH_OFFSET)
                    {
                        const uintptr_t owner =
                            candidateAddress -
                            INTERNAL_PATH_OFFSET;

                        if (IsValidOptionsOwner(owner))
                        {
                            return owner;
                        }
                    }
                }

                offset =
                    static_cast<size_t>(
                        candidate - begin
                    ) + 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }

        return 0;
    }

    // ============================================================
    // FULL PRIVATE-WRITABLE MEMORY SCAN
    // ============================================================

    uintptr_t FindOptionsOwner()
    {
        if (g_exeBase == 0 ||
            g_minApplicationAddress == 0 ||
            g_maxApplicationAddress == 0)
        {
            return 0;
        }

        uintptr_t address =
            g_minApplicationAddress;

        while (address <
            g_maxApplicationAddress)
        {
            MEMORY_BASIC_INFORMATION mbi{};

            const SIZE_T queried =
                VirtualQuery(
                    reinterpret_cast<const void*>(address),
                    &mbi,
                    sizeof(mbi)
                );

            if (queried == 0)
            {
                break;
            }

            const uintptr_t regionBase =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress);

            const size_t regionSize =
                static_cast<size_t>(mbi.RegionSize);

            if (mbi.State == MEM_COMMIT &&
                mbi.Type == MEM_PRIVATE &&
                IsHeapCandidateProtection(mbi.Protect))
            {
                const uintptr_t owner =
                    ScanRegionForOwner(
                        regionBase,
                        regionSize
                    );

                if (owner != 0)
                {
                    return owner;
                }
            }

            const uintptr_t next =
                regionBase +
                static_cast<uintptr_t>(mbi.RegionSize);

            if (next <= address)
            {
                break;
            }

            address =
                next;
        }

        return 0;
    }
}

// ============================================================
// INITIALIZE
// ============================================================

bool OptionsDetectorInitialize(
    OptionsDetectorLogFn logFn
)
{
    if (g_initialized)
    {
        return true;
    }

    g_log =
        logFn;

    g_exeBase =
        reinterpret_cast<uintptr_t>(
            GetModuleHandleW(nullptr)
        );

    if (g_exeBase == 0)
    {
        LogMessage(
            L"ERROR: T7 Options detector could not resolve EXE base."
        );

        return false;
    }

    SYSTEM_INFO systemInfo{};

    GetSystemInfo(
        &systemInfo
    );

    g_minApplicationAddress =
        reinterpret_cast<uintptr_t>(
            systemInfo.lpMinimumApplicationAddress
        );

    g_maxApplicationAddress =
        reinterpret_cast<uintptr_t>(
            systemInfo.lpMaximumApplicationAddress
        );

    if (g_minApplicationAddress == 0 ||
        g_maxApplicationAddress == 0 ||
        g_maxApplicationAddress <=
        g_minApplicationAddress)
    {
        LogMessage(
            L"ERROR: T7 Options detector could not resolve application address range."
        );

        g_exeBase =
            0;

        return false;
    }

    g_cachedOwner =
        FindOptionsOwner();

    g_inside =
        g_cachedOwner != 0;

    g_lastCount =
        g_inside ? 1 : 0;

    g_baselineCount =
        0;

    g_outsideCandidateSince =
        0;

#ifdef _DEBUG
    LogMessage(
        L"T7 Options detector initialized (internal Scaleform owner signature)."
    );
#endif

#ifdef _DEBUG
    {
        wchar_t buffer[256]{};

        swprintf_s(
            buffer,
            L"Options detector initial state: owner=0x%016llX | inside=%s",
            static_cast<unsigned long long>(g_cachedOwner),
            g_inside ? L"YES" : L"NO"
        );

        LogMessage(buffer);
    }
#endif

    g_initialized =
        true;

    return true;
}

// ============================================================
// POLL
// ============================================================

bool OptionsDetectorPoll()
{
    if (!g_initialized)
    {
        return false;
    }

    const ULONGLONG now =
        GetTickCount64();

    // Fast path: while Options is open, validate the cached owner only.
    if (g_cachedOwner != 0 &&
        IsValidOptionsOwner(g_cachedOwner))
    {
        g_lastCount =
            1;

        g_inside =
            true;

        g_outsideCandidateSince =
            0;

        return true;
    }

    g_cachedOwner =
        0;

    // The UI can destroy/recreate objects while navigating. Relocate the
    // live owner before treating a missing cached object as an exit.
    const uintptr_t foundOwner =
        FindOptionsOwner();

    if (foundOwner != 0)
    {
        g_cachedOwner =
            foundOwner;

        g_lastCount =
            1;

        g_inside =
            true;

        g_outsideCandidateSince =
            0;

        return true;
    }

    g_lastCount =
        0;

    if (!g_inside)
    {
        g_outsideCandidateSince =
            0;

        return false;
    }

    // Require the owner to remain absent before declaring Options closed.
    if (g_outsideCandidateSince == 0)
    {
        g_outsideCandidateSince =
            now;

        return true;
    }

    if (now -
        g_outsideCandidateSince <
        OPTIONS_EXIT_CONFIRM_MS)
    {
        return true;
    }

    g_inside =
        false;

    g_outsideCandidateSince =
        0;

    return false;
}

// ============================================================
// DIAGNOSTICS
// ============================================================

int OptionsDetectorLastCount()
{
    return g_lastCount;
}

int OptionsDetectorBaselineCount()
{
    return g_baselineCount;
}

const wchar_t* OptionsDetectorLanguage()
{
    return DETECTOR_FAMILY;
}

const wchar_t* OptionsDetectorPattern()
{
    return DETECTOR_PATTERN;
}

// ============================================================
// SHUTDOWN
// ============================================================

void OptionsDetectorShutdown()
{
    g_initialized =
        false;

    g_inside =
        false;

    g_lastCount =
        0;

    g_baselineCount =
        0;

    g_cachedOwner =
        0;

    g_exeBase =
        0;

    g_minApplicationAddress =
        0;

    g_maxApplicationAddress =
        0;

    g_outsideCandidateSince =
        0;

    g_log =
        nullptr;
}
