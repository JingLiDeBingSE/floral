#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <string>

#include "together.h"

/* Starts the relay helper with its stdin and stdout connected to the returned pipe ends; the caller owns
   the three handles. Implemented by main.cpp, which launches every sidecar */
using RelayLaunch = bool (*)(HANDLE* toHelper, HANDLE* fromHelper, HANDLE* process);

struct RelayBroker {
    std::string host;
    int         port = 0;
    std::string user;
    std::string pass;
};

/* TogetherLink over the Node relay helper (sidecars/together-mqtt/together-server.js), one JSON line per
   message in each direction */
class Relay : public TogetherLink {
public:
    Relay(RelayLaunch launch, RelayBroker broker);
    ~Relay() override;

    void Join(const std::string& code) override;
    void Leave() override;
    void Send(const std::string& json) override;
    bool Poll(Event& ev) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
