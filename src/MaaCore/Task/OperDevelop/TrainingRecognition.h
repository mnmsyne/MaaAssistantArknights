#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/AsstInfrastDef.h"
#include "Common/AsstTypes.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "OperDevelopTypes.h"

// Pure recognition primitives for the OperDevelop (干员培养) mastery/training-room feature, split out of
// OperDevelopTaskProcess.cpp so they can be exercised without pulling in Controller/AbstractTask. Each
// function here takes an already-captured cv::Mat (plus any small caller-owned state it needs, e.g. the
// per-skill template cache) and returns a value -- no ctrler() reads, no clicks, no retry loops. Methods
// that also mutate OperDevelopTaskProcessImpl's own members (e.g. caching a freshly captured template, or
// a device-polling retry loop) stay in OperDevelopTaskProcess.cpp; see that file for the ones deliberately
// left in place.
namespace asst::training_recognition
{
// Crops the fixed on-screen region for `skill_index`'s skill icon on the training-room's own layout
// (distinct from the operator-detail skill icon captured by capture_skill_image).
cv::Mat capture_training_skill_image(const cv::Mat& image, int skill_index);

// Recognizes the auto-played "mastery completed" announcement modal (chibi art + rank banner + confirm
// button) shown on training-room entry when a finished stage is still unclaimed.
std::optional<training::CompletionAnnouncement> recognize_completion_announcement(const cv::Mat& image);

// Matches the skill icon for `skill_index` (looked up in `skill_images`, keyed the same way as the
// caller's own template cache) against `image` and derives the row control immediately to its left.
std::optional<Rect> locate_skill_row_control(
    const cv::Mat& image,
    int skill_index,
    const std::unordered_map<int, cv::Mat>& skill_images);

// Reads the mastery-level dot marker (0-3 filled dots) for `skill_index` from a single frame.
std::optional<int> recognize_mastery_frame(const cv::Mat& image, int skill_index);

// Reads one fixed material cell's quantity text. Slot identity comes entirely from `cell_task`'s ROI.
training::MaterialCellReading recognize_material_cell(const cv::Mat& image, const std::string& cell_task);

// Reads all material cells (skill book + up to two extra materials) for the current mastery stage.
training::MaterialSlotFrame recognize_material_slots(const cv::Mat& image);

// Reads the 等级 (level requirement) cell, which is not itself a material.
std::optional<training::MaterialRequirement> recognize_level_requirement(const cv::Mat& image);

// Locates the confirm button on the mastery material page by its blue-fill connected component.
std::optional<Rect> recognize_material_confirm_button(const cv::Mat& image);
} // namespace asst::training_recognition
