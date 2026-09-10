#pragma once

#include <cmath>

#include "OperDevelopTypes.h"

// Efficiency estimates for the OperDevelop (干员培养) feature. The resource-backed assistant lookups and the
// bonus matcher that used to live here are shared with 基建换班-训练室协助优化 through TrainingConfig.h
// (namespace asst::training) and the TrainingAssistantPanel interaction primitives.

namespace asst::training
{
// The training-speed multiplier for an assistant with `assistant_bonus`, given the room's `control_center_bonus`
// (0 or 5): the control center's own bonus plus the assistant's own skill bonus on top of the base speed.
inline double efficiency_rate(int control_center_bonus, int assistant_bonus) noexcept
{
    return (100 + control_center_bonus + assistant_bonus) / 100.0;
}

// Conservative stage duration: base minutes divided by the flat rate. The one-shot halving trigger of a seated
// halving assistant is deliberately ignored -- it can only shorten the real run, so the estimate stays honest.
inline int estimate_stage_minutes(int base_minutes, int control_center_bonus, int assistant_bonus)
{
    const double rate = efficiency_rate(control_center_bonus, assistant_bonus);
    return rate <= 0 ? 0 : static_cast<int>(std::ceil(base_minutes / rate));
}
} // namespace asst::training
