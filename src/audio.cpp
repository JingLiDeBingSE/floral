#include "audio.h"
#include "log.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

/*
 * Playback backend: miniaudio (bundles dr_mp3 / dr_flac decoding).
 *
 * A download thread streams the file into an in-memory buffer via WinInet and the decoder
 * reads from that buffer. Seeks wait for the download to reach their target; the device
 * callback plays silence instead of waiting when the download falls behind.
 * The decoder keeps the source sample rate and the device is opened at that rate.
 */

namespace {

std::recursive_mutex    g_apiMx;   // all Audio* entry points take it, so track changes can't race decoder callbacks

std::mutex              g_decMx;   // ma_decoder is not thread-safe: callback and seek must serialize
std::mutex              g_mx;
std::condition_variable g_cv;

std::vector<unsigned char> g_buf;            // downloaded bytes so far
size_t                     g_readPos = 0;    // current decoder read position
bool                       g_eof = false;
std::atomic<bool>          g_abort{ false };
std::atomic<bool>          g_dlFailed{ false };   // the download stopped before the whole file arrived

std::thread g_dl;
std::thread g_seekThread;

ma_device  g_dev{};
ma_decoder g_dec{};
std::atomic<bool> g_devReady{ false };
std::atomic<bool> g_decReady{ false };

std::atomic<float>     g_vol{ 1.0f };
std::atomic<bool>      g_paused{ true };
std::atomic<bool>      g_ended{ false };     // the decoder has played the whole file
std::atomic<bool>      g_failed{ false };    // the decoder ran out of data because the download broke off
std::atomic<int>       g_posMs{ 0 };         // decoder position in ms, kept here so the UI thread never reads g_dec
std::wstring           g_err;

std::atomic<bool> g_seeking{ false };
std::atomic<int>  g_seekReq{ -1 };           // set under g_mx and announced on g_cv
std::atomic<bool> g_seekStop{ false };
thread_local bool t_seekWorker = false;      // a newer seek request cuts short the waits of this thread only

/* ── Decoder callbacks: read from the memory buffer ── */
ma_result OnRead(ma_decoder*, void* out, size_t bytes, size_t* got) {
    std::unique_lock<std::mutex> lk(g_mx);
    g_cv.wait(lk, [&] {
        return g_abort || g_eof || g_readPos + bytes <= g_buf.size() || (t_seekWorker && g_seekReq >= 0);
    });
    if (g_abort) { *got = 0; return MA_SUCCESS; }
    size_t avail = g_buf.size() - g_readPos;
    size_t n = (bytes < avail) ? bytes : avail;
    if (n) memcpy(out, g_buf.data() + g_readPos, n);
    g_readPos += n;
    *got = n;
    return MA_SUCCESS;
}

ma_result OnSeek(ma_decoder*, ma_int64 offset, ma_seek_origin origin) {
    if (origin == ma_seek_origin_end) return MA_NOT_IMPLEMENTED;   // the length is unknown until the download ends
    std::unique_lock<std::mutex> lk(g_mx);
    ma_int64 target = (origin == ma_seek_origin_start ? 0 : (ma_int64)g_readPos) + offset;
    if (target < 0) return MA_BAD_SEEK;
    g_cv.wait(lk, [&] {
        return g_abort || g_eof || (ma_int64)g_buf.size() >= target || (t_seekWorker && g_seekReq >= 0);
    });
    // dr_flac probes past the end while searching and narrows its range when this fails
    if (g_abort || target > (ma_int64)g_buf.size()) return MA_BAD_SEEK;
    g_readPos = (size_t)target;
    return MA_SUCCESS;
}


/** Caller holds g_decMx */
void PublishPosition() {
    ma_uint64 cur = 0;
    ma_decoder_get_cursor_in_pcm_frames(&g_dec, &cur);
    g_posMs = (int)(cur * 1000 / g_dec.outputSampleRate);
}

/** Whether the download is far enough ahead that a decoder read in the callback will not wait for it */
bool Buffered() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_eof || g_buf.size() - g_readPos >= 256 * 1024;
}

/* ── Audio output callback ── */
void OnData(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    float* p = (float*)out;
    ma_uint32 ch = dev->playback.channels;
    ma_uint64 got = 0;
    // The callback never waits: while a seek is queued or running, or the download lags behind, it plays silence
    std::unique_lock<std::mutex> lk(g_decMx, std::try_to_lock);
    if (lk.owns_lock() && g_decReady && g_seekReq < 0 && Buffered()) {
        ma_decoder_read_pcm_frames(&g_dec, out, frames, &got);
        PublishPosition();
        if (got < frames) {                 // the decoder has used up all the data there will be
            if (g_dlFailed) g_failed = true;
            else            g_ended = true;
        }
    }

    float v = g_vol.load();
    size_t n = (size_t)frames * ch;
    for (size_t i = 0; i < n; i++) p[i] *= v;
    if (got < frames) {
        memset(p + (size_t)got * ch, 0, ((size_t)frames - (size_t)got) * ch * sizeof(float));
    }
}

/* ── Download thread ── */
void DownloadThread(std::wstring url) {
    HINTERNET inet = InternetOpenW(L"KugouPlayer/1.0", INTERNET_OPEN_TYPE_DIRECT,
                                   nullptr, nullptr, 0);
    if (!inet) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_eof = true;
        g_cv.notify_all();
        return;
    }
    DWORD ct = 8000, rt = 20000;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &ct, sizeof(ct));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &rt, sizeof(rt));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_NO_UI;
    HINTERNET req = InternetOpenUrlW(inet, url.c_str(), nullptr, 0, flags, 0);

    // Growing the buffer copies it while holding g_mx, which stalls the decoder; reserve the full size instead
    DWORD length = 0, lengthSize = sizeof(length);
    bool knownLength = req && HttpQueryInfoW(req, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                                             &length, &lengthSize, nullptr);
    if (knownLength) {
        std::lock_guard<std::mutex> lk(g_mx);
        try { g_buf.reserve(length); } catch (const std::bad_alloc&) {}   // the server's length may be bogus
    }

    bool readOk = req != nullptr;
    size_t received = 0;
    std::vector<unsigned char> chunk(65536);
    while (readOk && !g_abort) {
        DWORD got = 0;
        readOk = InternetReadFile(req, chunk.data(), (DWORD)chunk.size(), &got);
        if (!readOk || got == 0) break;
        received += got;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_buf.insert(g_buf.end(), chunk.begin(), chunk.begin() + got);
        }
        g_cv.notify_all();
    }
    if (req) InternetCloseHandle(req);
    InternetCloseHandle(inet);

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_dlFailed = !readOk || (knownLength && received < length);
        g_eof = true;
    }
    g_cv.notify_all();
}

/** Local file: read it into the buffer in one go */
bool LoadLocal(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0) { CloseHandle(h); return false; }
    std::vector<unsigned char> tmp((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, tmp.data(), (DWORD)sz.QuadPart, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_buf.assign(tmp.begin(), tmp.begin() + read);
        g_eof = true;
    }
    g_cv.notify_all();
    return true;
}

/* A streaming seek waits for the download to reach the target, so seeks run here instead of on the UI thread */
void SeekWorker() {
    t_seekWorker = true;
    std::unique_lock<std::mutex> lk(g_mx);
    for (;;) {
        g_cv.wait(lk, [] { return g_seekStop || g_seekReq >= 0; });
        if (g_seekStop) return;
        g_seeking = true;
        int ms = g_seekReq.exchange(-1);
        lk.unlock();
        {
            std::lock_guard<std::mutex> dk(g_decMx);
            ma_decoder_seek_to_pcm_frame(&g_dec, (ma_uint64)ms * g_dec.outputSampleRate / 1000);
            if (g_seekReq < 0) PublishPosition();   // otherwise AudioSeekMS already shows the newer target
            g_ended = false;
            g_failed = false;
        }
        lk.lock();
        g_seeking = false;
    }
}

void Teardown() {
    // Release the decoder only after everything that reads it has stopped:
    // the device callback, the download thread and the seek worker
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_abort = true;             // unblocks decoder reads and seeks waiting for data
        g_seekStop = true;
    }
    g_cv.notify_all();

    if (g_devReady) {
        ma_device_stop(&g_dev);     // once stopped, the callback never re-enters
        ma_device_uninit(&g_dev);
        g_devReady = false;
    }
    if (g_dl.joinable()) g_dl.join();
    if (g_seekThread.joinable()) g_seekThread.join();

    if (g_decReady) {
        ma_decoder_uninit(&g_dec);
        g_decReady = false;
    }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_buf.clear();
        g_buf.shrink_to_fit();
        g_readPos = 0;
        g_eof = false;
        g_abort = false;            // a new download thread exits right away while this is set
        g_dlFailed = false;
    }
    g_paused = true;
    g_ended = false;
    g_failed = false;
    g_posMs = 0;
    g_seekReq = -1;
}

}  // namespace

/* ── Public API ── */

bool AudioOpen(const std::wstring& path) {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    g_err.clear();
    Teardown();
    if (path.empty()) { g_err = L"空地址"; return false; }

    bool isHttp = (path.rfind(L"http://", 0) == 0) || (path.rfind(L"https://", 0) == 0);
    if (isHttp) {
        g_dl = std::thread(DownloadThread, path);
    } else if (!LoadLocal(path)) {
        g_err = L"打不开本地文件";
        return false;
    }

    // Buffer a little before decoding starts, so playback doesn't starve right away
    {
        std::unique_lock<std::mutex> lk(g_mx);
        g_cv.wait_for(lk, std::chrono::seconds(15),
                      [] { return g_eof || g_buf.size() >= 512 * 1024; });
    }

    // Decode to f32 stereo at the source sample rate; f32 holds 16- and 24-bit samples exactly
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 0);
    if (ma_decoder_init(OnRead, OnSeek, nullptr, &dc, &g_dec) != MA_SUCCESS) {
        g_err = L"解码器初始化失败";
        Teardown();
        return false;
    }
    g_decReady = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = g_dec.outputSampleRate;
    cfg.dataCallback      = OnData;
    if (ma_device_init(nullptr, &cfg, &g_dev) != MA_SUCCESS) {
        g_err = L"音频设备打开失败";
        Teardown();
        return false;
    }
    g_devReady = true;
    Logf("AUD", "采样率：音源 %u Hz，设备 %u Hz", g_dec.outputSampleRate, g_dev.playback.internalSampleRate);

    g_seekStop = false;
    g_seekThread = std::thread(SeekWorker);
    return true;
}

void AudioPlay() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (!g_devReady || !g_paused) return;   // already running, don't start again
    ma_device_start(&g_dev);
    g_paused = false;
}

void AudioPause() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (!g_devReady) return;
    ma_device_stop(&g_dev);
    g_paused = true;
}

void AudioClose() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx); Teardown(); }

void AudioSeekMS(int ms) {
    if (ms < 0 || !g_decReady) return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_seekReq = ms;
    }
    g_posMs = ms;                                    // report the target until the seek lands
    g_cv.notify_all();
}

bool AudioIsSeeking() { return g_seeking || g_seekReq >= 0; }

void AudioSetVolume(int v) {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    g_vol = (float)v / 1000.0f;
}

int AudioPosMS() { return g_decReady ? g_posMs.load() : -1; }

bool AudioIsEnded() { return g_ended.load(); }
bool AudioIsFailed() { return g_failed.load(); }

bool AudioIsPlaying() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx); return g_devReady && !g_paused.load(); }
std::wstring AudioLastError() { return g_err; }
