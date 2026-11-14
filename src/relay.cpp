#include "relay.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "json.h"

namespace {

constexpr size_t kMaxQueuedMessages = 64;       // waiting for the helper; beyond this the oldest is dropped
constexpr unsigned long long kRetryMs = 5000;   // pause before restarting the helper or joining again
constexpr DWORD kExitWaitMs = 1000;             // for the helper to leave the room when the relay is destroyed

struct Line {
    std::string text;
    bool message;   // a send, which the queue may drop
};

std::string JoinCommand(long long id, const std::string& code, const RelayBroker& broker) {
    return "{\"cmd\":\"join\",\"id\":" + std::to_string(id) + ",\"code\":" + JsonQuote(code) +
           ",\"host\":" + JsonQuote(broker.host) + ",\"port\":" + std::to_string(broker.port) +
           ",\"user\":" + JsonQuote(broker.user) + ",\"pass\":" + JsonQuote(broker.pass) + "}";
}

}  // namespace

/* The worker thread starts the helper, writes the queued lines to it and restarts it after an unexpected
   exit; a reader thread per helper process turns its output into events. mx guards the state they share */
struct Relay::Impl {
    RelayLaunch launch = nullptr;
    RelayBroker broker;

    std::mutex mx;
    std::condition_variable wake;
    std::deque<Event> events;
    std::deque<Line> lines;   // for the helper, in order
    size_t messages = 0;      // sends among lines
    bool quit = false;

    long long id = 0;         // of the current Join; the helper tags its events with it
    bool inRoom = false;      // from Join until Leave or Failed
    bool joined = false;      // Joined reported
    bool down = false;        // Down reported and Up not yet
    bool rejoin = false;      // the room is to be joined again at retryAt
    unsigned long long retryAt = 0;
    std::string code;         // the room code from Joined

    // Opened and closed by the worker thread
    HANDLE process = nullptr, toHelper = nullptr, fromHelper = nullptr;
    bool running = false;     // the helper's output has not ended
    std::thread reader, worker;

    void Report(Event::Kind kind, std::string text, unsigned long long atMs) {
        events.push_back({ kind, std::move(text), atMs });
    }

    void Queue(std::string text, bool message) {
        if (message && messages == kMaxQueuedMessages) {
            lines.erase(std::find_if(lines.begin(), lines.end(), [](const Line& l) { return l.message; }));
            --messages;
        }
        if (message) ++messages;
        lines.push_back({ std::move(text), message });
        wake.notify_all();
    }

    void DropLines() {
        lines.clear();
        messages = 0;
    }

    /* Starts a new join id, so that events of the previous room still on their way are ignored */
    void ResetRoom() {
        ++id;
        inRoom = joined = down = rejoin = false;
        code.clear();
        events.clear();
    }

    void MarkDown(unsigned long long atMs) {
        if (!down) Report(Event::Down, "", atMs);
        down = true;
    }

    void MarkUp(unsigned long long atMs) {
        if (down) Report(Event::Up, "", atMs);
        down = false;
    }

    /* The helper has lost the room it had joined and cannot get it back by itself */
    void RejoinLater(unsigned long long now) {
        MarkDown(now);
        rejoin = true;
        retryAt = now + kRetryMs;
        wake.notify_all();
    }

    void Handle(const std::string& line, unsigned long long atMs) {
        JVal v;
        if (!Json(line, v)) return;
        const JVal* tag = v.get("id");
        const JVal* kindVal = v.get("ev");
        const JVal* textVal = v.get("text");
        const std::string kind = kindVal ? kindVal->str() : "";
        std::string text = textVal ? textVal->str() : "";

        std::lock_guard<std::mutex> lk(mx);
        if (!tag || tag->ll() != id) return;
        if (kind == "message") {
            Report(Event::Message, std::move(text), atMs);
        } else if (kind == "joined") {
            code = text;
            if (joined) MarkUp(atMs);
            else Report(Event::Joined, std::move(text), atMs);
            joined = true;
        } else if (kind == "failed") {
            if (joined) {
                RejoinLater(atMs);
            } else {
                inRoom = false;
                Report(Event::Failed, std::move(text), atMs);
            }
        } else if (kind == "down") {
            MarkDown(atMs);
        } else if (kind == "up") {
            MarkUp(atMs);
        }
    }

    void Read() {
        std::string pending;
        char chunk[65536];
        DWORD n = 0;
        while (ReadFile(fromHelper, chunk, sizeof(chunk), &n, nullptr) && n) {
            const unsigned long long atMs = GetTickCount64();
            pending.append(chunk, n);
            size_t begin = 0;
            for (size_t end; (end = pending.find('\n', begin)) != std::string::npos; begin = end + 1)
                Handle(pending.substr(begin, end - begin), atMs);
            pending.erase(0, begin);
        }

        std::lock_guard<std::mutex> lk(mx);
        const unsigned long long now = GetTickCount64();
        running = false;
        DropLines();
        if (inRoom && joined) {
            RejoinLater(now);
        } else if (inRoom) {
            inRoom = false;
            Report(Event::Failed, "relay helper exited", now);
        }
        wake.notify_all();
    }

    void Work() {
        std::unique_lock<std::mutex> lk(mx);
        for (;;) {
            if (process && !running) {
                lk.unlock();
                reader.join();
                CloseHandle(toHelper);
                CloseHandle(fromHelper);
                CloseHandle(process);
                lk.lock();
                process = nullptr;
                continue;
            }
            if (quit && (!running || lines.empty())) break;   // the lines queued by Leave, such as bye, go out first

            const unsigned long long now = GetTickCount64();
            const bool rejoinDue = rejoin && now >= retryAt;
            if (inRoom && !process && (!rejoin || rejoinDue)) {
                lk.unlock();
                HANDLE to = nullptr, from = nullptr, proc = nullptr;
                const bool started = launch(&to, &from, &proc);
                lk.lock();
                if (started) {
                    toHelper = to;
                    fromHelper = from;
                    process = proc;
                    running = true;
                    reader = std::thread(&Impl::Read, this);
                } else if (joined) {
                    RejoinLater(GetTickCount64());
                } else if (inRoom) {
                    inRoom = false;
                    DropLines();
                    Report(Event::Failed, "relay helper could not start", GetTickCount64());
                }
                continue;
            }
            if (rejoinDue && running) {
                rejoin = false;
                Queue(JoinCommand(id, code, broker), false);
            }
            if (running && !lines.empty()) {
                const std::string text = lines.front().text + '\n';
                if (lines.front().message) --messages;
                lines.pop_front();
                lk.unlock();
                // A failed write means the helper has exited, which the reader reports
                DWORD written = 0;
                WriteFile(toHelper, text.data(), (DWORD)text.size(), &written, nullptr);
                lk.lock();
                continue;
            }
            if (rejoin) wake.wait_for(lk, std::chrono::milliseconds(retryAt - now));
            else wake.wait(lk);
        }
        lk.unlock();

        if (!process) return;
        CloseHandle(toHelper);   // the helper leaves the room and exits when its input ends
        if (WaitForSingleObject(process, kExitWaitMs) == WAIT_TIMEOUT) TerminateProcess(process, 0);
        reader.join();
        CloseHandle(fromHelper);
        CloseHandle(process);
    }
};

Relay::Relay(RelayLaunch launch, RelayBroker broker) : impl_(new Impl) {
    impl_->launch = launch;
    impl_->broker = std::move(broker);
    impl_->worker = std::thread(&Impl::Work, impl_.get());
}

Relay::~Relay() {
    {
        std::lock_guard<std::mutex> lk(impl_->mx);
        impl_->quit = true;
    }
    impl_->wake.notify_all();
    impl_->worker.join();
}

void Relay::Join(const std::string& code) {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->ResetRoom();
    impl_->inRoom = true;
    impl_->Queue(JoinCommand(impl_->id, code, impl_->broker), false);
}

void Relay::Leave() {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->ResetRoom();
    if (impl_->running) impl_->Queue("{\"cmd\":\"leave\"}", false);
    else impl_->DropLines();
}

void Relay::Send(const std::string& json) {
    std::lock_guard<std::mutex> lk(impl_->mx);
    if (impl_->inRoom) impl_->Queue("{\"cmd\":\"send\",\"text\":" + JsonQuote(json) + "}", true);
}

bool Relay::Poll(Event& ev) {
    std::lock_guard<std::mutex> lk(impl_->mx);
    if (impl_->events.empty()) return false;
    ev = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}
