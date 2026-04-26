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

// Per-bot weapon state
static float g_fWeaponLastSwitchTime[MAXPLAYERS + 1];

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

/**
 * Execute weapon management: switch weapons, reload.
 * @return false (never locks).
 */
bool TaskWeapon_Update(int client, int &buttons, float vel[3]) {
    #pragma unused vel

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
            bool hasNearbyAmmo = false;
            int ammoEnt = -1;
            float clientPos[3];
            GetClientAbsOrigin(client, clientPos);
            while ((ammoEnt = FindEntityByClassname(ammoEnt, "weapon_ammo_spawn")) != -1) {
                if (!IsValidEntity(ammoEnt)) continue;
                float ammoPos[3];
                GetEntPropVector(ammoEnt, Prop_Data, "m_vecAbsOrigin", ammoPos);
                if (GetVectorDistance(clientPos, ammoPos) < 500.0) {
                    hasNearbyAmmo = true;
                    break;
                }
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
    if (!HasEntProp(weapon, Prop_Data, "m_iClip1")) return 0;

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
