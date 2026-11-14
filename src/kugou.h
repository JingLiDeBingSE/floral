#pragma once
#include <string>
#include <vector>

/*
 * Kugou API client (no Node dependency):
 *   QR-code login + fetch the "Liked" playlist
 *
 * Everything goes through the local kugou-api service at 127.0.0.1:3000.
 */

/** Fetch the QR code; on success the image is written to the png at qrPath */
bool KgLoginStart(const std::wstring& qrPath, std::wstring& err);

/** Poll the scan status: 0 waiting / 1 scanned, pending confirm / 2 success / 3 QR expired / 4 error */
int  KgLoginPoll(std::string& token, std::string& userid, std::wstring& err);

/* ── NetEase Cloud Music (same API style, port 4000) ── */
bool NgLoginStart(const std::wstring& qrPath, std::wstring& err);
int  NgLoginPoll(std::string& cookie, std::wstring& err);   // 2 = success, cookie filled in
int  NgExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err);
std::string  NgTrimCookie(const std::string& full);   // keep only MUSIC_U / __csrf

/* level for NgResolveUrl and QqResolveUrl: 0 standard / 1 320k / 2 lossless / 3 Hi-Res; gotLevel receives the tier served */
std::wstring NgResolveUrl(const std::string& songId, int level, const std::string& cookie,
                          std::wstring& err, int* gotLevel = nullptr);   // anonymous when cookie is empty

/* ── QQ Music (port 3200) ── */
bool QqLoginStart(const std::wstring& qrPath, std::wstring& err);
int  QqLoginPoll(std::string& cookie, std::wstring& err);      // 2 = success
int  QqExportLikes(const std::string& cookie, const std::wstring& outTsv, std::wstring& err);
std::wstring QqResolveUrl(const std::string& mid, const std::string& cookie, int level,
                           std::wstring& err, int* gotLevel = nullptr);

bool MatchOnPlatform(int source, const std::string& title, const std::string& artist,
                     int wantDur, std::string& hashOut, long long& mixIdOut, long long& albumIdOut,
                     std::wstring& err);

/* ── Playlists ── */
struct KgPlaylist {
    std::string  gcid;       // global_collection_id
    std::wstring name;
    std::string coverUrl;
    int         count = 0;
    int         ver = 0;          // list_ver, which the service increases whenever the tracks change
};

/** Fetch all playlists of the account */
int KgFetchPlaylists(const std::string& token, const std::string& userid,
                     std::vector<KgPlaylist>& out, std::wstring& err);

/** Fetch tracks of the given playlist and write a tsv; returns the track count (-1 on failure).
    countHint is the expected track count, used to request every page at once; 0 when unknown */
int KgExportPlaylist(const std::string& token, const std::string& userid, const std::string& gcid,
                     int countHint, const std::wstring& outTsv, std::wstring& err);

/** Fetch the "Liked" songs and write a tsv; returns the number of songs written (-1 on failure) */
int  KgExportLikes(const std::string& token, const std::string& userid,
                   const std::wstring& outTsv, std::wstring& err);
