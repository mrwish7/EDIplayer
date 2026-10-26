#pragma once

// EDI network input: receives AF or PF (PFT) packets over UDP (unicast or
// multicast) or TCP, reassembles PFT fragments and hands complete AF packets
// to a callback on its own receive thread.
//
// Source URL forms:
//   tcp://host:port                 connect to an EDI TCP server
//   udp://port  or  udp://:port     listen for unicast UDP
//   udp://group:port[?iface=ip]     join a multicast group (optionally on
//                                   the interface with that local address)
//   host:port                       TCP (UDP if host is a multicast address)
//   port                            UDP unicast

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct EdiSourceUrl {
    bool tcp = true;
    std::string host;     // TCP host, or UDP multicast group (empty = unicast)
    uint16_t port = 0;
    std::string iface;    // UDP multicast interface address (optional)

    static bool parse(const std::string &url, EdiSourceUrl &out, std::string &err);
    std::string describe() const;
};

class EdiSource {
public:
    enum class State { Idle, Connecting, Receiving, Waiting };

    using PacketCallback = std::function<void(const uint8_t *af, size_t len)>;

    EdiSource();
    ~EdiSource();

    bool start(const EdiSourceUrl &url, PacketCallback cb);
    void stop();

    State state() const { return state_.load(); }
    std::string lastError() const;

    // Counters (updated by the receive thread)
    std::atomic<uint64_t> bytes{0}, packets{0}, afPackets{0}, pfPackets{0}, reconnects{0};

private:
    void run();
    void runTcp();
    void runUdp();
    void handlePacket(const uint8_t *p, size_t n);
    void setError(const std::string &e);
    bool sleepInterruptible(int ms);

    EdiSourceUrl url_;
    PacketCallback cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Idle};
    mutable std::mutex errMutex_;
    std::string lastError_;
    struct Impl;
    Impl *impl_;
};
