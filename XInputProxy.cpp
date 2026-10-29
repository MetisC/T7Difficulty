#include "pch.h"

#include <Windows.h>
#include <Xinput.h>

#include <cwchar>

// ============================================================
// T7 - XINPUT1_3 TRANSPARENT PROXY
//
// Esta misma DLL contiene:
//   - T7Difficulty
//   - hooks
//   - detector de Options
//   - overlay
//   - input
//   - proxy XINPUT1_3
//
// IMPORTANTE:
//
// NO hay un segundo DllMain.
// NO carga una segunda T7Difficulty.dll.
//
// Las funciones exportadas publicamente como:
//   XInputGetState
//   XInputSetState
//   etc.
//
// tienen nombres INTERNOS Proxy_* para no entrar en conflicto
// con las declaraciones del SDK de Windows / Xinput.h.
//
// El fichero XINPUT1_3.def se encarga de exportarlas hacia fuera
// con los nombres originales.
//
// El estado de los mandos NO se modifica.
// Todo se reenvia al XINPUT1_3.dll REAL de System32.
// ============================================================

namespace
{
    // ========================================================
    // REAL SYSTEM XINPUT
    // ========================================================

    HMODULE g_realXInput =
        nullptr;

    INIT_ONCE g_xinputProxyInitOnce =
        INIT_ONCE_STATIC_INIT;

    // ========================================================
    // FUNCTION TYPES
    // ========================================================

    using FnXInputEnable =
        void(WINAPI*)(
            BOOL
            );

    using FnXInputGetBatteryInformation =
        DWORD(WINAPI*)(
            DWORD,
            BYTE,
            XINPUT_BATTERY_INFORMATION*
            );

    using FnXInputGetCapabilities =
        DWORD(WINAPI*)(
            DWORD,
            DWORD,
            XINPUT_CAPABILITIES*
            );

    using FnXInputGetDSoundAudioDeviceGuids =
        DWORD(WINAPI*)(
            DWORD,
            GUID*,
            GUID*
            );

    using FnXInputGetKeystroke =
        DWORD(WINAPI*)(
            DWORD,
            DWORD,
            XINPUT_KEYSTROKE*
            );

    using FnXInputGetState =
        DWORD(WINAPI*)(
            DWORD,
            XINPUT_STATE*
            );

    using FnXInputSetState =
        DWORD(WINAPI*)(
            DWORD,
            XINPUT_VIBRATION*
            );

    // ========================================================
    // UNDOCUMENTED XINPUT1_3 ORDINALS
    // ========================================================

    // Ordinal 100
    using FnXInputGetStateEx =
        DWORD(WINAPI*)(
            DWORD,
            XINPUT_STATE*
            );

    // Ordinal 101
    using FnXInputWaitForGuideButton =
        DWORD(WINAPI*)(
            DWORD,
            DWORD,
            LPVOID
            );

    // Ordinal 102
    using FnXInputCancelGuideButtonWait =
        DWORD(WINAPI*)(
            DWORD
            );

    // Ordinal 103
    using FnXInputPowerOffController =
        DWORD(WINAPI*)(
            DWORD
            );

    // ========================================================
    // REAL FUNCTION POINTERS
    // ========================================================

    FnXInputEnable
        g_realXInputEnable =
        nullptr;

    FnXInputGetBatteryInformation
        g_realXInputGetBatteryInformation =
        nullptr;

    FnXInputGetCapabilities
        g_realXInputGetCapabilities =
        nullptr;

    FnXInputGetDSoundAudioDeviceGuids
        g_realXInputGetDSoundAudioDeviceGuids =
        nullptr;

    FnXInputGetKeystroke
        g_realXInputGetKeystroke =
        nullptr;

    FnXInputGetState
        g_realXInputGetState =
        nullptr;

    FnXInputSetState
        g_realXInputSetState =
        nullptr;

    FnXInputGetStateEx
        g_realXInputGetStateEx =
        nullptr;

    FnXInputWaitForGuideButton
        g_realXInputWaitForGuideButton =
        nullptr;

    FnXInputCancelGuideButtonWait
        g_realXInputCancelGuideButtonWait =
        nullptr;

    FnXInputPowerOffController
        g_realXInputPowerOffController =
        nullptr;

    // ========================================================
    // BUILD ABSOLUTE SYSTEM32 PATH
    // ========================================================

    bool BuildRealXInputPath(
        wchar_t* destination,
        size_t destinationCount
    )
    {
        if (destination == nullptr ||
            destinationCount == 0)
        {
            return false;
        }

        const UINT length =
            GetSystemDirectoryW(
                destination,
                static_cast<UINT>(
                    destinationCount
                    )
            );

        if (length == 0 ||
            length >= destinationCount)
        {
            return false;
        }

        constexpr wchar_t suffix[] =
            L"\\XINPUT1_3.dll";

        const size_t suffixLength =
            (sizeof(suffix) /
                sizeof(suffix[0])) - 1;

        if (static_cast<size_t>(
            length
            ) +
            suffixLength +
            1 >
            destinationCount)
        {
            return false;
        }

        wcscat_s(
            destination,
            destinationCount,
            suffix
        );

        return true;
    }

    // ========================================================
    // RESOLVE NAMED EXPORT
    // ========================================================

    template<typename T>
    void ResolveNamedExport(
        T& destination,
        const char* name
    )
    {
        if (g_realXInput ==
            nullptr)
        {
            destination =
                nullptr;

            return;
        }

        destination =
            reinterpret_cast<T>(
                GetProcAddress(
                    g_realXInput,
                    name
                )
                );
    }

    // ========================================================
    // RESOLVE ORDINAL EXPORT
    // ========================================================

    template<typename T>
    void ResolveOrdinalExport(
        T& destination,
        WORD ordinal
    )
    {
        if (g_realXInput ==
            nullptr)
        {
            destination =
                nullptr;

            return;
        }

        destination =
            reinterpret_cast<T>(
                GetProcAddress(
                    g_realXInput,
                    MAKEINTRESOURCEA(
                        ordinal
                    )
                )
                );
    }

    // ========================================================
    // LOAD REAL XINPUT
    // ========================================================

    BOOL CALLBACK InitializeRealXInputProxy(
        PINIT_ONCE,
        PVOID,
        PVOID*
    )
    {
        wchar_t path[MAX_PATH]{};

        if (!BuildRealXInputPath(
            path,
            MAX_PATH
        ))
        {
            return TRUE;
        }

        // ----------------------------------------------------
        // VERY IMPORTANT
        //
        // Absolute path:
        //
        // C:\Windows\System32\XINPUT1_3.dll
        //
        // Never:
        //
        // LoadLibraryW(L"XINPUT1_3.dll")
        //
        // because that could resolve back to this proxy.
        // ----------------------------------------------------

        g_realXInput =
            LoadLibraryW(
                path
            );

        if (g_realXInput ==
            nullptr)
        {
            return TRUE;
        }

        // ====================================================
        // NAMED EXPORTS
        // ====================================================

        ResolveNamedExport(
            g_realXInputEnable,
            "XInputEnable"
        );

        ResolveNamedExport(
            g_realXInputGetBatteryInformation,
            "XInputGetBatteryInformation"
        );

        ResolveNamedExport(
            g_realXInputGetCapabilities,
            "XInputGetCapabilities"
        );

        ResolveNamedExport(
            g_realXInputGetDSoundAudioDeviceGuids,
            "XInputGetDSoundAudioDeviceGuids"
        );

        ResolveNamedExport(
            g_realXInputGetKeystroke,
            "XInputGetKeystroke"
        );

        ResolveNamedExport(
            g_realXInputGetState,
            "XInputGetState"
        );

        ResolveNamedExport(
            g_realXInputSetState,
            "XInputSetState"
        );

        // ====================================================
        // ORDINAL-ONLY EXPORTS
        // ====================================================

        ResolveOrdinalExport(
            g_realXInputGetStateEx,
            100
        );

        ResolveOrdinalExport(
            g_realXInputWaitForGuideButton,
            101
        );

        ResolveOrdinalExport(
            g_realXInputCancelGuideButtonWait,
            102
        );

        ResolveOrdinalExport(
            g_realXInputPowerOffController,
            103
        );

        return TRUE;
    }

    // ========================================================
    // THREAD-SAFE LAZY INITIALIZATION
    // ========================================================

    void EnsureRealXInputProxy()
    {
        InitOnceExecuteOnce(
            &g_xinputProxyInitOnce,
            InitializeRealXInputProxy,
            nullptr,
            nullptr
        );
    }
}

// ============================================================
// PUBLIC WRAPPERS
//
// Los nombres internos llevan Proxy_.
//
// El .DEF los exportara hacia fuera con los nombres normales.
//
// NO MODIFICAMOS ABSOLUTAMENTE NADA DEL MANDO.
// ============================================================

// ============================================================
// XInputEnable
// ============================================================

extern "C"
void WINAPI Proxy_XInputEnable(
    BOOL enable
)
{
    EnsureRealXInputProxy();

    if (g_realXInputEnable !=
        nullptr)
    {
        g_realXInputEnable(
            enable
        );
    }
}

// ============================================================
// XInputGetBatteryInformation
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetBatteryInformation(
    DWORD userIndex,
    BYTE devType,
    XINPUT_BATTERY_INFORMATION* batteryInformation
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetBatteryInformation ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetBatteryInformation(
            userIndex,
            devType,
            batteryInformation
        );
}

// ============================================================
// XInputGetCapabilities
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetCapabilities(
    DWORD userIndex,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetCapabilities ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetCapabilities(
            userIndex,
            flags,
            capabilities
        );
}

// ============================================================
// XInputGetDSoundAudioDeviceGuids
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(
    DWORD userIndex,
    GUID* renderGuid,
    GUID* captureGuid
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetDSoundAudioDeviceGuids ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetDSoundAudioDeviceGuids(
            userIndex,
            renderGuid,
            captureGuid
        );
}

// ============================================================
// XInputGetKeystroke
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetKeystroke(
    DWORD userIndex,
    DWORD reserved,
    XINPUT_KEYSTROKE* keystroke
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetKeystroke ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetKeystroke(
            userIndex,
            reserved,
            keystroke
        );
}

// ============================================================
// XInputGetState
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetState(
    DWORD userIndex,
    XINPUT_STATE* state
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetState ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetState(
            userIndex,
            state
        );
}

// ============================================================
// XInputSetState
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration
)
{
    EnsureRealXInputProxy();

    if (g_realXInputSetState ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputSetState(
            userIndex,
            vibration
        );
}

// ============================================================
// ORDINAL 100
//
// XInputGetStateEx
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputGetStateEx(
    DWORD userIndex,
    XINPUT_STATE* state
)
{
    EnsureRealXInputProxy();

    if (g_realXInputGetStateEx ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputGetStateEx(
            userIndex,
            state
        );
}

// ============================================================
// ORDINAL 101
//
// XInputWaitForGuideButton
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputWaitForGuideButton(
    DWORD userIndex,
    DWORD flags,
    LPVOID unknown
)
{
    EnsureRealXInputProxy();

    if (g_realXInputWaitForGuideButton ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputWaitForGuideButton(
            userIndex,
            flags,
            unknown
        );
}

// ============================================================
// ORDINAL 102
//
// XInputCancelGuideButtonWait
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputCancelGuideButtonWait(
    DWORD userIndex
)
{
    EnsureRealXInputProxy();

    if (g_realXInputCancelGuideButtonWait ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputCancelGuideButtonWait(
            userIndex
        );
}

// ============================================================
// ORDINAL 103
//
// XInputPowerOffController
// ============================================================

extern "C"
DWORD WINAPI Proxy_XInputPowerOffController(
    DWORD userIndex
)
{
    EnsureRealXInputProxy();

    if (g_realXInputPowerOffController ==
        nullptr)
    {
        return
            ERROR_DEVICE_NOT_CONNECTED;
    }

    return
        g_realXInputPowerOffController(
            userIndex
        );
}

// ============================================================
// ORDINAL 1 / "DllMain" EXPORT COMPATIBILITY
//
// OJO:
//
// ESTO NO ES EL DllMain REAL DE NUESTRA DLL.
//
// El DllMain real sigue estando exclusivamente en:
//
//     dllmain.cpp
//
// La DLL original de Microsoft expone tambien un simbolo llamado
// "DllMain" como ordinal 1.
//
// Nuestro XINPUT1_3.def hace:
//
//     DllMain=ProxyDllMainExport @1
//
// para reproducir esa superficie de exports sin crear un segundo
// entry point.
// ============================================================

extern "C"
BOOL WINAPI ProxyDllMainExport(
    HINSTANCE,
    DWORD,
    LPVOID
)
{
    return TRUE;
}