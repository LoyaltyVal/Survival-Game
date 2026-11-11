#include "net.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <cerrno>
#endif

#include <algorithm>
#include <cstring>

namespace sg {

std::string Address::toString() const {
  return std::to_string((ip >> 24) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF) + ":" + std::to_string(port);
}

bool Address::parse(const std::string& host, uint16_t port, Address& out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return false;
  auto* sin = reinterpret_cast<sockaddr_in*>(result->ai_addr);
  out.ip = ntohl(sin->sin_addr.s_addr);
  out.port = port;
  freeaddrinfo(result);
  return true;
}

std::vector<std::string> localAddresses() {
  std::vector<std::string> out;
#ifndef _WIN32
  // Linux/macOS: the hostname usually resolves to 127.0.1.1, so list the interfaces instead.
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (ifaddrs* i = list; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
      uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
      if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;
      std::string s = Address{ip, 0}.toString();
      s = s.substr(0, s.rfind(':'));
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    freeifaddrs(list);
  }
  if (!out.empty()) return out;
#endif
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) != 0) return out;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &result) != 0) return out;
  for (addrinfo* a = result; a; a = a->ai_next) {
    uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
    if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;  // loopback, link-local
    Address addr{ip, 0};
    std::string s = addr.toString();
    s = s.substr(0, s.rfind(':'));
    bool seen = false;
    for (const auto& o : out) seen = seen || o == s;
    if (!seen) out.push_back(s);
  }
  freeaddrinfo(result);
  return out;
}

bool netInit() {
#ifdef _WIN32
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  return true;
#endif
}

void netShutdown() {
#ifdef _WIN32
  WSACleanup();
#endif
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t port) {
  close();
  auto h = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (h == INVALID_SOCKET) return false;
#else
  if (h < 0) return false;
#endif
  handle_ = static_cast<Handle>(h);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(h, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close();
    return false;
  }

#ifdef _WIN32
  // Don't let an ICMP "port unreachable" (e.g. pinging an offline server) error out later receives.
  BOOL reportReset = FALSE;
  DWORD bytes = 0;
  WSAIoctl(h, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &reportReset, sizeof(reportReset), nullptr, 0, &bytes,
           nullptr, nullptr);
  u_long nonBlocking = 1;
  if (ioctlsocket(h, FIONBIO, &nonBlocking) != 0) { close(); return false; }
#else
  if (fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK) != 0) { close(); return false; }
#endif
  return true;
}

void UdpSocket::close() {
  if (handle_ == kInvalid) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(handle_));
#else
  ::close(handle_);
#endif
  handle_ = kInvalid;
}

bool UdpSocket::send(const Address& to, const void* data, size_t size) {
  if (handle_ == kInvalid) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(to.ip);
  addr.sin_port = htons(to.port);
  auto sent = ::sendto(handle_, static_cast<const char*>(data), static_cast<int>(size), 0,
                       reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  return sent == static_cast<decltype(sent)>(size);
}

int UdpSocket::receive(Address& from, void* buffer, size_t capacity) {
  if (handle_ == kInvalid) return -1;
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  auto n = ::recvfrom(handle_, static_cast<char*>(buffer), static_cast<int>(capacity), 0,
                      reinterpret_cast<sockaddr*>(&addr), &len);
  if (n < 0) {
#ifdef _WIN32
    int err = WSAGetLastError();
    // WSAECONNRESET: ICMP "port unreachable" from an earlier send; not fatal for UDP.
    if (err == WSAEWOULDBLOCK || err == WSAECONNRESET) return 0;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED) return 0;
#endif
    return -1;
  }
  from.ip = ntohl(addr.sin_addr.s_addr);
  from.port = ntohs(addr.sin_port);
  return static_cast<int>(n);
}

}  // namespace sg
