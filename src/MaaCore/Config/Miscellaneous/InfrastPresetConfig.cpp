#include "InfrastPresetConfig.h"

#include <algorithm>
#include <set>
#include <unordered_map>

#include "Config/Miscellaneous/InfrastConfig.h"
#include "Utils/Logger.hpp"

namespace asst::infrast
{
static std::optional<CustomRoomConfig> parse_preset_room(const std::string& facility, const json::value& room)
{
    if (!room.is_object()) {
        LogError << "Invalid custom infrastructure room";
        return std::nullopt;
    }
    CustomRoomConfig result;
    for (const auto& [key, target] : std::initializer_list<std::pair<std::string, bool*>> {
             { "skip", &result.skip },
             { "autofill", &result.autofill },
             { "sort", &result.sort },
             { "use_operator_groups", &result.use_operator_groups } }) {
        if (room.contains(key)) {
            if (!room.at(key).is_boolean()) {
                LogError << "Invalid room flag" << key;
                return std::nullopt;
            }
            *target = room.at(key).as_boolean();
        }
    }
    if (room.contains("product")) {
        if (!room.at("product").is_string()) {
            LogError << "Invalid room product";
            return std::nullopt;
        }
        // Only an omitted product may skip verification; an explicit name must fit the facility.
        static const std::unordered_map<std::string, std::pair<CustomRoomConfig::Product, std::string>>
            product_names = {
                { "Battle Record", { CustomRoomConfig::Product::BattleRecord, "Mfg" } },
                { "Pure Gold", { CustomRoomConfig::Product::PureGold, "Mfg" } },
                { "Dualchip", { CustomRoomConfig::Product::Dualchip, "Mfg" } },
                { "Originium Shard", { CustomRoomConfig::Product::OriginiumShard, "Mfg" } },
                { "LMD", { CustomRoomConfig::Product::LMD, "Trade" } },
                { "Orundum", { CustomRoomConfig::Product::Orundum, "Trade" } },
            };
        const std::string product = room.at("product").as_string();
        if (auto iter = product_names.find(product); iter != product_names.cend()) {
            if (facility != iter->second.second) {
                LogError << "Preset product does not match the facility" << facility << product;
                return std::nullopt;
            }
            result.product = iter->second.first;
        }
        else {
            LogError << "Unsupported preset product" << product;
            return std::nullopt;
        }
    }
    for (const auto& [key, target] : std::initializer_list<std::pair<std::string, std::vector<std::string>*>> {
             { "operators", &result.names },
             { "candidates", &result.candidates } }) {
        if (!room.contains(key)) {
            continue;
        }
        if (!room.at(key).is_array()) {
            LogError << "Invalid room operator list" << key;
            return std::nullopt;
        }
        for (const auto& value : room.at(key).as_array()) {
            if (!value.is_string()) {
                LogError << "Invalid room operator name" << key;
                return std::nullopt;
            }
            if (!value.as_string().empty()) {
                target->emplace_back(value.as_string());
            }
        }
    }
    return result;
}

}

std::optional<asst::infrast::PresetImport> asst::infrast::parse_preset_import(const json::value& config)
{
    LogTraceFunction;
    if (!config.is_object() || !config.contains("plans") || !config.at("plans").is_array()) {
        LogError << "Preset import requires a plans array";
        return std::nullopt;
    }
    PresetImport result;
    std::map<std::pair<int, int>, PresetRoom> rooms;
    const auto& plans = config.at("plans").as_array();
    if (plans.size() > 3) {
        result.warnings.emplace_back("extra_plans");
    }
    for (size_t slot = 0; slot < std::min(plans.size(), size_t { 3 }); ++slot) {
        const auto& plan = plans.at(slot);
        if (!plan.is_object() || (plan.contains("rooms") && !plan.at("rooms").is_object())) {
            LogError << "Invalid preset plan" << slot;
            return std::nullopt;
        }
        if (!plan.contains("rooms")) {
            continue;
        }
        for (const auto& [name, values] : plan.at("rooms").as_object()) {
            const auto facility = std::ranges::find(PresetFacilities, name, &PresetFacility::config_name);
            if (facility == PresetFacilities.end()) {
                // Unsupported facilities such as dormitories are silently skipped; users know their own plans.
                continue;
            }
            if (!values.is_array()) {
                LogError << "Invalid preset facility" << name;
                return std::nullopt;
            }
            int index = 0;
            for (const auto& room : values.as_array()) {
                const auto room_index = index++;
                if (room_index >= facility->room_count) {
                    LogError << "Preset room index is out of range" << name << room_index;
                    return std::nullopt;
                }
                auto parsed = parse_preset_room(facility->name, room);
                if (!parsed) {
                    return std::nullopt;
                }
                auto& target = rooms[{ int(facility - PresetFacilities.begin()), room_index }];
                target.facility = facility->name;
                target.index = room_index;
                // Skipping a write must not authorize deleting the referenced queue during cleanup.
                target.referenced_slot_count = std::max(target.referenced_slot_count, int(slot) + 1);
                if (parsed->skip) {
                    continue;
                }
                if (parsed->autofill || parsed->use_operator_groups || !parsed->candidates.empty()) {
                    result.warnings.emplace_back("dynamic_room");
                    continue;
                }
                if (parsed->names.empty()) {
                    continue;
                }
                const std::set<std::string> unique(parsed->names.begin(), parsed->names.end());
                const auto& info = InfrastData.get_facility_info(facility->name);
                if (unique.size() != parsed->names.size() || parsed->names.size() > size_t(info.max_num_of_opers)) {
                    LogError << "Invalid preset operator count or duplicate names" << name << room_index << slot;
                    return std::nullopt;
                }
                // 同一房间的全部队列必须共用一个产物：未指定沿用已指定的，冲突直接拒绝整份导入
                if (parsed->product != CustomRoomConfig::Product::Unknown) {
                    if (target.product != CustomRoomConfig::Product::Unknown && target.product != parsed->product) {
                        LogError << "Conflicting preset products for one room" << name << room_index << slot;
                        return std::nullopt;
                    }
                    target.product = parsed->product;
                }
                target.slots.emplace(int(slot), std::move(*parsed));
                ++result.slot_count;
            }
        }
    }
    if (result.slot_count == 0) {
        LogError << "Preset import has no static operator queues";
        return std::nullopt;
    }
    for (auto& [key, room] : rooms) {
        if (room.slots.empty()) {
            continue;
        }
        result.rooms.emplace_back(std::move(room));
    }
    std::ranges::sort(result.warnings);
    result.warnings.erase(std::unique(result.warnings.begin(), result.warnings.end()), result.warnings.end());
    return result;
}
