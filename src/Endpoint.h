#ifndef COURIER_ENDPOINT_H
#define COURIER_ENDPOINT_H

#include <cstdint>

namespace Courier {

struct Endpoint {
  const char* host;
  uint16_t port;
  const char* path;

  Endpoint(const char* host = nullptr,
           uint16_t port = 0,
           const char* path = nullptr)
      : host(host), port(port), path(path) {}
};

}  // namespace Courier

#endif // COURIER_ENDPOINT_H
