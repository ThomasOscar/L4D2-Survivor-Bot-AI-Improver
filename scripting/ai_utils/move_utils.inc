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

// ============================================================================
// Safe Teleport Utilities
// ============================================================================

#define TELEPORT_MAX_ATTEMPTS    2       // Max teleport attempts before giving up
#define TELEPORT_MIN_INTERVAL    8.0     // Min seconds between teleports
#define TELEPORT_HEIGHT_BELOW    -4000.0 // Z below this = underground/out of map
#define TELEPORT_HEIGHT_ABOVE    5000.0  // Z above this = out of map (skybox)
#define TELEPORT_TRACE_HEIGHT    300.0   // Height to trace down for ground check
#define TELEPORT_GROUND_CLEAR    2.0     // Min clearance above ground at destination

/**
 * Check if a position is likely valid (not underground, not in skybox, has ground beneath).
 * Uses simple Z-bounds check + traceline to verify ground exists below the point.
 *
 * @param pos     Position to validate.
 * @return        True if position appears valid and safe.
 */
stock bool AI_IsValidPosition(const float pos[3])
{
    // Out-of-bounds Z check
    if (pos[2] < TELEPORT_HEIGHT_BELOW || pos[2] > TELEPORT_HEIGHT_ABOVE)
        return false;

    // Trace down from above the position to check for ground
    float start[3], end[3];
    start[0] = pos[0];
    start[1] = pos[1];
    start[2] = pos[2] + TELEPORT_TRACE_HEIGHT;
    end[0] = pos[0];
    end[1] = pos[1];
    end[2] = pos[2] - 100.0;

    Handle trace = TR_TraceRayFilterEx(start, end, MASK_PLAYERSOLID_BRUSHONLY, RayType_EndPoint, AI_TraceFilter_NoPlayers, 0);
    bool hitGround = TR_DidHit(trace);
    delete trace;

    return hitGround;
}

/**
 * Trace filter that ignores all player entities.
 */
stock bool AI_TraceFilter_NoPlayers(int entity, int contentsMask, int client)
{
    return (entity > MaxClients || entity <= 0);
}

/**
 * Try to safely teleport a bot near a target position.
 * Checks: position validity, max attempts, cooldown.
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
    // Not stuck enough yet
    if (dist <= stuckThreshold || elapsed <= timeThreshold)
        return false;

    // Check cooldown
    float curTime = GetGameTime();
    if (g_BotState[client].lastTeleportTime > 0.0 &&
        (curTime - g_BotState[client].lastTeleportTime) < TELEPORT_MIN_INTERVAL)
        return false;

    // Already tried too many times — abandon
    if (g_BotState[client].teleportAttempts >= TELEPORT_MAX_ATTEMPTS)
        return false;

    // Generate candidate position near target with random offset
    float telePos[3];
    telePos[0] = targetPos[0] + GetRandomFloat(-80.0, 80.0);
    telePos[1] = targetPos[1] + GetRandomFloat(-80.0, 80.0);
    telePos[2] = targetPos[2] + 10.0;

    // Validate destination
    if (!AI_IsValidPosition(telePos)) {
        // Try a few more candidates
        bool found = false;
        for (int attempt = 0; attempt < 3; attempt++) {
            telePos[0] = targetPos[0] + GetRandomFloat(-120.0, 120.0);
            telePos[1] = targetPos[1] + GetRandomFloat(-120.0, 120.0);
            telePos[2] = targetPos[2] + GetRandomFloat(0.0, 30.0);
            if (AI_IsValidPosition(telePos)) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_BotState[client].teleportAttempts++;
            g_BotState[client].lastTeleportTime = curTime;
            return false;
        }
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
}

/**
 * Check if the bot has exhausted all teleport attempts and should abandon the task.
 */
stock bool AI_ShouldAbandonNavigation(int client)
{
    return g_BotState[client].teleportAttempts >= TELEPORT_MAX_ATTEMPTS;
}
