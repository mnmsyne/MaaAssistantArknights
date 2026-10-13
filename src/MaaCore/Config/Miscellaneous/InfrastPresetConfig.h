#pragma once

#include <map>
#include <meojson/json.hpp>

#include "Common/AsstInfrastDef.h"

namespace asst::infrast
{
struct PresetRoom
{
    std::string facility;
    int index = 0;
    // 全部队列统一后的预期产物；Unknown 表示无任一队列显式指定，不做产物校验
    CustomRoomConfig::Product product = CustomRoomConfig::Product::Unknown;
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
