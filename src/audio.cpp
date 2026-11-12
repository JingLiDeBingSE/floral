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
 * Why MCI was replaced: mpegvideo only ships MP3/WMA decoders, so lossless
 * FLAC would not open. Here we pull the stream ourselves: one thread downloads
 * sequentially into an in-memory buffer via WinInet, the decoder callback reads
 * from that buffer and waits on the download thread when data runs short —
 * streaming (nothing hits disk) while FLAC plays normally.
 */

namespace {

std::recursive_mutex    g_apiMx;   // all Audio* entry points take it, so track changes can't race decoder callbacks

std::mutex              g_decMx;   // ma_decoder is not thread-safe: callback and seek must serialize
std::mutex              g_mx;
std::condition_variable g_cv;

std::vector<unsigned char> g_buf;            // downloaded bytes so far
size_t                     g_readPos = 0;    // current decoder read position
bool                       g_eof = false;
bool                       g_abort = false;

std::thread g_dl;

ma_device  g_dev{};
ma_decoder g_dec{};
std::atomic<bool> g_devReady{ false };
std::atomic<bool> g_decReady{ false };

std::atomic<float> g_vol{ 1.0f };
std::atomic<bool>  g_paused{ true };
std::wstring       g_err;

/* ── Decoder callbacks: read from the memory buffer ── */
ma_result OnRead(ma_decoder*, void* out, size_t bytes, size_t* got) {
    std::unique_lock<std::mutex> lk(g_mx);
    g_cv.wait(lk, [&] {
        return g_abort || g_eof || g_readPos + bytes <= g_buf.size();
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
    std::unique_lock<std::mutex> lk(g_mx);
    size_t base = (origin == ma_seek_origin_start) ? 0 : g_readPos;
    ma_int64 target = (ma_int64)base + offset;
    if (target < 0) target = 0;
    g_cv.wait(lk, [&] { return g_abort || g_eof || (ma_int64)g_buf.size() >= target; });
    g_readPos = (size_t)target;
    return MA_SUCCESS;
}


/* ── Audio output callback ── */
void OnData(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    float* p = (float*)out;
    ma_uint32 ch = dev->playback.channels;
    ma_uint64 got = 0;
    if (g_decReady) {
        std::lock_guard<std::mutex> lk(g_decMx);
        ma_decoder_read_pcm_frames(&g_dec, out, frames, &got);
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

    std::vector<unsigned char> chunk(65536);
    while (req && !g_abort) {
        DWORD got = 0;
        if (!InternetReadFile(req, chunk.data(), (DWORD)chunk.size(), &got) || got == 0) break;
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

void Teardown() {
    // Order matters: stop callbacks from touching the decoder first, then stop
    // the device, and only then uninit. Otherwise WASAPI still holds the device
    // object while we uninit it, and we AV inside a system DLL.
    g_decReady = false;
    g_abort = true;                 // unblocks any decoder callback stuck waiting for data
    g_cv.notify_all();

    if (g_devReady) {
        ma_device_stop(&g_dev);     // once stopped, the callback never re-enters
        ma_device_uninit(&g_dev);
        g_devReady = false;
    }
    if (g_dl.joinable()) g_dl.join();

    if (g_decReady.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_decMx);   // device stopped, wait for the callback to fully exit
        ma_decoder_uninit(&g_dec);
    }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_buf.clear();
        g_buf.shrink_to_fit();
        g_readPos = 0;
        g_eof = false;
        g_abort = false;            // ← must be reset, or a new download thread exits immediately
    }
    g_paused = true;
}

}  // namespace

/* Seek state: seeks run on a background thread so the UI never blocks */
std::atomic<bool> g_seeking{ false };
std::atomic<int>  g_seekReq{ -1 };
std::atomic<bool> g_seekStop{ false };
std::atomic<int>  g_lastPosMs{ 0 };
std::atomic<int>  g_lastLenMs{ 0 };
std::thread       g_seekThread;
static void       SeekWorker();

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

    // Fix f32 / stereo / 48k: the decoder converts internally, so 24bit/96k
    // master files can't break device init (miniaudio would assert)
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 48000);
    if (ma_decoder_init(OnRead, OnSeek, nullptr, &dc, &g_dec) != MA_SUCCESS) {
        g_err = L"解码器初始化失败";
        Teardown();
        return false;
    }
    g_decReady = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 48000;
    cfg.dataCallback      = OnData;
    if (ma_device_init(nullptr, &cfg, &g_dev) != MA_SUCCESS) {
        g_err = L"音频设备打开失败";
        Teardown();
        return false;
    }
    g_devReady = true;
    if (!g_seekThread.joinable()) {          // seek thread is started only once
        g_seekStop = false;
        g_seekThread = std::thread(SeekWorker);
    }
    if (ma_device_start(&g_dev) != MA_SUCCESS) {
        g_err = L"音频设备启动失败";
        Teardown();
        return false;
    }
    g_paused = false;
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

void AudioStop() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (g_decReady) ma_decoder_seek_to_pcm_frame(&g_dec, 0);
    AudioPause();
}

void AudioClose() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx); Teardown(); }

/* Background seek: a streaming seek must wait for the download to reach the
 * target, which would freeze the UI if done on the UI thread */
static void SeekWorker() {
    for (;;) {
        if (g_seekStop) return;
        int ms = g_seekReq.exchange(-1);
        if (ms < 0) { Sleep(15); continue; }

        g_seeking = true;
        bool wasPlaying = !g_paused;
        if (g_devReady) ma_device_stop(&g_dev);      // stop the device first, don't fight the decoder

        if (g_decReady) {
            ma_uint32 rate = g_dec.outputSampleRate ? g_dec.outputSampleRate : 44100;
            ma_uint64 frame = (ma_uint64)ms * rate / 1000;
            std::lock_guard<std::mutex> lk2(g_decMx);   // lock only the decoder, not the global mutex
            if (g_decReady) ma_decoder_seek_to_pcm_frame(&g_dec, frame);
        }

        if (g_devReady && wasPlaying) ma_device_start(&g_dev);
        g_seeking = false;
    }
}

void AudioSeekMS(int ms) {
    if (ms < 0 || !g_decReady) return;
    g_seekReq = ms;                                  // just store one atomic and return
}

bool AudioIsSeeking() { return g_seeking.load(); }

void AudioSetVolume(int v) {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    g_vol = (float)v / 1000.0f;
}

int AudioPosMS() {
    if (!g_decReady) return -1;      // g_decReady is atomic, no lock needed
    // A seek can hold the decoder lock for a long time: if we can't get it,
    // return the last value rather than block the UI
    std::unique_lock<std::mutex> lk2(g_decMx, std::try_to_lock);
    if (!lk2.owns_lock()) return g_lastPosMs.load();
    ma_uint64 cur = 0;
    {
        if (ma_decoder_get_cursor_in_pcm_frames(&g_dec, &cur) != MA_SUCCESS) return -1;
    }
    ma_uint32 rate = g_dec.outputSampleRate ? g_dec.outputSampleRate : 44100;
    int ms = (int)(cur * 1000 / rate);
    g_lastPosMs = ms;
    return ms;
}

int AudioLenMS() {
    if (!g_decReady) return -1;
    std::unique_lock<std::mutex> lk2(g_decMx, std::try_to_lock);
    if (!lk2.owns_lock()) return g_lastLenMs.load();     // same as above, don't block
    ma_uint64 len = 0;
    {
        if (ma_decoder_get_length_in_pcm_frames(&g_dec, &len) != MA_SUCCESS || len == 0) return -1;
    }
    ma_uint32 rate = g_dec.outputSampleRate ? g_dec.outputSampleRate : 44100;
    int ms = (int)(len * 1000 / rate);
    g_lastLenMs = ms;
    return ms;
}

bool AudioIsOpen() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx); return g_devReady; }
bool AudioIsPlaying() {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx); return g_devReady && !g_paused.load(); }
std::wstring AudioLastError() { return g_err; }
