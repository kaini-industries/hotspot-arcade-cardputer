// Versioned, bounded settings storage. SD uses alternating config.a/config.b
// records; NVS mirrors the newest generation for no-card recovery. At boot the
// highest valid generation wins, with SD winning an equal-generation tie.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include "ha_metadata.h"

#define HA_CONFIG_SCHEMA 2
#define HA_CONFIG_MAX_BYTES 2048
#define HA_CONFIG_LINE_MAX 256

static const char* const HA_CONFIG_DIR = "/hotspot-arcade";
static const char* const HA_CONFIG_A = "/hotspot-arcade/config.a";
static const char* const HA_CONFIG_B = "/hotspot-arcade/config.b";
static const char* const HA_CONFIG_V1 = "/hotspot-arcade/config.txt";
static const char* const HA_CONFIG_V1_IMPORTED = "/hotspot-arcade/config.txt.v1.imported";
static const char* const HA_MIGRATION_V2_DONE = "/hotspot-arcade/migration-v2.done";

extern bool haSdOk;

struct HaConfigRecord {
    uint32_t generation;
    char ssid[33];
    uint8_t audio;
    uint8_t language;
};

struct HaConfigRuntime {
    HaConfigRecord value;
    int8_t sdSlot;
    bool begun;
    bool sdDirty;
    bool nvsDirty;
    bool legacyRenamePending;
};

static HaConfigRuntime haConfigRt = {};

static uint32_t haConfigCrcUpdate(uint32_t crc, const uint8_t* data, size_t len) {
    while(len--) {
        crc ^= *data++;
        for(uint8_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static uint32_t haConfigCrcLine(uint32_t crc, const char* line) {
    crc = haConfigCrcUpdate(crc, (const uint8_t*)line, strlen(line));
    const uint8_t nl = '\n';
    return haConfigCrcUpdate(crc, &nl, 1);
}

static int haConfigReadLine(File& file, char* out, size_t capacity) {
    if(!out || capacity < 2) return -1;
    size_t count = 0;
    bool got = false;
    bool overflow = false;
    bool newline = false;
    while(file.available()) {
        int value = file.read();
        if(value < 0) break;
        got = true;
        if(value == '\n') { newline = true; break; }
        if(value == '\r') continue;
        if(count + 1 < capacity) out[count++] = (char)value;
        else overflow = true;
    }
    out[count] = '\0';
    if(overflow) return -1;
    return got ? (newline ? 1 : 2) : 0;
}

static bool haConfigParseU32(const char* value, uint32_t& out) {
    if(!value || !value[0] || value[0] == '-') return false;
    char* end = nullptr;
    unsigned long long parsed = strtoull(value, &end, 10);
    if(!end || *end || parsed > UINT32_MAX) return false;
    out = (uint32_t)parsed;
    return true;
}

static int haConfigHex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool haConfigEncode(const char* input, char* output, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    if(!input || !output || !capacity) return false;
    size_t used = 0;
    for(const uint8_t* cursor = (const uint8_t*)input; *cursor; cursor++) {
        const uint8_t c = *cursor;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                           c == '_' || c == '.';
        const size_t needed = plain ? 1 : 3;
        if(used + needed >= capacity) return false;
        if(plain) output[used++] = (char)c;
        else {
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15];
        }
    }
    output[used] = '\0';
    return true;
}

static bool haConfigDecode(const char* input, char* output, size_t capacity) {
    if(!input || !output || !capacity) return false;
    size_t used = 0;
    for(size_t i = 0; input[i]; i++) {
        uint8_t c = (uint8_t)input[i];
        if(c == '%') {
            if(!input[i + 1] || !input[i + 2]) return false;
            int hi = haConfigHex(input[i + 1]);
            int lo = haConfigHex(input[i + 2]);
            if(hi < 0 || lo < 0) return false;
            c = (uint8_t)((hi << 4) | lo);
            if(c == 0) return false;
            i += 2;
        }
        if(used + 1 >= capacity) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    return true;
}

static bool haConfigValidUtf8(const char* value) {
    const uint8_t* p = (const uint8_t*)value;
    while(*p) {
        if(*p < 0x20 || *p == 0x7F) return false;
        if(*p < 0x80) { p++; continue; }
        uint8_t needed = 0;
        uint32_t code = 0;
        if((*p & 0xE0) == 0xC0) { needed = 1; code = *p & 0x1F; if(code < 2) return false; }
        else if((*p & 0xF0) == 0xE0) { needed = 2; code = *p & 0x0F; }
        else if((*p & 0xF8) == 0xF0) { needed = 3; code = *p & 0x07; }
        else return false;
        p++;
        for(uint8_t i = 0; i < needed; i++, p++) {
            if((*p & 0xC0) != 0x80) return false;
            code = (code << 6) | (*p & 0x3F);
        }
        if((needed == 2 && code < 0x800) || (needed == 3 && code < 0x10000) ||
           (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
    }
    return true;
}

static bool haConfigValid(const HaConfigRecord& value) {
    size_t length = strnlen(value.ssid, sizeof(value.ssid));
    return length > 0 && length <= 32 && length < sizeof(value.ssid) &&
           value.audio <= 2 && value.language < HA_GENERATED_LANGUAGE_COUNT &&
           haConfigValidUtf8(value.ssid);
}

static bool haConfigSame(const HaConfigRecord& left, const HaConfigRecord& right) {
    return left.generation == right.generation && left.audio == right.audio &&
           left.language == right.language && strcmp(left.ssid, right.ssid) == 0;
}

static bool haConfigReadSd(const char* path, HaConfigRecord& out) {
    if(!haSdOk) return false;
    File file = SD.open(path, FILE_READ);
    if(!file) return false;
    if(file.size() == 0 || file.size() > HA_CONFIG_MAX_BYTES) { file.close(); return false; }
    HaConfigRecord parsed = {};
    bool header = false, gotGeneration = false, gotSsid = false;
    bool gotAudio = false, gotLanguage = false, gotCrc = false;
    uint32_t crc = 0xFFFFFFFFUL;
    char line[HA_CONFIG_LINE_MAX];
    while(true) {
        int status = haConfigReadLine(file, line, sizeof(line));
        if(status == 0) break;
        // Every v2 record line, including the CRC terminator, must end in LF so
        // a power cut after the last CRC digit is still detected as a torn write.
        if(status != 1) { file.close(); return false; }
        if(strncmp(line, "crc=", 4) == 0) {
            if(gotCrc || strlen(line + 4) != 8) { file.close(); return false; }
            char* end = nullptr;
            unsigned long expected = strtoul(line + 4, &end, 16);
            if(!end || *end || (crc ^ 0xFFFFFFFFUL) != (uint32_t)expected) {
                file.close();
                return false;
            }
            gotCrc = true;
            continue;
        }
        if(gotCrc) { file.close(); return false; }
        crc = haConfigCrcLine(crc, line);
        if(!header) {
            if(strcmp(line, "HA2|CONFIG") != 0) { file.close(); return false; }
            header = true;
        } else if(strncmp(line, "generation=", 11) == 0 && !gotGeneration) {
            gotGeneration = haConfigParseU32(line + 11, parsed.generation);
            if(!gotGeneration) { file.close(); return false; }
        } else if(strncmp(line, "ssid=", 5) == 0 && !gotSsid) {
            gotSsid = haConfigDecode(line + 5, parsed.ssid, sizeof(parsed.ssid));
            if(!gotSsid) { file.close(); return false; }
        } else if(strncmp(line, "audio=", 6) == 0 && !gotAudio) {
            uint32_t value = 0;
            gotAudio = haConfigParseU32(line + 6, value) && value <= 2;
            parsed.audio = (uint8_t)value;
            if(!gotAudio) { file.close(); return false; }
        } else if(strncmp(line, "language=", 9) == 0 && !gotLanguage) {
            uint32_t value = 0;
            gotLanguage = haConfigParseU32(line + 9, value) && value < HA_GENERATED_LANGUAGE_COUNT;
            parsed.language = (uint8_t)value;
            if(!gotLanguage) { file.close(); return false; }
        } else { file.close(); return false; }
    }
    file.close();
    if(!header || !gotGeneration || !gotSsid || !gotAudio || !gotLanguage || !gotCrc ||
       !haConfigValid(parsed))
        return false;
    out = parsed;
    return true;
}

static bool haConfigWriteLine(File& file, uint32_t& crc, const char* format, ...) {
    char line[HA_CONFIG_LINE_MAX];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if(length < 0 || (size_t)length >= sizeof(line)) return false;
    if(file.write((const uint8_t*)line, (size_t)length) != (size_t)length ||
       file.write((uint8_t)'\n') != 1)
        return false;
    crc = haConfigCrcLine(crc, line);
    return true;
}

static bool haConfigWriteSd(const char* path, const HaConfigRecord& value) {
    if(!haSdOk || !haConfigValid(value)) return false;
    char encoded[sizeof(value.ssid) * 3];
    if(!haConfigEncode(value.ssid, encoded, sizeof(encoded))) return false;
    SD.mkdir(HA_CONFIG_DIR);
    SD.remove(path);
    File file = SD.open(path, FILE_WRITE);
    if(!file) return false;
    uint32_t crc = 0xFFFFFFFFUL;
    bool ok = haConfigWriteLine(file, crc, "HA2|CONFIG") &&
              haConfigWriteLine(file, crc, "generation=%lu", (unsigned long)value.generation) &&
              haConfigWriteLine(file, crc, "ssid=%s", encoded) &&
              haConfigWriteLine(file, crc, "audio=%u", (unsigned)value.audio) &&
              haConfigWriteLine(file, crc, "language=%u", (unsigned)value.language);
    if(ok) {
        char finalLine[32];
        int n = snprintf(finalLine, sizeof(finalLine), "crc=%08lX\n", (unsigned long)(crc ^ 0xFFFFFFFFUL));
        ok = n > 0 && (size_t)n < sizeof(finalLine) &&
             file.write((const uint8_t*)finalLine, (size_t)n) == (size_t)n;
    }
    file.flush();
    ok = ok && file.size() <= HA_CONFIG_MAX_BYTES;
    file.close();
    if(!ok) return false;
    HaConfigRecord verified = {};
    return haConfigReadSd(path, verified) && verified.generation == value.generation &&
           strcmp(verified.ssid, value.ssid) == 0 && verified.audio == value.audio &&
           verified.language == value.language;
}

struct HaConfigNvsBlob {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    HaConfigRecord value;
    uint32_t crc;
};

static bool haConfigReadNvs(HaConfigRecord& out) {
    Preferences prefs;
    if(!prefs.begin("ha_cfg2", true)) return false;
    HaConfigNvsBlob blob = {};
    bool ok = prefs.getBytesLength("record") == sizeof(blob) &&
              prefs.getBytes("record", &blob, sizeof(blob)) == sizeof(blob);
    prefs.end();
    if(!ok || blob.magic != 0x32434648UL || blob.schema != HA_CONFIG_SCHEMA ||
       blob.size != sizeof(blob))
        return false;
    uint32_t expected = haConfigCrcUpdate(0xFFFFFFFFUL, (const uint8_t*)&blob,
                                          offsetof(HaConfigNvsBlob, crc)) ^ 0xFFFFFFFFUL;
    if(expected != blob.crc || !haConfigValid(blob.value)) return false;
    out = blob.value;
    return true;
}

static bool haConfigWriteNvs(const HaConfigRecord& value) {
    if(!haConfigValid(value)) return false;
    HaConfigNvsBlob blob = {};
    blob.magic = 0x32434648UL;
    blob.schema = HA_CONFIG_SCHEMA;
    blob.size = sizeof(blob);
    blob.value = value;
    blob.crc = haConfigCrcUpdate(0xFFFFFFFFUL, (const uint8_t*)&blob,
                                 offsetof(HaConfigNvsBlob, crc)) ^ 0xFFFFFFFFUL;
    Preferences prefs;
    if(!prefs.begin("ha_cfg2", false)) return false;
    bool ok = prefs.putBytes("record", &blob, sizeof(blob)) == sizeof(blob);
    prefs.end();
    if(!ok) return false;
    HaConfigRecord verified = {};
    return haConfigReadNvs(verified) && haConfigSame(verified, value);
}

static bool haConfigReadLegacy(HaConfigRecord& value) {
    if(!haSdOk || !SD.exists(HA_CONFIG_V1)) return false;
    File file = SD.open(HA_CONFIG_V1, FILE_READ);
    if(!file || file.size() > HA_CONFIG_MAX_BYTES) { if(file) file.close(); return false; }
    bool changed = false;
    char line[HA_CONFIG_LINE_MAX];
    while(true) {
        int status = haConfigReadLine(file, line, sizeof(line));
        if(status == 0) break;
        if(status < 0) { file.close(); return false; }
        char* equals = strchr(line, '=');
        if(!equals) continue;
        *equals++ = '\0';
        if(strcmp(line, "ssid") == 0 && equals[0] && strlen(equals) <= 32 && haConfigValidUtf8(equals)) {
            strlcpy(value.ssid, equals, sizeof(value.ssid));
            changed = true;
        } else if(strcmp(line, "audio") == 0) {
            uint32_t parsed = 0;
            if(haConfigParseU32(equals, parsed) && parsed <= 2) { value.audio = parsed; changed = true; }
        } else if(strcmp(line, "lang") == 0) {
            uint32_t parsed = 0;
            if(haConfigParseU32(equals, parsed) && parsed < HA_GENERATED_LANGUAGE_COUNT) {
                value.language = parsed;
                changed = true;
            }
        }
    }
    file.close();
    return changed && haConfigValid(value);
}

static bool haConfigCommit(HaConfigRecord next) {
    if(!haConfigValid(next)) return false;
    // Reusing UINT32_MAX would create equal-generation conflicts that cannot be
    // ordered after a torn cross-media update. Require an explicit reset/migration
    // instead of pretending a saturated generation committed successfully.
    if(haConfigRt.value.generation == UINT32_MAX) return false;
    next.generation = haConfigRt.value.generation + 1;
    bool sdWritten = false;
    int8_t nextSlot = haConfigRt.sdSlot == 0 ? 1 : 0;
    if(haSdOk) {
        const char* path = nextSlot == 0 ? HA_CONFIG_A : HA_CONFIG_B;
        sdWritten = haConfigWriteSd(path, next);
        if(sdWritten) haConfigRt.sdSlot = nextSlot;
    }
    bool nvsWritten = haConfigWriteNvs(next);
    if(!sdWritten && !nvsWritten) return false;
    haConfigRt.value = next;
    haConfigRt.begun = true;
    // A successful medium establishes the new authoritative value. Keep retrying
    // the other copy at the same generation so later SD removal/insertion cannot
    // resurrect stale settings.
    haConfigRt.sdDirty = !sdWritten;
    haConfigRt.nvsDirty = !nvsWritten;
    return true;
}

static bool haConfigFilesEqual(const char* leftPath, const char* rightPath) {
    if(!haSdOk || !SD.exists(leftPath) || !SD.exists(rightPath)) return false;
    File left = SD.open(leftPath, FILE_READ);
    File right = SD.open(rightPath, FILE_READ);
    if(!left || !right || left.size() != right.size() || left.size() > HA_CONFIG_MAX_BYTES) {
        if(left) left.close();
        if(right) right.close();
        return false;
    }
    bool equal = true;
    while(left.available() || right.available()) {
        if(left.read() != right.read()) { equal = false; break; }
    }
    left.close();
    right.close();
    return equal;
}

// Repair is idempotent and may be called from loop(). It never advances the
// selected generation: this is a mirror repair, not a user-visible settings edit.
static bool haConfigRepairMirrors() {
    if(!haConfigRt.begun || !haConfigValid(haConfigRt.value)) return false;
    bool ok = true;
    if(haConfigRt.nvsDirty) {
        if(haConfigWriteNvs(haConfigRt.value)) haConfigRt.nvsDirty = false;
        else ok = false;
    }
    if(haConfigRt.sdDirty && haSdOk) {
        int8_t target = haConfigRt.sdSlot == 0 ? 1 : 0;
        const char* path = target == 0 ? HA_CONFIG_A : HA_CONFIG_B;
        if(haConfigWriteSd(path, haConfigRt.value)) {
            haConfigRt.sdSlot = target;
            haConfigRt.sdDirty = false;
        } else {
            ok = false;
        }
    }
    if(haConfigRt.legacyRenamePending && haSdOk) {
        bool preserved = false;
        if(!SD.exists(HA_CONFIG_V1)) {
            preserved = SD.exists(HA_CONFIG_V1_IMPORTED);
        } else if(SD.exists(HA_CONFIG_V1_IMPORTED)) {
            // Never overwrite an earlier preserved original. Removing the source
            // is safe only when it is byte-for-byte the already imported copy.
            preserved = haConfigFilesEqual(HA_CONFIG_V1, HA_CONFIG_V1_IMPORTED) &&
                        SD.remove(HA_CONFIG_V1);
        } else {
            preserved = SD.rename(HA_CONFIG_V1, HA_CONFIG_V1_IMPORTED) &&
                        !SD.exists(HA_CONFIG_V1) && SD.exists(HA_CONFIG_V1_IMPORTED);
        }
        if(preserved) haConfigRt.legacyRenamePending = false;
        else ok = false;
    }
    return ok;
}

static bool haConfigBegin(const char* defaultSsid, uint8_t defaultAudio, uint8_t defaultLanguage) {
    HaConfigRecord selected = {};
    strlcpy(selected.ssid, defaultSsid, sizeof(selected.ssid));
    selected.audio = defaultAudio <= 2 ? defaultAudio : 1;
    selected.language = defaultLanguage < HA_GENERATED_LANGUAGE_COUNT ? defaultLanguage : 0;
    HaConfigRecord a = {}, b = {}, nvs = {};
    bool validA = haConfigReadSd(HA_CONFIG_A, a);
    bool validB = haConfigReadSd(HA_CONFIG_B, b);
    bool validNvs = haConfigReadNvs(nvs);
    bool validSd = false;
    HaConfigRecord sd = {};
    if(validA && (!validB || a.generation >= b.generation)) { sd = a; haConfigRt.sdSlot = 0; validSd = true; }
    else if(validB) { sd = b; haConfigRt.sdSlot = 1; validSd = true; }
    else haConfigRt.sdSlot = -1;

    bool selectedSd = validSd && (!validNvs || sd.generation >= nvs.generation);
    if(selectedSd) selected = sd;
    else if(validNvs) selected = nvs;

    bool migrated = false;
    if(!validSd && !validNvs) migrated = haConfigReadLegacy(selected);
    haConfigRt.value = selected;
    haConfigRt.begun = true;
    if(validSd || validNvs) {
        haConfigRt.sdDirty = !validSd || (!selectedSd && !haConfigSame(sd, selected));
        haConfigRt.nvsDirty = !validNvs || (selectedSd && !haConfigSame(nvs, selected));
    } else {
        // Persist validated defaults as well as legacy imports. A first boot must
        // not remain recoverable only from compiled constants until a setting is
        // manually changed.
        if(!haConfigCommit(selected)) return false;
    }
    haConfigRt.legacyRenamePending = haSdOk && SD.exists(HA_CONFIG_V1) &&
                                      (migrated || validSd || validNvs);
    (void)haConfigRepairMirrors();
    return haConfigValid(haConfigRt.value);
}

static const HaConfigRecord& haConfigGet() { return haConfigRt.value; }

static bool haConfigSave(const char* ssid, uint8_t audio, uint8_t language) {
    if(!haConfigRt.begun) return false;
    if(ssid && strnlen(ssid, sizeof(haConfigRt.value.ssid)) >= sizeof(haConfigRt.value.ssid))
        return false;
    HaConfigRecord next = haConfigRt.value;
    if(ssid) strlcpy(next.ssid, ssid, sizeof(next.ssid));
    next.audio = audio;
    next.language = language;
    return haConfigCommit(next);
}

static void haConfigMigrationDone() {
    if(!haSdOk || SD.exists(HA_MIGRATION_V2_DONE)) return;
    File marker = SD.open(HA_MIGRATION_V2_DONE, FILE_WRITE);
    if(marker) {
        static const char text[] = "schema=2\n";
        marker.write((const uint8_t*)text, sizeof(text) - 1);
        marker.flush();
        marker.close();
    }
}
