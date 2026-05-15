#pragma once

#include <cstddef>
#include <cstdint>

// D2R memory layout constants live here so offset updates do not require
// touching overlay rendering, event processing, or Win32 window code.
constexpr uint32_t D2_UNIT_MONSTER = 1;

constexpr uint16_t D2_STAT_HITPOINTS = 6;
constexpr uint16_t D2_STAT_MAXHP = 7;
constexpr uint16_t D2_STAT_ITEM_CRUSHINGBLOW = 136;
constexpr uint16_t D2_STAT_ITEM_DEADLYSTRIKE = 141;
constexpr uint16_t D2_STAT_PASSIVE_CRITICAL_STRIKE = 337;
constexpr uint16_t D2_STAT_PASSIVE_MASTERY_MELEE_CRIT = 344;
constexpr uint16_t D2_STAT_PASSIVE_MASTERY_THROW_CRIT = 347;

constexpr int D2_UNIT_HASH_BUCKETS = 128;
constexpr int D2_MAX_STAT_COUNT = 256;
constexpr uintptr_t D2_UNIT_HASH_TABLE_STRIDE = D2_UNIT_HASH_BUCKETS * sizeof(uintptr_t);

constexpr uintptr_t D2_UNIT_OFFSET_TYPE = 0x00;
constexpr uintptr_t D2_UNIT_OFFSET_ID = 0x08;
constexpr uintptr_t D2_UNIT_OFFSET_PATH = 0x38;
constexpr uintptr_t D2_UNIT_OFFSET_STATS = 0x88;
constexpr uintptr_t D2_UNIT_OFFSET_NEXT = 0x150;
constexpr uintptr_t D2_SERVER_UNIT_OFFSET_NEXT = 0x158;

constexpr uintptr_t D2_PATH_OFFSET_X = 0x00;
constexpr uintptr_t D2_PATH_OFFSET_Y = 0x04;

constexpr uintptr_t D2_STATLIST_BASE_ARRAY = 0x30;
constexpr uintptr_t D2_STATLIST_EXTRA_ARRAY = 0x80;

constexpr uintptr_t D2_GAME_UNIT_TABLE_PLAYERS = 0x2230;
constexpr uintptr_t D2_GAME_UNIT_TABLE_MONSTERS = 0x2630;
constexpr uintptr_t D2_GAME_UNIT_TABLE_OBJECTS = 0x2A30;
constexpr uintptr_t D2_GAME_UNIT_TABLE_ITEMS = 0x2E30;
constexpr uintptr_t D2_GAME_UNIT_TABLE_MISSILES = 0x3230;
constexpr uintptr_t D2_GAME_UNIT_TABLE_TILES = 0x3630;

constexpr size_t D2_SERVER_UNIT_SNAPSHOT_BYTES = 0x220;
constexpr size_t D2_CLIENT_UNIT_SNAPSHOT_BYTES = 0x220;
constexpr size_t D2_EXTENDED_UNIT_SNAPSHOT_BYTES = 0x800;
constexpr size_t D2_STATLIST_SNAPSHOT_BYTES = 0x180;
constexpr size_t D2_STAT_ENTRIES_SNAPSHOT_BYTES = 0x800;

constexpr uint32_t D2_UNITFLAG_UPGRLIFENHITCLASS = 0x00008000;
constexpr size_t D2_CLIENT_HITCLASS_CANDIDATE_OFFSET = 0x110;
constexpr size_t D2_CLIENT_UNIT_FLAGS_CANDIDATE_OFFSET = 0x124;
constexpr size_t D2_CLIENT_UNIT_FLAGS_EX_CANDIDATE_OFFSET = 0x128;

constexpr size_t HIT_RECORDER_MAX_SAMPLES = 160;
constexpr uint64_t HIT_RECORDER_CONTEXT_SAMPLES = 18;
constexpr size_t HIT_RECORDER_MAX_HITS = 96;
constexpr int64_t HIT_RECORDER_NEARBY_HIT_MS = 2000;
constexpr int HIT_RECORDER_TARGET_DIFF_LIMIT = 64;
constexpr int HIT_RECORDER_PLAYER_DIFF_LIMIT = 48;
constexpr int HIT_RECORDER_EXTENDED_DIFF_LIMIT = 96;
constexpr int HIT_RECORDER_STATLIST_DIFF_LIMIT = 96;
constexpr size_t HIT_RECORDER_MAX_NEARBY_HIT_DETAILS = 8;
constexpr int HIT_RECORDER_STAT_DIFF_LIMIT = 80;

constexpr size_t MONSTER_TRACK_MAX_UNITS = 1024;
