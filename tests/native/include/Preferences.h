#pragma once

#include <Arduino.h>
#include <map>

class Preferences {
public:
    inline static std::map<std::string, std::map<std::string, std::vector<uint8_t>>> storage;
    inline static bool failWrites = false;
    inline static int writesRemaining = -1;

    bool begin(const char* name, bool readOnly = false) {
        current = name ? name : "";
        readonly = readOnly;
        open = !current.empty();
        return open;
    }
    size_t getBytesLength(const char* key) const {
        if(!open) return 0;
        auto ns = storage.find(current);
        if(ns == storage.end()) return 0;
        auto value = ns->second.find(key);
        return value == ns->second.end() ? 0 : value->second.size();
    }
    size_t getBytes(const char* key, void* output, size_t length) const {
        if(getBytesLength(key) != length) return 0;
        const auto& bytes = storage.at(current).at(key);
        std::memcpy(output, bytes.data(), length);
        return length;
    }
    size_t putBytes(const char* key, const void* input, size_t length) {
        if(!open || readonly || failWrites || writesRemaining == 0) return 0;
        if(writesRemaining > 0) writesRemaining--;
        const auto* first = static_cast<const uint8_t*>(input);
        storage[current][key] = std::vector<uint8_t>(first, first + length);
        return length;
    }
    uint32_t getUInt(const char* key, uint32_t fallback = 0) const {
        uint32_t value = fallback;
        return getBytesLength(key) == sizeof(value) &&
                       getBytes(key, &value, sizeof(value)) == sizeof(value)
                   ? value
                   : fallback;
    }
    size_t putUInt(const char* key, uint32_t value) {
        return putBytes(key, &value, sizeof(value));
    }
    bool clear() {
        if(!open || readonly || failWrites) return false;
        storage[current].clear();
        return true;
    }
    void end() { open = false; }
    static void reset() { storage.clear(); failWrites = false; writesRemaining = -1; }

private:
    std::string current;
    bool readonly = false;
    bool open = false;
};
