// Automatic port forwarding with UPnP, so friends on the internet can join a
// server hosted on this PC without touching the router settings.
//
// Asks the home router (Internet Gateway Device) to forward the game's UDP
// ports to this computer and reports the public IP address to share. Runs on
// a background thread; most home routers support it, some have it switched off.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sg {

class PortForwarder {
 public:
  enum class State { Idle, Working, Open, Failed };
  struct Status {
    State state = State::Idle;
    std::string publicIp;  // what friends type in (when Open)
    std::string localIp;   // this PC on the home network
    std::string message;   // human-readable result
    bool behindCarrierNat = false;  // ISP shares one public IP: forwarding can't work
  };

  ~PortForwarder();
  void start(const std::vector<uint16_t>& udpPorts);  // returns immediately
  void stop();                                        // removes the mappings (waits up to ~2 s)
  Status status() const;

 private:
  void run(std::vector<uint16_t> ports);
  void set(State s, const std::string& msg);

  mutable std::mutex mu_;
  Status status_;
  std::thread thread_;
  // Filled by run() so stop() can remove the mappings again.
  std::string controlHost_, controlPath_, serviceType_;
  uint16_t controlPort_ = 0;
  std::vector<uint16_t> mapped_;
};

}  // namespace sg
