#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Task/AbstractTask.h"
#include "Utils/StringMisc.hpp"

#include "MaaUtils/NoWarningCV.hpp"

// Pure data structures and helpers for the OperDevelop (干员培养) mastery-training feature. Everything here
// is device-independent; recognition primitives live in TrainingRecognition.h, resource accessors in
// TrainingAssistants.h, and the task flow in OperDevelopTaskProcess.cpp.

namespace asst::training
{
// User-visible mode names (in ComboBox order):
//   Halving    → "优先减半协助" — wire token "halving": M1/M2 prefer an available halving assistant,
//                falling back to the overall ranking when none qualifies; M3 uses the overall ranking.
//   Efficiency → "最高效率协助" — wire token "efficiency": every stage takes the top of the overall
//                assistant ranking. Halving assistants participate in that ranking like anyone else.
enum class TrainingMode
{
    Halving,
    Efficiency,
};

inline constexpr std::string_view training_mode_token(TrainingMode mode) noexcept
{
    return mode == TrainingMode::Halving ? "halving" : "efficiency";
}

// Positive-evidence classification of what a training room is currently doing. `Idle` must come from a
// positive idle signal on the page being inspected, not from the absence of the other signals.
enum class TrainingActivity
{
    Unknown,
    Idle,
    Processing,
    Completed,
    OccupiedFallback,
};

struct AssistantStatus
{
    // Ratio in [0, 1], matching InfrastOperImageAnalyzer::mood_ratio.
    double mood = 0;
    bool working = false;
    bool selected = false;
};

// One usable configured roster entry verified against the live training-room assistant panel.
struct AssistantInfo
{
    std::string name;
    int bonus = -1;
    bool is_halving = false;
    AssistantStatus status {};
};

// Slim per-stage plan produced for preflight display and start-time re-verification.
struct StageEstimate
{
    int skill_index = 0;
    int from_mastery = 0;
    int to_mastery = 0;
    int base_minutes = 0;
    // Conservative estimate using only the flat bonuses; the halving trigger (when one is seated) can only
    // shorten the real run, never lengthen it.
    int estimated_minutes = 0;
    std::string plan_kind = "efficiency"; // "halving" or "efficiency"
    std::string assistant_name;
    int assistant_bonus = -1;
    std::string warning;
};

struct SkillRequest
{
    int index = 0;
    int target_mastery = 0;

    MEO_JSONIZATION(index, target_mastery);
};

struct ExpectedMastery
{
    int index = 0;
    int current_mastery = -1;

    MEO_JSONIZATION(index, current_mastery);
};

struct ExpectedStagePlan
{
    int skill_index = 0;
    int from_mastery = -1;
    int to_mastery = -1;
    std::string plan_kind;
    std::string assistant_name;
    int estimated_minutes = -1;

    MEO_JSONIZATION(skill_index, from_mastery, to_mastery, plan_kind, assistant_name, estimated_minutes);
};

inline bool stage_matches_expected(const StageEstimate& actual, const ExpectedStagePlan& expected)
{
    return actual.skill_index == expected.skill_index && actual.from_mastery == expected.from_mastery &&
           actual.to_mastery == expected.to_mastery && actual.plan_kind == expected.plan_kind &&
           actual.assistant_name == expected.assistant_name && actual.estimated_minutes == expected.estimated_minutes;
}

struct DevelopmentConfig
{
    std::string action; // "preflight" or "execute"
    std::string operator_id;
    std::vector<SkillRequest> skills;
    TrainingMode training_mode = TrainingMode::Halving;
    bool allow_consume = false;
    std::vector<ExpectedMastery> expected_mastery;
    std::string preflight_token;
    std::optional<ExpectedStagePlan> expected_stage_plan;
};

// The assistant panel lists the whole roster, but only the configured candidates matter: the scan settles
// every candidate (verified or rejected) and stops, so the page cap is just the worst-case bound for a
// candidate whose name fails to OCR. Ten pages cover the observed candidate spread with margin.
constexpr int MaxAssistantScanPages = 10;

inline std::optional<int> stable_mastery(std::span<const std::optional<int>> observations)
{
    if (observations.size() < 3 || !observations.front()) {
        return std::nullopt;
    }
    const int expected = *observations.front();
    if (expected < 0 || expected > 3 ||
        !std::ranges::all_of(observations, [&](const auto& value) { return value && *value == expected; })) {
        return std::nullopt;
    }
    return expected;
}

// 上游已移除 BattleDataConfig::get_id：按名取 id 统一走 find_first_oper，未命中返回空串。
inline std::string oper_id_by_name(const std::string& name)
{
    const auto oper = BattleData.find_first_oper(battle::Role::Unknown, name);
    return oper != nullptr ? oper->id : std::string {};
}

// Deterministic fingerprint of a freshly-generated preflight plan, used to detect whether starting the task
// still sees the same plan the user was shown, or upstream state (mastery, config, control-center bonus)
// drifted in between.
inline std::string make_preflight_token(
    const DevelopmentConfig& config,
    int control_center_bonus_percent,
    const std::string& control_center_assistant,
    const std::unordered_map<int, int>& current_mastery,
    const StageEstimate* current_stage)
{
    std::string token = config.operator_id;
    for (const auto& request : config.skills) {
        token += std::format(":{}-{}-{}", request.index, current_mastery.at(request.index), request.target_mastery);
    }
    token += std::format(
        ":{}:{}:{}",
        training_mode_token(config.training_mode),
        control_center_bonus_percent,
        oper_id_by_name(control_center_assistant));
    if (current_stage) {
        token += std::format(
            ":{}-{}-{}:{}:{}:{}",
            current_stage->skill_index,
            current_stage->from_mastery,
            current_stage->to_mastery,
            current_stage->plan_kind,
            oper_id_by_name(current_stage->assistant_name),
            current_stage->estimated_minutes);
    }
    return token;
}

// Whether the device-observed mastery levels still match what the caller expected them to be (e.g. what preflight
// was shown), field by field.
inline bool expected_state_matches(const DevelopmentConfig& config, const std::unordered_map<int, int>& current_mastery)
{
    if (config.expected_mastery.empty() || config.expected_mastery.size() != config.skills.size()) {
        return false;
    }
    return std::ranges::all_of(config.expected_mastery, [&](const auto& expected) {
        auto current = current_mastery.find(expected.index);
        return current != current_mastery.end() && current->second == expected.current_mastery;
    });
}

struct TrainingRoomSnapshot
{
    TrainingActivity activity = TrainingActivity::Unknown;
    std::string operator_name;
    std::string assistant_name;
    std::string skill_name;
    int mastery = -1;
    int skill_index = 0;
    std::string time_left;
    bool trainee_present = false;
    bool assistant_present = false;
    int assistant_bonus = -1;
    std::string detection_source;
    cv::Mat skill_image;
    // True when this Completed snapshot came from the auto-played "mastery completed" announcement shown on
    // training-room entry (chibi art + rank banner + confirm button), rather than a static completed room state.
    // acknowledge_completion_announcement() must dismiss the announcement directly for this snapshot -- there is
    // no CompletedEntry card list while the announcement is on screen.
    bool completion_announced = false;
    std::string completion_skill_name;
};

struct CompletionAnnouncement
{
    int rank = 0;
    std::string skill_name;
};

enum class TraineeSelectionResult
{
    Ready,
    WorkingElsewhere,
    PossiblyWorkingElsewhere,
    NotFound,
    Failed,
};

// ----- material cells (single-frame reading) -----

struct MaterialRequirement
{
    int owned = 0;
    int required = 0;

    bool sufficient() const noexcept { return owned >= required; }

    bool operator==(const MaterialRequirement&) const noexcept = default;
};

// The mastery confirmation page lays out a fixed row of cells: 等级 (level requirement, NOT a material),
// then up to three material cells (skill book + up to two extra materials). Each cell is read independently
// via its own task-config ROI (OperDevelopMaterialCell1/2/3), so slot identity comes from which cell was
// read, never from sorting OCR hits inside one wide region.
inline constexpr size_t MaterialSlotMax = 3;

enum class MaterialCellState
{
    Absent,
    Unreadable,
    Parsed,
};

struct MaterialCellReading
{
    MaterialCellState state = MaterialCellState::Absent;
    MaterialRequirement value {};

    bool operator==(const MaterialCellReading&) const noexcept = default;
};

using MaterialSlotFrame = std::array<MaterialCellReading, MaterialSlotMax>;

// All slots must resolve for a frame to be trusted: every mastery stage requires at least the skill book, so
// a frame with zero parsed cells is never "complete" even though trailing empty cells legitimately read as
// Absent, and any Unreadable cell keeps the whole frame incomplete so consumption can never proceed on a
// partially-verified page.
struct MaterialValidation
{
    MaterialSlotFrame frame;

    bool complete() const noexcept
    {
        const bool any_parsed = std::ranges::any_of(frame, [](const MaterialCellReading& cell) {
            return cell.state == MaterialCellState::Parsed;
        });
        const bool none_unreadable = std::ranges::none_of(frame, [](const MaterialCellReading& cell) {
            return cell.state == MaterialCellState::Unreadable;
        });
        return any_parsed && none_unreadable;
    }

    bool sufficient() const noexcept
    {
        return complete() && std::ranges::all_of(frame, [](const MaterialCellReading& cell) {
                   return cell.state != MaterialCellState::Parsed || cell.value.sufficient();
               });
    }
};

inline std::optional<int> parse_nonnegative_integer(std::string_view text) noexcept
{
    int value = 0;
    if (!utils::chars_to_number<int, true>(text, value) || value < 0) {
        return std::nullopt;
    }
    return value;
}

inline std::optional<MaterialRequirement> parse_material_requirement(std::span<const std::string> texts)
{
    static const std::regex QuantityPattern(R"(^\s*(\d+)\s*/\s*(\d+)\s*$)");
    std::string text;
    for (const auto& part : texts) {
        text += part;
    }

    // The thin "/" between owned and required frequently survives OCR as "_", "-" or another lookalike, or
    // vanishes entirely (measured on-device: "578/8" -> "5788", "10/4" -> "104_", "83/3" -> "_833"). First
    // canonicalize known substitutes, then fall back to splitting a bare digit run with the required count
    // taken as the trailing 1-2 digits -- every mastery-page quantity has a small required value (<= 30) and
    // a positive owned count, which rejects garbage splits while recovering all observed shapes.
    std::string normalized;
    normalized.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '_':
        case '-':
        case '~':
        case '|':
        case ':':
            normalized.push_back('/');
            break;
        default:
            normalized.push_back(ch);
            break;
        }
    }
    std::smatch match;
    if (std::regex_match(normalized, match, QuantityPattern)) {
        const auto owned = parse_nonnegative_integer(match[1].str());
        const auto required = parse_nonnegative_integer(match[2].str());
        if (owned && required) {
            return MaterialRequirement { .owned = *owned, .required = *required };
        }
    }

    // Bare-number fallback: the separator itself was lost entirely. Strip all non-digit OCR debris first
    // ("104_" -> "104", "_833" -> "833"), then consider the required count as the trailing 1-2 digits.
    // Only accept a unique valid split. Re-reading the same ambiguous digit run before consumption would not
    // make a guessed split safe, so ambiguity must remain unreadable.
    std::string digits;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
        }
    }
    std::optional<MaterialRequirement> candidate;
    for (const size_t required_digits : { size_t { 1 }, size_t { 2 } }) {
        if (digits.size() <= required_digits) {
            break;
        }
        const std::string_view required_text(digits.data() + digits.size() - required_digits, required_digits);
        if (required_text.size() > 1 && required_text.front() == '0') {
            continue;
        }
        const auto owned = parse_nonnegative_integer(digits.substr(0, digits.size() - required_digits));
        const auto required = parse_nonnegative_integer(required_text);
        if (owned && *owned > 0 && required && *required > 0 && *required <= 30) {
            const MaterialRequirement current { .owned = *owned, .required = *required };
            if (candidate && *candidate != current) {
                return std::nullopt;
            }
            candidate = current;
        }
    }
    return candidate;
}

// Classifies one cell from its OCR output (all detections in the cell, in reading order). A failed OCR result is
// only Absent when the quantity ROI is also visually empty; visible foreground with no parseable quantity is
// Unreadable and must keep the consumption gate closed.
inline MaterialCellReading classify_material_cell(std::span<const std::string> texts, bool foreground_present = false)
{
    if (const auto requirement = parse_material_requirement(texts)) {
        return MaterialCellReading { .state = MaterialCellState::Parsed, .value = *requirement };
    }
    const bool has_digit = std::ranges::any_of(texts, [](const std::string& text) {
        return std::ranges::any_of(text, [](const char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        });
    });
    return MaterialCellReading { .state = has_digit || foreground_present ? MaterialCellState::Unreadable
                                                                          : MaterialCellState::Absent,
                                 .value = MaterialRequirement {} };
}

// ----- survivors of the removed monitor/planner layers -----

inline std::optional<int> parse_training_countdown_seconds(std::string_view text)
{
    std::string trimmed(text);
    utils::string_trim(trimmed);
    std::string_view remaining = trimmed;

    std::vector<int> parts;
    while (!remaining.empty()) {
        const size_t separator = remaining.find(':');
        const std::string_view part = remaining.substr(0, separator);
        int value = 0;
        if (part.empty() || !utils::chars_to_number<int, true>(part, value) || value < 0) {
            return std::nullopt;
        }
        parts.emplace_back(value);
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }

    if (parts.size() == 2) {
        if (parts[1] >= 60) {
            return std::nullopt;
        }
        return parts[0] * 60 + parts[1];
    }
    if (parts.size() == 3) {
        if (parts[1] >= 60 || parts[2] >= 60) {
            return std::nullopt;
        }
        return parts[0] * 3600 + parts[1] * 60 + parts[2];
    }
    return std::nullopt;
}

inline TrainingActivity classify_training_activity(
    bool idle_detected,
    bool processing_detected,
    bool completed_detected,
    bool trainee_present,
    bool assistant_present,
    std::optional<int> mastery) noexcept
{
    if (completed_detected) {
        return TrainingActivity::Completed;
    }
    if (processing_detected) {
        return TrainingActivity::Processing;
    }
    if (trainee_present && assistant_present && mastery && (*mastery == 1 || *mastery == 2)) {
        return TrainingActivity::OccupiedFallback;
    }
    if (idle_detected) {
        return TrainingActivity::Idle;
    }
    return TrainingActivity::Unknown;
}

} // namespace asst::training
