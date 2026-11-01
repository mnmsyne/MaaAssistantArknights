#pragma once

#include <array>
#include <map>
#include <meojson/json.hpp>

#include "Common/AsstInfrastDef.h"

namespace asst::infrast
{
struct PresetFacility
{
    std::string config_name;
    std::string name;
    std::string title;
    int room_count;
};

// Simplified Chinese overview order, shared by parsing and room identification.
inline const std::array<PresetFacility, 7> PresetFacilities = { {
    { "control", "Control", "控制中枢", 1 },
    { "meeting", "Reception", "会客室", 1 },
    { "manufacture", "Mfg", "制造站", 5 },
    { "trading", "Trade", "贸易站", 5 },
    { "power", "Power", "发电站", 3 },
    { "hire", "Office", "办公室", 1 },
    { "recycle", "Recycle", "回收站", 1 },
} };

struct PresetRoom
{
    std::string facility;
    int index = 0;
    // 全部队列统一后的预期产物；Unknown 表示无任一队列显式指定，不做产物校验
    CustomRoomConfig::Product product = CustomRoomConfig::Product::Unknown;
    // Highest referenced plan slot + 1, including skipped, dynamic and empty placeholders.
    int referenced_slot_count = 0;
    std::map<int, CustomRoomConfig> slots;
};

struct PresetImport
{
    std::vector<PresetRoom> rooms;
    std::vector<std::string> warnings;
    size_t slot_count = 0;
};

std::optional<PresetImport> parse_preset_import(const json::value& config);
}
