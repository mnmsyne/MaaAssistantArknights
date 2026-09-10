#pragma once

#include <optional>

#include "Common/AsstBattleDef.h"
#include "Vision/VisionHelper.h"

namespace asst
{
struct OperBoxInfo
{
    std::string id;
    std::string name;
    int level = 0;     // 等级
    int elite = 0;     // 精英度
    int potential = 0; // 潜能
    int rarity = 0;    // 稀有度

    Rect rect;
    bool own = false;
};

class OperBoxImageAnalyzer final : public VisionHelper
{
public:
    using VisionHelper::VisionHelper;
    virtual ~OperBoxImageAnalyzer() override = default;
    bool analyze();

    void set_role_rois(Rect top, Rect bottom)
    {
        m_role_top_roi = top;
        m_role_bottom_roi = bottom;
    }

    // Narrows opers_analyze() to the templates for a single already-in-game-filtered role, skipping the other 8
    // MultiMatcher passes. Unset (default) preserves the original unfiltered 9-role scan.
    void set_role_filter(battle::Role role) noexcept { m_role_filter = role; }

    const auto& get_result() const noexcept { return m_result; }

private:
    int level_num(const std::string& level);
    bool analyzer_oper_box();
    // 获取lv rect
    bool opers_analyze();
    bool level_analyze();
    bool elite_analyze();
    bool potential_analyze();

    Rect m_role_top_roi;
    Rect m_role_bottom_roi;
    std::optional<battle::Role> m_role_filter;
    std::vector<asst::OperBoxInfo> m_result;
};
}
