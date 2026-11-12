#include "kugou.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <set>

/*
 * Ported two jobs from the old Node scripts to C++:
 *   1) QR-code login (obtain token / userid)
 *   2) Fetch the "Likes" playlist and write kugou-likes.tsv
 *
 * Depends only on the local kugou-api service (127.0.0.1:3000); see kugou-login.js / kugou-fetch.js for API details.
 */

/* ══════════════ HTTP ══════════════ */
static std::string Http(const std::wstring& url) {
    std::string out;
    HINTERNET inet = InternetOpenW(L"KugouPlayer/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!inet) return out;
    DWORD ct = 5000, rt = 12000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &rt, sizeof(rt));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_NO_UI;
    HINTERNET req = InternetOpenUrlW(inet, url.c_str(), nullptr, 0, flags, 0);
    if (req) {
        char buf[8192];
        DWORD got = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
            out.append(buf, got);
        InternetCloseHandle(req);
    }
    InternetCloseHandle(inet);
    return out;
}

static std::wstring Wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string UriEnc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; sprintf(b, "%%%02X", c); o += b; }
    }
    return o;
}

/* ══════════════ Minimal JSON ══════════════ */
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

static bool Json(const std::string& s, JVal& out) {
    size_t p = 0;
    return ParseVal(s, p, out, 0);
}

/* ══════════════ base64 → png ══════════════ */
static bool SaveBase64Png(const std::string& b64, const std::wstring& path) {
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int map[256];
    for (int i = 0; i < 256; i++) map[i] = -1;
    for (int i = 0; i < 64; i++) map[(unsigned char)tab[i]] = i;

    std::string bin;
    int buf = 0, bits = 0;
    for (unsigned char c : b64) {
        if (c == '=') break;
        int v = map[c];
        if (v < 0) continue;                 // skip newlines etc.
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bin += (char)((buf >> bits) & 0xFF);
        }
    }
    if (bin.size() < 100) return false;

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return false;
    fwrite(bin.data(), 1, bin.size(), f);
    fclose(f);
    return true;
}

/* ══════════════ Login ══════════════ */
static std::string g_qrKey;

bool KgLoginStart(const std::wstring& qrPath, std::wstring& err) {
    g_qrKey.clear();

    JVal k;
    if (!Json(Http(Wide("http://127.0.0.1:3000/login/qr/key?timestamp=" +
                        std::to_string(GetTickCount64()))), k)) {
        err = L"接口没响应，kugou-api 起来了吗";
        return false;
    }
    const JVal* d = k.get("data");
    const JVal* q = d ? d->get("qrcode") : nullptr;
    if (!q || q->str().empty()) { err = L"拿二维码 key 失败"; return false; }
    g_qrKey = q->str();

    JVal c;
    if (!Json(Http(Wide("http://127.0.0.1:3000/login/qr/create?key=" + UriEnc(g_qrKey) +
                        "&qrimg=true")), c)) {
        err = L"生成二维码超时";
        return false;
    }
    const JVal* cd = c.get("data");
    const JVal* b64 = cd ? cd->get("base64") : nullptr;
    if (!b64) { err = L"接口没返回二维码图"; return false; }

    std::string s = b64->str();
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = s.substr(comma + 1);   // strip data:image/png;base64,
    if (!SaveBase64Png(s, qrPath)) { err = L"二维码图片写入失败"; return false; }
    return true;
}

int KgLoginPoll(std::string& token, std::string& userid, std::wstring& err) {
    if (g_qrKey.empty()) { err = L"没有二维码 key"; return 4; }

    JVal r;
    std::string raw = Http(Wide("http://127.0.0.1:3000/login/qr/check?key=" + UriEnc(g_qrKey) +
                                "&timestamp=" + std::to_string(GetTickCount64())));
    if (raw.empty() || !Json(raw, r)) return 0;    // transient failure: keep waiting

    // Kugou nests status under data (not top-level); per kugou-login.js: r.json.data.status
    const JVal* d = r.get("data");
    const JVal* st = d ? d->get("status") : nullptr;
    if (!st) st = r.get("status");
    int status = st ? (int)st->ll() : -1;
    if (status == 0) { err = L"二维码已过期，点「重新登录」再来一次"; return 3; }
    if (status == 1 || status == -1) return 0;
    if (status == 2) return 1;
    if (status != 4) return 0;

    // status == 4: login succeeded, cookie carries token / userid
    std::string cookie;
    const JVal* ck = r.get("cookie");
    if (!ck) { const JVal* dd = r.get("data"); if (dd) ck = dd->get("cookie"); }
    if (ck) {
        if (ck->t == JVal::ARR) {
            for (int i = 0; i < ck->count(); i++) {
                if (!cookie.empty()) cookie += ";";
                cookie += ck->at(i)->str();
            }
        } else if (ck->t == JVal::STR) {
            cookie = ck->str();
        } else if (ck->t == JVal::OBJ) {
            for (const auto& kv : ck->o) {
                if (!cookie.empty()) cookie += ";";
                cookie += kv.first + "=" + kv.second.str();
            }
        }
    }

    auto field = [&](const char* name) -> std::string {
        std::string k = std::string(name) + "=";
        size_t p = cookie.find(k);
        if (p == std::string::npos) return "";
        p += k.size();
        size_t e = cookie.find(';', p);
        return cookie.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };

    token  = field("token");
    userid = field("userid");
    if (token.empty() || userid.empty()) {       // some responses put the fields directly in data
        const JVal* dd = r.get("data");
        if (dd) {
            const JVal* t = dd->get("token");
            const JVal* u = dd->get("userid");
            if (token.empty() && t) token = t->str();
            if (userid.empty() && u) userid = std::to_string(u->ll());
        }
    }
    if (token.empty() || userid.empty()) { err = L"登录成功但没拿到 token"; return 4; }
    return 2;
}

/* ══════════════ Playlist export ══════════════ */
static std::string Clean(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
    size_t a = o.find_first_not_of(' ');
    size_t b = o.find_last_not_of(' ');
    return (a == std::string::npos) ? "" : o.substr(a, b - a + 1);
}

/* Forward declaration: fetch tracks by playlist id (defined below) */
static int KgExportTracksByGcid(const std::string& token, const std::string& userid,
                                const std::string& gcid,
                                const std::wstring& outTsv, std::wstring& err);
/* All playlists under the account */
int KgFetchPlaylists(const std::string& token, const std::string& userid,
                     std::vector<KgPlaylist>& out, std::wstring& err) {
    out.clear();
    std::string cookie = "token=" + token + ";userid=" + userid;
    const std::string auth = "&cookie=" + UriEnc(cookie) + "&userid=" + userid +
                             "&token=" + UriEnc(token) + "&timestamp=" +
                             std::to_string(GetTickCount64());
    auto api = [&](const std::string& path) {
        return Http(Wide("http://127.0.0.1:3000" + path + auth));
    };

    // At most 3 pages (100 per page) is enough
    for (int page = 1; page <= 3; page++) {
        JVal r;
        if (!Json(api("/user/playlist?pagesize=100&page=" + std::to_string(page)), r)) {
            if (page == 1) { err = L"拉歌单列表超时"; return -1; }
            break;
        }
        const JVal* d = r.get("data");
        const JVal* list = d ? (d->get("info") ? d->get("info") : d->get("list")) : nullptr;
        if (!list || list->count() == 0) break;

        for (int i = 0; i < list->count(); i++) {
            const JVal* p = list->at(i);
            if (!p) continue;
            const JVal* g = p->get("global_collection_id");
            if (!g || g->str().empty()) continue;
            KgPlaylist kp;
            kp.gcid = g->str();
            const JVal* nm = p->get("listname");
            if (!nm) nm = p->get("name");
            if (nm) {
                int n = MultiByteToWideChar(CP_UTF8, 0, nm->str().data(), (int)nm->str().size(), nullptr, 0);
                kp.name.resize((size_t)n);
                MultiByteToWideChar(CP_UTF8, 0, nm->str().data(), (int)nm->str().size(), &kp.name[0], n);
            }
            if (kp.name.empty()) kp.name = L"(未命名歌单)";
            const JVal* cnt = p->get("count");
            if (!cnt) cnt = p->get("song_count");
            if (cnt) kp.count = (int)cnt->ll();
            const JVal* pic = p->get("pic");
            if (pic) kp.coverUrl = pic->str();
            const JVal* def = p->get("is_def");
            kp.isDefault = (def && def->ll() == 2);
            // Only playlists created by this user: list_create_userid == self
            // (type=0: own, type=1: saved from others, source=2: album)
            const JVal* cu = p->get("list_create_userid");
            long long createUid = cu ? cu->ll() : 0;
            if (createUid != 0 && std::to_string(createUid) != userid) continue;
            const JVal* ty = p->get("type");
            if (ty && ty->ll() != 0) continue;
            out.push_back(std::move(kp));
        }
        if (list->count() < 100) break;
    }
    if (out.empty()) { err = L"没有拿到任何歌单"; return -1; }
    return (int)out.size();
}

/* Tracks of the specified playlist */
int KgExportPlaylist(const std::string& token, const std::string& userid,
                     const std::string& gcid, const std::wstring& outTsv, std::wstring& err) {
    if (gcid.empty()) { err = L"歌单 id 为空"; return -1; }
    return KgExportTracksByGcid(token, userid, gcid, outTsv, err);
}
/* Fetch all tracks by playlist id and write the TSV (shared by Likes / any playlist) */
static int KgExportTracksByGcid(const std::string& token, const std::string& userid,
                                const std::string& gcid,
                                const std::wstring& outTsv, std::wstring& err) {
    std::string cookie = "token=" + token + ";userid=" + userid;
    const std::string auth = "&cookie=" + UriEnc(cookie) + "&userid=" + userid +
                             "&token=" + UriEnc(token) + "&timestamp=" +
                             std::to_string(GetTickCount64());
    auto api = [&](const std::string& path) {
        return Http(Wide("http://127.0.0.1:3000" + path + auth));
    };
    (void)api;    // 2) fetch tracks and build the TSV directly — no JVal pointers kept, so no dangling after vector realloc
    std::string out = "title\tartist\talbum\tseconds\tcover\thash\tmixsongid\talbum_id\tcoverUrl\n";
    int n = 0;

    auto appendTo = [&](const JVal* t, std::string& dst, int& cnt) {
        if (!t || t->t != JVal::OBJ) return;

        // name looks like "artistA, artistB - actual title"; remark is a Chinese alias, don't use it as the title
        const JVal* name2 = t->get("name");
        std::string raw = name2 ? name2->str() : "";
        size_t dash = raw.find(" - ");
        std::string title = (dash == std::string::npos) ? raw : raw.substr(dash + 3);
        if (title.empty()) {                      // fall back to remark only when name is missing
            const JVal* remark = t->get("remark");
            if (remark) title = remark->str();
        }
        title = Clean(title);
        if (title.empty()) return;

        std::string artist;
        const JVal* si = t->get("singerinfo");
        if (si && si->t == JVal::ARR) {
            for (int i = 0; i < si->count(); i++) {
                const JVal* a = si->at(i);
                const JVal* an = a ? a->get("name") : nullptr;
                if (an && !an->str().empty()) {
                    if (!artist.empty()) artist += "、";
                    artist += an->str();
                }
            }
        }

        std::string album;
        const JVal* ai = t->get("albuminfo");
        const JVal* an2 = ai ? ai->get("name") : nullptr;
        if (an2) album = an2->str();

        const JVal* tl = t->get("timelen");
        long long sec = tl ? (tl->ll() / 1000) : 0;

        std::string cover;
        const JVal* cv = t->get("cover");
        if (cv) {
            cover = cv->str();
            size_t p = cover.find("{size}");
            if (p != std::string::npos) cover.replace(p, 6, "400");
        }

        const JVal* h = t->get("hash");
        std::string hash = h ? h->str() : "";

        const JVal* mid = t->get("mixsongid");
        if (!mid) mid = t->get("add_mixsongid");
        if (!mid) mid = t->get("audio_id");
        long long mixId = mid ? mid->ll() : 0;

        const JVal* aid = t->get("album_id");
        long long albumId = aid ? aid->ll() : 0;

        out += Clean(title) + "\t" + Clean(artist) + "\t" + Clean(album) + "\t" +
               std::to_string(sec) + "\t\t" + hash + "\t" + std::to_string(mixId) + "\t" +
               std::to_string(albumId) + "\t" + cover + "\n";
        n++;
    };

    // Sequential paging: the API is serial anyway; concurrency is not faster and tends to drop pages
    // Up to 3 retries per page; if any page ultimately fails, fail the whole export and don't write a partial playlist
    std::set<std::string> seen;          // dedupe by hash (pages occasionally overlap)
    bool failed = false;
    for (int page = 1; page <= 30 && !failed; page++) {
        const JVal* arr = nullptr;
        JVal r;
        for (int attempt = 0; attempt < 3 && !arr; attempt++) {
            if (attempt) Sleep(150 + 120 * (DWORD)attempt);
            r = JVal();
            if (!Json(api("/playlist/track/all?id=" + UriEnc(gcid) + "&page=" +
                          std::to_string(page) + "&pagesize=300"), r)) continue;
            const JVal* rd = r.get("data");
            const JVal* cand = rd ? (rd->get("info") ? rd->get("info")
                                  : (rd->get("songs") ? rd->get("songs") : rd->get("lists"))) : nullptr;
            if (cand && cand->count() > 0) arr = cand;
        }
        if (!arr) { failed = true; break; }

        int total = arr->count();
        std::string buf;
        for (int i = 0; i < total; i++) {
            buf.clear();
            int c = 0;
            appendTo(arr->at(i), buf, c);
            if (buf.empty()) continue;
            // column 6 is hash; use it to dedupe
            int col = 0;
            size_t a = 0, b = 0;
            for (; b < buf.size(); b++) {
                if (buf[b] != '\t') continue;
                if (++col == 5) { a = b + 1; }
                else if (col == 6) break;
            }
            std::string key = (col >= 6 && a > 0 && b > a) ? buf.substr(a, b - a) : buf;
            if (!seen.insert(key).second) continue;
            out += buf;
            n++;
        }
        if (total < 300) break;              // last page
    }
    if (failed) { err = L"拉歌单失败（分页请求出错，已保留旧歌单）"; return -1; }    if (n == 0) { err = L"歌单里没有歌曲"; return -1; }

    FILE* f = nullptr;
    _wfopen_s(&f, outTsv.c_str(), L"wb");
    if (!f) { err = L"写 tsv 失败"; return -1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return n;
}


int KgExportLikes(const std::string& token, const std::string& userid,
                  const std::wstring& outTsv, std::wstring& err) {
    std::string cookie = "token=" + token + ";userid=" + userid;
    const std::string auth = "&cookie=" + UriEnc(cookie) + "&userid=" + userid +
                             "&token=" + UriEnc(token) + "&timestamp=" +
                             std::to_string(GetTickCount64());
    auto api = [&](const std::string& path) {
        return Http(Wide("http://127.0.0.1:3000" + path + auth));
    };

    // 1) playlist list, locate "Likes"
    JVal pl;
    if (!Json(api("/user/playlist?pagesize=100&page=1"), pl)) { err = L"拉歌单列表超时"; return -1; }
    const JVal* pd = pl.get("data");
    const JVal* list = pd ? (pd->get("info") ? pd->get("info") : pd->get("list")) : nullptr;
    if (!list) { err = L"歌单列表为空（登录可能失效）"; return -1; }

    std::string gcid, name;
    for (int i = 0; i < list->count() && gcid.empty(); i++) {      // 1) exact name match
        const JVal* p = list->at(i);
        const JVal* nm = p ? p->get("listname") : nullptr;
        const JVal* g = p ? p->get("global_collection_id") : nullptr;
        if (nm && g && nm->str() == "我喜欢" && !g->str().empty()) { name = nm->str(); gcid = g->str(); }
    }
    for (int i = 0; i < list->count() && gcid.empty(); i++) {      // 2) is_def == 2
        const JVal* p = list->at(i);
        const JVal* def = p ? p->get("is_def") : nullptr;
        const JVal* g = p ? p->get("global_collection_id") : nullptr;
        if (def && def->ll() == 2 && g && !g->str().empty()) { gcid = g->str(); }
    }
    for (int i = 0; i < list->count() && gcid.empty(); i++) {      // 3) name contains "Likes"
        const JVal* p = list->at(i);
        const JVal* nm = p ? p->get("listname") : nullptr;
        const JVal* g = p ? p->get("global_collection_id") : nullptr;
        if (nm && g && nm->str().find("我喜欢") != std::string::npos && !g->str().empty()) {
            name = nm->str(); gcid = g->str();
        }
    }
    for (int i = 0; i < list->count() && gcid.empty(); i++) {      // 4) fallback: first playlist
        const JVal* p = list->at(i);
        const JVal* g = p ? p->get("global_collection_id") : nullptr;
        if (g && !g->str().empty()) gcid = g->str();
    }
    if (gcid.empty()) { err = L"没找到可用的歌单"; return -1; }

    // hand off to the shared helper to fetch tracks by gcid
    return KgExportTracksByGcid(token, userid, gcid, outTsv, err);
}


/* ══════════════ NetEase Cloud Music (port 4000) ══════════════ */

/* NetEase has rate limiting: dense requests yield repeated 502 / -460. The reference project throttles via withOnlineRequestLimit; same here */
static std::mutex   g_ngMx;
static ULONGLONG    g_ngLast = 0;
static int          g_ngGap  = 350;      // milliseconds; grows automatically after failures

static std::string NgHttp(const std::wstring& url) {
    std::lock_guard<std::mutex> lk(g_ngMx);
    ULONGLONG now = GetTickCount64();
    if (now - g_ngLast < (ULONGLONG)g_ngGap) {
        Sleep((DWORD)(g_ngGap - (now - g_ngLast)));
    }
    g_ngLast = GetTickCount64();
    std::string r = Http(url);
    if (r.empty() || r.find("\"code\":-460") != std::string::npos) {
        g_ngGap = (g_ngGap < 2000) ? g_ngGap + 250 : 2000;   // back off when rate-limited
    } else {
        g_ngGap = (g_ngGap > 350) ? g_ngGap - 50 : 350;      // recover gradually when healthy
    }
    return r;
}


static const wchar_t* kNgBase = L"http://127.0.0.1:4000";

static std::string g_ngKey;

bool NgLoginStart(const std::wstring& qrPath, std::wstring& err) {
    g_ngKey.clear();
    // this endpoint occasionally returns 502; retry a few times
    for (int i = 0; i < 4 && g_ngKey.empty(); i++) {
        JVal k;
        if (!Json(NgHttp(Wide("http://127.0.0.1:4000/login/qr/key?timestamp=" +
                            std::to_string(GetTickCount64()))), k)) {
            Sleep(600);
            continue;
        }
        const JVal* d = k.get("data");
        const JVal* uk = d ? d->get("unikey") : nullptr;
        if (uk && !uk->str().empty()) g_ngKey = uk->str();
        else Sleep(600);
    }
    if (g_ngKey.empty()) { err = L"拿二维码 key 失败（接口 502，稍后重试）"; return false; }

    JVal c;
    if (!Json(NgHttp(Wide("http://127.0.0.1:4000/login/qr/create?key=" + UriEnc(g_ngKey) +
                        "&qrimg=true")), c)) {
        err = L"生成二维码超时";
        return false;
    }
    const JVal* cd = c.get("data");
    const JVal* img = cd ? cd->get("qrimg") : nullptr;
    if (!img) { err = L"接口没返回二维码图"; return false; }

    std::string s = img->str();
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = s.substr(comma + 1);
    if (!SaveBase64Png(s, qrPath)) { err = L"二维码写入失败"; return false; }
    return true;
}

int NgLoginPoll(std::string& cookie, std::wstring& err) {
    if (g_ngKey.empty()) { err = L"没有二维码 key"; return 4; }
    JVal r;
    // This NetEase endpoint intermittently returns 502 or drops the connection: one failure must not end polling; keep waiting for the next round
    std::string raw = NgHttp(Wide("http://127.0.0.1:4000/login/qr/check?key=" + UriEnc(g_ngKey) +
                                  "&timestamp=" + std::to_string(GetTickCount64())));
    if (raw.empty() || !Json(raw, r)) return 0;
    const JVal* c = r.get("code");
    int code = c ? (int)c->ll() : -1;
    if (code == 800) { err = L"二维码过期了，重新点一次「重新登录」"; return 3; }
    if (code == 801 || code == -1) return 0;     // waiting for scan
    if (code == 802) return 1;                   // scanned, awaiting confirmation
    if (code != 803) return 0;

    // 803: login succeeded, cookie is at the top level
    const JVal* ck = r.get("cookie");
    std::string ck2;
    if (ck && ck->t == JVal::STR) ck2 = ck->str();
    if (ck2.empty()) { err = L"登录成功但没拿到 cookie"; return 4; }
    cookie = ck2;
    return 2;
}

/* Following folia-major: public APIs use an anonymous cookie (/register/anonimous);
   the login cookie is stored separately. Without an anonymous cookie NetEase occasionally returns 502 */
static std::string g_anonCookie;

static std::string GetAnonCookie() {
    if (!g_anonCookie.empty()) return g_anonCookie;
    JVal r;
    std::string raw = NgHttp(Wide("http://127.0.0.1:4000/register/anonimous?timestamp=" +
                                std::to_string(GetTickCount64())));
    if (!raw.empty() && Json(raw, r)) {
        const JVal* c = r.get("cookie");
        if (c && !c->str().empty()) g_anonCookie = c->str();
    }
    return g_anonCookie;
}

std::wstring NgResolveUrl(const std::string& songId, const char* level, std::wstring& err, int* gotLevel) {
    if (songId.empty()) { err = L"没有歌曲 id"; return L""; }
    // Not every track has lossless/high quality: step down the requested levels
    const char* chain[3] = { "standard", "exhigh", "lossless" };
    int want = 1;
    if (level && !strcmp(level, "standard")) want = 0;
    if (level && !strcmp(level, "lossless")) want = 2;

    std::string anon = GetAnonCookie();
    for (int q = want; q >= 0; q--) {
        std::string u = "http://127.0.0.1:4000/song/url/v1?id=" + UriEnc(songId) +
                        "&level=" + chain[q];
        if (!anon.empty()) u += "&cookie=" + UriEnc(NgTrimCookie(anon));
        JVal r;
        if (!Json(NgHttp(Wide(u)), r)) continue;
        const JVal* d = r.get("data");
        const JVal* first = d ? d->at(0) : nullptr;
        const JVal* url = first ? first->get("url") : nullptr;
        if (url && !url->str().empty()) {
            if (gotLevel) *gotLevel = q;
            return Wide(url->str());
        }
    }
    err = L"这首歌没有可用地址（可能要 VIP 或版权受限）";
    return L"";
}

/* NetEase cookies are long (2~3KB); passing the whole thing in a URL exceeds WinInet's 2083-character limit,
   so keep only the two auth fields MUSIC_U and __csrf */
std::string NgTrimCookie(const std::string& full) {
    auto pick = [&](const char* k) -> std::string {
        std::string key = std::string(k) + "=";
        size_t p = full.find(key);
        if (p == std::string::npos) return "";
        size_t e = full.find(';', p);
        return full.substr(p, (e == std::string::npos) ? (full.size() - p) : (e - p));
    };
    std::string a = pick("MUSIC_U");
    std::string b = pick("__csrf");
    if (a.empty()) return full;
    return b.empty() ? a : (a + "; " + b);
}

int NgExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err) {
    std::string enc = cookie.empty() ? "" : UriEnc(NgTrimCookie(cookie));
    std::string ck  = enc.empty() ? "" : ("&cookie=" + enc);   // append after an existing ?
    std::string ckQ = enc.empty() ? "" : ("?cookie=" + enc);   // first query parameter

    // 1) current account uid
    std::string accRaw = NgHttp(L"http://127.0.0.1:4000/user/account" + Wide(ckQ));
    if (accRaw.empty()) { err = L"接口服务未启动（netease-api）"; return -1; }
    JVal acc;
    if (!Json(accRaw, acc)) { err = L"账号接口返回异常：" + Wide(accRaw.substr(0, 80)); return -1; }
    const JVal* codeJ = acc.get("code");
    long long code = codeJ ? codeJ->ll() : 200;
    if (code == 301 || code == 401 || code == 403) { err = L"登录已失效，请重新登录"; return -1; }
    const JVal* ad = acc.get("account");
    const JVal* uid = ad ? ad->get("id") : nullptr;
    if (!uid || uid->ll() == 0) { err = L"还没登录网易云"; return -1; }

    // 2) find "Liked Music"
    JVal pl;
    if (!Json(NgHttp(L"http://127.0.0.1:4000/user/playlist?uid=" +
                   Wide(std::to_string(uid->ll())) + Wide(ck)), pl)) {
        err = L"拉歌单超时";
        return -1;
    }
    const JVal* arr = pl.get("playlist");
    if (!arr || arr->count() == 0) { err = L"歌单为空"; return -1; }

    long long pid = 0;
    for (int i = 0; i < arr->count() && !pid; i++) {
        const JVal* p = arr->at(i);
        const JVal* sp = p ? p->get("specialType") : nullptr;
        const JVal* id = p ? p->get("id") : nullptr;
        if (sp && sp->ll() == 5 && id) pid = id->ll();      // 5 = liked music
    }
    if (!pid) { const JVal* id = arr->at(0)->get("id"); if (id) pid = id->ll(); }
    if (!pid) { err = L"没找到喜欢的歌单"; return -1; }

    // 3) fetch tracks, building the TSV as we go
    std::string out = "title\tartist\talbum\tseconds\tcover\thash\tmixsongid\talbum_id\tcoverUrl\n";
    int n = 0;
    for (int off = 0; off < 5000; off += 100) {
        // NetEase intermittently returns 502; retry each page a few times before giving up
        JVal r;
        const JVal* songs = nullptr;
        std::string u = "http://127.0.0.1:4000/playlist/track/all?id=" + std::to_string(pid) +
                        "&limit=100&offset=" + std::to_string(off) + ck;
        for (int attempt = 0; attempt < 8; attempt++) {
            std::string raw = NgHttp(Wide(u));
            if (!raw.empty() && Json(raw, r)) {
                songs = r.get("songs");
                if (songs && songs->count() > 0) break;
            }
            Sleep(400 + attempt * 350);
        }
        if (!songs || songs->count() == 0) {
            if (n > 0) continue;               // skip this page instead of dropping the whole playlist
            err = L"歌单接口一直返回 502，稍后重试";
            return -1;
        }

        for (int i = 0; i < songs->count(); i++) {
            const JVal* s = songs->at(i);
            if (!s) continue;
            const JVal* nm = s->get("name");
            std::string title = Clean(nm ? nm->str() : "");
            if (title.empty()) continue;

            std::string artist;
            const JVal* ar = s->get("ar");
            if (ar && ar->t == JVal::ARR) {
                for (int k = 0; k < ar->count(); k++) {
                    const JVal* a = ar->at(k)->get("name");
                    if (a && !a->str().empty()) { if (!artist.empty()) artist += "、"; artist += a->str(); }
                }
            }
            std::string album;
            const JVal* al = s->get("al");
            const JVal* aln = al ? al->get("name") : nullptr;
            if (aln) album = aln->str();
            std::string cover;
            const JVal* pic = al ? al->get("picUrl") : nullptr;
            if (pic) cover = pic->str();

            const JVal* dt = s->get("dt");
            long long sec = dt ? dt->ll() / 1000 : 0;
            const JVal* id = s->get("id");
            std::string sid = id ? std::to_string(id->ll()) : "";

            out += Clean(title) + "\t" + Clean(artist) + "\t" + Clean(album) + "\t" +
                   std::to_string(sec) + "\t\t" + sid + "\t0\t0\t" + cover + "\n";
            n++;
        }
        if (songs->count() < 100) break;
    }
    if (n == 0) { err = L"歌单里没有歌曲"; return -1; }

    FILE* f = nullptr;
    _wfopen_s(&f, outTsv.c_str(), L"wb");
    if (!f) { err = L"写 tsv 失败"; return -1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return n;
}
/* ══════════════ QQ Music (port 3200) ══════════════ */

static std::string g_qqKey;
static std::mutex  g_qqMx;
static ULONGLONG   g_qqLast = 0;
static int         g_qqGap  = 400;

static std::string QqHttp(const std::wstring& url) {
    std::lock_guard<std::mutex> lk(g_qqMx);
    ULONGLONG now = GetTickCount64();
    if (now - g_qqLast < (ULONGLONG)g_qqGap) Sleep((DWORD)(g_qqGap - (now - g_qqLast)));
    g_qqLast = GetTickCount64();
    std::string r = Http(url);
    if (r.empty()) g_qqGap = (g_qqGap < 2000) ? g_qqGap + 200 : 2000;
    else if (g_qqGap > 400) g_qqGap -= 50;
    return r;
}

/* Session: after login the API identifies callers via qqmusic_session=<token>; the full cookie is too long, so extract only this */
static std::string QqSessionParam(const std::string& cookie) {
    if (cookie.empty()) return "";
    size_t p = cookie.find("qqmusic_session=");
    if (p == std::string::npos) return "&cookie=" + UriEnc(cookie);
    size_t e = cookie.find(';', p);
    std::string kv = cookie.substr(p, (e == std::string::npos) ? std::string::npos : (e - p));
    return "&cookie=" + UriEnc(kv);
}

bool QqLoginStart(const std::wstring& qrPath, std::wstring& err) {
    g_qqKey.clear();
    for (int i = 0; i < 4 && g_qqKey.empty(); i++) {
        JVal k;
        if (!Json(QqHttp(L"http://127.0.0.1:3200/login/qr/key"), k)) { Sleep(600); continue; }
        const JVal* d = k.get("data");
        const JVal* u = d ? d->get("unikey") : nullptr;
        if (u && !u->str().empty()) g_qqKey = u->str();
        else Sleep(600);
    }
    if (g_qqKey.empty()) { err = L"拿二维码 key 失败（qq-api 未启动？）"; return false; }

    JVal c;
    if (!Json(QqHttp(L"http://127.0.0.1:3200/login/qr/create?key=" + Wide(UriEnc(g_qqKey)) +
                     L"&qrimg=true"), c)) {
        err = L"生成二维码超时";
        return false;
    }
    const JVal* cd = c.get("data");
    const JVal* img = cd ? cd->get("qrimg") : nullptr;
    if (!img) { err = L"接口没返回二维码图"; return false; }

    std::string s = img->str();
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = s.substr(comma + 1);
    if (!SaveBase64Png(s, qrPath)) { err = L"二维码写入失败"; return false; }
    return true;
}

int QqLoginPoll(std::string& cookie, std::wstring& err) {
    if (g_qqKey.empty()) { err = L"没有二维码 key"; return 4; }
    JVal r;
    std::string raw = QqHttp(L"http://127.0.0.1:3200/login/qr/check?key=" + Wide(UriEnc(g_qqKey)));
    if (raw.empty() || !Json(raw, r)) return 0;          // keep waiting on transient glitches

    const JVal* c = r.get("code");
    int code = c ? (int)c->ll() : -1;
    if (code == 800) { err = L"二维码过期了，重新点一次「重新登录」"; return 3; }
    if (code == 801 || code == -1) return 0;
    if (code == 802) return 1;                            // scanned
    if (code != 803) return 0;

    const JVal* ck = r.get("cookie");
    std::string ck2;
    if (ck && ck->t == JVal::STR) ck2 = ck->str();
    if (ck2.empty()) { err = L"登录成功但没拿到会话"; return 4; }
    cookie = ck2;
    return 2;
}

std::wstring QqResolveUrl(const std::string& mid, const std::string& cookie,
                          int level, std::wstring& err, int* gotLevel) {
    if (mid.empty()) { err = L"没有歌曲 mid"; return L""; }

    std::string tail = QqSessionParam(cookie);          // "" or "&cookie=xxx"
    if (!tail.empty() && tail[0] == '&') tail = tail.substr(1);

    // Not logged in: M500 only (~128k); higher tiers may be available after login
    const char* lv[3] = { "", "level=exhigh", "level=lossless" };
    int want = level < 0 ? 0 : (level > 2 ? 2 : level);

    for (int q = want; q >= 0; q--) {
        std::string query = lv[q] ? std::string(lv[q]) : "";
        if (!query.empty() && !tail.empty()) query = tail + "&" + query;
        else if (query.empty()) query = tail;

        std::string u = "http://127.0.0.1:3200/getMusicPlay/" + UriEnc(mid);
        if (!query.empty()) u += "?" + query;

        JVal r;
        if (!Json(QqHttp(Wide(u)), r)) continue;
        const JVal* d = r.get("data");
        const JVal* pu = d ? d->get("playUrl") : nullptr;
        const JVal* one = pu ? pu->get(mid.c_str()) : nullptr;
        const JVal* url = one ? one->get("url") : nullptr;
        if (url && !url->str().empty()) {
            if (gotLevel) *gotLevel = q;
            return Wide(url->str());
        }
    }
    err = L"这首歌没有可用地址（可能要 VIP 或版权受限）";
    return L"";
}
int QqExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err) {
    std::string out = "title\tartist\talbum\tseconds\tcover\thash\tmixsongid\talbum_id\tcoverUrl\n";
    int n = 0;
    for (int off = 0; off < 5000; off += 100) {
        JVal r;
        const JVal* songs = nullptr;
        std::string u = "http://127.0.0.1:3200/user/liked-songs?offset=" + std::to_string(off) +
                        "&limit=100" + QqSessionParam(cookie);
        for (int attempt = 0; attempt < 5; attempt++) {
            std::string raw = QqHttp(Wide(u));
            if (!raw.empty() && Json(raw, r)) {
                songs = r.get("songs");
                break;
            }
            Sleep(500 + attempt * 300);
        }
        if (!songs || songs->count() == 0) break;

        for (int i = 0; i < songs->count(); i++) {
            const JVal* s = songs->at(i);
            if (!s) continue;
            const JVal* nm = s->get("name");
            if (!nm && s->get("title")) nm = s->get("title");
            std::string title = Clean(nm ? nm->str() : "");
            if (title.empty()) continue;

            std::string artist;
            const JVal* sg = s->get("singer");
            if (sg && sg->t == JVal::ARR) {
                for (int k2 = 0; k2 < sg->count(); k2++) {
                    const JVal* a = sg->at(k2)->get("name");
                    if (a && !a->str().empty()) { if (!artist.empty()) artist += "、"; artist += a->str(); }
                }
            }
            std::string album, albumMid;
            const JVal* al = s->get("album");
            if (al) {
                const JVal* an = al->get("name");
                if (an) album = an->str();
                const JVal* am = al->get("mid");
                if (am) albumMid = am->str();
            }
            std::string cover;
            if (!albumMid.empty())
                cover = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + albumMid + ".jpg";

            const JVal* iv = s->get("interval");
            long long sec = iv ? iv->ll() : 0;
            const JVal* md = s->get("mid");
            std::string mid = md ? md->str() : "";

            out += Clean(title) + "\t" + Clean(artist) + "\t" + Clean(album) + "\t" +
                   std::to_string(sec) + "\t\t" + mid + "\t0\t0\t" + cover + "\n";
            n++;
        }
        if (songs->count() < 100) break;
    }
    if (n == 0) { err = L"没拉到喜欢的歌（未登录或接口异常）"; return -1; }

    FILE* f = nullptr;
    _wfopen_s(&f, outTsv.c_str(), L"wb");
    if (!f) { err = L"写 tsv 失败"; return -1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return n;
}
/* ══════════════ Cross-platform matching: search the local platform for similar tracks by title + artist ══════════════ */

static std::string Lower(const std::string& s) {
    std::string o;
    for (char c : s) o += (char)tolower((unsigned char)c);
    return o;
}

/* Strip common suffixes: (Live) (Remaster) (Explicit) etc. */
static std::string NormTitle(const std::string& s) {
    std::string o;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[' || c == L'（') { depth++; continue; }
        if (c == ')' || c == ']' || c == L'）') { if (depth) depth--; continue; }
        if (depth) continue;
        o += c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '-')) o.pop_back();
    return Lower(o);
}

static int Score(const std::string& t1, const std::string& a1,
                 const std::string& t2, const std::string& a2, int dur2, int wantDur) {
    int sc = 0;
    std::string n1 = NormTitle(t1), n2 = NormTitle(t2);
    if (n1 == n2) sc += 60;
    else if (n2.find(n1) != std::string::npos || n1.find(n2) != std::string::npos) sc += 30;

    std::string l1 = Lower(a1), l2 = Lower(a2);
    if (!l1.empty() && !l2.empty()) {
        if (l1 == l2) sc += 30;
        else if (l2.find(l1) != std::string::npos || l1.find(l2) != std::string::npos) sc += 20;
    }
    if (wantDur > 0 && dur2 > 0 && abs(wantDur - dur2) <= 3) sc += 10;
    return sc;
}

/* Search on the source platform; returns true on a hit and fills hash */
bool MatchOnPlatform(int source, const std::string& title, const std::string& artist,
                     int wantDur, std::string& hashOut, long long& mixIdOut, long long& albumIdOut,
                     std::wstring& err) {
    std::string q = title;
    if (!artist.empty()) q += " " + artist;
    std::string enc = UriEnc(q);

    std::string base;
    if (source == 2)      base = "http://127.0.0.1:3200/search?keyword=" + enc + "&limit=10";
    else if (source == 1) base = "http://127.0.0.1:4000/search?keywords=" + enc + "&limit=10";
    else                  base = "http://127.0.0.1:3000/search?keyword=" + enc + "&page=1&pagesize=10";

    std::string raw = (source == 1) ? NgHttp(Wide(base)) : (source == 2 ? QqHttp(Wide(base)) : Http(Wide(base)));
    if (raw.empty()) { err = L"搜索接口没响应"; return false; }

    JVal r;
    if (!Json(raw, r)) { err = L"搜索返回无法解析"; return false; }

    int best = -1;
    std::string bestHash;
    long long curMixId = 0, curAlbumId = 0, bestMix = 0, bestAlbum = 0;

    auto pick = [](const JVal* it, const char* k1, const char* k2) -> const JVal* {
        const JVal* v = it->get(k1);
        if (!v && k2) v = it->get(k2);
        return v;
    };

    auto tryItem = [&](const JVal* it) {
        if (!it) return;
        const JVal* nm = (source == 0) ? pick(it, "SongName", "name") : pick(it, "name", "title");
        if (!nm) return;
        std::string t2 = nm->str();

        std::string a2, h, dur;
        const JVal* id = nullptr;
        if (source == 2) {
            const JVal* sg = it->get("singer");
            if (sg && sg->t == JVal::ARR && sg->count() > 0) {
                const JVal* an = sg->at(0)->get("name");
                if (an) a2 = an->str();
            }
            id = it->get("mid");
            const JVal* iv = it->get("interval");
            if (iv) dur = std::to_string(iv->ll());
        } else if (source == 1) {
            const JVal* ar = it->get("ar");
            if (ar && ar->t == JVal::ARR && ar->count() > 0) {
                const JVal* an = ar->at(0)->get("name");
                if (an) a2 = an->str();
            }
            id = it->get("id");
            const JVal* dt = it->get("dt");
            if (dt) dur = std::to_string(dt->ll() / 1000);
        } else {
            const JVal* sg = pick(it, "SingerName", "singername");
            if (sg) a2 = sg->str();
            id = pick(it, "FileHash", "hash");
            const JVal* du = pick(it, "Duration", "duration");
            if (du) dur = std::to_string(du->ll());
            const JVal* mix = pick(it, "MixSongID", nullptr);
            const JVal* alb = pick(it, "AlbumID", nullptr);
            if (mix) curMixId = mix->ll();
            if (alb) curAlbumId = alb->ll();
        }
        if (!id) return;
        std::string idStr = (id->t == JVal::NUM) ? std::to_string(id->ll()) : id->str();
        if (idStr.empty()) return;

        int sc = Score(title, artist, t2, a2, dur.empty() ? 0 : atoi(dur.c_str()), wantDur);
        if (sc > best) { best = sc; bestHash = idStr; bestMix = curMixId; bestAlbum = curAlbumId; }
    };

    if (source == 2) {
        const JVal* items = r.get("items");
        if (items && items->t == JVal::ARR) for (int i = 0; i < items->count(); i++) tryItem(items->at(i));
    } else if (source == 1) {
        const JVal* res = r.get("result");
        const JVal* songs = res ? res->get("songs") : nullptr;
        if (songs && songs->t == JVal::ARR) for (int i = 0; i < songs->count(); i++) tryItem(songs->at(i));
    } else {
        const JVal* data = r.get("data");
        const JVal* lists = data ? data->get("lists") : nullptr;
        if (lists && lists->t == JVal::ARR) for (int i = 0; i < lists->count(); i++) tryItem(lists->at(i));
    }

    if (best < 60) { err = L"没有匹配的版本"; return false; }
    hashOut = bestHash;
    mixIdOut = bestMix;
    albumIdOut = bestAlbum;
    return true;
}
