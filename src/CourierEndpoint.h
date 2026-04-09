#ifndef COURIER_ENDPOINT_H
#define COURIER_ENDPOINT_H

#include <Arduino.h>

struct CourierEndpoint {
  String host;       // override courier-level host (empty = use default)
  uint16_t port = 0; // override courier-level port (0 = use default)
  String path;       // transport-specific path
};

#endif // COURIER_ENDPOINT_H
