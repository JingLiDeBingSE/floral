#include "identity.h"
#include "json.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

extern std::wstring g_dataDir;   // set by main.cpp (Documents\Floral)

namespace {

std::wstring FilePath() {
    std::wstring dir = g_dataDir;
    if (dir.empty()) {
        wchar_t docs[MAX_PATH] = { 0 };
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, nullptr, 0, docs)))
            dir = std::wstring(docs) + L"\\Floral";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\identity.json";
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

}  // namespace

std::wstring IdentityDataDir() {
    std::wstring p = FilePath();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::string Identity::Code() const {
    if (id.size() != 8) return id;
    return id.substr(0, 4) + "-" + id.substr(4);
}

Identity IdentityLoad() {
    Identity out;
    const std::wstring path = FilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;

    std::string body;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);

    JVal root;
    if (!Json(body, root)) return out;
    if (const JVal* v = root.get("id")) out.id = v->str();
    if (const JVal* v = root.get("name")) out.name = v->str();
    if (const JVal* v = root.get("secret")) out.secret = v->str();
    if (const JVal* v = root.get("serverKey")) out.serverKey = v->str();
    out.valid = !out.id.empty() && !out.secret.empty();
    return out;
}

bool IdentitySave(const Identity& id) {
    const std::wstring path = FilePath();
    std::string body = "{\n  \"id\": " + JsonQuote(id.id) +
                       ",\n  \"name\": " + JsonQuote(id.name) +
                       ",\n  \"secret\": " + JsonQuote(id.secret) +
                       ",\n  \"serverKey\": " + JsonQuote(id.serverKey) + "\n}\n";

    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const size_t put = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (put != body.size()) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void IdentityClear() {
    DeleteFileW(FilePath().c_str());
    Logf("IDN", "已清除本机身份缓存");
}
