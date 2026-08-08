#pragma once

#include <Arduino.h>
#include <map>
#include <set>

#define FILE_READ 0
#define FILE_WRITE 1

class SDClass;

class File {
public:
    File() = default;
    explicit operator bool() const { return state && state->open; }
    size_t size() const { return *this && !state->directory ? state->bytes->size() : 0; }
    int available() const {
        return *this && !state->directory && state->position < state->bytes->size();
    }
    int read() {
        if(!available()) return -1;
        return (*state->bytes)[state->position++];
    }
    size_t write(const uint8_t* bytes, size_t length);
    size_t write(uint8_t value) { return write(&value, 1); }
    bool seek(size_t position) {
        if(!*this || state->directory || position > state->bytes->size()) return false;
        state->position = position;
        return true;
    }
    bool isDirectory() const { return *this && state->directory; }
    const char* name() const { return *this ? state->path.c_str() : ""; }
    File openNextFile();
    void flush() {}
    void close() { if(state) state->open = false; }

private:
    struct State {
        SDClass* owner;
        std::vector<uint8_t>* bytes;
        size_t position;
        bool writable;
        bool directory;
        bool open;
        std::string path;
        std::vector<std::string> entries;
        size_t entryPosition;
    };
    explicit File(std::shared_ptr<State> value) : state(std::move(value)) {}
    std::shared_ptr<State> state;
    friend class SDClass;
};

class SDClass {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> directories = {"/"};
    long writeRemaining = -1;
    bool failMkdir = false;
    bool failWriteOpen = false;
    std::string failRenameSource;
    std::string failRenameDestination;

    static std::string normalize(const char* raw) {
        std::string path = raw ? raw : "";
        if(path.empty()) return path;
        if(path.front() != '/') path.insert(path.begin(), '/');
        while(path.size() > 1 && path.back() == '/') path.pop_back();
        return path;
    }

    static std::string parent(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        if(slash == std::string::npos || slash == 0) return "/";
        return path.substr(0, slash);
    }

    void ensureParents(const std::string& path) {
        std::string current = parent(path);
        std::vector<std::string> missing;
        while(!current.empty() && directories.find(current) == directories.end()) {
            missing.push_back(current);
            current = parent(current);
        }
        for(auto it = missing.rbegin(); it != missing.rend(); ++it) directories.insert(*it);
    }

    File open(const char* path, int mode = FILE_READ) {
        const std::string normalized = normalize(path);
        if(mode == FILE_READ && directories.find(normalized) != directories.end()) {
            auto state = std::make_shared<File::State>();
            state->owner = this;
            state->bytes = nullptr;
            state->position = 0;
            state->writable = false;
            state->directory = true;
            state->open = true;
            state->path = normalized;
            state->entryPosition = 0;
            for(const auto& directory : directories) {
                if(directory != normalized && parent(directory) == normalized)
                    state->entries.push_back(directory);
            }
            for(const auto& file : files) {
                if(parent(file.first) == normalized) state->entries.push_back(file.first);
            }
            std::sort(state->entries.begin(), state->entries.end());
            return File(state);
        }
        auto found = files.find(normalized);
        if(mode == FILE_READ && found == files.end()) return File{};
        if(mode == FILE_WRITE && failWriteOpen) return File{};
        if(found == files.end()) {
            ensureParents(normalized);
            found = files.emplace(normalized, std::vector<uint8_t>{}).first;
        }
        auto state = std::make_shared<File::State>();
        state->owner = this;
        state->bytes = &found->second;
        state->position = mode == FILE_WRITE ? found->second.size() : 0;
        state->writable = mode == FILE_WRITE;
        state->directory = false;
        state->open = true;
        state->path = normalized;
        state->entryPosition = 0;
        return File(state);
    }
    bool mkdir(const char* raw) {
        if(failMkdir) return false;
        const std::string path = normalize(raw);
        if(path.empty()) return false;
        ensureParents(path);
        directories.insert(path);
        return true;
    }
    bool remove(const char* raw) {
        const std::string path = normalize(raw);
        if(files.erase(path) > 0) return true;
        if(path != "/" && directories.find(path) != directories.end()) {
            for(const auto& file : files) if(parent(file.first) == path) return false;
            for(const auto& directory : directories)
                if(directory != path && parent(directory) == path) return false;
            directories.erase(path);
            return true;
        }
        return false;
    }
    bool exists(const char* raw) const {
        const std::string path = normalize(raw);
        return files.find(path) != files.end() || directories.find(path) != directories.end();
    }
    bool rename(const char* source, const char* destination) {
        const std::string from = normalize(source);
        const std::string to = normalize(destination);
        if((!failRenameSource.empty() && from == normalize(failRenameSource.c_str())) ||
           (!failRenameDestination.empty() && to == normalize(failRenameDestination.c_str())))
            return false;
        auto found = files.find(from);
        if(found == files.end() || exists(to.c_str())) return false;
        ensureParents(to);
        files[to] = found->second;
        files.erase(found);
        return true;
    }
    void putText(const char* path, const std::string& text) {
        const std::string normalized = normalize(path);
        ensureParents(normalized);
        files[normalized] = std::vector<uint8_t>(text.begin(), text.end());
    }
    void reset() {
        files.clear();
        directories = {"/"};
        writeRemaining = -1;
        failMkdir = false;
        failWriteOpen = false;
        failRenameSource.clear();
        failRenameDestination.clear();
    }
};

inline size_t File::write(const uint8_t* bytes, size_t length) {
    if(!*this || !state->writable || state->directory) return 0;
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

inline File File::openNextFile() {
    if(!*this || !state->directory || state->entryPosition >= state->entries.size())
        return File{};
    const std::string path = state->entries[state->entryPosition++];
    return state->owner->open(path.c_str(), FILE_READ);
}

inline SDClass SD;
