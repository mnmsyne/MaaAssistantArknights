#include "OperDevelopProcessImpl.h"

namespace asst
{

bool OperDevelopTaskProcessImpl::on_training_assistant_page()
{
    return ProcessTask(*this, { "InfrastTrainingAssistantFlag" }).set_retry_times(2).run();
}

bool OperDevelopTaskProcessImpl::inspect_assistants(std::optional<std::string_view> verify_only)
{
    TrainingAssistantPanel panel(*this);
    using AssistantStepResult = TrainingAssistantPanel::AssistantStepResult;
    using OperListEntry = TrainingAssistantPanel::OperListEntry;

    m_assistants.clear();

    const int next_mastery =
        m_inspected_current_mastery >= 1 && m_inspected_current_mastery <= 2 ? m_inspected_current_mastery + 1 : -1;
    const auto stage_configs = training::stage_assistant_groups(m_target_role, next_mastery);
    const auto halving_configs = training::halving_assistants_by_name();
    if (stage_configs.empty() && halving_configs.empty()) {
        Log.warn(__FUNCTION__, "no configured assistant candidates");
        return false;
    }

    if (!on_training_assistant_page() || !panel.open_training_assistant_panel(4)) {
        return false;
    }

    std::string trainee_name = m_inspected_training_operator_name;
    if (trainee_name.empty()) {
        const auto trainee = BattleData.find_oper_by_id(m_config.operator_id);
        trainee_name = trainee ? trainee->name : std::string {};
    }
    const auto available = [&](const OperListEntry& entry) {
        return entry.name != trainee_name && (!entry.working || entry.selected) && entry.mood > 0.05;
    };

    // The scan only serves the configured candidate list: role-boosting assistants for the pending stage plus
    // the halving assistants. Every candidate is settled exactly once -- verified into m_assistants, or
    // rejected as unavailable / pruned / unverifiable -- and once the whole list is settled the remaining
    // pages hold nothing relevant, so the visitor stops the scan instead of paging through the full roster.
    std::vector<std::string> pending_names;
    pending_names.reserve(stage_configs.size() + halving_configs.size());
    for (const auto& [name, group] : stage_configs) {
        pending_names.emplace_back(name);
    }
    for (const auto& [name, config] : halving_configs) {
        pending_names.emplace_back(name);
    }
    // In verify-only mode (execute re-check) the preset assistant is the only thing to look for, so the first
    // settled candidate ends the scan.
    const auto settle = [&](const std::string& name) {
        std::erase(pending_names, name);
        return verify_only || pending_names.empty();
    };
    // Pruning state, mirroring the 基建换班 training-assistant flow: in halving mode for M1/M2 a verified
    // halving assistant beats every non-halving one by pick rule; otherwise a stage candidate whose configured
    // maximum cannot reach the best verified bonus is not worth a read (ties keep the first).
    int best_bonus = -1;
    bool halving_verified = false;
    const bool halving_beats_static = m_config.training_mode == training::TrainingMode::Halving && next_mastery == 2;

    const AssistantStepResult scan_result =
        panel.scan_oper_list_pages(MaxAssistantScanPages, [&](const OperListEntry& snapshot) {
            if (verify_only && snapshot.name != *verify_only) {
                return true;
            }
            const auto stage_config = stage_configs.find(snapshot.name);
            const auto halving_config = halving_configs.find(snapshot.name);
            if (stage_config == stage_configs.end() && halving_config == halving_configs.end()) {
                return true;
            }
            if (!available(snapshot)) {
                Log.info(
                    __FUNCTION__,
                    "assistant candidate unavailable",
                    snapshot.name,
                    snapshot.working,
                    snapshot.mood);
                return settle(snapshot.name);
            }
            // A halving-only candidate needs no panel read: its trigger and role bonus are config data (same
            // as the 基建换班 M2 flow), so the entry is recorded straight from the snapshot.
            if (halving_config != halving_configs.end() && stage_config == stage_configs.end()) {
                const int role_bonus =
                    m_target_role != battle::Role::Unknown && halving_config->second.roles.contains(m_target_role)
                        ? halving_config->second.role_bonus
                        : 0;
                m_assistants.emplace_back(
                    AssistantInfo {
                        .name = snapshot.name,
                        .bonus = role_bonus,
                        .is_halving = true,
                        .status =
                            AssistantStatus {
                                .mood = snapshot.mood,
                                .working = snapshot.working,
                                .selected = snapshot.selected,
                            },
                    });
                halving_verified = true;
                best_bonus = std::max(best_bonus, role_bonus);
                Log.info(__FUNCTION__, "verified assistant candidate", snapshot.name, role_bonus, true);
                return settle(snapshot.name);
            }
            if (halving_beats_static && halving_verified && halving_config == halving_configs.end()) {
                Log.info(__FUNCTION__, "non-halving candidate pruned by a verified halving assistant", snapshot.name);
                return settle(snapshot.name);
            }
            if (stage_config != stage_configs.end() && best_bonus >= stage_config->second.max_bonus) {
                Log.info(
                    __FUNCTION__,
                    "stage candidate pruned:",
                    snapshot.name,
                    "max bonus:",
                    stage_config->second.max_bonus,
                    "best:",
                    best_bonus);
                return settle(snapshot.name);
            }

            if (!panel.ensure_assistant_entry_selected(snapshot, available)) {
                Log.info(__FUNCTION__, "assistant candidate selection was not verified", snapshot.name);
                return settle(snapshot.name);
            }

            const auto observation = TrainingAssistantPanel::read_stable_observation<std::pair<int, bool>>(
                3,
                std::chrono::milliseconds(250),
                [&]() -> std::optional<std::pair<int, bool>> {
                    const auto percentages = panel.recognize_oper_list_percentages("InfrastTrainingAssistantSkillArea");
                    int static_bonus = -1;
                    if (stage_config != stage_configs.end()) {
                        if (const auto matched =
                                training::match_stage_bonus(percentages, stage_config->second.variants)) {
                            static_bonus = matched->first;
                        }
                    }
                    const bool halving_confirmed =
                        halving_config != halving_configs.end() &&
                        std::ranges::find(percentages, halving_config->second.trigger_bonus) != percentages.end();
                    return std::pair { static_bonus, halving_confirmed };
                },
                [this] { return need_exit(); });
            if (!observation || (observation->first < 0 && !observation->second)) {
                Log.info(__FUNCTION__, "assistant candidate bonus was not verified", snapshot.name);
                return settle(snapshot.name);
            }

            int bonus = observation->first;
            if (observation->second) {
                const int role_bonus =
                    m_target_role != battle::Role::Unknown && halving_config->second.roles.contains(m_target_role)
                        ? halving_config->second.role_bonus
                        : 0;
                bonus = std::max(bonus, role_bonus);
            }
            halving_verified = halving_verified || observation->second;
            best_bonus = std::max(best_bonus, bonus);
            m_assistants.emplace_back(
                AssistantInfo {
                    .name = snapshot.name,
                    .bonus = bonus,
                    .is_halving = observation->second,
                    .status =
                        AssistantStatus {
                            .mood = snapshot.mood,
                            .working = snapshot.working,
                            .selected = snapshot.selected,
                        },
                });
            Log.info(__FUNCTION__, "verified assistant candidate", snapshot.name, bonus, observation->second);
            return settle(snapshot.name);
        });

    if (need_exit()) {
        return false;
    }
    const bool closed = panel.return_to_training_page(1);
    Log.info(__FUNCTION__, "assistant roster size", m_assistants.size());
    return closed && scan_result == AssistantStepResult::Completed && !m_assistants.empty();
}

std::optional<std::string> OperDevelopTaskProcessImpl::select_assistant_by_name(std::string_view target_name)
{
    TrainingAssistantPanel panel(*this);

    if (!on_training_assistant_page() || !panel.open_training_assistant_panel(4)) {
        Log.warn(__FUNCTION__, "failed to open assistant panel");
        return std::nullopt;
    }

    const auto available = [&](const TrainingAssistantPanel::OperListEntry& entry) {
        return entry.name != m_inspected_training_operator_name && (!entry.working || entry.selected) &&
               entry.mood > 0.05;
    };
    const auto outcome = panel.select_and_confirm(target_name, available, MaxAssistantScanPages);
    if (need_exit()) {
        return std::nullopt;
    }
    if (outcome.panel_open) {
        // 面板仍开着（未找到/不可用/选择未生效/确认未关闭面板）：受验证关闭
        panel.return_to_training_page(1);
    }
    if (!on_training_assistant_page()) {
        return std::nullopt;
    }
    using AssistantSelectResult = TrainingAssistantPanel::AssistantSelectResult;
    return outcome.result == AssistantSelectResult::Confirmed ||
                   outcome.result == AssistantSelectResult::AlreadySelected
               ? std::optional(std::string(target_name))
               : std::nullopt;
}

bool OperDevelopTaskProcessImpl::inspect_control_center_bonus()
{
    m_control_center_bonus = false;
    m_control_center_assistant.clear();
    if (!ProcessTask(*this, { "InfrastBegin" }).set_retry_times(0).run()) {
        return false;
    }

    using Clock = std::chrono::steady_clock;
    // Wall-clock fallback only: entry itself is still gated on two stable control-rect frames below.
    constexpr auto EnterTimeout = std::chrono::seconds(60);
    constexpr unsigned int ObserveIntervalMs = 250;
    const auto facility_post_delay = std::chrono::milliseconds(Task.get("InfrastEnterFacility")->post_delay);
    const auto deadline = Clock::now() + EnterTimeout;
    auto next_click_time = Clock::now() + std::chrono::milliseconds(750);
    std::optional<Rect> previous_control;
    int stable_control_frames = 0;
    bool entered = false;

    while (!need_exit() && Clock::now() < deadline) {
        const cv::Mat image = ctrler()->get_image();

        // The infrastructure overview grid must be handled before the garrison keywords: its dorm tiles also
        // render 休息中/空闲中 labels inside the InfrastEnterOperList roi, which would false-match there and
        // leave the analyzer below staring at the overview (observed on device when a previous run left the
        // game on the overview). Clicking the Control tile leads to the facility page, where InfrastStationedInfo
        // takes over on the following iterations.
        InfrastFacilityImageAnalyzer facility(image);
        facility.set_to_be_analyzed({ "Control" });
        const Rect control = facility.analyze() ? facility.get_rect("Control", 0) : Rect {};
        if (!control.empty()) {
            const bool stable = previous_control && std::abs(previous_control->x - control.x) <= 12 &&
                                std::abs(previous_control->y - control.y) <= 12 &&
                                std::abs(previous_control->width - control.width) <= 12 &&
                                std::abs(previous_control->height - control.height) <= 12;
            stable_control_frames = stable ? stable_control_frames + 1 : 1;
            previous_control = control;

            const auto now = Clock::now();
            if (stable_control_frames >= 2 && now >= next_click_time) {
                Log.info(__FUNCTION__, "clicking stable control center rectangle", control);
                ctrler()->click(control);
                next_click_time = now + facility_post_delay;
                previous_control.reset();
                stable_control_frames = 0;
            }
            sleep(ObserveIntervalMs);
            continue;
        }
        previous_control.reset();
        stable_control_frames = 0;

        // Facility interior / garrison pages: no facility grid here. 进驻信息 opens the garrison list; the
        // garrison keywords confirm it is already open.
        const auto matches_ocr_task = [&](std::string_view task_name) {
            OCRer analyzer(image);
            analyzer.set_task_info(std::string(task_name));
            return analyzer.analyze().has_value();
        };
        if (matches_ocr_task("InfrastEnterOperList") || matches_ocr_task("InfrastStationedInfo")) {
            ProcessTask enter_oper_list(*this, { "InfrastEnterOperList", "InfrastStationedInfo" });
            enter_oper_list.set_retry_times(0).set_ignore_error(true);
            if (enter_oper_list.run()) {
                entered = true;
                break;
            }
        }
        sleep(ObserveIntervalMs);
    }
    if (!entered) {
        Log.warn(__FUNCTION__, "timed out waiting for the control center operator list");
        return false;
    }

    // Only the currently garrisoned assistants matter here (below immediately skips any !selected card), and
    // the control center has just 5 garrison slots that are all visible on the facility's own view without
    // paging through the full operator roster.
    const auto& control_training_skills = Training.control_center_training_skills();
    InfrastOperImageAnalyzer analyzer(ctrler()->get_image());
    analyzer.set_facility("Control");
    analyzer.set_to_be_calced(
        InfrastOperImageAnalyzer::ToBeCalced::Selected | InfrastOperImageAnalyzer::ToBeCalced::Skill);
    const bool scan_succeeded = analyzer.analyze() && !analyzer.get_result().empty();
    if (scan_succeeded) {
        analyzer.sort_by_loc();
        for (const auto& item : analyzer.get_result()) {
            if (!item.selected) {
                continue;
            }
            for (const auto& skill : item.skills) {
                const auto recognized = control_training_skills.find(skill.id);
                if (recognized != control_training_skills.end()) {
                    m_control_center_bonus = true;
                    const auto oper = BattleData.find_oper_by_id(recognized->second);
                    m_control_center_assistant = oper ? oper->name : recognized->second;
                    break;
                }
            }
            if (m_control_center_bonus) {
                break;
            }
        }
    }
    ProcessTask(*this, { "Return" }).run();
    Log.info(__FUNCTION__, "control center training bonus", m_control_center_bonus, m_control_center_assistant);
    return !need_exit() && scan_succeeded;
}

} // namespace asst
