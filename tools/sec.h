/*
 * Crypto helpers for the Floral relay server
 *
 *  - RSA-2048 key pair generated once, private half stored DPAPI-protected
 *  - The public half is printed as a JWK, which the client pastes in and hands to Node's
 *    crypto.publicEncrypt; that keeps the platform token off the wire in clear text
 *  - Base64 for the protocol
 *
 * No third-party dependency: everything goes through Windows CNG.
 */
#pragma once
#include <string>

namespace sec {

std::string Base64Encode(const std::string& raw);
bool        Base64Decode(const std::string& text, std::string& out);

/* Creates keys\server.key (DPAPI-protected) and returns the public key as a JWK JSON string */
bool GenerateKeyPair(const std::string& keyPath, std::string& publicJwk);

/* Loads the private key; false when the file is missing or unreadable */
bool LoadPrivateKey(const std::string& keyPath);

/* Decrypts what the client produced with publicEncrypt(RSA_PKCS1_OAEP_PADDING, oaepHash sha1) */
bool RsaDecryptOaep(const std::string& cipher, std::string& plain);

bool HasPrivateKey();

/* The public half as a JWK, ready to hand to a client */
bool LoadPublicJwk(const std::string& keyPath, std::string& jwk);

}  // namespace sec
