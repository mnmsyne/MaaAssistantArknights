#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/TrainingConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/AbstractTask.h"
#include "Task/Infrast/TrainingAssistantPanel.h"
#include "Task/ProcessTask.h"
#include "Utils/DebugImageHelper.hpp"
#include "Utils/Logger.hpp"
#include "Vision/FeatureMatcher.h"
#include "Vision/Infrast/InfrastFacilityImageAnalyzer.h"
#include "Vision/Infrast/InfrastOperImageAnalyzer.h"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/OperBoxImageAnalyzer.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

#include "OperDevelopTypes.h"
#include "TrainingAssistants.h"
#include "TrainingRecognition.h"

#include "OperDevelopTaskProcess.h"

namespace asst
{
using training::AssistantInfo;
using training::AssistantStatus;
using training::DevelopmentConfig;
using training::ExpectedMastery;
using training::ExpectedStagePlan;
using training::MaterialRequirement;
using training::MaterialValidation;
using training::MaxAssistantScanPages;
using training::SkillRequest;
using training::StageEstimate;
using training::TraineeSelectionResult;
using training::TrainingRoomSnapshot;

// Implementation of the OperDevelop (干员培养) task chain. The class declaration lives in this private
// header so the member-function definitions can be spread across the sibling translation units
// (task-process orchestration / training room / assistants / operator page) without exposing the internals
// outside Task/OperDevelop/.
class OperDevelopTaskProcessImpl final : public OperDevelopTaskProcess
{
public:
    using OperDevelopTaskProcess::OperDevelopTaskProcess;
    virtual ~OperDevelopTaskProcessImpl() override = default;

    void set_config(DevelopmentConfig config) { m_config = std::move(config); }

    bool set_params(const json::value& params) override;

protected:
    virtual bool _run() override;

private:
    bool run_core();

    struct SelectedTrainingSlot
    {
        std::string name;
        std::vector<int> percentages;
    };

    // ----- training room inspection & completion announcement -----
    std::optional<SelectedTrainingSlot> inspect_selected_training_slot(bool assistant);
    int resolve_live_assistant_bonus(std::string_view name, const std::vector<int>& percentages, int mastery) const;
    std::optional<std::string> recognize_training_countdown(int skill_index);
    std::optional<training::TrainingRoomSnapshot> inspect_training_page();
    bool wait_for_completion_animation(int expected_mastery);
    bool acknowledge_completion_announcement(int expected_rank);
    void emit_training_status(const training::TrainingRoomSnapshot& snapshot, std::string_view fallback_operator);

    // ----- operator page inspection & trainee selection -----
    TraineeSelectionResult select_trainee(std::string_view target_name, battle::Role target_role);

    // ----- assistant roster management -----
    bool inspect_assistants(std::optional<std::string_view> verify_only = std::nullopt);
    bool on_training_assistant_page();
    std::optional<std::string> select_assistant_by_name(std::string_view target_name);
    bool inspect_control_center_bonus();

    // ----- navigation & operator/skill/mastery page operations -----
    bool navigate_to_operator(const std::shared_ptr<battle::OperProps>& oper);
    bool expand_skill_panel();
    bool select_operator_role(battle::Role role);
    bool close_operator_role_filter();
    bool locate_mastery_button();
    bool click_mastery_button();
    bool recognize_skill_rank_7(const cv::Mat& image = cv::Mat());
    bool capture_skill_image(const cv::Mat& image, int skill_index);
    bool choose_mastery_skill(int skill_index);
    bool recover_to_mastery_list(std::string_view reason, int skill_index);
    std::optional<int> recognize_mastery(int skill_index);
    bool read_material_page();
    bool click_material_confirm();

    // ----- orchestration helpers -----
    void settle_leave_infrast(int rounds, int return_retry_times = 0, int interval_ms = 300);
    bool blocked(std::string_view reason, std::string_view message, json::object details_extra = {});
    void save_failure_image(std::string_view suffix);
    void save_develop_debug_image([[maybe_unused]] std::string_view suffix);

    DevelopmentConfig m_config;
    battle::Role m_target_role = battle::Role::Unknown;
    int m_operator_elite = 0;
    bool m_operator_role_filter_used = false;
    std::string m_operator_navigation_failure;
    std::unordered_map<int, cv::Mat> m_skill_images;
    std::optional<Rect> m_mastery_button_rect;
    std::optional<Rect> m_material_confirm_rect;
    training::MaterialValidation m_last_material_validation;
    std::optional<training::MaterialRequirement> m_last_level_requirement;
    std::vector<training::AssistantInfo> m_assistants;
    bool m_control_center_bonus = false;
    std::string m_control_center_assistant;
    std::string m_inspected_training_operator_name;
    int m_inspected_training_skill_index = 0;
    int m_inspected_current_mastery = -1;
    // Whether this run already claimed a finished stage (so the seated trainee must not be re-selected).
    bool m_claimed_this_run = false;
    // Best-effort human-readable snapshot of what inspect_training_page actually observed on its last
    // unclassified frame, surfaced via blocked() so a failure is diagnosable from asst.log alone.
    std::string m_last_training_page_diagnostic;
};

std::shared_ptr<OperDevelopTaskProcess>
    make_oper_develop_task_process(const AsstCallback& callback, Assistant* inst, std::string_view task_chain);
} // namespace asst
