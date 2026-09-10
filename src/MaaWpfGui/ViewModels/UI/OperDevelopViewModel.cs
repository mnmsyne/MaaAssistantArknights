// <copyright file="OperDevelopViewModel.cs" company="MaaAssistantArknights">
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
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using MaaWpfGui.Configuration.Factory;
using MaaWpfGui.Constants;
using MaaWpfGui.Constants.Enums;
using MaaWpfGui.Helper;
using MaaWpfGui.Main;
using MaaWpfGui.Models.AsstTasks;
using MaaWpfGui.States;
using MaaWpfGui.Utilities.ValueType;
using MaaWpfGui.ViewModels.Items;
using Newtonsoft.Json.Linq;
using Serilog;
using Stylet;

namespace MaaWpfGui.ViewModels.UI;

/// <summary>
/// The view model of 干员培养 (OperDevelop), extracted from <see cref="ToolboxViewModel"/> into its own
/// independent view model. Uses the shared global <see cref="RunningState"/> instance like the other pages.
/// </summary>
public class OperDevelopViewModel : PropertyChangedBase
{
    private static readonly ILogger _logger = Log.ForContext<OperDevelopViewModel>();

    private readonly RunningState _runningState;

    private IReadOnlyList<OperatorOption> _allOperators = [];
    private HashSet<string> _excludedOperatorIds = [];
    private readonly Dictionary<int, int> _preflightMastery = [];
    private string _preflightToken = string.Empty;
    private AsstOperDevelopTask.StagePlan? _preflightStagePlan;
    private LogCardItemViewModel? _currentLogCard;
    private LogCardItemViewModel? _pendingPreflightLogCard;
    private readonly OperDevelopMessageHandler _messageHandler;

    /// <summary>
    /// Initializes a new instance of the <see cref="OperDevelopViewModel"/> class.
    /// </summary>
    public OperDevelopViewModel()
    {
        _messageHandler = new OperDevelopMessageHandler(this);
        _runningState = RunningState.Instance;

        _excludedOperatorIds = LoadExcludedOperatorIds() ?? [];
        _allOperators = DataHelper.Operators.Values
            .Where(info => !string.IsNullOrWhiteSpace(info.Name) && info.Rarity is >= 4 and <= 6)
            .Select(info => new OperatorOption(info.Id, info.Name!, info.Rarity, info.Type))
            .OrderBy(info => info.Name, StringComparer.Ordinal)
            .ToList();

        RarityOptions =
        [
            new GenericCombinedData<int?>(LocalizationHelper.GetString("OperDevelopAllRarities"), null),
            new GenericCombinedData<int?>("6★", 6),
            new GenericCombinedData<int?>("5★", 5),
            new GenericCombinedData<int?>("4★", 4),
        ];
        RoleOptions =
        [
            new GenericCombinedData<OperatorRole?>(LocalizationHelper.GetString("OperDevelopAllRoles"), null),
            new GenericCombinedData<OperatorRole?>("近卫", OperatorRole.Warrior),
            new GenericCombinedData<OperatorRole?>("先锋", OperatorRole.Pioneer),
            new GenericCombinedData<OperatorRole?>("医疗", OperatorRole.Medic),
            new GenericCombinedData<OperatorRole?>("重装", OperatorRole.Tank),
            new GenericCombinedData<OperatorRole?>("狙击", OperatorRole.Sniper),
            new GenericCombinedData<OperatorRole?>("术师", OperatorRole.Caster),
            new GenericCombinedData<OperatorRole?>("辅助", OperatorRole.Support),
            new GenericCombinedData<OperatorRole?>("特种", OperatorRole.Special),
        ];

        SelectedRarity = RarityOptions[0];
        SelectedRole = RoleOptions[0];
        NormalizeStoredTrainingMode();
        var savedId = ConfigFactory.CurrentConfig.Toolbox.OperDevelopOperatorId;
        SelectedOperator = _allOperators.FirstOrDefault(info => info.Id == savedId);
        OperatorQuery = SelectedOperator?.Name ?? string.Empty;
        RefreshCandidates();
    }

    /// <summary>
    /// Rewrites a stored training-mode token left over from an older build to the current default. Old builds
    /// stored "safe_halving"/"highest_efficiency"/"efficiency"(optimal-route); none match the current token set,
    /// so the stale value is replaced in the persisted config on the next save instead of silently falling back
    /// at every read.
    /// </summary>
    private void NormalizeStoredTrainingMode()
    {
        if (!_validTrainingModes.Contains(ConfigFactory.CurrentConfig.Toolbox.OperDevelopTrainingMode))
        {
            ConfigFactory.CurrentConfig.Toolbox.OperDevelopTrainingMode = DefaultTrainingMode;
        }
    }

    public IReadOnlyList<GenericCombinedData<int?>> RarityOptions { get; private set; } = [];

    public IReadOnlyList<GenericCombinedData<OperatorRole?>> RoleOptions { get; private set; } = [];

    public const string DefaultTrainingMode = "halving";

    private static readonly HashSet<string> _validTrainingModes = ["halving", "efficiency"];

    public IReadOnlyList<GenericCombinedData<string>> TrainingModeOptions { get; } =
    [
        new(LocalizationHelper.GetString("OperDevelopModeHalving"), "halving"),
        new(LocalizationHelper.GetString("OperDevelopModeEfficiency"), "efficiency"),
    ];

    public ObservableCollection<LogCardItemViewModel> LogCardViewModels { get; } = [];

    /// <summary>
    /// Left-column module selection. 0 = 技能专精, 1 = 干员升级 (placeholder, not yet implemented). Session-only,
    /// not persisted.
    /// </summary>
    public int OperDevelopModuleIndex
    {
        get; set {
            if (SetAndNotify(ref field, value))
            {
                NotifyOfPropertyChange(nameof(IsMasteryModule));
            }
        }
    }

    public bool IsMasteryModule => OperDevelopModuleIndex == 0;

    public IReadOnlyList<OperatorOption> Candidates
    {
        get; private set {
            SetAndNotify(ref field, value);
        }
    } = [];

    public GenericCombinedData<int?> SelectedRarity
    {
        get; set {
            if (SetAndNotify(ref field, value))
            {
                InvalidatePreflight();
                RefreshCandidates();
            }
        }
    } = null!;

    public GenericCombinedData<OperatorRole?> SelectedRole
    {
        get; set {
            if (SetAndNotify(ref field, value))
            {
                InvalidatePreflight();
                RefreshCandidates();
            }
        }
    } = null!;

    public string OperatorQuery
    {
        get; set {
            if (!SetAndNotify(ref field, value))
            {
                return;
            }

            var exact = _allOperators.FirstOrDefault(info => info.Name == value);
            if (exact is not null && exact != SelectedOperator)
            {
                SelectedOperator = exact;
            }
            InvalidatePreflight();
            RefreshCandidates();
        }
    } = string.Empty;

    public OperatorOption? SelectedOperator
    {
        get; set {
            if (!SetAndNotify(ref field, value))
            {
                return;
            }

            if (value is not null)
            {
                ConfigFactory.CurrentConfig.Toolbox.OperDevelopOperatorId = value.Id;
                if (OperatorQuery != value.Name)
                {
                    OperatorQuery = value.Name;
                }

                if (_excludedOperatorIds.Contains(value.Id))
                {
                    AddLog(LocalizationHelper.GetString("OperDevelopExcludedOperator"), UiLogColor.Warning);
                }
            }
            else
            {
                ConfigFactory.CurrentConfig.Toolbox.OperDevelopOperatorId = string.Empty;
            }

            if (value is null or { Rarity: < 6 } && SkillIndex == 3)
            {
                SkillIndex = 1;
            }
            NotifyOfPropertyChange(nameof(SkillIndexOptions));
            InvalidatePreflight();
        }
    }

    public IReadOnlyList<GenericCombinedData<int>> SkillIndexOptions => SelectedOperator?.Rarity is 6
        ? [
            new(LocalizationHelper.GetString("OperDevelopSkillOption1"), 1),
            new(LocalizationHelper.GetString("OperDevelopSkillOption2"), 2),
            new(LocalizationHelper.GetString("OperDevelopSkillOption3"), 3),
          ]
        : [
            new(LocalizationHelper.GetString("OperDevelopSkillOption1"), 1),
            new(LocalizationHelper.GetString("OperDevelopSkillOption2"), 2),
          ];

    public int SkillIndex
    {
        get => ConfigFactory.CurrentConfig.Toolbox.OperDevelopSkillIndex;
        set {
            int clamped = Math.Clamp(value, 1, 3);
            if (ConfigFactory.CurrentConfig.Toolbox.OperDevelopSkillIndex == clamped)
            {
                return;
            }
            ConfigFactory.CurrentConfig.Toolbox.OperDevelopSkillIndex = clamped;
            NotifyOfPropertyChange();
            InvalidatePreflight();
        }
    }

    public int MasteryRank
    {
        get => ConfigFactory.CurrentConfig.Toolbox.OperDevelopMasteryRank;
        set {
            ConfigFactory.CurrentConfig.Toolbox.OperDevelopMasteryRank = Math.Clamp(value, 1, 3);
            NotifyOfPropertyChange();
            InvalidatePreflight();
        }
    }

    public string TrainingMode
    {
        get => ConfigFactory.CurrentConfig.Toolbox.OperDevelopTrainingMode;

        set {
            ConfigFactory.CurrentConfig.Toolbox.OperDevelopTrainingMode =
                _validTrainingModes.Contains(value) ? value : DefaultTrainingMode;
            NotifyOfPropertyChange();
            InvalidatePreflight();
        }
    }

    public string EstimatedTimeText
    {
        get; internal set {
            if (SetAndNotify(ref field, value))
            {
                NotifyOfPropertyChange(nameof(StagePlanSummaryText));
            }
        }
    } = "—";

    public string PlannedAssistantName
    {
        get; internal set {
            if (SetAndNotify(ref field, value))
            {
                NotifyOfPropertyChange(nameof(StagePlanSummaryText));
            }
        }
    } = string.Empty;

    public string StagePlanSummaryText => string.IsNullOrEmpty(PlannedAssistantName)
        ? $"预计时间：{EstimatedTimeText}"
        : $"协助干员：{PlannedAssistantName}（预计时间：{EstimatedTimeText}）";

    public string PreflightSummary
    {
        get; internal set {
            SetAndNotify(ref field, value);
        }
    } = string.Empty;

    public bool PreflightPassed
    {
        get; internal set {
            if (SetAndNotify(ref field, value))
            {
                NotifyOfPropertyChange(nameof(CanConfirmAndStart));
            }
        }
    }

    public bool CanConfirmAndStart => PreflightPassed;

    public async Task RunPreflight()
    {
        if (!TryBuildTask("preflight", false, out var task, out var error))
        {
            AddLog(error, UiLogColor.Error);
            return;
        }

        InvalidatePreflight();
        BeginLogCard();
        MarkPendingPreflightLogCard();
        AddLog(LocalizationHelper.GetString("OperDevelopPreflightStarted"), UiLogColor.Info);
        AddLog($"干员检查  {SelectedOperator?.Name}", UiLogColor.Info, "Bold");
        _runningState.SetIdle(false);
        if (!await ConnectToEmulatorAsync())
        {
            _runningState.SetIdle(true);
            return;
        }

        StartOperDevelopTask(task);
    }

    public async Task StartConfirmed()
    {
        if (!PreflightPassed || string.IsNullOrEmpty(_preflightToken))
        {
            AddLog(LocalizationHelper.GetString("OperDevelopPreflightRequired"), UiLogColor.Warning);
            return;
        }

        var pending = GetSelectedSkills().FirstOrDefault(item =>
            _preflightMastery.TryGetValue(item.Index, out int current) && current < item.TargetMastery);
        string skills = pending is null
            ? LocalizationHelper.GetString("OperDevelopNoPendingStage")
            : $"S{pending.Index}: M{_preflightMastery[pending.Index]} → M{_preflightMastery[pending.Index] + 1}";

        if (!TryBuildTask("execute", true, out var task, out var error))
        {
            AddLog(error, UiLogColor.Error);
            return;
        }

        task.PreflightToken = _preflightToken;
        task.ExpectedStagePlan = _preflightStagePlan;
        task.ExpectedMastery = _preflightMastery
            .OrderBy(pair => pair.Key)
            .Select(pair => new AsstOperDevelopTask.MasteryExpectation { Index = pair.Key, CurrentMastery = pair.Value })
            .ToList();
        BeginLogCard();
        AddLog($"确认开始  {SelectedOperator?.Name}  {skills}", UiLogColor.Warning, "Bold");
        AddLog("本次将不可逆消耗专精材料，具体数量见上方材料复核日志。", UiLogColor.Warning);
        AddLog(LocalizationHelper.GetString("OperDevelopExecutionStarted"), UiLogColor.Info);
        _runningState.SetIdle(false);
        if (!await ConnectToEmulatorAsync())
        {
            _runningState.SetIdle(true);
            return;
        }

        StartOperDevelopTask(task);
        InvalidatePreflight();
    }

    private bool StartOperDevelopTask(AsstOperDevelopTask task)
    {
        var (success, _) = Instances.AsstProxy.AsstAppendTaskWithEncoding(AsstProxy.TaskType.OperDevelop, task);
        if (!success || !Instances.AsstProxy.AsstStart())
        {
            AddLog(LocalizationHelper.GetString("OperDevelopTaskStartFailed"), UiLogColor.Error);
            _runningState.SetIdle(true);
            return false;
        }

        return true;
    }

    public async Task StopTask()
    {
        if (!Instances.AsstProxy.AsstRunning())
        {
            AddLog(LocalizationHelper.GetString("Stopped"), UiLogColor.Warning);
            return;
        }

        AddLog(LocalizationHelper.GetString("Stopping"), UiLogColor.Warning);
        await Instances.TaskQueueViewModel.Stop();
        AddLog(LocalizationHelper.GetString("Stopped"), UiLogColor.Info);
    }

    private async Task<bool> ConnectToEmulatorAsync()
    {
        string error = string.Empty;
        bool connected = await Task.Run(() => Instances.AsstProxy.AsstConnect(ref error));
        if (connected)
        {
            return true;
        }

        AddLog(error, UiLogColor.Error);
        return false;
    }

    /// <summary>
    /// Entry point for Core progress messages on the "OperDevelop" task chain. Forwards to
    /// <see cref="OperDevelopMessageHandler"/>, which owns the actual per-"what" dispatch.
    /// </summary>
    public void HandleOperDevelopCoreMessage(JObject outerDetails) => _messageHandler.HandleOperDevelopCoreMessage(outerDetails);

    /// <summary>Gets the mutable current preflight mastery snapshot. For <see cref="OperDevelopMessageHandler"/> only.</summary>
    internal Dictionary<int, int> PreflightMastery => _preflightMastery;

    internal string PreflightToken
    {
        get => _preflightToken;
        set => _preflightToken = value;
    }

    internal AsstOperDevelopTask.StagePlan? PreflightStagePlan
    {
        get => _preflightStagePlan;
        set => _preflightStagePlan = value;
    }

    /// <summary>
    /// Removes the pending preflight log card (if any), since a training-status update superseded it. For
    /// <see cref="OperDevelopMessageHandler"/> only.
    /// </summary>
    internal void ClearPendingPreflightLogCard()
    {
        if (_pendingPreflightLogCard is not null)
        {
            LogCardViewModels.Remove(_pendingPreflightLogCard);
            if (ReferenceEquals(_currentLogCard, _pendingPreflightLogCard))
            {
                _currentLogCard = null;
            }
            _pendingPreflightLogCard = null;
        }
    }

    /// <summary>Marks the current log card as the pending-preflight one, so a later training-status update can
    /// remove it if the preflight's own result never arrived. For <see cref="OperDevelopMessageHandler"/> only.</summary>
    internal void MarkPendingPreflightLogCard() => _pendingPreflightLogCard = _currentLogCard;

    internal List<AsstOperDevelopTask.Skill> GetSelectedSkills() =>
        [new() { Index = SkillIndex, TargetMastery = MasteryRank }];

    private static HashSet<string>? LoadExcludedOperatorIds()
    {
        string path = Path.Combine(PathsHelper.ResourceDir, "training.json");
        try
        {
            var root = JObject.Parse(File.ReadAllText(path));
            if (root["excluded_operators"] is not JObject excludedOperators || !excludedOperators.Properties().Any())
            {
                _logger.Error("Operator development exclusion resource is missing or empty: {Path}", path);
                return null;
            }

            return excludedOperators.Properties().Select(property => property.Name).ToHashSet(StringComparer.Ordinal);
        }
        catch (Exception ex)
        {
            _logger.Error(ex, "Failed to load operator development exclusion resource: {Path}", path);
            return null;
        }
    }

    internal void InvalidatePreflight()
    {
        PreflightPassed = false;
        _preflightToken = string.Empty;
        _preflightStagePlan = null;
        _preflightMastery.Clear();
        EstimatedTimeText = "—";
        PlannedAssistantName = string.Empty;
        PreflightSummary = string.Empty;
    }

    private void RefreshCandidates()
    {
        string query = OperatorQuery.Trim();

        // "全部星级" (SelectedRarity.Value == null) still means 6★-only here -- mastery-worthy skills only exist
        // on 6★ operators, so surfacing 4/5★ candidates by default just adds noise to the dropdown.
        int rarityFilter = SelectedRarity?.Value ?? 6;
        Candidates = _allOperators
            .Where(info => info.Rarity == rarityFilter)
            .Where(info => SelectedRole?.Value is null || info.Role == SelectedRole.Value)
            .Where(info => query.Length == 0 || info.Name.Contains(query, StringComparison.OrdinalIgnoreCase))
            .ToList();
    }

    private bool TryBuildTask(string action, bool allowConsume, out AsstOperDevelopTask task, out string error)
    {
        task = new();
        error = string.Empty;
        if (SettingsViewModel.GameSettings.ClientType is not (ClientType.Official or ClientType.Bilibili))
        {
            error = LocalizationHelper.GetString("OperDevelopCnOnly");
            return false;
        }
        if (SelectedOperator is null || OperatorQuery != SelectedOperator.Name)
        {
            error = LocalizationHelper.GetString("OperDevelopSelectExactOperator");
            return false;
        }
        if (_excludedOperatorIds.Contains(SelectedOperator.Id))
        {
            error = LocalizationHelper.GetString("OperDevelopExcludedOperator");
            return false;
        }
        var skills = GetSelectedSkills();
        if (skills.Count == 0)
        {
            error = LocalizationHelper.GetString("OperDevelopSelectSkill");
            return false;
        }
        if (!_runningState.GetIdle() || !_runningState.GetInit())
        {
            error = LocalizationHelper.GetString("OperDevelopAssistantNotReady");
            return false;
        }

        task = new AsstOperDevelopTask {
            Action = action,
            OperatorId = SelectedOperator.Id,
            Skills = skills,
            TrainingMode = TrainingMode,
            AllowConsume = allowConsume,
        };
        return true;
    }

    internal void BeginLogCard()
    {
        if (_currentLogCard is not null && _currentLogCard.Items.Count == 0)
        {
            return;
        }

        _currentLogCard = new LogCardItemViewModel();
        LogCardViewModels.Add(_currentLogCard);
    }

    internal void AddLog(
        string message,
        string color = UiLogColor.Trace,
        string weight = "Regular",
        bool showTime = true)
    {
        if (_currentLogCard is null)
        {
            BeginLogCard();
        }

        _currentLogCard!.Items.Add(new LogItemViewModel(message, color, weight, showTime: showTime));
        while (LogCardViewModels.Sum(card => card.Items.Count) > 200)
        {
            var firstCard = LogCardViewModels[0];
            if (LogCardViewModels.Count > 1)
            {
                LogCardViewModels.RemoveAt(0);
            }
            else
            {
                firstCard.Items.RemoveAt(0);
            }
        }
    }

    public sealed record OperatorOption(string Id, string Name, int Rarity, OperatorRole Role);
}
