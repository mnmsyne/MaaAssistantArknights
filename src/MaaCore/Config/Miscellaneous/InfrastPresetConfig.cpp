#include "InfrastPresetConfig.h"

#include <set>
#include <unordered_map>

#include "Config/Miscellaneous/InfrastConfig.h"
#include "Utils/Logger.hpp"

namespace asst::infrast
{
static bool product_matches_facility(const std::string& facility, CustomRoomConfig::Product product)
{
    switch (product) {
    case CustomRoomConfig::Product::BattleRecord:
    case CustomRoomConfig::Product::PureGold:
    case CustomRoomConfig::Product::Dualchip:
    case CustomRoomConfig::Product::OriginiumShard:
        return facility == "Mfg";
    case CustomRoomConfig::Product::LMD:
    case CustomRoomConfig::Product::Orundum:
        return facility == "Trade";
    default:
        return false;
    }
}

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
        static const std::unordered_map<std::string, CustomRoomConfig::Product> product_names = {
            { "Battle Record", CustomRoomConfig::Product::BattleRecord },
            { "Pure Gold", CustomRoomConfig::Product::PureGold },
            { "Dualchip", CustomRoomConfig::Product::Dualchip },
            { "Originium Shard", CustomRoomConfig::Product::OriginiumShard },
            { "LMD", CustomRoomConfig::Product::LMD },
            { "Orundum", CustomRoomConfig::Product::Orundum },
        };
        const std::string product = room.at("product").as_string();
        if (auto iter = product_names.find(product); iter != product_names.cend()) {
            if (!product_matches_facility(facility, iter->second)) {
                LogError << "Preset product does not match the facility" << facility << product;
                return std::nullopt;
            }
            result.product = iter->second;
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
    const std::map<std::string, std::string> facilities = {
        { "control", "Control" }, { "manufacture", "Mfg" },   { "trading", "Trade" },
        { "power", "Power" },     { "meeting", "Reception" }, { "hire", "Office" },
    };
    // Overview rows are ordered Control, Reception, Mfg, Trade, Power, Office; visiting rooms in
    // that order keeps both the preflight and the import scans monotonic on a single downward pass.
    const std::map<std::string, int> facility_order = {
        { "Control", 0 }, { "Reception", 1 }, { "Mfg", 2 }, { "Trade", 3 }, { "Power", 4 }, { "Office", 5 },
    };
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
            const auto facility = facilities.find(name);
            if (facility == facilities.end()) {
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
                auto parsed = parse_preset_room(facility->second, room);
                if (!parsed) {
                    return std::nullopt;
                }
                if (parsed->skip || parsed->names.empty()) {
                    continue;
                }
                if (parsed->autofill || parsed->use_operator_groups || !parsed->candidates.empty()) {
                    result.warnings.emplace_back("dynamic_room");
                    continue;
                }
                if ((facility->second == "Control" || facility->second == "Reception" ||
                     facility->second == "Office") &&
                    room_index != 0) {
                    LogError << "Preset facility has only one room" << name << room_index;
                    return std::nullopt;
                }
                const std::set<std::string> unique(parsed->names.begin(), parsed->names.end());
                const auto& info = InfrastData.get_facility_info(facility->second);
                if (unique.size() != parsed->names.size() || parsed->names.size() > size_t(info.max_num_of_opers)) {
                    LogError << "Invalid preset operator count or duplicate names" << name << room_index << slot;
                    return std::nullopt;
                }
                auto& target = rooms[{ facility_order.at(facility->second), room_index }];
                target.facility = facility->second;
                target.index = room_index;
                // 同一房间的全部队列必须共用一个产物：未指定沿用已指定的，冲突直接拒绝整份导入
                if (parsed->product != CustomRoomConfig::Product::Unknown) {
                    if (target.product == CustomRoomConfig::Product::Unknown) {
                        target.product = parsed->product;
                    }
                    else if (target.product != parsed->product) {
                        LogError << "Conflicting preset products for one room" << name << room_index << slot;
                        return std::nullopt;
                    }
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
        result.rooms.emplace_back(std::move(room));
    }
    std::ranges::sort(result.warnings);
    result.warnings.erase(std::unique(result.warnings.begin(), result.warnings.end()), result.warnings.end());
    return result;
}
