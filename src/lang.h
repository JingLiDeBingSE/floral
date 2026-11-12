#pragma once

/*
 * 界面语言：只有中文和英文。
 * 用法：Text(T(L"中文", L"English"), ...)
 *       std::wstring s = T(L"共 ", L"") + ... （拼接时用 std::wstring 包一层）
 */
enum class Lang { Zh, En };

extern Lang g_lang;

inline const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    return (g_lang == Lang::En) ? en : zh;
}

const wchar_t* LangName();       // "中文" / "English"
