#include "edi_source.h"
#include "edi/edi_log.h"
extern "C" {
#include "edi/pft.h"
}

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
static void closeSock(sock_t s) { closesocket(s); }
static int sockError() { return WSAGetLastError(); }
static void setNonBlocking(sock_t s, bool on) { u_long m = on ? 1 : 0; ioctlsocket(s, FIONBIO, &m); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
static void closeSock(sock_t s) { close(s); }
static int sockError() { return errno; }
static void setNonBlocking(sock_t s, bool on)
{
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
#endif

static const int kPollMs = 200;              // stop-flag check interval
static const int kStaleMs = 10000;           // no data for this long: reconnect
static const size_t kMaxPacket = 1 << 20;    // larger "packets" mean we lost sync

// ---------------------------------------------------------------------------
// URL parsing

static bool isMulticast(const std::string &host)
{
    in_addr a;
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
    return (ntohl(a.s_addr) >> 28) == 0xE;
}

static bool parsePort(const std::string &s, uint16_t &port)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
    long v = atol(s.c_str());
    if (v <= 0 || v > 65535) return false;
    port = (uint16_t)v;
    return true;
}

bool EdiSourceUrl::parse(const std::string &in, EdiSourceUrl &out, std::string &err)
{
    std::string s = in;
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(0, 1);
    out = EdiSourceUrl();

    int scheme = 0;   // 0 none, 1 tcp, 2 udp
    if (s.rfind("tcp://", 0) == 0) { scheme = 1; s = s.substr(6); }
    else if (s.rfind("udp://", 0) == 0) { scheme = 2; s = s.substr(6); }

    size_t q = s.find('?');
    if (q != std::string::npos) {
        std::string query = s.substr(q + 1);
        s = s.substr(0, q);
        if (query.rfind("iface=", 0) == 0) out.iface = query.substr(6);
        else { err = "Unknown option '" + query + "' (only ?iface=<local ip> is supported)"; return false; }
    }

    std::string host, port;
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) port = s;
    else { host = s.substr(0, colon); port = s.substr(colon + 1); }
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);

    if (!parsePort(port, out.port)) {
        err = "Missing or invalid port in '" + in + "'";
        return false;
    }
    if (scheme == 1) out.tcp = true;
    else if (scheme == 2) out.tcp = false;
    else out.tcp = !host.empty() && !isMulticast(host);
    out.host = host;

    if (out.tcp && host.empty()) { err = "TCP source needs a host: tcp://host:port"; return false; }
    if (!out.tcp && !host.empty() && !isMulticast(host)) {
        err = "UDP address must be a multicast group (224.0.0.0/4); use udp://port for unicast";
        return false;
    }
    return true;
}

std::string EdiSourceUrl::describe() const
{
    if (tcp) return "tcp://" + host + ":" + std::to_string(port);
    std::string d = "udp://" + host + ":" + std::to_string(port);
    if (!iface.empty()) d += "?iface=" + iface;
    return d;
}

// ---------------------------------------------------------------------------

struct EdiSource::Impl {
    pft_ctx_t pft;
    uint8_t afBuf[65536];
};

EdiSource::EdiSource() : impl_(new Impl)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

EdiSource::~EdiSource()
{
    stop();
    delete impl_;
#ifdef _WIN32
    WSACleanup();
#endif
}

bool EdiSource::start(const EdiSourceUrl &url, PacketCallback cb)
{
    stop();
    url_ = url;
    cb_ = std::move(cb);
    stop_ = false;
    bytes = packets = afPackets = pfPackets = reconnects = 0;
    setError("");
    pft_init(&impl_->pft);
    state_ = State::Connecting;
    thread_ = std::thread(&EdiSource::run, this);
    return true;
}

void EdiSource::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    state_ = State::Idle;
}

std::string EdiSource::lastError() const
{
    std::lock_guard<std::mutex> lk(errMutex_);
    return lastError_;
}

void EdiSource::setError(const std::string &e)
{
    {
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_ = e;
    }
    if (!e.empty()) edi_log("source: %s", e.c_str());
}

bool EdiSource::sleepInterruptible(int ms)
{
    for (int t = 0; t < ms && !stop_; t += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return !stop_;
}

void EdiSource::run()
{
    if (url_.tcp) runTcp();
    else runUdp();
    state_ = State::Idle;
}

// One complete AF or PF packet received (UDP datagram or TCP-framed)
void EdiSource::handlePacket(const uint8_t *p, size_t n)
{
    packets++;
    if (n >= 2 && p[0] == 'A' && p[1] == 'F') {
        afPackets++;
        cb_(p, n);
    } else if (n >= 2 && p[0] == 'P' && p[1] == 'F') {
        pfPackets++;
        if (pft_push_fragment(&impl_->pft, p, n) > 0) {
            size_t len;
            while ((len = pft_get_af_packet(&impl_->pft, impl_->afBuf, sizeof(impl_->afBuf))) > 0)
                cb_(impl_->afBuf, len);
        }
    }
}

// Wait until s is readable (or writable). Returns 1 ready, 0 timeout, -1 error.
static int waitSock(sock_t s, bool write, int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select((int)s + 1, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
    return r < 0 ? -1 : r;
}

// Length of the AF/PF packet starting at p, 0 if more header bytes are needed,
// or -1 if p doesn't start with a packet sync.
static long packetLength(const uint8_t *p, size_t avail)
{
    if (avail < 2) return 0;
    if (p[0] == 'A' && p[1] == 'F') {
        if (avail < 10) return 0;
        uint32_t plen = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
        size_t total = 10 + (size_t)plen + ((p[8] & 0x80) ? 2 : 0);
        return total > kMaxPacket ? -1 : (long)total;
    }
    if (p[0] == 'P' && p[1] == 'F') {
        if (avail < 14) return 0;
        bool fec = (p[10] & 0x80) != 0, addr = (p[10] & 0x40) != 0;
        size_t plen = ((size_t)(p[10] & 0x3F) << 8) | p[11];
        return (long)(14 + (fec ? 2 : 0) + (addr ? 4 : 0) + plen);
    }
    return -1;
}

static std::string sockErrStr(int e)
{
#ifdef _WIN32
    char buf[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)e, 0, buf, sizeof(buf), nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s.empty() ? "error " + std::to_string(e) : s;
#else
    return strerror(e);
#endif
}

void EdiSource::runTcp()
{
    std::vector<uint8_t> buf;
    bool first = true;

    while (!stop_) {
        if (!first) {
            reconnects++;
            state_ = State::Waiting;
            if (!sleepInterruptible(2000)) break;
        }
        first = false;
        state_ = State::Connecting;

        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        std::string port = std::to_string(url_.port);
        int gai = getaddrinfo(url_.host.c_str(), port.c_str(), &hints, &res);
        if (gai != 0 || !res) {
            setError("cannot resolve " + url_.host + ": " + gai_strerror(gai));
            continue;
        }

        sock_t s = BAD_SOCKET;
        std::string err;
        for (addrinfo *ai = res; ai && s == BAD_SOCKET && !stop_; ai = ai->ai_next) {
            sock_t t = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (t == BAD_SOCKET) continue;
            setNonBlocking(t, true);
            connect(t, ai->ai_addr, (int)ai->ai_addrlen);
            // non-blocking connect: poll for completion so stop() stays responsive
            int r = 0;
            for (int waited = 0; !stop_ && waited < 10000 && (r = waitSock(t, true, kPollMs)) == 0; waited += kPollMs) {}
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (r == 1 && getsockopt(t, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) == 0 && soerr == 0) {
                setNonBlocking(t, false);
                s = t;
            } else {
                err = r == 0 ? "connection timed out" : sockErrStr(soerr ? soerr : sockError());
                closeSock(t);
            }
        }
        freeaddrinfo(res);
        if (stop_) { if (s != BAD_SOCKET) closeSock(s); break; }
        if (s == BAD_SOCKET) {
            setError("connect to " + url_.describe() + " failed: " + err);
            continue;
        }

        int rcvbuf = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
        edi_log("source: connected to %s", url_.describe().c_str());
        setError("");
        state_ = State::Receiving;
        buf.clear();
        size_t skipped = 0;
        auto lastData = std::chrono::steady_clock::now();

        while (!stop_) {
            int r = waitSock(s, false, kPollMs);
            if (r < 0) { setError("connection error: " + sockErrStr(sockError())); break; }
            if (r == 0) {
                if (std::chrono::steady_clock::now() - lastData > std::chrono::milliseconds(kStaleMs)) {
                    setError("no data for " + std::to_string(kStaleMs / 1000) + " s - reconnecting");
                    break;
                }
                continue;
            }
            uint8_t chunk[65536];
            int n = recv(s, (char *)chunk, sizeof(chunk), 0);
            if (n <= 0) {
                setError(n == 0 ? "server closed the connection" : "receive failed: " + sockErrStr(sockError()));
                break;
            }
            lastData = std::chrono::steady_clock::now();
            bytes += (uint64_t)n;
            buf.insert(buf.end(), chunk, chunk + n);

            // split the byte stream into packets
            size_t pos = 0;
            while (pos < buf.size()) {
                long len = packetLength(&buf[pos], buf.size() - pos);
                if (len < 0) {   // lost sync: skip ahead to the next AF/PF
                    pos++;
                    skipped++;
                    continue;
                }
                if (len == 0 || pos + (size_t)len > buf.size()) break;
                if (skipped) {
                    edi_log("source: TCP stream resynchronised (skipped %zu bytes)", skipped);
                    skipped = 0;
                }
                handlePacket(&buf[pos], (size_t)len);
                pos += (size_t)len;
            }
            buf.erase(buf.begin(), buf.begin() + (long)pos);
        }
        closeSock(s);
    }
}

void EdiSource::runUdp()
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCKET) { setError("cannot create UDP socket"); return; }
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    int rcvbuf = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(url_.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
        setError("cannot listen on UDP port " + std::to_string(url_.port) + ": " + sockErrStr(sockError()));
        closeSock(s);
        return;
    }

    if (!url_.host.empty()) {
        ip_mreq mreq = {};
        inet_pton(AF_INET, url_.host.c_str(), &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (!url_.iface.empty() && inet_pton(AF_INET, url_.iface.c_str(), &mreq.imr_interface) != 1) {
            setError("invalid interface address " + url_.iface);
            closeSock(s);
            return;
        }
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) != 0) {
            setError("cannot join multicast group " + url_.host + ": " + sockErrStr(sockError()));
            closeSock(s);
            return;
        }
        edi_log("source: joined %s on %s", url_.host.c_str(), url_.iface.empty() ? "default interface" : url_.iface.c_str());
    }
    edi_log("source: listening on %s", url_.describe().c_str());

    std::vector<uint8_t> pkt(65536);
    auto lastData = std::chrono::steady_clock::now();
    state_ = State::Connecting;    // "waiting for data" until the first datagram
    while (!stop_) {
        int r = waitSock(s, false, kPollMs);
        if (r < 0) { setError("receive error: " + sockErrStr(sockError())); break; }
        if (r == 0) {
            if (state_ == State::Receiving &&
                std::chrono::steady_clock::now() - lastData > std::chrono::milliseconds(kStaleMs)) {
                setError("no data for " + std::to_string(kStaleMs / 1000) + " s");
                state_ = State::Connecting;
            }
            continue;
        }
        int n = recv(s, (char *)pkt.data(), (int)pkt.size(), 0);
        if (n <= 0) continue;
        lastData = std::chrono::steady_clock::now();
        if (state_ != State::Receiving) { state_ = State::Receiving; setError(""); }
        bytes += (uint64_t)n;
        handlePacket(pkt.data(), (size_t)n);
    }
    closeSock(s);
}
