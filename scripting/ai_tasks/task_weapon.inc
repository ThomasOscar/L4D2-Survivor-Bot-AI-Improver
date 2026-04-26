/**
 * AI Task: Weapon Management
 * 
 * Priority Order: 3
 * Handles weapon switching based on combat range and ammo management.
 */

#if defined _task_weapon_included
    #endinput
#endif
#define _task_weapon_included

// Weapon range thresholds
#define WEAPON_CLOSE_RANGE      200.0
#define WEAPON_FAR_RANGE        400.0
#define WEAPON_RELOAD_THRESHOLD 0.3     // Reload at 30% clip
#define WEAPON_SWITCH_COOLDOWN  2.0     // Don't switch too fast
#define WEAPON_AMMO_EMERGENCY_TIME 20.0 // Seconds with no ammo before emergency refill
#define WEAPON_AMMO_CHECK_INTERVAL 5.0  // How often to check ammo status
#define WEAPON_AMMO_REFILL_COOLDOWN 60.0 // Cooldown between emergency refills per bot

// Per-bot weapon state
static float g_fWeaponLastSwitchTime[MAXPLAYERS + 1];
static float g_fWeaponAmmoEmptyTime[MAXPLAYERS + 1];   // When ammo first became empty
static float g_fWeaponLastAmmoCheck[MAXPLAYERS + 1];    // Last ammo status check time
static float g_fLastEmergencyRefill[MAXPLAYERS + 1];    // Last emergency refill time

/**
 * Check if weapon task should activate for this bot.
 */
bool TaskWeapon_ShouldUpdate(int client) {
    if (g_BotState[client].controlledByType > 0) return false;
    if (g_BotState[client].isIncapacitated) return false;

    // Check if reload is needed
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (IsValidEntity(activeWeapon)) {
        if (HasEntProp(activeWeapon, Prop_Send, "m_iClip1")) {
            int clip = GetEntProp(activeWeapon, Prop_Send, "m_iClip1");
            if (clip == 0) return true;  // Empty clip needs reload
        }
    }

    // Check if weapon switch would be beneficial
    if (g_BotState[client].isInCombat) {
        float curTime = GetGameTime();
        if ((curTime - g_fWeaponLastSwitchTime[client]) < WEAPON_SWITCH_COOLDOWN) {
            return false;
        }
        return TaskWeapon_ShouldSwitch(client);
    }

    return false;
}

// Ammo pile proximity cache (avoid FindEntityByClassname every frame when clip==0)
static float g_fAmmoNearCacheTime[MAXPLAYERS + 1] = {0.0, ...};
static bool g_bAmmoNearCacheResult[MAXPLAYERS + 1] = {false, ...};
#define AMMO_NEAR_CACHE_INTERVAL 2.0

/**
 * Execute weapon management: switch weapons, reload.
 * @return false (never locks).
 */
bool TaskWeapon_Update(int client, int &buttons, float vel[3]) {
    #pragma unused vel
    int safetyCount;

    float curTime = GetGameTime();

    // === Emergency ammo refill ===
    // If bot's primary weapon has been completely out of ammo for a long time
    // and there's no ammo source nearby, refill one magazine worth of ammo.
    if ((curTime - g_fWeaponLastAmmoCheck[client]) >= WEAPON_AMMO_CHECK_INTERVAL) {
        g_fWeaponLastAmmoCheck[client] = curTime;

        int primary = GetPlayerWeaponSlot(client, 0);
        if (primary != -1 && IsValidEntity(primary) && HasEntProp(primary, Prop_Send, "m_iClip1")) {
            int clip = GetEntProp(primary, Prop_Send, "m_iClip1");
            int reserve = 0;
            if (HasEntProp(primary, Prop_Send, "m_iPrimaryAmmoCount")) {
                reserve = GetEntProp(primary, Prop_Send, "m_iPrimaryAmmoCount");
            }

            if (clip <= 0 && reserve <= 0) {
                // Completely out of ammo — track when this started
                if (g_fWeaponAmmoEmptyTime[client] <= 0.0) {
                    g_fWeaponAmmoEmptyTime[client] = curTime;
                }

                float emptyDuration = curTime - g_fWeaponAmmoEmptyTime[client];
                if (emptyDuration >= WEAPON_AMMO_EMERGENCY_TIME) {
                    // Check: is there any ammo pile nearby?
                    bool hasNearbyAmmo = false;
                    int ammoEnt = -1;
                    safetyCount = 0;
                    float clientPos[3];
                    GetClientAbsOrigin(client, clientPos);
                    while ((ammoEnt = FindEntityByClassname(ammoEnt, "weapon_ammo_spawn")) != -1) {
                        if (++safetyCount > 256) break;
                        if (!IsValidEntity(ammoEnt)) { ammoEnt++; continue; }
                        float ammoPos[3];
                        GetEntPropVector(ammoEnt, Prop_Data, "m_vecAbsOrigin", ammoPos);
                        if (GetVectorDistance(clientPos, ammoPos) < 800.0) {
                            hasNearbyAmmo = true;
                            break;
                        }
                    }

                    // Check: is there a weapon on the ground we could swap to?
                    bool hasNearbyWeapon = false;
                    int wepEnt = -1;
                    safetyCount = 0;
                    while ((wepEnt = FindEntityByClassname(wepEnt, "weapon_rifle")) != -1 ||
                           (wepEnt = FindEntityByClassname(wepEnt, "weapon_smg")) != -1 ||
                           (wepEnt = FindEntityByClassname(wepEnt, "weapon_pumpshotgun")) != -1 ||
                           (wepEnt = FindEntityByClassname(wepEnt, "weapon_autoshotgun")) != -1) {
                        if (++safetyCount > 256) break;
                        if (!IsValidEntity(wepEnt)) { wepEnt++; continue; }
                        int owner = GetEntPropEnt(wepEnt, Prop_Data, "m_hOwnerEntity");
                        if (owner > 0 && owner <= MaxClients && IsPlayerAlive(owner)) continue;
                        float wepPos[3];
                        GetEntPropVector(wepEnt, Prop_Data, "m_vecAbsOrigin", wepPos);
                        if (GetVectorDistance(clientPos, wepPos) < 500.0) {
                            hasNearbyWeapon = true;
                            break;
                        }
                    }

                    if (!hasNearbyAmmo && !hasNearbyWeapon) {
                        // Emergency: refill clip + reserve ammo
                        // Cooldown: don't refill the same bot more than once per minute
                        if ((curTime - g_fLastEmergencyRefill[client]) < WEAPON_AMMO_REFILL_COOLDOWN) {
                            // Still on cooldown, skip
                        } else {
                            int maxClip = TaskWeapon_GetMaxClip(primary);
                            if (maxClip > 0) {
                                SetEntProp(primary, Prop_Send, "m_iClip1", maxClip);
                                // Also refill reserve ammo so bot doesn't immediately run out again
                                if (HasEntProp(primary, Prop_Send, "m_iPrimaryAmmoCount")) {
                                    SetEntProp(primary, Prop_Send, "m_iPrimaryAmmoCount", maxClip * 3);
                                }
                                g_fWeaponAmmoEmptyTime[client] = 0.0;
                                g_fLastEmergencyRefill[client] = curTime;
                                AI_DebugDecision(client, "Emergency ammo refill +%d clip +%d reserve", maxClip, maxClip * 3);
                            }
                        }
                    }
                }
            } else {
                // Has ammo — reset empty timer
                g_fWeaponAmmoEmptyTime[client] = 0.0;
            }
        }
    }

    // === Reload check ===
    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    if (IsValidEntity(activeWeapon) && HasEntProp(activeWeapon, Prop_Send, "m_iClip1")) {
        int clip = GetEntProp(activeWeapon, Prop_Send, "m_iClip1");
        
        if (clip == 0) {
            buttons |= IN_RELOAD;
            return false;
        }

        // Proactive reload: if not actively shooting and clip is low
        if (!g_BotState[client].isInCombat) {
            int maxClip = TaskWeapon_GetMaxClip(activeWeapon);
            if (maxClip > 0 && (float(clip) / float(maxClip)) < WEAPON_RELOAD_THRESHOLD) {
                buttons |= IN_RELOAD;
                return false;
            }
        }
    }

    // === Weapon switch based on range ===
    if (!g_BotState[client].isInCombat) return false;

    int target = g_BotState[client].targetEntity;
    if (target <= 0) return false;

    float targetPos[3];
    if (target <= MaxClients) {
        if (!IsClientInGame(target) || !IsPlayerAlive(target)) return false;
        GetClientAbsOrigin(target, targetPos);
    } else {
        if (!IsValidEntity(target)) return false;
        GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", targetPos);
    }

    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

    // Get primary weapon info
    int primary = GetPlayerWeaponSlot(client, 0);
    if (primary == -1 || !IsValidEntity(primary)) return false;

    char primaryClass[64];
    GetEntityClassname(primary, primaryClass, sizeof(primaryClass));
    bool primaryIsShotgun = TaskWeapon_IsShotgun(primaryClass);

    // Close range: prefer shotgun
    if (dist < WEAPON_CLOSE_RANGE && primaryIsShotgun) {
        if (activeWeapon != primary) {
            ClientCommand(client, "slot1");
            g_fWeaponLastSwitchTime[client] = GetGameTime();
        }
        return false;
    }

    // Far range: prefer rifle/sniper (non-shotgun)
    if (dist > WEAPON_FAR_RANGE && !primaryIsShotgun) {
        if (activeWeapon != primary) {
            ClientCommand(client, "slot1");
            g_fWeaponLastSwitchTime[client] = GetGameTime();
        }
        return false;
    }

    // If we have a secondary and primary is empty, switch to secondary
    // But first check if there's a nearby ammo pile — if so, let task_search guide us there
    if (IsValidEntity(activeWeapon) && HasEntProp(activeWeapon, Prop_Send, "m_iClip1")) {
        int clip = GetEntProp(activeWeapon, Prop_Send, "m_iClip1");
        if (clip == 0) {
            // Check for nearby ammo resupply before switching weapons
            // Use cached result to avoid FindEntityByClassname every frame
            float curTime2 = GetGameTime();
            bool hasNearbyAmmo = g_bAmmoNearCacheResult[client];
            if ((curTime2 - g_fAmmoNearCacheTime[client]) >= AMMO_NEAR_CACHE_INTERVAL) {
                hasNearbyAmmo = false;
                int ammoEnt = -1;
                safetyCount = 0;
                float clientPos[3];
                GetClientAbsOrigin(client, clientPos);
                while ((ammoEnt = FindEntityByClassname(ammoEnt, "weapon_ammo_spawn")) != -1) {
                    if (++safetyCount > 256) break;
                    if (!IsValidEntity(ammoEnt)) { ammoEnt++; continue; }
                    float ammoPos[3];
                    GetEntPropVector(ammoEnt, Prop_Data, "m_vecAbsOrigin", ammoPos);
                    if (GetVectorDistance(clientPos, ammoPos) < 500.0) {
                        hasNearbyAmmo = true;
                        break;
                    }
                }
                g_fAmmoNearCacheTime[client] = curTime2;
                g_bAmmoNearCacheResult[client] = hasNearbyAmmo;
            }
            if (!hasNearbyAmmo) {
                // No nearby ammo resupply, switch to secondary weapon
                int secondary = GetPlayerWeaponSlot(client, 1);
                if (secondary != -1 && IsValidEntity(secondary)) {
                    ClientCommand(client, "slot2");
                    g_fWeaponLastSwitchTime[client] = GetGameTime();
                }
            }
        }
    }

    return false;
}

/**
 * Reset weapon state.
 */
void TaskWeapon_Reset(int client) {
    g_fWeaponLastSwitchTime[client] = 0.0;
    g_fWeaponAmmoEmptyTime[client] = 0.0;
    g_fWeaponLastAmmoCheck[client] = 0.0;
}

// ============================================================
// Internal helpers
// ============================================================

/**
 * Check if a weapon switch would improve combat effectiveness.
 */
static bool TaskWeapon_ShouldSwitch(int client) {
    int target = g_BotState[client].targetEntity;
    if (target <= 0) return false;

    float targetPos[3];
    if (target <= MaxClients) {
        if (!IsClientInGame(target) || !IsPlayerAlive(target)) return false;
        GetClientAbsOrigin(target, targetPos);
    } else {
        if (!IsValidEntity(target)) return false;
        GetEntPropVector(target, Prop_Data, "m_vecAbsOrigin", targetPos);
    }

    float dist = AI_GetVectorDistance(g_BotState[client].absOrigin, targetPos);

    int primary = GetPlayerWeaponSlot(client, 0);
    if (primary == -1 || !IsValidEntity(primary)) return false;

    char primaryClass[64];
    GetEntityClassname(primary, primaryClass, sizeof(primaryClass));
    bool isShotgun = TaskWeapon_IsShotgun(primaryClass);

    int activeWeapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");

    // Close range + have shotgun + not using it
    if (dist < WEAPON_CLOSE_RANGE && isShotgun && activeWeapon != primary) {
        return true;
    }

    // Far range + have rifle + not using it
    if (dist > WEAPON_FAR_RANGE && !isShotgun && activeWeapon != primary) {
        return true;
    }

    return false;
}

/**
 * Check if a weapon classname is a shotgun.
 */
static bool TaskWeapon_IsShotgun(const char[] classname) {
    if (StrEqual(classname, "weapon_autoshotgun")) return true;
    if (StrEqual(classname, "weapon_shotgun_spas")) return true;
    if (StrEqual(classname, "weapon_pumpshotgun")) return true;
    if (StrEqual(classname, "weapon_shotgun_chrome")) return true;
    return false;
}

/**
 * Get the maximum clip size of a weapon.
 */
static int TaskWeapon_GetMaxClip(int weapon) {
    if (!IsValidEntity(weapon)) return 0;
    if (!HasEntProp(weapon, Prop_Send, "m_iClip1")) return 0;

    // Use Prop_Send for m_iPrimaryAmmoType, then lookup max clip
    // Simplified: check common weapon clip sizes
    char classname[64];
    GetEntityClassname(weapon, classname, sizeof(classname));

    if (StrEqual(classname, "weapon_autoshotgun") || StrEqual(classname, "weapon_shotgun_spas"))
        return 10;
    if (StrEqual(classname, "weapon_pumpshotgun") || StrEqual(classname, "weapon_shotgun_chrome"))
        return 8;
    if (StrEqual(classname, "weapon_smg") || StrEqual(classname, "weapon_smg_silenced") || StrEqual(classname, "weapon_smg_mp5"))
        return 50;
    if (StrEqual(classname, "weapon_rifle") || StrEqual(classname, "weapon_rifle_desert") || StrEqual(classname, "weapon_rifle_ak47"))
        return 50;
    if (StrEqual(classname, "weapon_rifle_sg552"))
        return 50;
    if (StrEqual(classname, "weapon_hunting_rifle"))
        return 15;
    if (StrEqual(classname, "weapon_sniper_military") || StrEqual(classname, "weapon_sniper_scout") || StrEqual(classname, "weapon_sniper_awp"))
        return 30;
    if (StrEqual(classname, "weapon_rifle_m60"))
        return 150;
    if (StrEqual(classname, "weapon_grenade_launcher"))
        return 1;
    if (StrEqual(classname, "weapon_pistol"))
        return 15;
    if (StrEqual(classname, "weapon_pistol_magnum"))
        return 8;

    return 30; // Default fallback
}
