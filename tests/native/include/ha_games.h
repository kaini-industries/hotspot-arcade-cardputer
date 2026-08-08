#pragma once

// Persistence tests need only the engine's bounded roster constants and game ids,
// not the several-thousand-line header-only referee implementation.
#include "ha_proto.h"

#ifndef HA_MAX_PLAYERS
#define HA_MAX_PLAYERS 10
#endif
#ifndef HA_NICK_LEN
#define HA_NICK_LEN 20
#endif
