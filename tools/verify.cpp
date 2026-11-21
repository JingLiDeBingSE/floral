/*
 * Platform account verification
 *
 * The server never stores a platform token: it is decrypted, used for one check, then dropped.
 *
 * Kugou and QQ require the platform's own request signing, so the server runs the very same sidecar
 * the client ships (kugou-api, qq-api) on loopback and asks it to identify the session. NetEase needs
 * no signing, so it can be called directly.
 *
 * FLORAL_VERIFY_STUB=1 accepts any credentials; local protocol tests only.
 */
#include "control.h"
#include "../src/json.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace ctl {
namespace {

constexpr int kKugouPort = 3000;
constexpr int kQqPort = 3200;
constexpr size_t kMaxResponse = 512 * 1024;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

/* Plain GET over WinHTTP; returns the body, or an empty string on failure */
std::string HttpGet(const std::wstring& url, const std::wstring& headers = std::wstring(), int timeoutMs = 8000) {
    std::string body;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = { 0 }, path[2048] = { 0 };
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return body;

    HINTERNET session = WinHttpOpen(L"FloralRelay/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); return body; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return body; }

    const wchar_t* hdr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
    const DWORD hdrLen = headers.empty() ? 0 : (DWORD)-1L;
    if (WinHttpSendRequest(req, hdr, hdrLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            body += chunk;
            if (body.size() > kMaxResponse) break;
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

std::string LocalUrl(int port, const std::string& pathAndQuery) {
    return "http://127.0.0.1:" + std::to_string(port) + pathAndQuery;
}

bool StubEnabled() {
    char buf[8] = { 0 };
    const DWORD n = GetEnvironmentVariableA("FLORAL_VERIFY_STUB", buf, sizeof(buf));
    return n > 0 && buf[0] == '1';
}

/* Pulls "key":"value" out of a JSON blob without a full parse */
std::string JsonField(const std::string& raw, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = raw.find(pat);
    if (p == std::string::npos) return "";
    p = raw.find('"', raw.find(':', p + pat.size()));
    if (p == std::string::npos) return "";
    const size_t e = raw.find('"', p + 1);
    if (e == std::string::npos) return "";
    return raw.substr(p + 1, e - p - 1);
}

}  // namespace

VerifyResult VerifyKugou(const std::string& uid, const std::string& token) {
    VerifyResult r;
    if (StubEnabled()) { r.ok = true; r.name = "酷狗测试用户"; return r; }

    // The local kugou-api does the platform signing; we only ask it who this session belongs to
    const std::string cookie = "userid=" + uid + ";token=" + token;
    const std::string raw = HttpGet(Widen(LocalUrl(kKugouPort, "/user/detail?cookie=" + UrlEncode(cookie))));
    if (raw.empty()) { r.error = "kugou-service-offline"; return r; }
    if (raw.find("\"status\":1") == std::string::npos) { r.error = "token-rejected"; return r; }

    const std::string uid2 = JsonField(raw, "userid");
    if (!uid2.empty() && uid2 != uid) { r.error = "uid-mismatch"; return r; }

    r.name = JsonField(raw, "nickname");
    r.ok = true;
    return r;
}

VerifyResult VerifyQQ(const std::string& uid, const std::string& cookie) {
    VerifyResult r;
    if (StubEnabled()) { r.ok = true; r.name = "QQ测试用户"; return r; }

    const std::string raw = HttpGet(Widen(LocalUrl(kQqPort, "/login/status?cookie=" + UrlEncode(cookie))));
    if (raw.empty()) { r.error = "qq-service-offline"; return r; }
    if (raw.find("qqmusic_session") == std::string::npos) { r.error = "token-rejected"; return r; }

    r.name = JsonField(raw, "nickname");
    (void)uid;
    r.ok = true;
    return r;
}

VerifyResult VerifyNetease(const std::string& uid, const std::string& cookie) {
    VerifyResult r;
    if (StubEnabled()) { r.ok = true; r.name = "网易云测试用户"; return r; }

    // NetEase needs no request signing, but the session must travel as a Cookie header:
    // the same cookie the client keeps in netease-session.json.
    const std::string raw = HttpGet(Widen("https://music.163.com/api/nuser/account/get"),
                                    Widen("Cookie: " + cookie + "\r\n"));
    if (raw.empty()) { r.error = "netease-unreachable"; return r; }

    const JVal* account = nullptr;
    JVal root;
    if (Json(raw, root)) {
        account = root.get("account");
        if (!account || account->t != JVal::OBJ) { r.error = "token-rejected"; return r; }
        const std::string uid2 = account->get("id") ? std::to_string(account->get("id")->ll()) : "";
        if (!uid2.empty() && !uid.empty() && uid2 != uid) { r.error = "uid-mismatch"; return r; }
        if (const JVal* profile = root.get("profile")) {
            if (const JVal* nick = profile->get("nickname")) r.name = nick->str();
        }
    } else {
        if (raw.find("\"account\":null") != std::string::npos) { r.error = "token-rejected"; return r; }
        r.error = "netease-bad-response";
        return r;
    }
    r.ok = true;
    return r;
}
}  // namespace ctl
