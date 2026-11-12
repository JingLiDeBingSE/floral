#include "app.h"
#include <fstream>
#include <sstream>
#include <string>

/*
 * 数据加载。
 *
 * 读 kugou-likes.tsv —— 由 kugou-export.js 从酷狗「我喜欢」导出：
 *   title <TAB> artist <TAB> album <TAB> seconds <TAB> cover（相对路径，可为空）
 *
 * 文件不存在就显示空列表。要换成别的数据源，改这里即可。
 */

/** 相对工作目录还是 exe 所在目录找文件，返回找到的路径（找不到返回空） */
static std::wstring FindAsset(const std::wstring& name) {
    // 1) 当前工作目录
    if (GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES) return name;

    // 2) exe 所在目录及其上一级（build\songlist.exe -> 项目根目录）
    wchar_t exe[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring dir(exe);
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) {
            std::wstring base = dir.substr(0, p);
            std::wstring c1 = base + L"\\" + name;
            if (GetFileAttributesW(c1.c_str()) != INVALID_FILE_ATTRIBUTES) return c1;
            std::wstring up = base.substr(0, base.find_last_of(L"\\/"));
            std::wstring c2 = up + L"\\" + name;
            if (GetFileAttributesW(c2.c_str()) != INVALID_FILE_ATTRIBUTES) return c2;
        }
    }
    return L"";
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r");
    return s.substr(a, b - a + 1);
}

std::wstring g_likesName = L"kugou-likes.tsv";

void InitData() {
    g_app.songs.clear();

    std::string bytes;
    std::wstring path = FindAsset(g_likesName);

    if (!path.empty()) {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (f) bytes.assign((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) return;        // 没有歌单就是空的，等登录后拉
    if (bytes.empty()) return;

    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);

    std::wstring text = Utf8ToWide(bytes);
    std::wstringstream ss(text);
    std::wstring line;

    bool first = true;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        if (first) { first = false; continue; }    // 跳过表头

        std::vector<std::wstring> col;
        size_t start = 0;
        while (true) {
            size_t tab = line.find(L'\t', start);
            if (tab == std::wstring::npos) { col.push_back(line.substr(start)); break; }
            col.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (col.size() < 4) continue;

        Song s;
        s.title = Trim(col[0]);
        s.artist = Trim(col[1]);
        s.album = col.size() > 2 ? Trim(col[2]) : L"";
        s.durationSec = _wtoi(col[3].c_str());
        s.cover = col.size() > 4 ? Trim(col[4]) : L"";
        if (!s.cover.empty()) s.cover = FindAsset(s.cover);
        if (col.size() > 5) {
            std::wstring h = Trim(col[5]);
            s.hash.assign(h.begin(), h.end());     // hash 是十六进制，直接窄化
        }
        if (col.size() > 6) s.mixId = _wtoi64(col[6].c_str());
        if (col.size() > 7) s.albumId = _wtoi64(col[7].c_str());
        if (col.size() > 8) {
            std::wstring u = Trim(col[8]);
            s.coverUrl.assign(u.begin(), u.end());
        }
        if (s.title.empty()) continue;
        g_app.songs.push_back(s);
    }

    g_app.tileAnim.assign(g_app.songs.size(), 0.0f);
}
