#include "OptiI18n.h"

#include <windows.h>

#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
struct OptiI18nPair
{
    const char* en;
    const char* zh;
};

#include "OptiI18nTable.inc"

OptiI18n::Language g_language = OptiI18n::Language::Chinese;
bool g_inited = false;

std::unordered_map<std::string_view, const char*> g_enToZh;

// Entry points return pointers into g_frameStrings; a deque keeps them valid
// while new entries keep being appended during the same frame.
std::deque<std::string> g_frameStrings;

const std::wstring& ModuleDir()
{
    static std::wstring dir;
    if (!dir.empty())
        return dir;

    static int anchor = 0;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&anchor), &module);

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, path, MAX_PATH) != 0)
    {
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir = full.substr(0, slash + 1);
    }
    return dir;
}

void BuildMap()
{
    if (!g_enToZh.empty())
        return;

    g_enToZh.reserve(sizeof(kOptiI18nPairs) / sizeof(kOptiI18nPairs[0]));
    for (const auto& pair : kOptiI18nPairs)
    {
        if (pair.en != nullptr && pair.zh != nullptr && pair.en[0] != '\0')
            g_enToZh.emplace(std::string_view(pair.en), pair.zh);
    }
}

const char* Cache(std::string value)
{
    g_frameStrings.push_back(std::move(value));
    return g_frameStrings.back().c_str();
}

const char* Lookup(const std::string_view text)
{
    const auto it = g_enToZh.find(text);
    return it == g_enToZh.end() ? nullptr : it->second;
}
} // namespace

namespace OptiI18n
{
void Initialize()
{
    if (g_inited)
        return;
    g_inited = true;

    std::ifstream file(ModuleDir() + L"OptiScaler.i18n", std::ios::binary);
    std::string value;
    if (file)
        file >> value;

    g_language = value == "en" ? Language::English : Language::Chinese;
    if (g_language == Language::Chinese)
        BuildMap();
}

Language GetLanguage()
{
    Initialize();
    return g_language;
}

bool IsEnglish()
{
    Initialize();
    return g_language == Language::English;
}

void SetLanguage(Language language)
{
    Initialize();
    g_language = language;
    if (language == Language::Chinese)
        BuildMap();

    std::ofstream file(ModuleDir() + L"OptiScaler.i18n", std::ios::binary | std::ios::trunc);
    if (file)
        file << (language == Language::English ? "en" : "zh");
}

const char* Text(const char* text)
{
    if (text == nullptr || text[0] == '\0' || IsEnglish())
        return text;

    const char* translated = Lookup(std::string_view(text));
    return translated != nullptr ? translated : text;
}

const char* Text(const char* text, const char* text_end)
{
    if (text == nullptr)
        return text;
    if (text_end == nullptr)
        return Text(text);
    if (text_end <= text || IsEnglish())
        return text;

    const std::string_view input(text, static_cast<size_t>(text_end - text));
    const char* translated = Lookup(input);
    return translated != nullptr ? translated : text;
}

const char* Label(const char* label)
{
    if (label == nullptr || label[0] == '\0' || label[0] == '#' || IsEnglish())
        return label;

    // ImGui stops displaying at the first "##". "###label" makes the whole
    // original label the hash input again, so the ID matches upstream exactly.
    const char* visible_end = std::strstr(label, "##");
    if (visible_end == nullptr)
    {
        const char* translated = Lookup(std::string_view(label));
        if (translated == nullptr)
            return label;

        std::string out(translated);
        out += "###";
        out += label;
        return Cache(std::move(out));
    }

    if (visible_end == label)
        return label;

    const char* translated = Lookup(std::string_view(label, static_cast<size_t>(visible_end - label)));
    if (translated == nullptr)
    {
        // Fall back to the whole label (with "##" and its id) for tables that
        // store the full string. Visible text is tried first so that entries
        // keyed on the visible part keep winning.
        translated = Lookup(std::string_view(label));
        if (translated == nullptr)
            return label;
    }

    std::string out(translated);
    out += "###";
    out += label;
    return Cache(std::move(out));
}

void BeginFrame()
{
    g_frameStrings.clear();
}
} // namespace OptiI18n
