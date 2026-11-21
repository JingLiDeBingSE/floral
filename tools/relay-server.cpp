/*
 * Floral listen-together relay
 *
 * A dumb byte pipe. Two clients that name the same room get their frames forwarded to each other;
 * the server never parses the payload, which is encrypted end to end by the clients with a key
 * derived from the room code.
 *
 * Wire format
 *   client -> server, once:   {"room":"<16-64 hex chars>"}\n
 *   server -> client, once:   {"ok":true}\n   or   {"ok":false,"err":"..."}\n
 *   both directions after:    uint32 big-endian length, then that many payload bytes
 *
 * Build:  see scripts/build-relay.ps1  (produces floral-relay.exe, no runtime needed)
 * Run:    floral-relay.exe [port]     default port 443
 *
 * A room holds at most two connections. Once the first one leaves, the other keeps the room open
 * and a new peer may take the free slot.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "control.h"
#include "sec.h"
#include "logwin.h"

#pragma comment(lib, "ws2_32.lib")

namespace {


constexpr int kMaxPerRoom = 2;
constexpr size_t kMaxFrameBytes = 512 * 1024;
constexpr size_t kMaxHandshakeBytes = 4096;
constexpr int kHandshakeTimeoutMs = 10000;

std::mutex g_logMx;
std::string g_logPath;

void Log(const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d  ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    const std::string line = std::string(stamp) + text;

    std::lock_guard<std::mutex> lk(g_logMx);
    logwin::Console(line);
    logwin::File(g_logPath, line);
}

/* ── Connection registry ───────────────────────────────────────────────── */

struct Conn {
    SOCKET sock = INVALID_SOCKET;
    std::mutex sendMx;          // frames are written whole, one writer at a time
};

struct Room {
    std::vector<std::shared_ptr<Conn>> members;
};

struct Registry {
    std::mutex mx;
    std::map<std::string, Room> rooms;

    bool Join(const std::string& id, std::shared_ptr<Conn> c) {
        std::lock_guard<std::mutex> lk(mx);
        Room& r = rooms[id];
        if ((int)r.members.size() >= kMaxPerRoom) return false;
        r.members.push_back(std::move(c));
        return true;
    }

    void Leave(const std::string& id, const std::shared_ptr<Conn>& c) {
        std::lock_guard<std::mutex> lk(mx);
        auto it = rooms.find(id);
        if (it == rooms.end()) return;
        auto& v = it->second.members;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == c) { v.erase(v.begin() + (ptrdiff_t)i); break; }
        }
        if (v.empty()) rooms.erase(it);
    }

    /* Copies the peers of a room so the caller can send without holding the lock */
    std::vector<std::shared_ptr<Conn>> Peers(const std::string& id, const std::shared_ptr<Conn>& self) {
        std::lock_guard<std::mutex> lk(mx);
        std::vector<std::shared_ptr<Conn>> out;
        auto it = rooms.find(id);
        if (it == rooms.end()) return out;
        for (auto& m : it->second.members) if (m != self) out.push_back(m);
        return out;
    }
};

Registry g_reg;

bool SendAll(const std::shared_ptr<Conn>& c, const char* data, size_t size) {
    std::lock_guard<std::mutex> lk(c->sendMx);
    size_t sent = 0;
    while (sent < size) {
        int n = send(c->sock, data + sent, (int)(size - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool SendFrame(const std::shared_ptr<Conn>& c, const char* payload, size_t size) {
    if (size > kMaxFrameBytes) return false;
    std::vector<char> buf(4 + size);
    buf[0] = (char)((size >> 24) & 0xFF);
    buf[1] = (char)((size >> 16) & 0xFF);
    buf[2] = (char)((size >> 8) & 0xFF);
    buf[3] = (char)(size & 0xFF);
    if (size) memcpy(buf.data() + 4, payload, size);
    return SendAll(c, buf.data(), buf.size());
}

bool SendJson(const std::shared_ptr<Conn>& c, const std::string& text) {
    std::string line = text + "\n";
    return SendAll(c, line.data(), line.size());
}

bool RecvExact(SOCKET s, char* dst, size_t size) {
    size_t got = 0;
    while (got < size) {
        int n = recv(s, dst + got, (int)(size - got), 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

bool IsHexRoomId(const std::string& id) {
    if (id.size() < 16 || id.size() > 64) return false;
    for (char c : id) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

/* Pulls "room":"..." out of the handshake line; the payload is fixed and simple, no parser needed */
std::string ReadRoomField(const std::string& line) {
    const std::string key = "\"room\"";
    size_t p = line.find(key);
    if (p == std::string::npos) return "";
    p = line.find(':', p + key.size());
    if (p == std::string::npos) return "";
    size_t q = line.find('"', p);
    if (q == std::string::npos) return "";
    size_t e = line.find('"', q + 1);
    if (e == std::string::npos) return "";
    return line.substr(q + 1, e - q - 1);
}

void Serve(SOCKET s) {
    auto conn = std::make_shared<Conn>();
    conn->sock = s;

    // Receive the handshake line byte by byte; it is short and arrives at once
    std::string line;
    DWORD start = GetTickCount();
    for (;;) {
        if (GetTickCount() - start > (DWORD)kHandshakeTimeoutMs) {
            SendJson(conn, "{\"ok\":false,\"err\":\"handshake timeout\"}");
            closesocket(s);
            return;
        }
        char ch = 0;
        int n = recv(s, &ch, 1, 0);
        if (n <= 0) { closesocket(s); return; }
        if (ch == '\n') break;
        line.push_back(ch);
        if (line.size() > kMaxHandshakeBytes) {
            SendJson(conn, "{\"ok\":false,\"err\":\"handshake too long\"}");
            closesocket(s);
            return;
        }
    }

    // A control connection (accounts / friends) starts with {"t":…}; anything else is a room pipe
    if (line.find("\"t\"") != std::string::npos && line.find("\"room\"") == std::string::npos) {
        // Hand the socket over, followed by the line already consumed
        ctl::ServePrefetched((void*)(uintptr_t)s, line);
        return;
    }

    const std::string room = ReadRoomField(line);
    if (!IsHexRoomId(room)) {
        SendJson(conn, "{\"ok\":false,\"err\":\"bad room\"}");
        closesocket(s);
        return;
    }
    if (!g_reg.Join(room, conn)) {
        SendJson(conn, "{\"ok\":false,\"err\":\"room full\"}");
        closesocket(s);
        Log("room %s refused: full", room.substr(0, 8).c_str());
        return;
    }
    if (!SendJson(conn, "{\"ok\":true}")) {
        g_reg.Leave(room, conn);
        closesocket(s);
        return;
    }
    Log("room %s joined", room.substr(0, 8).c_str());

    // Pipe frames until the peer closes
    std::vector<char> header(4);
    while (RecvExact(s, header.data(), 4)) {
        const size_t size = ((size_t)(unsigned char)header[0] << 24) |
                            ((size_t)(unsigned char)header[1] << 16) |
                            ((size_t)(unsigned char)header[2] << 8) |
                            (size_t)(unsigned char)header[3];
        if (size > kMaxFrameBytes) break;
        std::vector<char> payload(size);
        if (size && !RecvExact(s, payload.data(), size)) break;
        for (auto& peer : g_reg.Peers(room, conn)) {
            if (!SendFrame(peer, payload.data(), payload.size())) {
                // The peer is gone; it removes itself when its own socket errors out
            }
        }
    }

    g_reg.Leave(room, conn);
    Log("room %s left", room.substr(0, 8).c_str());
    shutdown(s, SD_BOTH);
    closesocket(s);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 443;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v > 0 && v < 65536) port = v;
    }

    // Log next to the exe so a hidden scheduled task still leaves a trail
    char exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        std::string p(exePath);
        size_t slash = p.find_last_of("\\/");
        g_logPath = (slash == std::string::npos ? std::string(".") : p.substr(0, slash)) + "\\relay.log";
    }

    // Keys and account data live next to the exe
    std::string baseDir = ".";
    {
        char exe[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
            std::string p(exe);
            size_t slash = p.find_last_of("\\/");
            if (slash != std::string::npos) baseDir = p.substr(0, slash);
        }
    }
    const std::string keyDir = baseDir + "\\keys";
    const std::string keyPath = keyDir + "\\server.key";
    CreateDirectoryA(keyDir.c_str(), nullptr);

    if (argc > 1 && strcmp(argv[1], "--genkey") == 0) {
        std::string jwk;
        if (!sec::GenerateKeyPair(keyPath, jwk)) { printf("KEYGEN FAILED\n"); return 1; }
        printf("%s\n", jwk.c_str());
        return 0;
    }

    // Generate the key on first start, inside whatever account the service runs as: a key written
    // from an admin session may not be readable by the SYSTEM task, and this sidesteps that entirely.
    if (!sec::LoadPrivateKey(keyPath)) {
        std::string jwk;
        if (sec::GenerateKeyPair(keyPath, jwk)) {
            Log("首次启动：已生成服务器密钥 %s", keyPath.c_str());
            Log("公钥 JWK：%s", jwk.c_str());
            if (!sec::LoadPrivateKey(keyPath))
                Log("警告：密钥已生成但仍无法加载（错误 %lu）", GetLastError());
        } else {
            Log("警告：密钥生成失败；再试一次 --genkey，或检查 keys 目录权限");
        }
    }
    if (!ctl::Init(baseDir + "\\data"))
        Log("警告：账号数据无法加载，按空库运行");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("WSAStartup failed");
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        Log("socket failed: %d", WSAGetLastError());
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0) {
        Log("bind %d failed: %d (is another program using the port?)", port, WSAGetLastError());
        return 1;
    }
    if (listen(listener, 64) != 0) {
        Log("listen failed: %d", WSAGetLastError());
        return 1;
    }
    Log("floral-relay listening on 0.0.0.0:%d", port);

    for (;;) {
        SOCKET s = accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR) continue;
            Sleep(50);
            continue;
        }
        // Small frames every two seconds: latency matters more than throughput
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        std::thread(Serve, s).detach();
    }
}
