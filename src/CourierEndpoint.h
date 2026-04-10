#ifndef COURIER_ENDPOINT_H
#define COURIER_ENDPOINT_H

#include <cstdint>

struct CourierEndpoint {
  const char* host;
  uint16_t port;
  const char* path;

  CourierEndpoint(const char* host = nullptr,
                  uint16_t port = 0,
                  const char* path = nullptr)
      : host(host), port(port), path(path) {}
};

#endif // COURIER_ENDPOINT_H
