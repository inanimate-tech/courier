#ifndef COURIER_NET_UTIL_H
#define COURIER_NET_UTIL_H

#include <ctime>

namespace Courier {

// Flush the lwIP DNS cache so the next resolve re-queries the DNS server.
// Cloudflare-style anycast DNS shuffles record order per query, so a fresh
// resolve is a probabilistic failover away from a broken cached IP (lwIP
// only ever holds ONE address per host — DNS_MAX_HOST_IP is 1).
void flushDnsCache();

// Set the *system* clock via settimeofday(). mbedTLS validates certificate
// validity dates against the system clock — syncing only ezTime leaves TLS
// validation permanently broken (poem-firmware's root-cause finding).
void setSystemClock(time_t epoch);

// Read the *system* clock: time(nullptr) on device, the native test seam
// (systemClockForTests) off-device. Used to detect drift/poisoning between
// the system clock and ezTime's UTC.now() so Client::loop() can re-bridge.
time_t getSystemClock();

// Parse an RFC 7231 HTTP Date header, e.g. "Tue, 18 Feb 2026 12:00:00 GMT".
// Returns the UTC epoch, or 0 on parse failure.
time_t parseHttpDateToEpoch(const char* dateHeader);

// Compile-time epoch floor (from __DATE__/__TIME__). Reject Date-header
// clock-sets earlier than the firmware build — guards against MITM clock
// rollback on the unauthenticated time-bootstrap request.
//
// Caveat: __DATE__/__TIME__ are the build machine's LOCAL wall clock with
// no timezone info, parsed here as if UTC — so buildEpoch() can lead true
// UTC by up to ~14 hours (UTC+14). Comparisons against real UTC times must
// allow kBuildEpochTzSlack, or a genuine current-UTC Date header gets
// rejected on any UTC-ahead build machine.
time_t buildEpoch();

// Timezone slack for buildEpoch() comparisons: covers every UTC offset with
// margin. Loosening a gross-rollback guard by a day is immaterial to what
// it exists to block (clocks rolled back to expired-cert eras).
constexpr time_t kBuildEpochTzSlack = 24 * 3600;

#ifndef ESP_PLATFORM
// Native-test observability for the two side-effecting functions above.
extern int dnsFlushCountForTests;
extern time_t systemClockForTests;
#endif

}  // namespace Courier

#endif  // COURIER_NET_UTIL_H
