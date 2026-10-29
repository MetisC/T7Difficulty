#pragma once
namespace TreasurePolicy
{
constexpr bool ValidRounds(int rounds) { return rounds >= 0 && rounds <= 5; }
constexpr int RoundValue(int configured, int original, unsigned battleMode)
{
    return battleMode == 0x0B && original == 2 && ValidRounds(configured) && configured != 0 ? configured : original;
}
// Disabled uses 4 because 2 is already No Double Damage.
constexpr bool Suppress(int policy, int type)
{
    return (policy == 1 && type == 5) || (policy == 2 && type == 4) ||
        (policy == 3 && (type == 4 || type == 5)) ||
        (policy == 4 && (type == 1 || type == 2 || type == 4 || type == 5 || type == 6 || type == 7));
}
constexpr int NextSpecial(int current, int direction)
{
    return (current + direction + 5) % 5;
}
}
