#pragma once
#include <string>

/* RSA-OAEP(SHA-1) encryption against the server's public key (a JWK with n and e).
   Matches the server side, which decrypts with Windows CNG as well. */
namespace rscrypt {

std::string Base64Encode(const std::string& raw);
bool        Base64Decode(const std::string& text, std::string& out);

/* Encrypts UTF-8 text; returns raw bytes in `out`, false when the key is malformed */
bool EncryptOaep(const std::string& jwkJson, const std::string& text, std::string& out);

}  // namespace rscrypt
