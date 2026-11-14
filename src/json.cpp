#include "json.h"

#include <cstdio>
#include <cstdlib>

static void SkipWs(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}

static void PutUtf8(std::string& o, unsigned int cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

static bool ParseStr(const std::string& s, size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size() && s[p] != '"') {
        char c = s[p++];
        if (c != '\\') { out += c; continue; }
        if (p >= s.size()) return false;
        char e = s[p++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '/': out += '/'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'u': {
            if (p + 4 > s.size()) return false;
            unsigned int cp = (unsigned int)strtoul(s.substr(p, 4).c_str(), nullptr, 16);
            p += 4;
            PutUtf8(out, cp);
            break;
        }
        default: out += e;
        }
    }
    if (p >= s.size()) return false;
    ++p;                                   // skip closing quote
    return true;
}

static bool ParseVal(const std::string& s, size_t& p, JVal& v, int depth = 0) {
    if (depth > 40) return false;
    SkipWs(s, p);
    if (p >= s.size()) return false;
    char c = s[p];
    if (c == '{') {
        v.t = JVal::OBJ; ++p; SkipWs(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        for (;;) {
            SkipWs(s, p);
            std::string k;
            if (!ParseStr(s, p, k)) return false;
            SkipWs(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            ++p;
            JVal child;
            if (!ParseVal(s, p, child, depth + 1)) return false;
            v.o.emplace_back(std::move(k), std::move(child));
            SkipWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return false;
        }
    }
    if (c == '[') {
        v.t = JVal::ARR; ++p; SkipWs(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        for (;;) {
            JVal child;
            if (!ParseVal(s, p, child, depth + 1)) return false;
            v.a.push_back(std::move(child));
            SkipWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return false;
        }
    }
    if (c == '"') { v.t = JVal::STR; return ParseStr(s, p, v.s); }
    if (!s.compare(p, 4, "true"))  { v.t = JVal::BOOL; v.b = true;  p += 4; return true; }
    if (!s.compare(p, 5, "false")) { v.t = JVal::BOOL; v.b = false; p += 5; return true; }
    if (!s.compare(p, 4, "null"))  { v.t = JVal::NUL; p += 4; return true; }
    {
        const char* begin = s.c_str() + p;
        char* end = nullptr;
        double d = strtod(begin, &end);
        if (end == begin) return false;
        p += (size_t)(end - begin);
        v.t = JVal::NUM; v.num = d;
        return true;
    }
}

bool Json(const std::string& s, JVal& out) {
    size_t p = 0;
    return ParseVal(s, p, out, 0);
}

std::string JsonQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
