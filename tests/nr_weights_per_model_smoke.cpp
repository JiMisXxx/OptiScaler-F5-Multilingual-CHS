// x64 MSVC: cl /std:c++20 /EHsc /W4 /WX /O2 tests/nr_weights_per_model_smoke.cpp
// Neural Rendering model weights per model (OptiScaler/dlssnr/DlssNr_WeightsRules.h): migration of the old single
// WeightsSet, which list a folder appears in, the "Not used now" line, the rebuild key.
#include "../OptiScaler/dlssnr/DlssNr_WeightsRules.h"

#include <cstdio>

using namespace DlssNr::WeightsRules;

static int failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main()
{
    // File names tell the model; the dot keeps a longer resource name apart; case is ignored
    Models m;
    NoteFile(m, "WEIGHTS_HT.1A2B3C4D.bin");
    CHECK(m.a && !m.b);
    NoteFile(m, "WEIGHTS_HTX.1A2B3C4D.bin");
    CHECK(!m.b);
    NoteFile(m, "weights_cc_small_fp8.00000001.bin");
    CHECK(m.a && m.b);
    Models none;
    NoteFile(none, "readme.txt");
    NoteFile(none, "WEIGHTS_HT");
    CHECK(!none.a && !none.b);

    // Migration: both models take the old set only where the folder has that model's file
    Pair out;
    CHECK(Migrate("Strong", false, { true, true }, out) && out.a == "Strong" && out.b == "Strong");
    CHECK(Migrate("Strong", false, { false, true }, out) && out.a.empty() && out.b == "Strong");
    CHECK(Migrate("Strong", false, { true, false }, out) && out.a == "Strong" && out.b.empty());
    CHECK(Migrate("Strong", false, { false, false }, out) && out.a.empty() && out.b.empty()); // migrated, to stock
    CHECK(!Migrate("Strong", true, { true, true }, out));  // a new key is in the file: nothing to migrate
    CHECK(!Migrate("", false, { true, true }, out));       // no old set

    // Lists: a folder shows only in the list of a model it has a file for; both models -> both lists
    const std::vector<Set> sets = { { "BothSet", { true, true } }, { "OnlyA", { true, false } },
                                    { "OnlyB", { false, true } }, { "Empty", {} } };
    const auto listA = ListFor(sets, true);
    const auto listB = ListFor(sets, false);
    CHECK(listA.size() == 2 && listA[0] == "BothSet" && listA[1] == "OnlyA");
    CHECK(listB.size() == 2 && listB[0] == "BothSet" && listB[1] == "OnlyB");

    // "Not used now": only when a set is chosen and no pass runs that model
    CHECK(NotUsedNow(true, true, false, true));
    CHECK(!NotUsedNow(true, true, true, false));
    CHECK(NotUsedNow(true, false, true, false));
    CHECK(!NotUsedNow(true, false, false, true));
    CHECK(!NotUsedNow(false, true, false, false)); // stock: nothing to say

    // A change of either set changes the key
    CHECK(BuiltKey("a", "b") == "a|b");
    CHECK(BuiltKey("", "") == "|");
    CHECK(BuiltKey("a", "") != BuiltKey("", "a"));

    printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
