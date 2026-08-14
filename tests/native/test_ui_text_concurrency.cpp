#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "ha_event_format.h"

std::atomic<uint32_t> haUiLocaleCache{(uint32_t)HaUiEnglish};

static bool isEither(const char* actual, const char* english, const char* german) {
    return std::strcmp(actual, english) == 0 || std::strcmp(actual, german) == 0;
}

int main() {
    uint8_t english = UINT8_MAX;
    uint8_t german = UINT8_MAX;
    for(size_t i = 0; i < HA_GENERATED_LANGUAGE_COUNT; i++) {
        if(std::strcmp(HA_GENERATED_LANGUAGES[i].code, "en") == 0) english = (uint8_t)i;
        if(std::strcmp(HA_GENERATED_LANGUAGES[i].code, "de") == 0) german = (uint8_t)i;
    }
    assert(english != UINT8_MAX);
    assert(german != UINT8_MAX);

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::thread switcher([&]() {
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for(unsigned i = 0; i < 100000; i++)
            haUiSetLocaleFromLanguage((i & 1U) ? german : english);
    });

    std::vector<std::thread> readers;
    for(unsigned worker = 0; worker < 4; worker++) {
        readers.emplace_back([&]() {
            while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for(unsigned i = 0; i < 50000; i++) {
                char line[96];
                std::snprintf(line, sizeof(line), "%s NOVA", haUiT(HaUiTextEventJoin));
                if(!isEither(line, "JOIN NOVA", "DA NOVA")) failed.store(true);
                std::snprintf(line, sizeof(line), "%s NOVA", haUiT(HaUiTextEventLeave));
                if(!isEither(line, "LEAVE NOVA", "WEG NOVA")) failed.store(true);

                const HaUiLocale locale = haUiActiveLocale();
                if(haFormatHostEvent(
                       HA_HOST_EVT_ROLE,
                       locale == HaUiGerman ? "Malen" : "Drawing",
                       "NOVA",
                       "?",
                       0,
                       "drawer",
                       line,
                       sizeof(line),
                       locale,
                       HA_GAME_DRAW) != HaHostEventStatus ||
                   !isEither(line, "Drawing: NOVA drawer", "Malen: NOVA zeichnet"))
                    failed.store(true);

                if(haFormatHostEvent(
                       HA_HOST_EVT_ROUND_WIN,
                       locale == HaUiGerman ? "Schach" : "Chess",
                       "NOVA",
                       "ORBIT",
                       0,
                       "mate",
                       line,
                       sizeof(line),
                       locale,
                       HA_GAME_CHESS) != HaHostEventStatus ||
                   !isEither(
                       line,
                       "Chess: NOVA beat ORBIT (mate)",
                       "Schach: NOVA besiegt ORBIT (Matt)"))
                    failed.store(true);
            }
        });
    }

    start.store(true, std::memory_order_release);
    switcher.join();
    for(std::thread& reader : readers) reader.join();
    assert(!failed.load());
    std::cout << "native atomic host-UI locale concurrency tests passed\n";
}
