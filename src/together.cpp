#include "together.h"

static TogetherPlayer* g_player = nullptr;
static TogetherLink*   g_link = nullptr;

void TogetherInit(TogetherPlayer* player, TogetherLink* link) {
    g_player = player;
    g_link = link;
}

void TogetherHost() {}
void TogetherJoin(const std::string&) {}
void TogetherLeave() {}
void TogetherTick(unsigned long long) {}
TogetherView TogetherStatus() { return TogetherView{}; }

int  TogetherStartPosMs(int) { return -1; }
bool TogetherHoldsTrackEnd() { return false; }
bool TogetherRoutesControls() { return false; }
void TogetherRequest(TogetherOp, int) {}

int TogetherSelfTest(const std::wstring&) { return 1; }
