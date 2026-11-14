#include "together.h"
#include "json.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

/*
 * Messages are JSON objects. Each carries v (protocol), type, from (a sender id drawn per session), role
 * ("host" or "guest"), name, seq (increasing per sender) and at (the sender's GetTickCount64).
 *
 *   state   host; every period while a guest is in the room, and at once on a change: list, parts, index,
 *           hash, play ("playing", "paused" or "buffering"), pos, members [{id, done}]
 *   list    host; one part of its queue: list, part, parts, songs
 *   status  guest; every period, and at once on a change: list (the queue it holds), index, hash, health
 *           ("ok", "loading" or "failed"), want and missing (the list it is collecting and the parts it
 *           lacks), heard (the newest seq received from the host), req {op, arg, seq, list}
 *   bye     either side, on leaving
 *
 * A list id is derived from the songs, so moving to another track of the same queue keeps it. A request
 * stays in the guest's status until the host lists its seq as done.
 */

namespace {

constexpr int kProtocol = 1;
constexpr unsigned long long kSilentMs = 4 * kTogetherPeriodMs;   // a peer unheard this long is gone
constexpr unsigned long long kNotFoundMs = 6000;    // a new guest that hears no host this long gives up
constexpr unsigned long long kFullMs = 3000;        // a guest left off the member list this long is not admitted
constexpr unsigned long long kMinGapMs = 100;       // least time between two state or two status messages
constexpr unsigned long long kSeekGapMs = 5000;     // least time between two position corrections
constexpr unsigned long long kSkewWindowMs = 3 * 60 * 1000;      // older offset samples are dropped to follow drift
constexpr int kToleranceMs = 1000;                  // positions this close count as in step
constexpr int kEndMarginMs = 1000;                  // a guest never starts closer than this to the end
constexpr int kMaxTrackSec = 24 * 3600;
constexpr size_t kPartBytes = 32 * 1024;
constexpr size_t kEnvelopeBytes = 1024;             // room kept in a part for the envelope and part fields
constexpr int kMaxParts = 256;
constexpr int kByeCopies = 3;                       // bye is the one message not repeated every period

enum class Play { Paused, Playing, Buffering };
enum class Health { Ok, Loading, Failed };

const char* const kPlayNames[] = { "paused", "playing", "buffering" };
const char* const kHealthNames[] = { "ok", "loading", "failed" };
const char* const kOpNames[] = { "pause", "resume", "seek", "next", "prev", "pick" };   // TogetherOp order

template <size_t N>
int Lookup(const char* const (&names)[N], const std::string& name) {
    for (size_t i = 0; i < N; i++) if (name == names[i]) return (int)i;
    return -1;
}

std::string Str(const JVal& o, const char* key) {
    const JVal* v = o.get(key);
    return v ? v->str() : "";
}

/* The number as an integer; def unless it is a number that a double holds exactly */
long long Int(const JVal* v, long long def = 0) {
    return v && v->t == JVal::NUM && std::fabs(v->num) <= 9007199254740992.0 ? (long long)v->num : def;
}

std::string SongJson(const Song& s) {
    return "{\"title\":" + JsonQuote(Narrow(s.title)) + ",\"artist\":" + JsonQuote(Narrow(s.artist)) +
           ",\"album\":" + JsonQuote(Narrow(s.album)) + ",\"durationSec\":" + std::to_string(s.durationSec) +
           ",\"coverUrl\":" + JsonQuote(s.coverUrl) + ",\"hash\":" + JsonQuote(s.hash) +
           ",\"mixId\":" + std::to_string(s.mixId) + ",\"albumId\":" + std::to_string(s.albumId) +
           ",\"source\":" + std::to_string(s.source) + "}";
}

/* Every entry is kept, so that positions match the host's queue; one the player cannot use fails to play */
Song ReadSong(const JVal& v) {
    Song s;
    s.title = Widen(Str(v, "title"));
    s.artist = Widen(Str(v, "artist"));
    s.album = Widen(Str(v, "album"));
    s.durationSec = (int)std::clamp(Int(v.get("durationSec")), 0LL, (long long)kMaxTrackSec);
    s.coverUrl = Str(v, "coverUrl");
    s.hash = Str(v, "hash");
    s.mixId = Int(v.get("mixId"));
    s.albumId = Int(v.get("albumId"));
    s.source = (int)Int(v.get("source"), -1);
    return s;
}

/* Identifies a queue by the platform and id of each song; empty for an empty queue */
std::string ListId(const std::vector<Song>& songs) {
    if (songs.empty()) return "";
    unsigned long long h = 14695981039346656037ull;   // FNV-1a
    auto mix = [&h](unsigned char c) { h = (h ^ c) * 1099511628211ull; };
    for (const Song& s : songs) {
        mix((unsigned char)s.source);
        for (char c : s.hash) mix((unsigned char)c);
        mix(0);
    }
    char id[17];
    snprintf(id, sizeof(id), "%016llx", h);
    return id;
}

/* The fields of each "list" message for the queue, split so that no message exceeds kPartBytes */
std::vector<std::string> SplitParts(const std::vector<Song>& songs, const std::string& list) {
    std::vector<std::string> groups;
    for (const Song& s : songs) {
        const std::string json = SongJson(s);
        if (groups.empty() || groups.back().size() + 1 + json.size() > kPartBytes - kEnvelopeBytes) groups.emplace_back();
        if (!groups.back().empty()) groups.back() += ',';
        groups.back() += json;
    }
    std::vector<std::string> parts;
    for (size_t i = 0; i < groups.size(); i++)
        parts.push_back(",\"list\":" + JsonQuote(list) + ",\"part\":" + std::to_string(i) + ",\"parts\":" +
                        std::to_string(groups.size()) + ",\"songs\":[" + groups[i] + "]");
    return parts;
}

std::string RandomId() {
    std::random_device rd;
    char id[17];
    snprintf(id, sizeof(id), "%08x%08x", rd(), rd());
    return id;
}

/* A position to start or seek to on a track of durationSec; an unknown length sets no limit */
int StartPoint(long long posMs, int durationSec) {
    if (durationSec > 0) posMs = std::min<long long>(posMs, durationSec * 1000LL - kEndMarginMs);
    return (int)std::max(0LL, posMs);
}

const char* GuestPhaseText(TogetherPhase phase) {
    switch (phase) {
    case TogetherPhase::Together: return "开始跟随房主";
    case TogetherPhase::NotFound: return "一直没有收到房主的消息，房间码可能不对";
    case TogetherPhase::Lost:     return "与房主失去联系";
    case TogetherPhase::Full:     return "房间已满";
    default:                      return "等待房主回应";
    }
}

/* Kept outside the session, so that a notice such as HostEnded is still seen after the session ends */
struct Notices {
    TogetherNotice kind = TogetherNotice::None;
    std::string text;
    int seq = 0;

    void Post(TogetherNotice k, const std::string& t = "") {
        kind = k;
        text = t;
        ++seq;
    }
};

TogetherView NoticeView(const Notices& n) {
    TogetherView v;
    v.notice = n.kind;
    v.noticeText = n.text;
    v.noticeSeq = n.seq;
    return v;
}

struct Envelope {
    std::string type, from, name;
    long long seq = 0;
    unsigned long long sentAt = 0;      // on the sender's clock
    unsigned long long arrivedAt = 0;   // on this machine's clock
};

/* A track and how it plays, as a state message carries them; a status message carries only the track */
struct Track {
    std::string list;
    int index = -1;
    std::string hash;
    Play play = Play::Paused;
    int posMs = 0;

    std::string Key() const { return list + '/' + std::to_string(index) + '/' + hash; }   // the track alone
};

/* The list, index and hash that both state and status messages carry */
Track ReadTrack(const JVal& m) {
    Track t;
    t.list = Str(m, "list");
    t.index = (int)Int(m.get("index"), -1);
    t.hash = Str(m, "hash");
    return t;
}

/* One room from joining until leaving; the role is fixed at creation */
class Session {
public:
    Session(TogetherPlayer& player, TogetherLink& link, Notices& notices, bool host, const std::string& code)
        : player_(player), link_(link), notices_(notices), id_(RandomId()), host_(host), code_(code) {
        link_.Join(code);
        if (host) Log("创建房间");
        else Log("加入房间 %s", code.c_str());
    }
    virtual ~Session() = default;

    void Tick(unsigned long long now);
    void Leave();
    bool Ended() const { return ended_; }
    TogetherView View() const;

    virtual int  StartPosMs(int) const { return -1; }
    virtual bool HoldsTrackEnd() const { return false; }
    virtual bool RoutesControls() const { return false; }
    virtual void Request(TogetherOp, int) {}

protected:
    virtual void OnMessage(const Envelope& e, const JVal& m) = 0;
    virtual void Update() = 0;                   // every tick once the room is joined
    virtual TogetherPhase Phase() const = 0;     // once the room is joined
    virtual std::string Peer() const = 0;

    long long Send(const char* type, const std::string& fields);   // fields: ,"key":value pairs; returns the seq
    void Log(const char* fmt, ...) const;
    unsigned long long Since(unsigned long long t) const { return now_ > t ? now_ - t : 0; }

    TogetherPlayer& player_;
    TogetherLink&   link_;
    Notices&        notices_;
    const std::string id_;
    unsigned long long now_ = 0;
    unsigned long long joinedAt_ = 0;
    bool ended_ = false;

private:
    void Receive(const TogetherLink::Event& ev);

    const bool host_;
    std::string code_;
    long long seq_ = 0;
    bool joined_ = false;
    bool linkDown_ = false;
    bool failed_ = false;
};

void Session::Tick(unsigned long long now) {
    now_ = now;
    TogetherLink::Event ev;
    while (!ended_ && link_.Poll(ev)) {
        switch (ev.kind) {
        case TogetherLink::Event::Joined:
            joined_ = true;
            joinedAt_ = ev.atMs;
            code_ = ev.text;
            Log("已进入房间 %s", code_.c_str());
            break;
        case TogetherLink::Event::Failed:
            failed_ = true;
            Log("没能进入房间：%s", ev.text.c_str());
            break;
        case TogetherLink::Event::Down:
            if (linkDown_) break;
            linkDown_ = true;
            notices_.Post(TogetherNotice::LinkDown);
            Log("连接断开");
            break;
        case TogetherLink::Event::Up:
            if (!linkDown_) break;
            linkDown_ = false;
            notices_.Post(TogetherNotice::LinkUp);
            Log("连接恢复");
            break;
        case TogetherLink::Event::Message:
            if (joined_) Receive(ev);
            break;
        }
    }
    if (joined_ && !failed_ && !ended_) Update();
}

void Session::Receive(const TogetherLink::Event& ev) {
    JVal m;
    if (!Json(ev.text, m) || Int(m.get("v")) != kProtocol) return;
    Envelope e;
    e.from = Str(m, "from");
    // The broker returns this session's own messages too; hosts listen to guests and guests to the host
    if (e.from.empty() || e.from == id_ || Str(m, "role") != (host_ ? "guest" : "host")) return;
    e.type = Str(m, "type");
    e.name = Str(m, "name");
    e.seq = Int(m.get("seq"));
    e.sentAt = (unsigned long long)std::max(0LL, Int(m.get("at")));
    e.arrivedAt = ev.atMs;
    OnMessage(e, m);
}

long long Session::Send(const char* type, const std::string& fields) {
    link_.Send("{\"v\":" + std::to_string(kProtocol) + ",\"type\":\"" + type + "\",\"from\":\"" + id_ +
               "\",\"role\":\"" + (host_ ? "host" : "guest") + "\",\"name\":" + JsonQuote(player_.Name()) +
               ",\"seq\":" + std::to_string(++seq_) + ",\"at\":" + std::to_string(now_) + fields + "}");
    return seq_;
}

void Session::Leave() {
    if (joined_) for (int i = 0; i < kByeCopies; i++) Send("bye", "");
    link_.Leave();
    Log("离开房间");
}

TogetherView Session::View() const {
    TogetherView v = NoticeView(notices_);
    v.host = host_;
    v.code = code_;
    v.linkDown = linkDown_;
    v.phase = failed_ ? TogetherPhase::Failed : joined_ ? Phase() : TogetherPhase::Starting;
    v.peer = Peer();
    return v;
}

void Session::Log(const char* fmt, ...) const {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Logf("TOG", "%s：%s", host_ ? "房主" : "客人", text);
}

class HostSession : public Session {
public:
    HostSession(TogetherPlayer& player, TogetherLink& link, Notices& notices)
        : Session(player, link, notices, true, "") {}

private:
    struct Guest {
        std::string id, name;
        unsigned long long heardAt = 0;
        Track track;                    // from its latest status, as are the fields below
        std::string want;
        std::vector<int> missing;
        long long heard = 0;
        Health health = Health::Ok;
    };
    struct Known { long long seq = 0, done = 0; };   // newest message, newest request handled

    void OnMessage(const Envelope& e, const JVal& m) override;
    void Update() override;
    TogetherPhase Phase() const override { return guest_ ? TogetherPhase::Together : TogetherPhase::Hosting; }
    std::string Peer() const override { return guest_ ? guest_->name : ""; }

    void Admit(const Envelope& e);
    void Drop(const char* why);
    void Execute(const JVal& req);
    void SkipIfUnplayable();
    void RefreshList();
    Track Current() const;
    bool Moved(const Track& t) const;
    void SendState(const Track& t);
    void SendParts();

    std::optional<Guest> guest_;
    std::map<std::string, Known> known_;   // every guest heard in this room, kept after it leaves
    std::set<std::string> skipped_;        // tracks passed over because the guest could not play them
    std::string list_;
    std::vector<std::string> parts_;
    std::vector<long long> partSeq_;       // seq of the latest send of each part, 0 before the first
    Track sent_;
    unsigned long long sentAt_ = 0;
    bool dirty_ = false;                   // the member list changed since the latest state
};

void HostSession::OnMessage(const Envelope& e, const JVal& m) {
    Known& known = known_[e.from];
    if (e.seq <= known.seq) return;        // a repeat, or overtaken by a newer message
    known.seq = e.seq;
    if (e.type == "bye") {
        if (guest_ && guest_->id == e.from) Drop("退出了房间");
        return;
    }
    if (e.type != "status") return;
    if (!guest_) Admit(e);
    if (guest_->id != e.from) return;      // the room holds a single guest
    RefreshList();                         // the request and the report are checked against the queue as it is now

    Guest& g = *guest_;
    g.name = e.name;
    g.heardAt = e.arrivedAt;
    g.track = ReadTrack(m);
    g.want = Str(m, "want");
    g.heard = Int(m.get("heard"));
    g.health = (Health)std::max(0, Lookup(kHealthNames, Str(m, "health")));
    g.missing.clear();
    if (const JVal* missing = m.get("missing"))
        for (const JVal& part : missing->a) g.missing.push_back((int)Int(&part, -1));

    const JVal* req = m.get("req");
    const long long reqSeq = req ? Int(req->get("seq")) : 0;
    if (reqSeq > known.done) {
        known.done = reqSeq;
        dirty_ = true;
        Execute(*req);
    }
    SkipIfUnplayable();
}

void HostSession::Admit(const Envelope& e) {
    guest_ = Guest{ e.from, e.name };
    dirty_ = true;
    notices_.Post(TogetherNotice::PeerJoined, e.name);
    Log("%s 加入了房间", e.name.c_str());
}

void HostSession::Drop(const char* why) {
    Log("%s %s", guest_->name.c_str(), why);
    notices_.Post(TogetherNotice::PeerLeft, guest_->name);
    guest_.reset();
}

void HostSession::Execute(const JVal& req) {
    const int op = Lookup(kOpNames, Str(req, "op"));
    const long long arg = Int(req.get("arg"), -1);
    if (op < 0) return;
    switch ((TogetherOp)op) {
    case TogetherOp::Pause:  player_.SetPlaying(false); break;
    case TogetherOp::Resume: player_.SetPlaying(true); break;
    case TogetherOp::Seek:   if (arg >= 0 && arg <= kMaxTrackSec * 1000LL) player_.Seek((int)arg); break;
    case TogetherOp::Next:   player_.Step(1); break;
    case TogetherOp::Prev:   player_.Step(-1); break;
    case TogetherOp::Pick:
        if (Str(req, "list") != list_ || arg < 0 || arg >= (long long)player_.Queue().size()) {
            Log("没有执行点歌：客人看到的歌单已经不是当前的歌单");
            return;
        }
        player_.Play((int)arg, true);
        break;
    }
    Log("执行了 %s 的请求：%s，参数 %lld", guest_->name.c_str(), kOpNames[op], arg);
}

/* Moves on once when the guest reports that it cannot play the current track */
void HostSession::SkipIfUnplayable() {
    const Track t = Current();
    const Guest& g = *guest_;
    if (g.health != Health::Failed || t.index < 0 || g.track.Key() != t.Key()) return;
    if (!skipped_.insert(t.Key()).second) return;
    Log("%s 放不了第 %d 首，跳到下一首", g.name.c_str(), t.index + 1);
    notices_.Post(TogetherNotice::PeerCantPlay, g.name);
    player_.Step(1);
}

void HostSession::Update() {
    if (guest_ && Since(guest_->heardAt) > kSilentMs) Drop("长时间没有消息，已移出房间");
    if (!guest_) return;
    RefreshList();
    const Track t = Current();
    const unsigned long long since = Since(sentAt_);
    if (since >= kTogetherPeriodMs || (since >= kMinGapMs && (dirty_ || Moved(t)))) SendState(t);
    SendParts();
}

void HostSession::RefreshList() {
    const std::vector<Song>& queue = player_.Queue();
    std::string list = ListId(queue);
    if (list == list_) return;
    list_ = std::move(list);
    parts_ = SplitParts(queue, list_);
    partSeq_.assign(parts_.size(), 0);
    Log("歌单 %s 有 %d 首，分成 %d 段", list_.c_str(), (int)queue.size(), (int)parts_.size());
}

Track HostSession::Current() const {
    const TogetherPlayer::State s = player_.Now();
    const std::vector<Song>& queue = player_.Queue();
    Track t;
    t.list = list_;
    if (s.index >= 0 && s.index < (int)queue.size()) {
        t.index = s.index;
        t.hash = queue[s.index].hash;
    }
    t.play = !s.playing ? Play::Paused : (s.buffering || s.failed) ? Play::Buffering : Play::Playing;
    t.posMs = s.posMs;
    return t;
}

/* Whether the playback differs from what the guest infers from the latest state */
bool HostSession::Moved(const Track& t) const {
    if (t.Key() != sent_.Key() || t.play != sent_.play) return true;
    const long long expected = sent_.posMs + (sent_.play == Play::Playing ? (long long)Since(sentAt_) : 0);
    return std::llabs(t.posMs - expected) > kToleranceMs;
}

void HostSession::SendState(const Track& t) {
    Send("state", ",\"list\":" + JsonQuote(t.list) + ",\"parts\":" + std::to_string(parts_.size()) +
                  ",\"index\":" + std::to_string(t.index) + ",\"hash\":" + JsonQuote(t.hash) +
                  ",\"play\":\"" + kPlayNames[(int)t.play] + "\",\"pos\":" + std::to_string(t.posMs) +
                  ",\"members\":[{\"id\":" + JsonQuote(guest_->id) + ",\"done\":" + std::to_string(known_[guest_->id].done) + "}]");
    sent_ = t;
    sentAt_ = now_;
    dirty_ = false;
}

/* Sends the parts of the queue that the guest lacks. A part already sent goes again only once the guest has
   heard a later message, as the part was then lost rather than still on its way */
void HostSession::SendParts() {
    const Guest& g = *guest_;
    if (g.track.list == list_) return;
    for (int i = 0; i < (int)parts_.size(); i++) {
        const bool lacking = g.want != list_ || std::find(g.missing.begin(), g.missing.end(), i) != g.missing.end();
        if (lacking && partSeq_[i] <= g.heard) partSeq_[i] = Send("list", parts_[i]);
    }
}

class GuestSession : public Session {
public:
    GuestSession(TogetherPlayer& player, TogetherLink& link, Notices& notices, const std::string& code)
        : Session(player, link, notices, false, code) {}

    int  StartPosMs(int index) const override;
    bool HoldsTrackEnd() const override { return phase_ == TogetherPhase::Together; }
    bool RoutesControls() const override { return phase_ == TogetherPhase::Together || phase_ == TogetherPhase::Lost; }
    void Request(TogetherOp op, int arg) override;

private:
    struct HostState {
        long long seq = 0;
        unsigned long long sentAt = 0;
        Track track;
        bool admitted = false;    // this guest is on the member list
        long long done = 0;       // newest request of this guest that the host has handled
    };
    struct Ask {
        TogetherOp op;
        int arg;
        long long seq;
        std::string list;         // the queue the guest saw when asking
    };

    void OnMessage(const Envelope& e, const JVal& m) override;
    void Update() override;
    TogetherPhase Phase() const override { return phase_; }
    std::string Peer() const override { return hostName_; }

    void OnState(const Envelope& e, const JVal& m);
    void OnPart(const Envelope& e, const JVal& m);
    void NoteList(const std::string& list, int parts, long long seq);
    TogetherPhase CurrentPhase() const;
    void ApplyQueue();
    void Follow();
    void SendStatus();
    const Song* HostSong() const;
    long long HostPosMs() const;
    std::vector<int> Missing() const;

    TogetherPhase phase_ = TogetherPhase::Waiting;
    std::string hostId_, hostName_;
    long long hostSeq_ = 0;
    unsigned long long heardAt_ = 0;           // 0 until the host is heard
    HostState state_;
    unsigned long long unlistedSince_ = 0;     // since when the states have left this guest out, 0 when listed
    std::deque<std::pair<unsigned long long, long long>> skew_;   // arrival time, arrival minus send time
    long long offset_ = 0;                     // from the host's clock to this one: the least skew
    std::string queueList_;                    // the list this session put into the player's queue
    bool queueShown_ = false;
    std::string wantList_;                     // the host's list according to its newest message
    long long wantSeq_ = 0;
    std::vector<std::vector<Song>> wantParts_; // parts collected for wantList_, empty until received
    std::string trackKey_;
    bool playing_ = false;                     // whether Follow last had the player play or pause
    bool retried_ = false;
    bool cantPlay_ = false;
    unsigned long long seekAt_ = 0;
    std::optional<Ask> ask_;
    long long askSeq_ = 0;
    std::string statusSent_;
    unsigned long long statusAt_ = 0;
};

void GuestSession::OnMessage(const Envelope& e, const JVal& m) {
    if (hostId_.empty()) hostId_ = e.from;
    if (e.from != hostId_) return;
    if (e.type == "bye") {
        Log("房主结束了房间");
        notices_.Post(TogetherNotice::HostEnded);
        link_.Leave();
        ended_ = true;
        return;
    }

    skew_.emplace_back(e.arrivedAt, (long long)e.arrivedAt - (long long)e.sentAt);
    while (skew_.front().first + kSkewWindowMs < e.arrivedAt) skew_.pop_front();
    offset_ = std::min_element(skew_.begin(), skew_.end(),
                               [](const auto& a, const auto& b) { return a.second < b.second; })->second;

    if (e.seq > hostSeq_) {
        hostSeq_ = e.seq;
        heardAt_ = e.arrivedAt;
        hostName_ = e.name;
    }
    if (e.type == "state") OnState(e, m);
    else if (e.type == "list") OnPart(e, m);
}

void GuestSession::OnState(const Envelope& e, const JVal& m) {
    if (e.seq <= state_.seq) return;
    HostState s;
    s.seq = e.seq;
    s.sentAt = e.sentAt;
    s.track = ReadTrack(m);
    const int play = Lookup(kPlayNames, Str(m, "play"));
    const long long parts = Int(m.get("parts"), -1);
    if (play < 0 || parts < 0 || parts > kMaxParts || s.track.list.empty() != (parts == 0)) return;
    s.track.play = (Play)play;
    s.track.posMs = (int)std::clamp(Int(m.get("pos")), 0LL, kMaxTrackSec * 1000LL);
    if (const JVal* members = m.get("members"))
        for (const JVal& member : members->a)
            if (Str(member, "id") == id_) {
                s.admitted = true;
                s.done = Int(member.get("done"));
            }
    state_ = s;

    NoteList(s.track.list, (int)parts, e.seq);
    if (s.admitted) unlistedSince_ = 0;
    else if (!unlistedSince_) unlistedSince_ = e.arrivedAt;
    if (ask_ && s.admitted && s.done >= ask_->seq) ask_.reset();
}

void GuestSession::OnPart(const Envelope& e, const JVal& m) {
    const std::string list = Str(m, "list");
    const long long part = Int(m.get("part"), -1);
    const long long parts = Int(m.get("parts"));
    const JVal* songs = m.get("songs");
    if (list.empty() || parts < 1 || parts > kMaxParts || part < 0 || part >= parts || !songs || !songs->count()) return;
    NoteList(list, (int)parts, e.seq);
    if (list != wantList_ || part >= (long long)wantParts_.size() || !wantParts_[part].empty()) return;
    for (const JVal& song : songs->a) wantParts_[part].push_back(ReadSong(song));
}

/* Takes the list named by a host message as the host's current one when no newer message named another */
void GuestSession::NoteList(const std::string& list, int parts, long long seq) {
    if (seq < wantSeq_) return;
    wantSeq_ = seq;
    if (list == wantList_) return;
    wantList_ = list;
    wantParts_.assign(list == queueList_ ? 0 : parts, std::vector<Song>());
}

std::vector<int> GuestSession::Missing() const {
    std::vector<int> missing;
    for (size_t i = 0; i < wantParts_.size(); i++)
        if (wantParts_[i].empty()) missing.push_back((int)i);
    return missing;
}

TogetherPhase GuestSession::CurrentPhase() const {
    if (!heardAt_) return Since(joinedAt_) > kNotFoundMs ? TogetherPhase::NotFound : TogetherPhase::Waiting;
    if (Since(heardAt_) > kSilentMs) return TogetherPhase::Lost;
    if (state_.admitted) return TogetherPhase::Together;
    return unlistedSince_ && Since(unlistedSince_) > kFullMs ? TogetherPhase::Full : TogetherPhase::Waiting;
}

void GuestSession::Update() {
    const TogetherPhase phase = CurrentPhase();
    if (phase != phase_) {
        phase_ = phase;
        Log("%s", GuestPhaseText(phase));
    }
    if (phase_ == TogetherPhase::Together) {
        ApplyQueue();
        Follow();
    }
    SendStatus();
}

void GuestSession::ApplyQueue() {
    if (wantList_.empty() || wantList_ == queueList_ || !Missing().empty()) return;
    std::vector<Song> songs;
    for (std::vector<Song>& part : wantParts_)
        songs.insert(songs.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    wantParts_.clear();
    Log("收齐了房主的歌单 %s，共 %d 首", wantList_.c_str(), (int)songs.size());
    player_.SetQueue(std::move(songs), !queueShown_);
    queueShown_ = true;
    queueList_ = wantList_;
}

/* The local copy of the host's current track; null while the queue does not hold it */
const Song* GuestSession::HostSong() const {
    const Track& h = state_.track;
    const std::vector<Song>& queue = player_.Queue();
    if (h.list != queueList_ || h.index < 0 || h.index >= (int)queue.size() || queue[h.index].hash != h.hash) return nullptr;
    return &queue[h.index];
}

/* The host's position at this tick, extrapolated from its latest state */
long long GuestSession::HostPosMs() const {
    const Track& h = state_.track;
    if (h.play != Play::Playing) return h.posMs;
    return h.posMs + ((long long)now_ - ((long long)state_.sentAt + offset_));
}

/* Brings the player to the host's track, play state and position as the latest state gives them */
void GuestSession::Follow() {
    const Song* song = HostSong();
    if (!song) return;
    const Track& h = state_.track;
    if (h.Key() != trackKey_) {
        trackKey_ = h.Key();
        retried_ = cantPlay_ = false;
    }
    const TogetherPlayer::State p = player_.Now();
    const bool play = h.play == Play::Playing;
    if (p.index != h.index) {
        Log("播放第 %d 首", h.index + 1);
        player_.Play(h.index, play);
        playing_ = play;
        return;
    }
    if (p.failed) {
        if (!retried_) {
            retried_ = true;
            Log("第 %d 首打不开，重试一次", h.index + 1);
            player_.Retry();
        } else if (!cantPlay_) {
            cantPlay_ = true;
            Log("放不了第 %d 首，已告诉房主", h.index + 1);
            notices_.Post(TogetherNotice::CantPlay);
        }
        return;
    }
    const long long hostPos = HostPosMs();
    // The player pauses by itself only at the end of its copy of the track, which may be shorter than the host's;
    // it then waits there for the host's next track
    if (play && playing_ && !p.playing && hostPos >= p.posMs - kToleranceMs) return;
    if (p.playing != play) player_.SetPlaying(play);
    playing_ = play;
    const int target = StartPoint(hostPos, song->durationSec);
    // A pending request may already have moved the player, as dragging the progress bar does
    if (!ask_ && !p.buffering && !p.dragging && Since(seekAt_) > kSeekGapMs && std::abs(p.posMs - target) > kToleranceMs) {
        Log("进度与房主相差 %d 毫秒，跳到 %d 毫秒", p.posMs - target, target);
        seekAt_ = now_;
        player_.Seek(target);
    }
}

void GuestSession::SendStatus() {
    const TogetherPlayer::State p = player_.Now();
    const std::vector<Song>& queue = player_.Queue();
    const Health health = cantPlay_ && p.failed ? Health::Failed : p.buffering ? Health::Loading : Health::Ok;
    std::string fields = ",\"list\":" + JsonQuote(queueList_) + ",\"index\":" + std::to_string(p.index) +
                         ",\"hash\":" + JsonQuote(p.index >= 0 && p.index < (int)queue.size() ? queue[p.index].hash : "") +
                         ",\"health\":\"" + kHealthNames[(int)health] + "\",\"want\":" + JsonQuote(wantList_);
    if (ask_)
        fields += ",\"req\":{\"op\":\"" + std::string(kOpNames[(int)ask_->op]) + "\",\"arg\":" + std::to_string(ask_->arg) +
                  ",\"seq\":" + std::to_string(ask_->seq) + ",\"list\":" + JsonQuote(ask_->list) + "}";
    const unsigned long long since = Since(statusAt_);
    if (since < kMinGapMs || (since < kTogetherPeriodMs && fields == statusSent_)) return;
    statusSent_ = fields;
    statusAt_ = now_;
    // heard and missing stay out of the comparison above, so that a message from the host sends no status
    std::string missing;
    for (int part : Missing()) missing += (missing.empty() ? "" : ",") + std::to_string(part);
    Send("status", fields + ",\"heard\":" + std::to_string(hostSeq_) + ",\"missing\":[" + missing + "]");
}

int GuestSession::StartPosMs(int index) const {
    const Song* song = HostSong();
    if (phase_ != TogetherPhase::Together || !song || index != state_.track.index) return -1;
    return StartPoint(HostPosMs(), song->durationSec);
}

void GuestSession::Request(TogetherOp op, int arg) {
    if (phase_ != TogetherPhase::Together) return;
    ask_ = Ask{ op, arg, ++askSeq_, queueList_ };
    Log("请房主执行 %s，参数 %d", kOpNames[(int)op], arg);
}

/* One program's side of listening together: its player and link, its notices, and the current session */
struct Client {
    TogetherPlayer* player = nullptr;
    TogetherLink*   link = nullptr;
    Notices notices;
    std::unique_ptr<Session> session;

    void Start(bool host, const std::string& code) {
        Leave();
        if (host) session = std::make_unique<HostSession>(*player, *link, notices);
        else session = std::make_unique<GuestSession>(*player, *link, notices, code);
    }
    void Leave() {
        if (!session) return;
        session->Leave();
        session.reset();
    }
    void Tick(unsigned long long now) {
        if (!session) return;
        session->Tick(now);
        if (session->Ended()) session.reset();
    }
    TogetherView View() const { return session ? session->View() : NoticeView(notices); }
    int  StartPosMs(int index) const { return session ? session->StartPosMs(index) : -1; }
    bool HoldsTrackEnd() const { return session && session->HoldsTrackEnd(); }
    bool RoutesControls() const { return session && session->RoutesControls(); }
    void Request(TogetherOp op, int arg) { if (session) session->Request(op, arg); }
};

Client g_client;

}  // namespace

void TogetherInit(TogetherPlayer* player, TogetherLink* link) {
    g_client.player = player;
    g_client.link = link;
}

void TogetherHost() { g_client.Start(true, ""); }
void TogetherJoin(const std::string& code) { g_client.Start(false, code); }
void TogetherLeave() { g_client.Leave(); }
void TogetherTick(unsigned long long nowMs) { g_client.Tick(nowMs); }
TogetherView TogetherStatus() { return g_client.View(); }

int  TogetherStartPosMs(int index) { return g_client.StartPosMs(index); }
bool TogetherHoldsTrackEnd() { return g_client.HoldsTrackEnd(); }
bool TogetherRoutesControls() { return g_client.RoutesControls(); }
void TogetherRequest(TogetherOp op, int arg) { g_client.Request(op, arg); }
