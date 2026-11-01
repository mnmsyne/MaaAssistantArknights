#pragma once

#include <initializer_list>

#include "Config/Miscellaneous/InfrastPresetConfig.h"
#include "InfrastAbstractTask.h"

namespace asst
{
class InfrastPresetInitTask final : public InfrastAbstractTask
{
public:
    InfrastPresetInitTask(const AsstCallback& callback, Assistant* inst, std::string_view task_chain);
    bool set_params(const json::value& params);

    virtual std::string facility_name() const override { return m_facility; }

    virtual size_t max_num_of_opers() const noexcept override { return m_capacity; }

protected:
    virtual bool _run() override;

    virtual bool on_run_fails() override { return false; }

private:
    enum class RoomOpenResult
    {
        Opened,
        NotFound,
        ProductFailure,
    };

    enum class RecognitionResult
    {
        Matched,
        Mismatched,
        Uncertain,
    };

    std::string room_label(const infrast::PresetRoom& room) const;
    RoomOpenResult open_room(const infrast::PresetRoom& room);
    bool check_room(const infrast::PresetRoom& room, bool selector = false);
    RecognitionResult check_editor_room(const infrast::PresetRoom& room);
    RecognitionResult match_product(const infrast::PresetRoom& room);
    RecognitionResult match_trade_order(
        const std::vector<TextRect>& overview_labels,
        const Rect& room_label,
        infrast::CustomRoomConfig::Product expected_product) const;
    RecognitionResult classify_product(
        const std::vector<std::string>& observed,
        infrast::CustomRoomConfig::Product expected_product,
        std::initializer_list<std::pair<infrast::CustomRoomConfig::Product, std::string_view>> keywords) const;
    bool text_contains(const std::string& text, const std::string& name) const;
    bool same_overview(const cv::Mat& previous, const cv::Mat& current) const;
    std::optional<cv::Mat> stable_overview_image();
    bool wait_for(const std::string& task, int retry_times);
    std::optional<int> queue_count() const;
    std::optional<Rect>
        find_queue_label(const std::string& task, const std::vector<std::string>& text, bool first_miss_to_top);
    bool open_slot(const infrast::PresetRoom& room, int slot);
    bool import_slot(const infrast::PresetRoom& room, int slot, const infrast::CustomRoomConfig& config);
    bool prepare_slot_review();
    RecognitionResult review_slot(const infrast::CustomRoomConfig& config);
    RecognitionResult recognize_slot(const infrast::CustomRoomConfig& config) const;
    bool selected_cards_complete(const cv::Mat& image, const std::vector<infrast::Oper>& opers) const;
    bool trim_queues(const infrast::PresetRoom& room);
    void report(
        const std::string& status,
        const std::string& reason = "",
        const infrast::CustomRoomConfig* config = nullptr);

    infrast::PresetImport m_import;
    std::string m_facility;
    size_t m_capacity = 1;
    size_t m_completed = 0;
    size_t m_created = 0;
    int m_slot = -1;
    std::string m_fail_reason;
};
}
