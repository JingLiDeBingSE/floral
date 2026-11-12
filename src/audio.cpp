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
 * 播放后端：miniaudio（自带 dr_mp3 / dr_flac 解码）
 *
 * 为什么换掉 MCI：mpegvideo 只有 MP3/WMA 解码器，FLAC 无损打不开。
 * 这里自己拉流：一个线程用 WinInet 顺序下载到内存缓冲，解码器回调从缓冲里读，
 * 缓冲不够就等下载线程——既是流式（不落盘），又能让 FLAC 正常播。
 */

namespace {

std::recursive_mutex    g_apiMx;   // 所有 Audio* 入口都走它，避免换歌和解码回调打架

std::mutex              g_decMx;   // ma_decoder 不是线程安全的：回调和 seek 必须串行
std::mutex              g_mx;
std::condition_variable g_cv;

std::vector<unsigned char> g_buf;            // 已下载的字节
size_t                     g_readPos = 0;    // 解码器当前读到的位置
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

/* ── 解码器回调：从内存缓冲读 ── */
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


/* ── 音频输出回调 ── */
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

/* ── 下载线程 ── */
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

/** 本地文件：一次性读进缓冲 */
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
    // 顺序很关键：先让回调不再碰解码器，再停设备，最后才 uninit。
    // 否则 WASAPI 内部还持有设备对象时就 uninit，会 AV 在系统 DLL 里。
    g_decReady = false;
    g_abort = true;                 // 让可能卡在等数据的解码回调立刻返回
    g_cv.notify_all();

    if (g_devReady) {
        ma_device_stop(&g_dev);     // 停完回调不会再进来
        ma_device_uninit(&g_dev);
        g_devReady = false;
    }
    if (g_dl.joinable()) g_dl.join();

    if (g_decReady.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_decMx);   // 设备已停，等回调彻底退出
        ma_decoder_uninit(&g_dec);
    }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_buf.clear();
        g_buf.shrink_to_fit();
        g_readPos = 0;
        g_eof = false;
        g_abort = false;            // ← 必须复位，否则新起的下载线程立刻退出
    }
    g_paused = true;
}

}  // namespace

/* 跳转相关的状态：seek 放到后台线程做，避免卡住 UI */
std::atomic<bool> g_seeking{ false };
std::atomic<int>  g_seekReq{ -1 };
std::atomic<bool> g_seekStop{ false };
std::atomic<int>  g_lastPosMs{ 0 };
std::atomic<int>  g_lastLenMs{ 0 };
std::thread       g_seekThread;
static void       SeekWorker();

/* ── 对外接口 ── */

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

    // 先攒一点再开始解码，避免一上来就断流
    {
        std::unique_lock<std::mutex> lk(g_mx);
        g_cv.wait_for(lk, std::chrono::seconds(15),
                      [] { return g_eof || g_buf.size() >= 512 * 1024; });
    }

    // 固定成 f32 / 立体声 / 48k：解码器内部会转，
    // 免得某些 24bit/96k 的母带文件把设备初始化搞崩（miniaudio 会 assert）
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
    if (!g_seekThread.joinable()) {          // 跳转线程只起一次
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
    if (!g_devReady || !g_paused) return;   // 已经在跑就别重复 start
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

/* 后台跳转：流媒体 seek 要等下载追上目标位置，放在 UI 线程会卡死界面 */
static void SeekWorker() {
    for (;;) {
        if (g_seekStop) return;
        int ms = g_seekReq.exchange(-1);
        if (ms < 0) { Sleep(15); continue; }

        g_seeking = true;
        bool wasPlaying = !g_paused;
        if (g_devReady) ma_device_stop(&g_dev);      // 先停设备，别和解码器抢

        if (g_decReady) {
            ma_uint32 rate = g_dec.outputSampleRate ? g_dec.outputSampleRate : 44100;
            ma_uint64 frame = (ma_uint64)ms * rate / 1000;
            std::lock_guard<std::mutex> lk2(g_decMx);   // 只锁解码器，不锁全局
            if (g_decReady) ma_decoder_seek_to_pcm_frame(&g_dec, frame);
        }

        if (g_devReady && wasPlaying) ma_device_start(&g_dev);
        g_seeking = false;
    }
}

void AudioSeekMS(int ms) {
    if (ms < 0 || !g_decReady) return;
    g_seekReq = ms;                                  // 只写一个 atomic，立刻返回
}

bool AudioIsSeeking() { return g_seeking.load(); }

void AudioSetVolume(int v) {
    std::lock_guard<std::recursive_mutex> lk(g_apiMx);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    g_vol = (float)v / 1000.0f;
}

int AudioPosMS() {
    if (!g_decReady) return -1;      // g_decReady 是 atomic，不加锁
    // 跳转时解码器会被锁住很久：拿不到锁就返回上次的值，绝不阻塞 UI
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
    if (!lk2.owns_lock()) return g_lastLenMs.load();     // 同上，别阻塞
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
