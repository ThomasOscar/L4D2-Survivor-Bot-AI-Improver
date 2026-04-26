/**
 * AI Movement Utilities
 * 
 * Bot movement control helpers for OnPlayerRunCmd-based movement.
 * No global variable dependencies — all data passed via parameters.
 *
 * NOTE: In SourceMod, bot movement is typically controlled by modifying the
 * vel[3] parameter in OnPlayerRunCmd. vel[0] = forward/back, vel[1] = left/right.
 * These helpers compute the appropriate vel values to drive a bot toward a target.
 */

#if defined _move_utils_included
    #endinput
#endif
#define _move_utils_included

#include <sourcemod>
#include <sdktools>

// Default movement speed for bots
#define AI_MOVE_SPEED 450.0

// ============================================================================
// Movement Control via vel (OnPlayerRunCmd)
// ============================================================================

/**
 * Compute the vel[3] values to move a bot toward a target position.
 * Call this inside OnPlayerRunCmd to drive the bot's movement.
 *
 * @param client        Client index.
 * @param targetPos     Destination world position.
 * @param clientOrigin  Client's current world position.
 * @param clientEyeAng  Client's current eye angles.
 * @param vel           Output velocity array to modify [forward, side, up].
 * @param moveSpeed     Movement speed magnitude (default AI_MOVE_SPEED).
 */
stock void AI_ComputeMoveToward(int client, const float targetPos[3], const float clientOrigin[3], 
                                 const float clientEyeAng[3], float vel[3], float moveSpeed = AI_MOVE_SPEED)
{
    // Direction from client to target in world space
    float direction[3];
    direction[0] = targetPos[0] - clientOrigin[0];
    direction[1] = targetPos[1] - clientOrigin[1];
    direction[2] = 0.0; // Ignore vertical for ground movement

    float dist = SquareRoot(direction[0] * direction[0] + direction[1] * direction[1]);
    if (dist < 1.0)
    {
        vel[0] = 0.0;
        vel[1] = 0.0;
        vel[2] = 0.0;
        return;
    }

    // Normalize direction
    direction[0] /= dist;
    direction[1] /= dist;

    // Get the client's forward and right vectors from eye angles
    float fwd[3], rgt[3];
    float angles[3];
    angles[0] = 0.0; // Ignore pitch for ground movement
    angles[1] = clientEyeAng[1];
    angles[2] = 0.0;
    GetAngleVectors(angles, fwd, rgt, NULL_VECTOR);

    // Project desired direction onto client's local axes
    // vel[0] = forward component (positive = move forward)
    // vel[1] = right component (positive = move right)
    vel[0] = (direction[0] * fwd[0] + direction[1] * fwd[1]) * moveSpeed;
    vel[1] = (direction[0] * rgt[0] + direction[1] * rgt[1]) * moveSpeed;
    vel[2] = 0.0;
}

/**
 * Clear movement velocity (stop the bot).
 *
 * @param vel   Velocity array to zero out.
 */
stock void AI_ClearMoveVelocity(float vel[3])
{
    vel[0] = 0.0;
    vel[1] = 0.0;
    vel[2] = 0.0;
}

/**
 * Make a bot press the jump button.
 *
 * @param buttons   Button bitmask to modify (passed by reference).
 */
stock void AI_PressJumpButton(int &buttons)
{
    buttons |= IN_JUMP;
}

/**
 * Make a bot press the crouch button.
 *
 * @param buttons   Button bitmask to modify (passed by reference).
 */
stock void AI_PressCrouchButton(int &buttons)
{
    buttons |= IN_DUCK;
}

// ============================================================================
// Stuck Detection
// ============================================================================

/**
 * Check if a bot appears to be stuck by comparing current position to last known position.
 * 
 * Usage pattern:
 *   - Store the bot's position each tick/interval.
 *   - Call this function to compare current vs last stored position.
 *   - If returns true for several consecutive checks, the bot is likely stuck.
 *
 * @param client            Client index (unused, reserved for future use).
 * @param currentPos        Bot's current position.
 * @param lastPos           Bot's position from the previous check.
 * @param stuckThreshold    Minimum movement distance to be considered "not stuck" (squared).
 *                          Default 100.0 means ~10 units of movement.
 * @return                  True if the bot hasn't moved beyond the threshold.
 */
stock bool AI_IsPlayerStuck(int client, const float currentPos[3], const float lastPos[3], float stuckThreshold = 100.0)
{
    float dx = currentPos[0] - lastPos[0];
    float dy = currentPos[1] - lastPos[1];
    float dz = currentPos[2] - lastPos[2];
    float distSq = dx * dx + dy * dy + dz * dz;
    return (distSq < stuckThreshold);
}

/**
 * Get a bot's current movement speed (ground speed, ignoring vertical).
 *
 * @param client    Client index.
 * @return          Current ground speed in units/second, or 0.0 if invalid.
 */
stock float AI_GetClientSpeed(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client))
        return 0.0;

    float velocity[3];
    GetEntPropVector(client, Prop_Data, "m_vecVelocity", velocity);
    // Ground speed (ignore Z)
    return SquareRoot(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
}

/**
 * Get a bot's full 3D speed (including vertical component).
 *
 * @param client    Client index.
 * @return          Current 3D speed in units/second, or 0.0 if invalid.
 */
stock float AI_GetClientSpeed3D(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client))
        return 0.0;

    float velocity[3];
    GetEntPropVector(client, Prop_Data, "m_vecVelocity", velocity);
    return GetVectorLength(velocity);
}

/**
 * Get a bot's maximum configured movement speed.
 *
 * @param client    Client index.
 * @return          Maximum speed, or 0.0 if invalid.
 */
stock float AI_GetClientMaxSpeed(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client))
        return 0.0;
    return GetEntPropFloat(client, Prop_Data, "m_flMaxspeed");
}

// ============================================================================
// Movement Direction Helpers
// ============================================================================

/**
 * Compute vel to make a bot strafe (move sideways) relative to a threat.
 * Useful for dodging or circling.
 *
 * @param clientEyeAng     Client's current eye angles.
 * @param vel              Output velocity array to modify.
 * @param strafeRight      True = strafe right, false = strafe left.
 * @param moveSpeed        Speed magnitude (default AI_MOVE_SPEED).
 */
stock void AI_ComputeStrafe(const float clientEyeAng[3], float vel[3], bool strafeRight, float moveSpeed = AI_MOVE_SPEED)
{
    vel[0] = 0.0;
    vel[1] = strafeRight ? moveSpeed : -moveSpeed;
    vel[2] = 0.0;
}

/**
 * Compute vel to make a bot move backward (away from current facing direction).
 *
 * @param vel           Output velocity array to modify.
 * @param moveSpeed     Speed magnitude (default AI_MOVE_SPEED).
 */
stock void AI_ComputeMoveBackward(float vel[3], float moveSpeed = AI_MOVE_SPEED)
{
    vel[0] = -moveSpeed;
    vel[1] = 0.0;
    vel[2] = 0.0;
}

/**
 * Compute vel to make a bot move forward.
 *
 * @param vel           Output velocity array to modify.
 * @param moveSpeed     Speed magnitude (default AI_MOVE_SPEED).
 */
stock void AI_ComputeMoveForward(float vel[3], float moveSpeed = AI_MOVE_SPEED)
{
    vel[0] = moveSpeed;
    vel[1] = 0.0;
    vel[2] = 0.0;
}

/**
 * Compute vel to make a bot move away from a position.
 * Calculates the direction from pos to client, converts to forward/side movement.
 *
 * @param client        Bot client index.
 * @param fromPos       Position to move away from.
 * @param clientOrigin  Bot's current absOrigin.
 * @param clientAngles  Bot's current eye angles (modified to face away).
 * @param vel           Output velocity array.
 * @param moveSpeed     Speed magnitude.
 */
stock void AI_ComputeMoveAway(int client, const float fromPos[3], const float clientOrigin[3],
                               float clientAngles[3], float vel[3], float moveSpeed = AI_MOVE_SPEED)
{
    float awayDir[3];
    SubtractVectors(clientOrigin, fromPos, awayDir);
    awayDir[2] = 0.0;
    float len = GetVectorLength(awayDir);
    if (len < 1.0) {
        // We're right on top of the position, pick a direction
        awayDir[0] = 1.0; awayDir[1] = 0.0; awayDir[2] = 0.0;
    } else {
        NormalizeVector(awayDir, awayDir);
    }

    // Face away from the position
    float awayAngles[3];
    GetVectorAngles(awayDir, awayAngles);
    clientAngles[0] = 0.0;
    clientAngles[1] = awayAngles[1];
    clientAngles[2] = 0.0;

    // Move forward (which is now away from the position)
    vel[0] = moveSpeed;
    vel[1] = 0.0;
    vel[2] = 0.0;
}

// ============================================================================
// Safe Teleport Utilities
// ============================================================================

#define TELEPORT_MAX_ATTEMPTS    2       // Max teleport attempts before giving up
#define TELEPORT_MIN_INTERVAL    8.0     // Min seconds between teleports
#define TELEPORT_HEIGHT_BELOW    -4000.0 // Z below this = underground/out of map
#define TELEPORT_HEIGHT_ABOVE    5000.0  // Z above this = out of map (skybox)
#define TELEPORT_TRACE_HEIGHT    500.0   // Height to trace down for ground check
#define TELEPORT_GROUND_CLEAR    2.0     // Min clearance above ground at destination
#define TELEPORT_MIN_CLEARANCE   70.0    // Min vertical clearance (player height ~72 units)
#define TELEPORT_MAX_OFFSET      60.0    // Max random offset from target (small to avoid walls)
#define TELEPORT_STEP_DISTANCE   200.0   // Max distance to teleport forward (unstick step, NOT to target)
#define TELEPORT_STUCK_CHECK_INTERVAL 2.0  // Seconds between stuck position checks
#define TELEPORT_STUCK_THRESHOLD  50.0   // Squared distance threshold for "not moving"

// Per-bot stuck tracking for teleport
static float g_fTeleportLastPos[MAXPLAYERS + 1][3];
static float g_fTeleportLastCheckTime[MAXPLAYERS + 1];
static int g_iTeleportStuckCount[MAXPLAYERS + 1];

/**
 * Thoroughly validate a teleport position.
 * Checks: Z bounds, ground beneath, clearance above, not inside solid.
 *
 * @param pos     Position to validate (will be adjusted downward to ground).
 * @param outPos  Output: adjusted position on the ground.
 * @return        True if position is safe for teleport.
 */
stock bool AI_ValidateTeleportPosition(const float pos[3], float outPos[3])
{
    // 1. Out-of-bounds Z check
    if (pos[2] < TELEPORT_HEIGHT_BELOW || pos[2] > TELEPORT_HEIGHT_ABOVE)
        return false;

    // 2. Trace down from well above the position to find ground
    float start[3], end[3];
    start[0] = pos[0];
    start[1] = pos[1];
    start[2] = pos[2] + TELEPORT_TRACE_HEIGHT;
    end[0] = pos[0];
    end[1] = pos[1];
    end[2] = pos[2] - 200.0;

    Handle trace = TR_TraceRayFilterEx(start, end, MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint, AI_TraceFilter_NoPlayers, 0);
    bool hitGround = TR_DidHit(trace);

    if (!hitGround) {
        delete trace;
        return false;  // No ground found below
    }

    // Get the ground position
    float groundPos[3];
    TR_GetEndPosition(groundPos, trace);
    delete trace;

    // 3. Check that ground is at a reasonable distance below the starting point
    // (not too far below = we're not teleporting to a completely different floor)
    float groundDist = start[2] - groundPos[2];
    if (groundDist > TELEPORT_TRACE_HEIGHT + 200.0) {
        return false;  // Ground is way too far below
    }

    // 4. Set output position slightly above ground
    outPos[0] = groundPos[0];
    outPos[1] = groundPos[1];
    outPos[2] = groundPos[2] + TELEPORT_GROUND_CLEAR;

    // 5. Check vertical clearance above the ground position (player needs ~72 units)
    float ceilingStart[3], ceilingEnd[3];
    ceilingStart[0] = outPos[0];
    ceilingStart[1] = outPos[1];
    ceilingStart[2] = outPos[2];
    ceilingEnd[0] = outPos[0];
    ceilingEnd[1] = outPos[1];
    ceilingEnd[2] = outPos[2] + TELEPORT_MIN_CLEARANCE;

    Handle ceilTrace = TR_TraceRayFilterEx(ceilingStart, ceilingEnd, MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint, AI_TraceFilter_NoPlayers, 0);
    bool hitCeiling = TR_DidHit(ceilTrace);
    delete ceilTrace;

    if (hitCeiling) {
        return false;  // Not enough headroom — would be stuck in ceiling
    }

    // 6. Check that the position is not inside a wall by tracing a short horizontal line
    // Trace from the position outward in a direction; if it starts in solid, it'll be 0 length
    float wallCheck[3];
    wallCheck[0] = outPos[0] + 20.0;
    wallCheck[1] = outPos[1];
    wallCheck[2] = outPos[2] + 30.0;  // Check at torso height

    Handle wallTrace = TR_TraceRayFilterEx(outPos, wallCheck, MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint, AI_TraceFilter_NoPlayers, 0);
    float wallFrac = TR_GetFraction(wallTrace);
    delete wallTrace;

    // If the trace didn't go anywhere near 20 units, we're probably inside a wall
    if (wallFrac < 0.3) {
        return false;
    }

    return true;
}

/**
 * Trace filter that ignores all player entities.
 */
stock bool AI_TraceFilter_NoPlayers(int entity, int contentsMask, int client)
{
    return (entity > MaxClients || entity <= 0);
}

/**
 * Check if a position is likely valid (simpler check for non-teleport uses).
 * Uses Z-bounds + ground check.
 */
stock bool AI_IsValidPosition(const float pos[3])
{
    float outPos[3];
    return AI_ValidateTeleportPosition(pos, outPos);
}

/**
 * Try to safely teleport a bot near a target position.
 * Uses thorough position validation with ground snapping.
 * If the bot is still stuck after max attempts, returns false to signal abandon.
 *
 * @param client          Bot client index.
 * @param targetPos       The target position to teleport near.
 * @param dist            Current distance to target (must exceed threshold).
 * @param elapsed         Seconds since navigation started.
 * @param stuckThreshold  Min distance to target to consider teleport.
 * @param timeThreshold   Min seconds elapsed to consider teleport.
 * @return                true = teleported successfully, false = not teleported (or should abandon).
 */
stock bool AI_TryStuckTeleport(int client, const float targetPos[3], float dist, float elapsed,
                                float stuckThreshold = 300.0, float timeThreshold = 8.0)
{
    // Not stuck enough yet (distance + time gate)
    if (dist <= stuckThreshold || elapsed <= timeThreshold)
        return false;

    // Don't teleport if bot hasn't been in this task long enough
    if (g_BotState[client].taskStartTime <= 0.0)
        return false;

    // === Actual stuck detection: check if bot has moved recently ===
    float curTime = GetGameTime();
    if ((curTime - g_fTeleportLastCheckTime[client]) >= TELEPORT_STUCK_CHECK_INTERVAL) {
        bool isStuck = AI_IsPlayerStuck(client, g_BotState[client].absOrigin,
                                         g_fTeleportLastPos[client], TELEPORT_STUCK_THRESHOLD);

        if (isStuck) {
            g_iTeleportStuckCount[client]++;
        } else {
            g_iTeleportStuckCount[client] = 0;  // Reset — bot is moving fine
        }

        // Store current position for next check
        g_fTeleportLastPos[client][0] = g_BotState[client].absOrigin[0];
        g_fTeleportLastPos[client][1] = g_BotState[client].absOrigin[1];
        g_fTeleportLastPos[client][2] = g_BotState[client].absOrigin[2];
        g_fTeleportLastCheckTime[client] = curTime;
    }

    // Bot must be actually stuck (not moving) for at least 2 consecutive checks before teleport
    if (g_iTeleportStuckCount[client] < 2) {
        return false;  // Bot is still moving — don't teleport
    }

    // Check cooldown
    if (g_BotState[client].lastTeleportTime > 0.0 &&
        (curTime - g_BotState[client].lastTeleportTime) < TELEPORT_MIN_INTERVAL)
        return false;

    // Already tried too many times — abandon
    if (g_BotState[client].teleportAttempts >= TELEPORT_MAX_ATTEMPTS)
        return false;

    // Generate candidate position: step forward toward target, NOT teleport to target
    // This prevents cross-map teleports — we only move ~200 units in the target direction
    float botPos[3];
    botPos = g_BotState[client].absOrigin;

    // Calculate direction from bot to target
    float dir[3];
    dir[0] = targetPos[0] - botPos[0];
    dir[1] = targetPos[1] - botPos[1];
    dir[2] = 0.0;  // Keep horizontal

    float dirLen = SquareRoot(dir[0] * dir[0] + dir[1] * dir[1]);
    if (dirLen < 1.0) {
        g_BotState[client].teleportAttempts++;
        g_BotState[client].lastTeleportTime = curTime;
        return false;  // Can't determine direction
    }

    // Normalize and scale to step distance
    dir[0] = (dir[0] / dirLen) * TELEPORT_STEP_DISTANCE;
    dir[1] = (dir[1] / dirLen) * TELEPORT_STEP_DISTANCE;

    // Try candidate positions along the direction with random offsets
    float telePos[3];
    bool found = false;

    for (int attempt = 0; attempt < 5; attempt++) {
        float candidate[3];
        float randomOffset = (attempt == 0) ? 30.0 : 30.0 + attempt * 15.0;
        candidate[0] = botPos[0] + dir[0] + GetRandomFloat(-randomOffset, randomOffset);
        candidate[1] = botPos[1] + dir[1] + GetRandomFloat(-randomOffset, randomOffset);
        candidate[2] = botPos[2] + 50.0;  // Start search from above current position

        // Verify candidate is not too far from current position (safety check)
        float candDist = AI_GetVectorDistance(botPos, candidate);
        if (candDist > TELEPORT_STEP_DISTANCE + 100.0) {
            continue;  // Too far from current position — skip
        }

        if (AI_ValidateTeleportPosition(candidate, telePos)) {
            // Final safety: verify teleported position is within reasonable range of bot
            float teleDist = AI_GetVectorDistance(botPos, telePos);
            if (teleDist > TELEPORT_STEP_DISTANCE + 100.0) {
                continue;  // Ground snapping moved too far — skip
            }
            found = true;
            break;
        }
    }

    if (!found) {
        g_BotState[client].teleportAttempts++;
        g_BotState[client].lastTeleportTime = curTime;
        return false;
    }

    // Execute teleport
    TeleportEntity(client, telePos, NULL_VECTOR, NULL_VECTOR);
    g_BotState[client].teleportAttempts++;
    g_BotState[client].lastTeleportTime = curTime;

    return true;
}

/**
 * Reset teleport tracking state. Call when a navigation task starts or resets.
 */
stock void AI_ResetTeleportState(int client)
{
    g_BotState[client].teleportAttempts = 0;
    g_BotState[client].lastTeleportTime = 0.0;
    g_iTeleportStuckCount[client] = 0;
    g_fTeleportLastCheckTime[client] = 0.0;
}

/**
 * Check if the bot has exhausted all teleport attempts and should abandon the task.
 */
stock bool AI_ShouldAbandonNavigation(int client)
{
    return g_BotState[client].teleportAttempts >= TELEPORT_MAX_ATTEMPTS;
}
