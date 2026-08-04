#include <cassert>
#include <iostream>

bool haSdOk = true;
#include "ha_config.h"

static void resetRuntime() {
    haConfigRt = HaConfigRuntime{};
}

static void resetAll() {
    SD.reset();
    Preferences::reset();
    resetRuntime();
    haSdOk = true;
}

static void testAlternatingAndRecovery() {
    resetAll();
    assert(haConfigBegin("Hotspot Arcade", 1, 0));
    assert(haConfigSave("Party One", 2, 1));
    assert(SD.exists(HA_CONFIG_A));
    assert(haConfigGet().generation == 1);
    assert(haConfigSave("Party Two", 0, 0));
    assert(SD.exists(HA_CONFIG_B));
    assert(haConfigGet().generation == 2);

    // A corrupted newest slot must leave the previous verified slot usable.
    SD.files[HA_CONFIG_B][10] ^= 0x01;
    Preferences::reset();
    resetRuntime();
    assert(haConfigBegin("default", 1, 0));
    assert(std::strcmp(haConfigGet().ssid, "Party One") == 0);
    assert(haConfigGet().generation == 1);
}

static void testNvsGenerationResolution() {
    resetAll();
    HaConfigRecord sd = {7, "SD", 1, 0};
    HaConfigRecord nvs = {8, "NVS", 2, 1};
    assert(haConfigWriteSd(HA_CONFIG_A, sd));
    assert(haConfigWriteNvs(nvs));
    assert(haConfigBegin("default", 1, 0));
    assert(std::strcmp(haConfigGet().ssid, "NVS") == 0);

    // SD wins a same-generation conflict.
    resetRuntime();
    nvs.generation = 7;
    assert(haConfigWriteNvs(nvs));
    assert(haConfigBegin("default", 1, 0));
    assert(std::strcmp(haConfigGet().ssid, "SD") == 0);
}

static void testEveryInterruptedWriteKeepsOldGeneration() {
    resetAll();
    assert(haConfigBegin("default", 1, 0));
    assert(haConfigSave("known-good", 1, 0));
    const auto baseFiles = SD.files;
    const auto baseNvs = Preferences::storage;
    Preferences::failWrites = true;
    assert(haConfigSave("interrupted", 2, 1)); // SD succeeds; learn the exact record length.
    const long completeLength = (long)SD.files.at(HA_CONFIG_B).size();
    Preferences::failWrites = false;

    for(long cut = 0; cut < completeLength; cut++) {
        SD.files = baseFiles;
        Preferences::storage = baseNvs;
        SD.writeRemaining = cut;
        Preferences::failWrites = true;
        resetRuntime();
        assert(haConfigBegin("default", 1, 0));
        (void)haConfigSave("interrupted", 2, 1);
        SD.writeRemaining = -1;
        Preferences::failWrites = false;
        resetRuntime();
        assert(haConfigBegin("default", 1, 0));
        if(std::strcmp(haConfigGet().ssid, "known-good") != 0) {
            std::cerr << "interrupted cut " << cut << "/" << completeLength
                      << " selected " << haConfigGet().ssid << " generation "
                      << haConfigGet().generation << "\n";
            std::abort();
        }
        assert(haConfigGet().generation == 1);
    }
}

static void testLegacyMigrationIsIdempotent() {
    resetAll();
    SD.putText(HA_CONFIG_V1, "ssid=Legacy Party\naudio=2\nlang=1\n");
    assert(haConfigBegin("default", 1, 0));
    assert(std::strcmp(haConfigGet().ssid, "Legacy Party") == 0);
    assert(SD.exists(HA_CONFIG_V1_IMPORTED));
    assert(!SD.exists(HA_CONFIG_V1));
    auto files = SD.files;
    resetRuntime();
    assert(haConfigBegin("default", 1, 0));
    assert(SD.files == files);
    haConfigMigrationDone();
    assert(SD.exists(HA_MIGRATION_V2_DONE));
}

static void testBoundsAndValidation() {
    resetAll();
    assert(haConfigBegin("default", 1, 0));
    assert(!haConfigSave("", 1, 0));
    assert(!haConfigSave("012345678901234567890123456789012", 1, 0));
    assert(!haConfigSave("valid", 3, 0));
    assert(!haConfigSave("valid", 1, 99));
    SD.putText(HA_CONFIG_A, std::string(HA_CONFIG_MAX_BYTES + 1, 'x'));
    Preferences::reset();
    resetRuntime();
    assert(haConfigBegin("safe-default", 1, 0));
    assert(std::strcmp(haConfigGet().ssid, "safe-default") == 0);
}

int main() {
    testAlternatingAndRecovery();
    testNvsGenerationResolution();
    testEveryInterruptedWriteKeepsOldGeneration();
    testLegacyMigrationIsIdempotent();
    testBoundsAndValidation();
    std::cout << "native config tests passed\n";
}
