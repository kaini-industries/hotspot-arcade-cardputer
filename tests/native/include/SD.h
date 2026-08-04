#pragma once

#include <Arduino.h>
#include <map>

#define FILE_READ 0
#define FILE_WRITE 1

class SDClass;

class File {
public:
    File() = default;
    explicit operator bool() const { return state && state->open; }
    size_t size() const { return *this ? state->bytes->size() : 0; }
    int available() const { return *this && state->position < state->bytes->size(); }
    int read() {
        if(!available()) return -1;
        return (*state->bytes)[state->position++];
    }
    size_t write(const uint8_t* bytes, size_t length);
    size_t write(uint8_t value) { return write(&value, 1); }
    void flush() {}
    void close() { if(state) state->open = false; }

private:
    struct State {
        SDClass* owner;
        std::vector<uint8_t>* bytes;
        size_t position;
        bool writable;
        bool open;
    };
    explicit File(std::shared_ptr<State> value) : state(std::move(value)) {}
    std::shared_ptr<State> state;
    friend class SDClass;
};

class SDClass {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    long writeRemaining = -1;

    File open(const char* path, int mode) {
        auto found = files.find(path);
        if(mode == FILE_READ && found == files.end()) return File{};
        if(found == files.end()) found = files.emplace(path, std::vector<uint8_t>{}).first;
        auto state = std::make_shared<File::State>();
        state->owner = this;
        state->bytes = &found->second;
        state->position = mode == FILE_WRITE ? found->second.size() : 0;
        state->writable = mode == FILE_WRITE;
        state->open = true;
        return File(state);
    }
    bool mkdir(const char*) { return true; }
    bool remove(const char* path) { return files.erase(path) > 0; }
    bool exists(const char* path) const { return files.find(path) != files.end(); }
    bool rename(const char* source, const char* destination) {
        auto found = files.find(source);
        if(found == files.end() || exists(destination)) return false;
        files[destination] = found->second;
        files.erase(found);
        return true;
    }
    void putText(const char* path, const std::string& text) {
        files[path] = std::vector<uint8_t>(text.begin(), text.end());
    }
    void reset() { files.clear(); writeRemaining = -1; }
};

inline size_t File::write(const uint8_t* bytes, size_t length) {
    if(!*this || !state->writable) return 0;
    size_t allowed = length;
    if(state->owner->writeRemaining >= 0) {
        allowed = std::min<size_t>(allowed, (size_t)state->owner->writeRemaining);
        state->owner->writeRemaining -= (long)allowed;
    }
    if(state->position + allowed > state->bytes->size()) state->bytes->resize(state->position + allowed);
    std::copy(bytes, bytes + allowed, state->bytes->begin() + state->position);
    state->position += allowed;
    return allowed;
}

inline SDClass SD;
