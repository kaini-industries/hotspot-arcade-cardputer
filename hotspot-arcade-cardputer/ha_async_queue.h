// Small bounded multi-producer queue for requests crossing from AsyncTCP callbacks
// into Arduino's loop task. Hardware I/O is performed only by the consumer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#else
#include <mutex>
#endif

template <typename T, size_t Capacity>
class HaAsyncQueue {
    static_assert(Capacity > 0 && Capacity <= UINT16_MAX, "queue capacity must fit uint16_t");

public:
    bool push(const T& value) {
        lock();
        if(_count == Capacity) {
            if(_dropped != UINT32_MAX) _dropped++;
            unlock();
            return false;
        }
        _items[_tail] = value;
        _tail = (uint16_t)((_tail + 1) % Capacity);
        _count++;
        unlock();
        return true;
    }

    bool pop(T& value) {
        lock();
        if(!_count) {
            unlock();
            return false;
        }
        value = _items[_head];
        _head = (uint16_t)((_head + 1) % Capacity);
        _count--;
        unlock();
        return true;
    }

    uint16_t size() const {
        lock();
        uint16_t result = _count;
        unlock();
        return result;
    }

    uint32_t dropped() const {
        lock();
        uint32_t result = _dropped;
        unlock();
        return result;
    }

private:
    void lock() const {
#if defined(ARDUINO_ARCH_ESP32)
        portENTER_CRITICAL(&_mux);
#else
        _mux.lock();
#endif
    }

    void unlock() const {
#if defined(ARDUINO_ARCH_ESP32)
        portEXIT_CRITICAL(&_mux);
#else
        _mux.unlock();
#endif
    }

    T _items[Capacity] = {};
    uint16_t _head = 0;
    uint16_t _tail = 0;
    uint16_t _count = 0;
    uint32_t _dropped = 0;
#if defined(ARDUINO_ARCH_ESP32)
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
#else
    mutable std::mutex _mux;
#endif
};

