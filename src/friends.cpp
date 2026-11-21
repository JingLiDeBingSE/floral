#include "friends.h"
#include "identity.h"
#include "json.h"
#include "log.h"
#include "relay.h"
#include "rscrypt.h"

#include <algorithm>

/*
 * Client half of the account / friends protocol.
 *
 * The connection lives in the Node helper (see relay.cpp, control channel); this file only reacts to
 * its events and decides what to send next:
 *
 *   open  → hello when the cached identity is usable, pubkey otherwise
 *   pubkey → pin it, then bind {platform, uid, token, name} encrypted with that key
 *   bound  → cache the identity and say hello
 *   ok     → friends and requests arrive; the panel reads them from FriendsStatus()
 */

namespace {

enum class Stage { Idle, WaitingLink, AskingKey, Binding, Ready, Rejected };

struct State {
    Relay*           link = nullptr;
    Stage            stage = Stage::Idle;
    Identity         me;
    bool             meLoaded = false;

    int              platform = 0;      // 0 kugou, 1 netease, 2 qq
    std::string      uid, token;
    std::wstring     nickname;
    bool             haveAccount = false;

    std::vector<FriendEntry> friends, pending;
    std::string      inviteFrom, inviteName, inviteRoom;
    std::wstring     notice;
    int              noticeSeq = 0;
    unsigned long long lastAttempt = 0;
};

State g;

void Say(const std::wstring& text) {
    g.notice = text;
    g.noticeSeq++;
    Logf("FRI", "%s", std::string(text.begin(), text.end()).c_str());
}

const char* PlatformTag(int source) {
    return source == 2 ? "qq" : (source == 1 ? "netease" : "kugou");
}

std::string Widen2Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Narrow2Widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

void Send(const std::string& json) {
    if (g.link) g.link->CtlSend(json);
}

void SendHello() {
    Send("{\"t\":\"hello\",\"id\":" + JsonQuote(g.me.id) + ",\"secret\":" + JsonQuote(g.me.secret) + "}");
}

void SendPubkey() {
    g.stage = Stage::AskingKey;
    Send("{\"t\":\"pubkey\"}");
}

bool SendBind() {
    if (!g.haveAccount || g.uid.empty() || g.token.empty()) return false;
    if (g.me.serverKey.empty()) return false;

    const std::string plain = "{\"platform\":\"" + std::string(PlatformTag(g.platform)) +
                              "\",\"uid\":" + JsonQuote(g.uid) + ",\"token\":" + JsonQuote(g.token) +
                              ",\"name\":" + JsonQuote(Widen2Narrow(g.nickname)) + "}";
    std::string cipher;
    if (!rscrypt::EncryptOaep(g.me.serverKey, plain, cipher)) {
        Say(L"无法加密绑定信息");
        return false;
    }
    g.stage = Stage::Binding;
    Send("{\"t\":\"bind\",\"blob\":" + JsonQuote(rscrypt::Base64Encode(cipher)) + "}");
    return true;
}

void UpsertFriend(const std::string& id, const std::string& name, int online /* -1 leave */) {
    auto it = std::find_if(g.friends.begin(), g.friends.end(), [&](const FriendEntry& f) { return f.id == id; });
    if (it == g.friends.end()) {
        if (online < 0) return;
        FriendEntry f;
        f.id = id;
        f.name = name;
        f.online = online != 0;
        g.friends.push_back(f);
        return;
    }
    if (!name.empty()) it->name = name;
    if (online >= 0) it->online = online != 0;
    std::sort(g.friends.begin(), g.friends.end(), [](const FriendEntry& a, const FriendEntry& b) {
        if (a.online != b.online) return a.online;
        return a.name < b.name;
    });
}

void HandleMessage(const std::string& line) {
    JVal m;
    if (!Json(line, m)) return;
    const std::string t = m.get("t") ? m.get("t")->str() : "";

    if (t == "pubkey") {
        const JVal* jwk = m.get("jwk");
        if (!jwk) return;
        // Serialise the object back to text: the cache stores it as a string
        std::string key;
        if (jwk->t == JVal::STR) key = jwk->str();
        else {
            // Rebuild the two fields we need
            const JVal* n = jwk->get("n");
            const JVal* e = jwk->get("e");
            if (!n || !e) return;
            key = "{\"kty\":\"RSA\",\"n\":" + JsonQuote(n->str()) + ",\"e\":" + JsonQuote(e->str()) + "}";
        }
        if (!g.me.serverKey.empty() && g.me.serverKey != key) {
            Say(L"服务器密钥已更换，已停止连接");
            g.stage = Stage::Rejected;
            return;
        }
        if (g.me.serverKey.empty()) {
            g.me.serverKey = key;
            IdentitySave(g.me);
        }
        if (g.me.valid) SendHello();
        else if (!g.haveAccount) Say(L"请先登录一个平台账号");
        else SendBind();
        return;
    }

    if (t == "bound") {
        g.me.id = m.get("id") ? m.get("id")->str() : "";
        g.me.name = m.get("name") ? m.get("name")->str() : "";
        g.me.secret = m.get("secret") ? m.get("secret")->str() : "";
        g.me.valid = !g.me.id.empty() && !g.me.secret.empty();
        IdentitySave(g.me);
        Logf("FRI", "身份已建立 %s", g.me.id.c_str());
        SendHello();
        return;
    }

    if (t == "ok") {
        g.stage = Stage::Ready;
        g.friends.clear();
        g.pending.clear();
        if (g.me.name.empty()) g.me.name = m.get("name") ? m.get("name")->str() : "";
        if (const JVal* fs = m.get("friends"))
            for (const JVal& f : fs->a) {
                FriendEntry e;
                if (const JVal* v = f.get("id")) e.id = v->str();
                if (const JVal* v = f.get("name")) e.name = v->str();
                if (const JVal* v = f.get("online")) e.online = v->ll() != 0 || v->b;
                if (!e.id.empty()) g.friends.push_back(e);
            }
        if (const JVal* ps = m.get("pending"))
            for (const JVal& p : ps->a) {
                FriendEntry e;
                if (const JVal* v = p.get("from")) e.id = v->str();
                if (const JVal* v = p.get("name")) e.name = v->str();
                if (!e.id.empty()) g.pending.push_back(e);
            }
        Logf("FRI", "已就绪：%d 位好友，%d 条请求", (int)g.friends.size(), (int)g.pending.size());
        return;
    }

    if (t == "err") {
        const std::string code = m.get("code") ? m.get("code")->str() : "";
        if (code == "auth") {
            // The cached secret no longer matches: drop it and re-bind next time
            g.me.secret.clear();
            g.me.valid = false;
            IdentitySave(g.me);
            Say(L"身份已失效，正在重新绑定");
            SendPubkey();
        } else if (code == "verify") {
            Say(L"平台账号校验失败，请重新登录");
        } else if (code == "locked") {
            Say(L"校验失败次数过多，请 10 分钟后再试");
        } else if (code == "crypto" || code == "nokey") {
            Say(L"服务器密钥异常");
        }
        return;
    }

    if (t == "request") {
        FriendEntry e;
        e.name = m.get("name") ? m.get("name")->str() : "";
        e.id = m.get("from") ? m.get("from")->str() : "";
        if (e.id.empty()) return;
        if (std::none_of(g.pending.begin(), g.pending.end(), [&](const FriendEntry& p) { return p.id == e.id; }))
            g.pending.push_back(e);
        Say(L"收到一个好友请求");
        return;
    }
    if (t == "friend") {
        const std::string id = m.get("id") ? m.get("id")->str() : "";
        const std::string name = m.get("name") ? m.get("name")->str() : "";
        UpsertFriend(id, name, 1);
        Say(L"已成为好友");
        return;
    }
    if (t == "presence") {
        const std::string id = m.get("id") ? m.get("id")->str() : "";
        const bool online = m.get("online") && (m.get("online")->b || m.get("online")->ll() != 0);
        UpsertFriend(id, "", online ? 1 : 0);
        return;
    }
    if (t == "renamed") {
        if (m.get("name")) g.me.name = m.get("name")->str();
        return;
    }
    if (t == "accepted") {
        const std::string id = m.get("id") ? m.get("id")->str() : "";
        const std::string name = m.get("name") ? m.get("name")->str() : "";
        if (m.get("ok") && (m.get("ok")->b || m.get("ok")->ll())) {
            g.pending.erase(std::remove_if(g.pending.begin(), g.pending.end(),
                                           [&](const FriendEntry& p) { return p.id == id; }),
                            g.pending.end());
            UpsertFriend(id, name, 1);
        }
        return;
    }
    if (t == "invite") {
        g.inviteFrom = m.get("from") ? m.get("from")->str() : "";
        g.inviteName = m.get("name") ? m.get("name")->str() : "";
        g.inviteRoom = m.get("room") ? m.get("room")->str() : "";
        Say(L"收到一起听邀请");
        return;
    }
    if (t == "add") {
        if (m.get("ok") && (m.get("ok")->b || m.get("ok")->ll() != 0)) Say(L"好友请求已发送");
        else {
            const std::string reason = m.get("reason") ? m.get("reason")->str() : "";
            if (reason == "nouser") Say(L"没有这个好友码");
            else if (reason == "self") Say(L"不能加自己");
            else if (reason == "already") Say(L"已经是好友了");
            else if (reason == "pending") Say(L"请求已发送，等对方确认");
        }
        return;
    }
    if (t == "invited") {
        if (!(m.get("ok") && (m.get("ok")->b || m.get("ok")->ll() != 0))) Say(L"对方不在线");
        return;
    }
}

}  // namespace

void FriendsInit(Relay* link) {
    g.link = link;
    if (!g.meLoaded) {
        g.me = IdentityLoad();
        g.meLoaded = true;
    }
}

void FriendsTick(unsigned long long nowMs) {
    if (!g.link) return;

    Relay::CtlEvent ev;
    while (g.link->CtlPoll(ev)) {
        if (ev.kind == Relay::CtlEvent::Open) {
            g.stage = g.me.valid ? Stage::Ready : Stage::WaitingLink;
            if (g.me.valid) SendHello();
            else SendPubkey();
        } else if (ev.kind == Relay::CtlEvent::Down) {
            g.stage = Stage::WaitingLink;
            g.friends.clear();
            g.pending.clear();
        } else {
            HandleMessage(ev.text);
        }
    }

    // Reconnect when the helper lost the link and nobody is driving it any more
    if (g.stage == Stage::Idle && g.link && nowMs - g.lastAttempt > 3000) {
        g.lastAttempt = nowMs;
        g.stage = Stage::WaitingLink;
    }
}

FriendsView FriendsStatus() {
    FriendsView v;
    v.connected = (g.stage != Stage::Idle);
    v.ready = (g.stage == Stage::Ready);
    v.myId = g.me.id;
    v.myName = g.me.name;
    v.friends = g.friends;
    v.pending = g.pending;
    v.inviteFrom = g.inviteFrom;
    v.inviteName = g.inviteName;
    v.inviteRoom = g.inviteRoom;
    v.notice = g.notice;
    v.noticeSeq = g.noticeSeq;
    return v;
}

void FriendsAttachAccount(int source, const std::string& uid, const std::string& token,
                          const std::wstring& nickname) {
    g.platform = source;
    g.uid = uid;
    g.token = token;
    g.nickname = nickname;
    g.haveAccount = !uid.empty() && !token.empty();
    if (!g.meLoaded) { g.me = IdentityLoad(); g.meLoaded = true; }
    if (!g.haveAccount) return;

    // A cached identity is enough on its own; otherwise the key and a bind are needed
    if (g.me.valid) {
        if (g.stage == Stage::Ready) SendHello();
        return;
    }
    SendPubkey();
}

void FriendsDetachAccount() {
    g.haveAccount = false;
    g.uid.clear();
    g.token.clear();
}

void FriendsAdd(const std::string& code) { Send("{\"t\":\"add\",\"code\":" + JsonQuote(code) + "}"); }
void FriendsAccept(const std::string& uid) { Send("{\"t\":\"accept\",\"uid\":" + JsonQuote(uid) + "}"); }
void FriendsDecline(const std::string& uid) { Send("{\"t\":\"decline\",\"uid\":" + JsonQuote(uid) + "}"); }
void FriendsRename(const std::string& name) { Send("{\"t\":\"rename\",\"name\":" + JsonQuote(name) + "}"); }
void FriendsInvite(const std::string& uid, const std::string& room) {
    Send("{\"t\":\"invite\",\"uid\":" + JsonQuote(uid) + ",\"room\":" + JsonQuote(room) + "}");
}

void FriendsConsumeInvite() {
    g.inviteFrom.clear();
    g.inviteName.clear();
    g.inviteRoom.clear();
}
