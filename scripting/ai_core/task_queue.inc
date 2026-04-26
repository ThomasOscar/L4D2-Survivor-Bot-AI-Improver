/**
 * L4D2 AI Bot - Task Queue Engine
 * 
 * Core scheduling logic: registers tasks, sorts by priority, and executes
 * the highest-priority applicable task each frame for each bot.
 * 
 * Architecture inspired by VScript Advanced Bot AI's task traversal system.
 */

#if defined _ai_task_queue_included
 #endinput
#endif
#define _ai_task_queue_included

// Global task registry
AITaskDef g_Tasks[MAX_TASKS];
int g_iTaskCount = 0;

// Debug: per-bot last reported task (to avoid spam)
static int g_iDebugLastTask[MAXPLAYERS + 1];

/**
 * Register a task into the global task queue.
 * Tasks are automatically sorted by 'order' (ascending = higher priority).
 *
 * @param id        Task ID from AITaskId enum
 * @param order     Priority order (0 = highest priority)
 * @param name      Human-readable name for debug
 * @param tickInterval  Minimum interval between checks (0.0 = every frame)
 */
void AITaskQueue_RegisterTask(AITaskId id, int order, const char[] name, float tickInterval = 0.0) {
    if (g_iTaskCount >= MAX_TASKS) {
        LogError("[AI Bot] Cannot register task '%s': MAX_TASKS (%d) reached", name, MAX_TASKS);
        return;
    }

    g_Tasks[g_iTaskCount].id = id;
    g_Tasks[g_iTaskCount].order = order;
    strcopy(g_Tasks[g_iTaskCount].name, sizeof(AITaskDef::name), name);
    g_Tasks[g_iTaskCount].enabled = true;
    g_Tasks[g_iTaskCount].tickInterval = tickInterval;
    g_Tasks[g_iTaskCount].lastTickTime = 0.0;
    g_iTaskCount++;

    // Sort by order after insertion
    SortTasksByOrder();
}

/**
 * Sort tasks by priority order (insertion sort, ascending).
 * Lower order = higher priority = checked first.
 */
static void SortTasksByOrder() {
    AITaskDef temp;
    for (int i = 1; i < g_iTaskCount; i++) {
        // Copy g_Tasks[i] into temp
        temp.id = g_Tasks[i].id;
        temp.order = g_Tasks[i].order;
        strcopy(temp.name, sizeof(AITaskDef::name), g_Tasks[i].name);
        temp.enabled = g_Tasks[i].enabled;
        temp.tickInterval = g_Tasks[i].tickInterval;
        temp.lastTickTime = g_Tasks[i].lastTickTime;

        int j = i - 1;
        while (j >= 0 && g_Tasks[j].order > temp.order) {
            // Shift g_Tasks[j] to g_Tasks[j+1]
            g_Tasks[j + 1].id = g_Tasks[j].id;
            g_Tasks[j + 1].order = g_Tasks[j].order;
            strcopy(g_Tasks[j + 1].name, sizeof(AITaskDef::name), g_Tasks[j].name);
            g_Tasks[j + 1].enabled = g_Tasks[j].enabled;
            g_Tasks[j + 1].tickInterval = g_Tasks[j].tickInterval;
            g_Tasks[j + 1].lastTickTime = g_Tasks[j].lastTickTime;
            j--;
        }
        // Place temp at j+1
        g_Tasks[j + 1].id = temp.id;
        g_Tasks[j + 1].order = temp.order;
        strcopy(g_Tasks[j + 1].name, sizeof(AITaskDef::name), temp.name);
        g_Tasks[j + 1].enabled = temp.enabled;
        g_Tasks[j + 1].tickInterval = temp.tickInterval;
        g_Tasks[j + 1].lastTickTime = temp.lastTickTime;
    }
}

/**
 * Core execution function — called every frame for each bot from OnPlayerRunCmd.
 * Iterates tasks in priority order and executes the first applicable one.
 *
 * @param client    Bot client index
 * @param buttons   Reference to button bitfield (IN_ATTACK, IN_USE, etc.)
 * @return          true if a task locked execution, false otherwise
 */
bool AITaskQueue_Execute(int client, int &buttons, float vel[3]) {
    float curTime = GetGameTime();

    // If bot is pinned by SI, skip all task processing
    if (g_BotState[client].controlledByType > 0) {
        return false;
    }

    // Iterate tasks in priority order (lowest order number = highest priority)
    for (int i = 0; i < g_iTaskCount; i++) {
        if (!g_Tasks[i].enabled) continue;

        AITaskId taskId = g_Tasks[i].id;
        int taskOrder = g_Tasks[i].order;

        // Check tick interval throttle
        if (g_Tasks[i].tickInterval > 0.0 &&
            (curTime - g_Tasks[i].lastTickTime) < g_Tasks[i].tickInterval) {
            // If this task is currently locked by this bot, let it continue
            if (g_BotState[client].taskLocked && g_BotState[client].currentTaskId == view_as<int>(taskId)) {
                // Still locked, skip interval check for active task
            } else {
                continue;
            }
        }

        // Check if this task should run for this bot
        bool shouldUpdate = AITask_ShouldUpdate(taskId, client);

        if (shouldUpdate) {
            // If a lower-priority task is currently locked, preempt it
            if (g_BotState[client].taskLocked &&
                g_BotState[client].currentTaskOrder > taskOrder) {
                AITask_Reset(view_as<AITaskId>(g_BotState[client].currentTaskId), client);
                g_BotState[client].taskLocked = false;
            }

            // If a same-or-higher priority task is locked and it's not this task, skip
            if (g_BotState[client].taskLocked &&
                g_BotState[client].currentTaskOrder <= taskOrder &&
                g_BotState[client].currentTaskId != view_as<int>(taskId)) {
                continue;
            }

            // Execute the task
            bool isLocked = AITask_Update(taskId, client, buttons, vel);

            // Only update lastTickTime after task actually executes
            g_Tasks[i].lastTickTime = curTime;

            if (isLocked) {
                g_BotState[client].currentTaskId = view_as<int>(taskId);
                g_BotState[client].currentTaskOrder = taskOrder;
                g_BotState[client].taskLocked = true;
                if (g_BotState[client].taskStartTime == 0.0) {
                    g_BotState[client].taskStartTime = curTime;
                }

                // Debug: log task switch with reason
                if (g_bDebugMode && g_iDebugLastTask[client] != view_as<int>(taskId)) {
                    g_iDebugLastTask[client] = view_as<int>(taskId);
                    char taskName[32];
                    AITask_GetName(taskId, taskName, sizeof(taskName));
                    char reason[128];
                    strcopy(reason, sizeof(reason), g_BotState[client].taskSwitchReason);
                    if (strlen(reason) == 0) strcopy(reason, sizeof(reason), "locked");
                    AI_DebugDecision(client, "→ %s (%s)", taskName, reason);
                }

                return true;  // Task locked, skip remaining tasks
            }
        } else {
            // If this task was previously active but no longer needed, reset it
            if (g_BotState[client].currentTaskId == view_as<int>(taskId)) {
                AITask_Reset(taskId, client);
                g_BotState[client].currentTaskId = view_as<int>(TASK_NONE);
                g_BotState[client].currentTaskOrder = -1;
                g_BotState[client].taskLocked = false;
                g_BotState[client].taskStartTime = 0.0;
            }
        }
    }

    return false;
}

// ============================================================
// Task Dispatcher — maps task IDs to concrete task functions.
// Uncomment cases as task modules are implemented.
// ============================================================

/**
 * Check if a task should execute for the given bot.
 */
bool AITask_ShouldUpdate(AITaskId taskId, int client) {
    switch (taskId) {
        case TASK_RESCUE:     return TaskRescue_ShouldUpdate(client);
        case TASK_COMBAT:     return TaskCombat_ShouldUpdate(client);
        case TASK_THROWABLE:  return TaskThrowable_ShouldUpdate(client);
        case TASK_HEAL:       return TaskHeal_ShouldUpdate(client);
        case TASK_NAVIGATE:   return TaskNavigate_ShouldUpdate(client);
        case TASK_WEAPON:     return TaskWeapon_ShouldUpdate(client);
        case TASK_PROP_CARRY: return TaskPropCarry_ShouldUpdate(client);
        case TASK_SEARCH:     return TaskSearch_ShouldUpdate(client);
        case TASK_ITEM_PASS:  return TaskItemPass_ShouldUpdate(client);
        case TASK_TRIGGER_SEARCH: return TaskTriggerSearch_ShouldUpdate(client);
        case TASK_DEFIB:    return TaskDefib_ShouldUpdate(client);
        case TASK_UPGRADE:  return TaskUpgrade_ShouldUpdate(client);
        case TASK_SUPPLY:   return TaskSupply_ShouldUpdate(client);
        // TASK_DAMAGE_PROTECT is SDKHook-driven, no ShouldUpdate needed
        default: {}
    }
    return false;
}

/**
 * Execute a task's update logic.
 * @return true if the task wants to lock (prevent lower-priority tasks).
 */
bool AITask_Update(AITaskId taskId, int client, int &buttons, float vel[3]) {
    switch (taskId) {
        case TASK_RESCUE:     return TaskRescue_Update(client, buttons, vel);
        case TASK_COMBAT:     return TaskCombat_Update(client, buttons, vel);
        case TASK_THROWABLE:  return TaskThrowable_Update(client, buttons, vel);
        case TASK_HEAL:       return TaskHeal_Update(client, buttons, vel);
        case TASK_NAVIGATE:   return TaskNavigate_Update(client, buttons, vel);
        case TASK_WEAPON:     return TaskWeapon_Update(client, buttons, vel);
        case TASK_PROP_CARRY: return TaskPropCarry_Update(client, buttons, vel);
        case TASK_SEARCH:     return TaskSearch_Update(client, buttons, vel);
        case TASK_ITEM_PASS:  return TaskItemPass_Update(client, buttons, vel);
        case TASK_TRIGGER_SEARCH: return TaskTriggerSearch_Update(client, buttons, vel);
        case TASK_DEFIB:    return TaskDefib_Update(client, buttons, vel);
        case TASK_UPGRADE:  return TaskUpgrade_Update(client, buttons, vel);
        case TASK_SUPPLY:   return TaskSupply_Update(client, buttons, vel);
        // TASK_DAMAGE_PROTECT is SDKHook-driven, no Update needed
        default: {}
    }
    return false;
}

/**
 * Reset/cleanup a task's state when it is preempted or no longer needed.
 */
void AITask_Reset(AITaskId taskId, int client) {
    switch (taskId) {
        case TASK_RESCUE:     TaskRescue_Reset(client);
        case TASK_COMBAT:     TaskCombat_Reset(client);
        case TASK_THROWABLE:  TaskThrowable_Reset(client);
        case TASK_HEAL:       TaskHeal_Reset(client);
        case TASK_NAVIGATE:   TaskNavigate_Reset(client);
        case TASK_WEAPON:     TaskWeapon_Reset(client);
        case TASK_PROP_CARRY: TaskPropCarry_Reset(client);
        case TASK_SEARCH:     TaskSearch_Reset(client);
        case TASK_ITEM_PASS:  TaskItemPass_Reset(client);
        case TASK_TRIGGER_SEARCH: TaskTriggerSearch_Reset(client);
        case TASK_DEFIB:    TaskDefib_Reset(client);
        case TASK_UPGRADE:  TaskUpgrade_Reset(client);
        case TASK_SUPPLY:   TaskSupply_Reset(client);
        // TASK_DAMAGE_PROTECT has no task state to reset
        default: {}
    }
}

/**
 * Initialize the task queue. Called from OnPluginStart.
 * Registers all tasks in priority order.
 */
void AITaskQueue_Init() {
    g_iTaskCount = 0;

    // Register tasks by priority (uncomment as modules are implemented)
    AITaskQueue_RegisterTask(TASK_RESCUE,     0, "Rescue");
    AITaskQueue_RegisterTask(TASK_DEFIB,      0, "Defib", 0.5);       // Defib = same as rescue (saving lives)
    AITaskQueue_RegisterTask(TASK_COMBAT,     1, "Combat", 0.15);    // Throttle combat scan (6.7 Hz instead of 30 Hz)
    AITaskQueue_RegisterTask(TASK_THROWABLE,  2, "Throwable");
    AITaskQueue_RegisterTask(TASK_HEAL,       2, "Heal");
    AITaskQueue_RegisterTask(TASK_NAVIGATE,   3, "Navigate");
    AITaskQueue_RegisterTask(TASK_WEAPON,     3, "Weapon");
    AITaskQueue_RegisterTask(TASK_PROP_CARRY, 4, "PropCarry");
    AITaskQueue_RegisterTask(TASK_SEARCH,     4, "Search");
    AITaskQueue_RegisterTask(TASK_ITEM_PASS,  3, "ItemPass");
    AITaskQueue_RegisterTask(TASK_TRIGGER_SEARCH, 5, "TriggerSearch");
    AITaskQueue_RegisterTask(TASK_UPGRADE,  4, "Upgrade", 2.0);
    AITaskQueue_RegisterTask(TASK_SUPPLY,   3, "Supply", 1.0);  // Supply management for bots
    // TASK_DAMAGE_PROTECT is SDKHook-only — no queue registration needed

    // Initialize rescue global state
    TaskRescue_InitGlobals();
}
