#pragma once

#include <Windows.h>

using OptionsDetectorLogFn = void (*)(const wchar_t* text);

// TEKKEN 7 language-independent Options detector.
//
// It looks for a runtime UI object containing the internal (non-localized)
// Scaleform path:
//
//   mc.List1_Panel_mc.Panel6.OptionName_mc.DT_OptionName_mc
//
// and validates the surrounding object with the structural fields recovered
// from the live Options captures.
bool OptionsDetectorInitialize(OptionsDetectorLogFn logFn);

// Poll from the worker thread. The detector caches the live Options owner,
// so while Options remains open this is only a handful of safe reads.
bool OptionsDetectorPoll();

// Diagnostics kept compatible with the original SC6 project/log code.
// LastCount is 1 when a validated live Options owner exists, otherwise 0.
// BaselineCount is 0 (outside Options).
int OptionsDetectorLastCount();
int OptionsDetectorBaselineCount();
const wchar_t* OptionsDetectorLanguage();
const wchar_t* OptionsDetectorPattern();

void OptionsDetectorShutdown();
