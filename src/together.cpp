#include "together.h"
#include "log.h"

#include <windows.h>
#include <wininet.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "wininet.lib")

/*
 * 一起听（中继版）
 *
 * 真正的 MQTT 由本地的 together-server.js 负责（Node，exe 里内嵌）。
 * 这里只跟 127.0.0.1:4100 说 HTTP，避免在 Win32 里手写 TLS/MQTT。
 */

namespace {

const int   kPort = 4100;

std::mutex          g_mx;
std::string         g_myCode;
std::string         g_peerNick;
std::string         g_nick;
bool                g_joined = false;
std::thread         g_pollThread;
std::atomic<bool>   g_run{ false };
std::deque<TogetherEvent> g_evq;

/* 上一次看到的对端状态，用来判断有没有变化 */
std::string g_lastPeerSig;

std::string HttpGet(const std::wstring& url, int timeoutMs = 8000) {
    HINTERNET net = InternetOpenW(L"Floral/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return "";
    DWORD t = timeoutMs;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

    HINTERNET req = InternetOpenUrlW(net, url.c_str(), nullptr, 0,
                                     INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD, 0);
    if (!req) { InternetCloseHandle(net); return ""; }

    std::string out;
    char buf[2048];
    DWORD got = 0;
    while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) out.append(buf, got);
    InternetCloseHandle(req);
    InternetCloseHandle(net);
    return out;
}

/* 极简 JSON 取值（只认字符串/数字，够用） */
std::string Val(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    if (p >= j.size()) return "";
    if (j[p] == '"') {
        p++;
        std::string out;
        for (; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) p++;
            out += j[p];
        }
        return out;
    }
    size_t e = p;
    while (e < j.size() && (isdigit((unsigned char)j[e]) || j[e] == '-' || j[e] == '.')) e++;
    return j.substr(p, e - p);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string UrlEnc(const std::string& s) {
    std::string o;
    char b[8];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { sprintf(b, "%%%02X", c); o += b; }
    }
    return o;
}

std::string JsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

/* 轮询对端状态 */
void PollLoop() {
    while (g_run) {
        std::string r = HttpGet(L"http://127.0.0.1:4100/state", 4000);
        if (!r.empty()) {
            bool skip = r.find("\"skip\":true") != std::string::npos;
            if (skip) {
                TogetherEvent ev; ev.kind = 5;
                std::lock_guard<std::mutex> lk(g_mx);
                g_evq.push_back(ev);
            }
            std::string peer = Val(r, "peer");
            if (!peer.empty()) {
                // data 子对象：取我们关心的字段
                std::string sig = r;
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (sig != g_lastPeerSig) {
                        g_lastPeerSig = sig;
                        bool first = g_peerNick != peer;
                        g_peerNick = peer;
                        if (first) {
                            TogetherEvent e2; e2.kind = 1; e2.nick = peer;
                            g_evq.push_back(e2);
                        }
                        TogetherEvent ev;
                        ev.kind = 2;
                        ev.nick = peer;
                        ev.song = Val(r, "s");
                        ev.src = Val(r, "src");
                        ev.title = Val(r, "ti");
                        ev.artist = Val(r, "ar");
                        ev.posMs = atoi(Val(r, "pos").c_str());
                        ev.playing = Val(r, "play") == "1";
                        ev.stampMs = _strtoi64(Val(r, "t").c_str(), nullptr, 10);
                        g_evq.push_back(ev);
                    } else if (g_peerNick != peer) {
                        g_peerNick = peer;
                    }
                }
            }
        }
        for (int i = 0; i < 10 && g_run; i++) Sleep(100);
    }
}

}  // namespace

/* 在场心跳：只带昵称，不带播放进度。让对面能认出"谁在房间里" */
void TogetherSayHello() {
    if (!TogetherActive()) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"n\":\"%s\",\"hello\":1}", JsonEsc(g_nick).c_str());
    std::string enc = UrlEnc(buf);
    HttpGet(L"http://127.0.0.1:4100/push?data=" + Widen(enc), 3000);
}

/* 默认昵称：计算机名，短一点；取不到就用角色名 */
std::string TogetherDefaultNick(bool host) {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD n = sizeof(name);
    std::string s;
    if (GetComputerNameA(name, &n) && n > 0) s.assign(name, n);
    if (s.empty()) s = host ? "房主" : "朋友";
    if (s.size() > 24) s.resize(24);
    return s;
}
bool TogetherHost(const std::string& nick, std::string& codeOut, std::wstring& err, int) {
    TogetherStop();
    g_nick = nick.empty() ? "房主" : nick;

    std::string r = HttpGet(L"http://127.0.0.1:4100/open?nick=" + Widen(UrlEnc(g_nick)), 20000);
    if (r.empty()) { err = L"一起听服务没起来（日志里有原因）"; return false; }
    std::string code = Val(r, "code");
    if (code.empty()) { err = L"开房失败：服务没有返回房间码"; return false; }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_myCode = code;
        g_joined = true;
        g_lastPeerSig.clear();
    }
    codeOut = code;
    g_run = true;
    g_pollThread = std::thread(PollLoop);
    Logf("TOG", "MQTT 开房成功 code=%s", code.c_str());
    return true;
}

bool TogetherJoin(const std::string& code, const std::string& nick, std::wstring& err) {
    TogetherStop();
    g_nick = nick.empty() ? "朋友" : nick;

    std::string r = HttpGet(L"http://127.0.0.1:4100/join?code=" + Widen(UrlEnc(code)) +
                            L"&nick=" + Widen(UrlEnc(g_nick)), 20000);
    if (r.empty()) { err = L"一起听服务没起来"; return false; }
    if (r.find("\"ok\":true") == std::string::npos) {
        err = L"加入失败：房间码不对或网络不通";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_myCode = code;
        g_joined = true;
        g_lastPeerSig.clear();
    }
    g_run = true;
    g_pollThread = std::thread(PollLoop);
    Logf("TOG", "MQTT 加入成功 code=%s", code.c_str());
    return true;
}

void TogetherStop() {
    Logf("TOG", "停止一起听（joinable=%d）", g_pollThread.joinable() ? 1 : 0);
    g_run = false;
    if (g_pollThread.joinable()) {
        if (g_pollThread.get_id() == std::this_thread::get_id()) {
            g_pollThread.detach();          // 自己线程里不能 join 自己
        } else {
            g_pollThread.join();
        }
    }
    if (g_joined) HttpGet(L"http://127.0.0.1:4100/close", 3000);
    std::lock_guard<std::mutex> lk(g_mx);
    g_joined = false;
    g_myCode.clear();
    g_peerNick.clear();
    g_lastPeerSig.clear();
    g_evq.clear();
}

bool TogetherActive() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_joined;
}

std::string TogetherPeerNick() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_peerNick;
}

std::string TogetherMyCode() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_myCode;
}

/* UTC 纪元毫秒（跨机器可比；GetTickCount64 只有本机意义） */
unsigned long long TogetherNowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v;
    v.LowPart  = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (v.QuadPart - 116444736000000000ULL) / 10000ULL;
}
void TogetherPushState(const std::string& song, const std::string& src,
                       int posMs, bool playing,
                       const std::string& title, const std::string& artist) {
    if (!TogetherActive()) return;
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"n\":\"%s\",\"s\":\"%s\",\"src\":\"%s\",\"ti\":\"%s\",\"ar\":\"%s\",\"pos\":%d,\"play\":%d,\"t\":%llu}",
             JsonEsc(g_nick).c_str(), JsonEsc(song).c_str(), JsonEsc(src).c_str(),
             JsonEsc(title).c_str(), JsonEsc(artist).c_str(),
             posMs, playing ? 1 : 0, TogetherNowMs());
    std::string enc = UrlEnc(buf);                 // 必须先落地：两个临时对象取 begin/end 是 UB
    std::wstring url = L"http://127.0.0.1:4100/push?data=" + Widen(enc);
    HttpGet(url, 3000);
}

void TogetherRequestSkip() {
    if (!TogetherActive()) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"n\":\"%s\",\"skip\":1}", JsonEsc(g_nick).c_str());
    std::string enc = UrlEnc(buf);
    HttpGet(L"http://127.0.0.1:4100/push?data=" + Widen(enc), 3000);
    Logf("TOG", "已请求房主跳过这首");
}

bool TogetherPoll(TogetherEvent& ev) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_evq.empty()) return false;
    ev = g_evq.front();
    g_evq.pop_front();
    return true;
}
