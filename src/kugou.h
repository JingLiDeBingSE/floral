#pragma once
#include <string>
#include <vector>

/*
 * 酷狗接口客户端（不依赖 Node）：
 *   扫码登录 + 拉「我喜欢」歌单
 *
 * 全部走本机 127.0.0.1:3000 的 kugou-api 服务。
 */

/** 取二维码：成功会把图写到 qrPath 指向的 png */
bool KgLoginStart(const std::wstring& qrPath, std::wstring& err);

/** 轮询扫码状态：0 等待 / 1 已扫码待确认 / 2 成功 / 3 二维码过期 / 4 出错 */
int  KgLoginPoll(std::string& token, std::string& userid, std::wstring& err);

/* ── 网易云（同一套接口风格，端口 4000） ── */
bool NgLoginStart(const std::wstring& qrPath, std::wstring& err);
int  NgLoginPoll(std::string& cookie, std::wstring& err);   // 2 = 成功，cookie 已填
int  NgExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err);
std::string  NgTrimCookie(const std::string& full);   // 只留 MUSIC_U / __csrf
std::wstring NgResolveUrl(const std::string& songId, const char* level, std::wstring& err, int* gotLevel = nullptr);

/* ── QQ 音乐（端口 3200） ── */
bool QqLoginStart(const std::wstring& qrPath, std::wstring& err);
int  QqLoginPoll(std::string& cookie, std::wstring& err);      // 2 = 成功
int  QqExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err);
std::wstring QqResolveUrl(const std::string& mid, const std::string& cookie, int level,
                           std::wstring& err, int* gotLevel = nullptr);

bool MatchOnPlatform(int source, const std::string& title, const std::string& artist,
                     int wantDur, std::string& hashOut, long long& mixIdOut, long long& albumIdOut,
                     std::wstring& err);

/* ── 歌单 ── */
struct KgPlaylist {
    std::string  gcid;       // global_collection_id
    std::wstring name;
    std::string coverUrl;
    int         count = 0;
    bool        isDefault = false;
};

/** 拉账号下全部歌单 */
int KgFetchPlaylists(const std::string& token, const std::string& userid,
                     std::vector<KgPlaylist>& out, std::wstring& err);

/** 拉指定歌单的曲目并写出 tsv，返回歌曲数（失败 -1） */
int KgExportPlaylist(const std::string& token, const std::string& userid,
                     const std::string& gcid, const std::wstring& outTsv, std::wstring& err);

/** 拉「我喜欢」并写出 tsv，返回写出的歌曲数（失败返回 -1） */
int  KgExportLikes(const std::string& token, const std::string& userid,
                   const std::wstring& outTsv, std::wstring& err);
