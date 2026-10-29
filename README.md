# T7Difficulty

A lightweight **TEKKEN 7** mod for choosing the CPU difficulty in **Treasure Battle** without replacing the mode, changing its opponent selection, or installing a different AI system.

## Features

- Choose **Default, Easy, Medium, Hard, Very Hard, or Ultra Hard** for Treasure Battle.
- **Default** leaves the game's original Treasure Battle difficulty scaling untouched.
- Small in-game **D3D11 overlay**, displayed while the main Options screen is open.
- Controller and keyboard navigation.
- Saves your selected difficulty to `T7Difficulty.ini`.

> **Version note:** The current `main` branch contains the difficulty selector. Round-count and special-battle controls are not yet present in the published `main` source. This README can be updated when those additions are merged.

## Installation

1. Build the solution in **Visual Studio 2022**, using **x64** (Debug or Release).
2. Locate the directory containing `TekkenGame-Win64-Shipping.exe`, usually `TEKKEN 7/TekkenGame/Binaries/Win64/`.
3. **Back up any existing `XINPUT1_3.dll`** in that directory.
4. Copy the generated `XINPUT1_3.dll` into the same directory.
5. Start the game, open **Options**, and use the on-screen selector.

Use **L1/LB** or **Z** to select the previous difficulty, and **R1/RB** or **X** to select the next one. Your choice is saved when you leave Options.

The mod also builds a `T7Difficulty.dll` for manual injection if the XInput proxy isn't loaded. See [BUILD_AND_INSTALL.txt](BUILD_AND_INSTALL.txt) for detailed build and troubleshooting instructions.

## Configuration

The mod creates/reads `T7Difficulty.ini` next to its loaded DLL:

```ini
[Difficulty]
TreasureBattle=0
```

Values: `0` Default, `1` Easy, `2` Medium, `3` Hard, `4` Very Hard, `5` Ultra Hard.

## Compatibility

This is a **version-sensitive** mod: it checks the original instructions at its hook locations and refuses incompatible hooks rather than patching unknown code. Compatibility with every TEKKEN 7 executable is **not** guaranteed. It does not modify the game executable on disk.

## Credits and acknowledgements

Special thanks to **[NobodyCatchMe-AZ92](https://github.com/NobodyCatchMe-AZ92)**, creator of **[T7 Ghost AI + Ghost Battle Mode](https://github.com/NobodyCatchMe-AZ92/T7-Ghost-AI)**.

Their reverse-engineering research into TEKKEN 7's Treasure Battle mechanics provided valuable technical insights for the development of the **round-count and special-fight options** for T7Difficulty.

T7Difficulty is an **independent project**. It does not require the Ghost Battle mod or its AI system.

The T7 Ghost AI project is distributed under the **MIT License**, copyright © 2026 NobodyCatchMe-AZ92. Consult [its license](https://github.com/NobodyCatchMe-AZ92/T7-Ghost-AI/blob/main/LICENSE) when reusing its code: copying substantial portions requires retaining the copyright and permission notices.

## Disclaimer

Unofficial fan-made modification. Not affiliated with or endorsed by Bandai Namco Entertainment.
