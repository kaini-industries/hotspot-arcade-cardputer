#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifndef PROGMEM
#define PROGMEM
#endif

#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
static inline size_t strlcpy(char* destination, const char* source, size_t capacity) {
    const size_t length = std::strlen(source);
    if(capacity) {
        const size_t copied = std::min(length, capacity - 1);
        std::memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return length;
}
#endif
