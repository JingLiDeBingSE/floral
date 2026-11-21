#include "rscrypt.h"
#include "json.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace rscrypt {
namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64UrlToStd(const std::string& s, bool& ok) {
    std::string out;
    for (char c : s) {
        if (c == '-') out += '+';
        else if (c == '_') out += '/';
        else if (c == '+' || c == '/' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '=') out += c;
        else if (c != '\n' && c != '\r' && c != ' ') { ok = false; return out; }
    }
    while (out.size() % 4) out += '=';
    ok = true;
    return out;
}

}  // namespace

std::string Base64Encode(const std::string& raw) {
    std::string out;
    size_t i = 0;
    while (i + 2 < raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == raw.size()) {
        const unsigned v = ((unsigned char)raw[i] << 16) | ((unsigned char)raw[i + 1] << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
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
        if (c == '=') continue;
        const int v = rev[(unsigned char)c];
        if (v < 0) return false;
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return true;
}

bool EncryptOaep(const std::string& jwkJson, const std::string& text, std::string& out) {
    JVal jwk;
    if (!Json(jwkJson, jwk)) return false;
    const JVal* nVal = jwk.get("n");
    const JVal* eVal = jwk.get("e");
    if (!nVal || !eVal) return false;

    bool ok = false;
    std::string nRaw, eRaw;
    if (!Base64Decode(Base64UrlToStd(nVal->str(), ok), nRaw) || !ok) return false;
    if (!Base64Decode(Base64UrlToStd(eVal->str(), ok), eRaw) || !ok) return false;
    if (nRaw.empty() || eRaw.empty()) return false;

    // BCRYPT_RSAPUBLIC_BLOB: header, then exponent and modulus, both big-endian
    BCRYPT_RSAKEY_BLOB hdr{};
    hdr.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    hdr.BitLength = (ULONG)nRaw.size() * 8;
    hdr.cbPublicExp = (ULONG)eRaw.size();
    hdr.cbModulus = (ULONG)nRaw.size();
    hdr.cbPrime1 = 0;
    hdr.cbPrime2 = 0;

    std::vector<unsigned char> blob(sizeof(hdr) + eRaw.size() + nRaw.size());
    memcpy(blob.data(), &hdr, sizeof(hdr));
    memcpy(blob.data() + sizeof(hdr), eRaw.data(), eRaw.size());
    memcpy(blob.data() + sizeof(hdr) + eRaw.size(), nRaw.data(), nRaw.size());

    BCRYPT_ALG_HANDLE prov = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&prov, BCRYPT_RSA_ALGORITHM, nullptr, 0))) return false;

    BCRYPT_KEY_HANDLE key = nullptr;
    const bool imported = BCRYPT_SUCCESS(
        BCryptImportKeyPair(prov, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0));
    bool done = false;
    if (imported) {
        BCRYPT_OAEP_PADDING_INFO pi{};
        pi.pszAlgId = BCRYPT_SHA1_ALGORITHM;      // the server decrypts with the same padding

        ULONG need = 0;
        if (BCRYPT_SUCCESS(BCryptEncrypt(key, (PUCHAR)text.data(), (ULONG)text.size(), &pi,
                                         nullptr, 0, nullptr, 0, &need, BCRYPT_PAD_OAEP))) {
            std::vector<unsigned char> buf(need);
            ULONG wrote = 0;
            if (BCRYPT_SUCCESS(BCryptEncrypt(key, (PUCHAR)text.data(), (ULONG)text.size(), &pi,
                                             nullptr, 0, buf.data(), need, &wrote, BCRYPT_PAD_OAEP))) {
                out.assign((const char*)buf.data(), wrote);
                done = true;
            }
        }
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(prov, 0);
    return done;
}

}  // namespace rscrypt
