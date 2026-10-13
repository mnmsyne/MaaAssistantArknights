#include "InfrastPresetInitTask.h"

#include <set>
#include <unordered_map>

#include "Config/Miscellaneous/InfrastConfig.h"
#include "Config/Miscellaneous/OcrConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Infrast/InfrastOperImageAnalyzer.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

asst::InfrastPresetInitTask::InfrastPresetInitTask(
    const AsstCallback& callback,
    Assistant* inst,
    std::string_view task_chain) :
    InfrastAbstractTask(callback, inst, task_chain)
{
    set_retry_times(0); // Saved slots must never be replayed by a task-level retry.
}

bool asst::InfrastPresetInitTask::set_params(const json::value& params)
{
    const auto filename = params.find<std::string>("filename");
    if (!filename || filename->empty()) {
        return false;
    }
    const auto path = utils::path(*filename);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        LogError << "Preset import file does not exist" << path;
        return false;
    }
    const auto config = json::open(path, true, true);
    if (!config) {
        LogError << "Invalid preset import JSON" << path;
        return false;
    }
    auto parsed = infrast::parse_preset_import(*config);
    if (!parsed) {
        return false;
    }
    m_import = std::move(*parsed);
    return true;
}

std::vector<std::string> asst::InfrastPresetInitTask::room_labels(const infrast::PresetRoom& room) const
{
    auto labels = Task.get<OcrTaskInfo>("InfrastPresetRoom" + room.facility)->text;
    if (room.facility == "Mfg" || room.facility == "Trade" || room.facility == "Power") {
        for (auto& label : labels) {
            label += std::to_string(room.index + 1);
        }
    }
    return labels;
}

bool asst::InfrastPresetInitTask::check_room(const infrast::PresetRoom& room, bool selector)
{
    // The room title can be caught mid-fade right after a page switch ("制造站3" read as
    // "制造站g" at ~0.5 score on live runs); re-read a few times before failing.
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info(selector ? "InfrastPresetSelectorRoom" : "InfrastPresetEditorRoom");
        // The small selector title misreads the room number too often ("制造站3" -> "制造站z");
        // the editor context has already pinned the room, so the selector check matches the facility name only.
        ocr.set_required(
            selector ? Task.get<OcrTaskInfo>("InfrastPresetRoom" + room.facility)->text : room_labels(room));
        if (ocr.analyze()) {
            return true;
        }
        if (attempt + 1 < 3) {
            sleep(500);
        }
    }
    return false;
}

asst::InfrastPresetInitTask::RoomOpenResult asst::InfrastPresetInitTask::open_room(const infrast::PresetRoom& room)
{
    m_facility = room.facility;
    m_capacity = size_t(InfrastData.get_facility_info(m_facility).max_num_of_opers);
    m_cur_facility_index = room.index;
    // No overview reset here: rooms are ordered like the overview rows, so every room is
    // reached by a single monotonic downward scan instead of scrolling up and down again.
    if (!ProcessTask(*this, { "InfrastPresetOverview" }).run()) {
        return RoomOpenResult::NotFound;
    }
    const auto expected = room_labels(room);
    const auto facility_labels = Task.get<OcrTaskInfo>("InfrastPresetRoom" + room.facility)->text;
    for (int page = 0; page < 18 && !need_exit(); ++page) {
        std::set<std::string> rejected_titles;
        for (int attempt = 0; attempt < 6 && !need_exit(); ++attempt) {
            const auto image = stable_overview_image();
            if (!image) {
                return RoomOpenResult::NotFound;
            }
            OCRer labels(*image);
            labels.set_task_info("InfrastPresetOverviewRoom");
            labels.set_required({}); // Preserve room numbers when remembering rejected candidates.
            const auto matches = labels.analyze();
            if (!matches) {
                break;
            }
            const bool has_exact = std::ranges::any_of(*matches, [&](const auto& label) {
                return !rejected_titles.contains(label.text) && text_contains(label.text, expected);
            });
            // The trade order line is only visible as the overview sub-text; verify it before any
            // editor interaction so a mismatched order can never reach the queue writes.
            if (room.facility == "Trade" && room.product != infrast::CustomRoomConfig::Product::Unknown) {
                const auto exact_label = std::ranges::find_if(*matches, [&](const auto& label) {
                    return text_contains(label.text, expected);
                });
                if (exact_label == matches->end()) {
                    break; // An unpinned row must not be imported on a guess; keep scanning pages.
                }
                if (!match_trade_order(*matches, exact_label->rect, room.product)) {
                    return RoomOpenResult::ProductMismatch;
                }
            }
            bool rescan = false;
            for (const auto& label : *matches) {
                if (rejected_titles.contains(label.text) ||
                    !text_contains(label.text, has_exact ? expected : facility_labels)) {
                    continue;
                }
                OCRer edit(*image);
                edit.set_task_info("InfrastPresetEditQueue");
                edit.set_roi({ 430, label.rect.y + label.rect.height, 160, 100 });
                if (const auto buttons = edit.analyze()) {
                    if (!same_overview(*image, ctrler()->get_image())) {
                        LogInfo << "Preset overview moved during recognition; refreshing candidates";
                        rescan = true;
                        break;
                    }
                    const auto& button = buttons->front().rect;
                    ctrler()->click(Point { button.x + button.width / 2, button.y + button.height / 2 });
                    if (!wait_for("InfrastPresetEditor", 5)) {
                        // A missed click leaves the overview open; there is no editor to close.
                        if (need_exit() || !wait_for("InfrastPresetOverview", 2)) {
                            return RoomOpenResult::NotFound;
                        }
                        LogWarn << "Preset editor did not open; retrying from a fresh overview" << room.facility
                                << room.index << attempt;
                        rescan = true;
                        break;
                    }
                    if (check_room(room)) {
                        // The product word sits in the editor title, next to the room name; checking it here
                        // guarantees the row identity is already pinned by the editor title check.
                        if (!match_product(room, room.product)) {
                            ProcessTask(*this, { "InfrastPresetCloseEditor" }).run();
                            return RoomOpenResult::ProductMismatch;
                        }
                        return RoomOpenResult::Opened;
                    }
                    LogInfo << "Skipping a different preset editor room" << room.facility << room.index;
                    if (!text_contains(label.text, expected)) {
                        rejected_titles.insert(label.text);
                    }
                    if (!ProcessTask(*this, { "InfrastPresetCloseEditor" }).run()) {
                        return RoomOpenResult::NotFound;
                    }
                    // Closing an editor may reposition the list. Never reuse these pixels or coordinates.
                    rescan = true;
                    break;
                }
            }
            if (!rescan) {
                break;
            }
        }
        if (!ProcessTask(*this, { "InfrastPresetOverviewDown" }).run()) {
            break;
        }
    }
    // A room without any saved queue only offers an add-shift button that would change
    // the actual stationed operators, so it can never open the queue editor.
    LogError << "Preset room was not found" << room.facility << room.index;
    return RoomOpenResult::NotFound;
}

bool asst::InfrastPresetInitTask::match_product(
    const infrast::PresetRoom& room,
    infrast::CustomRoomConfig::Product expected_product) const
{
    if (room.facility != "Mfg" || expected_product == infrast::CustomRoomConfig::Product::Unknown) {
        return true;
    }
    static const std::unordered_map<infrast::CustomRoomConfig::Product, std::string> product_words = {
        { infrast::CustomRoomConfig::Product::BattleRecord, "作战记录" },
        { infrast::CustomRoomConfig::Product::PureGold, "贵金属" },
        { infrast::CustomRoomConfig::Product::Dualchip, "芯片" },
        { infrast::CustomRoomConfig::Product::OriginiumShard, "源石碎片" },
    };
    const auto expected_iter = product_words.find(expected_product);
    if (expected_iter == product_words.cend()) {
        // Unreachable after parse validation; stop instead of guessing on an unmapped product.
        LogError << "No editor title word for product" << room.facility << int(expected_product);
        return false;
    }
    const auto& expected = expected_iter->second;
    // The editor title reads "制造站 N <产品>"; the room identity is already confirmed at this point.
    OCRer ocr(ctrler()->get_image());
    ocr.set_task_info("InfrastPresetEditorProduct");
    std::vector<std::string> observed;
    if (const auto results = ocr.analyze()) {
        for (const auto& result : *results) {
            observed.emplace_back(result.text);
        }
    }
    const bool matched =
        std::ranges::any_of(observed, [&](const std::string& text) { return text_contains(text, { expected }); });
    if (!matched) {
        LogWarn << "Preset room product mismatch" << room.facility << room.index << expected << observed;
    }
    return matched;
}

bool asst::InfrastPresetInitTask::match_trade_order(
    const std::vector<TextRect>& overview_labels,
    const Rect& room_label,
    infrast::CustomRoomConfig::Product expected_product) const
{
    // The overview sub-text names the current order line; these are the only two trade lines.
    // The sub-text is small and live OCR drops characters ("龙门商法" repeatedly read as "龙商法"),
    // so the match uses the distinctive line-name suffix; the full name stays for diagnostics.
    static const std::unordered_map<infrast::CustomRoomConfig::Product, std::pair<std::string, std::string>>
        order_words = {
            { infrast::CustomRoomConfig::Product::LMD, { "龙门商法", "商法" } },
            { infrast::CustomRoomConfig::Product::Orundum, { "开采协力", "协力" } },
        };
    const auto expected_iter = order_words.find(expected_product);
    if (expected_iter == order_words.cend()) {
        // Unreachable after parse validation; stop instead of guessing on an unmapped order.
        LogError << "No overview word for trade order" << int(expected_product);
        return false;
    }
    const auto& [expected, expected_key] = expected_iter->second;
    // The sub-text sits between the room name and the edit-queue button below it.
    const int band_top = room_label.y + room_label.height;
    const int band_bottom = band_top + 45;
    std::vector<std::string> observed;
    for (const auto& label : overview_labels) {
        const bool in_band = label.rect.y >= band_top && label.rect.y + label.rect.height <= band_bottom &&
                             label.rect.x + label.rect.width > room_label.x &&
                             label.rect.x < room_label.x + room_label.width;
        if (!in_band) {
            continue;
        }
        if (text_contains(label.text, { expected_key })) {
            return true;
        }
        observed.emplace_back(label.text);
    }
    LogWarn << "Preset trade order not verified" << room_label << expected << observed;
    return false;
}

bool asst::InfrastPresetInitTask::text_contains(const std::string& text, const std::vector<std::string>& names) const
{
    const auto& config = OcrConfig::get_instance();
    const auto normalized = config.process_equivalence_class(text);
    return std::ranges::any_of(names, [&](const auto& name) {
        return normalized.find(config.process_equivalence_class(name)) != std::string::npos;
    });
}

bool asst::InfrastPresetInitTask::same_overview(const cv::Mat& previous, const cv::Mat& current) const
{
    const auto& roi = Task.get("InfrastPresetEditQueue")->roi;
    if (previous.empty() || current.empty() || previous.size() != current.size() || previous.type() != current.type() ||
        roi.x < 0 || roi.y < 0 || roi.width <= 0 || roi.height <= 0 || roi.x + roi.width > current.cols ||
        roi.y + roi.height > current.rows) {
        return false;
    }
    const cv::Rect region(roi.x, roi.y, roi.width, roi.height);
    // Compare only the room/queue-button column, excluding animated operator portraits.
    const auto difference = cv::norm(previous(region), current(region), cv::NORM_L1);
    return difference / (double(region.area()) * current.channels()) <= 0.5;
}

std::optional<cv::Mat> asst::InfrastPresetInitTask::stable_overview_image()
{
    auto previous = ctrler()->get_image().clone();
    int stable_frames = 0;
    for (int sample = 0; sample < 12 && !need_exit(); ++sample) {
        if (!sleep(250)) {
            return std::nullopt;
        }
        auto current = ctrler()->get_image().clone();
        stable_frames = same_overview(previous, current) ? stable_frames + 1 : 0;
        if (stable_frames >= 2) {
            return current;
        }
        previous = std::move(current);
    }
    LogError << "Preset overview did not stabilize";
    return std::nullopt;
}

bool asst::InfrastPresetInitTask::wait_for(const std::string& task, int retry_times)
{
    // Page transitions may outlast a fixed sleep; gate the next OCR on a recognition loop instead.
    ProcessTask wait(*this, { task });
    wait.set_retry_times(retry_times);
    return wait.run();
}

std::optional<int> asst::InfrastPresetInitTask::queue_count() const
{
    OCRer ocr(ctrler()->get_image());
    ocr.set_task_info("InfrastPresetEditor");
    ocr.set_required({}); // Keep the queue count instead of the matched label.
    if (const auto results = ocr.analyze()) {
        for (const auto& result : *results) {
            const auto slash = result.text.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 >= result.text.size() ||
                result.text[slash + 1] != '3') {
                continue;
            }
            const auto value = result.text[slash - 1];
            if (value >= '0' && value <= '3' &&
                (slash < 2 || result.text[slash - 2] < '0' || result.text[slash - 2] > '9')) {
                return value - '0';
            }
        }
    }
    return std::nullopt;
}

bool asst::InfrastPresetInitTask::open_slot(const infrast::PresetRoom& room, int slot)
{
    if (!check_room(room)) {
        return false;
    }
    const auto count = queue_count();
    if (!count) {
        LogError << "Preset queue count recognition failed" << room.facility << room.index;
        m_fail_reason = "queue_count";
        return false;
    }
    if (slot > *count) {
        LogError << "Preset queue gap cannot be filled without changing a skipped slot" << slot;
        m_fail_reason = "queue_gap";
        return false;
    }
    auto labels = Task.get<OcrTaskInfo>("InfrastPresetQueue")->text;
    for (auto& label : labels) {
        label += std::to_string(slot + 1);
    }
    for (int page = 0; page < 4 && !need_exit(); ++page) {
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info(slot == *count ? "InfrastPresetAddQueue" : "InfrastPresetQueue");
        if (slot != *count) {
            ocr.set_required(labels);
        }
        if (const auto found = ocr.analyze()) {
            // Open the first portrait, anchored to the queue label. Avoid trash and swap buttons.
            const auto rect = slot == *count ? found->front().rect
                                             : found->front().rect.move(Task.get("InfrastPresetQueue")->rect_move);
            ctrler()->click(rect);
            sleep(800);
            if (slot == *count) {
                // Adding a queue immediately creates an empty saved slot; open its first + card next.
                if (!check_room(room) || queue_count() != std::optional<int>(slot + 1)) {
                    return false;
                }
                ++m_created;
                report("created");
                return open_slot(room, slot);
            }
            // Single-operator facilities have no clear button, as in ordinary infrastructure selection.
            // A missing clear button does not prove that a queue is locked or matches current staffing.
            const auto selector_task = m_capacity == 1 ? "InfrastPresetSelectorPage" : "InfrastPresetSelector";
            if (!wait_for(selector_task, 5)) {
                return false;
            }
            return check_room(room, true);
        }
        if (page == 0 && slot < *count) {
            if (!ProcessTask(*this, { "InfrastPresetQueueTop" }).run()) {
                return false;
            }
            continue;
        }
        if (!ProcessTask(*this, { "InfrastPresetQueueDown" }).run()) {
            return false;
        }
    }
    return false;
}

bool asst::InfrastPresetInitTask::review_slot(const infrast::CustomRoomConfig& config)
{
    LogTraceFunction;
    // Opening a saved queue places its selected operators first, in queue order.
    close_quick_formation_expand_role();
    swipe_to_the_left_of_operlist();
    sleep(500);
    InfrastOperImageAnalyzer analyzer(ctrler()->get_image());
    analyzer.set_to_be_calced(InfrastOperImageAnalyzer::ToBeCalced::Selected);
    if (!analyzer.analyze()) {
        return false;
    }
    analyzer.sort_by_loc();
    std::vector<std::string> observed;
    const auto& replacement = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    for (const auto& oper : analyzer.get_result()) {
        if (!oper.selected) {
            continue;
        }
        RegionOCRer name;
        name.set_replace(replacement->replace_map, replacement->replace_full);
        name.set_image(oper.name_img);
        name.set_bin_expansion(0);
        if (!name.analyze()) {
            return false;
        }
        observed.emplace_back(name.get_result().text);
    }
    auto expected = config.names;
    if (!config.sort) {
        std::ranges::sort(observed);
        std::ranges::sort(expected);
    }
    const bool matched = observed == expected;
    LogInfo << "Preset strict roster review" << observed << expected << matched;
    return matched;
}

bool asst::InfrastPresetInitTask::trim_queues(const infrast::PresetRoom& room)
{
    // Sparse slots are deliberate placeholders: preserve them and only remove trailing queues.
    const int expected = room.slots.rbegin()->first + 1;
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        if (!check_room(room)) {
            return false;
        }
        const auto count = queue_count();
        if (!count || *count < expected) {
            return false;
        }
        if (*count == expected) {
            return true;
        }
        bool deleted = false;
        for (int page = 0; page < 4 && !need_exit(); ++page) {
            OCRer label(ctrler()->get_image());
            label.set_task_info("InfrastPresetQueue");
            label.set_required({ "队列" + std::to_string(*count) });
            if (const auto found = label.analyze()) {
                // The trash button is anchored below this exact queue's label, never the swap button.
                ctrler()->click(found->front().rect.move(Task.get("InfrastPresetDeleteQueue")->rect_move));
                if (!wait_for("InfrastPresetDeleteConfirm", 5) || !wait_for("InfrastPresetEditor", 5) ||
                    !check_room(room)) {
                    return false;
                }
                sleep(500);
                if (queue_count() != std::optional<int>(*count - 1)) {
                    return false;
                }
                LogInfo << "Deleted trailing preset queue" << room.facility << room.index << *count;
                deleted = true;
                break;
            }
            if (!ProcessTask(*this, { "InfrastPresetQueueDown" }).run()) {
                return false;
            }
        }
        if (!deleted) {
            return false;
        }
    }
    return !need_exit() && check_room(room) && queue_count() == std::optional<int>(expected);
}

bool asst::InfrastPresetInitTask::import_slot(
    const infrast::PresetRoom& room,
    int slot,
    const infrast::CustomRoomConfig& config)
{
    m_fail_reason.clear();
    if (!open_slot(room, slot)) {
        return false;
    }
    if (review_slot(config)) {
        LogInfo << "Preset queue already matches; preserving selection" << room.facility << room.index << slot;
        return ProcessTask(*this, { "InfrastPresetCancelSelection" }).run() && wait_for("InfrastPresetEditor", 5) &&
               check_room(room) && !need_exit();
    }
    // The shared selector consumes its config; each slot receives a fresh copy.
    m_cur_facility_index = 0;
    set_custom_config({ config });
    close_quick_formation_expand_role();
    swipe_to_the_left_of_operlist();
    infrast::CustomRoomConfig empty_selection;
    if ((m_capacity > 1 && (!click_clear_button() || !review_slot(empty_selection))) ||
        !swipe_and_select_custom_opers() || need_exit() || !check_room(room, true)) {
        if (!need_exit()) {
            ProcessTask(*this, { "InfrastPresetCancelSelection" }).run();
        }
        return false;
    }
    // Do not call the normal confirmation wrapper: it commits actual stationed operators.
    if (!ProcessTask(*this, { "InfrastPresetConfirm" }).run() || !wait_for("InfrastPresetEditor", 5) ||
        !check_room(room)) {
        return false;
    }
    // Read the saved queue back before reporting success.
    if (!open_slot(room, slot)) {
        return false;
    }
    const bool verified = review_slot(config);
    const bool closed = ProcessTask(*this, { "InfrastPresetCancelSelection" }).run() &&
                        wait_for("InfrastPresetEditor", 5) && check_room(room);
    return verified && closed && !need_exit();
}

void asst::InfrastPresetInitTask::report(
    const std::string& status,
    const std::string& reason,
    const infrast::CustomRoomConfig* config)
{
    auto info = basic_info_with_what("InfrastPresetImport");
    auto& details = info["details"];
    details["status"] = status;
    details["reason"] = reason;
    details["completed"] = m_completed;
    details["created"] = m_created;
    details["total"] = m_import.slot_count;
    details["slot"] = m_slot + 1;
    if (config != nullptr) {
        details["names"] = json::array(config->names);
    }
    callback(AsstMsg::SubTaskExtraInfo, info);
}

bool asst::InfrastPresetInitTask::_run()
{
    LogTraceFunction;
    for (const auto& warning : m_import.warnings) {
        report("warning", warning);
    }
    if (!ProcessTask(*this, { "InfrastPresetBegin" }).run() || need_exit()) {
        report(need_exit() ? "cancelled" : "failed", "navigation");
        return false;
    }
    // The overview may be left scrolled anywhere by a previous run; rooms are visited top-down,
    // so pull the list back to the top exactly once before the monotonic scan.
    ProcessTask(*this, { "InfrastPresetResetOverview" }).run();
    if (need_exit()) {
        report("cancelled", "navigation");
        return false;
    }
    // Single monotonic pass: open each room once, verify and fill its queues, then move on.
    for (const auto& room : m_import.rooms) {
        m_cur_facility_index = room.index;
        m_slot = room.slots.begin()->first;
        auto opened = open_room(room);
        if (need_exit()) {
            report("cancelled", "room");
            return false;
        }
        if (opened == RoomOpenResult::ProductMismatch) {
            report("warning", "product_mismatch");
            report("failed", "product_mismatch");
            return false;
        }
        if (opened == RoomOpenResult::NotFound) {
            m_fail_reason = "room";
            report(need_exit() ? "cancelled" : "failed", "room");
            return false;
        }
        report("room");
        for (const auto& [slot, config] : room.slots) {
            m_slot = slot;
            m_cur_facility_index = room.index;
            report("progress", "", &config);
            if (!import_slot(room, slot, config)) {
                m_cur_facility_index = room.index;
                report(
                    need_exit() ? "cancelled" : "failed",
                    m_fail_reason.empty() ? "selection_or_save" : m_fail_reason,
                    &config);
                return false;
            }
            ++m_completed;
            m_cur_facility_index = room.index;
            report("saved", "", &config);
        }
        if (!trim_queues(room)) {
            report(need_exit() ? "cancelled" : "failed", "queue_count");
            return false;
        }
        if (!ProcessTask(*this, { "InfrastPresetCloseEditor" }).run() || need_exit()) {
            report(need_exit() ? "cancelled" : "failed", "navigation");
            return false;
        }
    }
    report("completed");
    return true;
}
