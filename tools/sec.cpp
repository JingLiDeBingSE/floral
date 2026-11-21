#include "sec.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace sec {
namespace {

BCRYPT_ALG_HANDLE        g_prov   = nullptr;
BCRYPT_KEY_HANDLE        g_key    = nullptr;
bool                     g_loaded = false;

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void Cleanup() {
    if (g_key) { BCryptDestroyKey(g_key); g_key = nullptr; }
    if (g_prov) { BCryptCloseAlgorithmProvider(g_prov, 0); g_prov = nullptr; }
    g_loaded = false;
}

bool OpenProvider() {
    if (g_prov) return true;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&g_prov, BCRYPT_RSA_ALGORITHM, nullptr, 0))) {
        g_prov = nullptr;
        return false;
    }
    return true;
}

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

bool WriteFileBytes(const std::string& path, const void* data, size_t size) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* DPAPI with a fixed entropy value, so a copied key file is useless on another machine */
DATA_BLOB Entropy() {
    static const char kMark[] = "FloralRelayKeyV1";
    DATA_BLOB b;
    b.pbData = (BYTE*)kMark;
    b.cbData = (DWORD)sizeof(kMark) - 1;
    return b;
}

bool Protect(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
    DATA_BLOB din, dout, ent = Entropy();
    din.pbData = (BYTE*)in.data();
    din.cbData = (DWORD)in.size();
    // Machine scope: the key is generated from an admin session but read by the SYSTEM task
    if (!CryptProtectData(&din, L"floral-relay-key", &ent, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE, &dout)) return false;
    out.assign(dout.pbData, dout.pbData + dout.cbData);
    LocalFree(dout.pbData);
    return true;
}

bool Unprotect(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
    DATA_BLOB din, dout, ent = Entropy();
    din.pbData = (BYTE*)in.data();
    din.cbData = (DWORD)in.size();
    // Accept both scopes, so a key written by an older build still loads
    if (!CryptUnprotectData(&din, nullptr, &ent, nullptr, nullptr,
                            CRYPTPROTECT_LOCAL_MACHINE, &dout) &&
        !CryptUnprotectData(&din, nullptr, &ent, nullptr, nullptr, 0, &dout))
        return false;
    out.assign(dout.pbData, dout.pbData + dout.cbData);
    LocalFree(dout.pbData);
    return true;
}

std::string Base64Url(const unsigned char* data, size_t size) {
    std::string s = Base64Encode(std::string((const char*)data, size));
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

/* Big-endian bytes of the BCRYPT_RSAKEY_BLOB fields, with leading zeros trimmed */
std::string TrimBE(const unsigned char* p, ULONG n) {
    ULONG i = 0;
    while (i < n && p[i] == 0) ++i;
    return Base64Url(p + i, n - i);
}

}  // namespace

std::string Base64Encode(const std::string& raw) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& text, std::string& out) {
    static int rev[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++) rev[(unsigned char)kB64[i]] = i;
        init = true;
    }
    out.clear();
    unsigned buf = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = rev[(unsigned char)c];
        if (v < 0) return false;
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return true;
}

bool GenerateKeyPair(const std::string& keyPath, std::string& publicJwk) {
    if (!OpenProvider()) return false;
    Cleanup();
    if (!OpenProvider()) return false;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(g_prov, &key, 2048, 0))) return false;
    if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) { BCryptDestroyKey(key); return false; }

    ULONG need = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_RSAPRIVATE_BLOB, nullptr, 0, &need, 0)) || need == 0) {
        BCryptDestroyKey(key);
        return false;
    }
    std::vector<unsigned char> blob(need);
    if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_RSAPRIVATE_BLOB, blob.data(), need, &need, 0))) {
        BCryptDestroyKey(key);
        return false;
    }

    // Read the public half out of the blob first: the blob is wiped right after it is sealed
    const BCRYPT_RSAKEY_BLOB* hdr = (const BCRYPT_RSAKEY_BLOB*)blob.data();
    if (hdr->Magic != BCRYPT_RSAPRIVATE_MAGIC) {
        SecureZeroMemory(blob.data(), blob.size());
        BCryptDestroyKey(key);
        return false;
    }
    const unsigned char* p = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    // BCRYPT_RSAKEY_BLOB layout: cbPublicExp, cbModulus, cbPrime1, cbPrime2 (all ULONG)
    // followed by publicExponent, modulus, prime1, prime2, ... in that order
    const unsigned char* exponent = p;
    const unsigned char* modulus = p + hdr->cbPublicExp;
    publicJwk = "{\"kty\":\"RSA\",\"n\":\"" + TrimBE(modulus, hdr->cbModulus) +
                "\",\"e\":\"" + TrimBE(exponent, hdr->cbPublicExp) + "\"}";

    std::vector<unsigned char> sealed;
    const bool ok = Protect(blob, sealed);
    SecureZeroMemory(blob.data(), blob.size());
    BCryptDestroyKey(key);
    if (!ok) return false;
    if (!WriteFileBytes(keyPath, sealed.data(), sealed.size())) return false;

    // Keep the public half next to it so the server can hand it out on request
    std::string pubPath = keyPath;
    size_t slash = pubPath.find_last_of("\\/");
    if (slash != std::string::npos) pubPath = pubPath.substr(0, slash) + "\\public.jwk";
    WriteFileBytes(pubPath, publicJwk.data(), publicJwk.size());
    return true;
}

/* The public key as a JWK, for the pubkey endpoint; empty when it was never generated */
bool LoadPublicJwk(const std::string& keyPath, std::string& jwk) {
    std::string pubPath = keyPath;
    size_t slash = pubPath.find_last_of("\\/");
    if (slash != std::string::npos) pubPath = pubPath.substr(0, slash) + "\\public.jwk";
    std::vector<unsigned char> raw;
    if (!ReadFileBytes(pubPath, raw)) return false;
    jwk.assign((const char*)raw.data(), raw.size());
    return true;
}
bool LoadPrivateKey(const std::string& keyPath) {
    if (g_loaded) return true;
    if (!OpenProvider()) return false;

    std::vector<unsigned char> sealed;
    if (!ReadFileBytes(keyPath, sealed)) return false;
    std::vector<unsigned char> blob;
    if (!Unprotect(sealed, blob)) return false;

    // The key must be imported with OAEP support flagged, so BCryptDecrypt may use it
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(g_prov, nullptr, BCRYPT_RSAPRIVATE_BLOB, &g_key,
                                            blob.data(), (ULONG)blob.size(), 0))) {
        SecureZeroMemory(blob.data(), blob.size());
        return false;
    }
    SecureZeroMemory(blob.data(), blob.size());
    g_loaded = true;
    return true;
}

bool HasPrivateKey() { return g_loaded; }

bool RsaDecryptOaep(const std::string& cipher, std::string& plain) {
    if (!g_key) return false;
    BCRYPT_OAEP_PADDING_INFO pi{};
    pi.pszAlgId = BCRYPT_SHA1_ALGORITHM;   // matches Node's default oaepHash

    ULONG need = 0;
    if (!BCRYPT_SUCCESS(BCryptDecrypt(g_key, (PUCHAR)cipher.data(), (ULONG)cipher.size(), &pi,
                                      nullptr, 0, nullptr, 0, &need, BCRYPT_PAD_OAEP)))
        return false;
    std::vector<unsigned char> buf(need);
    ULONG done = 0;
    if (!BCRYPT_SUCCESS(BCryptDecrypt(g_key, (PUCHAR)cipher.data(), (ULONG)cipher.size(), &pi,
                                      nullptr, 0, buf.data(), need, &done, BCRYPT_PAD_OAEP)))
        return false;
    plain.assign((const char*)buf.data(), done);
    SecureZeroMemory(buf.data(), buf.size());
    return true;
}

}  // namespace sec
