#pragma once
#include <string>

/*
 * 一起听（中继版）
 *
 * 房间码 → 本地 together-server.js（Node，内嵌）→ EMQX Cloud（TLS）
 * C++ 只跟本地 HTTP 打交道，不直接讲 MQTT。
 */

struct TogetherEvent {
    int  kind = 0;          // 1 = 已连接  2 = 对方状态  3 = 对方断开  4 = 出错
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

void TogetherSayHello();                       // 在场心跳（只报昵称）
std::string TogetherDefaultNick(bool host);    // 默认昵称：计算机名   // UTC 毫秒，跨机器可比

void TogetherRequestSkip();       // 告诉房主：这首我放不了，跳过吧
bool TogetherPoll(TogetherEvent& ev);