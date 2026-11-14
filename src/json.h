#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

/* Minimal JSON value; strings are UTF-8 */
struct JVal {
    enum T { NUL, NUM, STR, BOOL, ARR, OBJ } t = NUL;
    bool        b = false;
    double      num = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* get(const char* k) const {
        if (t != OBJ) return nullptr;
        for (const auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    const JVal* at(int i) const {
        if (t != ARR || i < 0 || i >= (int)a.size()) return nullptr;
        return &a[(size_t)i];
    }
    int count() const { return t == ARR ? (int)a.size() : 0; }
    std::string str() const { return t == STR ? s : ""; }
    long long ll() const {
        if (t == NUM) return (long long)num;
        if (t == STR) return atoll(s.c_str());      // API may return a string
        return 0;
    }
};

bool Json(const std::string& s, JVal& out);

/* The UTF-8 string as a quoted JSON string literal */
std::string JsonQuote(const std::string& s);
