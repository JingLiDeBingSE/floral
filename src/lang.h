#pragma once

/*
 * UI language: Chinese and English only.
 * Usage: Text(T(L"中文", L"English"), ...)
 *       std::wstring s = T(L"共 ", L"") + ... (wrap in std::wstring when concatenating)
 */
enum class Lang { Zh, En };

extern Lang g_lang;

inline const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    return (g_lang == Lang::En) ? en : zh;
}

const wchar_t* LangName();       // returns "中文" / "English"
