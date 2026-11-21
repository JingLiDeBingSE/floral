/*
 * Floral accounts + friends service
 *
 * Runs on the same port as the room relay: a connection whose first line holds "room" is a relay
 * pipe, one whose first line holds "t" is a control connection handled here.
 *
 * Control messages are one JSON object per line:
 *   → {"t":"bind","blob":"<base64 RSA>"}      blob decrypts to {platform,uid,token,name}
 *   ← {"t":"bound","id":"K7QM3XPD","name":"小李","secret":"…"}
 *   → {"t":"hello","id":"…","secret":"…"}
 *   ← {"t":"ok","online":["A1"],"pending":[{"from":"C3","name":"小王"}]}
 *   → {"t":"add","code":"A1B2C3D4"}            ← {"t":"add","ok":true,"reason":"…"}
 *   → {"t":"accept","uid":"…"} / {"t":"decline","uid":"…"}
 *   → {"t":"rename","name":"…"}
 *   → {"t":"invite","uid":"…","room":"…"}      ← pushed to the friend as {"t":"invite","from":…}
 *   ← pushes: {"t":"request","from":…,"name":…} {"t":"presence","uid":…,"online":true}
 *             {"t":"friendOnline"/"friendOffline"} are folded into presence
 */
#pragma once
#include <string>

namespace ctl {

/* Loads accounts.json, rotates the daily backup, prepares the lockout table */
bool Init(const std::string& dataDir);

/* Serves one control connection until the peer closes it. Takes ownership of the socket. */
void Serve(void* socketHandle);

/* Same, but the first line was already consumed by the dispatcher */
void ServePrefetched(void* socketHandle, const std::string& firstLine);

/* Verification hooks, implemented in verify.cpp; both return the platform nickname on success */
struct VerifyResult {
    bool ok = false;
    std::string name;      // platform nickname, UTF-8
    std::string error;     // reason when !ok
};
VerifyResult VerifyKugou(const std::string& uid, const std::string& token);
VerifyResult VerifyNetease(const std::string& uid, const std::string& cookie);
VerifyResult VerifyQQ(const std::string& uid, const std::string& cookie);

}  // namespace ctl
