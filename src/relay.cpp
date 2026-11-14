#include "relay.h"

#include <deque>

struct Relay::Impl {
    std::deque<Event> events;
};

Relay::Relay(RelayLaunch, RelayBroker) : impl_(new Impl) {}
Relay::~Relay() = default;

void Relay::Join(const std::string&) {
    Event ev;
    ev.kind = Event::Failed;
    ev.text = "relay not available";
    ev.atMs = GetTickCount64();
    impl_->events.push_back(ev);
}

void Relay::Leave() {}
void Relay::Send(const std::string&) {}

bool Relay::Poll(Event& ev) {
    if (impl_->events.empty()) return false;
    ev = impl_->events.front();
    impl_->events.pop_front();
    return true;
}
