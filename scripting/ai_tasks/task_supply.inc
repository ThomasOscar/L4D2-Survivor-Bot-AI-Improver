/**
 * AI Task: Supply Management
 *
 * Priority Order: 3
 * Ensures bots maintain adequate supplies (weapons, medkit, pills, throwable).
 * Bots follow the team and pick up missing items when nearby.
 * If picking up takes the bot too far from the team, teleport back.
 * Gives items to human players who are missing supplies (with cooldowns).
 *
 * Supply priorities:
 *  1. Primary weapon (if missing)
 *  2. Secondary weapon (if missing)
 *  3. Medkit / Defib (slot 3 - if empty)
 *  4. Pills/Adrenaline (slot 4 - if empty)
 *  5. Throwable (slot 2 - if empty)
 *  6. Ammo (if clip+reserve empty)
 *  7. Give items to human players who need them
 */

#if defined _task_supply_included
    #endinput
#endif
#define _task_supply_included

// Supply constants
#define SUPPLY_SEARCH_RANGE     600.0   // Range to scan for nearby items
#define SUPPLY_PICKUP_RANGE     60.0    // Range to pick up items
#define SUPPLY_TEAM_TELEPORT    500.0   // If this far from team after pickup, teleport back
#define SUPPLY_GIVE_RANGE       150.0   // Range to give items to players
#define SUPPLY_AMMO_RANGE       300.0   // Range for ammo pile
#define SUPPLY_SCAN_INTERVAL    1.5     // Seconds between scans

// Slot type constants
enum {
    SUPPLY_SLOT_NONE = 0,
    SUPPLY_SLOT_PRIMARY,
    SUPPLY_SLOT_SECONDARY,
    SUPPLY_SLOT_MEDKIT,
    SUPPLY_SLOT_PILLS,
    SUPPLY_SLOT_THROWABLE,
    SUPPLY_SLOT_AMMO
};

// Per-bot supply state
static int g_iSupplyTarget[MAXPLAYERS + 1];         // Target entity to pick up
static int g_iSupplySlotType[MAXPLAYERS + 1];        // Which slot type we're searching for
static float g_fSupplyScanTime[MAXPLAYERS + 1];      // Last scan time (for throttle)
static float g_fSupplyPickupTime[MAXPLAYERS + 1];    // Time when pickup started (for stuck detection)

/**
 * Check if supply task should activate for this bot.
 */
bool TaskSupply_ShouldUpdate(int client) {
    if (!g_bSupplyEnabled) return false;
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;
    if (g_BotState[client].isInCombat) return false;
    if (g_BotState[client].isRescuing) return false;
    if (g_BotState[client].isHealing) return false;

    // If already tracking a supply target, continue
    if (g_iSupplyTarget[client] > 0 && IsValidEntity(g_iSupplyTarget[client])) {
        return true;
    }

    float curTime = GetGameTime();
    if ((curTime - g_fSupplyScanTime[client]) < SUPPLY_SCAN_INTERVAL) {
        return false;
    }
    g_fSupplyScanTime[client] = curTime;

    // Check what this bot is missing
    int missingSlot = TaskSupply_GetMissingSlot(client);
    if (missingSlot != SUPPLY_SLOT_NONE) {
        // Find nearby item for this slot
        int target = TaskSupply_FindNearest(client, missingSlot);
        if (target != -1) {
            g_iSupplyTarget[client] = target;
            g_iSupplySlotType[client] = missingSlot;
            g_fSupplyPickupTime[client] = GetGameTime();
            return true;
        }
    }

    // Check if any nearby human player needs items we can give
    if (TaskSupply_HasPlayerNeedingItem(client)) {
        return true;
    }

    return false;
}

/**
 * Execute supply logic: move toward item, pick up, give to players.
 * @return false (never locks — interruptible by higher priority tasks).
 */
bool TaskSupply_Update(int client, int &buttons, float vel[3]) {
    // === Phase 1: Give items to human players who need them ===
    if (TaskSupply_TryGiveToPlayer(client, buttons)) {
        return false;  // Don't lock
    }

    // === Phase 2: Pick up missing items ===
    int target = g_iSupplyTarget[client];
    if (target <= 0 || !IsValidEntity(target)) {
        // Lost our target
        TaskSupply_Reset(client);
        return false;
    }

    // Check if we already have this slot filled (someone gave us an item)
    int missingSlot = TaskSupply_GetMissingSlot(client);
    if (missingSlot == SUPPLY_SLOT_NONE) {
        // All slots filled — check if too far from team and teleport back
        TaskSupply_TeleportBackToTeam(client);
        TaskSupply_Reset(client);
        return false;
    }

    // For ammo, check if we still need ammo
    if (g_iSupplySlotType[client] == SUPPLY_SLOT_AMMO) {
        int primaryWeapon = GetPlayerWeaponSlot(client, 0);
        if (primaryWeapon != -1) {
            int clip = GetEntProp(primaryWeapon, Prop_Send, "m_iClip1");
            int reserve = GetEntProp(primaryWeapon, Prop_Send, "m_iPrimaryAmmoCount");
            if (clip > 0 || reserve > 0) {
                TaskSupply_Reset(client);
                return false;
            }
        }
    }

    // Check if target entity is still valid/unowned
    if (!TaskSupply_IsAvailable(target)) {
        TaskSupply_Reset(client);
        return false;
    }

    float targetPos[3];
    GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", targetPos);
    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

    // Check stuck teleport
    float pickupElapsed = GetGameTime() - g_fSupplyPickupTime[client];
    if (AI_ShouldAbandonNavigation(client)) {
        TaskSupply_Reset(client);
        return false;
    }
    AI_TryStuckTeleport(client, targetPos, dist, pickupElapsed, 300.0, 8.0);

    if (dist <= SUPPLY_PICKUP_RANGE) {
        // Close enough — look at item and pick up
        targetPos[2] += 10.0;
        AI_SnapViewToPosition(client, targetPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
        AI_PressUseButton(client, buttons);

        // After pickup, check if too far from team and teleport back
        TaskSupply_TeleportBackToTeam(client);

        // Keep target for a few more frames (pickup may need multiple frames)
        return false;
    }

    // Move toward item
    AI_ComputeMoveToward(client, targetPos, g_BotState[client].absOrigin,
                          g_BotState[client].eyeAngles, vel);
    return false;  // Never lock
}

/**
 * Reset supply state.
 */
void TaskSupply_Reset(int client) {
    g_iSupplyTarget[client] = -1;
    g_iSupplySlotType[client] = SUPPLY_SLOT_NONE;
    g_fSupplyPickupTime[client] = 0.0;
    AI_ResetTeleportState(client);
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Determine which supply slot the bot is missing (highest priority first).
 */
static int TaskSupply_GetMissingSlot(int client) {
    // Priority 1: Primary weapon
    if (GetPlayerWeaponSlot(client, 0) == -1) {
        return SUPPLY_SLOT_PRIMARY;
    }

    // Priority 2: Secondary weapon (pistol)
    if (GetPlayerWeaponSlot(client, 1) == -1) {
        return SUPPLY_SLOT_SECONDARY;
    }

    // Priority 3: Medkit or defib (slot 3)
    if (GetPlayerWeaponSlot(client, 3) == -1) {
        return SUPPLY_SLOT_MEDKIT;
    }

    // Priority 4: Pills/Adrenaline (slot 4)
    if (GetPlayerWeaponSlot(client, 4) == -1) {
        return SUPPLY_SLOT_PILLS;
    }

    // Priority 5: Throwable (slot 2)
    if (GetPlayerWeaponSlot(client, 2) == -1) {
        return SUPPLY_SLOT_THROWABLE;
    }

    // Priority 6: Ammo (clip + reserve both empty)
    int primaryWeapon = GetPlayerWeaponSlot(client, 0);
    if (primaryWeapon != -1) {
        int clip = GetEntProp(primaryWeapon, Prop_Send, "m_iClip1");
        int reserve = GetEntProp(primaryWeapon, Prop_Send, "m_iPrimaryAmmoCount");
        if (clip <= 0 && reserve <= 0) {
            return SUPPLY_SLOT_AMMO;
        }
    }

    return SUPPLY_SLOT_NONE;
}

/**
 * Find the nearest available item for the given slot type.
 */
static int TaskSupply_FindNearest(int client, int slotType) {
    float botPos[3];
    botPos = g_BotState[client].absOrigin;
    int best = -1;
    float bestDist = SUPPLY_SEARCH_RANGE + 1.0;

    if (slotType == SUPPLY_SLOT_PRIMARY) {
        static const char primaryClasses[][] = {
            "weapon_smg", "weapon_pumpshotgun", "weapon_rifle",
            "weapon_autoshotgun", "weapon_hunting_rifle", "weapon_smg_silenced",
            "weapon_smg_mp5", "weapon_rifle_desert", "weapon_rifle_sg552",
            "weapon_shotgun_chrome", "weapon_shotgun_spas", "weapon_rifle_ak47",
            "weapon_sniper_military", "weapon_sniper_scout", "weapon_sniper_awp",
            "weapon_rifle_m60", "weapon_grenade_launcher"
        };
        static const char spawnClasses[][] = {
            "weapon_smg_spawn", "weapon_pumpshotgun_spawn",
            "weapon_rifle_spawn", "weapon_autoshotgun_spawn",
            "weapon_hunting_rifle_spawn", "weapon_smg_silenced_spawn",
            "weapon_rifle_desert_spawn", "weapon_shotgun_chrome_spawn",
            "weapon_shotgun_spas_spawn", "weapon_rifle_ak47_spawn",
            "weapon_sniper_military_spawn", "weapon_rifle_m60_spawn",
            "weapon_grenade_launcher_spawn"
        };
        best = TaskSupply_FindNearestOfClass(primaryClasses, sizeof(primaryClasses), botPos, bestDist, best);
        if (best != -1) { float ePos[3]; GetEntPropVector(best, Prop_Data, "m_vecAbsOrigin", ePos); bestDist = AI_GetVectorDistance(botPos, ePos); }
        best = TaskSupply_FindNearestOfClass(spawnClasses, sizeof(spawnClasses), botPos, bestDist, best);
    } else if (slotType == SUPPLY_SLOT_SECONDARY) {
        static const char secondaryClasses[][] = {
            "weapon_pistol", "weapon_pistol_magnum",
            "weapon_pistol_spawn", "weapon_pistol_magnum_spawn",
            "weapon_melee", "weapon_melee_spawn"
        };
        best = TaskSupply_FindNearestOfClass(secondaryClasses, sizeof(secondaryClasses), botPos, bestDist, best);
    } else if (slotType == SUPPLY_SLOT_MEDKIT) {
        static const char medkitClasses[][] = {
            "weapon_first_aid_kit", "weapon_first_aid_kit_spawn",
            "weapon_defibrillator", "weapon_defibrillator_spawn"
        };
        best = TaskSupply_FindNearestOfClass(medkitClasses, sizeof(medkitClasses), botPos, bestDist, best);
    } else if (slotType == SUPPLY_SLOT_PILLS) {
        static const char pillsClasses[][] = {
            "weapon_pain_pills", "weapon_pain_pills_spawn",
            "weapon_adrenaline", "weapon_adrenaline_spawn"
        };
        best = TaskSupply_FindNearestOfClass(pillsClasses, sizeof(pillsClasses), botPos, bestDist, best);
    } else if (slotType == SUPPLY_SLOT_THROWABLE) {
        static const char throwableClasses[][] = {
            "weapon_pipe_bomb", "weapon_pipe_bomb_spawn",
            "weapon_molotov", "weapon_molotov_spawn",
            "weapon_vomitjar", "weapon_vomitjar_spawn"
        };
        best = TaskSupply_FindNearestOfClass(throwableClasses, sizeof(throwableClasses), botPos, bestDist, best);
    } else if (slotType == SUPPLY_SLOT_AMMO) {
        int ent = -1;
        int safetyCount = 0;
        while ((ent = FindEntityByClassname(ent, "weapon_ammo_spawn")) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(ent)) { ent++; continue; }
            float ePos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", ePos);
            float d = AI_GetVectorDistance(botPos, ePos);
            if (d < bestDist) {
                bestDist = d;
                best = ent;
            }
        }
    }

    return best;
}

/**
 * Find the nearest available entity from a list of classnames.
 */
static int TaskSupply_FindNearestOfClass(const char[][] classes, int numClasses,
                                          const float botPos[3], float bestDist, int currentBest) {
    int best = currentBest;
    for (int i = 0; i < numClasses; i++) {
        int ent = -1;
        int safetyCount = 0;
        while ((ent = FindEntityByClassname(ent, classes[i])) != -1) {
            if (++safetyCount > 256) break;
            if (!IsValidEntity(ent)) { ent++; continue; }
            if (!TaskSupply_IsAvailable(ent)) continue;

            // For spawn entities, check if they still have items
            char cls[64];
            GetEntityClassname(ent, cls, sizeof(cls));
            if (StrContains(cls, "_spawn") != -1) {
                int offset = FindDataMapInfo(ent, "m_nCount");
                if (offset > 0) {
                    int count = GetEntProp(ent, Prop_Data, "m_nCount");
                    if (count <= 0) continue;
                }
                // Spawn entities are always "available" (no owner check)
            }

            float ePos[3];
            GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", ePos);
            float d = AI_GetVectorDistance(botPos, ePos);
            if (d < bestDist) {
                bestDist = d;
                best = ent;
            }
        }
    }
    return best;
}

/**
 * Check if an entity is available (not held by a living player).
 */
static bool TaskSupply_IsAvailable(int entity) {
    if (!IsValidEntity(entity)) return false;

    // Spawn entities are always available
    char cls[64];
    GetEntityClassname(entity, cls, sizeof(cls));
    if (StrContains(cls, "_spawn") != -1) return true;

    // Check owner — only skip if held by a living player
    if (HasEntProp(entity, Prop_Data, "m_hOwnerEntity")) {
        int owner = GetEntPropEnt(entity, Prop_Data, "m_hOwnerEntity");
        if (owner > 0 && owner <= MaxClients && IsClientInGame(owner) && IsPlayerAlive(owner)) {
            return false;
        }
    }
    return true;
}

/**
 * If bot is too far from the team after picking up an item, teleport back.
 */
static void TaskSupply_TeleportBackToTeam(int client) {
    float botPos[3];
    botPos = g_BotState[client].absOrigin;

    // Find the nearest teammate (prefer human players)
    float nearestTeammateDist = 999999.0;
    float nearestTeammatePos[3];
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;

        float tmPos[3];
        GetClientAbsOrigin(i, tmPos);
        float d = AI_GetVectorDistance(botPos, tmPos);
        if (d < nearestTeammateDist) {
            nearestTeammateDist = d;
            nearestTeammatePos = tmPos;
        }
    }

    if (nearestTeammateDist > SUPPLY_TEAM_TELEPORT && nearestTeammateDist < 999999.0) {
        // Too far from team — teleport near nearest teammate
        float candidate[3];
        candidate[0] = nearestTeammatePos[0] + GetRandomFloat(-60.0, 60.0);
        candidate[1] = nearestTeammatePos[1] + GetRandomFloat(-60.0, 60.0);
        candidate[2] = nearestTeammatePos[2] + 50.0;  // Start search from above

        float telePos[3];
        if (AI_ValidateTeleportPosition(candidate, telePos)) {
            TeleportEntity(client, telePos, NULL_VECTOR, NULL_VECTOR);

            if (g_bDebugMode) {
                char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                PrintToServer("[AI Bot] %s supply: teleported back to team (was %.0f units away)", dbgN, nearestTeammateDist);
            }
        }
    }
}

/**
 * Check if any nearby human player needs an item we can give.
 */
static bool TaskSupply_HasPlayerNeedingItem(int client) {
    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || IsFakeClient(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (AI_IsPlayerIncapacitated(i)) continue;

        float tmPos[3];
        GetClientAbsOrigin(i, tmPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
        if (dist > SUPPLY_GIVE_RANGE) continue;

        // Check cooldown
        float curTime = GetGameTime();
        if (g_BotState[client].supplyGiveCooldown[i] > curTime) continue;

        // Check what the player needs
        // Medkit (slot 3)
        if (GetPlayerWeaponSlot(i, 3) == -1 && GetPlayerWeaponSlot(client, 3) != -1) {
            int mySlot3 = GetPlayerWeaponSlot(client, 3);
            char cls[64];
            GetEntityClassname(mySlot3, cls, sizeof(cls));
            if (StrEqual(cls, "weapon_first_aid_kit")) {
                return true;
            }
        }

        // Pills/Adrenaline (slot 4)
        if (GetPlayerWeaponSlot(i, 4) == -1 && GetPlayerWeaponSlot(client, 4) != -1) {
            return true;
        }

        // Throwable (slot 2)
        if (GetPlayerWeaponSlot(i, 2) == -1 && GetPlayerWeaponSlot(client, 2) != -1) {
            return true;
        }
    }
    return false;
}

/**
 * Try to give items to nearby human players who need them.
 * @return true if gave an item this frame.
 */
static bool TaskSupply_TryGiveToPlayer(int client, int &buttons) {
    float curTime = GetGameTime();

    for (int i = 1; i <= MaxClients; i++) {
        if (i == client) continue;
        if (!IsClientInGame(i) || IsFakeClient(i) || !IsPlayerAlive(i)) continue;
        if (GetClientTeam(i) != 2) continue;
        if (AI_IsPlayerIncapacitated(i)) continue;

        // Check cooldown
        if (g_BotState[client].supplyGiveCooldown[i] > curTime) continue;

        float tmPos[3];
        GetClientAbsOrigin(i, tmPos);
        float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, tmPos);
        if (dist > SUPPLY_GIVE_RANGE) continue;

        // Priority: Medkit > Pills > Throwable
        // Give medkit
        if (GetPlayerWeaponSlot(i, 3) == -1) {
            int mySlot3 = GetPlayerWeaponSlot(client, 3);
            if (mySlot3 != -1) {
                char cls[64];
                GetEntityClassname(mySlot3, cls, sizeof(cls));
                if (StrEqual(cls, "weapon_first_aid_kit")) {
                    // Drop medkit toward player
                    float lookPos[3];
                    lookPos[0] = tmPos[0];
                    lookPos[1] = tmPos[1];
                    lookPos[2] = tmPos[2] + 40.0;
                    AI_SnapViewToPosition(client, lookPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                    FakeClientCommand(client, "slot4");  // Switch to slot 3 (0-indexed, slot4 = medkit slot)
                    FakeClientCommand(client, "drop");
                    g_BotState[client].supplyGiveCooldown[i] = curTime + g_fSupplyGiveCooldown;

                    if (g_bDebugMode) {
                        char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                        char tgtN[64]; GetClientName(i, tgtN, sizeof(tgtN));
                        PrintToServer("[AI Bot] %s gave medkit to player %s", dbgN, tgtN);
                    }
                    return true;
                }
            }
        }

        // Give pills/adrenaline
        if (GetPlayerWeaponSlot(i, 4) == -1) {
            int mySlot4 = GetPlayerWeaponSlot(client, 4);
            if (mySlot4 != -1) {
                float lookPos[3];
                lookPos[0] = tmPos[0];
                lookPos[1] = tmPos[1];
                lookPos[2] = tmPos[2] + 40.0;
                AI_SnapViewToPosition(client, lookPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                FakeClientCommand(client, "slot5");  // Switch to slot 4 (0-indexed, slot5 = pills slot)
                FakeClientCommand(client, "drop");
                g_BotState[client].supplyGiveCooldown[i] = curTime + g_fSupplyGiveCooldown;

                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    char tgtN[64]; GetClientName(i, tgtN, sizeof(tgtN));
                    PrintToServer("[AI Bot] %s gave pills to player %s", dbgN, tgtN);
                }
                return true;
            }
        }

        // Give throwable
        if (GetPlayerWeaponSlot(i, 2) == -1) {
            int mySlot2 = GetPlayerWeaponSlot(client, 2);
            if (mySlot2 != -1) {
                float lookPos[3];
                lookPos[0] = tmPos[0];
                lookPos[1] = tmPos[1];
                lookPos[2] = tmPos[2] + 40.0;
                AI_SnapViewToPosition(client, lookPos, g_BotState[client].eyePos, g_BotState[client].eyeAngles);
                FakeClientCommand(client, "slot3");  // Switch to slot 2 (0-indexed, slot3 = throwable slot)
                FakeClientCommand(client, "drop");
                g_BotState[client].supplyGiveCooldown[i] = curTime + g_fSupplyGiveCooldown;

                if (g_bDebugMode) {
                    char dbgN[64]; GetClientName(client, dbgN, sizeof(dbgN));
                    char tgtN[64]; GetClientName(i, tgtN, sizeof(tgtN));
                    PrintToServer("[AI Bot] %s gave throwable to player %s", dbgN, tgtN);
                }
                return true;
            }
        }
    }

    return false;
}
