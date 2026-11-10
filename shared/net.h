// Minimal cross-platform, non-blocking UDP socket (Winsock or POSIX).
// Kept behind this small interface so ENet or Steam Datagram Relay can be
// swapped in later without touching game code.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sg {

struct Address {
  uint32_t ip = 0;    // host byte order, e.g. 127.0.0.1 = 0x7F000001
  uint16_t port = 0;  // host byte order

  bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
  bool operator!=(const Address& o) const { return !(*this == o); }
  std::string toString() const;

  // Parses "1.2.3.4" or "localhost". Returns false on failure.
  static bool parse(const std::string& host, uint16_t port, Address& out);
};

struct AddressHash {
  size_t operator()(const Address& a) const { return (static_cast<size_t>(a.ip) << 16) ^ a.port; }
};

// Call once at startup / shutdown (no-ops on POSIX).
bool netInit();
void netShutdown();

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // port 0 = let the OS pick (clients).
  bool open(uint16_t port);
  void close();
  bool isOpen() const { return handle_ != kInvalid; }

  bool send(const Address& to, const void* data, size_t size);
  // Returns bytes received, 0 if nothing is waiting, -1 on error.
  int receive(Address& from, void* buffer, size_t capacity);

 private:
#ifdef _WIN32
  using Handle = uintptr_t;
#else
  using Handle = int;
#endif
  static constexpr Handle kInvalid = static_cast<Handle>(-1);
  Handle handle_ = kInvalid;
};

}  // namespace sg
