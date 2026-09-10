#include "OperDevelopProcessImpl.h"

namespace asst
{

bool OperDevelopTaskProcessImpl::navigate_to_operator(const std::shared_ptr<battle::OperProps>& oper)
{
    m_operator_navigation_failure.clear();
    const auto matches_task = [&](std::string_view task_name) {
        ProcessTask detector(*this, { std::string(task_name) });
        detector.set_retry_times(0).set_ignore_error(true);
        return detector.run();
    };
    const auto is_selection_page = [&]() {
        OCRer analyzer(ctrler()->get_image());
        analyzer.set_roi(Task.get("OperDevelopConfirmTrainee")->roi);
        analyzer.set_required({ "确认" });
        const auto results = analyzer.analyze();
        return results && std::ranges::any_of(*results, [](const auto& item) { return item.text == "确认"; });
    };
    const auto is_infrast_operator_page = [&]() {
        OCRer analyzer(ctrler()->get_image());
        analyzer.set_required({ "当前房间入住信息", "控制中枢", "进驻信息", "清空" });
        const auto results = analyzer.analyze();
        if (!results) {
            return false;
        }
        std::unordered_set<std::string> recognized;
        for (const auto& result : *results) {
            if (result.text == "当前房间入住信息" || result.text == "控制中枢" || result.text == "进驻信息" ||
                result.text == "清空") {
                recognized.emplace(result.text);
            }
        }
        return recognized.contains("当前房间入住信息") ||
               (recognized.contains("控制中枢") && recognized.contains("清空"));
    };
    const auto is_operator_list_page = [&]() {
        if (is_selection_page() || is_infrast_operator_page()) {
            return false;
        }
        const cv::Mat image = ctrler()->get_image();
        OCRer sort_labels(image);
        sort_labels.set_roi({ 650, 0, 570, 100 });
        sort_labels.set_required({ "等级", "稀有度", "自定义排序" });
        const auto sort_results = sort_labels.analyze();
        const int matched_sort_labels =
            sort_results ? static_cast<int>(std::ranges::count_if(
                               *sort_results,
                               [](const auto& item) {
                                   return item.text == "等级" || item.text == "稀有度" || item.text == "自定义排序";
                               }))
                         : 0;
        if (matched_sort_labels >= 2) {
            Log.info(__FUNCTION__, "recognized operator list from sort labels", matched_sort_labels);
            return true;
        }

        // The expanded quick-navigation overlay hides the sort labels but leaves most roster cards visible.
        OCRer roster_names(image);
        const auto& replace = Task.get<OcrTaskInfo>("CharsNameOcrReplace");
        roster_names.set_replace(replace->replace_map, replace->replace_full);
        const auto name_results = roster_names.analyze();
        std::unordered_set<std::string> names;
        if (name_results) {
            for (const auto& result : *name_results) {
                if (BattleData.find_first_oper(battle::Role::Unknown, result.text)) {
                    names.emplace(result.text);
                }
            }
        }
        if (names.size() >= 4) {
            Log.info(__FUNCTION__, "recognized operator list beneath quick-navigation overlay", names.size());
            return true;
        }
        return matches_task("ParadoxReturnOperListFlag");
    };
    const auto is_operator_detail_page = [&]() {
        const cv::Mat image = ctrler()->get_image();
        OCRer right_panel(image);
        right_panel.set_roi({ 780, 160, 500, 540 });
        right_panel.set_required({ "精英化", "潜能" });
        const auto right_results = right_panel.analyze();
        const bool standard_detail =
            right_results &&
            std::ranges::any_of(*right_results, [](const auto& item) { return item.text == "精英化"; }) &&
            std::ranges::any_of(*right_results, [](const auto& item) { return item.text == "潜能"; });
        if (standard_detail) {
            return true;
        }

        // The expanded skill panel covers the standard right-side markers. Trust the operator-only trust label
        // on the left instead; it is not present on the roster or training-room pages.
        OCRer left_panel(image);
        left_panel.set_roi({ 0, 180, 520, 360 });
        left_panel.set_required({ "信赖值" });
        const auto left_results = left_panel.analyze();
        return left_results &&
               std::ranges::any_of(*left_results, [](const auto& item) { return item.text == "信赖值"; });
    };
    // A previous failed/stopped run can leave the game on a deep screen (material popup, trainee list,
    // role-filter overlay, the leave-infrastructure confirm). From there OperBoxBegin's own next-chain has
    // been observed to ping-pong (QuickSwitch Entry/Open cycling for minutes). Collapse the state first:
    // confirm the leave-infrast popup when present (it blocks everything else and its red button does not
    // match the blue PopupConfirm template), then press Return; afterwards reach the roster through a
    // deadline-bounded loop that nudges with Return between attempts.
    settle_leave_infrast(4);
    const auto begin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    bool reached_list = false;
    while (!need_exit()) {
        const bool began = ProcessTask(*this, { "OperBoxBegin" }).set_retry_times(1).set_ignore_error(true).run();
        if (began && is_operator_list_page()) {
            reached_list = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= begin_deadline) {
            break;
        }
        settle_leave_infrast(1, 1, 500);
    }
    if (!reached_list) {
        Log.warn(__FUNCTION__, "failed to reach the operator list via OperBoxBegin");
        m_operator_navigation_failure = "无法从当前界面进入干员列表，已终止。";
        save_failure_image("operator_navigation");
        return false;
    }

    m_operator_role_filter_used = select_operator_role(oper->role);
    if (!m_operator_role_filter_used) {
        Log.warn(__FUNCTION__, "failed to apply in-game role filter, falling back to full operator scan");
        save_failure_image("role_filter");
    }
    const bool role_filter_collapsed = close_operator_role_filter();
    if (!role_filter_collapsed) {
        Log.warn(__FUNCTION__, "role filter panel remained open; using the resource fallback card ROIs");
    }

    struct RosterPage
    {
        std::optional<Rect> target;
        std::optional<int> target_elite;
        std::string signature;
    };

    const auto analyze_roster_page = [&]() -> std::optional<RosterPage> {
        OperBoxImageAnalyzer analyzer(ctrler()->get_image());
        const auto roi_prefix = role_filter_collapsed ? "OperDevelopRoleFilterFull" : "OperDevelopRoleFilterFallback";
        analyzer.set_role_rois(
            Task.get(std::string(roi_prefix) + "TopROI")->roi,
            Task.get(std::string(roi_prefix) + "BottomROI")->roi);
        if (m_operator_role_filter_used) {
            analyzer.set_role_filter(oper->role);
        }
        if (!analyzer.analyze() || analyzer.get_result().empty()) {
            return std::nullopt;
        }

        RosterPage page;
#ifdef ASST_DEBUG
        for (const auto& box : analyzer.get_result()) {
            Log.info(__FUNCTION__, "operator roster card", box.id, box.name, box.rect, "elite", box.elite);
        }
#endif
        for (const auto& box : analyzer.get_result()) {
            page.signature.append(box.id);
            page.signature.push_back('|');
            if (box.id == oper->id && box.name == oper->name && box.rarity == oper->rarity) {
                page.target = box.rect;
                page.target_elite = box.elite;
            }
        }
        return page;
    };
    const auto scan_direction = [&](std::string_view swipe_task) {
        std::string previous_signature;
        std::string previous_previous_signature;
        for (int page_index = 0; page_index < 15 && !need_exit(); ++page_index) {
            auto page = analyze_roster_page();
            if (!page) {
                sleep(300);
                page = analyze_roster_page();
            }
            if (!page) {
                Log.warn(__FUNCTION__, "failed to analyze operator roster page", page_index);
                return false;
            }

            if (page->target) {
                // Reuse the operator-recognition tool's card analyzer and its two-frame confirmation pattern.
                // Matching the battle-data id, localized name, rarity and stable card location is required before
                // clicking; there is deliberately no single-frame or partial-name fallback here.
                sleep(500);
                const auto confirmed = analyze_roster_page();
                if (confirmed && confirmed->target && confirmed->target_elite && page->target_elite &&
                    *confirmed->target_elite == *page->target_elite &&
                    std::abs(confirmed->target->x - page->target->x) <= 24 &&
                    std::abs(confirmed->target->y - page->target->y) <= 24) {
                    m_operator_elite = *confirmed->target_elite;
                    Log.info(
                        __FUNCTION__,
                        "confirmed target operator card in two frames",
                        oper->name,
                        *confirmed->target,
                        "elite",
                        m_operator_elite);
                    ctrler()->click(*confirmed->target);
                    for (int retry = 0; retry < 5 && !need_exit(); ++retry) {
                        sleep(retry == 0 ? 800 : 300);
                        if (is_operator_detail_page()) {
                            Log.info(
                                __FUNCTION__,
                                "verified operator detail-page structure after card click",
                                oper->name);
                            return true;
                        }
                    }
                    Log.warn(__FUNCTION__, "operator card click did not open a detail page", oper->name);
                    m_operator_navigation_failure =
                        std::format("已在干员列表中找到{}，但无法打开干员详情，已终止。", oper->name);
                    return false;
                }
                Log.warn(__FUNCTION__, "target operator card was not stable across two frames", oper->name);
            }

            if (!previous_signature.empty() && page->signature == previous_signature &&
                previous_signature == previous_previous_signature) {
                Log.info(__FUNCTION__, "operator roster reached scan boundary on page", page_index);
                return false;
            }
            previous_previous_signature = previous_signature;
            previous_signature = std::move(page->signature);
            if (!ProcessTask(*this, { std::string(swipe_task) }).run() || need_exit()) {
                return false;
            }
        }
        return false;
    };

    // Role switching preserves the horizontal position. A trailing card can be partially hidden at the right edge,
    // where its name ROI would overlap the filter area or leave the screen. Move cards left first so this edge card
    // becomes fully visible in the existing safe OperBox ROI, then scan back toward the leading side.
    if (scan_direction("OperBoxSlowlySwipeToTheRight") || scan_direction("OperDevelopSwipeToTheLeft")) {
        return true;
    }
    save_failure_image("operator_search");
    return false;
}

bool OperDevelopTaskProcessImpl::expand_skill_panel()
{
    const auto locate_compact_expander = [&](const cv::Mat& image) -> std::optional<Rect> {
        if (!recognize_skill_rank_7(image)) {
            return std::nullopt;
        }
        const Rect area = Task.get("OperDevelopSkillPanelExpandArea")->roi;
        const cv::Rect cv_area = make_rect<cv::Rect>(area) & cv::Rect(0, 0, image.cols, image.rows);
        if (cv_area.width <= 0 || cv_area.height <= 0) {
            return std::nullopt;
        }

        cv::Mat gray;
        cv::cvtColor(image(cv_area), gray, cv::COLOR_BGR2GRAY);
        cv::Mat binary;
        cv::threshold(gray, binary, 90, 255, cv::THRESH_BINARY);
        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int components = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
        const bool chevron_found = std::ranges::any_of(std::views::iota(1, components), [&](int component) {
            const int area = stats.at<int>(component, cv::CC_STAT_AREA);
            const int width = stats.at<int>(component, cv::CC_STAT_WIDTH);
            const int height = stats.at<int>(component, cv::CC_STAT_HEIGHT);
            return area >= 4 && area <= 140 && width >= 3 && width <= 28 && height >= 2 && height <= 15;
        });
        const Rect click_area {
            area.x + area.width / 2 - 5,
            area.y + area.height / 2 - 5,
            10,
            10,
        };
        return chevron_found ? std::optional<Rect>(click_area) : std::nullopt;
    };

    // Wall-clock fallback instead of a fixed retry count: both branches below still require a positive match
    // (and the compact-expander branch requires it twice) before acting.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!need_exit() && std::chrono::steady_clock::now() < deadline) {
        const cv::Mat image = ctrler()->get_image();
        OCRer analyzer(image);
        analyzer.set_roi(Task.get("OperDevelopSkillPanelHeaderArea")->roi);
        analyzer.set_required({ "技能" });
        const auto results = analyzer.analyze();
        if (results) {
            const auto exact = std::ranges::find_if(*results, [](const auto& item) { return item.text == "技能"; });
            if (exact != results->end()) {
                Log.info(__FUNCTION__, "recognized skill panel header at", exact->rect);
                ctrler()->click(exact->rect);
                sleep(400);
                return true;
            }
        }

        // Some compact operator layouts expose only RANK 7 and a small chevron below the skill cards.
        // Confirm the expander in two fresh frames before clicking the tight ROI, so animation or OCR noise
        // cannot cause a nearby action to be triggered.
        const auto first_expander = locate_compact_expander(image);
        if (first_expander) {
            sleep(200);
            const auto confirmed_expander = locate_compact_expander(ctrler()->get_image());
            if (confirmed_expander) {
                Log.info(__FUNCTION__, "confirmed compact skill panel expander at", *confirmed_expander);
                ctrler()->click(*confirmed_expander);
                sleep(500);
                if (locate_mastery_button()) {
                    return true;
                }
            }
        }
        sleep(200);
    }
    return false;
}

bool OperDevelopTaskProcessImpl::select_operator_role(battle::Role role)
{
    // 职业显示名统一取自上游 enum_to_string，避免本地重复维护映射
    static const std::vector<std::string> RoleDisplayNames = [] {
        std::vector<std::string> names;
        for (auto role : { battle::Role::Pioneer,
                           battle::Role::Warrior,
                           battle::Role::Tank,
                           battle::Role::Sniper,
                           battle::Role::Caster,
                           battle::Role::Medic,
                           battle::Role::Support,
                           battle::Role::Special }) {
            names.emplace_back(enum_to_string(role, false));
        }
        return names;
    }();

    const std::string role_task = battle::get_role_task_name(role);
    if (role_task.empty()) {
        return false;
    }
    const std::string role_display = enum_to_string(role, false);
    OCRer current_filter(ctrler()->get_image());
    current_filter.set_roi(Task.get("OperDevelopRoleFilterArea")->roi);
    std::vector<std::string> filter_names { "ALL" };
    for (const auto& name : RoleDisplayNames) {
        filter_names.emplace_back(name);
    }
    current_filter.set_required(filter_names);
    const auto current_results = current_filter.analyze();
    if (current_results) {
        if (std::ranges::any_of(*current_results, [&](const auto& item) { return item.text == role_display; })) {
            Log.info(__FUNCTION__, "target role filter is already selected", role_display);
            return true;
        }
        const auto current = std::ranges::find_if(*current_results, [&](const auto& item) {
            return item.text == "ALL" ||
                   std::ranges::any_of(RoleDisplayNames, [&](const auto& name) { return item.text == name; });
        });
        if (current != current_results->end()) {
            Log.info(__FUNCTION__, "recognized current role filter at", current->rect);
            ctrler()->click(current->rect);
            sleep(500);
        }
    }

    ProcessTask(*this, { "BattleQuickFormationExpandRole" }).set_retry_times(3).set_ignore_error(true).run();
    if (!ProcessTask(*this, { "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" })
             .set_retry_times(2)
             .run()) {
        return false;
    }
    return ProcessTask(*this, { "BattleQuickFormationRole-" + role_task }).set_retry_times(1).run();
}

bool OperDevelopTaskProcessImpl::close_operator_role_filter()
{
    Matcher collapse_button(ctrler()->get_image());
    collapse_button.set_task_info("InfrastCloseQuickFormationExpandRole");
    if (const auto button = collapse_button.analyze()) {
        Log.info(__FUNCTION__, "closing operator role filter panel", button->rect);
        ctrler()->click(button->rect);
        sleep(500);

        // The collapse template was positively matched before the click. Its disappearance is a direct panel-state
        // confirmation even on layouts where the small expand icon is not recognized by the generic OperBox task.
        Matcher still_open(ctrler()->get_image());
        still_open.set_task_info("InfrastCloseQuickFormationExpandRole");
        const bool closed = !still_open.analyze().has_value();
        Log.info(__FUNCTION__, "operator role filter panel closed", closed, "verification", "collapse_template_absent");
        return closed;
    }

    Matcher expand_button(ctrler()->get_image());
    expand_button.set_task_info("OperBoxRoleTab");
    const bool closed = expand_button.analyze().has_value();
    Log.info(__FUNCTION__, "operator role filter panel closed", closed, "verification", "expand_template");
    return closed;
}

namespace
{
// English-label OCR (MASTERY RANK / RANK 7) is matched after normalizing away whitespace/punctuation noise.
std::string normalized_alnum_upper(const std::string& text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char ch : text) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::toupper(ch)));
        }
    }
    return normalized;
}
}

// Confirms the leave-infrastructure popup when present, then presses Return, `rounds` times — the shared
// "collapse whatever deep screen a previous run left" nudge before navigating somewhere specific.
void OperDevelopTaskProcessImpl::settle_leave_infrast(int rounds, int return_retry_times, int interval_ms)
{
    for (int i = 0; i < rounds && !need_exit(); ++i) {
        ProcessTask(*this, { "OperDevelopLeaveInfrastConfirm" }).set_retry_times(0).set_ignore_error(true).run();
        ProcessTask(*this, { "Return" }).set_retry_times(return_retry_times).set_ignore_error(true).run();
        sleep(interval_ms);
    }
}

bool OperDevelopTaskProcessImpl::locate_mastery_button()
{
    m_mastery_button_rect.reset();
    const cv::Mat image = ctrler()->get_image();
    OCRer analyzer(image);
    analyzer.set_roi(Task.get("OperDevelopMasteryButtonArea")->roi);
    analyzer.set_required({ "技能专精", "训练室占用中" });
    const auto results = analyzer.analyze();
    if (results && !results->empty()) {
        m_mastery_button_rect = results->front().rect;
        Log.info(__FUNCTION__, "recognized Chinese mastery or occupied-training entry at", *m_mastery_button_rect);
        return true;
    }

    // The current CN operator detail UI can render this action as the English label "MASTERY RANK". Use the
    // character model as a fallback and normalize whitespace/punctuation instead of relying on a localized word.
    OCRer english(image);
    english.set_roi(Task.get("OperDevelopMasteryButtonArea")->roi);
    english.set_use_char_model(true);
    english.set_use_raw(true);
    const auto english_results = english.analyze();
    if (!english_results) {
        return false;
    }
    for (const auto& result : *english_results) {
        if (normalized_alnum_upper(result.text).find("MASTERYRANK") == std::string::npos) {
            continue;
        }
        m_mastery_button_rect = result.rect;
        Log.info(__FUNCTION__, "recognized English mastery button at", *m_mastery_button_rect);
        return true;
    }
    return false;
}

bool OperDevelopTaskProcessImpl::click_mastery_button()
{
    if (!locate_mastery_button() || !m_mastery_button_rect) {
        return false;
    }
    const Rect button = *m_mastery_button_rect;
    m_mastery_button_rect.reset();
    ctrler()->click(button);
    sleep(800);
    return true;
}

bool OperDevelopTaskProcessImpl::recognize_skill_rank_7(const cv::Mat& image)
{
    OCRer analyzer(image.empty() ? ctrler()->get_image() : image);
    analyzer.set_roi(Task.get("OperDevelopSkillRankArea")->roi);
    analyzer.set_use_char_model(true);
    analyzer.set_use_raw(true);
    const auto results = analyzer.analyze();
    if (!results) {
        return false;
    }

    std::vector<Rect> rank_rects;
    std::vector<Rect> seven_rects;
    for (const auto& result : *results) {
        const std::string normalized = normalized_alnum_upper(result.text);
        if (normalized.find("RANK7") != std::string::npos) {
            Log.info(__FUNCTION__, "recognized combined RANK 7 at", result.rect);
            return true;
        }
        if (normalized == "RANK") {
            rank_rects.emplace_back(result.rect);
        }
        else if (normalized == "7") {
            seven_rects.emplace_back(result.rect);
        }
    }

    for (const auto& rank : rank_rects) {
        const int rank_center_y = rank.y + rank.height / 2;
        for (const auto& seven : seven_rects) {
            const int seven_center_y = seven.y + seven.height / 2;
            const bool same_line = std::abs(rank_center_y - seven_center_y) <= 35;
            const bool follows_rank = seven.x >= rank.x + rank.width - 15 && seven.x <= rank.x + rank.width + 80;
            if (same_line && follows_rank) {
                Log.info(__FUNCTION__, "recognized split RANK 7 at", rank, seven);
                return true;
            }
        }
    }
    return false;
}

bool OperDevelopTaskProcessImpl::capture_skill_image(const cv::Mat& image, int skill_index)
{
    const Rect roi = Task.get("OperDevelopSkillIcon" + std::to_string(skill_index))->roi;
    const cv::Rect cv_roi = make_rect<cv::Rect>(roi) & cv::Rect(0, 0, image.cols, image.rows);
    if (cv_roi.width <= 0 || cv_roi.height <= 0) {
        return false;
    }
    m_skill_images[skill_index] = image(cv_roi).clone();
    return !m_skill_images.at(skill_index).empty();
}

// A stray cost-page (是否专精该技能) can linger from an aborted attempt; both entry points recover the same
// way: back to the skill list and re-verify the mastery-list flag before continuing.
bool OperDevelopTaskProcessImpl::recover_to_mastery_list(std::string_view reason, int skill_index)
{
    Log.warn(__FUNCTION__, reason, skill_index);
    if (!ProcessTask(*this, { "Return" }).run() || need_exit() ||
        !ProcessTask(*this, { "OperDevelopMasteryListFlag" }).set_retry_times(2).run()) {
        return false;
    }
    return true;
}

bool OperDevelopTaskProcessImpl::choose_mastery_skill(int skill_index)
{
    for (int retry = 0; retry < 3 && !need_exit(); ++retry) {
        const cv::Mat image = ctrler()->get_image();
        OCRer cost_page(image);
        cost_page.set_task_info("OperDevelopCostFlag");
        if (cost_page.analyze()) {
            if (!recover_to_mastery_list(
                    "found an unverified mastery material page; returning to the skill list before selecting",
                    skill_index)) {
                return false;
            }
            continue;
        }

        OCRer mastery_list(image);
        mastery_list.set_task_info("OperDevelopMasteryListFlag");
        if (!mastery_list.analyze()) {
            sleep(300);
            continue;
        }

        const auto mastery_button = training_recognition::locate_skill_row_control(image, skill_index, m_skill_images);
        if (!mastery_button) {
            sleep(300);
            continue;
        }

        Log.info(__FUNCTION__, "clicking verified mastery button", skill_index, *mastery_button);
        ctrler()->click(*mastery_button);
        sleep(700);
        if (need_exit()) {
            return false;
        }
        if (ProcessTask(*this, { "OperDevelopCostFlag" }).set_retry_times(1).run()) {
            return true;
        }

        OCRer still_on_list(ctrler()->get_image());
        still_on_list.set_task_info("OperDevelopMasteryListFlag");
        if (!still_on_list.analyze()) {
            return false;
        }
        sleep(300);
    }
    save_failure_image("skill_feature_match");
    return false;
}

std::optional<int> OperDevelopTaskProcessImpl::recognize_mastery(int skill_index)
{
    std::array<std::optional<int>, 3> observations;
    for (auto& observation : observations) {
        observation = training_recognition::recognize_mastery_frame(ctrler()->get_image(), skill_index);
        if (!observation) {
            return std::nullopt;
        }
        sleep(120);
    }
    const auto mastery = training::stable_mastery(observations);
    if (!mastery) {
        Log.warn(__FUNCTION__, "mastery observations are unstable", skill_index);
    }
    return mastery;
}

// Require two consecutive matching material observations before trusting the page. Every cell must resolve
// (Parsed or positively Absent) and every parsed quantity must cover its requirement before this returns true;
// anything else keeps consumption gated off. The confirm button rect comes from the accepted frame.
bool OperDevelopTaskProcessImpl::read_material_page()
{
    m_material_confirm_rect.reset();
    m_last_level_requirement.reset();
    std::optional<training::MaterialSlotFrame> previous_frame;
    std::optional<training::MaterialRequirement> previous_level;
    static constexpr int MaxObservations = 4;

    for (int observation = 0; observation < MaxObservations && !need_exit(); ++observation) {
        const cv::Mat image = ctrler()->get_image();
        m_material_confirm_rect = training_recognition::recognize_material_confirm_button(image);
        m_last_material_validation.frame = training_recognition::recognize_material_slots(image);
        m_last_level_requirement = training_recognition::recognize_level_requirement(image);

        const size_t expected_count =
            std::ranges::count_if(m_last_material_validation.frame, [](const training::MaterialCellReading& cell) {
                return cell.state != training::MaterialCellState::Absent;
            });
        const bool stable = m_last_material_validation.complete() && previous_frame &&
                            *previous_frame == m_last_material_validation.frame &&
                            previous_level == m_last_level_requirement;
        if (stable) {
            if (!m_last_material_validation.sufficient()) {
                Log.info(__FUNCTION__, "material quantities are stable but insufficient", expected_count);
                return false;
            }
            if (m_material_confirm_rect) {
                Log.info(
                    __FUNCTION__,
                    "material quantities are stable and sufficient",
                    expected_count,
                    "observation",
                    observation + 1);
                return true;
            }
            Log.info(__FUNCTION__, "material quantities are sufficient but confirmation button is not ready");
        }

        if (m_last_material_validation.complete()) {
            previous_frame = m_last_material_validation.frame;
            previous_level = m_last_level_requirement;
        }
        else {
            previous_frame.reset();
            previous_level.reset();
        }
        if (observation + 1 < MaxObservations) {
            sleep(180);
        }
    }

    const size_t expected_count =
        std::ranges::count_if(m_last_material_validation.frame, [](const training::MaterialCellReading& cell) {
            return cell.state != training::MaterialCellState::Absent;
        });
    Log.info(
        __FUNCTION__,
        "material quantities did not reach a stable readable state; refusing to proceed",
        expected_count,
        m_material_confirm_rect.has_value());
    save_failure_image("material_ocr");
    return false;
}

bool OperDevelopTaskProcessImpl::click_material_confirm()
{
    if (!m_material_confirm_rect) {
        return false;
    }
    const Rect button = *m_material_confirm_rect;
    m_material_confirm_rect.reset();
    ctrler()->click(button);
    sleep(1000);
    return true;
}

} // namespace asst
