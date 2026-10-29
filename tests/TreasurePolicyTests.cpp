#include "../TreasurePolicy.h"
using namespace TreasurePolicy;
constexpr bool CheckPolicies()
{
    for (int type = -2; type <= 10; ++type)
    {
        if (Suppress(0, type)) return false;
        if (Suppress(1, type) != (type == 5)) return false;
        if (Suppress(2, type) != (type == 4)) return false;
        if (Suppress(3, type) != (type == 4 || type == 5)) return false;
        if (Suppress(4, type) != (type == 1 || type == 2 || type == 4 || type == 5 || type == 6 || type == 7)) return false;
    }
    for (int rounds = 0; rounds <= 5; ++rounds)
    {
        if (!ValidRounds(rounds)) return false;
        if (RoundValue(rounds, 2, true) != (rounds == 0 ? 2 : rounds)) return false;
        if (RoundValue(rounds, 2, false) != 2) return false;
        if (RoundValue(rounds, 3, true) != 3) return false;
    }
    if (RoundValue(-1, 2, true) != 2 || RoundValue(6, 2, true) != 2) return false;
    for (int policy = 0; policy <= 4; ++policy)
    {
        if (NextSpecial(policy, 1) != (policy + 1) % 5) return false;
        if (NextSpecial(policy, -1) != (policy + 4) % 5) return false;
    }
    return !ValidRounds(-1) && !ValidRounds(6);
}
static_assert(CheckPolicies(), "Treasure policies must preserve vanilla and filter only identified types");
