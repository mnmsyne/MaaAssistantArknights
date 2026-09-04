#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common/AsstTypes.h"
#include "InstHelper.h"

namespace asst
{
class AbstractTask;

class TrainingAssistantPanel final : protected InstHelper
{
public:
    struct OperListEntry
    {
        std::string name;
        Rect rect;
        double mood = 0;
        bool working = false;
        bool selected = false;
    };

    enum class AssistantStepResult
    {
        Completed,
        Failed,
        Stopped,
    };

    enum class AssistantConfirmWait
    {
        PanelClosed,
        PanelStillOpen,
        Unknown,
    };

    // 选定并确认一名协助干员的完整结果：流程 = 扫描找目标 → available 校验 → 点选验证 → 确认按钮 → 等待面板关闭。
    enum class AssistantSelectResult
    {
        Confirmed,         // 确认生效，面板已关闭
        AlreadySelected,   // 目标已在座，受验证关闭面板
        TargetUnavailable, // 目标在面板上但 available 判定不可用
        NotFound,          // 扫描范围内未见目标
        SelectFailed,      // 点选未通过验证
        ConfirmFailed,     // 确认点击后面板仍未关闭（确认未生效）
        StateUnknown,      // 确认点击后面板状态无法判别
        PageFailed,        // 扫描/页面识别失败
        Stopped,
    };

    struct AssistantSelectOutcome
    {
        AssistantSelectResult result = AssistantSelectResult::NotFound;
        bool panel_open = true; // 返回时面板是否仍开着；Confirmed/AlreadySelected 保证已受验证关闭
    };

    explicit TrainingAssistantPanel(AbstractTask& owner);

    std::optional<std::vector<OperListEntry>> read_stable_oper_list_page(int max_retries = 3);
    bool open_training_assistant_panel(int rewind_pages = 4);
    bool return_to_training_page(int max_returns);
    AssistantStepResult scan_oper_list_pages(
        int max_pages,
        const std::function<bool(const OperListEntry&)>& visitor,
        bool rewind_first = false,
        const std::vector<OperListEntry>* first_page = nullptr);
    bool ensure_assistant_entry_selected(
        const OperListEntry& expected,
        const std::function<bool(const OperListEntry&)>& available);
    std::vector<int> recognize_oper_list_percentages(std::string_view task_name);
    AssistantConfirmWait wait_after_assistant_confirm();

    // 按 target 扫描协助面板并完成点选与确认。available 语义由调用方定义（干员培养排除受训者、基建换班
    // 放行原座）；AlreadySelected 分支供“目标本就在座”的调用方使用。除 Confirmed/AlreadySelected 外面板
    // 不代为清理，调用方按 outcome.panel_open 决定收尾（基建侧保留其 return_to_training_page(3) 的受验证退出）。
    // selected_means_seated：true 时命中已带选中标记的目标视为“已在座”，受验证关闭面板即返回
    // AlreadySelected；false 供同一面板会话内先点击过候选检视数值的调用方使用——选中标记只是暂留的
    // 单选光标而非在座（实测 09-05：检视点击会移动标记），此时仍走点选验证与确认。
    AssistantSelectOutcome select_and_confirm(
        std::string_view target,
        const std::function<bool(const OperListEntry&)>& available,
        int max_pages,
        bool rewind_first = false,
        bool selected_means_seated = true);

    // 连续两次相同观测才采信（干员培养与基建换班的协助加成验证共用）。read 产出一次观测（可空表示该帧
    // 未读出；空观测不参与比对），重试耗尽返回空。cancelled 返回 true 时立即以空结束。
    template <typename T>
    static std::optional<T> read_stable_observation(
        int retries,
        std::chrono::milliseconds interval,
        const std::function<std::optional<T>()>& read,
        const std::function<bool()>& cancelled)
    {
        std::optional<T> previous;
        for (int retry = 0; retry < retries; ++retry) {
            if (cancelled()) {
                return std::nullopt;
            }
            std::optional<T> observation = read();
            if (observation && previous == observation) {
                return observation;
            }
            previous = observation;
            if (retry < retries - 1) {
                std::this_thread::sleep_for(interval);
            }
        }
        return std::nullopt;
    }

private:
    std::optional<std::vector<OperListEntry>> analyze_oper_list_page();
    bool ensure_oper_list_sort(std::string_view task_name, std::string_view label);

    AbstractTask& m_owner;
};
} // namespace asst

