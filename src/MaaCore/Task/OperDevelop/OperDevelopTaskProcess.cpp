#include "OperDevelopProcessImpl.h"

namespace asst
{
namespace
{
inline json::object stage_estimate_to_json(const training::StageEstimate& stage, std::string_view warning)
{
    return json::object {
        { "skill_index", stage.skill_index },
        { "from_mastery", stage.from_mastery },
        { "to_mastery", stage.to_mastery },
        { "base_minutes", stage.base_minutes },
        { "estimated_minutes", stage.estimated_minutes },
        { "plan_kind", stage.plan_kind },
        { "assistant_name", stage.assistant_name },
        { "assistant_bonus", stage.assistant_bonus },
        { "warning", std::string(warning) },
    };
}
}

OperDevelopTaskProcess::OperDevelopTaskProcess(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain) :
    AbstractTask(callback, inst, task_chain)
{
}

bool OperDevelopTaskProcessImpl::set_params(const json::value& params)
{
    LogTraceFunction;

    DevelopmentConfig config;
    auto action = params.find<std::string>("action");
    auto operator_id = params.find<std::string>("operator_id");
    auto skills = params.find<std::vector<SkillRequest>>("skills");
    if (!action || !operator_id || !skills || (*action != "preflight" && *action != "execute")) {
        return false;
    }
    config.action = *action;
    config.operator_id = *operator_id;
    config.skills = std::move(*skills);
    if (const auto training_mode = params.find<std::string>("training_mode")) {
        if (*training_mode == "halving") {
            config.training_mode = training::TrainingMode::Halving;
        }
        else if (*training_mode == "efficiency") {
            config.training_mode = training::TrainingMode::Efficiency;
        }
        else {
            return false;
        }
    }
    config.allow_consume = params.get("allow_consume", false);
    config.expected_mastery = params.get("expected_mastery", std::vector<ExpectedMastery> {});
    config.preflight_token = params.get("preflight_token", std::string {});
    config.expected_stage_plan = params.find<ExpectedStagePlan>("expected_stage_plan");

    const auto oper = BattleData.find_oper_by_id(config.operator_id);
    if (!oper || oper->rarity < 4 || config.skills.empty()) {
        return false;
    }
    const int max_skill_index = oper->rarity == 6 ? 3 : 2;
    std::unordered_set<int> indices;
    for (const auto& skill : config.skills) {
        if (skill.index < 1 || skill.index > max_skill_index || skill.target_mastery < 1 || skill.target_mastery > 3 ||
            !indices.emplace(skill.index).second) {
            return false;
        }
    }
    std::ranges::sort(config.skills, {}, &SkillRequest::index);
    if (config.action == "preflight" && config.allow_consume) {
        return false;
    }
    if (config.action == "execute" && (!config.allow_consume || config.expected_mastery.empty() ||
                                       config.preflight_token.empty() || !config.expected_stage_plan)) {
        return false;
    }

    set_config(std::move(config));
    return true;
}

// Picks the assistant for one stage from the scanned roster.
//
// Overall rule: every available assistant (halving ones included) competes in one ranking by verified bonus,
// ties broken by the panel's efficiency-sort order (roster scan order).
//   mode == Halving && next_mastery <= 2: prefer the best halving assistant; when none qualifies, fall back
//                                         to the overall best.
//   otherwise (mode == Efficiency, or the M3 stage of either mode): take the overall best.
static const training::AssistantInfo* pick_assistant(
    const std::vector<training::AssistantInfo>& assistants,
    training::TrainingMode mode,
    int next_mastery)
{
    const training::AssistantInfo* best = nullptr;
    const training::AssistantInfo* best_halving = nullptr;
    for (const auto& candidate : assistants) {
        if (candidate.bonus < 0) {
            continue;
        }
        const bool available =
            !(candidate.status.working && !candidate.status.selected) && candidate.status.mood > 0.05;
        if (!available) {
            continue;
        }
        if (!best || candidate.bonus > best->bonus) {
            best = &candidate;
        }
        if (candidate.is_halving && (!best_halving || candidate.bonus > best_halving->bonus)) {
            best_halving = &candidate;
        }
    }
    if (mode == training::TrainingMode::Halving && next_mastery <= 2 && best_halving) {
        return best_halving;
    }
    return best;
}

bool OperDevelopTaskProcessImpl::_run()
{
    if (!Training.ensure_loaded()) {
        return blocked(
            "training_config_load_failed",
            "训练配置加载失败，未执行任何游戏操作；请检查资源文件并重启 MAA。");
    }

    const bool ok = run_core();
    if (!ok && !need_exit()) {
        // A failed run can stop anywhere (material page, trainee list, a popup). Leaving the game there risks
        // the NEXT run's OperBoxBegin recovery chain cycling through screens for minutes. Best-effort settle:
        // confirm the leave-infrast popup when present, then press Return while it still matches, bounded,
        // ignoring errors.
        ProcessTask(*this, { "OperDevelopLeaveInfrastConfirm", "Return" })
            .set_retry_times(5)
            .set_ignore_error(true)
            .run();
    }
    return ok;
}

bool OperDevelopTaskProcessImpl::run_core()
{
    m_target_role = battle::Role::Unknown;
    m_operator_elite = 0;
    m_operator_role_filter_used = false;
    m_operator_navigation_failure.clear();
    m_skill_images.clear();
    m_mastery_button_rect.reset();
    m_material_confirm_rect.reset();
    m_last_material_validation = {};
    m_last_level_requirement.reset();
    m_assistants.clear();
    m_control_center_bonus = false;
    m_control_center_assistant.clear();
    m_inspected_training_operator_name.clear();
    m_inspected_training_skill_index = 0;
    m_inspected_current_mastery = -1;
    m_claimed_this_run = false;
    m_last_training_page_diagnostic.clear();

    const auto oper = BattleData.find_oper_by_id(m_config.operator_id);
    if (!oper) {
        return blocked("operator_not_found", "未在当前资源中找到指定干员。");
    }
    if (const auto exclusion_reason = Training.exclusion_reason(m_config.operator_id)) {
        return blocked(
            "oper_develop_excluded",
            "指定干员为集成战略特勤干员，只能通过其关联的集成战略培养，未执行任何游戏操作。",
            json::object {
                { "operator_id", m_config.operator_id },
                { "operator", oper->name },
                { "exclusion_reason", *exclusion_reason },
            });
    }
    m_target_role = oper->role;

    // ----- phase 0: control-center training bonus -----
    // Inspected FIRST, while the flow has no reason to return into infrastructure later: leaving the
    // infrastructure view afterwards triggers the leave-confirm popup, which navigate_to_operator's entry
    // cleanup already handles (it confirms the popup). The old order -- inspecting the control center from
    // inside the training-room flow and then trying to re-enter the training facility across the main
    // infrastructure grid -- depended on facility-template matching that does not hold on every base layout.
    if (!inspect_control_center_bonus()) {
        save_failure_image("control_center");
        return blocked("control_center_inspection_failed", "无法检查控制中枢训练加成，已终止且未消耗材料。");
    }

    // ----- phase A: operator detail page — read mastery dots & capture skill icons -----
    if (!navigate_to_operator(oper)) {
        if (!m_operator_navigation_failure.empty()) {
            return blocked("operator_navigation_failed", m_operator_navigation_failure);
        }
        return blocked("operator_not_owned", "未识别指定干员，已终止。");
    }
    if (m_operator_elite > 0 && m_operator_elite != 2) {
        return blocked("elite_requirement", "未达到精英 2，已终止。");
    }
    const bool roster_card_confirms_elite_two = m_operator_elite == 2;
    if (!locate_mastery_button()) {
        if (!recognize_skill_rank_7()) {
            save_failure_image("skill_rank");
            return blocked("skill_rank_requirement", "未识别技能等级 RANK 7，已终止。");
        }
        expand_skill_panel();
        if (!locate_mastery_button()) {
            save_failure_image("skill_panel");
            if (!roster_card_confirms_elite_two) {
                return blocked("elite_requirement", "未达到精英 2，已终止。");
            }
            return blocked("skill_panel_expand_failed", "未识别已展开的技能面板，已终止。");
        }
    }
    m_operator_elite = 2;

    std::unordered_map<int, int> current_mastery;
    const cv::Mat operator_detail = ctrler()->get_image();
    for (const auto& request : m_config.skills) {
        if (!capture_skill_image(operator_detail, request.index)) {
            save_failure_image("skill_icon");
            return blocked("skill_icon_capture_failed", "未识别目标技能图标，已终止。");
        }
        auto mastery = recognize_mastery(request.index);
        if (!mastery) {
            save_failure_image("mastery_marker");
            return blocked("mastery_recognition_failed", "未识别技能专精等级，已终止。");
        }
        current_mastery.emplace(request.index, *mastery);
    }

    const auto emit_operator_inspection = [&]() {
        json::value operator_info = basic_info_with_what("OperDevelopOperatorInspected");
        operator_info["details"] = json::object {
            { "operator", oper->name },
            { "elite", m_operator_elite },
            { "skill_rank", 7 },
            { "role_filter_used", m_operator_role_filter_used },
        };
        callback(AsstMsg::SubTaskExtraInfo, operator_info);
        for (const auto& request : m_config.skills) {
            json::value skill_info = basic_info_with_what("OperDevelopSkillInspected");
            skill_info["details"] = json::object {
                { "index", request.index },
                { "current_mastery", current_mastery.at(request.index) },
                { "target_mastery", request.target_mastery },
                { "needs_training", current_mastery.at(request.index) < request.target_mastery },
            };
            callback(AsstMsg::SubTaskExtraInfo, skill_info);
        }
    };

    if (std::ranges::all_of(m_config.skills, [&](const auto& request) {
            return current_mastery.at(request.index) >= request.target_mastery;
        })) {
        emit_operator_inspection();
        json::value info = basic_info_with_what("OperDevelopCompleted");
        info["details"] = json::object { { "message", "所选技能均已达到目标专精等级，无需消耗材料。" } };
        callback(AsstMsg::SubTaskExtraInfo, info);
        return true;
    }

    // The pending stage is fixed before entering the training room so that inspect_training_page runs with
    // truthful context (the m_inspected_* fields feed the status snapshot and the announcement path). The
    // all_of early-return above guarantees pending exists.
    const auto pending = std::ranges::find_if(m_config.skills, [&](const auto& request) {
        return current_mastery.at(request.index) < request.target_mastery;
    });
    const int next_mastery = current_mastery.at(pending->index) + 1;
    m_inspected_training_operator_name = oper->name;
    m_inspected_training_skill_index = pending->index;
    m_inspected_current_mastery = current_mastery.at(pending->index);

    // ----- phase B: enter the training room through the mastery entry and classify its state -----
    if (!click_mastery_button()) {
        save_failure_image("mastery_list");
        return blocked("mastery_entry_failed", "未识别技能专精入口，已终止。");
    }
    auto snapshot = inspect_training_page();
    if (!snapshot) {
        save_failure_image("training_status");
        return blocked(
            "training_status_recognition_failed",
            "未识别训练室当前状态，已终止。" + (m_last_training_page_diagnostic.empty()
                                                    ? std::string {}
                                                    : " [" + m_last_training_page_diagnostic + "]"));
    }
    emit_training_status(*snapshot, oper->name);

    if (snapshot->activity == training::TrainingActivity::Processing ||
        snapshot->activity == training::TrainingActivity::OccupiedFallback) {
        // In-progress training is reported and released untouched; MAA does not babysit stages.
        return true;
    }

    if (snapshot->activity == training::TrainingActivity::Completed) {
        // Only the auto-played completion announcement reaches this branch: the mastery-list page this flow
        // inspects never renders a static completion marker. Dismissing the modal is the whole claim; the
        // game itself auto-claims a finished-but-unclaimed stage the moment the skill's row is clicked
        // (measured on device: the next stage's material page opens directly), and the planning flow below
        // always performs that click before any assistant swap.
        if (!acknowledge_completion_announcement(snapshot->mastery)) {
            return blocked(
                "training_completion_claim_failed",
                "专精完成动画未稳定、完成等级不符或未能返回专精列表；未继续任何操作。");
        }
        m_claimed_this_run = true;
        if (snapshot->skill_index >= 1 && snapshot->skill_index <= 3) {
            // The announced level becomes the fresh current mastery for that skill; other skills keep their
            // dot-readings from phase A.
            current_mastery[snapshot->skill_index] =
                std::max(current_mastery[snapshot->skill_index], snapshot->mastery);
        }
        snapshot.reset();
    }

    // ----- phase C: plan the pending stage on the idle mastery list -----
    emit_operator_inspection();

    if (!m_claimed_this_run) {
        // A freshly started stage needs its trainee (re)confirmed; after a claim the trainee is still seated.
        const TraineeSelectionResult trainee_result = select_trainee(oper->name, oper->role);
        if (trainee_result != TraineeSelectionResult::Ready) {
            save_failure_image("trainee_selection");
            switch (trainee_result) {
            case TraineeSelectionResult::WorkingElsewhere:
                return blocked(
                    "trainee_working_elsewhere",
                    "指定干员正在其他设施工作，无法进驻训练室。请先将其换下，再重新预检。");
            case TraineeSelectionResult::PossiblyWorkingElsewhere:
                return blocked(
                    "trainee_possibly_working_elsewhere",
                    "训练室可选名单中未找到指定干员，同时检测到正在其他设施工作的干员。请先确认目标干员已停止工作，"
                    "再重新预检。");
            case TraineeSelectionResult::NotFound:
                return blocked(
                    "trainee_not_found",
                    "训练室可选名单中未找到指定干员。请确认干员未在其他设施工作，再重新预检。");
            default:
                return blocked("trainee_selection_failed", "无法在训练室内复核并选择指定干员，已终止。");
            }
        }
    }

    // Material page: single-frame read; any unreadable cell aborts before anything can be consumed.
    if (!choose_mastery_skill(pending->index)) {
        save_failure_image("material_page");
        return blocked(
            "material_page_failed",
            std::format("无法打开技能 S{} 的专精材料页面，已终止。", pending->index));
    }
    const bool materials_sufficient = read_material_page();
    json::value material_info = basic_info_with_what("OperDevelopMaterialInspected");
    material_info["details"] = json::object {
        { "index", pending->index },
        { "current_mastery", current_mastery.at(pending->index) },
        { "target_mastery", pending->target_mastery },
        { "materials_sufficient", materials_sufficient },
        { "material_data_complete", m_last_material_validation.complete() },
    };
    {
        static constexpr std::array<const char*, training::MaterialSlotMax> SlotRoles { "skill_book",
                                                                                        "material_1",
                                                                                        "material_2" };
        auto& requirements = material_info["details"]["requirements"].as_array();
        for (size_t slot = 0; slot < m_last_material_validation.frame.size(); ++slot) {
            const auto& cell = m_last_material_validation.frame.at(slot);
            json::object requirement_info {
                { "slot", slot },
                { "role", SlotRoles.at(slot) },
                { "recognized", cell.state == training::MaterialCellState::Parsed },
            };
            if (cell.state == training::MaterialCellState::Parsed) {
                requirement_info["owned"] = cell.value.owned;
                requirement_info["required"] = cell.value.required;
                requirement_info["sufficient"] = cell.value.sufficient();
            }
            requirements.emplace_back(std::move(requirement_info));
        }
        json::object level_info { { "recognized", m_last_level_requirement.has_value() } };
        if (m_last_level_requirement) {
            level_info["owned"] = m_last_level_requirement->owned;
            level_info["required"] = m_last_level_requirement->required;
            level_info["sufficient"] = m_last_level_requirement->sufficient();
        }
        material_info["details"]["level_requirement"] = std::move(level_info);
    }
    callback(AsstMsg::SubTaskExtraInfo, material_info);
    if (!ProcessTask(*this, { "Return" }).run() || need_exit() ||
        !ProcessTask(*this, { "OperDevelopMasteryListFlag" }).set_retry_times(2).run()) {
        return false;
    }
    if (!materials_sufficient) {
        return blocked(
            m_last_material_validation.complete() ? "materials_insufficient" : "material_recognition_incomplete",
            m_last_material_validation.complete()
                ? "当前待专精阶段材料不足，已终止。"
                : "当前待专精阶段材料数量无法完整识别，为避免误消耗已终止。请在游戏内人工核对材料后重试。");
    }

    std::optional<std::string_view> verify_only;
    if (m_config.action == "execute" && m_config.expected_stage_plan) {
        verify_only = m_config.expected_stage_plan->assistant_name;
    }
    if (!inspect_assistants(verify_only)) {
        return blocked("assistant_inspection_failed", "无法检查训练室协助干员及基建技能，已终止预检。");
    }
    // The control-center bonus was inspected in phase 0, before the flow ever left the infrastructure view,
    // so no return trip is needed here; the device is still on the training-room mastery page.

    // Build the single-stage estimate with the simplified pick rule.
    const int control_center_bonus = m_control_center_bonus ? ControlCenterBonusPercent : 0;
    const auto* picked = pick_assistant(m_assistants, m_config.training_mode, next_mastery);
    if (!picked) {
        return blocked("assistant_plan_failed", "未找到当前阶段可用的协助干员，未消耗材料。");
    }
    StageEstimate stage;
    stage.skill_index = pending->index;
    stage.from_mastery = current_mastery.at(pending->index);
    stage.to_mastery = next_mastery;
    stage.base_minutes = Training.mastery_base_minutes().at(static_cast<size_t>(next_mastery - 1));
    stage.plan_kind =
        picked->is_halving && m_config.training_mode == training::TrainingMode::Halving ? "halving" : "efficiency";
    stage.assistant_name = picked->name;
    stage.assistant_bonus = picked->bonus;
    // A halving pick may carry bonus == -1 (its value is the role-agnostic halving trigger, not a static
    // efficiency bonus); estimate and display treat that as "no static bonus" instead of a -1% penalty.
    const int display_bonus = std::max(0, picked->bonus);
    stage.assistant_bonus = display_bonus;
    stage.estimated_minutes = training::estimate_stage_minutes(stage.base_minutes, control_center_bonus, display_bonus);
    stage.warning = picked->is_halving && stage.plan_kind != "halving"
                        ? "减半干员按总排序参与竞争，本阶段由更高加成的协助干员胜出。"
                    : stage.plan_kind == "halving" ? "本阶段由减半协助干员开始；减半效果触发后实际耗时会更短。"
                                                   : "";
    const std::string token = training::make_preflight_token(
        m_config,
        control_center_bonus,
        m_control_center_assistant,
        current_mastery,
        &stage);

    if (m_config.action == "preflight") {
        json::value info = basic_info_with_what("OperDevelopPreflight");
        auto& details = info["details"];
        details["eligible"] = true;
        details["materials_sufficient"] = true;
        details["estimated_minutes"] = stage.estimated_minutes;
        details["preflight_token"] = token;
        details["control_center_bonus"] = control_center_bonus;
        details["control_center_assistant"] = m_control_center_assistant;
        details["pending_skill_index"] = pending->index;
        details["operator"] = oper->name;
        details["elite"] = m_operator_elite;
        details["message"] = std::format(
            "预检通过：技能 S{} 将从 M{} 专精到 M{}，协助干员 {}（+{}%），预计约 {} 分钟（未计减半效果）。",
            pending->index,
            stage.from_mastery,
            stage.to_mastery,
            stage.assistant_name,
            stage.assistant_bonus,
            stage.estimated_minutes);
        auto& skills = details["skills"].as_array();
        for (const auto& request : m_config.skills) {
            skills.emplace_back(
                json::object {
                    { "index", request.index },
                    { "current_mastery", current_mastery.at(request.index) },
                    { "target_mastery", request.target_mastery },
                    { "needs_training", current_mastery.at(request.index) < request.target_mastery },
                });
        }
        details["current_stage_plan"] = stage_estimate_to_json(stage, stage.warning);
        callback(AsstMsg::SubTaskExtraInfo, info);
        return true;
    }

    // ----- execute: re-verify against the preflight snapshot, then start the stage -----
    if (m_config.preflight_token != token || !training::expected_state_matches(m_config, current_mastery) ||
        !m_config.expected_stage_plan || !training::stage_matches_expected(stage, *m_config.expected_stage_plan)) {
        return blocked("state_changed", "现场专精状态或协助方案与预检快照不一致，请重新预检；未消耗材料。");
    }

    if (!select_assistant_by_name(stage.assistant_name)) {
        return blocked("assistant_selection_failed", "无法选择当前方案指定的协助干员，未消耗材料。");
    }
    if (!choose_mastery_skill(pending->index) ||
        !ProcessTask(*this, { "OperDevelopCostFlag" }).set_retry_times(2).run()) {
        return blocked("stage_recheck_failed", "开始当前阶段前的材料页面复核失败，未消耗材料。");
    }
    if (!read_material_page()) {
        return blocked(
            m_last_material_validation.complete() ? "materials_insufficient" : "material_recognition_incomplete",
            m_last_material_validation.complete() ? "当前待专精阶段材料不足，已终止。"
                                                  : "当前待专精阶段材料数量无法完整识别，为避免误消耗已终止。");
    }
    if (!click_material_confirm()) {
        return blocked("stage_start_failed", "未能确认开始训练，任务已终止。");
    }

    json::value info = basic_info_with_what("OperDevelopStageStarted");
    info["details"] = json::object {
        { "message",
          std::format(
              "已开始技能 {} 的专精 {} "
              "阶段，协助干员：{}。本次只启动当前阶段，任务随即释放；如需继续，可重新执行预检。",
              pending->index,
              next_mastery,
              stage.assistant_name) },
        { "skill_index", pending->index },
        { "target_mastery", next_mastery },
        { "assistant", stage.assistant_name },
        { "base_minutes", stage.base_minutes },
        { "estimated_minutes", stage.estimated_minutes },
        { "plan_kind", stage.plan_kind },
    };
    callback(AsstMsg::SubTaskExtraInfo, info);
    return true;
}

bool OperDevelopTaskProcessImpl::blocked(std::string_view reason, std::string_view message, json::object details_extra)
{
    if (need_exit()) {
        return false;
    }
    json::value info = basic_info_with_what("OperDevelopBlocked");
    info["details"] = json::object {
        { "reason", reason },
        { "message", message },
    };
    for (const auto& [key, value] : details_extra) {
        info["details"][key] = value;
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
    return false;
}

void OperDevelopTaskProcessImpl::save_failure_image(std::string_view suffix)
{
    utils::save_debug_image(
        ctrler()->get_image(),
        utils::path("debug") / "oper_develop",
        true,
        "operator development recognition failure",
        suffix);
}

// Debug-only recognition dumps taken during normal flow -- e.g. the selected assistant/trainee list, a
// training-page snapshot. Compiled out entirely in non-debug builds, unlike save_failure_image above, which
// stays unconditional because failure diagnostics are useful everywhere.
void OperDevelopTaskProcessImpl::save_develop_debug_image([[maybe_unused]] std::string_view suffix)
{
#ifdef ASST_DEBUG
    utils::save_debug_image(
        ctrler()->get_image(),
        utils::path("debug") / "oper_develop",
        true,
        "operator development debug snapshot",
        suffix);
#endif
}

std::shared_ptr<OperDevelopTaskProcess>
    make_oper_develop_task_process(const AsstCallback& callback, Assistant* inst, std::string_view task_chain)
{
    return std::make_shared<OperDevelopTaskProcessImpl>(callback, inst, task_chain);
}

} // namespace asst
