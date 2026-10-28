#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pds {
class WinsockRuntime {
public:
    WinsockRuntime();
    ~WinsockRuntime();
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    bool Ready() const { return ready_; }
private:
    bool ready_ = false;
};
class UniqueSocket {
public:
    explicit UniqueSocket(SOCKET socket = INVALID_SOCKET) : socket_(socket) {}
    ~UniqueSocket();
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& other) noexcept : socket_(other.Release()) {}
    UniqueSocket& operator=(UniqueSocket&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }
    SOCKET Get() const { return socket_; }
    void Reset(SOCKET socket = INVALID_SOCKET);
    SOCKET Release() { const auto socket = socket_; socket_ = INVALID_SOCKET; return socket; }
private:
    SOCKET socket_;
};
class TcpServer {
public:
    bool Listen(uint16_t port);
    SOCKET Accept(std::string& peer);
    SOCKET Socket() const { return listen_.Get(); }
    static void Close(SOCKET socket);
    static bool SendAll(SOCKET socket, const void* data, size_t n);
    static bool RecvExact(SOCKET socket, void* data, size_t n);
private:
    UniqueSocket listen_;
};
bool Readable(SOCKET socket, long timeout_ms);
}
