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
      _state(State::Booting),
      _defaultTransport(config.defaultTransport ? config.defaultTransport : ""),
      _health{},
      _reconnect{}
{
  _instance = this;

  // Auto-register a built-in WebSocketTransport as "ws" when the Config
  // provides a host. Users with no need for the built-in (MQTT-only,
  // UDP-only, or fully-custom transport setups) leave host null and
  // register their own transports explicitly.
  if (_config.host && _config.host[0] != '\0') {
    addTransport<WebSocketTransport>("ws");
  }
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

  transitionTo(State::WifiConnecting);
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
  case State::Booting:
    // No-op — waiting for setup() to transition to WifiConnecting
    break;
  case State::WifiConnecting:
    handleWifiConnectingState();
    break;
  case State::WifiConfiguring:
    handleWifiConfiguringState();
    break;
  case State::WifiConnected:
    handleWifiConnectedState();
    break;
  case State::TransportsConnecting:
    handleTransportsConnectingState();
    break;
  case State::Connected:
    handleConnectedState();
    break;
  case State::Reconnecting:
    handleReconnectingState();
    break;
  case State::ConnectionFailed:
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
      transitionTo(State::WifiConnected);
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
    transitionTo(State::WifiConnecting);
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
  transitionTo(State::TransportsConnecting);
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

      // Endpoint values were seeded by attachTransport() and may have been
      // overridden by the user via transport.setEndpoint(...). Just begin().
      entry.transport->begin();
    }
  }

  // Timeout check - if no transport connects within 30 seconds, retry
  if (millis() - _transportsConnectingStartMillis > TRANSPORT_CONNECTION_TIMEOUT)
  {
    Serial.println("[courier] No transport connected within 30s - entering reconnection state");
    _reconnect.disconnectedCallbacksFired = true;
    fireDisconnectedCallbacks();
    transitionTo(State::Reconnecting);
    return;
  }

  // Transition to Connected when any transport is connected
  if (isConnected())
  {
    Serial.println("[courier] Transport connected - entering CONNECTED state");

    // Fire onTransportsDidConnect hooks
    fireDidConnectHooks();

    transitionTo(State::Connected);

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
        transitionTo(State::Reconnecting);
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
    transitionTo(State::ConnectionFailed);
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
    transitionTo(State::WifiConnecting);
    _timeSyncAttempted = false;
    _reconnect.attempts = 0;
    _reconnect.currentInterval = MIN_RECONNECT_INTERVAL;
    return;
  }

  // WiFi is good, retry transports - go back through WIFI_CONNECTED to re-run hooks
  Serial.println("[courier] WiFi OK - transitioning to WIFI_CONNECTED");
  teardownAllTransports();
  transitionTo(State::WifiConnected);
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
  transitionTo(State::WifiConfiguring);
}

void Client::staticWifiFailedCallback(WiFiManager* wm)
{
  if (_instance)
  {
    Serial.println("[courier] WiFi connection failed, launching config portal.");
    _instance->transitionTo(State::WifiConfiguring);
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

void Client::dispatchJSON(const char* transportName, const char* payload, size_t length)
{
  if (!_messageCallback) return;
  JsonDocument doc;
  if (auto err = deserializeJson(doc, payload, length)) {
    // Not JSON — silently drop. Per-transport hooks still saw the raw bytes.
    return;
  }
  const char* mtype = doc["type"] | "";
  _messageCallback(transportName, mtype, doc);
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

void Client::attachTransport(const char* name, Transport* transport)
{
  // Find first empty slot. Asserts on full registry or duplicate name.
  for (int i = 0; i < MAX_TRANSPORTS; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      assert(false && "transport name already registered");
      delete transport;
      return;
    }
  }
  for (int i = 0; i < MAX_TRANSPORTS; i++) {
    if (!_transports[i].name) {
      _transports[i].name = name;
      _transports[i].transport.reset(transport);
      if (i >= _transportCount) _transportCount = i + 1;

      // Seed endpoint from Config so the simple "static path in Config" case
      // works without any setEndpoint call from the user. The user can still
      // override via transport.setEndpoint(...) before begin() runs.
      transport->setEndpoint(_config.host, _config.port, _config.path);

      // Wire JSON dispatch via the internal hook slot. The user-facing
      // _onMessage slot stays free for per-transport hooks (Phase 8).
      transport->setClientHook([this, name](const char* p, size_t l) {
        dispatchJSON(name, p, l);
      });
      transport->setConnectionCallback([this](Transport* t, bool c) {
        handleTransportConnection(t, c);
      });
      transport->setFailureCallback([this, transport]() {
        handleTransportFailure(transport);
      });
      return;
    }
  }
  assert(false && "transport registry full");
  delete transport;
}

Transport* Client::lookupTransport(const char* name)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      return _transports[i].transport.get();
    }
  }
  return nullptr;
}

Transport* Client::lookupDefaultTransport()
{
  const char* name = _defaultTransport.length() > 0
      ? _defaultTransport.c_str()
      : _config.defaultTransport;
  if (!name || !name[0]) return nullptr;
  return lookupTransport(name);
}

bool Client::send(JsonDocument& doc)
{
  return send(doc, SendOptions{});
}

bool Client::send(JsonDocument& doc, const SendOptions& options)
{
  Transport* t = lookupDefaultTransport();
  if (!t) return false;
  return t->send(doc, options);
}

void Client::setDefaultTransport(const char* name)
{
  _defaultTransport = name ? name : "";
}

void Client::removeTransport(const char* name)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].name && strcmp(_transports[i].name, name) == 0) {
      _transports[i].name = nullptr;
      _transports[i].transport.reset();
      _transports[i].failed = false;
      return;
    }
  }
}

void Client::suspend()
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->suspend();
    }
  }
}

void Client::resume()
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport) {
      _transports[i].transport->resume();
    }
  }
}

void Client::reconnect()
{
  Serial.println("[courier] Manual reconnect requested");
  teardownAllTransports();
  fireErrorCallbacks("RECONNECT", "manual reconnect requested");
  _reconnect.disconnectedCallbacksFired = true;
  fireDisconnectedCallbacks();
  transitionTo(State::Reconnecting);
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

// --- Transport failure escalation ---

void Client::handleTransportFailure(Transport* transport)
{
  for (int i = 0; i < _transportCount; i++) {
    if (_transports[i].transport.get() == transport) {
      _transports[i].failed = true;
      Serial.printf("[courier] Transport '%s' reported failure\n", _transports[i].name);
      break;
    }
  }

  if (_state == State::Connected && allPersistentTransportsFailed()) {
    Serial.println("[courier] All persistent transports failed — escalating");
    fireErrorCallbacks("TRANSPORT", "all persistent transports failed");
    _reconnect.disconnectedCallbacksFired = true;
    teardownAllTransports();
    fireDisconnectedCallbacks();
    transitionTo(State::Reconnecting);
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
