#include "TelemetryChannel.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

#pragma pack(push, 1)
struct TelemetryPacket {
  uint64_t sequence_id;
  uint32_t queue_depth;
  uint64_t process_time_ns;
};
#pragma pack(pop)

int g_sock = -1;
sockaddr_in g_dest{};
bool g_enabled = false;

bool ResolveHost(const char *host, uint16_t port, sockaddr_in *out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo *result = nullptr;
  const std::string port_str = std::to_string(port);
  if (getaddrinfo(host, port_str.c_str(), &hints, &result) != 0 || !result)
    return false;

  std::memset(out, 0, sizeof(*out));
  std::memcpy(out, result->ai_addr, sizeof(sockaddr_in));
  out->sin_port = htons(port);
  freeaddrinfo(result);
  return true;
}

} // namespace

namespace TelemetryChannel {

void InitFromEnvironment() {
  if (g_enabled)
    return;

  const char *host = std::getenv("HFT_TELEMETRY_HOST");
  const char *port_str = std::getenv("HFT_TELEMETRY_PORT");
  if (!host || !port_str || !*host || !*port_str)
    return;

  const int port = std::atoi(port_str);
  if (port <= 0 || port > 65535)
    return;

  g_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (g_sock < 0)
    return;

  if (!ResolveHost(host, static_cast<uint16_t>(port), &g_dest)) {
    std::cerr << "[Wrapper] Telemetry disabled: could not resolve "
              << host << "\n";
    close(g_sock);
    g_sock = -1;
    return;
  }

  g_enabled = true;
  char addr_buf[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &g_dest.sin_addr, addr_buf, sizeof(addr_buf));
  std::cout << "[Wrapper] Telemetry UDP -> " << addr_buf << ":" << port << "\n";
}

void SendSample(uint64_t sequence_id, uint32_t queue_depth,
                uint64_t process_time_ns) {
  if (!g_enabled || g_sock < 0)
    return;

  const TelemetryPacket packet{sequence_id, queue_depth, process_time_ns};
  sendto(g_sock, &packet, sizeof(packet), MSG_DONTWAIT,
         reinterpret_cast<sockaddr *>(&g_dest), sizeof(g_dest));
}

} // namespace TelemetryChannel
