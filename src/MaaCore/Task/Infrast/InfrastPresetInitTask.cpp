#include "InfrastPresetInitTask.h"

#include <set>

#include "Config/Miscellaneous/BattleDataConfig.h"
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

std::string asst::InfrastPresetInitTask::room_label(const infrast::PresetRoom& room) const
{
    const auto facility = std::ranges::find(infrast::PresetFacilities, room.facility, &infrast::PresetFacility::name);
    return facility->title + (facility->room_count > 1 ? std::to_string(room.index + 1) : "");
}

bool asst::InfrastPresetInitTask::check_room(const infrast::PresetRoom& room, bool selector)
{
    if (!selector) {
        return check_editor_room(room) == RecognitionResult::Matched;
    }
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        // The editor has already pinned the room; preserve the selector's facility-only check.
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info("InfrastPresetSelectorRoom");
        const auto facility =
            std::ranges::find(infrast::PresetFacilities, room.facility, &infrast::PresetFacility::name);
        ocr.set_required({ facility->title });
        if (ocr.analyze()) {
            return true;
        }
        if (attempt + 1 < 3) {
            sleep(500);
        }
    }
    return false;
}

asst::InfrastPresetInitTask::RecognitionResult
    asst::InfrastPresetInitTask::check_editor_room(const infrast::PresetRoom& room)
{
    const auto& config = OcrConfig::get_instance();
    const auto expected = room_label(room);
    auto result = RecognitionResult::Uncertain;
    // The room title can be caught mid-fade. Only a complete known identity proves a different room.
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        result = RecognitionResult::Uncertain;
        RegionOCRer ocr(ctrler()->get_image());
        ocr.set_task_info("InfrastPresetEditorRoom");
        ocr.set_required({});
        if (const auto title = ocr.analyze()) {
            const auto text = config.process_equivalence_class(title->text);
            const auto equals = [&](const std::string& label) {
                return text == config.process_equivalence_class(label);
            };
            if (equals(expected)) {
                return RecognitionResult::Matched;
            }
            for (const auto& facility : infrast::PresetFacilities) {
                for (int index = 0; index < facility.room_count; ++index) {
                    if (equals(room_label({ .facility = facility.name, .index = index }))) {
                        LogInfo << "Preset editor belongs to a different room" << title->text;
                        result = RecognitionResult::Mismatched;
                    }
                }
            }
            if (result == RecognitionResult::Uncertain) {
                LogWarn << "Preset editor room identity is uncertain" << title->text << attempt;
            }
        }
        if (attempt + 1 < 3 && !sleep(500)) {
            break;
        }
    }
    if (result == RecognitionResult::Uncertain) {
        LogError << "Preset editor room recognition exhausted retries" << room.facility << room.index;
    }
    return result;
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
    const auto expected = room_label(room);
    const auto facility = std::ranges::find(infrast::PresetFacilities, room.facility, &infrast::PresetFacility::name);
    int uncertain_orders = 0;
    for (int page = 0; page < 18 && !need_exit(); ++page) {
        std::vector<Rect> rejected_rows;
        cv::Mat rejection_image;
        for (int attempt = 0; attempt < 6 && !need_exit(); ++attempt) {
            const auto image = stable_overview_image();
            if (!image) {
                return RoomOpenResult::NotFound;
            }
            if (!rejection_image.empty() && !same_overview(rejection_image, *image)) {
                rejected_rows.clear();
            }
            rejection_image = *image;
            const auto rejected = [&](const TextRect& label) {
                return std::ranges::any_of(rejected_rows, [&](const Rect& row) {
                    // Allow detector jitter, but never merge separate overview rows by their text.
                    return std::abs(row.x - label.rect.x) <= 8 && std::abs(row.y - label.rect.y) <= 8;
                });
            };
            OCRer labels(*image);
            labels.set_task_info("InfrastPresetOverviewRoom");
            labels.set_required({});
            auto matches = labels.analyze();
            if (!matches) {
                break;
            }
            for (auto& label : *matches) {
                if (!text_contains(label.text, facility->title) || text_contains(label.text, expected)) {
                    continue;
                }
                // An explicit different number is evidence, not a detector truncation to repair.
                if (std::ranges::any_of(label.text, [](char ch) { return ch >= '0' && ch <= '9'; })) {
                    continue;
                }
                const auto& detected = label.rect;
                const Rect roi { detected.x - 4, detected.y - 4, 100, detected.height + 8 };
                const auto& overview = Task.get("InfrastPresetOverviewRoom")->roi;
                // Require the complete title region inside both the visible overview and the snapshot.
                // Never clip an edge row and then mistake its remaining pixels for a complete identity.
                if (detected.width <= 0 || detected.height <= 0 || detected.width > roi.width - 4 ||
                    !overview.include(roi) || !Rect(0, 0, image->cols, image->rows).include(roi)) {
                    continue;
                }
                OCRer title(*image, roi);
                title.set_params({ .full_match = true, .without_det = true });
                title.set_required({ expected });
                if (const auto confirmed = title.analyze()) {
                    // Keep the detector rectangle: order bands and queue buttons are anchored to it.
                    label.text = confirmed->front().text;
                }
            }
            const bool has_exact = std::ranges::any_of(*matches, [&](const auto& label) {
                return !rejected(label) && text_contains(label.text, expected);
            });
            bool rescan = false;
            for (const auto& label : *matches) {
                if (rejected(label) || !text_contains(label.text, has_exact ? expected : facility->title)) {
                    continue;
                }
                OCRer edit(*image);
                edit.set_task_info("InfrastPresetEditQueue");
                edit.set_roi({ 430, label.rect.y + label.rect.height, 160, 100 });
                auto buttons = edit.analyze();
                const bool adding = !buttons;
                if (adding) {
                    edit.set_task_info("InfrastPresetOverviewAddQueue");
                    edit.set_roi({ 430, label.rect.y + label.rect.height, 160, 100 });
                    buttons = edit.analyze();
                }
                if (buttons) {
                    if (!same_overview(*image, ctrler()->get_image())) {
                        LogInfo << "Preset overview moved during recognition; refreshing candidates";
                        rescan = true;
                        break;
                    }
                    // Keep order evidence tied to the clicked row in this snapshot; decide only after
                    // the editor confirms its identity. No queue interaction is allowed before then.
                    const auto order =
                        room.facility == "Trade" && room.product != infrast::CustomRoomConfig::Product::Unknown
                            ? match_trade_order(*matches, label.rect, room.product)
                            : RecognitionResult::Matched;
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
                    const auto identity = check_editor_room(room);
                    if (identity == RecognitionResult::Uncertain) {
                        ProcessTask(*this, { "InfrastPresetCloseEditor" }).run();
                        return RoomOpenResult::NotFound;
                    }
                    if (identity == RecognitionResult::Matched) {
                        // The overview add button only opens the editor; creating a slot is left to open_slot.
                        if (adding && queue_count() != std::optional<int>(0)) {
                            LogError << "Preset overview add entry did not open an empty editor" << room.facility
                                     << room.index;
                            ProcessTask(*this, { "InfrastPresetCloseEditor" }).run();
                            return RoomOpenResult::NotFound;
                        }
                        if (order == RecognitionResult::Mismatched) {
                            ProcessTask(*this, { "InfrastPresetCloseEditor" }).run();
                            return RoomOpenResult::ProductFailure;
                        }
                        if (order == RecognitionResult::Uncertain) {
                            if (!ProcessTask(*this, { "InfrastPresetCloseEditor" }).run()) {
                                return RoomOpenResult::NotFound;
                            }
                            if (++uncertain_orders >= 3) {
                                LogError << "Preset trade order recognition exhausted retries" << room.index;
                                return RoomOpenResult::ProductFailure;
                            }
                            // Move an edge row fully into the detection region before reading it again.
                            const auto& overview = Task.get("InfrastPresetOverviewRoom")->roi;
                            if (label.rect.y + label.rect.height + 45 > overview.y + overview.height &&
                                !ProcessTask(*this, { "InfrastPresetOverviewDown" }).run()) {
                                return RoomOpenResult::ProductFailure;
                            }
                            rescan = true;
                            break;
                        }
                        const auto product = match_product(room);
                        if (product != RecognitionResult::Matched) {
                            ProcessTask(*this, { "InfrastPresetCloseEditor" }).run();
                            return RoomOpenResult::ProductFailure;
                        }
                        return RoomOpenResult::Opened;
                    }
                    LogInfo << "Skipping a different preset editor room" << room.facility << room.index;
                    rejected_rows.emplace_back(label.rect);
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
    LogError << "Preset room was not found" << room.facility << room.index;
    return RoomOpenResult::NotFound;
}

asst::InfrastPresetInitTask::RecognitionResult
    asst::InfrastPresetInitTask::match_product(const infrast::PresetRoom& room)
{
    if (room.facility != "Mfg" || room.product == infrast::CustomRoomConfig::Product::Unknown) {
        return RecognitionResult::Matched;
    }
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info("InfrastPresetEditorProduct");
        std::vector<std::string> observed;
        if (const auto results = ocr.analyze()) {
            for (const auto& result : *results) {
                observed.emplace_back(result.text);
            }
        }
        const auto result = classify_product(
            observed,
            room.product,
            { { infrast::CustomRoomConfig::Product::BattleRecord, "作战记录" },
              { infrast::CustomRoomConfig::Product::PureGold, "贵金属" },
              { infrast::CustomRoomConfig::Product::Dualchip, "芯片" },
              { infrast::CustomRoomConfig::Product::OriginiumShard, "源石材料" } });
        LogInfo << "Preset manufacturing product review" << room.index << int(room.product) << observed << int(result)
                << attempt;
        if (result != RecognitionResult::Uncertain) {
            return result;
        }
        if (attempt + 1 < 3 && !sleep(500)) {
            break;
        }
    }
    LogError << "Preset manufacturing product recognition exhausted retries" << room.index;
    return RecognitionResult::Uncertain;
}

asst::InfrastPresetInitTask::RecognitionResult asst::InfrastPresetInitTask::match_trade_order(
    const std::vector<TextRect>& overview_labels,
    const Rect& room_label,
    infrast::CustomRoomConfig::Product expected_product) const
{
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
        observed.emplace_back(label.text);
    }
    // Small order text often loses characters; retain its distinctive suffix instead of requiring
    // the full names "龙门商法" and "开采协力". Conflicting known types are uncertain, not a match.
    const auto result = classify_product(
        observed,
        expected_product,
        { { infrast::CustomRoomConfig::Product::LMD, "商法" },
          { infrast::CustomRoomConfig::Product::Orundum, "协力" } });
    LogInfo << "Preset trade order evidence" << room_label << int(expected_product) << observed << int(result);
    return result;
}

asst::InfrastPresetInitTask::RecognitionResult asst::InfrastPresetInitTask::classify_product(
    const std::vector<std::string>& observed,
    infrast::CustomRoomConfig::Product expected_product,
    std::initializer_list<std::pair<infrast::CustomRoomConfig::Product, std::string_view>> keywords) const
{
    std::optional<infrast::CustomRoomConfig::Product> recognized;
    for (const auto& [product, word] : keywords) {
        if (!std::ranges::any_of(observed, [&](const auto& text) { return text_contains(text, std::string(word)); })) {
            continue;
        }
        if (recognized) {
            return RecognitionResult::Uncertain;
        }
        recognized = product;
    }
    if (!recognized) {
        return RecognitionResult::Uncertain;
    }
    return *recognized == expected_product ? RecognitionResult::Matched : RecognitionResult::Mismatched;
}

bool asst::InfrastPresetInitTask::text_contains(const std::string& text, const std::string& name) const
{
    const auto& config = OcrConfig::get_instance();
    const auto normalized = config.process_equivalence_class(text);
    return normalized.find(config.process_equivalence_class(name)) != std::string::npos;
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
    auto previous = ctrler()->get_image();
    int stable_frames = 0;
    for (int sample = 0; sample < 12 && !need_exit(); ++sample) {
        if (!sleep(250)) {
            return std::nullopt;
        }
        auto current = ctrler()->get_image();
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
    return ProcessTask(*this, { task }).set_retry_times(retry_times).run();
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

std::optional<asst::Rect> asst::InfrastPresetInitTask::find_queue_label(
    const std::string& task,
    const std::vector<std::string>& text,
    bool first_miss_to_top)
{
    for (int page = 0; page < 4 && !need_exit(); ++page) {
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info(task);
        if (!text.empty()) {
            ocr.set_required(text);
        }
        if (const auto found = ocr.analyze()) {
            return found->front().rect;
        }
        const auto scroll = page == 0 && first_miss_to_top ? "InfrastPresetQueueTop" : "InfrastPresetQueueDown";
        if (!ProcessTask(*this, { scroll }).run()) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool asst::InfrastPresetInitTask::open_slot(const infrast::PresetRoom& room, int slot)
{
    // Every caller has just verified the editor identity. Recheck after each page transition below.
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
    const auto found = find_queue_label(
        slot == *count ? "InfrastPresetAddQueue" : "InfrastPresetQueue",
        slot == *count ? std::vector<std::string> {} : labels,
        slot < *count);
    if (!found) {
        return false;
    }
    // Open the first portrait, anchored to the queue label. Avoid trash and swap buttons.
    const auto rect = slot == *count ? *found : found->move(Task.get("InfrastPresetQueue")->rect_move);
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

bool asst::InfrastPresetInitTask::prepare_slot_review()
{
    // Fail closed if the role-based reset cannot be completed; never treat this as a roster mismatch.
    close_quick_formation_expand_role();
    return ProcessTask(*this, { "BattleQuickFormationExpandRole" }).set_retry_times(3).run() &&
           ProcessTask(*this, { "BattleQuickFormationRole-Pioneer" }).run() &&
           ProcessTask(*this, { "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" }).run() &&
           ProcessTask(*this, { "InfrastCloseQuickFormationExpandRole" }).run() && sleep(500) && !need_exit();
}

asst::InfrastPresetInitTask::RecognitionResult
    asst::InfrastPresetInitTask::review_slot(const infrast::CustomRoomConfig& config)
{
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        const auto result = recognize_slot(config);
        if (result != RecognitionResult::Uncertain) {
            return result;
        }
        LogWarn << "Preset roster recognition is uncertain" << attempt;
        if (attempt + 1 < 3 && !sleep(500)) {
            break;
        }
    }
    LogError << "Preset roster recognition exhausted retries";
    return RecognitionResult::Uncertain;
}

asst::InfrastPresetInitTask::RecognitionResult
    asst::InfrastPresetInitTask::recognize_slot(const infrast::CustomRoomConfig& config) const
{
    LogTraceFunction;
    // The caller prepares the page once; clearing selection does not move the list.
    const auto image = ctrler()->get_image();
    InfrastOperImageAnalyzer analyzer(image);
    analyzer.set_to_be_calced(InfrastOperImageAnalyzer::ToBeCalced::Selected);
    if (!analyzer.analyze() || !selected_cards_complete(image, analyzer.get_result())) {
        return RecognitionResult::Uncertain;
    }
    analyzer.sort_by_loc();
    std::vector<std::string> observed;
    const auto& replacement = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    const auto& known_names = BattleData.get_all_oper_names();
    std::set<std::string> unique;
    for (const auto& oper : analyzer.get_result()) {
        if (!oper.selected) {
            continue;
        }
        RegionOCRer name;
        name.set_replace(replacement->replace_map, replacement->replace_full);
        name.set_image(oper.name_img);
        name.set_bin_expansion(0);
        if (!name.analyze()) {
            return RecognitionResult::Uncertain;
        }
        const auto& text = name.get_result().text;
        if (!known_names.contains(text) || !unique.emplace(text).second) {
            LogWarn << "Preset roster contains an unverified operator" << text;
            return RecognitionResult::Uncertain;
        }
        observed.emplace_back(text);
    }
    auto expected = config.names;
    if (!config.sort) {
        std::ranges::sort(observed);
        std::ranges::sort(expected);
    }
    const bool matched = observed == expected;
    LogInfo << "Preset strict roster review" << observed << expected << matched;
    return matched ? RecognitionResult::Matched : RecognitionResult::Mismatched;
}

bool asst::InfrastPresetInitTask::selected_cards_complete(const cv::Mat& image, const std::vector<infrast::Oper>& opers)
    const
{
    constexpr Point origin { 411, 85 }, stride { 144, 281 };
    const auto selected = Task.get<MatchTaskInfo>("InfrastOperSelected");
    if (selected->special_params.empty() || selected->color_scales.size() != 1 ||
        !std::holds_alternative<MatchTaskInfo::ColorRange>(selected->color_scales.front())) {
        return false;
    }
    const auto& color = std::get<MatchTaskInfo::ColorRange>(selected->color_scales.front());
    size_t selected_count = 0;
    // The role reset puts all selected cards on the first page. Scan their fixed border strips
    // independently of smiley detection, so a partially detected roster cannot authorize clearing.
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 6; ++column) {
            const Rect strip { origin.x + column * stride.x,
                               origin.y + row * stride.y,
                               selected->specific_rect.width,
                               selected->specific_rect.height };
            if (strip.empty() || !Rect(0, 0, image.cols, image.rows).include(strip)) {
                return false;
            }
            cv::Mat hsv, mask;
            cv::cvtColor(image(make_rect<cv::Rect>(strip)), hsv, cv::COLOR_BGR2HSV);
            cv::inRange(hsv, color.first, color.second, mask);
            if (cv::countNonZero(mask) < selected->special_params.front()) {
                continue;
            }
            ++selected_count;
            const auto detected = std::ranges::count_if(opers, [&](const auto& oper) {
                return oper.selected && std::abs(oper.rect.x - selected->rect_move.x - strip.x) <= 4 &&
                       std::abs(oper.rect.y - selected->rect_move.y - strip.y) <= 4;
            });
            if (detected != 1) {
                LogWarn << "Preset selected card has no unique detection" << strip << detected;
                return false;
            }
        }
    }
    const auto detected_count = size_t(std::ranges::count_if(opers, &infrast::Oper::selected));
    if (selected_count != detected_count || selected_count > m_capacity) {
        LogWarn << "Preset selected card count is uncertain" << selected_count << detected_count << m_capacity;
        return false;
    }
    return true;
}

bool asst::InfrastPresetInitTask::trim_queues(const infrast::PresetRoom& room)
{
    // Referenced slots include skipped, dynamic and empty placeholders; preserve their queues.
    // Entry follows a verified import_slot; each deletion verifies the editor again below.
    const int expected = room.referenced_slot_count;
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        const auto count = queue_count();
        if (!count) {
            return false;
        }
        if (*count <= expected) {
            // A skipped trailing slot may not exist; cleanup must not create it.
            return true;
        }
        const auto found = find_queue_label("InfrastPresetQueue", { "队列" + std::to_string(*count) }, false);
        if (!found) {
            return false;
        }
        // The trash button is anchored below this exact queue's label, never the swap button.
        ctrler()->click(found->move(Task.get("InfrastPresetDeleteQueue")->rect_move));
        if (!wait_for("InfrastPresetDeleteConfirm", 5) || !wait_for("InfrastPresetEditor", 5) || !check_room(room)) {
            return false;
        }
        sleep(500);
        if (queue_count() != std::optional<int>(*count - 1)) {
            return false;
        }
        LogInfo << "Deleted trailing preset queue" << room.facility << room.index << *count;
    }
    const auto count = queue_count();
    return !need_exit() && count && *count <= expected;
}

bool asst::InfrastPresetInitTask::import_slot(
    const infrast::PresetRoom& room,
    int slot,
    const infrast::CustomRoomConfig& config)
{
    m_fail_reason.clear();
    if (!open_slot(room, slot) || !prepare_slot_review()) {
        return false;
    }
    auto result = review_slot(config);
    if (result == RecognitionResult::Uncertain) {
        ProcessTask(*this, { "InfrastPresetCancelSelection" }).run();
        return false;
    }
    if (result == RecognitionResult::Matched) {
        LogInfo << "Preset queue already matches; preserving selection" << room.facility << room.index << slot;
    }
    else {
        // The shared selector consumes its config; keep the real room index for callbacks.
        infrast::CustomFacilityConfig facility_config(size_t(room.index) + 1);
        facility_config.at(room.index) = config;
        set_custom_config(std::move(facility_config));
        infrast::CustomRoomConfig empty_selection;
        const CustomOperSelectionOptions options {
            .selection_already_cleared = m_capacity > 1,
            .notify_room_operators = false,
            .prepare_selected_opers = [this] { return prepare_slot_review(); },
        };
        if ((m_capacity > 1 && (!click_clear_button() || review_slot(empty_selection) != RecognitionResult::Matched)) ||
            !swipe_and_select_custom_opers_impl(options) || need_exit() || !check_room(room, true)) {
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
        if (!open_slot(room, slot) || !prepare_slot_review()) {
            return false;
        }
        result = review_slot(config);
    }
    const bool closed = ProcessTask(*this, { "InfrastPresetCancelSelection" }).run() &&
                        wait_for("InfrastPresetEditor", 5) && check_room(room);
    return result == RecognitionResult::Matched && closed && !need_exit();
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
    // The overview may be left scrolled anywhere by a previous run; rooms are visited top-down,
    // so pull the list back to the top exactly once before the monotonic scan.
    if (!ProcessTask(*this, { "InfrastPresetBegin" }).run() || need_exit() ||
        !ProcessTask(*this, { "InfrastPresetResetOverview" }).run() || need_exit()) {
        report(need_exit() ? "cancelled" : "failed", "navigation");
        return false;
    }
    // Single monotonic pass: open each room once, verify and fill its queues, then move on.
    for (const auto& room : m_import.rooms) {
        m_slot = room.slots.begin()->first;
        auto opened = open_room(room);
        if (need_exit()) {
            report("cancelled", "room");
            return false;
        }
        if (opened == RoomOpenResult::ProductFailure) {
            report("warning", "product_mismatch");
            report("failed", "product_mismatch");
            return false;
        }
        if (opened == RoomOpenResult::NotFound) {
            report(need_exit() ? "cancelled" : "failed", "room");
            return false;
        }
        report("room");
        for (const auto& [slot, config] : room.slots) {
            m_slot = slot;
            report("progress", "", &config);
            if (!import_slot(room, slot, config)) {
                report(
                    need_exit() ? "cancelled" : "failed",
                    m_fail_reason.empty() ? "selection_or_save" : m_fail_reason,
                    &config);
                return false;
            }
            ++m_completed;
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
