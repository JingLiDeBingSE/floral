#pragma once
#include <string>
#include <vector>

struct Relay;

struct FriendEntry {
    std::string id;      // 8 chars
    std::string name;
    bool        online = false;
};

struct FriendsView {
    bool connected = false;      // control connection alive
    bool ready = false;          // the server accepted us
    std::string myId, myName;    // myId is empty until the identity is known
    std::vector<FriendEntry> friends;
    std::vector<FriendEntry> pending;      // incoming requests
    std::string inviteFrom, inviteName, inviteRoom;   // newest invite; consumed by the UI
    std::wstring notice;         // last thing worth telling the user
    int noticeSeq = 0;
};

void FriendsInit(Relay* link);
void FriendsTick(unsigned long long nowMs);
FriendsView FriendsStatus();

/* The platform session is valid: uid + token prove who this account is */
void FriendsAttachAccount(int source, const std::string& uid, const std::string& token,
                          const std::wstring& nickname);

/* Signed out: drop the connection but keep the cached identity */
void FriendsDetachAccount();

void FriendsAdd(const std::string& code);
void FriendsAccept(const std::string& uid);
void FriendsDecline(const std::string& uid);
void FriendsInvite(const std::string& uid, const std::string& room);
void FriendsRename(const std::string& name);
void FriendsConsumeInvite();
