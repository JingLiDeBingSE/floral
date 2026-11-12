#pragma once
#include <string>

/*
 * Listen Together (relay edition)
 *
 * Room code → local together-server.js (Node, embedded) → EMQX Cloud (TLS)
 * C++ speaks only to the local HTTP server, never MQTT directly.
 */

struct TogetherEvent {
    int  kind = 0;          // 1 = connected  2 = peer state  3 = peer left  4 = error
    std::string nick;
    std::string song;
    std::string src;
    std::string title;
    std::string artist;
    int  posMs = 0;
    bool playing = false;
    long long stampMs = 0;
    std::wstring err;
};

bool TogetherHost(const std::string& nick, std::string& codeOut, std::wstring& err, int addrIndex = -1);
bool TogetherJoin(const std::string& code, const std::string& nick, std::wstring& err);
void TogetherStop();

bool TogetherActive();
std::string TogetherPeerNick();
std::string TogetherMyCode();

void TogetherPushState(const std::string& song, const std::string& src,
                       int posMs, bool playing,
                       const std::string& title, const std::string& artist);

unsigned long long TogetherNowMs();

void TogetherSayHello();                       // presence heartbeat (nick only)
std::string TogetherDefaultNick(bool host);    // default nickname: computer name   // UTC ms, comparable across machines

void TogetherRequestSkip();       // tell host: can't play this track, skip it
bool TogetherPoll(TogetherEvent& ev);