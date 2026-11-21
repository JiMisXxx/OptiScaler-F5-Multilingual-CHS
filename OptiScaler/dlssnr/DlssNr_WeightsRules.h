#pragma once
// Neural Rendering model weights, one set per model: [DlssNr] WeightsSetA (Preset A, Model A, WEIGHTS_HT.*) and
// WeightsSetB (Preset B or Default, Model B, WEIGHTS_CC_SMALL_FP8.*). Plain rules, no config and no ImGui, so
// tests/nr_weights_per_model_smoke.cpp runs them.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace DlssNr::WeightsRules
{
inline constexpr std::string_view kResourceA = "WEIGHTS_HT";
inline constexpr std::string_view kResourceB = "WEIGHTS_CC_SMALL_FP8";

// Which model's weights a set folder holds, from its file names (<RESOURCE>.<CRC>.bin)
struct Models
{
    bool a = false, b = false;
};

inline bool StartsWithNoCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(),
                      [](char l, char r) { return std::tolower((unsigned char) l) == std::tolower((unsigned char) r); });
}

inline void NoteFile(Models& m, std::string_view file)
{
    if (StartsWithNoCase(file, "WEIGHTS_HT."))
        m.a = true;
    else if (StartsWithNoCase(file, "WEIGHTS_CC_SMALL_FP8."))
        m.b = true;
}

struct Set
{
    std::string name;
    Models models;
};

// The folders a model's list shows: only those with that model's file (a folder with both is in both lists)
inline std::vector<std::string> ListFor(const std::vector<Set>& sets, bool modelA)
{
    std::vector<std::string> names;
    for (const auto& s : sets)
        if (modelA ? s.models.a : s.models.b)
            names.push_back(s.name);
    return names;
}

struct Pair
{
    std::string a, b;
    bool operator==(const Pair&) const = default;
};

// The old single [DlssNr] WeightsSet, taken for both models when neither new key is in the file: a model keeps the
// folder only when the folder has that model's file, else stock. Returns false when nothing is to migrate.
inline bool Migrate(const std::string& oldSet, bool newKeysPresent, Models oldHas, Pair& out)
{
    out = {};
    if (oldSet.empty() || newKeysPresent)
        return false;
    if (oldHas.a)
        out.a = oldSet;
    if (oldHas.b)
        out.b = oldSet;
    return true;
}

// The amber "Not used now" line: a set is chosen for a model and no pass runs that model. Passes 1-3 count,
// passes 4 and up use Pass 1's, the driver proxy runs one pass; Default and B run Model B.
inline bool NotUsedNow(bool setChosen, bool modelA, bool anyPassRunsA, bool anyPassRunsB)
{
    return setChosen && !(modelA ? anyPassRunsA : anyPassRunsB);
}

// The key the renderers compare to know when to rebuild
inline std::string BuiltKey(const std::string& a, const std::string& b) { return a + "|" + b; }
} // namespace DlssNr::WeightsRules
