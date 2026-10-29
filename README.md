# T7Difficulty

A lightweight, configurable **TEKKEN 7 Treasure Battle** mod. Adjust the CPU difficulty, the number of rounds required to win, and which special battles can appear — **without replacing Treasure Battle or installing a new AI system**.

## Features

### CPU difficulty

Choose from **Default, Easy, Medium, Hard, Very Hard, or Ultra Hard**.

**Default** preserves the original Treasure Battle difficulty scaling. The other settings use the game's existing AI difficulty profiles.

### Rounds to win

Choose **Default** (the game's usual two rounds) or **1–5** rounds required to win a match in Treasure Battle.

### Special battles

Choose which types of special encounters to suppress:

| Setting | Effect |
| --- | --- |
| **Vanilla** | Original Treasure Battle encounter selection |
| **No Turbo** | Suppress Turbo Battle |
| **No Double Damage** | Suppress Double Damage Battle |
| **No Turbo & Double Damage** | Suppress both of those battle types |
| **Disabled** | Suppress the identified special encounters: bosses, Double Damage, Turbo, Aerial Juggle, and Heist |

Suppressed encounters are converted to normal fights. The mod does not directly modify character selection, ranks, or reward logic; however, disabling special encounters may also remove opportunities for rewards tied to those encounters.

## In-game controls

Open the game's main **Options** screen to display the compact D3D11 overlay with three rows: **CPU Difficulty**, **Rounds**, and **Special Battles**.

- **L3 / TAB**: select the next row.
- **L1 / LB / Z**: previous value.
- **R1 / RB / X**: next value.

Changes are saved to `T7Difficulty.ini` when leaving Options. Input is accepted only while the game window is focused and Options is open.

## Installation

**Requirements:** Windows x64, TEKKEN 7 with a compatible executable, and (to build from source) Visual Studio 2022 with the Desktop C++ workload, MSVC v143 and Windows SDK.

1. Build `T7Difficulty.sln` in **x64 Release** (or x64 Debug for diagnostics).
2. Find the folder containing `TekkenGame-Win64-Shipping.exe`, normally `TEKKEN 7/TekkenGame/Binaries/Win64/`.
3. Back up any existing `XINPUT1_3.dll` in that folder.
4. Place the generated `XINPUT1_3.dll` alongside the game executable and launch the game.
5. Open **Options** to configure the three settings.

The build also produces `T7Difficulty.dll`, an alternative manual-injection filename for the same mod. **Do not load both DLLs at once.** See [BUILD_AND_INSTALL.txt](BUILD_AND_INSTALL.txt) for full troubleshooting and verification notes.

## Configuration

```ini
[Difficulty]
TreasureBattle=0

[TreasureBattle]
Rounds=0
SpecialFights=0
```

| Key | Values |
| --- | --- |
| `[Difficulty] TreasureBattle` | `0` Default, `1` Easy, `2` Medium, `3` Hard, `4` Very Hard, `5` Ultra Hard |
| `[TreasureBattle] Rounds` | `0` Default, `1–5` rounds to win |
| `[TreasureBattle] SpecialFights` | `0` Vanilla, `1` No Turbo, `2` No Double Damage, `3` No Turbo & Double Damage, `4` Disabled |

Missing settings default to their original (Vanilla) behavior, so existing difficulty-only INI files remain compatible.

## Executable compatibility

This mod uses runtime instruction hooks and **does not patch the game executable on disk**.

- The original CPU difficulty hooks validate their expected instruction bytes.
- The newer rounds and special-battle hooks target a **specific 2021 x64 executable** and validate its build and instruction context before installation.
- If the additional hooks cannot be safely installed, the extensions remain disabled while the original difficulty functionality is retained where its own hooks are compatible.
- No Steam client or Ghost Battle DLL is required to run the mod.

**Not a universal TEKKEN 7 patch:** the newer features are not guaranteed to work on the final Steam 2023 build or on other executables without porting and verification. Check `T7Difficulty.log` for hook installation diagnostics.

**Testing status:** the new functionality has static checks and documented reverse-engineering validation, but its in-game behavior — especially first/subsequent match rounds and special-encounter transitions — still requires runtime testing. See [BUILD_AND_INSTALL.txt](BUILD_AND_INSTALL.txt) for the test checklist.

## Credits and acknowledgements

Special thanks to **[NobodyCatchMe-AZ92](https://github.com/NobodyCatchMe-AZ92)**, creator of **[T7 Ghost AI + Ghost Battle Mode](https://github.com/NobodyCatchMe-AZ92/T7-Ghost-AI)**.

Their extensive reverse-engineering work on TEKKEN 7's Treasure Battle mechanics provided valuable technical insights for the **round-count and special-fight options** implemented here.

T7Difficulty is an independent project. It does **not** require, include, or replace the Ghost Battle mod or its AI system.

The referenced project is distributed under the [MIT License](https://github.com/NobodyCatchMe-AZ92/T7-Ghost-AI/blob/main/LICENSE), copyright © 2026 NobodyCatchMe-AZ92. When reusing code from that project, retain the applicable copyright and permission notices as required by its license.

## Disclaimer

Unofficial fan-made modification, not affiliated with or endorsed by Bandai Namco Entertainment.
