#ifndef COURIER_CLIENT_H
#define COURIER_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <cassert>
#include <memory>
#include <type_traits>
#include <utility>
#include "Transport.h"
#include "WebSocketTransport.h"
#include "Endpoint.h"

namespace Courier {

enum class State {
  Booting,
  WifiConnecting,
  WifiConnected,
  WifiConfiguring,
  TransportsConnecting,
  Connected,
  Reconnecting,
  ConnectionFailed,
};

struct Config {
  const char* host;
  uint16_t port;
  const char* path;
  const char* apName;
  const char* defaultTransport;  // e.g. "ws", "mqtt"; null disables Client::send
  uint32_t dns1;
  uint32_t dns2;

  Config(const char* host = nullptr,
         uint16_t port = 443,
         const char* path = "/",
         const char* apName = nullptr,
         const char* defaultTransport = nullptr,
         uint32_t dns1 = 0,
         uint32_t dns2 = 0)
      : host(host), port(port), path(path), apName(apName),
        defaultTransport(defaultTransport), dns1(dns1), dns2(dns2) {}
};

// NOTE: Only one Client instance is supported per process. WiFiManager
// requires a C-style function pointer callback, which necessitates a
// static instance pointer. Creating multiple Client instances will
// produce undefined behavior.
class Client {
public:
  // Callback types
  using Callback = std::function<void()>;
  using MessageCallback = std::function<void(const char* transportName,
                                              const char* type,
                                              JsonDocument& doc)>;
  using ConnectionChangeCallback = std::function<void(State state)>;
  using ErrorCallback = std::function<void(const char* category, const char* message)>;

  Client(const Config& config);
  ~Client();

  void setup();
  void loop();

  // --- State ---
  bool isConnected() const;
  State getState() const { return _state; }
  bool isTimeSynced() const;

  // --- Transports ---
  // Construct a transport in-place, register under `name`, return ref.
  // Asserts if the name is already registered or the registry is full.
  template<typename T, typename... Args>
  T& addTransport(const char* name, Args&&... args) {
    static_assert(std::is_base_of<Transport, T>::value,
                  "T must derive from Courier::Transport");
    T* t = new T(std::forward<Args>(args)...);
    attachTransport(name, t);
    return *t;
  }

  // Typed lookup. Asserts if `name` is not registered. The type T must
  // match the type used at addTransport<T>(); mistyped lookup is UB.
  // (ESP32 builds default to -fno-rtti, so dynamic_cast is unavailable.
  // Caller knows the type they registered.)
  template<typename T>
  T& transport(const char* name) {
    Transport* base = lookupTransport(name);
    assert(base != nullptr && "transport name not registered");
    return *static_cast<T*>(base);
  }

  void removeTransport(const char* name);

  // --- Send (routes via defaultTransport) ---
  // Returns false if no default transport is configured / registered, or
  // if the underlying transport's send() returns false.
  bool send(JsonDocument& doc);
  bool send(JsonDocument& doc, const SendOptions& options);

  // Override the default transport at runtime. Pass nullptr or "" to clear.
  void setDefaultTransport(const char* name);

  // --- Transport lifecycle ---
  void suspend();
  void resume();

  // --- Event callbacks (single-slot, last registration wins) ---
  void onMessage(MessageCallback cb);
  void onConnected(Callback cb);
  void onDisconnected(Callback cb);
  void onConnectionChange(ConnectionChangeCallback cb);
  void onError(ErrorCallback cb);

  // --- Lifecycle hooks (single-slot, last registration wins) ---
  void onTransportsWillConnect(Callback cb);
  void onTransportsDidConnect(Callback cb);

  // --- WiFi configuration ---
  void setAPName(const char* name);
  // Raw WiFiManager access — called before autoConnect. Use for custom
  // parameters, timeouts, hostname, callbacks, etc.
  using WiFiConfigureCallback = std::function<void(WiFiManager&)>;
  void onConfigureWiFi(WiFiConfigureCallback cb);

private:
  Config _config;
  State _state;

  // Transport map (fixed-size array, max 4 transports).
  // Client owns registered transports via unique_ptr.
  static constexpr int MAX_TRANSPORTS = 4;
  struct TransportEntry {
    const char* name = nullptr;
    std::unique_ptr<Transport> transport;
    Endpoint endpoint;
    bool failed = false;
  };
  TransportEntry _transports[MAX_TRANSPORTS];
  int _transportCount = 0;

  // Internal registry helpers
  void attachTransport(const char* name, Transport* t);   // takes ownership
  Transport* lookupTransport(const char* name);
  Transport* lookupDefaultTransport();

  // Default-transport routing for Client::send. Empty string means
  // "fall through to _config.defaultTransport".
  String _defaultTransport;

  // WiFi
  WiFiManager _wm;
  WiFiConfigureCallback _wifiConfigureCallback;
  String _apName;

  // State machine handlers
  void handleWifiConnectingState();
  void handleWifiConfiguringState();
  void handleWifiConnectedState();
  void handleTransportsConnectingState();
  void handleConnectedState();
  void handleReconnectingState();
  void handleConnectionFailedState();

  // WiFi helpers
  void setupWiFi();
  void launchWiFiConfigPortal();
  static void staticWifiFailedCallback(WiFiManager* wm);

  // Time sync
  bool syncTimeFromHttpDate();
  bool _timeSyncAttempted = false;

  // JSON dispatch — wired via Transport::setClientHook in attachTransport.
  void dispatchJSON(const char* transportName, const char* payload, size_t length);
  void handleTransportConnection(Transport* transport, bool connected);

  // Health monitoring
  struct HealthState {
    unsigned long lastWiFiCheckMillis = 0;
    unsigned int consecutiveWiFiFailures = 0;
    unsigned long lastErrorLogMillis = 0;
  } _health;

  // Reconnection backoff
  struct ReconnectState {
    unsigned int attempts = 0;
    unsigned long currentInterval = 0;
    unsigned long lastAttemptMillis = 0;
    bool disconnectedCallbacksFired = false;
  } _reconnect;
  unsigned long calculateBackoffInterval(unsigned int attempts);

  // Callback storage (all single-slot)
  MessageCallback _messageCallback;
  Callback _connectedCallback;
  Callback _disconnectedCallback;
  ConnectionChangeCallback _connectionChangeCallback;
  ErrorCallback _errorCallback;

  // Lifecycle hooks (single-slot)
  Callback _willConnectHook;
  Callback _didConnectHook;

  // Health monitoring constants
  static constexpr unsigned long WIFI_CHECK_INTERVAL = 5000;
  static constexpr uint8_t MAX_WIFI_FAILURES = 3;

  // Transport connection timeout
  static constexpr unsigned long TRANSPORT_CONNECTION_TIMEOUT = 30000;

  // Reconnection constants
  static constexpr unsigned long MIN_RECONNECT_INTERVAL = 5000;
  static constexpr unsigned long MAX_RECONNECT_INTERVAL = 60000;
  static constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 10;

  // Singleton for WiFiManager static callback
  static Client* _instance;

  // State transition — always use this instead of setting _state directly
  void transitionTo(State newState);

  // Fire callbacks helpers
  void fireConnectedCallbacks();
  void fireDisconnectedCallbacks();
  void fireConnectionChangeCallbacks();
  void fireErrorCallbacks(const char* category, const char* message);
  void fireWillConnectHooks();
  void fireDidConnectHooks();

  // Transport failure escalation
  void handleTransportFailure(Transport* transport);
  bool allPersistentTransportsFailed() const;
  void clearTransportFailureFlags();
  void teardownAllTransports();

  // Transports connecting state tracking
  unsigned long _transportsConnectingStartMillis = 0;
  bool _transportsBeginCalled = false;
};

}  // namespace Courier

#endif // COURIER_CLIENT_H
