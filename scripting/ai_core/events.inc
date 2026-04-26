/**
 * L4D2 AI Bot - Event-Driven State Updates
 * 
 * Hooks all critical game events and updates BotState accordingly.
 * This ensures BotState always reflects the latest game state without polling.
 *
 * L4D2 Event Reference: https://wiki.alliedmods.net/Left_4_Dead_2/Events
 */

#if defined _ai_events_included
 #endinput
#endif
#define _ai_events_included

// Store death positions for defib targeting
// Dead players may have invalid GetClientAbsOrigin (spectator position)
float g_fDeathPosition[MAXPLAYERS + 1][3];
bool g_bHasDeathPosition[MAXPLAYERS + 1];

/**
 * Register all event hooks. Called from OnPluginStart.
 */
void AIEvents_Init() {
    // === Special Infected Control Events ===
    HookEvent("tongue_grab",           Event_TongueGrab);
    HookEvent("tongue_release",        Event_TongueRelease);
    HookEvent("lunge_pounce",          Event_LungePounce);
    HookEvent("pounce_stopped",        Event_PounceStopped);
    HookEvent("jockey_ride",           Event_JockeyRide);
    HookEvent("jockey_ride_end",       Event_JockeyRideEnd);
    HookEvent("charger_carry_start",   Event_ChargerCarryStart);
    HookEvent("charger_carry_end",     Event_ChargerCarryEnd);
    HookEvent("charger_pummel_start",  Event_ChargerPummelStart);
    HookEvent("charger_pummel_end",    Event_ChargerPummelEnd);

    // === Heal Events ===
    HookEvent("heal_begin",            Event_HealBegin);
    HookEvent("heal_end",              Event_HealEnd);

    // === Revive Events ===
    HookEvent("revive_begin",          Event_ReviveBegin);
    HookEvent("revive_success",        Event_ReviveSuccess);

    // === Death/Incap Events ===
    HookEvent("player_death",          Event_PlayerDeath);
    HookEvent("player_incapacitated",  Event_PlayerIncapacitated);

    // === Hurt Events ===
    HookEvent("player_hurt",           Event_PlayerHurt);

    // === Respawn Events ===
    HookEvent("defibrillator_used",    Event_DefibUsed);
}

// ============================================================
// Special Infected Control: Smoker
// tongue_grab: userid=smoker, victim=survivor
// ============================================================
void Event_TongueGrab(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    int attacker = GetClientOfUserId(event.GetInt("userid"));
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker))
        return;
    g_BotState[victim].controlledByType = 1;  // Smoker
    g_BotState[victim].controllerEntity = attacker;
}

void Event_TongueRelease(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    g_BotState[victim].controlledByType = 0;
    g_BotState[victim].controllerEntity = -1;
}

// ============================================================
// Special Infected Control: Hunter
// lunge_pounce: userid=hunter, victim=survivor
// ============================================================
void Event_LungePounce(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    int attacker = GetClientOfUserId(event.GetInt("userid"));
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker))
        return;
    g_BotState[victim].controlledByType = 2;  // Hunter
    g_BotState[victim].controllerEntity = attacker;
}

void Event_PounceStopped(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    g_BotState[victim].controlledByType = 0;
    g_BotState[victim].controllerEntity = -1;
}

// ============================================================
// Special Infected Control: Jockey
// jockey_ride: userid=jockey, victim=survivor
// ============================================================
void Event_JockeyRide(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    int attacker = GetClientOfUserId(event.GetInt("userid"));
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker))
        return;
    g_BotState[victim].controlledByType = 3;  // Jockey
    g_BotState[victim].controllerEntity = attacker;
}

void Event_JockeyRideEnd(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    g_BotState[victim].controlledByType = 0;
    g_BotState[victim].controllerEntity = -1;
}

// ============================================================
// Special Infected Control: Charger
// charger_carry_start: userid=charger, victim=survivor
// ============================================================
void Event_ChargerCarryStart(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    int attacker = GetClientOfUserId(event.GetInt("userid"));
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker))
        return;
    g_BotState[victim].controlledByType = 4;  // Charger carry
    g_BotState[victim].controllerEntity = attacker;
}

void Event_ChargerCarryEnd(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    // Carry ended — might transition to pummel or release
    if (g_BotState[victim].controlledByType == 4) {
        g_BotState[victim].controlledByType = 0;
        g_BotState[victim].controllerEntity = -1;
    }
}

void Event_ChargerPummelStart(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    int attacker = GetClientOfUserId(event.GetInt("userid"));
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker))
        return;
    g_BotState[victim].controlledByType = 5;  // Charger pummel
    g_BotState[victim].controllerEntity = attacker;
}

void Event_ChargerPummelEnd(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("victim"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    g_BotState[victim].controlledByType = 0;
    g_BotState[victim].controllerEntity = -1;
}

// ============================================================
// Heal Events
// heal_begin: userid=healer(survivor), subject=patient(survivor)
// heal_end: same fields
// ============================================================
void Event_HealBegin(Event event, const char[] name, bool dontBroadcast) {
    int healer = GetClientOfUserId(event.GetInt("userid"));
    if (healer < 1 || healer > MaxClients || !IsClientInGame(healer))
        return;
    int subject = GetClientOfUserId(event.GetInt("subject"));
    g_BotState[healer].isHealing = true;
    g_BotState[healer].isFrozenForHeal = true;
    // Only store healTarget if subject is valid
    if (subject > 0 && subject <= MaxClients && IsClientInGame(subject)) {
        g_BotState[healer].healTarget = subject;
        if (subject != healer) {
            g_BotState[subject].isFrozenForHeal = true;
        }
    } else {
        g_BotState[healer].healTarget = -1;
    }
}

void Event_HealEnd(Event event, const char[] name, bool dontBroadcast) {
    int healer = GetClientOfUserId(event.GetInt("userid"));
    if (healer < 1 || healer > MaxClients || !IsClientInGame(healer))
        return;
    g_BotState[healer].isHealing = false;
    g_BotState[healer].healTarget = -1;
    g_BotState[healer].isFrozenForHeal = false;
    int subject = GetClientOfUserId(event.GetInt("subject"));
    if (subject > 0 && subject <= MaxClients && IsClientInGame(subject) && subject != healer) {
        g_BotState[subject].isFrozenForHeal = false;
    }
}

// ============================================================
// Revive Events
// revive_begin: userid=reviver, subject=victim(incapped)
// revive_success: userid=reviver, subject=victim
// ============================================================
void Event_ReviveBegin(Event event, const char[] name, bool dontBroadcast) {
    int reviver = GetClientOfUserId(event.GetInt("userid"));
    if (reviver < 1 || reviver > MaxClients || !IsClientInGame(reviver))
        return;
    int subject = GetClientOfUserId(event.GetInt("subject"));
    g_BotState[reviver].isRescuing = true;
    // Only store rescueTarget if subject is valid
    if (subject > 0 && subject <= MaxClients && IsClientInGame(subject)) {
        g_BotState[reviver].rescueTarget = subject;
    } else {
        g_BotState[reviver].rescueTarget = -1;
    }
}

void Event_ReviveSuccess(Event event, const char[] name, bool dontBroadcast) {
    int reviver = GetClientOfUserId(event.GetInt("userid"));
    if (reviver < 1 || reviver > MaxClients || !IsClientInGame(reviver))
        return;
    g_BotState[reviver].isRescuing = false;
    g_BotState[reviver].rescueTarget = -1;
    int subject = GetClientOfUserId(event.GetInt("subject"));
    if (subject > 0 && subject <= MaxClients && IsClientInGame(subject)) {
        g_BotState[subject].isIncapacitated = false;
    }
}

// ============================================================
// Death & Incap Events
// player_death: userid=victim
// player_incapacitated: userid=victim
// ============================================================
void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;

    // Store death position — prefer last known ground position over
    // GetClientAbsOrigin (which returns spectator position after death)
    float curOrigin[3];
    GetClientAbsOrigin(victim, curOrigin);

    // Check if current origin is valid (not floating in the sky = spectator)
    bool curOriginValid = (curOrigin[2] > -4000.0 && curOrigin[2] < 3000.0);

    if (curOriginValid && (g_fLastGroundPosition[victim][0] == 0.0 &&
                           g_fLastGroundPosition[victim][1] == 0.0 &&
                           g_fLastGroundPosition[victim][2] == 0.0)) {
        // No cached position — use current origin as fallback
        g_fDeathPosition[victim] = curOrigin;
    } else {
        // Use last known ground position (most reliable)
        g_fDeathPosition[victim] = g_fLastGroundPosition[victim];
    }
    g_bHasDeathPosition[victim] = true;

    // CRITICAL: Clear FL_FROZEN if bot was healing when killed
    if (g_BotState[victim].isFrozenForHeal) {
        int flags = GetEntityFlags(victim);
        if (((flags & FL_FROZEN) != 0)) {
            SetEntityFlags(victim, flags & ~FL_FROZEN);
        }
        g_BotState[victim].isFrozenForHeal = false;
    }
    g_BotState[victim].isHealing = false;
    g_BotState[victim].healTarget = -1;
    g_BotState[victim].healEndTime = 0.0;

    g_BotState[victim].isAlive = false;
    g_BotState[victim].isActive = false;
    g_BotState[victim].controlledByType = 0;
    g_BotState[victim].controllerEntity = -1;
}

void Event_PlayerIncapacitated(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;

    // Cache ground position NOW — player is still on the ground, not spectating
    GetClientAbsOrigin(victim, g_fLastGroundPosition[victim]);

    // CRITICAL: Clear FL_FROZEN if bot was healing when incapacitated
    if (g_BotState[victim].isFrozenForHeal) {
        int flags = GetEntityFlags(victim);
        if (((flags & FL_FROZEN) != 0)) {
            SetEntityFlags(victim, flags & ~FL_FROZEN);
        }
        g_BotState[victim].isFrozenForHeal = false;
    }
    g_BotState[victim].isHealing = false;
    g_BotState[victim].healTarget = -1;
    g_BotState[victim].healEndTime = 0.0;

    g_BotState[victim].isIncapacitated = true;
}

// ============================================================
// Hurt Events
// player_hurt: userid=victim, health=remaining_health
// ============================================================
void Event_PlayerHurt(Event event, const char[] name, bool dontBroadcast) {
    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (victim < 1 || victim > MaxClients || !IsClientInGame(victim))
        return;
    g_BotState[victim].health = event.GetInt("health");

    // Update last ground position while alive (for death position fallback)
    if (IsPlayerAlive(victim)) {
        GetClientAbsOrigin(victim, g_fLastGroundPosition[victim]);
    }
}

// ============================================================
// Respawn Events
// defibrillator_used: subject=revived player
// ============================================================
void Event_DefibUsed(Event event, const char[] name, bool dontBroadcast) {
    int subject = GetClientOfUserId(event.GetInt("subject"));
    if (subject > 0 && subject <= MaxClients) {
        g_bHasDeathPosition[subject] = false;
    }
}
