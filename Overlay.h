#pragma once

#include <Windows.h>
#include <atomic>

using OverlayLogFn = void (*)(const wchar_t* text);

bool OverlayInitialize(
    HMODULE ownerModule,
    const std::atomic<bool>* inOptions,
    const std::atomic<int>* difficulty,
    const std::atomic<int>* rounds,
    const std::atomic<int>* specialFights,
    const std::atomic<int>* selectedRow,
    OverlayLogFn logFn
);

void OverlayShutdown();
