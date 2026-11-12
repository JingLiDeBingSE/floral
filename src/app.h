#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <vector>

/* ───────── 数据 ───────── */
struct Song {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int durationSec = 0;
    std::wstring cover;     // 封面本地路径，可为空
    std::string  coverUrl;  // 封面网络地址（本地没有时按需下载）
    std::string  hash;      // 酷狗 hash
    long long    mixId = 0; // mixsongid，取播放地址时要当 album_audio_id
    long long    albumId = 0;
};

/* ───────── 歌单 ───────── */
struct Playlist {
    std::wstring name;
    std::string  gcid;        // 酷狗 global_collection_id
    int          count = 0;
    std::string  coverUrl;
    bool         isDefault = false;
};

/* ───────── 页面 ───────── */
enum class Page { Playlists, Grid, Player };

/* ───────── 命中区 ───────── */
enum HitKind {
    HK_NONE = 0,
    HK_MIN, HK_MAX, HK_CLOSE,
    HK_TILE,        // 封面格子
    HK_BACK,        // 播放页返回
    HK_PLAY,        // 播放/暂停
    HK_PREV,        // 上一首
    HK_NEXT,        // 下一首
    HK_PROGRESS,    // 进度条
    HK_SETTINGS,    // 打开设置
    HK_SET_CLOSE,   // 关设置
    HK_SET_BG,      // 设置面板外的遮罩（点了也关）
    HK_SET_CARD,    // 设置卡片本身（吞掉点击，别穿透到后面的歌）
    HK_SET_CLEAR,   // 清空缓存
    HK_SET_THEME,   // 切换主题
    HK_SET_LOGIN,   // 重新登录（先弹平台选择）
    HK_SET_LANG_ZH, // 设置：界面语言切到中文
    HK_SET_LANG_EN, // 设置：界面语言切到英文
    HK_LOGIN_PICK,  // 选了哪个平台
    HK_LOGIN_TIP,   // 标题栏的登录状态提示
    HK_PLAYER,      // 进入播放页
    HK_VOL,         // 音量按钮
    HK_VOL_TRACK,   // 音量滑杆
    HK_SRC,         // 音源切换（酷狗/网易云）
    HK_TOG,         // 一起听按钮
    HK_TOG_HOST,    // 开房
    HK_TOG_COPY,    // 复制连接码
    HK_TOG_INPUT,   // 连接码输入框
    HK_TOG_JOIN,    // 加入
    HK_TOG_LEAVE,   // 退出一起听
    HK_TOG_SWAP,    // 换个本机地址（已弃用，保留占位）
    HK_TOG_CARD,    // 卡片空白处：吞掉点击，不要关面板
    HK_SCROLL,      // 选歌界面的滚动条
    HK_PL,          // 歌单选择页的圆
    HK_PL_PICK,     // 标题栏「歌曲」：回歌单页
    HK_PICKER,      // 标题栏按钮：回歌单选择页
    HK_QUAL,        // 音质按钮
    HK_QUAL_ITEM    // 音质选项
};
struct Hit {
    D2D1_RECT_F r{};
    int kind = HK_NONE;
    int index = -1;
};

struct App {
    std::vector<Song> songs;
    std::vector<Playlist> playlists;

    /* 歌单选择页：整体平移 + 拖动惯性 */
    float plX = 0.0f, plY = 0.0f;             // 当前平移
    float plTargetX = 0.0f, plTargetY = 0.0f; // 缓动目标
    float plVelX = 0.0f, plVelY = 0.0f;       // 松手后的速度
    int   plHover = -1;                       // 悬停的歌单
    int   plActive = -1;                      // 已选中（准备伸展）
    float plSelT = 0.0f;                      // 选中动画 0→1（居中 + 伸展）
    bool  plDragging = false;
    POINT plDragStartMouse = { 0, 0 };
    float plDragStartX = 0.0f, plDragStartY = 0.0f;
    bool  plDragMoved = false;

    Page page = Page::Grid;
    float pageAnim = 0.0f;              // 0 = 网格，1 = 播放页

    int hoverTile = -1;
    std::vector<float> tileAnim;        // 每个格子的悬停动画 0..1

    int current = -1;                   // 正在播放的曲目
    bool playing = false;
    double pos = 0.0;                   // 播放进度（秒）

    float scroll = 0.0f;
    float scrollTarget = 0.0f;      // 平滑滚动：实际值追这个
    float maxScroll = 0.0f;

    POINT mouse = { -1, -1 };
    float width = 960.0f;
    float height = 640.0f;

    std::vector<Hit> hits;
};

extern App g_app;

#include "theme.h"

enum { WB_MIN = 0, WB_MAX = 1, WB_CLOSE = 2 };

void InitData();
extern std::wstring g_likesName;

/* 内嵌资源：歌单默认数据（data.cpp 用） */
bool LoadEmbeddedLikes(std::string& out);
