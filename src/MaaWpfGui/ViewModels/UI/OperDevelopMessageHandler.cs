// <copyright file="OperDevelopMessageHandler.cs" company="MaaAssistantArknights">
// Part of the MaaWpfGui project, maintained by the MaaAssistantArknights team (Maa Team)
// Copyright (C) 2021-2025 MaaAssistantArknights Contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License v3.0 only as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY
// </copyright>

#nullable enable
using System;
using System.Linq;
using System.Windows;
using MaaWpfGui.Constants;
using MaaWpfGui.Helper;
using MaaWpfGui.Models.AsstTasks;
using Newtonsoft.Json.Linq;

namespace MaaWpfGui.ViewModels.UI;

/// <summary>
/// Handles Core progress messages for the "OperDevelop" task chain on behalf of an owning
/// <see cref="OperDevelopViewModel"/>. Kept as a plain class holding an owner reference since it has exactly one
/// consumer and needs read/write access to several of the owner's fields/properties.
/// </summary>
internal sealed class OperDevelopMessageHandler(OperDevelopViewModel owner)
{
    /// <summary>
    /// Entry point for Core progress messages on the "OperDevelop" task chain. This is a thin dispatcher; each
    /// "what" discriminator is handled by its own private method below.
    /// </summary>
    public void HandleOperDevelopCoreMessage(JObject outerDetails)
    {
        // AsstProxy.CallbackFunction 已保证所有回调在 UI 线程执行，此处无需再调度。
        string what = outerDetails["what"]?.ToString() ?? string.Empty;
        var details = outerDetails["details"] as JObject ?? [];
        string message = details["message"]?.ToString() ?? what;

        switch (what)
        {
            case "OperDevelopTrainingStatus":
                HandleTrainingStatus(details, message);
                return;
            case "OperDevelopOperatorInspected":
                HandleOperatorInspected(details);
                return;
            case "OperDevelopSkillInspected":
                HandleSkillInspected(details);
                return;
            case "OperDevelopMaterialInspected":
                HandleMaterialInspected(details);
                return;
            case "OperDevelopPreflight":
                HandlePreflight(details, message);
                return;
            case "OperDevelopBlocked":
                HandleBlocked(message);
                return;
        }

        // Nested navigation tasks may emit generic diagnostics such as ExceededLimit while the oper-develop
        // process is safely recovering or falling back. Keep this page scoped to its own structured progress
        // events; everything else prefixed with OperDevelop (e.g. StageStarted / Completed) is logged verbatim.
        if (what.StartsWith("OperDevelop", StringComparison.Ordinal))
        {
            AddLog(message, UiLogColor.Info);
        }
    }

    /// <summary>
    /// Handles "OperDevelopTrainingStatus": the current state of the training room (processing / completed /
    /// occupied). MAA never babysits a running stage, so this is purely informational.
    /// </summary>
    private void HandleTrainingStatus(JObject details, string message)
    {
        owner.InvalidatePreflight();
        owner.ClearPendingPreflightLogCard();
        string status = details["status"]?.ToString() ?? string.Empty;
        string operatorName = details["operator"]?.ToString() ?? string.Empty;
        string skillName = details["skill"]?.ToString() ?? string.Empty;
        int mastery = details["mastery"]?.ToObject<int>() ?? -1;
        string timeLeft = details["time_left"]?.ToString() ?? string.Empty;
        owner.PreflightSummary = message;
        owner.EstimatedTimeText = status switch
        {
            "completed" => "已完成",
            "idle" => "空闲",
            _ when string.IsNullOrEmpty(timeLeft) => "训练中",
            _ => timeLeft,
        };
        BeginLogCard();
        AddLog(
            status switch {
                "completed" => "当前专精阶段已完成",
                "processing" => "技能专精中",
                "idle" => "训练室空闲",
                _ => "训练室被占用",
            },
            status == "processing" ? UiLogColor.Warning : UiLogColor.Info,
            "Bold");
        if (!string.IsNullOrEmpty(operatorName))
        {
            AddLog($"训练干员  {operatorName}", UiLogColor.Trace, showTime: false);
        }
        if (!string.IsNullOrEmpty(skillName) || mastery >= 1)
        {
            AddLog(string.IsNullOrEmpty(skillName) ? $"专精阶段  M{mastery}" : $"训练技能  {skillName}  M{mastery}", UiLogColor.Trace, showTime: false);
        }
        if (!string.IsNullOrEmpty(timeLeft))
        {
            AddLog($"剩余时间  {timeLeft}", UiLogColor.Trace, showTime: false);
        }
        AddLog(message, UiLogColor.Info, showTime: false);
    }

    /// <summary>Handles "OperDevelopOperatorInspected": basic eligibility facts about the selected operator.</summary>
    private void HandleOperatorInspected(JObject details)
    {
        int elite = details["elite"]?.ToObject<int>() ?? -1;
        bool roleFilterUsed = details["role_filter_used"]?.ToObject<bool>() ?? false;
        AddLog($"基础条件  精英 {elite}  技能 RANK 7", UiLogColor.Info, showTime: false);
        AddLog(
            roleFilterUsed ? "查找方式  已使用游戏内职业筛选" : "查找方式  职业筛选失败，已扫描完整列表",
            roleFilterUsed ? UiLogColor.Info : UiLogColor.Warning,
            showTime: false);
    }

    /// <summary>Handles "Oper DevelopSkillInspected": whether the requested skill still needs training.</summary>
    private void HandleSkillInspected(JObject details)
    {
        int index = details["index"]?.ToObject<int>() ?? 0;
        int current = details["current_mastery"]?.ToObject<int>() ?? -1;
        int target = details["target_mastery"]?.ToObject<int>() ?? -1;
        bool needsTraining = details["needs_training"]?.ToObject<bool>() ?? current < target;
        AddLog(
            needsTraining
                ? $"技能 S{index}  当前 M{current}  目标 M{target}"
                : $"技能 S{index}  当前 M{current}  已满足目标 M{target}",
            UiLogColor.Info,
            showTime: false);
    }

    /// <summary>Handles "OperDevelopMaterialInspected": whether the recognized material counts are sufficient.</summary>
    private void HandleMaterialInspected(JObject details)
    {
        int index = details["index"]?.ToObject<int>() ?? 0;
        int current = details["current_mastery"]?.ToObject<int>() ?? -1;
        bool sufficient = details["materials_sufficient"]?.ToObject<bool>() ?? false;
        bool complete = details["material_data_complete"]?.ToObject<bool>() ?? false;
        JArray requirements = details["requirements"] as JArray ?? [];

        string header = complete
            ? $"材料复核  S{index}  M{current} → M{current + 1}  数量已识别  {(sufficient ? "充足" : "不足")}"
            : $"材料复核  S{index}  M{current} → M{current + 1}  数量识别不完整（{requirements.Count(item => item?["recognized"]?.ToObject<bool>() == true)}/{requirements.Count} 项），为避免误消耗已终止";
        string headerColor = complete ? (sufficient ? UiLogColor.Info : UiLogColor.Error) : UiLogColor.Error;
        AddLog(header, headerColor, showTime: false);

        foreach (var item in requirements)
        {
            string role = item?["role"]?.ToString() ?? string.Empty;
            string displayName = role switch
            {
                "skill_book" => "技能书",
                "material_1" => "材料1",
                "material_2" => "材料2",
                _ => $"材料{(item?["slot"]?.ToObject<int>() ?? 0) + 1}",
            };
            bool itemRecognized = item?["recognized"]?.ToObject<bool>() == true;
            if (!itemRecognized)
            {
                AddLog($"{displayName}（未识别）", UiLogColor.Error, showTime: false);
                continue;
            }

            bool itemSufficient = item?["sufficient"]?.ToObject<bool>() == true;
            int owned = item?["owned"]?.ToObject<int>() ?? 0;
            int required = item?["required"]?.ToObject<int>() ?? 0;
            AddLog(
                $"{displayName}（{owned}/{required}）",
                itemSufficient ? UiLogColor.Info : UiLogColor.Error,
                showTime: false);
        }
    }

    /// <summary>
    /// Handles "OperDevelopPreflight": the preflight result for the single pending stage, including the chosen
    /// assistant and the time estimate.
    /// </summary>
    private void HandlePreflight(JObject details, string message)
    {
        owner.ClearPendingPreflightLogCard();
        var preflightMastery = owner.PreflightMastery;
        preflightMastery.Clear();
        foreach (var item in details["skills"] as JArray ?? [])
        {
            int index = item["index"]?.ToObject<int>() ?? 0;
            int current = item["current_mastery"]?.ToObject<int>() ?? -1;
            if (index is >= 1 and <= 3 && current is >= 0 and <= 3)
            {
                preflightMastery[index] = current;
            }
        }

        owner.PreflightToken = details["preflight_token"]?.ToString() ?? string.Empty;
        var preflightStagePlan = details["current_stage_plan"]?.ToObject<AsstOperDevelopTask.StagePlan>();
        owner.PreflightStagePlan = preflightStagePlan;
        owner.PlannedAssistantName = preflightStagePlan?.AssistantName ?? string.Empty;
        int minutes = details["estimated_minutes"]?.ToObject<int>() ?? 0;
        owner.EstimatedTimeText = minutes > 0 ? FormatDuration(minutes) : "—";

        bool eligible = details["eligible"]?.ToObject<bool>() ?? false;
        bool materialsSufficient = details["materials_sufficient"]?.ToObject<bool>() ?? false;
        owner.PreflightPassed = eligible && materialsSufficient && minutes > 0 && preflightStagePlan is not null &&
                          preflightMastery.Count == owner.GetSelectedSkills().Count;
        owner.PreflightSummary = message;

        BeginLogCard();
        int controlCenterBonus = details["control_center_bonus"]?.ToObject<int>() ?? 0;
        string controlCenterAssistant = details["control_center_assistant"]?.ToString() ?? string.Empty;
        AddLog("现场加成", UiLogColor.Trace, "Bold", showTime: false);
        AddLog(
            controlCenterBonus > 0
                ? $"控制中枢  {controlCenterAssistant}  +{controlCenterBonus}%"
                : "控制中枢  未检测到训练加成",
            controlCenterBonus > 0 ? UiLogColor.Info : UiLogColor.Trace,
            showTime: false);

        if (preflightStagePlan is not null)
        {
            int pendingSkillIndex = details["pending_skill_index"]?.ToObject<int>() ?? preflightStagePlan.SkillIndex;
            AddLog(
                $"专精方案  S{pendingSkillIndex}  M{preflightStagePlan.FromMastery} → M{preflightStagePlan.ToMastery}",
                UiLogColor.Info,
                "Bold");
            AddLog(
                $"{GetPlanKindText(preflightStagePlan.PlanKind)}  协助干员  {preflightStagePlan.AssistantName}",
                UiLogColor.Trace,
                showTime: false);
            AddLog($"预计用时  {FormatDuration(preflightStagePlan.EstimatedMinutes)}", UiLogColor.Trace, showTime: false);
            string stageWarning = details["current_stage_plan"]?["warning"]?.ToString() ?? string.Empty;
            if (!string.IsNullOrEmpty(stageWarning))
            {
                AddLog(stageWarning, UiLogColor.Warning, showTime: false);
            }
            AddLog($"计算基础  {FormatDuration(BaseMinutesOf(details))}", UiLogColor.Trace, showTime: false);
        }

        AddLog(message, owner.PreflightPassed ? UiLogColor.Info : UiLogColor.Error, "Bold");
    }

    private static int BaseMinutesOf(JObject details) =>
        details["current_stage_plan"]?["base_minutes"]?.ToObject<int>() ?? 0;

    /// <summary>Handles "OperDevelopBlocked": the process could not proceed and the preflight state is reset.</summary>
    private void HandleBlocked(string message)
    {
        owner.ClearPendingPreflightLogCard();
        owner.InvalidatePreflight();
        owner.PreflightSummary = message;
        AddLog(message, UiLogColor.Error, "Bold");
    }

    private static string GetPlanKindText(string planKind) => planKind switch {
        "halving" => LocalizationHelper.GetString("OperDevelopPlanHalving"),
        _ => LocalizationHelper.GetString("OperDevelopPlanEfficiency"),
    };

    private void BeginLogCard() => owner.BeginLogCard();

    private void AddLog(string message, string color = UiLogColor.Trace, string weight = "Regular", bool showTime = true) =>
        owner.AddLog(message, color, weight, showTime);

    private static string FormatDuration(int minutes)
    {
        int days = minutes / 1440;
        int hours = (minutes % 1440) / 60;
        int mins = minutes % 60;
        if (days > 0)
        {
            return $"{days}天 {hours}小时 {mins}分钟";
        }

        return hours > 0 ? $"{hours}小时 {mins}分钟" : $"{mins}分钟";
    }
}
