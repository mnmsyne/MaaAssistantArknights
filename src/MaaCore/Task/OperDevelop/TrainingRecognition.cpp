#include "TrainingRecognition.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <regex>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/TaskData.h"
#include "Utils/Logger.hpp"
#include "Utils/StringMisc.hpp"
#include "Vision/FeatureMatcher.h"
#include "Vision/Matcher.h"
#include "Vision/OCRer.h"
#include "Vision/RegionOCRer.h"

using namespace asst::training;

cv::Mat asst::training_recognition::capture_training_skill_image(const cv::Mat& image, int skill_index)
{
    if (skill_index < 1 || skill_index > 3) {
        return {};
    }
    const std::array skill_rois { Rect { 488, 100, 110, 110 },
                                  Rect { 488, 305, 110, 110 },
                                  Rect { 488, 505, 110, 110 } };
    const Rect& roi = skill_rois.at(static_cast<size_t>(skill_index - 1));
    const cv::Rect bounded = make_rect<cv::Rect>(roi) & cv::Rect(0, 0, image.cols, image.rows);
    return bounded.width > 0 && bounded.height > 0 ? image(bounded).clone() : cv::Mat {};
}

std::optional<CompletionAnnouncement>
    asst::training_recognition::recognize_completion_announcement(const cv::Mat& image)
{
    OCRer analyzer(image);
    analyzer.set_roi(Task.get("OperDevelopTrainingCompletionResultArea")->roi);
    analyzer.set_use_raw(true);
    const auto results = analyzer.analyze();
    if (!results) {
        return std::nullopt;
    }

    std::string joined;
    for (const auto& result : *results) {
        joined += result.text;
    }
    std::erase_if(joined, [](unsigned char ch) { return std::isspace(ch); });
    static const std::regex CompletionPattern("技能(.+?)已完成专精等级([123])");
    std::smatch match;
    int rank = 0;
    if (!std::regex_search(joined, match, CompletionPattern) || !utils::chars_to_number(match[2].str(), rank) ||
        rank < 1 || rank > 3) {
        return std::nullopt;
    }
    return CompletionAnnouncement { .rank = rank, .skill_name = match[1].str() };
}

std::optional<asst::Rect> asst::training_recognition::locate_skill_row_control(
    const cv::Mat& image,
    int skill_index,
    const std::unordered_map<int, cv::Mat>& skill_images)
{
    const auto skill_image = skill_images.find(skill_index);
    if (skill_image == skill_images.end() || skill_image->second.empty() || skill_index < 1 || skill_index > 3) {
        return std::nullopt;
    }

    const Rect search_roi = Task.get("InfrastTrainingChooseSkillRec")->roi;
    const auto is_expected_icon = [&](const Rect& matched) {
        const int row_height = search_roi.height / 3;
        const int row_top = search_roi.y + (skill_index - 1) * row_height;
        const int row_bottom = skill_index == 3 ? search_roi.y + search_roi.height : row_top + row_height;
        const int center_y = matched.y + matched.height / 2;
        return matched.width >= 60 && matched.width <= 115 && matched.height >= 75 && matched.height <= 130 &&
               matched.x >= search_roi.x - 10 && matched.x + matched.width <= search_roi.x + search_roi.width + 10 &&
               center_y >= row_top && center_y < row_bottom;
    };

    std::optional<Rect> matched;
    {
        FeatureMatcher analyzer(image);
        analyzer.set_task_info("InfrastTrainingChooseSkillRec");
        analyzer.set_templ(skill_image->second);
        if (analyzer.analyze()) {
            const auto& results = analyzer.get_result();
            const auto found =
                std::ranges::find_if(results, [&](const auto& result) { return is_expected_icon(result.rect); });
            if (found != results.end()) {
                matched = found->rect;
            }
        }
    }
    if (!matched) {
        std::array<double, 3> best_scores { -1.0, -1.0, -1.0 };
        std::array<Rect, 3> best_rects;
        constexpr std::array<double, 3> Scales { 1.05, 1.10, 1.15 };
        const int row_height = search_roi.height / 3;
        for (const double scale : Scales) {
            cv::Mat resized;
            cv::resize(
                skill_image->second,
                resized,
                cv::Size(
                    static_cast<int>(std::lround(skill_image->second.cols * scale)),
                    static_cast<int>(std::lround(skill_image->second.rows * scale))));
            for (int row = 0; row < 3; ++row) {
                const int row_bottom =
                    row == 2 ? search_roi.y + search_roi.height : search_roi.y + (row + 1) * row_height;
                Matcher analyzer(image);
                analyzer.set_roi(
                    { search_roi.x,
                      search_roi.y + row * row_height,
                      search_roi.width,
                      row_bottom - (search_roi.y + row * row_height) });
                analyzer.set_templ(resized);
                analyzer.set_method(MatchMethod::Ccoeff);
                analyzer.set_threshold(0.0);
                const auto result = analyzer.analyze();
                if (result && result->score > best_scores.at(row)) {
                    best_scores.at(row) = result->score;
                    best_rects.at(row) = result->rect;
                }
            }
        }

        const size_t target_row = static_cast<size_t>(skill_index - 1);
        double best_other_score = -1.0;
        for (size_t row = 0; row < best_scores.size(); ++row) {
            if (row != target_row) {
                best_other_score = std::max(best_other_score, best_scores.at(row));
            }
        }
        if (best_scores.at(target_row) >= 0.65 && best_scores.at(target_row) - best_other_score >= 0.08 &&
            is_expected_icon(best_rects.at(target_row))) {
            matched = best_rects.at(target_row);
        }
    }
    if (!matched) {
        return std::nullopt;
    }

    const Rect control { matched->x - 125, matched->y + matched->height - 15, 90, 80 };
    if (control.x < 0 || control.y < 0 || control.x + control.width > image.cols ||
        control.y + control.height > image.rows || control.x + control.width > search_roi.x + 10) {
        return std::nullopt;
    }
    return control;
}

std::optional<int> asst::training_recognition::recognize_mastery_frame(const cv::Mat& image, int skill_index)
{
    const Rect roi = Task.get("OperDevelopMasteryMarker" + std::to_string(skill_index))->roi;
    const cv::Rect cv_roi = make_rect<cv::Rect>(roi) & cv::Rect(0, 0, image.cols, image.rows);
    if (cv_roi.width <= 0 || cv_roi.height <= 0) {
        return std::nullopt;
    }

    cv::Mat gray;
    cv::cvtColor(image(cv_roi), gray, cv::COLOR_BGR2GRAY);
    cv::Mat binary;
    cv::threshold(gray, binary, 200, 255, cv::THRESH_BINARY);
    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int components = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
    int mastery = 0;
    for (int label = 1; label < components; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        const int width = stats.at<int>(label, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(label, cv::CC_STAT_HEIGHT);
        if (area >= 6 && area <= 120 && width >= 2 && width <= 12 && height >= 2 && height <= 12) {
            ++mastery;
        }
    }
    return mastery <= 3 ? std::optional(mastery) : std::nullopt;
}

MaterialCellReading
    asst::training_recognition::recognize_material_cell(const cv::Mat& image, const std::string& cell_task)
{
    const auto task_ptr = Task.get(cell_task);
    const Rect& cell = task_ptr->roi;
    if (cell.empty()) {
        return MaterialCellReading {};
    }
    // The count text mixes colors: yellow/white normally, orange-red owned digits when insufficient
    // (measured gray 124-137, below any workable gray threshold, while the backdrop above the chip reaches
    // gray 148). Binarize on the red channel instead: every text color sits at R >= 199 and every
    // background (blue/black chip body, rose backdrop) at R <= 160.
    cv::Mat r_channel;
    cv::extractChannel(image, r_channel, 2);
    cv::Mat r_image;
    cv::merge(std::array { r_channel, r_channel, r_channel }, r_image);
    RegionOCRer cell_ocr(r_image);
    cell_ocr.set_task_info("NumberOcrReplace");
    cell_ocr.set_roi(cell);
    if (task_ptr->special_params.size() >= 2) {
        cell_ocr.set_bin_threshold(task_ptr->special_params[0], task_ptr->special_params[1]);
    }
    cell_ocr.set_use_raw(false);

    const auto result = cell_ocr.analyze();
    const std::string text = result ? result->text : std::string {};
    const cv::Rect cv_cell = make_rect<cv::Rect>(cell) & cv::Rect(0, 0, image.cols, image.rows);
    bool foreground_present = false;
    if (cv_cell.width > 0 && cv_cell.height > 0) {
        cv::Mat foreground;
        const int lower = task_ptr->special_params.size() >= 2 ? task_ptr->special_params[0] : 175;
        const int upper = task_ptr->special_params.size() >= 2 ? task_ptr->special_params[1] : 255;
        cv::inRange(r_channel(cv_cell), cv::Scalar(lower), cv::Scalar(upper), foreground);
        foreground_present = cv::countNonZero(foreground) >= 8;
    }
    const auto reading = classify_material_cell(std::span<const std::string>(&text, 1), foreground_present);
    Log.info(__FUNCTION__, "material cell OCR", cell_task, text, "state", static_cast<int>(reading.state));
    return reading;
}

MaterialSlotFrame asst::training_recognition::recognize_material_slots(const cv::Mat& image)
{
    static constexpr std::array<const char*, MaterialSlotMax> CellTasks {
        "OperDevelopMaterialCell1",
        "OperDevelopMaterialCell2",
        "OperDevelopMaterialCell3",
    };
    MaterialSlotFrame frame;
    for (size_t slot = 0; slot < MaterialSlotMax; ++slot) {
        frame.at(slot) = recognize_material_cell(image, CellTasks.at(slot));
    }
    return frame;
}

std::optional<MaterialRequirement> asst::training_recognition::recognize_level_requirement(const cv::Mat& image)
{
    const auto reading = recognize_material_cell(image, "OperDevelopMaterialLevelCell");
    return reading.state == MaterialCellState::Parsed ? std::optional(reading.value) : std::nullopt;
}

std::optional<asst::Rect> asst::training_recognition::recognize_material_confirm_button(const cv::Mat& image)
{
    const Rect confirm_area = Task.get("OperDevelopMaterialConfirmArea")->roi;
    const cv::Rect cv_roi = make_rect<cv::Rect>(confirm_area) & cv::Rect(0, 0, image.cols, image.rows);
    if (cv_roi.width <= 0 || cv_roi.height <= 0) {
        return std::nullopt;
    }
    cv::Mat hsv;
    cv::cvtColor(image(cv_roi), hsv, cv::COLOR_BGR2HSV);
    cv::Mat blue;
    cv::inRange(hsv, cv::Scalar(80, 100, 120), cv::Scalar(115, 255, 255), blue);
    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int components = cv::connectedComponentsWithStats(blue, labels, stats, centroids, 8, CV_32S);
    int best_component = -1;
    int best_area = 0;
    for (int component = 1; component < components; ++component) {
        const int area = stats.at<int>(component, cv::CC_STAT_AREA);
        const int width = stats.at<int>(component, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(component, cv::CC_STAT_HEIGHT);
        if (area > best_area && area >= 3000 && width >= 120 && height >= 35) {
            best_component = component;
            best_area = area;
        }
    }
    if (best_component < 0) {
        return std::nullopt;
    }
    return Rect {
        confirm_area.x + stats.at<int>(best_component, cv::CC_STAT_LEFT),
        confirm_area.y + stats.at<int>(best_component, cv::CC_STAT_TOP),
        stats.at<int>(best_component, cv::CC_STAT_WIDTH),
        stats.at<int>(best_component, cv::CC_STAT_HEIGHT),
    };
}
