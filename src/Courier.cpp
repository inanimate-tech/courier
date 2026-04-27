#include "Courier.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ezTime.h>
#ifdef ESP_PLATFORM
#include <esp_netif.h>
#endif

namespace Courier {

// Static member initialization
Client* Client::_instance = nullptr;

Client::Client(const Config& config)
    : _config(config),
      _state(COURIER_BOOTING),
      _defaultTransport(config.defaultTransport ? config.defaultTransport : "ws"),
      _defaultTopic(config.defaultTopic ? config.defaultTopic : ""),
      _health{},
      _reconnect{}
{
  _instance = this;

  // Pre-register the built-in WS transport as "ws"
  _transports[0].name = "ws";
  _transports[0].transport = &_builtinWS;
  _transportCount = 1;

  // Wire callbacks on built-in WS transport
  _builtinWS.setMessageCallback([this](const char* p, size_t l) {
    handleTransportMessage(p, l);
  });
  _builtinWS.setConnectionCallback([this](Transport* t, bool c) {
    handleTransportConnection(t, c);
  });
  _builtinWS.setFailureCallback([this]() {
    handleTransportFailure(&_builtinWS);
  });
}

Client::~Client()
{
}

void Client::setup()
{
  // Setup WiFi mode and DNS
  setupWiFi();

  // Set AP name from config if provided and not already set
  if (_apName.isEmpty() && _config.apName) {
    _apName = _config.apName;
  }

  // Generate default AP name from MAC address if still empty
  if (_apName.isEmpty()) {
    #ifdef ESP_PLATFORM
    uint64_t mac = ESP.getEfuseMac();
    char buf[16];
    snprintf(buf, sizeof(buf), "Courier-%04X", (uint16_t)(mac & 0xFFFF));
    _apName = buf;
    #else
    _apName = "Courier";
    #endif
  }

  // Initialize health monitoring timestamps
  unsigned long now = millis();
  _health.lastWiFiCheckMillis = now;
  _reconnect.lastAttemptMillis = now;

  transitionTo(COURIER_WIFI_CONNECTING);
}

void Client::loop()
{
  // ezTime NTP maintenance — primary time source. Without continuous re-sync,
  // the ESP32 RTC drifts enough after 2-3 days to break TLS cert validation.
  // HTTP Date header (in syncTimeFromHttpDate) is the fallback for first boot
  // when NTP hasn't resolved yet.
  events();

  switch (_state)
  {
  case COURIER_BOOTING:
    // No-op — waiting for setup() to transition to WIFI_CONNECTING
    break;
  case COURIER_WIFI_CONNECTING:
    handleWifiConnectingState();
    break;
  case COURIER_WIFI_CONFIGURING:
    handleWifiConfiguringState();
    break;
  case COURIER_WIFI_CONNECTED:
    handleWifiConnectedState();
    break;
  case COURIER_TRANSPORTS_CONNECTING:
    handleTransportsConnectingState();
    break;
  case COURIER_CONNECTED:
    handleConnectedState();
    break;
  case COURIER_RECONNECTING:
    handleReconnectingState();
    break;
  case COURIER_CONNECTION_FAILED:
    handleConnectionFailedState();
    break;
  }
}

// --- State handlers ---

void Client::handleWifiConnectingState()
{
  String configuredSSID = _wm.getWiFiSSID();
  if (configuredSSID.length())
  {
    _wm.setAPCallback(staticWifiFailedCallback);
    _wm.setConnectTimeout(20);
    // Let users configure WiFiManager before autoConnect
    if (_wifiConfigureCallback) _wifiConfigureCallback(_wm);
    int res = _wm.autoConnect(_apName.c_str());
    if (res)
    {
      Serial.println("[courier] WiFi connected!");
      transitionTo(COURIER_WIFI_CONNECTED);
    }
    else
    {
      Serial.println("[courier] WiFi connection failed.");
      fireErrorCallbacks("WIFI", "autoConnect failed");
    }
  }
  else
  {
    launchWiFiConfigPortal();
  }
}

void Client::handleWifiConfiguringState()
{
  _wm.process();
  if (!_wm.getConfigPortalActive())
  {
    Serial.println("[courier] Config portal closed. Connecting...");
    transitionTo(COURIER_WIFI_CONNECTING);
  }
}

void Client::handleWifiConnectedState()
{
  // Configure custom DNS servers if provided (before any HTTPS calls).
  // Uses esp_netif API to set DNS without switching to static IP mode.
  // Sets MAIN + BACKUP for immediate use, and FALLBACK which survives DHCP renewals.
#ifdef ESP_PLATFORM
  if (_config.dns1 != 0) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns;
    dns.ip.type = IPADDR_TYPE_V4;

    dns.ip.u_addr.ip4.addr = _config.dns1;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns);
    Serial.printf("[courier] DNS: %s", IPAddress(_config.dns1).toString().c_str());

    if (_config.dns2 != 0) {
      dns.ip.u_addr.ip4.addr = _config.dns2;
      esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
      Serial.printf(", %s", IPAddress(_config.dns2).toString().c_str());
    }
    Serial.println();
  }
#endif

  // Attempt time synchronization once
  if (!_timeSyncAttempted)
  {
    Serial.println("[courier] Fetching time from HTTPS Date header...");
    if (syncTimeFromHttpDate()) {
      Serial.println("[courier] Time synced via HTTP Date header!");
    } else {
      Serial.println("[courier] HTTP time sync failed - time may be unavailable");
      fireErrorCallbacks("TIME_SYNC", "HTTP Date header not available");
    }
    _timeSyncAttempted = true;
  }

  // Fire onTransportsWillConnect hooks (e.g. registration)
  fireWillConnectHooks();

  // Transition to TRANSPORTS_CONNECTING
  transitionTo(COURIER_TRANSPORTS_CONNECTING);
  _transportsConnectingStartMillis = millis();
  _transportsBeginCalled = false;
}

void Client::handleTransportsConnectingState()
{
  // Begin all transports once (on first entry to this state).
  // begin() starts an async TLS handshake — don't call it again
  // on subsequent loop iterations while waiting for connection.
  if (!_transportsBeginCalled) {
    clearTransportFailureFlags();
    _transportsBeginCalled = true;
    for (int i = 0; i < _transportCount; i++) {
      TransportEntry& entry = _transports[i];
      if (!entry.transport || entry.transport->isConnected()) continue;

      const char* host = (entry.endpoint.host && entry.endpoint.host[0])
          ? entry.endpoint.host : _config.host;
      uint16_t port = entry.endpoint.port == 0
          ? _config.port : entry.endpoint.port;
      const char* path = (entry.endpoint.path && entry.endpoint.path[0])
          ? entry.endpoint.path : _config.path;

      entry.transport->begin(host, port, path);
    }
  }

  // Timeout check - if no transport connects within 30 seconds, retry
  if (millis() - _transportsConnectingStartMillis > TRANSPORT_CONNECTION_TIMEOUT)
  {
    Serial.println("[courier] No transport connected within 30s - entering reconnection state");
    _reconnect.disconnectedCallbacksFired = true;
    fireDisconnectedCallbacks();
    transitionTo(COURIER_RECONNECTING);
    return;
  }

  // Transition to CONNECTED when any transport is connected
  if (isConnected())
  {
    Serial.println("[courier] Transport connected - entering CONNECTED state");

    // Fire onTransportsDidConnect hooks
    fireDidConnectHooks();

    transitionTo(COURIER_CONNECTED);

    // Fire connected callbacks
    fireConnectedCallbacks();
  }

  // Process transport events while waiting for connection
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->loop();
    }
  }
}

void Client::handleConnectedState()
{
  unsigned long now = millis();

  // WiFi health monitoring - check every 5 seconds
  if (now - _health.lastWiFiCheckMillis >= WIFI_CHECK_INTERVAL)
  {
    _health.lastWiFiCheckMillis = now;

    if (WiFi.status() != WL_CONNECTED)
    {
      _health.consecutiveWiFiFailures++;
      Serial.printf("[courier] WiFi check failed (%d/%d)\n",
                    _health.consecutiveWiFiFailures, MAX_WIFI_FAILURES);

      if (_health.consecutiveWiFiFailures >= MAX_WIFI_FAILURES)
      {
        Serial.println("[courier] WiFi lost - entering reconnection state");
        fireErrorCallbacks("WIFI", "connection lost");
        _reconnect.disconnectedCallbacksFired = true;

        teardownAllTransports();
        _health.consecutiveWiFiFailures = 0;
        fireDisconnectedCallbacks();
        transitionTo(COURIER_RECONNECTING);
        return;
      }
    }
    else
    {
      if (_health.consecutiveWiFiFailures > 0)
      {
        Serial.println("[courier] WiFi recovered");
        _health.consecutiveWiFiFailures = 0;
      }
    }
  }

  // Run transport loops
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->loop();
    }
  }
}

void Client::handleReconnectingState()
{
  // Fire disconnected callbacks once on entry (safety net — all callers
  // should set this flag and fire callbacks before transitioning)
  if (!_reconnect.disconnectedCallbacksFired) {
    _reconnect.disconnectedCallbacksFired = true;
    fireDisconnectedCallbacks();
  }

  unsigned long now = millis();

  // Check if backoff interval has elapsed
  if (now - _reconnect.lastAttemptMillis < _reconnect.currentInterval)
  {
    return;
  }

  _reconnect.lastAttemptMillis = now;
  _reconnect.attempts++;

  // Check if max reconnection attempts reached
  if (_reconnect.attempts > MAX_RECONNECT_ATTEMPTS)
  {
    Serial.printf("[courier] Max reconnection attempts (%d) reached - entering failed state\n",
                  MAX_RECONNECT_ATTEMPTS);
    fireErrorCallbacks("RECONNECT", "max attempts exceeded");
    _reconnect.attempts = 0;
    transitionTo(COURIER_CONNECTION_FAILED);
    return;
  }

  // Calculate next backoff interval
  _reconnect.currentInterval = calculateBackoffInterval(_reconnect.attempts);
  Serial.printf("[courier] Reconnect attempt %d/%d (next backoff: %lums)\n",
                _reconnect.attempts, MAX_RECONNECT_ATTEMPTS, _reconnect.currentInterval);

  // Check WiFi status first
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[courier] WiFi lost - resetting to WiFi connection state");
    WiFi.disconnect();
    transitionTo(COURIER_WIFI_CONNECTING);
    _timeSyncAttempted = false;
    _reconnect.attempts = 0;
    _reconnect.currentInterval = MIN_RECONNECT_INTERVAL;
    return;
  }

  // WiFi is good, retry transports - go back through WIFI_CONNECTED to re-run hooks
  Serial.println("[courier] WiFi OK - transitioning to WIFI_CONNECTED");
  teardownAllTransports();
  transitionTo(COURIER_WIFI_CONNECTED);
}

void Client::handleConnectionFailedState()
{
  // Terminal state - log periodically
  unsigned long now = millis();
  if (now - _health.lastErrorLogMillis >= 60000)
  {
    _health.lastErrorLogMillis = now;
    Serial.println("[courier] Connection failed after maximum attempts - manual reboot required");
  }
}

// --- WiFi helpers ---

void Client::setupWiFi()
{
  WiFi.mode(WIFI_STA);
  _wm.setConfigPortalBlocking(false);
}

void Client::launchWiFiConfigPortal()
{
  _wm.startConfigPortal(_apName.c_str());
  transitionTo(COURIER_WIFI_CONFIGURING);
}

void Client::staticWifiFailedCallback(WiFiManager* wm)
{
  if (_instance)
  {
    Serial.println("[courier] WiFi connection failed, launching config portal.");
    _instance->transitionTo(COURIER_WIFI_CONFIGURING);
  }
}

// --- Time sync ---
// Fallback for first boot when NTP hasn't resolved yet. Parses the Date
// header from an HTTPS response to the configured host. NTP (via ezTime
// events() in loop()) is the primary time source for ongoing accuracy.

bool Client::syncTimeFromHttpDate()
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, String("https://") + _config.host + "/");

  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);

  int httpCode = http.sendRequest("HEAD");

  if (httpCode != 200 && httpCode != 204 && httpCode != 301 && httpCode != 302) {
    Serial.printf("[courier] HTTP time request failed: %d\n", httpCode);
    http.end();
    return false;
  }

  String dateHeader = http.header("Date");
  http.end();

  if (dateHeader.isEmpty()) {
    Serial.println("[courier] No Date header in response");
    return false;
  }

  Serial.printf("[courier] Date header: %s\n", dateHeader.c_str());

  int day, year, hour, minute, second;
  char monthStr[4];

  int parsed = sscanf(dateHeader.c_str(), "%*[^,], %d %3s %d %d:%d:%d",
                      &day, monthStr, &year, &hour, &minute, &second);

  if (parsed != 6) {
    Serial.printf("[courier] Failed to parse Date header (parsed %d fields)\n", parsed);
    return false;
  }

  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (strcmp(monthStr, months[i]) == 0) {
      month = i + 1;
      break;
    }
  }

  if (month == 0) {
    Serial.printf("[courier] Unknown month: %s\n", monthStr);
    return false;
  }

  setTime(hour, minute, second, day, month, year);
  Serial.printf("[courier] Time set to: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                year, month, day, hour, minute, second);

  return true;
}

// --- Transport message/connection handlers ---

void Client::handleTransportMessage(const char* payload, size_t length)
{
  // Fire raw message callback
  if (_rawMessageCallback) _rawMessageCallback(payload, length);

  // Parse JSON
  JsonDocument doc;
  if (auto err = deserializeJson(doc, payload, length))
  {
    Serial.print("[courier] Invalid JSON: ");
    Serial.println(err.c_str());
    return;
  }

  const char* mtype = doc["type"] | "";

  // Fire typed message callback
  if (_messageCallback) _messageCallback(mtype, doc);
}

void Client::handleTransportConnection(Transport* transport, bool connected)
{
  if (connected) {
    Serial.printf("[courier] %s connected\n", transport->name());
    _reconnect.attempts = 0;
    _reconnect.currentInterval = MIN_RECONNECT_INTERVAL;
  } else {
    Serial.printf("[courier] %s disconnected\n", transport->name());
  }
}

// --- Transport management ---

void Client::addTransport(const char* name, Transport* transport)
{
  // Check if name already exists (update in place)
  for (int i = 0; i < MAX_TRANSPORTS; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      _transports[i].transport = transport;
      wireTransportCallbacks(transport);
      return;
    }
  }
  // Find first empty slot
  for (int i = 0; i < MAX_TRANSPORTS; i++) {
    if (!_transports[i].name) {
      _transports[i].name = name;
      _transports[i].transport = transport;
      wireTransportCallbacks(transport);
      if (i >= _transportCount) _transportCount = i + 1;
      return;
    }
  }
}

Transport* Client::getTransport(const char* name)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      return _transports[i].transport;
    }
  }
  return nullptr;
}

void Client::removeTransport(const char* name)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      _transports[i].name = nullptr;
      _transports[i].transport = nullptr;
      _transports[i].endpoint = Endpoint{};
      _transports[i].failed = false;
      return;
    }
  }
}

void Client::setEndpoint(const char* transportName, const Endpoint& endpoint)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, transportName) == 0) {
      _transports[i].endpoint = endpoint;
      return;
    }
  }
}

void Client::suspendTransports()
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->suspend();
    }
  }
}

void Client::resumeTransports()
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->resume();
    }
  }
}

// --- Sending ---

bool Client::send(const char* payload)
{
  Transport* t = getTransport(_defaultTransport.c_str());
  if (!t || !t->isConnected()) return false;
  if (t->topicRequired() && !_defaultTopic.isEmpty()) {
    return t->publish(_defaultTopic.c_str(), payload);
  }
  return t->send(payload);
}

bool Client::sendTo(const char* transportName, const char* payload)
{
  Transport* t = getTransport(transportName);
  if (!t) return false;
  return t->send(payload);
}

bool Client::sendBinaryTo(const char* transportName, const uint8_t* data, size_t len)
{
  Transport* t = getTransport(transportName);
  if (!t || !t->isConnected()) return false;
  return t->sendBinary(data, len);
}

bool Client::publishTo(const char* transportName, const char* topic, const char* payload)
{
  Transport* t = getTransport(transportName);
  if (!t) return false;
  return t->publish(topic, payload);
}

void Client::setDefaultTransport(const char* name)
{
  _defaultTransport = name ? name : "ws";
}

void Client::setDefaultTopic(const char* topic)
{
  _defaultTopic = topic ? topic : "";
}

// --- State queries ---

bool Client::isConnected() const
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport && _transports[i].transport->isConnected()) {
      return true;
    }
  }
  return false;
}

bool Client::isTimeSynced() const
{
  return timeStatus() == timeSet;
}

// --- AP name ---

void Client::setAPName(const char* name)
{
  _apName = name;
}

// --- Callback registration ---

void Client::onMessage(MessageCallback cb)
{
  _messageCallback = cb;
}

void Client::onRawMessage(RawMessageCallback cb)
{
  _rawMessageCallback = cb;
}

void Client::onConnected(Callback cb)
{
  _connectedCallback = cb;
}

void Client::onDisconnected(Callback cb)
{
  _disconnectedCallback = cb;
}

void Client::onConnectionChange(ConnectionChangeCallback cb)
{
  _connectionChangeCallback = cb;
}

void Client::onError(ErrorCallback cb)
{
  _errorCallback = cb;
}

void Client::onTransportsWillConnect(Callback cb)
{
  _willConnectHook = cb;
}

void Client::onTransportsDidConnect(Callback cb)
{
  _didConnectHook = cb;
}

void Client::onConfigureWiFi(WiFiConfigureCallback cb)
{
  _wifiConfigureCallback = cb;
}

// --- Fire callback helpers ---

void Client::fireConnectedCallbacks()
{
  if (_connectedCallback) _connectedCallback();
}

void Client::fireDisconnectedCallbacks()
{
  if (_disconnectedCallback) _disconnectedCallback();
}

void Client::transitionTo(State newState)
{
  _state = newState;
  fireConnectionChangeCallbacks();
}

void Client::fireConnectionChangeCallbacks()
{
  if (_connectionChangeCallback) _connectionChangeCallback(_state);
}

void Client::fireWillConnectHooks()
{
  if (_willConnectHook) _willConnectHook();
}

void Client::fireDidConnectHooks()
{
  if (_didConnectHook) _didConnectHook();
}

void Client::fireErrorCallbacks(const char* category, const char* message)
{
  if (_errorCallback) _errorCallback(category, message);
}

void Client::wireTransportCallbacks(Transport* transport)
{
  transport->setMessageCallback([this](const char* p, size_t l) {
    handleTransportMessage(p, l);
  });
  transport->setConnectionCallback([this](Transport* t, bool c) {
    handleTransportConnection(t, c);
  });
  transport->setFailureCallback([this, transport]() {
    handleTransportFailure(transport);
  });
}

// --- Transport failure escalation ---

void Client::handleTransportFailure(Transport* transport)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport == transport) {
      _transports[i].failed = true;
      Serial.printf("[courier] Transport '%s' reported failure\n", _transports[i].name);
      break;
    }
  }

  if (_state == COURIER_CONNECTED && allPersistentTransportsFailed()) {
    Serial.println("[courier] All persistent transports failed — escalating");
    fireErrorCallbacks("TRANSPORT", "all persistent transports failed");
    _reconnect.disconnectedCallbacksFired = true;
    teardownAllTransports();
    fireDisconnectedCallbacks();
    transitionTo(COURIER_RECONNECTING);
  }
}

bool Client::allPersistentTransportsFailed() const
{
  bool anyPersistent = false;
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport && _transports[i].transport->isPersistent()) {
      anyPersistent = true;
      if (!_transports[i].failed) return false;
    }
  }
  return anyPersistent;
}

void Client::clearTransportFailureFlags()
{
  for (int i = 0; i < _transportCount; i++) {
    _transports[i].failed = false;
  }
}

void Client::teardownAllTransports()
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->disconnect();
    }
  }
  clearTransportFailureFlags();
}

// --- Backoff ---

unsigned long Client::calculateBackoffInterval(unsigned int attempts)
{
  unsigned long multiplier = 1UL << min((int)attempts, 8);
  unsigned long interval = MIN_RECONNECT_INTERVAL * multiplier;
  interval = min(interval, MAX_RECONNECT_INTERVAL);

  long jitterRange = interval / 5;
  long jitter = random(-jitterRange, jitterRange + 1);
  interval += jitter;

  interval = max(interval, MIN_RECONNECT_INTERVAL);

  return interval;
}

}  // namespace Courier
