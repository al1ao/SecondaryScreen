#include "net/tcp_server.h"
#include <algorithm>
#include <limits>
#include "util/log.h"

namespace pds {
WinsockRuntime::WinsockRuntime() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    ready_ = result == 0;
    if (!ready_) LOG_HR(HRESULT_FROM_WIN32(static_cast<DWORD>(result)), "WSAStartup");
}
WinsockRuntime::~WinsockRuntime() {
    if (ready_ && WSACleanup() != 0) LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "WSACleanup");
}
UniqueSocket::~UniqueSocket() { Reset(); }
void UniqueSocket::Reset(SOCKET socket) { TcpServer::Close(socket_); socket_ = socket; }
void TcpServer::Close(SOCKET socket) {
    if (socket != INVALID_SOCKET && closesocket(socket) == SOCKET_ERROR)
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "closesocket");
}
bool TcpServer::Listen(uint16_t port) {
    listen_.Reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listen_.Get() == INVALID_SOCKET) {
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "socket"); return false;
    }
    const BOOL reuse = TRUE;
    if (setsockopt(listen_.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "SO_REUSEADDR"); return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listen_.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listen_.Get(), SOMAXCONN) == SOCKET_ERROR) {
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "bind/listen loopback"); return false;
    }
    return true;
}
SOCKET TcpServer::Accept(std::string& peer) {
    sockaddr_in address{};
    int size = sizeof(address);
    UniqueSocket client(accept(listen_.Get(), reinterpret_cast<sockaddr*>(&address), &size));
    if (client.Get() == INVALID_SOCKET) { LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "accept"); return INVALID_SOCKET; }
    const BOOL no_delay = TRUE;
    const DWORD timeout = 5000;
    if (setsockopt(client.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) != 0 ||
        setsockopt(client.Get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0 ||
        setsockopt(client.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "client socket options"); return INVALID_SOCKET;
    }
    char ip[INET_ADDRSTRLEN]{};
    if (!InetNtopA(AF_INET, &address.sin_addr, ip, sizeof(ip))) {
        LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "InetNtop"); return INVALID_SOCKET;
    }
    peer = std::string(ip) + ':' + std::to_string(ntohs(address.sin_port));
    return client.Release();
}
bool TcpServer::SendAll(SOCKET socket, const void* data, size_t n) {
    const auto* bytes = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < n) {
        const int chunk = static_cast<int>((std::min)(n - sent, static_cast<size_t>((std::numeric_limits<int>::max)())));
        const int result = send(socket, bytes + sent, chunk, 0);
        if (result <= 0) {
            if (result < 0) LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "send");
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}
bool TcpServer::RecvExact(SOCKET socket, void* data, size_t n) {
    auto* bytes = static_cast<char*>(data);
    size_t got = 0;
    while (got < n) {
        const int chunk = static_cast<int>((std::min)(n - got, static_cast<size_t>((std::numeric_limits<int>::max)())));
        const int result = recv(socket, bytes + got, chunk, 0);
        if (result <= 0) {
            if (result < 0) LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "recv");
            return false;
        }
        got += static_cast<size_t>(result);
    }
    return true;
}
bool Readable(SOCKET socket, long timeout_ms) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int result = select(0, &read_set, nullptr, nullptr, &timeout);
    if (result == SOCKET_ERROR) LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "select");
    return result != 0; // Errors enter recv/accept, which handles the failed socket.
}
}
