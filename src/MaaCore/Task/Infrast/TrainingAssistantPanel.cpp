#include "TrainingAssistantPanel.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <regex>
#include <unordered_set>
#include <utility>

#include "Common/AsstInfrastDef.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/AbstractTask.h"
#include "Task/Infrast/InfrastAbstractTask.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Infrast/InfrastOperImageAnalyzer.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

namespace
{
template <typename Page>
std::string oper_list_page_signature(const Page& page)
{
    std::string signature;
    for (const auto& entry : page) {
        signature.append(entry.name);
        signature.push_back('\x1f');
        signature.push_back(entry.selected ? '1' : '0');
        signature.push_back(entry.working ? '1' : '0');
        signature.push_back('\x1e');
    }
    return signature;
}
} // namespace

asst::TrainingAssistantPanel::TrainingAssistantPanel(AbstractTask& owner) :
    InstHelper(owner.inst()),
    m_owner(owner)
{
}

std::optional<std::vector<asst::TrainingAssistantPanel::OperListEntry>>
    asst::TrainingAssistantPanel::analyze_oper_list_page()
{
    InfrastOperImageAnalyzer analyzer(ctrler()->get_image());
    analyzer.set_to_be_calced(
        InfrastOperImageAnalyzer::ToBeCalced::Mood | InfrastOperImageAnalyzer::ToBeCalced::Selected |
        InfrastOperImageAnalyzer::ToBeCalced::Doing);
    if (!analyzer.analyze()) {
        return std::nullopt;
    }
    analyzer.sort_by_loc();

    const auto& replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    std::vector<OperListEntry> result;
    result.reserve(analyzer.get_result().size());
    for (const auto& oper : analyzer.get_result()) {
        RegionOCRer name_analyzer;
        name_analyzer.set_image(oper.name_img);
        name_analyzer.set_replace(replace->replace_map, replace->replace_full);
        name_analyzer.set_bin_expansion(0);
        if (!name_analyzer.analyze() || name_analyzer.get_result().text.empty()) {
            continue;
        }
        result.emplace_back(
            OperListEntry {
                .name = name_analyzer.get_result().text,
                .rect = oper.rect,
                .mood = oper.mood_ratio,
                .working = oper.doing == infrast::Doing::Working,
                .selected = oper.selected,
            });
    }
    return result;
}

bool asst::TrainingAssistantPanel::ensure_oper_list_sort(std::string_view task_name, std::string_view label)
{
    const auto read = [&]() -> std::optional<std::pair<Rect, bool>> {
        const cv::Mat image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_roi(Task.get(std::string(task_name))->roi);
        analyzer.set_required({ std::string(label) });
        const auto results = analyzer.analyze();
        if (!results) {
            return std::nullopt;
        }
        const auto iter = std::ranges::find_if(*results, [&](const TextRect& item) { return item.text == label; });
        if (iter == results->end()) {
            return std::nullopt;
        }

        const cv::Rect roi = make_rect<cv::Rect>(iter->rect) & cv::Rect(0, 0, image.cols, image.rows);
        int cyan_pixels = 0;
        for (int y = roi.y; y < roi.y + roi.height; ++y) {
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                const auto& pixel = image.at<cv::Vec3b>(y, x);
                if (pixel[0] >= 130 && pixel[1] >= 90 && pixel[2] <= 140 && pixel[0] >= pixel[2] + 30) {
                    ++cyan_pixels;
                }
            }
        }
        return std::pair(iter->rect, cyan_pixels >= 5);
    };

    auto state = read();
    if (!state) {
        return false;
    }
    if (state->second) {
        return true;
    }
    ctrler()->click(state->first);
    sleep(1500);
    state = need_exit() ? std::nullopt : read();
    return state && state->second;
}

std::vector<int> asst::TrainingAssistantPanel::recognize_oper_list_percentages(std::string_view task_name)
{
    OCRer analyzer(ctrler()->get_image());
    analyzer.set_roi(Task.get(std::string(task_name))->roi);
    analyzer.set_use_char_model(true);
    analyzer.set_use_raw(true);
    const auto results = analyzer.analyze();
    if (!results) {
        return {};
    }

    static const std::regex PercentPattern(R"((\d{1,3})\s*%)");
    std::vector<int> percentages;
    for (const auto& result : *results) {
        for (auto iter = std::sregex_iterator(result.text.begin(), result.text.end(), PercentPattern);
             iter != std::sregex_iterator();
             ++iter) {
            const int value = std::stoi((*iter)[1].str());
            if (value <= 100 && std::ranges::find(percentages, value) == percentages.end()) {
                percentages.emplace_back(value);
            }
        }
    }
    return percentages;
}

std::optional<std::vector<asst::TrainingAssistantPanel::OperListEntry>>
    asst::TrainingAssistantPanel::read_stable_oper_list_page(int max_retries)
{
    std::optional<std::string> previous_signature;
    for (int retry = 0; retry < max_retries; ++retry) {
        if (need_exit()) {
            return std::nullopt;
        }
        const auto page = analyze_oper_list_page();
        if (page && !page->empty()) {
            const std::string signature = oper_list_page_signature(*page);
            if (previous_signature == signature) {
                return page;
            }
            previous_signature = std::move(signature);
        }
        else {
            previous_signature.reset();
        }
        if (retry < max_retries - 1) {
            sleep(250);
        }
    }
    return std::nullopt;
}

bool asst::TrainingAssistantPanel::open_training_assistant_panel(int rewind_pages)
{
    if (!ProcessTask(m_owner, { "InfrastTrainingOpenAssistant" }).set_ignore_error(true).run()) {
        return false;
    }
    if (!ensure_oper_list_sort("InfrastTrainingAssistantSortArea", "效率")) {
        return false;
    }
    swipe_operlist_to_the_left(m_owner, rewind_pages);
    if (need_exit()) {
        return false;
    }
    return ensure_oper_list_sort("InfrastTrainingAssistantSortArea", "效率");
}

asst::TrainingAssistantPanel::AssistantSelectOutcome asst::TrainingAssistantPanel::select_and_confirm(
    std::string_view target,
    const std::function<bool(const OperListEntry&)>& available,
    int max_pages,
    bool rewind_first,
    bool selected_means_seated)
{
    AssistantSelectOutcome outcome;
    const AssistantStepResult scanned = scan_oper_list_pages(
        max_pages,
        [&](const OperListEntry& entry) {
            if (entry.name != target) {
                return true;
            }
            if (!available(entry)) {
                Log.info(__FUNCTION__, "assistant confirm target unavailable:", entry.name);
                outcome.result = AssistantSelectResult::TargetUnavailable;
                return false;
            }
            if (entry.selected && selected_means_seated) {
                // 目标已在座：受验证关闭面板即视为选定
                outcome.result = AssistantSelectResult::AlreadySelected;
                outcome.panel_open = !return_to_training_page(1);
                return false;
            }
            if (!ensure_assistant_entry_selected(entry, available)) {
                Log.warn(__FUNCTION__, "assistant entry click did not take effect:", entry.name);
                outcome.result = AssistantSelectResult::SelectFailed;
                return false;
            }
            if (!ProcessTask(m_owner, { "InfrastConfirmButton" }).set_ignore_error(true).run() || need_exit()) {
                outcome.result = AssistantSelectResult::Stopped;
                return false;
            }
            switch (wait_after_assistant_confirm()) {
            case AssistantConfirmWait::PanelClosed:
                Log.info(__FUNCTION__, "assistant panel closed after confirm:", entry.name);
                outcome.result = AssistantSelectResult::Confirmed;
                outcome.panel_open = false;
                break;
            case AssistantConfirmWait::PanelStillOpen:
                // 面板未关闭（确认未生效），交由调用方按 outcome.panel_open 收尾
                Log.warn(__FUNCTION__, "assistant panel is still open after confirm, return for cleanup");
                outcome.result = AssistantSelectResult::ConfirmFailed;
                break;
            case AssistantConfirmWait::Unknown:
                // 门卫与排序区均无法识别，页面状态不明；是否已提交确认交由调用方按结果推断
                Log.warn(__FUNCTION__, "assistant panel state unknown after confirm");
                outcome.result = AssistantSelectResult::StateUnknown;
                break;
            }
            return false;
        },
        rewind_first);
    if (need_exit()) {
        outcome.result = AssistantSelectResult::Stopped;
        return outcome;
    }
    if (outcome.result == AssistantSelectResult::NotFound) {
        outcome.result = scanned == AssistantStepResult::Failed ? AssistantSelectResult::PageFailed
                                                                : AssistantSelectResult::NotFound;
    }
    return outcome;
}

bool asst::TrainingAssistantPanel::return_to_training_page(int max_returns)
{
    for (int retry = 0; retry < max_returns && !need_exit(); ++retry) {
        const bool returned = ProcessTask(m_owner, { "Return" }).set_ignore_error(true).run();
        OCRer main_page(ctrler()->get_image());
        main_page.set_task_info("InfrastTrainingAssistantFlag");
        if (returned && main_page.analyze()) {
            return true;
        }
    }
    return false;
}

asst::TrainingAssistantPanel::AssistantStepResult asst::TrainingAssistantPanel::scan_oper_list_pages(
    int max_pages,
    const std::function<bool(const OperListEntry&)>& visitor,
    bool rewind_first,
    const std::vector<OperListEntry>* first_page)
{
    if (rewind_first) {
        swipe_operlist_to_the_left(m_owner, max_pages);
        if (need_exit()) {
            return AssistantStepResult::Stopped;
        }
        if (!ensure_oper_list_sort("InfrastTrainingAssistantSortArea", "效率")) {
            return need_exit() ? AssistantStepResult::Stopped : AssistantStepResult::Failed;
        }
    }
    std::unordered_set<std::string> seen;
    for (int page_index = 0; page_index < max_pages; ++page_index) {
        if (need_exit()) {
            return AssistantStepResult::Stopped;
        }
        std::optional<std::vector<OperListEntry>> page;
        if (first_page && page_index == 0) {
            page = *first_page;
        }
        else {
            page = read_stable_oper_list_page();
        }
        if (!page) {
            return need_exit() ? AssistantStepResult::Stopped : AssistantStepResult::Failed;
        }
        Log.info(__FUNCTION__, "scan page:", page_index + 1, "entries:", page->size());
        size_t new_names = 0;
        for (const auto& entry : *page) {
            if (!seen.emplace(entry.name).second) {
                continue;
            }
            ++new_names;
            if (!visitor(entry)) {
                return need_exit() ? AssistantStepResult::Stopped : AssistantStepResult::Completed;
            }
        }
        if (new_names == 0 || page_index + 1 == max_pages) {
            break;
        }
        swipe_operlist_right_one_page(m_owner);
        sleep(300);
    }
    return need_exit() ? AssistantStepResult::Stopped : AssistantStepResult::Completed;
}

bool asst::TrainingAssistantPanel::ensure_assistant_entry_selected(
    const OperListEntry& expected,
    const std::function<bool(const OperListEntry&)>& available)
{
    const auto page = read_stable_oper_list_page();
    if (!page) {
        return false;
    }
    const auto current = std::ranges::find_if(*page, [&](const OperListEntry& entry) {
        return entry.name == expected.name && std::abs(entry.rect.x - expected.rect.x) <= 24 &&
               std::abs(entry.rect.y - expected.rect.y) <= 24;
    });
    if (current == page->end() || !available(*current)) {
        Log.info(__FUNCTION__, "assistant confirm target unavailable:", expected.name);
        return false;
    }
    Log.info(__FUNCTION__, "assistant confirm target:", expected.name, "selected:", current->selected);
    if (!current->selected) {
        ctrler()->click(current->rect);
        sleep(300);
        if (need_exit()) {
            return false;
        }
    }
    const auto verified_page = analyze_oper_list_page();
    if (!verified_page) {
        return false;
    }
    const auto verified_selected =
        std::ranges::count_if(*verified_page, [](const OperListEntry& entry) { return entry.selected; });
    const auto verified_target = std::ranges::find_if(*verified_page, [&](const OperListEntry& entry) {
        return entry.selected && entry.name == expected.name;
    });
    if (verified_selected != 1 || verified_target == verified_page->end()) {
        Log.warn(__FUNCTION__, "assistant entry click did not take effect:", expected.name);
        return false;
    }
    return true;
}

asst::TrainingAssistantPanel::AssistantConfirmWait asst::TrainingAssistantPanel::wait_after_assistant_confirm()
{
    for (int retry = 0; retry < 4 && !need_exit(); ++retry) {
        OCRer main_page(ctrler()->get_image());
        main_page.set_task_info("InfrastTrainingAssistantFlag");
        if (main_page.analyze()) {
            return AssistantConfirmWait::PanelClosed;
        }
        OCRer sort_label(ctrler()->get_image());
        sort_label.set_task_info("InfrastTrainingAssistantSortArea");
        if (sort_label.analyze()) {
            return AssistantConfirmWait::PanelStillOpen;
        }
        sleep(500);
    }
    return AssistantConfirmWait::Unknown;
}
