// Small UTF-8 boundary helpers for fixed Cardputer display buffers.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool haUtf8Continuation(uint8_t byte) {
    return (byte & 0xC0U) == 0x80U;
}

// Return the complete, canonical code-point width at `text`, or zero when the
// available suffix is invalid/incomplete. Inputs are already validated at their
// protocol/content boundary; the zero case normally means snprintf cut a glyph.
static inline size_t haUtf8CodePointBytes(const uint8_t* text, size_t available) {
    if(!text || !available) return 0;
    const uint8_t a = text[0];
    if(a <= 0x7FU) return 1;
    if(a >= 0xC2U && a <= 0xDFU)
        return available >= 2 && haUtf8Continuation(text[1]) ? 2 : 0;
    if(a >= 0xE0U && a <= 0xEFU) {
        if(available < 3 || !haUtf8Continuation(text[1]) ||
           !haUtf8Continuation(text[2]))
            return 0;
        if((a == 0xE0U && text[1] < 0xA0U) ||
           (a == 0xEDU && text[1] >= 0xA0U))
            return 0; // overlong encoding or UTF-16 surrogate
        return 3;
    }
    if(a >= 0xF0U && a <= 0xF4U) {
        if(available < 4 || !haUtf8Continuation(text[1]) ||
           !haUtf8Continuation(text[2]) || !haUtf8Continuation(text[3]))
            return 0;
        if((a == 0xF0U && text[1] < 0x90U) ||
           (a == 0xF4U && text[1] >= 0x90U))
            return 0; // overlong encoding or beyond U+10FFFF
        return 4;
    }
    return 0;
}

// Force a bounded C string to end at its last complete UTF-8 code point. This is
// deliberately allocation-free because it runs in the engine/AsyncTCP path.
static inline void haUtf8SafeTerminate(char* text, size_t capacity) {
    if(!text || !capacity) return;
    text[capacity - 1] = '\0';
    const size_t length = strnlen(text, capacity);
    size_t boundary = 0;
    while(boundary < length) {
        const size_t width = haUtf8CodePointBytes(
            reinterpret_cast<const uint8_t*>(text + boundary),
            length - boundary);
        if(!width) break;
        boundary += width;
    }
    if(boundary < length) text[boundary] = '\0';
}
