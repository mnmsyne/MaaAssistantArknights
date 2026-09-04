#include "InfrastTrainingTask.h"

#include "TrainingAssistantPanel.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/TrainingConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/BestMatcher.h"
#include "Vision/FeatureMatcher.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

namespace
{
constexpr int MaxAssistantPages = 5;

struct VerifiedAssistant
{
    std::string name;
    std::string operator_id;
    int bonus = -1;
    double mood_cost = 1.0;
    bool original = false;
};

bool better_assistant(const VerifiedAssistant& lhs, const VerifiedAssistant& rhs)
{
    if (lhs.bonus != rhs.bonus) {
        return lhs.bonus > rhs.bonus;
    }
    if (lhs.original != rhs.original) {
        return lhs.original;
    }
    if (std::abs(lhs.mood_cost - rhs.mood_cost) > 1e-9) {
        return lhs.mood_cost < rhs.mood_cost;
    }
    return lhs.operator_id < rhs.operator_id;
}
} // namespace

bool asst::InfrastTrainingTask::_run()
{
    m_all_available_opers.clear();

    set_product("SkillLevel");

    swipe_to_the_left_of_main_ui();

    if (!enter_facility()) {
        swipe_to_right_of_main_ui();
        if (!enter_facility()) {
            return false;
        }
    }

    auto status = analyze_status();
    if (!status) {
        return false;
    }

    if (m_continue_training && *status == TrainingStatus::Completed && m_level != 3) { // 继续训练
        click_bottom_left_tab();
        FeatureMatcher choose_skill_analyzer(ctrler()->get_image());
        choose_skill_analyzer.set_task_info("InfrastTrainingChooseSkillRec");
        choose_skill_analyzer.set_templ(m_skill_img);
        if (!choose_skill_analyzer.analyze()) {
            Log.error(__FUNCTION__, "choose skill failed");
            return false;
        }

        // Assistant switching is only a best-effort optimization after the next mastery has started.
        if (continue_train(skill_index_from_rect(choose_skill_analyzer.get_result().front().rect))) {
            if (!optimize_assistant_for_next_level()) {
                return false;
            }
        }
        else {
            Log.warn(__FUNCTION__, "failed to continue mastery training, skip assistant optimization");
        }
    }

    return true;
}

std::optional<asst::InfrastTrainingTask::TrainingStatus> asst::InfrastTrainingTask::analyze_status()
{
    const auto& image = ctrler()->get_image();
    RegionOCRer idle_analyzer(image);
    idle_analyzer.set_task_info("InfrastTrainingIdle");
    if (idle_analyzer.analyze()) {
        json::value cb_info = basic_info_with_what("InfrastTrainingIdle");
        callback(AsstMsg::SubTaskExtraInfo, cb_info);
        return TrainingStatus::Idle;
    }

    {
        const auto& replace_map = Task.get<OcrTaskInfo>("CharsNameOcrReplace")->replace_map;
        std::vector<std::pair<std::string, std::string>> task_replace =
            Task.get<OcrTaskInfo>("InfrastTrainingOperatorAndSkill")->replace_map;
        std::ranges::copy(replace_map, std::back_inserter(task_replace));
        RegionOCRer name_analyzer(image);
        name_analyzer.set_task_info("InfrastTrainingOperatorAndSkill");
        name_analyzer.set_replace(task_replace);
        name_analyzer.set_use_raw(true);
        if (!name_analyzer.analyze()) {
            Log.error(__FUNCTION__, "operator name recognition failed");
            return std::nullopt;
        }

        std::string name_str = name_analyzer.get_result().text;
        size_t separation_pos = name_str.find('\n');
        if (separation_pos == std::string::npos) {
            Log.error(__FUNCTION__, "separate string failed");
            return std::nullopt;
        }

        // '\n'前为干员名，'\n'后为技能名
        m_operator_name = name_str.substr(0, separation_pos);
    }

    {
        RegionOCRer skill_analyzer(image);
        skill_analyzer.set_task_info("InfrastTrainingOperatorAndSkill");
        skill_analyzer.set_use_raw(true);
        if (!skill_analyzer.analyze()) {
            Log.error(__FUNCTION__, "skill name recognition failed");
            return std::nullopt;
        }

        std::string skill_str = skill_analyzer.get_result().text;
        size_t separation_pos = skill_str.find('\n');
        if (separation_pos == std::string::npos) {
            Log.error(__FUNCTION__, "separate string failed");
            return std::nullopt;
        }

        m_skill_name = skill_str.substr(separation_pos + 1);
    }

    Rect roi = Task.get("InfrastTrainingSkillImg")->roi;
    m_skill_img = image(make_rect<cv::Rect>(roi));

    if (!level_analyze(image)) {
        Log.error(__FUNCTION__, "analyze level failed");
        return std::nullopt;
    }

    if (training_completed()) {
        json::value cb_info = basic_info_with_what("InfrastTrainingCompleted");
        cb_info["details"] = json::object {
            { "operator", m_operator_name },
            { "skill", m_skill_name },
            { "level", m_level },
        };
        callback(AsstMsg::SubTaskExtraInfo, cb_info);
        return TrainingStatus::Completed;
    }

    const auto& time_opt = time_left_analyze(image);
    if (!time_opt) {
        return std::nullopt;
    }

    json::value info = basic_info_with_what("InfrastTrainingTimeLeft");
    info["details"] = json::object {
        { "operator", m_operator_name },
        { "skill", m_skill_name },
        { "level", m_level },
        { "time", *time_opt },
    };
    callback(AsstMsg::SubTaskExtraInfo, info);

    return TrainingStatus::Processing;
}

bool asst::InfrastTrainingTask::level_analyze(const cv::Mat& image)
{
    const std::string task_name = "InfrastTrainingLevel";

    BestMatcher analyzer(image);
    analyzer.set_task_info(task_name);
    for (int i = 1; i <= 3; ++i) {
        std::string level_temp_name = task_name + std::to_string(i) + ".png";
        analyzer.append_templ(level_temp_name);
    }
    if (!analyzer.analyze()) {
        return false;
    }
    const auto& res = analyzer.get_result();
    utils::chars_to_number(res.templ_info.name.substr(task_name.size(), 1), m_level);
    Log.info(__FUNCTION__, "level has been set to ", m_level);

    return true;
}

bool asst::InfrastTrainingTask::training_completed()
{
    return ProcessTask(*this, { "InfrastTrainingCompleted" }).run();
}

std::optional<std::string> asst::InfrastTrainingTask::time_left_analyze(const cv::Mat& image)
{
    LogTraceFunction;
    RegionOCRer analyzer(image);
    analyzer.set_task_info("InfrastTrainingTime");
    analyzer.set_use_raw(true);
    if (!analyzer.analyze()) {
        return std::nullopt;
    }
    const auto& text = analyzer.get_result().text;
    if (text.empty() || text.find(":") == std::string::npos) {
        Log.error(__FUNCTION__, "time left analyze failed");
        return std::nullopt;
    }
    return text;
}

asst::InfrastTrainingTask& asst::InfrastTrainingTask::set_continue_training(bool continue_training) noexcept
{
    m_continue_training = continue_training;
    return *this;
}

bool asst::InfrastTrainingTask::continue_train(int index)
{
    static const std::vector<std::string> continue_train_task = { "InfrastTrainingContinue1",
                                                                  "InfrastTrainingContinue2",
                                                                  "InfrastTrainingContinue3" };
    return ProcessTask { *this, { continue_train_task[index - 1] } }.run();
}

bool asst::InfrastTrainingTask::optimize_assistant_for_next_level()
{
    TrainingAssistantPanel panel(*this);
    using AssistantSelectResult = TrainingAssistantPanel::AssistantSelectResult;
    using AssistantStepResult = TrainingAssistantPanel::AssistantStepResult;
    using OperListEntry = TrainingAssistantPanel::OperListEntry;

    LogTraceFunction;
    const int next_mastery = m_level + 1;
    bool panel_open = false;
    bool confirmation_submitted = false;
    const auto finish = [&](bool switched, std::string assistant = {}) {
        if (panel_open) {
            if (panel.return_to_training_page(3)) {
                panel_open = false;
            }
            else {
                Log.warn(__FUNCTION__, "failed to verify return to the training room page");
            }
        }
        json::value details = json::object {
            { "result", switched ? "switched" : "unchanged" },
            { "next_mastery", next_mastery },
        };
        if (!assistant.empty()) {
            details["assistant"] = std::move(assistant);
        }
        json::value cb_info = basic_info_with_what("InfrastTrainingAssistant");
        cb_info["details"] = std::move(details);
        callback(AsstMsg::SubTaskExtraInfo, cb_info);
        return true;
    };
    const auto fail = [&](std::string_view reason, std::string assistant = {}) {
        Log.warn(__FUNCTION__, reason);
        return finish(false, std::move(assistant));
    };

    if (!ProcessTask(*this, { "InfrastTrainingAssistantFlag" }).set_ignore_error(true).run()) {
        Log.info(__FUNCTION__, "assistant flag not found on the training skill page, keep current assistant");
        return finish(false);
    }

    // 上游跨职业重名重构后同名干员可能对应多个职业，角色匹配一律按集合包含判断
    const std::unordered_set<battle::Role> trainee_roles = BattleData.get_roles(m_operator_name);
    if (trainee_roles.empty()) {
        Log.warn(__FUNCTION__, "trainee role is unknown, ignore role-specific halving bonus:", m_operator_name);
    }

    std::unordered_map<std::string, TrainingHalvingAssistant> halving_configs;
    std::unordered_map<std::string, training::StageAssistantGroup> stage_configs;
    if (!Training.ensure_loaded()) {
        return fail("TrainingConfig is unavailable, keep current assistant");
    }
    if (next_mastery == 2) {
        halving_configs = training::halving_assistants_by_name();
        if (halving_configs.empty()) {
            return fail("no halving assistants loaded from TrainingConfig");
        }
    }
    else {
        // next_mastery == 3：_run 的 m_level != 3 门卫保证只会进入 2/3 两种取值
        for (const auto role : trainee_roles) {
            stage_configs.merge(training::stage_assistant_groups(role, next_mastery));
        }
        if (stage_configs.empty()) {
            return fail("no static M3 assistants loaded from TrainingConfig");
        }
    }

    if (need_exit()) {
        return false;
    }
    if (!panel.open_training_assistant_panel(MaxAssistantPages)) {
        return need_exit() ? false : fail("failed to open assistant panel");
    }
    panel_open = true;

    const auto first_page = panel.read_stable_oper_list_page();
    if (!first_page) {
        return need_exit() ? false : fail("failed to stably recognize the first assistant page");
    }
    const auto selected = std::ranges::find_if(*first_page, [](const OperListEntry& entry) { return entry.selected; });
    if (selected == first_page->end() ||
        std::ranges::count_if(*first_page, [](const OperListEntry& entry) { return entry.selected; }) != 1) {
        return fail("failed to identify exactly one original assistant on the first page");
    }
    const std::string original_assistant = selected->name;
    Log.info(__FUNCTION__, "original assistant:", original_assistant);

    const auto available = [&](const OperListEntry& entry) {
        return entry.name != m_operator_name && (!entry.working || entry.name == original_assistant) &&
               entry.mood > 0.05;
    };

    const auto scan_pages = [&](bool reset, auto&& visitor) -> AssistantStepResult {
        return panel.scan_oper_list_pages(
            MaxAssistantPages,
            std::forward<decltype(visitor)>(visitor),
            reset,
            reset ? nullptr : &*first_page);
    };

    // 选人决策（M2 减半 rank_factor / M3 效率+better_assistant）在各自 scan 分支完成；此处只负责
    // “找到目标→点选→确认”的机械流程，面板收尾语义由共享方法保证。
    const auto find_and_confirm = [&](std::string_view target) -> AssistantSelectResult {
        // M3 检视点击会暂留选中标记（单选光标 ≠ 在座），故 selected_means_seated 传 false
        const auto outcome = panel.select_and_confirm(target, available, MaxAssistantPages, true, false);
        switch (outcome.result) {
        case AssistantSelectResult::Stopped:
            return AssistantSelectResult::Stopped;
        case AssistantSelectResult::Confirmed:
            confirmation_submitted = true;
            panel_open = false;
            break;
        case AssistantSelectResult::AlreadySelected:
            panel_open = outcome.panel_open;
            break;
        case AssistantSelectResult::ConfirmFailed:
        case AssistantSelectResult::StateUnknown:
            // 确认按钮已点击，不能断言原座仍在
            confirmation_submitted = true;
            panel_open = outcome.panel_open;
            break;
        default:
            // 未见目标 / 不可用 / 点选未生效 / 页面失败：原座仍在
            panel_open = outcome.panel_open;
            break;
        }
        return outcome.result;
    };

    if (next_mastery == 2) {
        struct Candidate
        {
            std::string name;
            double rank_factor = 1;
            size_t order = 0;
        };

        std::vector<Candidate> candidates;
        size_t order = 0;
        // continue 路径换人发生在 M2 开段，剩余恒为全周期（960 分钟 > 300），代理恒真；
        // 本方法仅在该时机被调用，不存在剩余不足的训练中触发场景。
        const bool armable = Training.mastery_base_minutes().at(1) > Training.halving_trigger_minutes();
        const AssistantStepResult scan_result = scan_pages(false, [&](const OperListEntry& entry) {
            const size_t ui_order = order++;
            const auto config = halving_configs.find(entry.name);
            if (config == halving_configs.end()) {
                return true;
            }
            if (!available(entry)) {
                Log.info(
                    __FUNCTION__,
                    "halving candidate unavailable:",
                    entry.name,
                    "working:",
                    entry.working,
                    "mood:",
                    entry.mood);
                return true;
            }
            const int trigger = armable ? config->second.trigger_bonus : 0;
            const int role_bonus = std::ranges::any_of(
                                       trainee_roles,
                                       [&](const battle::Role role) { return config->second.roles.contains(role); })
                                       ? config->second.role_bonus
                                       : 0;
            // 候选比较分数而非完整耗时模型：两名减半协助 trigger 对称时退化为"职业匹配优先"；
            // 若未来出现非对称 trigger，需改用分阶段总耗时比较。
            const double rank_factor = (1.0 - trigger / 100.0) / (1.0 + role_bonus / 100.0);
            candidates.emplace_back(Candidate { entry.name, rank_factor, ui_order });
            Log.info(
                __FUNCTION__,
                "halving candidate:",
                entry.name,
                "trigger:",
                trigger,
                "role bonus:",
                role_bonus,
                "rank factor:",
                rank_factor,
                "order:",
                ui_order);
            return true;
        });
        if (scan_result == AssistantStepResult::Stopped) {
            return false;
        }
        if (scan_result == AssistantStepResult::Failed) {
            return fail("failed to scan M2 assistant pages", original_assistant);
        }
        if (candidates.empty()) {
            Log.info(__FUNCTION__, "no available halving assistant found");
            return finish(false, original_assistant);
        }
        const auto best = std::ranges::min_element(candidates, [](const Candidate& lhs, const Candidate& rhs) {
            return std::abs(lhs.rank_factor - rhs.rank_factor) > 1e-9 ? lhs.rank_factor < rhs.rank_factor
                                                                      : lhs.order < rhs.order;
        });
        Log.info(
            __FUNCTION__,
            "picked M2 assistant:",
            best->name,
            "rank factor:",
            best->rank_factor,
            "order:",
            best->order);
        if (best->name == original_assistant) {
            return finish(false, original_assistant);
        }
        const auto switch_result = find_and_confirm(best->name);
        if (switch_result == AssistantSelectResult::Stopped) {
            return false;
        }
        return switch_result == AssistantSelectResult::Confirmed
                   ? finish(true, best->name)
                   : fail(
                         "failed to switch M2 assistant",
                         confirmation_submitted ? std::string {} : original_assistant);
    }

    std::optional<VerifiedAssistant> best;
    const AssistantStepResult scan_result = scan_pages(false, [&](const OperListEntry& entry) {
        const auto config = stage_configs.find(entry.name);
        if (config == stage_configs.end()) {
            return true;
        }
        if (!available(entry)) {
            Log.info(
                __FUNCTION__,
                "M3 candidate unavailable:",
                entry.name,
                "working:",
                entry.working,
                "mood:",
                entry.mood);
            return true;
        }
        if (best && config->second.max_bonus < best->bonus) {
            Log.info(
                __FUNCTION__,
                "M3 candidate pruned:",
                entry.name,
                "max bonus:",
                config->second.max_bonus,
                "best:",
                best->bonus);
            return true;
        }

        if (!entry.selected) {
            ctrler()->click(entry.rect);
            sleep(300);
            if (need_exit()) {
                return false;
            }
        }

        const auto observation = TrainingAssistantPanel::read_stable_observation<std::pair<int, double>>(
            3,
            std::chrono::milliseconds(250),
            [&]() -> std::optional<std::pair<int, double>> {
                const auto percentages = panel.recognize_oper_list_percentages("InfrastTrainingAssistantSkillArea");
                const auto matched = training::match_stage_bonus(percentages, config->second.variants);
                if (matched) {
                    Log.info(
                        __FUNCTION__,
                        "M3 candidate verified:",
                        entry.name,
                        "bonus:",
                        matched->first,
                        "mood cost:",
                        matched->second);
                }
                else {
                    Log.info(__FUNCTION__, "M3 candidate unmatched:", entry.name, "percentages empty");
                }
                return matched;
            },
            [this] { return need_exit(); });
        if (observation) {
            const VerifiedAssistant verified {
                .name = entry.name,
                .operator_id = config->second.operator_id,
                .bonus = observation->first,
                .mood_cost = observation->second,
                .original = entry.name == original_assistant,
            };
            if (!best || better_assistant(verified, *best)) {
                best = verified;
            }
        }
        return true;
    });
    if (scan_result == AssistantStepResult::Stopped) {
        return false;
    }
    if (scan_result == AssistantStepResult::Failed) {
        return fail("failed to scan M3 assistant pages", original_assistant);
    }
    if (!best) {
        Log.info(__FUNCTION__, "no M3 assistant bonus was verified");
        return finish(false, original_assistant);
    }
    Log.info(__FUNCTION__, "picked M3 assistant:", best->name, "bonus:", best->bonus, "original:", best->original);
    if (best->name == original_assistant) {
        return finish(false, original_assistant);
    }
    const auto switch_result = find_and_confirm(best->name);
    if (switch_result == AssistantSelectResult::Stopped) {
        return false;
    }
    return switch_result == AssistantSelectResult::Confirmed
               ? finish(true, best->name)
               : fail("failed to switch M3 assistant", confirmation_submitted ? std::string {} : original_assistant);
}

int asst::InfrastTrainingTask::skill_index_from_rect(const Rect& r)
{
    int cy = r.y + r.height / 2;
    if (cy <= 300) {
        return 1;
    }
    if (cy <= 500) {
        return 2;
    }
    return 3;
}
