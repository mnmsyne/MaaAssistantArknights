#pragma once
#include "Config/AbstractConfig.h"

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/AsstBattleDef.h"

namespace asst
{
// The fixed training-speed bonus (%) contributed by 控制中枢 training-skill assistants, independent of any
// specific assistant's own skill bonus.
inline constexpr int ControlCenterBonusPercent = 5;

struct TrainingAssistant
{
    std::string operator_id;
    int bonus = 0;
    double mood_cost_per_hour = 1.0;
    bool dynamic = false;
};

// Halving assistants (逻各斯/艾丽妮) do not speed up the stage they sit on. Seating one arms a one-shot
// reduction on the NEXT mastery stage of the trainee: that stage starts with its duration multiplied by
// (1 - trigger_bonus/100). Arming succeeds only while the running stage still has more than
// halving_trigger_minutes of remaining time under the post-seating efficiency, and it survives later
// assistant switches. role_bonus is unrelated to the arming: it is a plain running-stage training-speed
// bonus (+role_bonus/100) that applies only while the trainee role matches.
struct TrainingHalvingAssistant
{
    std::string operator_id;
    // Next-stage duration reduction in percent (50 halves the next mastery stage).
    int trigger_bonus = 50;
    // Running-stage speed bonus in percent, applied only when the trainee role matches.
    int role_bonus = 0;
    std::unordered_set<battle::Role> roles;
};

class TrainingConfig final : public MAA_NS::SingletonHolder<TrainingConfig>, public AbstractConfig
{
public:
    virtual ~TrainingConfig() override = default;

    bool ensure_loaded() noexcept;

    const std::array<int, 3>& mastery_base_minutes() const noexcept { return m_mastery_base_minutes; }

    // Remaining-time threshold in minutes, measured under the post-seating efficiency: seating a halving
    // assistant arms the next-stage reduction only while the running stage still has more time left than
    // this. See TrainingHalvingAssistant.
    int halving_trigger_minutes() const noexcept { return m_halving_trigger_minutes; }

    const std::vector<TrainingAssistant>& stage_assistants(battle::Role role, int mastery) const noexcept;

    const std::vector<TrainingHalvingAssistant>& halving_assistants() const noexcept { return m_halving_assistants; }

    const std::unordered_map<std::string, std::string>& control_center_training_skills() const noexcept
    {
        return m_control_center_training_skills;
    }

    std::optional<std::string_view> exclusion_reason(std::string_view operator_id) const noexcept;

protected:
    virtual bool parse(const json::value& json) override;

private:
    std::once_flag m_load_once;
    bool m_load_succeeded = false;
    std::array<int, 3> m_mastery_base_minutes { 8 * 60, 16 * 60, 24 * 60 };
    int m_halving_trigger_minutes = 300;
    std::unordered_map<battle::Role, std::array<std::vector<TrainingAssistant>, 3>> m_stage_assistants;
    std::vector<TrainingHalvingAssistant> m_halving_assistants;
    std::unordered_map<std::string, std::string> m_control_center_training_skills;
    std::unordered_map<std::string, std::string> m_excluded_operators;
};

inline static auto& Training = TrainingConfig::get_instance();

// Pure, game-interaction-free helpers shared by every feature operating on training.json data
// (基建换班-训练室协助优化 and 干员培养). Resolving configured operator ids to in-game display names
// requires BattleData, so these builders run lazily at task time rather than at parse time.
namespace training
{
// Static (non-dynamic) stage assistants of one operator sharing a display name merge into one group;
// a display name claimed by different operators is ambiguous and dropped with a warning.
struct StageAssistantGroup
{
    std::string operator_id;
    std::vector<TrainingAssistant> variants;
    int max_bonus = 0;
};

std::optional<std::pair<int, double>>
    match_stage_bonus(const std::vector<int>& percentages, const std::vector<TrainingAssistant>& variants);
std::unordered_map<std::string, StageAssistantGroup> stage_assistant_groups(battle::Role role, int mastery);
std::unordered_map<std::string, TrainingHalvingAssistant> halving_assistants_by_name();
} // namespace training
} // namespace asst
