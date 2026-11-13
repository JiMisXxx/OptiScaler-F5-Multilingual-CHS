#pragma once

// Runtime UI language switch, one-way: the source keeps the upstream English
// literals untouched and the Chinese text is looked up at the ImGui entry points.
// Nothing is ever translated back into English, so a label can never lose its
// original string and the widget IDs stay identical in both languages.

namespace OptiI18n
{
enum class Language
{
    Chinese = 0,
    English = 1,
};

// Reads OptiScaler.i18n (zh / en) next to the dll; defaults to Chinese.
void Initialize();
Language GetLanguage();
void SetLanguage(Language language);

bool IsEnglish();

// Plain text: exact lookup in the en -> zh table, returns the input when there
// is no entry.
const char* Text(const char* text);
const char* Text(const char* text, const char* text_end);

// Widget label: translates only the visible part (before "##") and appends
// "###" + the complete original label, so ImGui hashes exactly the same string
// as upstream and the ID is unchanged in both languages.
const char* Label(const char* label);

// Entry points return pointers into a per-frame cache; clear it once per frame.
void BeginFrame();
} // namespace OptiI18n
