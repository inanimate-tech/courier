#ifndef COURIER_LOCK_H
#define COURIER_LOCK_H

#include <cstdint>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <chrono>
#include <mutex>
#endif

// Minimal mutexes for transports that are driven from more than one task —
// a dedicated send task alongside the app task running loop(), with ESP-IDF's
// own client task raising events in between.
//
// These are not a substitute for ESP-IDF's own locking. esp-mqtt (and
// esp_websocket_client) take an internal API lock on every entry point, so
// concurrent calls on a live handle are already serialised. What they do not
// protect is the handle's lifetime: esp_mqtt_client_destroy takes no lock and
// frees the client, including that API lock. Guarding teardown against
// in-flight calls is the job these mutexes exist for.
//
// Deadlock rule for every user of these: ESP-IDF clients take their own
// internal API lock with no timeout, and hold it across reconnects. So a
// Courier lock must never be held while waiting on an IDF lock that a task
// holding an IDF lock could be waiting on. In practice: never nest these two
// mutexes, and never call into ESP-IDF while holding a data-only lock.

namespace Courier {

// Data-only lock: short critical sections that never call into ESP-IDF.
class Mutex {
public:
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

#ifdef ESP_PLATFORM
    Mutex() : _handle(xSemaphoreCreateMutex()) {}
    ~Mutex() { if (_handle) vSemaphoreDelete(_handle); }
    void lock()   { if (_handle) xSemaphoreTake(_handle, portMAX_DELAY); }
    void unlock() { if (_handle) xSemaphoreGive(_handle); }

private:
    SemaphoreHandle_t _handle;
#else
    Mutex() = default;
    void lock()   { _mutex.lock(); }
    void unlock() { _mutex.unlock(); }

private:
    std::mutex _mutex;
#endif
};

// Bounded-wait lock: guards calls into an ESP-IDF client. tryLockFor() gives
// a caller a fast "busy, try again" instead of an unbounded block when another
// task is already inside the client.
class TimedMutex {
public:
    TimedMutex(const TimedMutex&) = delete;
    TimedMutex& operator=(const TimedMutex&) = delete;

#ifdef ESP_PLATFORM
    TimedMutex() : _handle(xSemaphoreCreateMutex()) {}
    ~TimedMutex() { if (_handle) vSemaphoreDelete(_handle); }
    void lock()   { if (_handle) xSemaphoreTake(_handle, portMAX_DELAY); }
    void unlock() { if (_handle) xSemaphoreGive(_handle); }
    bool tryLockFor(uint32_t ms) {
        if (!_handle) return true;
        return xSemaphoreTake(_handle, pdMS_TO_TICKS(ms)) == pdTRUE;
    }

private:
    SemaphoreHandle_t _handle;
#else
    TimedMutex() = default;
    void lock()   { _mutex.lock(); }
    void unlock() { _mutex.unlock(); }
    bool tryLockFor(uint32_t ms) {
        return _mutex.try_lock_for(std::chrono::milliseconds(ms));
    }

private:
    std::timed_mutex _mutex;
#endif
};

template <typename M>
class LockGuard {
public:
    explicit LockGuard(M& m) : _m(m) { _m.lock(); }
    ~LockGuard() { _m.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    M& _m;
};

}  // namespace Courier

#endif  // COURIER_LOCK_H
