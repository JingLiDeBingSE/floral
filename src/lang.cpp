#include "lang.h"

Lang g_lang = Lang::Zh;

const wchar_t* LangName() {
    return (g_lang == Lang::En) ? L"English" : L"中文";
}
