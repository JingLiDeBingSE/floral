#include "control.h"
#include "sec.h"
#include "logwin.h"
#include "../src/json.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace ctl {
namespace {

constexpr int kMaxBindings = 3;          // one Floral identity: one account per platform, at most three
constexpr int kMaxFriends = 200;
constexpr int kMaxFailed = 5;            // failed verifications before the pair is locked
constexpr unsigned long long kLockMs = 10 * 60 * 1000;
constexpr size_t kMaxLine = 64 * 1024;

struct Binding {
    std::string platform, uid;
    long long at = 0;
};

struct Account {
    std::string id, name, secret;
    long long createdAt = 0;
    std::vector<Binding> bindings;
    std::vector<std::string> friends;
};

struct Pending {
    std::string from;     // the account that asked
    std::string to;
};

struct LockEntry {
    int fails = 0;
    unsigned long long until = 0;
};

std::recursive_mutex g_mx;      // Log() is called from inside handlers that already hold it
std::mutex g_logMx;
std::vector<Account> g_accounts;
std::vector<Pending> g_pending;                       // friend requests not answered yet
std::map<std::string, LockEntry> g_locks;             // "platform|uid" → failures
std::map<std::string, SOCKET> g_online;               // account id → live control socket
std::string g_dataDir, g_accountsPath, g_backupDir, g_logPath;

/* ── log ─────────────────────────────────────────────────────────────── */
void Log(const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d  [ctl] ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    const std::string line = std::string(stamp) + text;

    std::lock_guard<std::mutex> lk(g_logMx);
    logwin::Console(line);
    logwin::File(g_logPath, line);
}

/* ── serialisation ───────────────────────────────────────────────────── */
std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    return out;
}

std::string AccountsJson() {
    std::string s = "{\"accounts\":[";
    for (size_t i = 0; i < g_accounts.size(); i++) {
        const Account& a = g_accounts[i];
        if (i) s += ',';
        s += "{\"id\":\"" + Escape(a.id) + "\",\"name\":\"" + Escape(a.name) +
             "\",\"secret\":\"" + Escape(a.secret) + "\",\"createdAt\":" + std::to_string(a.createdAt) +
             ",\"bindings\":[";
        for (size_t b = 0; b < a.bindings.size(); b++) {
            if (b) s += ',';
            s += "{\"platform\":\"" + Escape(a.bindings[b].platform) + "\",\"uid\":\"" +
                 Escape(a.bindings[b].uid) + "\",\"at\":" + std::to_string(a.bindings[b].at) + "}";
        }
        s += "],\"friends\":[";
        for (size_t f = 0; f < a.friends.size(); f++) {
            if (f) s += ',';
            s += "\"" + Escape(a.friends[f]) + "\"";
        }
        s += "]}";
    }
    s += "],\"pending\":[";
    for (size_t i = 0; i < g_pending.size(); i++) {
        if (i) s += ',';
        s += "{\"from\":\"" + Escape(g_pending[i].from) + "\",\"to\":\"" + Escape(g_pending[i].to) + "\"}";
    }
    s += "]}";
    return s;
}

bool SaveLocked() {
    const std::string tmp = g_accountsPath + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
    const std::string body = AccountsJson();
    const size_t put = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (put != body.size()) return false;
    // Replace atomically: a crash mid-write leaves the previous file intact
    return MoveFileExA(tmp.c_str(), g_accountsPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void BackupLocked() {
    if (g_backupDir.empty()) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof(name), "accounts-%04d%02d%02d.json", st.wYear, st.wMonth, st.wDay);
    const std::string dst = g_backupDir + "\\" + name;
    if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // one per day
    CopyFileA(g_accountsPath.c_str(), dst.c_str(), TRUE);

    // Keep a week
    WIN32_FIND_DATAA fd{};
    std::vector<std::string> files;
    HANDLE h = FindFirstFileA((g_backupDir + "\\accounts-*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { files.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    while (files.size() > 7) {
        DeleteFileA((g_backupDir + "\\" + files.front()).c_str());
        files.erase(files.begin());
    }
}

bool LoadLocked() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_accountsPath.c_str(), "rb") != 0 || !f) return true;   // no file yet is fine
    std::string body;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);

    JVal root;
    if (!Json(body, root)) { Log("accounts.json 解析失败，按空库启动"); return true; }
    if (const JVal* list = root.get("accounts")) {
        for (const JVal& a : list->a) {
            Account acc;
            if (const JVal* v = a.get("id")) acc.id = v->str();
            if (const JVal* v = a.get("name")) acc.name = v->str();
            if (const JVal* v = a.get("secret")) acc.secret = v->str();
            if (const JVal* v = a.get("createdAt")) acc.createdAt = v->ll();
            if (const JVal* bs = a.get("bindings"))
                for (const JVal& b : bs->a) {
                    Binding bd;
                    if (const JVal* v = b.get("platform")) bd.platform = v->str();
                    if (const JVal* v = b.get("uid")) bd.uid = v->str();
                    if (const JVal* v = b.get("at")) bd.at = v->ll();
                    if (!bd.platform.empty() && !bd.uid.empty()) acc.bindings.push_back(bd);
                }
            if (const JVal* fs = a.get("friends"))
                for (const JVal& fid : fs->a) acc.friends.push_back(fid.str());
            if (!acc.id.empty()) g_accounts.push_back(std::move(acc));
        }
    }
    if (const JVal* ps = root.get("pending"))
        for (const JVal& p : ps->a) {
            Pending pd;
            if (const JVal* v = p.get("from")) pd.from = v->str();
            if (const JVal* v = p.get("to")) pd.to = v->str();
            if (!pd.from.empty() && !pd.to.empty()) g_pending.push_back(pd);
        }
    return true;
}

/* ── helpers ─────────────────────────────────────────────────────────── */
Account* FindById(const std::string& id) {
    for (Account& a : g_accounts) if (a.id == id) return &a;
    return nullptr;
}

Account* FindByBinding(const std::string& platform, const std::string& uid) {
    for (Account& a : g_accounts)
        for (const Binding& b : a.bindings)
            if (b.platform == platform && b.uid == uid) return &a;
    return nullptr;
}

bool HasFriend(const Account& a, const std::string& id) {
    return std::find(a.friends.begin(), a.friends.end(), id) != a.friends.end();
}

bool AddFriendPair(const std::string& a, const std::string& b) {
    Account* pa = FindById(a);
    Account* pb = FindById(b);
    if (!pa || !pb || a == b) return false;
    if ((int)pa->friends.size() >= kMaxFriends || (int)pb->friends.size() >= kMaxFriends) return false;
    if (!HasFriend(*pa, b)) pa->friends.push_back(b);
    if (!HasFriend(*pb, a)) pb->friends.push_back(a);
    return true;
}

std::string RandomId(int len) {
    static const char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no I/O/0/1
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, (int)sizeof(kChars) - 2);
    std::string s;
    for (int i = 0; i < len; i++) s += kChars[dist(gen)];
    return s;
}

std::string RandomSecret() {
    static const char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string s;
    for (int i = 0; i < 48; i++) s += kHex[dist(gen)];
    return s;
}

long long NowSec() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

void SendLine(SOCKET s, const std::string& text) {
    if (s == INVALID_SOCKET) return;
    const std::string line = text + "\n";
    int sent = 0;
    while (sent < (int)line.size()) {
        const int n = send(s, line.data() + sent, (int)line.size() - sent, 0);
        if (n <= 0) return;
        sent += n;
    }
}

/* Pushes a message to a connected account; silently ignored when it is offline */
void Push(const std::string& id, const std::string& json) {
    auto it = g_online.find(id);
    if (it == g_online.end()) return;
    SendLine(it->second, json);
}

/* ── message handlers ────────────────────────────────────────────────── */
struct Conn {
    SOCKET sock = INVALID_SOCKET;
    std::string id;        // set after hello
};

std::string ReadLine(SOCKET s, std::string& buffered) {
    for (;;) {
        const size_t nl = buffered.find('\n');
        if (nl != std::string::npos) {
            std::string line = buffered.substr(0, nl);
            buffered.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[4096];
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return "";
        buffered.append(buf, (size_t)n);
        if (buffered.size() > kMaxLine) return "";
    }
}

void HandleBind(Conn& c, const JVal& m) {
    const JVal* blobVal = m.get("blob");
    if (!blobVal) { SendLine(c.sock, "{\"t\":\"err\",\"code\":\"bad\"}"); return; }

    std::string cipher;
    std::string plain;
    if (!sec::Base64Decode(blobVal->str(), cipher) || !sec::RsaDecryptOaep(cipher, plain)) {
        SendLine(c.sock, "{\"t\":\"err\",\"code\":\"crypto\"}");
        return;
    }

    JVal inner;
    if (!Json(plain, inner)) { SendLine(c.sock, "{\"t\":\"err\",\"code\":\"bad\"}"); return; }
    const std::string platform = inner.get("platform") ? inner.get("platform")->str() : "";
    const std::string uid = inner.get("uid") ? inner.get("uid")->str() : "";
    const std::string token = inner.get("token") ? inner.get("token")->str() : "";
    if (platform.empty() || uid.empty() || token.empty()) {
        SendLine(c.sock, "{\"t\":\"err\",\"code\":\"bad\"}");
        return;
    }

    // Lockout: five failed attempts for the same pair cost ten minutes
    const std::string lockKey = platform + "|" + uid;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        auto it = g_locks.find(lockKey);
        if (it != g_locks.end() && it->second.until > (unsigned long long)NowSec() * 1000ULL) {
            SendLine(c.sock, "{\"t\":\"err\",\"code\":\"locked\"}");
            return;
        }
    }

    VerifyResult vr;
    if (platform == "kugou") vr = VerifyKugou(uid, token);
    else if (platform == "netease") vr = VerifyNetease(uid, token);
    else if (platform == "qq") vr = VerifyQQ(uid, token);
    else { SendLine(c.sock, "{\"t\":\"err\",\"code\":\"platform\"}"); return; }

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!vr.ok) {
        LockEntry& e = g_locks[lockKey];
        e.fails++;
        if (e.fails >= kMaxFailed) { e.until = (unsigned long long)NowSec() * 1000ULL + kLockMs; e.fails = 0; }
        Log("校验失败 %s:%s (%s)", platform.c_str(), uid.c_str(), vr.error.c_str());
        SendLine(c.sock, "{\"t\":\"err\",\"code\":\"verify\",\"reason\":\"" + Escape(vr.error) + "\"}");
        return;
    }
    g_locks.erase(lockKey);

    // Already bound → return the identity it belongs to
    if (Account* acc = FindByBinding(platform, uid)) {
        if (acc->name.empty()) acc->name = vr.name;
        SendLine(c.sock, "{\"t\":\"bound\",\"id\":\"" + Escape(acc->id) + "\",\"name\":\"" + Escape(acc->name) +
                         "\",\"secret\":\"" + Escape(acc->secret) + "\"}");
        Log("%s:%s 绑定到已有身份 %s", platform.c_str(), uid.c_str(), acc->id.c_str());
        return;
    }

    // New identity; the platform nickname seeds the display name once
    Account acc;
    acc.id = RandomId(8);
    while (FindById(acc.id)) acc.id = RandomId(8);
    acc.name = vr.name.empty() ? ("Floral " + acc.id.substr(0, 4)) : vr.name;
    acc.secret = RandomSecret();
    acc.createdAt = NowSec();
    acc.bindings.push_back({ platform, uid, acc.createdAt });
    g_accounts.push_back(acc);
    SaveLocked();
    BackupLocked();
    Log("新建身份 %s（%s:%s，%s）", acc.id.c_str(), platform.c_str(), uid.c_str(), acc.name.c_str());
    SendLine(c.sock, "{\"t\":\"bound\",\"id\":\"" + Escape(acc.id) + "\",\"name\":\"" + Escape(acc.name) +
                     "\",\"secret\":\"" + Escape(acc.secret) + "\"}");
}

void HandleHello(Conn& c, const JVal& m) {
    const std::string id = m.get("id") ? m.get("id")->str() : "";
    const std::string secret = m.get("secret") ? m.get("secret")->str() : "";
    if (id.empty() || secret.empty()) { SendLine(c.sock, "{\"t\":\"err\",\"code\":\"bad\"}"); return; }

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Account* acc = FindById(id);
    if (!acc || acc->secret != secret) {
        SendLine(c.sock, "{\"t\":\"err\",\"code\":\"auth\"}");
        return;
    }
    c.id = id;
    g_online[id] = c.sock;

    std::string online, pending;
    for (const std::string& f : acc->friends) {
        if (!online.empty()) online += ',';
        online += "{\"id\":\"" + Escape(f) + "\",\"online\":" +
                  (g_online.count(f) ? "true" : "false") + "}";
    }
    for (const Pending& p : g_pending) {
        if (p.to != id) continue;
        Account* from = FindById(p.from);
        if (!pending.empty()) pending += ',';
        pending += "{\"from\":\"" + Escape(p.from) + "\",\"name\":\"" +
                   Escape(from ? from->name : "") + "\"}";
    }
    SendLine(c.sock, "{\"t\":\"ok\",\"id\":\"" + Escape(id) + "\",\"name\":\"" + Escape(acc->name) +
                     "\",\"code\":\"" + Escape(id) + "\",\"friends\":[" + online + "],\"pending\":[" + pending + "]}");

    // Tell friends this account is back
    for (const std::string& f : acc->friends)
        Push(f, "{\"t\":\"presence\",\"id\":\"" + Escape(id) + "\",\"online\":true}");
}

void HandleAdd(Conn& c, const JVal& m) {
    const std::string code = m.get("code") ? m.get("code")->str() : "";
    if (c.id.empty() || code.empty()) { SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"bad\"}"); return; }

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Account* me = FindById(c.id);
    Account* target = FindById(code);
    if (!me) { SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"auth\"}"); return; }
    if (!target) { SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"nouser\"}"); return; }
    if (target->id == me->id) { SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"self\"}"); return; }
    if (HasFriend(*me, target->id)) { SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"already\"}"); return; }

    for (const Pending& p : g_pending)
        if (p.from == me->id && p.to == target->id) {
            SendLine(c.sock, "{\"t\":\"add\",\"ok\":false,\"reason\":\"pending\"}");
            return;
        }

    g_pending.push_back({ me->id, target->id });
    SaveLocked();
    SendLine(c.sock, "{\"t\":\"add\",\"ok\":true}");
    Push(target->id, "{\"t\":\"request\",\"from\":\"" + Escape(me->id) + "\",\"name\":\"" +
                     Escape(me->name) + "\"}");
    Log("%s 请求加 %s", me->id.c_str(), target->id.c_str());
}

void HandleAnswer(Conn& c, const JVal& m, bool accept) {
    const std::string uid = m.get("uid") ? m.get("uid")->str() : "";
    if (c.id.empty() || uid.empty()) return;

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Account* me = FindById(c.id);
    Account* from = FindById(uid);
    if (!me || !from) return;

    for (size_t i = 0; i < g_pending.size(); i++) {
        if (g_pending[i].from != uid || g_pending[i].to != c.id) continue;
        g_pending.erase(g_pending.begin() + (ptrdiff_t)i);
        if (accept && AddFriendPair(uid, c.id)) {
            SendLine(c.sock, "{\"t\":\"accepted\",\"ok\":true,\"id\":\"" + Escape(uid) +
                             "\",\"name\":\"" + Escape(from->name) + "\"}");
            Push(uid, "{\"t\":\"friend\",\"id\":\"" + Escape(c.id) + "\",\"name\":\"" + Escape(me->name) + "\"}");
            Push(c.id, "{\"t\":\"friend\",\"id\":\"" + Escape(uid) + "\",\"name\":\"" + Escape(from->name) + "\"}");
            Log("%s 和 %s 成为好友", uid.c_str(), c.id.c_str());
        } else {
            SendLine(c.sock, "{\"t\":\"accepted\",\"ok\":false}");
            Push(uid, "{\"t\":\"declined\",\"id\":\"" + Escape(c.id) + "\"}");
        }
        SaveLocked();
        BackupLocked();
        return;
    }
}

void HandleRename(Conn& c, const JVal& m) {
    const std::string name = m.get("name") ? m.get("name")->str() : "";
    if (c.id.empty() || name.empty() || name.size() > 32) return;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Account* me = FindById(c.id);
    if (!me) return;
    me->name = name;
    SaveLocked();
    BackupLocked();
    SendLine(c.sock, "{\"t\":\"renamed\",\"ok\":true,\"name\":\"" + Escape(name) + "\"}");
    for (const std::string& f : me->friends)
        Push(f, "{\"t\":\"renamed\",\"id\":\"" + Escape(me->id) + "\",\"name\":\"" + Escape(name) + "\"}");
}

void HandleInvite(Conn& c, const JVal& m) {
    const std::string uid = m.get("uid") ? m.get("uid")->str() : "";
    const std::string room = m.get("room") ? m.get("room")->str() : "";
    if (c.id.empty() || uid.empty() || room.empty()) return;

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Account* me = FindById(c.id);
    if (!me || !HasFriend(*me, uid)) return;
    if (!g_online.count(uid)) {
        SendLine(c.sock, "{\"t\":\"invited\",\"ok\":false,\"reason\":\"offline\"}");
        return;
    }
    SendLine(c.sock, "{\"t\":\"invited\",\"ok\":true}");
    Push(uid, "{\"t\":\"invite\",\"from\":\"" + Escape(me->id) + "\",\"name\":\"" + Escape(me->name) +
              "\",\"room\":\"" + Escape(room) + "\"}");
    Log("%s 邀请 %s 进入房间 %s", me->id.c_str(), uid.c_str(), room.c_str());
}

}  // namespace

bool Init(const std::string& dataDir) {
    g_dataDir = dataDir;
    g_accountsPath = dataDir + "\\accounts.json";
    g_backupDir = dataDir + "\\backup";
    g_logPath = dataDir + "\\control.log";
    CreateDirectoryA(dataDir.c_str(), nullptr);
    CreateDirectoryA(g_backupDir.c_str(), nullptr);

    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!LoadLocked()) { Log("accounts.json 读取失败"); return false; }
    BackupLocked();     // one snapshot per day, even when nothing changed
    Log("已加载 %d 个身份，%d 条待处理请求", (int)g_accounts.size(), (int)g_pending.size());
    return true;
}

void ServePrefetched(void* sockHandle, const std::string& firstLine) {
    const SOCKET s = (SOCKET)(uintptr_t)sockHandle;
    Conn c;
    c.sock = s;
    std::string buffered = firstLine + "\n";

    for (;;) {
        const std::string line = ReadLine(s, buffered);
        if (line.empty()) break;
        JVal m;
        if (!Json(line, m)) continue;
        const std::string t = m.get("t") ? m.get("t")->str() : "";
        if (t == "bind") HandleBind(c, m);
        else if (t == "hello") HandleHello(c, m);
        else if (t == "add") HandleAdd(c, m);
        else if (t == "accept") HandleAnswer(c, m, true);
        else if (t == "decline") HandleAnswer(c, m, false);
        else if (t == "rename") HandleRename(c, m);
        else if (t == "invite") HandleInvite(c, m);
        else if (t == "pubkey") {
            std::string jwk;
            std::string keyPath = g_dataDir.substr(0, g_dataDir.find_last_of("\\/")) + "\\keys\\server.key";
            if (sec::LoadPublicJwk(keyPath, jwk))
                SendLine(s, "{\"t\":\"pubkey\",\"jwk\":" + jwk + "}");
            else
                SendLine(s, "{\"t\":\"err\",\"code\":\"nokey\"}");
        }
        else if (t == "ping") SendLine(s, "{\"t\":\"pong\"}");
    }

    if (!c.id.empty()) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_online.count(c.id) && g_online[c.id] == s) g_online.erase(c.id);
        Account* acc = FindById(c.id);
        if (acc)
            for (const std::string& f : acc->friends)
                Push(f, "{\"t\":\"presence\",\"id\":\"" + Escape(c.id) + "\",\"online\":false}");
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
}

}  // namespace ctl
