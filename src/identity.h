#pragma once
#include <string>

/*
 * Floral identity
 *
 * The identity itself lives on the server and is reached through the platform account: logging in
 * with Kugou / NetEase / QQ proves who you are, and the server answers with the Floral id, name and
 * a device secret. This file only caches that answer, so the app can show the friend code offline
 * and skip the bind round trip on the next start.
 *
 * File: <data dir>\identity.json
 */
struct Identity {
    std::string id;        // 8 chars, e.g. K7QM3XPD
    std::string name;      // display name shown to friends
    std::string secret;    // device credential issued on first bind
    std::string serverKey; // pinned public key (JWK); a change means the server was re-keyed
    bool        valid = false;

    /* "K7QM-3XPD" */
    std::string Code() const;
};

/* Reads <data dir>\identity.json; valid stays false when there is none */
Identity IdentityLoad();

/* Writes the cache; creates the data directory when needed */
bool IdentitySave(const Identity& id);

/* Forgets the cached identity (used when the server rejects the secret) */
void IdentityClear();

/* The data directory the rest of the app uses (Documents\Floral) */
std::wstring IdentityDataDir();
