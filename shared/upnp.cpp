#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // sscanf on fixed-format router replies
#endif
#include "upnp.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using SockHandle = SOCKET;
static const SockHandle kBad = INVALID_SOCKET;
static void closeSock(SockHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SockHandle = int;
static const SockHandle kBad = -1;
static void closeSock(SockHandle s) { ::close(s); }
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sg {

namespace {

void setTimeout(SockHandle s, int ms) {
#ifdef _WIN32
  DWORD t = static_cast<DWORD>(ms);
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

std::string lowerStr(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string ipToString(uint32_t hostOrder) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (hostOrder >> 24) & 255, (hostOrder >> 16) & 255, (hostOrder >> 8) & 255,
                hostOrder & 255);
  return buf;
}

bool isPrivate(const std::string& ip) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return true;
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
         a == 127 || a == 0;
}

// Splits "http://192.168.1.1:5000/rootDesc.xml".
bool parseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  std::string u = url;
  auto scheme = u.find("://");
  if (scheme != std::string::npos) u = u.substr(scheme + 3);
  auto slash = u.find('/');
  std::string hp = slash == std::string::npos ? u : u.substr(0, slash);
  path = slash == std::string::npos ? "/" : u.substr(slash);
  auto colon = hp.find(':');
  host = hp.substr(0, colon);
  port = colon == std::string::npos ? 80 : static_cast<uint16_t>(std::atoi(hp.substr(colon + 1).c_str()));
  return !host.empty() && port != 0;
}

// Minimal HTTP/1.1 request over TCP. Returns the full response; localIp gets
// the address this PC used to reach the router.
bool http(const std::string& host, uint16_t port, const std::string& request, std::string& response, std::string* localIp) {
  SockHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) return false;
  setTimeout(s, 2500);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
      ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closeSock(s);
    return false;
  }
  if (localIp) {
    sockaddr_in me{};
#ifdef _WIN32
    int len = sizeof(me);
#else
    socklen_t len = sizeof(me);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) *localIp = ipToString(ntohl(me.sin_addr.s_addr));
  }
  size_t sent = 0;
  while (sent < request.size()) {
    int n = ::send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
    if (n <= 0) {
      closeSock(s);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  response.clear();
  char buf[4096];
  for (;;) {
    int n = ::recv(s, buf, sizeof(buf), 0);
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
    if (response.size() > 512 * 1024) break;
  }
  closeSock(s);
  return !response.empty();
}

int httpStatus(const std::string& response) {
  int code = 0;
  std::sscanf(response.c_str(), "HTTP/%*s %d", &code);
  return code;
}

// Text between <tag> and </tag> (ignoring namespace prefixes), starting at `from`.
std::string xmlValue(const std::string& xml, const std::string& tag, size_t from = 0, size_t* endPos = nullptr) {
  std::string lx = lowerStr(xml), lt = lowerStr(tag);
  size_t p = from;
  while ((p = lx.find(lt, p)) != std::string::npos) {
    size_t open = lx.rfind('<', p);
    size_t close = lx.find('>', p);
    if (open != std::string::npos && close != std::string::npos && lx[open + 1] != '/') {
      size_t end = lx.find("</", close);
      if (end == std::string::npos) return "";
      if (endPos) *endPos = end;
      std::string v = xml.substr(close + 1, end - close - 1);
      v.erase(0, v.find_first_not_of(" \t\r\n"));
      v.erase(v.find_last_not_of(" \t\r\n") + 1);
      return v;
    }
    p += lt.size();
  }
  return "";
}

std::string soapRequest(const std::string& host, uint16_t port, const std::string& path, const std::string& service,
                        const std::string& action, const std::string& args) {
  std::string body =
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:" + action + " xmlns:u=\"" + service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
  return "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
         "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action +
         "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

PortForwarder::~PortForwarder() { stop(); }

void PortForwarder::set(State s, const std::string& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  status_.state = s;
  status_.message = msg;
}

PortForwarder::Status PortForwarder::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

void PortForwarder::start(const std::vector<uint16_t>& udpPorts) {
  if (thread_.joinable()) return;
  set(State::Working, "Asking your router to open the game ports...");
  thread_ = std::thread([this, udpPorts] { run(udpPorts); });
}

void PortForwarder::run(std::vector<uint16_t> ports) {
  // 1) Find the router with SSDP (a multicast "who is a gateway?" on the LAN).
  SockHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBad) return set(State::Failed, "Could not open a network socket");
  setTimeout(s, 400);
  sockaddr_in mc{};
  mc.sin_family = AF_INET;
  mc.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
  const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:service:WANIPConnection:1", "upnp:rootdevice"};
  std::vector<std::string> locations;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  int round = 0;
  while (std::chrono::steady_clock::now() < deadline && locations.empty()) {
    if (round++ % 3 == 0)
      for (const char* st : targets) {
        std::string req = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                          st + "\r\n\r\n";
        ::sendto(s, req.data(), static_cast<int>(req.size()), 0, reinterpret_cast<sockaddr*>(&mc), sizeof(mc));
      }
    char buf[2048];
    for (;;) {
      int n = ::recvfrom(s, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
      if (n <= 0) break;
      buf[n] = 0;
      std::string resp(buf), lr = lowerStr(resp);
      size_t p = lr.find("\nlocation:");
      if (p == std::string::npos) continue;
      size_t start = p + 10, end = resp.find("\r\n", start);
      std::string loc = resp.substr(start, end - start);
      loc.erase(0, loc.find_first_not_of(" \t"));
      if (std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
    }
  }
  closeSock(s);
  if (locations.empty())
    return set(State::Failed, "Your router didn't answer (UPnP is off or not supported). Forward the ports by hand.");

  // 2) Read each device description until one has a WAN connection service.
  std::string host, path, service, control, localIp;
  uint16_t port = 0;
  for (const auto& loc : locations) {
    std::string h, p;
    uint16_t pt;
    if (!parseUrl(loc, h, pt, p)) continue;
    std::string resp;
    if (!http(h, pt, "GET " + p + " HTTP/1.1\r\nHost: " + h + ":" + std::to_string(pt) + "\r\nConnection: close\r\n\r\n",
              resp, &localIp))
      continue;
    for (const char* svc : {"WANIPConnection", "WANPPPConnection"}) {
      size_t at = lowerStr(resp).find(lowerStr(svc));
      if (at == std::string::npos) continue;
      size_t typeStart = resp.rfind("<", resp.rfind("serviceType", at));
      std::string type = xmlValue(resp, "serviceType", typeStart == std::string::npos ? 0 : typeStart);
      std::string ctrl = xmlValue(resp, "controlURL", at);
      if (type.empty() || ctrl.empty()) continue;
      host = h;
      port = pt;
      service = type;
      if (ctrl.rfind("http", 0) == 0) parseUrl(ctrl, host, port, control);
      else control = ctrl[0] == '/' ? ctrl : "/" + ctrl;
      break;
    }
    if (!control.empty()) break;
  }
  if (control.empty()) return set(State::Failed, "Found your router but it doesn't allow automatic port forwarding.");

  // 3) Ask for the public IP and open each port.
  std::string resp, externalIp;
  if (http(host, port, soapRequest(host, port, control, service, "GetExternalIPAddress", ""), resp, nullptr))
    externalIp = xmlValue(resp, "NewExternalIPAddress");
  std::vector<uint16_t> opened;
  for (uint16_t p : ports) {
    std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(p) +
                       "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(p) +
                       "</NewInternalPort><NewInternalClient>" + localIp +
                       "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Survival Game " +
                       std::to_string(p) + "</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>";
    if (http(host, port, soapRequest(host, port, control, service, "AddPortMapping", args), resp, nullptr) &&
        httpStatus(resp) == 200)
      opened.push_back(p);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    controlHost_ = host;
    controlPort_ = port;
    controlPath_ = control;
    serviceType_ = service;
    mapped_ = opened;
    status_.publicIp = externalIp;
    status_.localIp = localIp;
    status_.behindCarrierNat = !externalIp.empty() && isPrivate(externalIp);
  }
  if (opened.empty())
    return set(State::Failed, "Your router refused to open the ports. Forward UDP " + std::to_string(ports.front()) +
                                  " by hand to " + localIp + ".");
  if (!externalIp.empty() && isPrivate(externalIp))
    return set(State::Failed, "Ports opened, but your internet provider shares your IP (carrier-grade NAT), so friends "
                              "outside can't reach it directly. Use a free VPN like Tailscale or ZeroTier.");
  std::string list;
  for (uint16_t p : opened) list += (list.empty() ? "" : ", ") + std::to_string(p);
  set(State::Open, "Router ports opened (UDP " + list + ")");
}

void PortForwarder::stop() {
  if (thread_.joinable()) thread_.join();
  std::string host, path, service;
  uint16_t port;
  std::vector<uint16_t> mapped;
  {
    std::lock_guard<std::mutex> lock(mu_);
    host = controlHost_;
    port = controlPort_;
    path = controlPath_;
    service = serviceType_;
    mapped.swap(mapped_);
  }
  for (uint16_t p : mapped) {
    std::string resp;
    std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(p) +
                       "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
    http(host, port, soapRequest(host, port, path, service, "DeletePortMapping", args), resp, nullptr);
  }
}

}  // namespace sg
