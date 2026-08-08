#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ha_ssid_transaction.h"

static std::vector<std::string> persisted;
static std::vector<std::string> started;
static int persistFailAt = -1;
static int startFailAt = -1;

static bool persistSsid(const char* value) {
    persisted.emplace_back(value);
    return (int)persisted.size() != persistFailAt;
}

static bool startSsid(const char* value) {
    started.emplace_back(value);
    return (int)started.size() != startFailAt;
}

static void resetFakes() {
    persisted.clear();
    started.clear();
    persistFailAt = -1;
    startFailAt = -1;
}

static void testCandidatePersistsBeforeRuntimeAndStart() {
    resetFakes();
    char runtime[33] = "Prior";
    assert(haSsidApplyTransaction(
               runtime, sizeof(runtime), "Candidate", true, persistSsid, startSsid) ==
           HaSsidAppliedRunning);
    assert(std::strcmp(runtime, "Candidate") == 0);
    assert((persisted == std::vector<std::string>{"Candidate"}));
    assert((started == std::vector<std::string>{"Candidate"}));
}

static void testRejectedCandidateRestartsOnlyPrior() {
    resetFakes();
    persistFailAt = 1;
    char runtime[33] = "Prior";
    assert(haSsidApplyTransaction(
               runtime, sizeof(runtime), "Candidate", true, persistSsid, startSsid) ==
           HaSsidCandidateRejectedPriorRunning);
    assert(std::strcmp(runtime, "Prior") == 0);
    assert((started == std::vector<std::string>{"Prior"}));
}

static void testFailedCandidateApUsesDurableFallback() {
    resetFakes();
    startFailAt = 1;
    char runtime[33] = "Prior";
    assert(haSsidApplyTransaction(
               runtime, sizeof(runtime), "Candidate", true, persistSsid, startSsid) ==
           HaSsidFallbackRunning);
    assert(std::strcmp(runtime, "Prior") == 0);
    assert((persisted == std::vector<std::string>{"Candidate", "Prior"}));
    assert((started == std::vector<std::string>{"Candidate", "Prior"}));
}

static void testRejectedRollbackRetainsDurableCandidateAndStops() {
    resetFakes();
    startFailAt = 1;
    persistFailAt = 2;
    char runtime[33] = "Prior";
    assert(haSsidApplyTransaction(
               runtime, sizeof(runtime), "Candidate", true, persistSsid, startSsid) ==
           HaSsidRollbackRejectedCandidateOffline);
    assert(std::strcmp(runtime, "Candidate") == 0);
    assert(started.size() == 1 && started[0] == "Candidate");
}

static void testManualOffUpdatesDurableAndRuntimeWithoutStarting() {
    resetFakes();
    char runtime[33] = "Prior";
    assert(haSsidApplyTransaction(
               runtime, sizeof(runtime), "Candidate", false, persistSsid, startSsid) ==
           HaSsidAppliedOffline);
    assert(std::strcmp(runtime, "Candidate") == 0);
    assert(started.empty());
}

int main() {
    testCandidatePersistsBeforeRuntimeAndStart();
    testRejectedCandidateRestartsOnlyPrior();
    testFailedCandidateApUsesDurableFallback();
    testRejectedRollbackRetainsDurableCandidateAndStops();
    testManualOffUpdatesDurableAndRuntimeWithoutStarting();
    std::cout << "native SSID transaction tests passed\n";
}
