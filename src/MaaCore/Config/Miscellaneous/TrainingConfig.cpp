#include "TrainingConfig.h"

#include "BattleDataConfig.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include <meojson/json.hpp>

#include "Utils/Logger.hpp"
#include "Utils/WorkingDir.hpp"

namespace
{
constexpr std::array<asst::battle::Role, 8> SupportedRoles = {
    asst::battle::Role::Caster,  asst::battle::Role::Medic,   asst::battle::Role::Pioneer, asst::battle::Role::Sniper,
    asst::battle::Role::Special, asst::battle::Role::Support, asst::battle::Role::Tank,    asst::battle::Role::Warrior,
};

struct ParsedTrainingSkill
{
    int phase = 0;
    int level = 0;
    std::unordered_set<asst::battle::Role> targets;
    std::array<int, 3> speed_bonus {};
    std::array<double, 3> mood_cost_extra {};
    bool dynamic = false;
};

bool unlock_reached(const ParsedTrainingSkill& skill, int phase, int level)
{
    return skill.phase < phase || (skill.phase == phase && skill.level <= level);
}

std::optional<asst::battle::Role> parse_role(std::string_view role)
{
    static const std::unordered_map<std::string_view, asst::battle::Role> RoleMap = {
        { "CASTER", asst::battle::Role::Caster },   { "MEDIC", asst::battle::Role::Medic },
        { "PIONEER", asst::battle::Role::Pioneer }, { "SNIPER", asst::battle::Role::Sniper },
        { "SPECIAL", asst::battle::Role::Special }, { "SUPPORT", asst::battle::Role::Support },
        { "TANK", asst::battle::Role::Tank },       { "WARRIOR", asst::battle::Role::Warrior },
    };
    const auto iter = RoleMap.find(role);
    return iter == RoleMap.end() ? std::nullopt : std::optional(iter->second);
}

std::optional<int> parse_integral_number(const json::value& value, int minimum, int maximum)
{
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double number = value.as_double();
    if (!std::isfinite(number) || std::trunc(number) != number || number < minimum || number > maximum) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

bool bonus_matches(const std::vector<int>& percentages, int expected)
{
    if (std::ranges::find(percentages, expected) != percentages.end()) {
        return true;
    }
    for (size_t lhs = 0; lhs < percentages.size(); ++lhs) {
        for (size_t rhs = lhs + 1; rhs < percentages.size(); ++rhs) {
            if (percentages[lhs] + percentages[rhs] == expected) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool asst::TrainingConfig::ensure_loaded() noexcept
{
    std::call_once(m_load_once, [this] {
        using namespace utils::path_literals;

        try {
            if (ResDir.empty()) {
                Log.error(__FUNCTION__, "resource directory is not initialized");
                return;
            }

            const auto resource_dir = ResDir.get();
            const auto base_path = resource_dir / "training.json"_p;
            if (!load(base_path)) {
                Log.error(__FUNCTION__, "failed to load training config", base_path);
                return;
            }

            m_load_succeeded = true;
        }
        catch (const std::exception& e) {
            Log.error(__FUNCTION__, "exception while loading training config", e.what());
        }
        catch (...) {
            Log.error(__FUNCTION__, "unknown exception while loading training config");
        }
    });
    return m_load_succeeded;
}

const std::vector<asst::TrainingAssistant>&
    asst::TrainingConfig::stage_assistants(battle::Role role, int mastery) const noexcept
{
    static const std::vector<TrainingAssistant> Empty;
    const auto role_iter = m_stage_assistants.find(role);
    if (role_iter == m_stage_assistants.end() || mastery < 1 || mastery > 3) {
        return Empty;
    }
    return role_iter->second.at(static_cast<size_t>(mastery - 1));
}

std::optional<std::pair<int, double>> asst::training::match_stage_bonus(
    const std::vector<int>& percentages,
    const std::vector<TrainingAssistant>& variants)
{
    std::optional<std::pair<int, double>> matched;
    for (const auto& variant : variants) {
        if (!bonus_matches(percentages, variant.bonus)) {
            continue;
        }
        const std::pair current(variant.bonus, variant.mood_cost_per_hour);
        if (!matched || current.first > matched->first ||
            (current.first == matched->first && current.second < matched->second)) {
            matched = current;
        }
    }
    return matched;
}

std::unordered_map<std::string, asst::training::StageAssistantGroup>
    asst::training::stage_assistant_groups(battle::Role role, int mastery)
{
    std::unordered_map<std::string, StageAssistantGroup> groups;
    for (const auto& config : Training.stage_assistants(role, mastery)) {
        if (config.dynamic) {
            continue;
        }
        const auto oper = BattleData.find_oper_by_id(config.operator_id);
        if (!oper) {
            continue;
        }
        auto [iter, inserted] =
            groups.try_emplace(oper->name, StageAssistantGroup { .operator_id = config.operator_id });
        if (!inserted && iter->second.operator_id != config.operator_id) {
            Log.warn("stage_assistant_groups: duplicate assistant display name:", oper->name);
            continue;
        }
        iter->second.variants.emplace_back(config);
        iter->second.max_bonus = std::max(iter->second.max_bonus, config.bonus);
    }
    return groups;
}

std::unordered_map<std::string, asst::TrainingHalvingAssistant> asst::training::halving_assistants_by_name()
{
    std::unordered_map<std::string, TrainingHalvingAssistant> configs;
    for (const auto& config : Training.halving_assistants()) {
        const auto oper = BattleData.find_oper_by_id(config.operator_id);
        if (oper) {
            configs.emplace(oper->name, config);
        }
    }
    return configs;
}

std::optional<std::string_view> asst::TrainingConfig::exclusion_reason(std::string_view operator_id) const noexcept
{
    const auto iter = m_excluded_operators.find(std::string(operator_id));
    if (iter == m_excluded_operators.end()) {
        return std::nullopt;
    }
    return iter->second;
}

bool asst::TrainingConfig::parse(const json::value& json)
{
    LogTraceFunction;
    const auto base_minutes = json.find<json::array>("mastery_base_minutes");
    const auto halving_trigger_json = json.find<json::value>("halving_trigger_minutes");
    const auto operators = json.find<json::object>("operators");
    const auto halving_assistants = json.find<json::array>("halving_assistants");
    const auto excluded_operators = json.find<json::object>("excluded_operators");
    if (!base_minutes || base_minutes->size() != 3 || !halving_trigger_json || !operators || !halving_assistants ||
        !excluded_operators) {
        Log.error(__FUNCTION__, "training config is incomplete");
        return false;
    }
    const auto halving_trigger_minutes =
        parse_integral_number(*halving_trigger_json, 1, std::numeric_limits<int>::max());
    if (!halving_trigger_minutes) {
        Log.error(__FUNCTION__, "invalid halving trigger minutes");
        return false;
    }
    std::array<int, 3> parsed_base_minutes {};
    for (size_t index = 0; index < parsed_base_minutes.size(); ++index) {
        const auto value = parse_integral_number(base_minutes->at(index), 1, std::numeric_limits<int>::max());
        if (!value) {
            Log.error(__FUNCTION__, "invalid mastery base minutes", index);
            return false;
        }
        parsed_base_minutes.at(index) = *value;
    }
    std::unordered_map<battle::Role, std::array<std::vector<TrainingAssistant>, 3>> parsed_assistants;
    std::unordered_map<std::string, std::string> parsed_control_center_skills;
    for (const auto& [operator_id, skills_json] : *operators) {
        if (operator_id.empty()) {
            Log.error(__FUNCTION__, "invalid training operator", operator_id);
            return false;
        }
        if (!BattleData.find_oper_by_id(operator_id)) {
            Log.warn(__FUNCTION__, "training operator is missing from battle data, skip", operator_id);
            continue;
        }
        if (!skills_json.is_array() || skills_json.as_array().empty()) {
            Log.error(__FUNCTION__, "invalid training operator", operator_id);
            return false;
        }
        std::map<int, std::vector<ParsedTrainingSkill>> training_groups;
        std::set<std::pair<int, int>> checkpoints;
        for (const auto& skill_json : skills_json.as_array()) {
            const auto group = skill_json.find<int>("group");
            const auto phase = skill_json.find<int>("phase");
            const auto level = skill_json.find<int>("level");
            const auto room = skill_json.find<std::string>("room");
            if (!group || *group < 0 || !phase || *phase < 0 || *phase > 2 || !level || *level <= 0 || !room) {
                Log.error(__FUNCTION__, "invalid training skill unlock", operator_id);
                return false;
            }
            if (*room == "CONTROL") {
                const auto skill = skill_json.find<std::string>("skill");
                if (!skill || skill->empty()) {
                    Log.error(__FUNCTION__, "invalid control center training skill", operator_id);
                    return false;
                }
                const auto [iter, inserted] = parsed_control_center_skills.emplace(*skill, operator_id);
                if (!inserted && iter->second != operator_id) {
                    Log.error(__FUNCTION__, "duplicated control center training skill", *skill);
                    return false;
                }
                continue;
            }
            if (*room != "TRAINING") {
                Log.error(__FUNCTION__, "invalid training skill room", operator_id, *room);
                return false;
            }
            const auto targets = skill_json.find<json::array>("targets");
            const auto speed_bonus = skill_json.find<json::array>("speed_bonus");
            const auto mood_cost_extra = skill_json.find<json::array>("mood_cost_extra");
            if (!targets || targets->empty() || !speed_bonus || speed_bonus->size() != 3 ||
                (mood_cost_extra && mood_cost_extra->size() != 3)) {
                Log.error(__FUNCTION__, "invalid training room skill effect", operator_id);
                return false;
            }
            ParsedTrainingSkill parsed_skill {
                .phase = *phase,
                .level = *level,
                .dynamic = skill_json.get("dynamic", false),
            };
            for (const auto& target_json : *targets) {
                if (!target_json.is_string()) {
                    Log.error(__FUNCTION__, "invalid training room skill target", operator_id);
                    return false;
                }
                const auto role = parse_role(target_json.as_string());
                if (!role) {
                    Log.error(__FUNCTION__, "unknown training room skill target", operator_id, target_json.as_string());
                    return false;
                }
                parsed_skill.targets.emplace(*role);
            }
            for (size_t index = 0; index < parsed_skill.speed_bonus.size(); ++index) {
                const auto bonus = parse_integral_number(speed_bonus->at(index), 0, 100);
                if (!bonus || (mood_cost_extra && !mood_cost_extra->at(index).is_number())) {
                    Log.error(__FUNCTION__, "invalid training room skill value", operator_id);
                    return false;
                }
                const double mood_extra = mood_cost_extra ? mood_cost_extra->at(index).as_double() : 0.0;
                if (!std::isfinite(mood_extra) || mood_extra < 0) {
                    Log.error(__FUNCTION__, "invalid training room mood cost", operator_id);
                    return false;
                }
                parsed_skill.speed_bonus.at(index) = *bonus;
                parsed_skill.mood_cost_extra.at(index) = mood_extra;
            }
            training_groups[*group].emplace_back(std::move(parsed_skill));
            checkpoints.emplace(*phase, *level);
        }
        for (auto& [group, skills] : training_groups) {
            std::ignore = group;
            std::ranges::sort(skills, {}, [](const ParsedTrainingSkill& skill) {
                return std::pair(skill.phase, skill.level);
            });
        }
        for (const auto [phase, level] : checkpoints) {
            std::vector<const ParsedTrainingSkill*> active_skills;
            for (const auto& [group, skills] : training_groups) {
                std::ignore = group;
                const ParsedTrainingSkill* active = nullptr;
                for (const auto& skill : skills) {
                    if (unlock_reached(skill, phase, level)) {
                        active = &skill;
                    }
                }
                if (active) {
                    active_skills.emplace_back(active);
                }
            }
            for (const auto role : SupportedRoles) {
                for (size_t mastery_index = 0; mastery_index < 3; ++mastery_index) {
                    int bonus = 0;
                    double mood_cost = 1.0;
                    bool dynamic = false;
                    for (const auto* skill : active_skills) {
                        if (!skill->targets.contains(role)) {
                            continue;
                        }
                        bonus += skill->speed_bonus.at(mastery_index);
                        mood_cost += skill->mood_cost_extra.at(mastery_index);
                        dynamic = dynamic || skill->dynamic;
                    }
                    if (bonus <= 0) {
                        continue;
                    }
                    parsed_assistants[role]
                        .at(mastery_index)
                        .emplace_back(
                            TrainingAssistant {
                                .operator_id = operator_id,
                                .bonus = bonus,
                                .mood_cost_per_hour = mood_cost,
                                .dynamic = dynamic,
                            });
                }
            }
        }
    }
    for (auto& [role, mastery_entries] : parsed_assistants) {
        std::ignore = role;
        for (auto& candidates : mastery_entries) {
            std::ranges::sort(candidates, [](const auto& lhs, const auto& rhs) {
                if (lhs.bonus != rhs.bonus) {
                    return lhs.bonus > rhs.bonus;
                }
                if (lhs.dynamic != rhs.dynamic) {
                    return !lhs.dynamic;
                }
                if (lhs.mood_cost_per_hour != rhs.mood_cost_per_hour) {
                    return lhs.mood_cost_per_hour < rhs.mood_cost_per_hour;
                }
                return lhs.operator_id < rhs.operator_id;
            });
            candidates.erase(
                std::unique(
                    candidates.begin(),
                    candidates.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return std::tie(lhs.operator_id, lhs.bonus, lhs.mood_cost_per_hour, lhs.dynamic) ==
                               std::tie(rhs.operator_id, rhs.bonus, rhs.mood_cost_per_hour, rhs.dynamic);
                    }),
                candidates.end());
        }
    }
    if (parsed_assistants.size() != SupportedRoles.size()) {
        Log.warn(__FUNCTION__, "training assistant roles are incomplete", parsed_assistants.size());
    }
    if (parsed_control_center_skills.empty()) {
        Log.warn(__FUNCTION__, "control center training skill list is empty");
    }
    for (const auto role : SupportedRoles) {
        const auto role_iter = parsed_assistants.find(role);
        if (role_iter == parsed_assistants.end()) {
            Log.warn(__FUNCTION__, "missing training assistant role", static_cast<int>(role));
            continue;
        }
        const auto& mastery_entries = role_iter->second;
        for (size_t mastery_index = 0; mastery_index < mastery_entries.size(); ++mastery_index) {
            if (mastery_entries.at(mastery_index).empty()) {
                Log.warn(__FUNCTION__, "missing mastery assistant list", static_cast<int>(role), mastery_index + 1);
            }
        }
    }
    std::vector<TrainingHalvingAssistant> parsed_halving_assistants;
    for (const auto& assistant_json : *halving_assistants) {
        const auto operator_id = assistant_json.find<std::string>("operator_id");
        if (!operator_id || operator_id->empty()) {
            Log.error(__FUNCTION__, "invalid halving assistant entry");
            return false;
        }
        if (!BattleData.find_oper_by_id(*operator_id)) {
            Log.warn(__FUNCTION__, "halving assistant is missing from battle data, skip", *operator_id);
            continue;
        }
        int trigger_bonus = 50;
        int role_bonus = 0;
        if (const auto value = assistant_json.find<json::value>("trigger_bonus")) {
            const auto parsed = parse_integral_number(*value, 1, 100);
            if (!parsed) {
                Log.error(__FUNCTION__, "invalid halving assistant trigger bonus", operator_id.value_or(""));
                return false;
            }
            trigger_bonus = *parsed;
        }
        if (const auto value = assistant_json.find<json::value>("role_bonus")) {
            const auto parsed = parse_integral_number(*value, 0, 100);
            if (!parsed) {
                Log.error(__FUNCTION__, "invalid halving assistant role bonus", operator_id.value_or(""));
                return false;
            }
            role_bonus = *parsed;
        }
        const auto roles = assistant_json.find<json::array>("roles");
        if (!roles) {
            Log.error(__FUNCTION__, "invalid halving assistant entry");
            return false;
        }
        TrainingHalvingAssistant assistant { .operator_id = *operator_id,
                                             .trigger_bonus = trigger_bonus,
                                             .role_bonus = role_bonus };
        for (size_t role_index = 0; role_index < roles->size(); ++role_index) {
            const auto& role_json = roles->at(role_index);
            if (!role_json.is_string()) {
                Log.error(__FUNCTION__, "halving assistant role is not a string", *operator_id, role_index);
                return false;
            }
            const auto role = parse_role(role_json.as_string());
            if (!role) {
                Log.error(__FUNCTION__, "invalid halving assistant role", role_json.as_string());
                return false;
            }
            assistant.roles.emplace(*role);
        }
        parsed_halving_assistants.emplace_back(std::move(assistant));
    }
    if (parsed_halving_assistants.empty()) {
        Log.warn(__FUNCTION__, "halving assistant list is empty after validation");
    }
    std::unordered_map<std::string, std::string> parsed_excluded_operators;
    if (excluded_operators->empty()) {
        Log.warn(__FUNCTION__, "excluded training operator list is empty");
    }
    for (const auto& [operator_id, reason_json] : *excluded_operators) {
        if (operator_id.empty()) {
            Log.error(__FUNCTION__, "invalid excluded operator entry", operator_id);
            return false;
        }
        if (!BattleData.find_oper_by_id(operator_id)) {
            Log.warn(__FUNCTION__, "excluded operator is missing from battle data, skip", operator_id);
            continue;
        }
        if (!reason_json.is_string() || reason_json.as_string().empty()) {
            Log.error(__FUNCTION__, "invalid excluded operator entry", operator_id);
            return false;
        }
        parsed_excluded_operators.emplace(operator_id, reason_json.as_string());
    }
    if (parsed_excluded_operators.empty()) {
        Log.warn(__FUNCTION__, "excluded operator list is empty after validation");
    }
    m_mastery_base_minutes = parsed_base_minutes;
    m_halving_trigger_minutes = *halving_trigger_minutes;
    m_stage_assistants = std::move(parsed_assistants);
    m_halving_assistants = std::move(parsed_halving_assistants);
    m_control_center_training_skills = std::move(parsed_control_center_skills);
    m_excluded_operators = std::move(parsed_excluded_operators);
    return true;
}
