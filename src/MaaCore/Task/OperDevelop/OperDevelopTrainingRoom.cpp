#include "OperDevelopProcessImpl.h"

namespace asst
{

std::optional<OperDevelopTaskProcessImpl::SelectedTrainingSlot>
    OperDevelopTaskProcessImpl::inspect_selected_training_slot(bool assistant)
{
    TrainingAssistantPanel panel(*this);
    using OperListEntry = TrainingAssistantPanel::OperListEntry;

    const std::string open_task = assistant ? "InfrastTrainingOpenAssistant" : "OperDevelopOpenTrainee";
    if (!ProcessTask(*this, { open_task }).run() || !sleep(500)) {
        return std::nullopt;
    }

    save_develop_debug_image(assistant ? "selected_assistant_list" : "selected_trainee_list");

    std::optional<SelectedTrainingSlot> selected;
    const auto page = panel.read_stable_oper_list_page();
    if (page) {
        const auto selected_count = std::ranges::count_if(*page, [](const OperListEntry& entry) {
            return entry.selected && BattleData.find_first_oper(battle::Role::Unknown, entry.name);
        });
        if (selected_count == 1) {
            const auto entry = std::ranges::find_if(*page, [](const OperListEntry& item) {
                return item.selected && BattleData.find_first_oper(battle::Role::Unknown, item.name);
            });
            selected = SelectedTrainingSlot { .name = entry->name };
            if (assistant) {
                selected->percentages = panel.recognize_oper_list_percentages("InfrastTrainingAssistantSkillArea");
            }
        }
    }

    if (!ProcessTask(*this, { "Return" }).run() || !sleep(500)) {
        return std::nullopt;
    }
    if (assistant && !ProcessTask(*this, { "InfrastTrainingAssistantFlag" }).set_retry_times(2).run()) {
        return std::nullopt;
    }
    return selected;
}

int OperDevelopTaskProcessImpl::resolve_live_assistant_bonus(
    std::string_view name,
    const std::vector<int>& percentages,
    int mastery) const
{
    if (mastery < 1 || mastery > 3) {
        return -1;
    }
    const auto groups = training::stage_assistant_groups(m_target_role, mastery);
    const auto group = groups.find(std::string { name });
    if (group == groups.end()) {
        return -1;
    }
    const auto matched = training::match_stage_bonus(percentages, group->second.variants);
    return matched ? matched->first : -1;
}

// Only safe to call once the caller has positively confirmed the room is Processing (the "训练中" badge), since
// the row control this clicks is the countdown toggle while training and the 专精/mastery button while idle.
std::optional<std::string> OperDevelopTaskProcessImpl::recognize_training_countdown(int skill_index)
{
    if (skill_index < 1 || skill_index > 3) {
        return std::nullopt;
    }
    const std::string time_task = std::format("OperDevelopTrainingTime{}", skill_index);
    static const std::regex CountdownPattern(R"((\d{1,3}:\d{2}:\d{2}))");
    const auto read = [&]() -> std::optional<std::string> {
        RegionOCRer analyzer(ctrler()->get_image());
        analyzer.set_task_info(time_task);
        analyzer.set_use_raw(true);
        if (!analyzer.analyze()) {
            return std::nullopt;
        }
        std::smatch match;
        const std::string text = analyzer.get_result().text;
        if (!std::regex_search(text, match, CountdownPattern) ||
            !training::parse_training_countdown_seconds(match[1].str())) {
            return std::nullopt;
        }
        return match[1].str();
    };

    std::optional<std::string> previous;
    bool toggled = false;
    for (int retry = 0; retry < 5 && !need_exit(); ++retry) {
        const auto current = read();
        if (current) {
            if (previous) {
                const auto previous_seconds = training::parse_training_countdown_seconds(*previous);
                const auto current_seconds = training::parse_training_countdown_seconds(*current);
                if (previous_seconds && current_seconds && *current_seconds <= *previous_seconds &&
                    *previous_seconds - *current_seconds <= 5) {
                    return current;
                }
            }
            previous = current;
            sleep(1000);
            continue;
        }
        if (!toggled) {
            const cv::Mat image = ctrler()->get_image();
            const auto control = training_recognition::locate_skill_row_control(image, skill_index, m_skill_images);
            if (!control) {
                Log.info(__FUNCTION__, "could not verify the countdown control location for skill", skill_index);
                return std::nullopt;
            }
            Log.info(__FUNCTION__, "clicking verified countdown toggle", skill_index, *control);
            ctrler()->click(*control);
            sleep(350);
            if (need_exit()) {
                return std::nullopt;
            }
            OCRer cost_page(ctrler()->get_image());
            cost_page.set_task_info("OperDevelopCostFlag");
            if (cost_page.analyze()) {
                recover_to_mastery_list(
                    "countdown toggle unexpectedly opened the mastery material page; recovering",
                    skill_index);
                return std::nullopt;
            }
            toggled = true;
        }
        sleep(350);
    }
    return std::nullopt;
}

// Capture -> classify -> (act) -> re-capture. Any device click invalidates the frame it was decided from, so
// every classification below is re-derived from a fresh frame after that frame's action, never from a stale one.
std::optional<training::TrainingRoomSnapshot> OperDevelopTaskProcessImpl::inspect_training_page()
{
    sleep(300);
    save_develop_debug_image("training_inspection");
    cv::Mat frame = ctrler()->get_image();

    // The game auto-plays a "mastery completed" announcement (chibi art + rank banner + confirm button) when
    // the training room is entered while a finished stage is still unclaimed. It renders over both the
    // mastery-list marker and the processing badge, so it must be checked before either of those positive
    // signals below -- otherwise the page reads as neither idle, processing, nor completed, and this function
    // has no basis to classify it at all (this is exactly what produced training_status_recognition_failed).
    if (const auto announcement = training_recognition::recognize_completion_announcement(frame)) {
        TrainingRoomSnapshot snapshot;
        snapshot.activity = training::TrainingActivity::Completed;
        snapshot.completion_announced = true;
        snapshot.completion_skill_name = announcement->skill_name;
        snapshot.mastery = announcement->rank;
        snapshot.skill_index = m_inspected_training_skill_index;
        snapshot.operator_name = m_inspected_training_operator_name;
        snapshot.trainee_present = !snapshot.operator_name.empty();
        if (snapshot.skill_index >= 1 && snapshot.skill_index <= 3) {
            snapshot.skill_name = std::format("S{}", snapshot.skill_index);
            const auto icon = m_skill_images.find(snapshot.skill_index);
            snapshot.skill_image =
                icon != m_skill_images.end()
                    ? icon->second.clone()
                    : training_recognition::capture_training_skill_image(frame, snapshot.skill_index);
        }
        snapshot.detection_source = "completion_announcement";
        Log.info(
            __FUNCTION__,
            "recognized mastery completion announcement",
            snapshot.completion_skill_name,
            "rank",
            snapshot.mastery);
        const bool announcement_valid = !snapshot.operator_name.empty() && snapshot.mastery >= 1 &&
                                        snapshot.skill_index >= 1 && !snapshot.skill_image.empty();
        if (announcement_valid) {
            return snapshot;
        }
        // The announcement text was legible but we lack the operator/skill context read from the detail page
        // before entering this room. Fall through instead of returning nullopt outright on a partial read; the
        // signals below get their say.
        m_last_training_page_diagnostic = std::format(
            "completion_announcement_recognized_but_context_missing skill={} rank={}",
            snapshot.completion_skill_name,
            snapshot.mastery);
    }

    std::string mastery_list_text;
    std::string processing_text;
    const auto detect_mastery_list = [&](const cv::Mat& image) {
        OCRer analyzer(image);
        analyzer.set_task_info("OperDevelopMasteryListFlag");
        const auto result = analyzer.analyze();
        if (result && !result->empty()) {
            mastery_list_text = result->front().text;
        }
        return result.has_value();
    };
    // "训练中" is the sole positive Processing signal. `InfrastTrainingIdle` belongs to the infrast facility
    // view and never appears here, so it must not be used to infer idleness on this page.
    const auto detect_processing = [&](const cv::Mat& image) {
        RegionOCRer analyzer(image);
        analyzer.set_task_info("OperDevelopTrainingProcessingFlag");
        const auto result = analyzer.analyze();
        if (result) {
            processing_text = result->text;
        }
        return result.has_value();
    };

    bool mastery_list_detected = detect_mastery_list(frame);
    bool processing_detected = detect_processing(frame);

    // Idle is a positive claim (mastery list in view, no training badge), not an inference from the absence of
    // other evidence. Require it to survive a second, freshly captured frame before it is trusted, since a
    // frame captured right after navigation can transiently look idle before the badge renders.
    bool idle_detected = false;
    if (mastery_list_detected && !processing_detected) {
        sleep(400);
        frame = ctrler()->get_image();
        mastery_list_detected = detect_mastery_list(frame);
        processing_detected = detect_processing(frame);
        idle_detected = mastery_list_detected && !processing_detected;
    }

    TrainingRoomSnapshot snapshot;
    snapshot.skill_index = m_inspected_training_skill_index;
    snapshot.mastery = m_inspected_current_mastery;
    const bool valid_skill_index = snapshot.skill_index >= 1 && snapshot.skill_index <= 3;

    std::optional<std::string> stable_countdown;
    if (processing_detected && valid_skill_index) {
        stable_countdown = recognize_training_countdown(snapshot.skill_index);
        // The countdown toggle click may have re-navigated the device (e.g. onto the material page, which
        // recognize_training_countdown already recovers from). Re-anchor every remaining check on a fresh frame.
        frame = ctrler()->get_image();
        mastery_list_detected = detect_mastery_list(frame);
        processing_detected = detect_processing(frame) || stable_countdown.has_value();
        idle_detected = false;
    }

    if (valid_skill_index) {
        snapshot.skill_name = std::format("S{}", snapshot.skill_index);
        snapshot.skill_image = training_recognition::capture_training_skill_image(frame, snapshot.skill_index);
    }
    if (stable_countdown) {
        snapshot.time_left = *stable_countdown;
    }

    if (processing_detected) {
        // Positive page-identity check: the training-room icon for the target skill must actually be on screen.
        // Without this, an operator left over from a previous run in the same room would be reported as ours.
        const bool page_identity_confirmed =
            !valid_skill_index ||
            training_recognition::locate_skill_row_control(frame, snapshot.skill_index, m_skill_images).has_value();
        const auto assistant = inspect_selected_training_slot(true);
        if (assistant) {
            snapshot.assistant_name = assistant->name;
            snapshot.assistant_present = true;
            snapshot.assistant_bonus =
                resolve_live_assistant_bonus(assistant->name, assistant->percentages, snapshot.mastery + 1);
        }
        if (page_identity_confirmed && !m_inspected_training_operator_name.empty()) {
            snapshot.operator_name = m_inspected_training_operator_name;
            snapshot.trainee_present = true;
        }
        else if (!page_identity_confirmed) {
            Log.warn(
                __FUNCTION__,
                "training room icon does not match the expected skill; someone else is occupying it",
                snapshot.skill_index);
            snapshot.trainee_present = snapshot.assistant_present;
            snapshot.operator_name.clear();
            processing_detected = false;
        }
        snapshot.detection_source = "selected_slots";
    }

    // A finished-but-unclaimed stage is intentionally NOT classified here: the mastery-list page never renders
    // a static 训练完成 marker (that badge lives on the facility room view, which this flow does not visit),
    // and the game auto-claims the stage when the skill's row is clicked -- a click the planning flow below
    // always performs before any assistant swap.
    snapshot.activity = training::classify_training_activity(
        idle_detected,
        processing_detected,
        false,
        snapshot.trainee_present,
        snapshot.assistant_present,
        snapshot.mastery >= 1 && snapshot.mastery <= 3 ? std::optional(snapshot.mastery) : std::nullopt);

    if (snapshot.activity == training::TrainingActivity::Idle) {
        return snapshot;
    }
    // `time_left` is best-effort: the interaction to reveal it may fail even while training is genuinely in
    // progress, so a missing readout must not invalidate an otherwise positively-confirmed Processing snapshot.
    const bool processing_valid = snapshot.activity == training::TrainingActivity::Processing &&
                                  !snapshot.operator_name.empty() && snapshot.mastery >= 1 && snapshot.skill_index >= 1;
    if (!processing_valid) {
        m_last_training_page_diagnostic = std::format(
            "activity={} idle={} processing={} mastery_list_text={} processing_text={} "
            "operator={} mastery={}",
            static_cast<int>(snapshot.activity),
            idle_detected,
            processing_detected,
            mastery_list_text,
            processing_text,
            snapshot.operator_name,
            snapshot.mastery);
    }
    return processing_valid ? std::optional<TrainingRoomSnapshot> { std::move(snapshot) } : std::nullopt;
}

bool OperDevelopTaskProcessImpl::wait_for_completion_animation(int expected_mastery)
{
    // Wall-clock fallback only: advancement is still gated on two stable matching frames below, this just
    // bounds how long a slow or stuck animation is allowed to run before giving up. Evidence is the same
    // announcement OCR the caller already used to detect completion in the first place -- no separate
    // confirm-button brightness check.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const Rect stability_area = Task.get("OperDevelopTrainingCompletionStabilityArea")->roi;
    cv::Mat previous;
    int previous_rank = -1;
    int stable_frames = 0;
    while (!need_exit() && std::chrono::steady_clock::now() < deadline) {
        const cv::Mat image = ctrler()->get_image();
        const auto announcement = training_recognition::recognize_completion_announcement(image);
        const cv::Rect bounded = make_rect<cv::Rect>(stability_area) & cv::Rect(0, 0, image.cols, image.rows);
        if (!announcement || announcement->rank != expected_mastery || bounded.empty()) {
            previous.release();
            previous_rank = -1;
            stable_frames = 0;
            sleep(300);
            continue;
        }

        const cv::Mat current = image(bounded).clone();
        bool stable = false;
        if (!previous.empty() && previous_rank == announcement->rank && previous.size() == current.size()) {
            cv::Mat difference;
            cv::absdiff(previous, current, difference);
            const cv::Scalar average = cv::mean(difference);
            stable = (average[0] + average[1] + average[2]) / 3.0 <= 3.0;
        }
        stable_frames = stable ? stable_frames + 1 : 1;
        previous = std::move(current);
        previous_rank = announcement->rank;
        if (stable_frames >= 2) {
            Log.info(__FUNCTION__, "completion animation final frame stable for mastery", announcement->rank);
            return true;
        }
        sleep(300);
    }
    Log.warn(__FUNCTION__, "completion animation did not reach a stable expected final frame", expected_mastery);
    return false;
}

// Dismisses the auto-played completion announcement: waits for its final frame to stabilize, then clicks the
// confirm button with bounded retry, re-checking via training_recognition::recognize_completion_announcement()
// that the modal actually disappeared before trusting the click landed. Only after the modal is gone do we wait
// for a landing page; either the training-room mastery list or the operator detail page is accepted as success.
bool OperDevelopTaskProcessImpl::acknowledge_completion_announcement(int expected_rank)
{
    if (!wait_for_completion_animation(expected_rank)) {
        return false;
    }

    bool dismissed = false;
    for (int attempt = 0; attempt < 3 && !need_exit(); ++attempt) {
        ProcessTask(*this, { "OperDevelopTrainingCompletionConfirm" }).run();
        sleep(300);
        const auto announcement = training_recognition::recognize_completion_announcement(ctrler()->get_image());
        if (!announcement || announcement->rank != expected_rank) {
            dismissed = true;
            break;
        }
        Log.info(__FUNCTION__, "completion announcement still visible after confirm click, attempt", attempt);
    }
    if (!dismissed) {
        Log.warn(__FUNCTION__, "completion announcement did not dismiss after bounded retries", expected_rank);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int landed_frames = 0;
    while (!need_exit() && std::chrono::steady_clock::now() < deadline) {
        OCRer mastery_list(ctrler()->get_image());
        mastery_list.set_task_info("OperDevelopMasteryListFlag");
        const bool landed = mastery_list.analyze().has_value() || locate_mastery_button();
        landed_frames = landed ? landed_frames + 1 : 0;
        if (landed_frames >= 2) {
            Log.info(__FUNCTION__, "landed on a recognized page after completion confirmation");
            return true;
        }
        sleep(250);
    }
    Log.warn(__FUNCTION__, "no recognized landing page after completion confirmation");
    return false;
}

void OperDevelopTaskProcessImpl::emit_training_status(
    const training::TrainingRoomSnapshot& snapshot,
    [[maybe_unused]] std::string_view fallback_operator)
{
    const bool completed = snapshot.activity == training::TrainingActivity::Completed;
    const bool idle = snapshot.activity == training::TrainingActivity::Idle;
    const std::string status = completed                                                     ? "completed"
                               : idle                                                        ? "idle"
                               : snapshot.activity == training::TrainingActivity::Processing ? "processing"
                                                                                             : "occupied";
    std::string message;
    if (completed) {
        message = std::format(
            "{} 的技能「{}」专精 M{} 已完成。执行预检可领取该阶段并规划下一阶段。",
            snapshot.operator_name,
            snapshot.skill_name.empty() ? std::format("S{}", snapshot.skill_index) : snapshot.skill_name,
            snapshot.mastery);
    }
    else if (idle) {
        message =
            snapshot.skill_index >= 1 && snapshot.skill_index <= 3 && snapshot.mastery >= 0 && snapshot.mastery <= 2
                ? std::format(
                      "训练室无进行中的训练；S{} 当前 M{}，可规划继续专精（点击技能行时游戏会自动领取已完成阶段）。",
                      snapshot.skill_index,
                      snapshot.mastery)
                : "训练室无进行中的训练，可规划下一阶段。";
    }
    else if (status == "processing") {
        message = snapshot.skill_name.empty()
                      ? std::format(
                            "{} 正在进行技能专精 M{} → M{}{}。本次不等待，任务已释放。",
                            snapshot.operator_name,
                            snapshot.mastery,
                            snapshot.mastery + 1,
                            snapshot.time_left.empty() ? "" : std::format("，剩余 {}", snapshot.time_left))
                      : std::format(
                            "{} 的技能「{}」正在专精 M{} → M{}{}。本次不等待，任务已释放。",
                            snapshot.operator_name,
                            snapshot.skill_name,
                            snapshot.mastery,
                            snapshot.mastery + 1,
                            snapshot.time_left.empty() ? "" : std::format("，剩余 {}", snapshot.time_left));
    }
    else {
        message = "训练室被其他干员占用且无法确认身份；未执行任何操作。";
    }

    json::value info = basic_info_with_what("OperDevelopTrainingStatus");
    info["details"] = json::object {
        { "message", message },
        { "status", status },
        { "detection_source", snapshot.detection_source },
        { "operator", snapshot.operator_name },
        { "assistant", snapshot.assistant_name },
        { "skill", snapshot.skill_name },
        { "skill_index", snapshot.skill_index },
        { "mastery", snapshot.mastery },
        { "time_left", snapshot.time_left },
        { "trainee_present", snapshot.trainee_present },
        { "assistant_present", snapshot.assistant_present },
        { "assistant_bonus", snapshot.assistant_bonus },
    };
    callback(AsstMsg::SubTaskExtraInfo, info);
}

TraineeSelectionResult
    OperDevelopTaskProcessImpl::select_trainee(std::string_view target_name, battle::Role target_role)
{
    if (!ProcessTask(*this, { "OperDevelopOpenTrainee" }).run()) {
        return TraineeSelectionResult::Failed;
    }

    // Positive page check before scanning: fail fast with a clear reason instead of scanning up to 20 pages of
    // whatever happens to be on screen if the click did not land on the trainee selection page.
    {
        OCRer confirm_present(ctrler()->get_image());
        confirm_present.set_task_info("OperDevelopConfirmTrainee");
        if (!confirm_present.analyze()) {
            Log.warn(__FUNCTION__, "did not land on the trainee selection page after opening it");
            save_failure_image("trainee_selection_page");
            return TraineeSelectionResult::Failed;
        }
    }

    const auto& replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
    const auto name_image_matches = [&](const cv::Mat& name_image) {
        if (name_image.empty()) {
            return false;
        }

        // Single-character operator names are sensitive to the text detector and antialiasing. Recognize the
        // already isolated name region without detection, and retry the foreground threshold while still
        // requiring an exact localized-name match after the shared replacement rules.
        static constexpr std::array<std::pair<int, int>, 5> Thresholds = {
            std::pair { 140, 255 }, std::pair { 110, 255 }, std::pair { 170, 255 },
            std::pair { 0, 120 },   std::pair { 0, 170 },
        };
        for (const auto [lower, upper] : Thresholds) {
            RegionOCRer analyzer(name_image);
            analyzer.set_replace(replace->replace_map, replace->replace_full);
            analyzer.set_bin_threshold(lower, upper);
            analyzer.set_bin_expansion(2);
            const auto result = analyzer.analyze();
            if (result && result->text == target_name) {
                return true;
            }
        }
        OCRer detector(name_image);
        detector.set_replace(replace->replace_map, replace->replace_full);
        const auto results = detector.analyze();
        return results && std::ranges::any_of(*results, [&](const auto& item) { return item.text == target_name; });
    };
    const auto selected_name_matches = [&]() {
        const Rect configured_roi = Task.get("OperDevelopSelectedTraineeNameArea")->roi;
        bool previous_match = false;
        for (int observation = 0; observation < 3 && !need_exit(); ++observation) {
            const cv::Mat image = ctrler()->get_image();
            const cv::Rect name_roi = make_rect<cv::Rect>(configured_roi) & cv::Rect(0, 0, image.cols, image.rows);
            const bool matched = name_roi.width > 0 && name_roi.height > 0 && name_image_matches(image(name_roi));
            if (matched && previous_match) {
                return true;
            }
            previous_match = matched;
            sleep(180);
        }
        return false;
    };
    const auto confirm_selected = [&](bool exact_card_name_verified = false) {
        const bool selection_verified = exact_card_name_verified || selected_name_matches();
        if (!selection_verified || !ProcessTask(*this, { "OperDevelopConfirmTrainee" }).run()) {
            return false;
        }
        return ProcessTask(*this, { "OperDevelopMasteryListFlag" }).set_retry_times(2).run();
    };

    // Opening the trainee list from an operator detail page normally preselects that operator. Verify the
    // detail pane by exact localized name before confirming, so a persisted selection can never be accepted.
    if (confirm_selected()) {
        return TraineeSelectionResult::Ready;
    }

    select_operator_role(target_role);
    if (!close_operator_role_filter()) {
        Log.warn(__FUNCTION__, "trainee role filter panel remained open");
    }
    bool saw_working_operator = false;
    const bool single_character_target =
        std::ranges::count_if(target_name, [](unsigned char ch) { return (ch & 0xc0) != 0x80; }) == 1;
    const auto scan_direction = [&](std::string_view swipe_task) {
        std::string previous_page_signature;
        for (int page = 0; page < 20 && !need_exit(); ++page) {
            const cv::Mat image = ctrler()->get_image();
            OCRer analyzer(image);
            const Rect names_area = Task.get("OperDevelopTraineeNamesArea")->roi;
            analyzer.set_roi(names_area);
            analyzer.set_replace(replace->replace_map, replace->replace_full);
            const auto results = analyzer.analyze();

            std::string page_signature;
            if (results) {
                for (const auto& item : *results) {
                    page_signature.append(item.text);
                    page_signature.append(std::to_string(item.rect.x / 20));
                    page_signature.append(std::to_string(item.rect.y / 20));
                    page_signature.push_back('|');

                    if (item.text == target_name) {
                        Log.info(__FUNCTION__, "recognized available target trainee", target_name, item.rect);
                        ctrler()->click(item.rect);
                        sleep(400);
                        return confirm_selected(true) ? TraineeSelectionResult::Ready : TraineeSelectionResult::Failed;
                    }
                    saw_working_operator = saw_working_operator || item.text.find("工作中") != std::string::npos;
                }
            }

            // The detector can omit a one-glyph name even though the glyph classifier recognizes it once the
            // left detail pane provides a larger region. Probe the normalized visible card grid, but never press
            // Confirm until the exact localized name is recognized from that detail pane.
            if (single_character_target) {
                constexpr int Columns = 6;
                constexpr int Rows = 2;
                const int card_width = names_area.width / Columns;
                const int card_height = names_area.height / Rows;
                for (int row = 0; row < Rows && !need_exit(); ++row) {
                    for (int column = 0; column < Columns && !need_exit(); ++column) {
                        const Rect card {
                            names_area.x + column * card_width,
                            names_area.y + row * card_height,
                            card_width,
                            card_height,
                        };
                        const bool working = results && std::ranges::any_of(*results, [&](const auto& item) {
                                                 const int center_x = item.rect.x + item.rect.width / 2;
                                                 const int center_y = item.rect.y + item.rect.height / 2;
                                                 return item.text.find("工作中") != std::string::npos &&
                                                        center_x >= card.x && center_x < card.x + card.width &&
                                                        center_y >= card.y && center_y < card.y + card.height;
                                             });
                        const Rect click_area {
                            card.x + card.width / 2 - 10,
                            card.y + std::min(100, card.height / 2) - 10,
                            20,
                            20,
                        };
                        ctrler()->click(click_area);
                        sleep(400);
                        if (!selected_name_matches()) {
                            continue;
                        }
                        Log.info(
                            __FUNCTION__,
                            "confirmed single-character target trainee from detail",
                            target_name,
                            card);
                        if (working) {
                            return TraineeSelectionResult::WorkingElsewhere;
                        }
                        return confirm_selected(true) ? TraineeSelectionResult::Ready : TraineeSelectionResult::Failed;
                    }
                }
            }

            if (!previous_page_signature.empty() && page_signature == previous_page_signature) {
                Log.info(__FUNCTION__, "trainee list reached scan boundary on page", page);
                return TraineeSelectionResult::NotFound;
            }
            previous_page_signature = std::move(page_signature);
            if (!ProcessTask(*this, { std::string(swipe_task) }).run() || need_exit()) {
                return TraineeSelectionResult::Failed;
            }
        }
        return TraineeSelectionResult::NotFound;
    };

    const auto right = scan_direction("InfrastOperListSlowlySwipeToTheRight");
    if (right == TraineeSelectionResult::Ready || right == TraineeSelectionResult::WorkingElsewhere) {
        return right;
    }
    const auto left = scan_direction("OperDevelopSwipeToTheLeft");
    if (left == TraineeSelectionResult::Ready || left == TraineeSelectionResult::WorkingElsewhere) {
        return left;
    }
    if (need_exit() || (right == TraineeSelectionResult::Failed && left == TraineeSelectionResult::Failed)) {
        return TraineeSelectionResult::Failed;
    }
    if (single_character_target) {
        save_failure_image("single_character_trainee");
    }
    return saw_working_operator ? TraineeSelectionResult::PossiblyWorkingElsewhere : TraineeSelectionResult::NotFound;
}

} // namespace asst
