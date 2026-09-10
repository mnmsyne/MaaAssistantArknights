// <copyright file="AsstOperDevelopTask.cs" company="MaaAssistantArknights">
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
using System.Collections.Generic;
using MaaWpfGui.Services;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace MaaWpfGui.Models.AsstTasks;

public sealed class AsstOperDevelopTask : AsstBaseTask
{
    public override AsstTaskType TaskType => AsstTaskType.OperDevelop;

    [JsonProperty("action")]
    public string Action { get; set; } = "preflight";

    [JsonProperty("operator_id")]
    public string OperatorId { get; set; } = string.Empty;

    [JsonProperty("skills")]
    public List<Skill> Skills { get; set; } = [];

    // Wire tokens understood by Core: "halving" (prefer halving assistants on M1/M2) and "efficiency"
    // (always the top of the overall ranking). Anything else is rejected by Core's set_params.
    [JsonProperty("training_mode")]
    public string TrainingMode { get; set; } = "halving";

    [JsonProperty("allow_consume")]
    public bool AllowConsume { get; set; }

    [JsonProperty("expected_mastery")]
    public List<MasteryExpectation> ExpectedMastery { get; set; } = [];

    [JsonProperty("preflight_token")]
    public string PreflightToken { get; set; } = string.Empty;

    [JsonProperty("expected_stage_plan")]
    public StagePlan? ExpectedStagePlan { get; set; }

    public override (AsstTaskType TaskType, JObject Params) Serialize() => (TaskType, JObject.FromObject(this));

    public sealed class Skill
    {
        [JsonProperty("index")]
        public int Index { get; set; }

        [JsonProperty("target_mastery")]
        public int TargetMastery { get; set; }
    }

    public sealed class StagePlan
    {
        [JsonProperty("skill_index")]
        public int SkillIndex { get; set; }

        [JsonProperty("from_mastery")]
        public int FromMastery { get; set; }

        [JsonProperty("to_mastery")]
        public int ToMastery { get; set; }

        [JsonProperty("plan_kind")]
        public string PlanKind { get; set; } = string.Empty;

        [JsonProperty("assistant_name")]
        public string AssistantName { get; set; } = string.Empty;

        [JsonProperty("estimated_minutes")]
        public int EstimatedMinutes { get; set; }
    }

    public sealed class MasteryExpectation
    {
        [JsonProperty("index")]
        public int Index { get; set; }

        [JsonProperty("current_mastery")]
        public int CurrentMastery { get; set; }
    }
}
