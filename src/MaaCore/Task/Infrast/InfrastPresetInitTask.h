#pragma once

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
        ProductMismatch,
    };

    std::vector<std::string> room_labels(const infrast::PresetRoom& room) const;
    RoomOpenResult open_room(const infrast::PresetRoom& room);
    bool check_room(const infrast::PresetRoom& room, bool selector = false);
    bool match_product(const infrast::PresetRoom& room, infrast::CustomRoomConfig::Product expected_product) const;
    bool match_trade_order(
        const std::vector<TextRect>& overview_labels,
        const Rect& room_label,
        infrast::CustomRoomConfig::Product expected_product) const;
    bool text_contains(const std::string& text, const std::vector<std::string>& names) const;
    bool same_overview(const cv::Mat& previous, const cv::Mat& current) const;
    std::optional<cv::Mat> stable_overview_image();
    bool wait_for(const std::string& task, int retry_times);
    std::optional<int> queue_count() const;
    bool open_slot(const infrast::PresetRoom& room, int slot);
    bool import_slot(const infrast::PresetRoom& room, int slot, const infrast::CustomRoomConfig& config);
    bool review_slot(const infrast::CustomRoomConfig& config);
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
