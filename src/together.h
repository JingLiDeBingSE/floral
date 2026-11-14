#pragma once
#include <string>
#include <vector>

#include "song.h"

/*
 * Listen together
 *
 * The host broadcasts its complete playback state; each guest converges to the latest state it has
 * received, so a lost, repeated or late message never leaves a lasting difference. A room lives in one
 * session object that exists from joining until leaving. main.cpp supplies the player and the relay
 * link through the two interfaces below; the self-test supplies fakes of both.
 */

/* Period of the host's state broadcast and of each guest's status report */
constexpr int kTogetherPeriodMs = 2000;

/* Playback as the session sees it; implemented by main.cpp */
struct TogetherPlayer {
    struct State {
        int  index = -1;          // current track in Queue(), -1 when none is selected
        bool playing = false;     // playing rather than paused; stays true while buffering
        bool buffering = false;   // loading, seeking or stalled, so the position is not advancing
        bool failed = false;      // the current track could not be resolved or opened
        bool dragging = false;    // the progress bar is held down
        int  posMs = 0;           // position reported by the audio engine
    };
    virtual ~TogetherPlayer() = default;

    virtual State Now() = 0;
    virtual const std::vector<Song>& Queue() = 0;
    virtual std::string Name() = 0;            // account nickname in UTF-8, empty when unknown

    // Replaces the queue and leaves no track selected. The song grid switches to the new queue when
    // showInGrid is set or when it was already showing the queue.
    virtual void SetQueue(std::vector<Song> songs, bool showInGrid) = 0;
    virtual void Play(int index, bool playing) = 0;   // opens the track; asks TogetherStartPosMs where to start
    virtual void SetPlaying(bool playing) = 0;
    virtual void Seek(int ms) = 0;
    virtual void Step(int dir) = 0;            // previous (-1) or next (+1) track of the queue
    virtual void Retry() = 0;                  // opens the current track again after a failure
};

/* Connection to the room through the relay helper; implemented by relay.cpp */
struct TogetherLink {
    struct Event {
        enum Kind {
            Joined,     // in the room; text is the room code
            Failed,     // could not join; text is the reason. The link stays idle until the next Join
            Down,       // connection lost after joining; the link reconnects by itself
            Up,         // connection restored after Down
            Message,    // text is a JSON message published by a member, possibly this one
        } kind = Message;
        std::string text;
        unsigned long long atMs = 0;   // GetTickCount64 when the event reached this process
    };
    virtual ~TogetherLink() = default;

    virtual void Join(const std::string& code) = 0;   // an empty code creates a new room
    virtual void Leave() = 0;
    virtual void Send(const std::string& json) = 0;   // returns at once; the message may be dropped
    virtual bool Poll(Event& ev) = 0;                  // takes the next event, false when there is none
};

enum class TogetherPhase {
    Off,        // not in a room
    Starting,   // joining or creating a room
    Hosting,    // host with nobody else in the room
    Together,   // host with a guest, or guest following the host
    Waiting,    // guest that has not heard from a host yet
    NotFound,   // guest that has heard no host for a while after joining
    Lost,       // guest whose host has gone silent; following resumes when the host is heard again
    Full,       // guest not admitted because the room already has a guest
    Failed,     // the link could not join; noticeText holds the reason
};

enum class TogetherNotice {
    None,
    PeerJoined,     // noticeText: the member's name
    PeerLeft,       // noticeText: the member's name
    HostEnded,      // the host closed the room; the session is over
    PeerCantPlay,   // host: a guest cannot play the current track, so the host moved on; noticeText: the name
    CantPlay,       // guest: the current track is not available on this platform; the host was told
    LinkDown,
    LinkUp,
};

struct TogetherView {
    TogetherPhase phase = TogetherPhase::Off;
    bool host = false;
    std::string code;                    // room code
    std::vector<std::string> peers;      // names of the other members in UTF-8
    TogetherNotice notice = TogetherNotice::None;
    std::string noticeText;
    int noticeSeq = 0;                   // increases with every new notice
    bool linkDown = false;
};

enum class TogetherOp { Pause, Resume, Seek, Next, Prev, Pick };

void TogetherInit(TogetherPlayer* player, TogetherLink* link);
void TogetherHost();
void TogetherJoin(const std::string& code);
void TogetherLeave();
void TogetherTick(unsigned long long nowMs);   // UI thread, every frame; nowMs from GetTickCount64
TogetherView TogetherStatus();

/* Consulted by the player; neutral answers when no session is running */
int  TogetherStartPosMs(int index);   // where to start the track just opened, -1 to keep the player's own position
bool TogetherHoldsTrackEnd();         // a following guest pauses at the end of a track and waits for the host
bool TogetherRoutesControls();        // a guest's playback controls become requests to the host
void TogetherRequest(TogetherOp op, int arg = 0);   // arg: milliseconds for Seek, queue index for Pick

/* Runs host and guest sessions against fake players over a lossy link; writes a report, 0 when all pass */
int TogetherSelfTest(const std::wstring& reportPath);
