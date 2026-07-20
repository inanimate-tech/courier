#include "Courier.h"
#include "HttpTransport.h"
#include "NetUtil.h"
#include <WiFi.h>
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

  // Auto-register the built-in WebSocketTransport as "ws" when the Config
  // provides a host AND the default transport is (or defaults to) "ws".
  // A Config aimed at another default ("https", "mqtt", ...) means the user
  // is bringing their own transport — don't spend RAM on a stray WS
  // connection to the same host.
  bool wsIsDefault = !_config.defaultTransport ||
                     strcmp(_config.defaultTransport, "ws") == 0;
  if (wsIsDefault && _config.host && _config.host[0] != '\0') {
    addTransport<WebSocketTransport>("ws");
  } else if (_config.host && _config.host[0] != '\0') {
    Serial.printf("[courier] host set but defaultTransport is \"%s\" - not "
                  "auto-registering \"ws\"\n", _config.defaultTransport);
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

  // Bridge ezTime -> system clock whenever NTP has synced and diverges from
  // the system clock by more than a few seconds: mbedTLS validates
  // certificate dates against the system clock (settimeofday), which
  // ezTime's own sync never touches. Re-checking (rather than a one-shot
  // latch) means a genuine NTP correction can repair a system clock that was
  // never bridged, poisoned by a bad HTTP Date, or has simply drifted -
  // small ongoing differences are left alone so this doesn't fight ezTime's
  // own continuous drift correction on every loop() call.
  if (timeStatus() == timeSet) {
    time_t nowUtc = UTC.now();
    if (nowUtc > 0) {
      time_t sysClock = getSystemClock();
      time_t divergence = nowUtc > sysClock ? nowUtc - sysClock : sysClock - nowUtc;
      if (divergence > 5) {
        setSystemClock(nowUtc);
      }
    }
  }

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

    // Re-resolve DNS on every connect cycle (initial setup AND each
    // recovery pass routes through here). Anycast DNS shuffles record
    // order, so a fresh resolve is the cheap failover away from a broken
    // cached IP. Harmless when the outage was WiFi-side.
    flushDnsCache();

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
// Fallback for first boot when NTP hasn't resolved yet. Bootstrap problem:
// TLS certificate validation needs a roughly-correct clock, but this IS the
// clock source on cold boot. So: plain HTTP (port 80) first — any response,
// even a redirect, carries a Date header — then HTTPS with the cert bundle
// (succeeds when the RTC is already warm). disable_auto_redirect keeps a 301
// on the http:// leg from being silently followed into TLS with a cold
// clock (which would fail cert validation and discard the very Date header
// this probe exists to capture) — the 301 itself is the response we want.
// The build-epoch floor (and a plausibility ceiling) rejects clock
// manipulation on this unauthenticated leg. Both legs are capped at 5s with
// no retries: NTP (ezTime events() in loop()) is the primary ongoing time
// source, so this probe must not block the state machine for long.

bool Client::syncTimeFromHttpDate()
{
  HttpTransport http;
  http.begin();
  HttpTransport::FetchOptions opts;
  opts.method = "HEAD";
  opts.timeoutMs = 5000;
  opts.retries = 0;
  opts.configure = [](esp_http_client_config_t& c) {
    c.disable_auto_redirect = true;
  };

  char url[192];
  int n = snprintf(url, sizeof(url), "http://%s/", _config.host);
  if (n < 0 || (size_t)n >= sizeof(url)) {
    Serial.println("[courier] time sync: host too long for URL buffer - skipping http:// leg");
    return false;
  }
  Response r = http.fetch(url, opts);
  if (!r.reachedServer() || !r.header("Date")) {
    n = snprintf(url, sizeof(url), "https://%s/", _config.host);
    if (n < 0 || (size_t)n >= sizeof(url)) {
      Serial.println("[courier] time sync: host too long for URL buffer - skipping https:// leg");
      return false;
    }
    r = http.fetch(url, opts);
  }
  if (!r.reachedServer()) {
    Serial.printf("[courier] HTTP time request failed: %d\n", r.status);
    return false;
  }

  const char* dateHeader = r.header("Date");
  if (!dateHeader) {
    Serial.println("[courier] No Date header in response");
    return false;
  }
  Serial.printf("[courier] Date header: %s\n", dateHeader);

  time_t epoch = parseHttpDateToEpoch(dateHeader);
  if (epoch == 0) {
    Serial.println("[courier] Failed to parse Date header");
    return false;
  }
  if (epoch < buildEpoch()) {
    Serial.println("[courier] Date header predates firmware build - rejecting");
    return false;
  }
  time_t ceiling = buildEpoch() + (time_t)(10 * 365 * 86400);  // 10 years
  if (epoch > ceiling) {
    Serial.println("[courier] Date header implausibly far in the future - rejecting");
    return false;
  }

  setSystemClock(epoch);   // mbedTLS reads the system clock for cert dates
  UTC.setTime(epoch);      // ezTime for display/scheduling
  Serial.println("[courier] Time set from HTTP Date header");
  return true;
}

// --- Transport message/connection handlers ---

void Client::dispatchJSON(const char* transportName, const char* payload, size_t length)
{
  if (!_messageCallback) return;
  JsonDocument doc;
  // The payload is a heap-owned scratch buffer freed right after this returns
  // (see Transport::drainPending contract), and the raw per-transport hook has
  // already run. Parsing it as mutable char* puts ArduinoJson in zero-copy
  // mode — strings in `doc` point into the buffer instead of being duplicated,
  // halving the peak footprint of large payloads (matters on no-PSRAM boards).
  if (auto err = deserializeJson(doc, const_cast<char*>(payload), length)) {
    // Not JSON — drop. Per-transport hooks still saw the raw bytes.
    Serial.printf("[courier] %s: dropping non-JSON payload (%u bytes): %s\n",
                  transportName, (unsigned)length, err.c_str());
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
