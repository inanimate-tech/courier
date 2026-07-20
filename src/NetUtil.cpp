#include "NetUtil.h"

#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "lwip/dns.h"
#include <sys/time.h>
#endif

namespace Courier {

#ifdef ESP_PLATFORM

void flushDnsCache()
{
    // Declared in lwip/dns.h on all ESP targets (esp-lwip extension).
    dns_clear_cache();
}

void setSystemClock(time_t epoch)
{
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

#else

int dnsFlushCountForTests = 0;
time_t systemClockForTests = 0;

void flushDnsCache() { dnsFlushCountForTests++; }
void setSystemClock(time_t epoch) { systemClockForTests = epoch; }

#endif

static int monthIndex(const char* mon)
{
    static const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, names[i], 3) == 0) return i + 1;
    }
    return 0;
}

// Days-from-civil (Howard Hinnant's algorithm) — UTC epoch without relying
// on timegm(), which isn't portable across the native test platforms.
static time_t civilToEpoch(int y, int m, int d, int hh, int mm, int ss)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + (time_t)hh * 3600 + (time_t)mm * 60 + ss;
}

time_t parseHttpDateToEpoch(const char* dateHeader)
{
    if (!dateHeader || !dateHeader[0]) return 0;

    int day, year, hour, minute, second;
    char mon[4] = {0};
    int parsed = sscanf(dateHeader, "%*[^,], %d %3s %d %d:%d:%d",
                        &day, mon, &year, &hour, &minute, &second);
    if (parsed != 6) return 0;

    int month = monthIndex(mon);
    if (month == 0) return 0;
    if (day < 1 || day > 31 || year < 1970 || year > 2999 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return 0;
    }
    return civilToEpoch(year, month, day, hour, minute, second);
}

time_t buildEpoch()
{
    // __DATE__ is "Feb 18 2026" (day space-padded), __TIME__ is "12:00:00".
    int day, year, hour, minute, second;
    char mon[4] = {0};
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return 0;
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return 0;
    int month = monthIndex(mon);
    if (month == 0) return 0;
    return civilToEpoch(year, month, day, hour, minute, second);
}

}  // namespace Courier
