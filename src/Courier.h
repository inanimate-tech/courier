#ifndef COURIER_H
#define COURIER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include "CourierTransport.h"
#include "CourierWSTransport.h"
#include "CourierEndpoint.h"

enum CourierState {
  COURIER_BOOTING,
  COURIER_WIFI_CONNECTING,
  COURIER_WIFI_CONNECTED,
  COURIER_WIFI_CONFIGURING,
  COURIER_TRANSPORTS_CONNECTING,
  COURIER_CONNECTED,
  COURIER_RECONNECTING,
  COURIER_CONNECTION_FAILED
};

struct CourierConfig {
  const char* host;
  uint16_t port = 443;
  const char* path = "/";
  const char* apName = nullptr;          // WiFi AP name for config portal
  const char* defaultTransport = "ws";   // which transport send() uses
  const char* defaultTopic = nullptr;    // topic for send() if transport requires it
  IPAddress dns1;                        // Primary DNS server (0.0.0.0 = use DHCP default)
  IPAddress dns2;                        // Secondary DNS server (0.0.0.0 = none)
};

// NOTE: Only one Courier instance is supported per process. WiFiManager
// requires a C-style function pointer callback, which necessitates a
// static instance pointer. Creating multiple Courier instances will
// produce undefined behavior.
class Courier {
public:
  // Callback types
  using Callback = std::function<void()>;
  using MessageCallback = std::function<void(const char* type, JsonDocument& doc)>;
  using RawMessageCallback = std::function<void(const char* payload, size_t length)>;
  using ConnectionChangeCallback = std::function<void(CourierState state)>;
  using ErrorCallback = std::function<void(const char* category, const char* message)>;

  explicit Courier(const CourierConfig& config);
  ~Courier();

  void setup();
  void loop();

  // --- State ---
  bool isConnected() const;
  CourierState getState() const { return _state; }
  bool isTimeSynced() const;

  // --- Sending ---
  bool send(const char* payload);
  bool sendTo(const char* transportName, const char* payload);
  bool sendBinaryTo(const char* transportName, const uint8_t* data, size_t len);
  bool publishTo(const char* transportName, const char* topic, const char* payload);

  // --- Default transport/topic ---
  void setDefaultTransport(const char* name);
  void setDefaultTopic(const char* topic);

  // --- Transports ---
  void addTransport(const char* name, CourierTransport* transport);
  CourierTransport* getTransport(const char* name);
  void removeTransport(const char* name);
  void setEndpoint(const char* transportName, const CourierEndpoint& endpoint);

  // --- Transport lifecycle ---
  void suspendTransports();
  void resumeTransports();

  // --- Built-in WS transport accessor ---
  CourierWSTransport& builtinWS() { return _builtinWS; }

  // --- Event callbacks (multiple registrations supported) ---
  void onMessage(MessageCallback cb);
  void onRawMessage(RawMessageCallback cb);
  void onConnected(Callback cb);
  void onDisconnected(Callback cb);
  void onConnectionChange(ConnectionChangeCallback cb);
  void onError(ErrorCallback cb);

  // --- Lifecycle hooks (multiple registrations, run in order) ---
  void onTransportsWillConnect(Callback cb);
  void onTransportsDidConnect(Callback cb);

  // --- WiFi configuration ---
  void setAPName(const char* name);
  // Raw WiFiManager access — called before autoConnect. Use for custom
  // parameters, timeouts, hostname, callbacks, etc.
  using WiFiConfigureCallback = std::function<void(WiFiManager&)>;
  void onConfigureWiFi(WiFiConfigureCallback cb);

private:
  CourierConfig _config;
  CourierState _state;

  // Transport map (fixed-size array, max 4 transports)
  static constexpr int MAX_TRANSPORTS = 4;
  struct TransportEntry {
    const char* name = nullptr;
    CourierTransport* transport = nullptr;
    CourierEndpoint endpoint;
  };
  TransportEntry _transports[MAX_TRANSPORTS];
  int _transportCount = 0;

  // Built-in WS transport
  CourierWSTransport _builtinWS;

  // Default transport/topic for send()
  String _defaultTransport = "ws";
  String _defaultTopic;

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

  // Transport message/connection handlers
  void handleTransportMessage(const char* payload, size_t length);
  void handleTransportConnection(CourierTransport* transport, bool connected);

  // Health monitoring
  struct HealthState {
    unsigned long lastWiFiCheckMillis = 0;
    unsigned long lastTransportCheckMillis = 0;
    unsigned int consecutiveWiFiFailures = 0;
    unsigned int consecutiveTransportFailures = 0;
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

  // Callback lists (simple fixed arrays)
  static constexpr int MAX_CALLBACKS = 4;

  MessageCallback _messageCallbacks[MAX_CALLBACKS];
  int _messageCallbackCount = 0;
  RawMessageCallback _rawMessageCallbacks[MAX_CALLBACKS];
  int _rawMessageCallbackCount = 0;
  Callback _connectedCallbacks[MAX_CALLBACKS];
  int _connectedCallbackCount = 0;
  Callback _disconnectedCallbacks[MAX_CALLBACKS];
  int _disconnectedCallbackCount = 0;
  ConnectionChangeCallback _connectionChangeCallbacks[MAX_CALLBACKS];
  int _connectionChangeCallbackCount = 0;
  ErrorCallback _errorCallbacks[MAX_CALLBACKS];
  int _errorCallbackCount = 0;

  // Lifecycle hooks
  Callback _willConnectHooks[MAX_CALLBACKS];
  int _willConnectHookCount = 0;
  Callback _didConnectHooks[MAX_CALLBACKS];
  int _didConnectHookCount = 0;

  // Health monitoring constants
  static constexpr unsigned long WIFI_CHECK_INTERVAL = 5000;
  static constexpr uint8_t MAX_WIFI_FAILURES = 3;
  static constexpr unsigned long TRANSPORT_CHECK_INTERVAL = 5000;
  static constexpr uint8_t MAX_TRANSPORT_FAILURES = 3;

  // Transport connection timeout
  static constexpr unsigned long TRANSPORT_CONNECTION_TIMEOUT = 30000;

  // Reconnection constants
  static constexpr unsigned long MIN_RECONNECT_INTERVAL = 5000;
  static constexpr unsigned long MAX_RECONNECT_INTERVAL = 60000;
  static constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 10;

  // Singleton for WiFiManager static callback
  static Courier* _instance;

  // State transition — always use this instead of setting _state directly
  void transitionTo(CourierState newState);

  // Fire callbacks helpers
  void fireConnectedCallbacks();
  void fireDisconnectedCallbacks();
  void fireConnectionChangeCallbacks();
  void fireErrorCallbacks(const char* category, const char* message);
  void fireWillConnectHooks();
  void fireDidConnectHooks();

  // Wire message/connection callbacks onto a transport
  void wireTransportCallbacks(CourierTransport* transport);

  // Transports connecting state tracking
  unsigned long _transportsConnectingStartMillis = 0;
  bool _transportsBeginCalled = false;
};

#endif // COURIER_H
