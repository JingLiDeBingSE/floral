#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <vector>
#include <string>

#include "together.h"

/* Starts the relay helper with its stdin and stdout connected to the returned pipe ends; the caller owns
   the three handles. Implemented by main.cpp, which launches every sidecar */
using RelayLaunch = bool (*)(HANDLE* toHelper, HANDLE* fromHelper, HANDLE* process);

struct RelayEndpoint {
    std::string host;
    int         port = 0;
};

/* Tried in order: the first that accepts the connection is used for the whole room */
using RelayEndpoints = std::vector<RelayEndpoint>;

/* TogetherLink over the Node relay helper (sidecars/together-mqtt/together-server.js), one JSON line per
   message in each direction */
class Relay : public TogetherLink {
public:
    Relay(RelayLaunch launch, RelayEndpoints relays);
    ~Relay() override;

    void Join(const std::string& code) override;
    void Leave() override;
    void Send(const std::string& json) override;
    bool Poll(Event& ev) override;

    /* Control channel: a second connection for the account / friends protocol. Independent of the
       room above, so leaving a room never disturbs it. */
    struct CtlEvent {
        enum Kind { Open, Message, Down } kind = Message;
        std::string text;
    };
    void CtlOpen(RelayEndpoints relays);
    void CtlSend(const std::string& json);
    bool CtlPoll(CtlEvent& ev);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
