/**
 * AI Aim Utilities
 * 
 * Aiming, visibility checks, and attack button helpers.
 * No global variable dependencies — all data passed via parameters.
 */

#if defined _aim_utils_included
    #endinput
#endif
#define _aim_utils_included

#include <sourcemod>
#include <sdktools>

// ============================================================================
// Trace Filter
// ============================================================================

/**
 * Standard trace filter: passes through the 'data' entity (self) and closed doors.
 *
 * @param entity        Entity hit by the trace.
 * @param contentsMask  Contents mask.
 * @param data          The entity to ignore (typically the tracing client).
 * @return              True if the trace should pass through this entity.
 */
stock bool AI_BaseTraceFilter(int entity, int contentsMask, int data)
{
    if (entity == data)
        return false;
    // Also pass through doors that are open
    if (HasEntProp(entity, Prop_Data, "m_eDoorState"))
        return false;
    return true;
}

// ============================================================================
// Visibility
// ============================================================================

/**
 * Check if there is a clear line of sight from an eye position to a target position.
 *
 * @param eyePos    Start position (typically client eye position).
 * @param targetPos End position to check visibility to.
 * @param mask      Trace mask (default MASK_SHOT).
 * @param ignoreEnt Entity to ignore in the trace (typically the tracing client).
 * @return          True if the path is clear.
 */
stock bool AI_IsVisiblePosition(const float eyePos[3], const float targetPos[3], int mask = MASK_SHOT, int ignoreEnt = -1)
{
    Handle hResult = TR_TraceRayFilterEx(eyePos, targetPos, mask, RayType_EndPoint, AI_BaseTraceFilter, ignoreEnt);
    float fraction = TR_GetFraction(hResult);
    delete hResult;
    return (fraction >= 0.99);
}

/**
 * Check if a client can see a target entity.
 * Tries three points on the target: abs origin, abs origin + view offset, and centeroid.
 *
 * @param clientEyePos  The client's eye position.
 * @param targetEntity  The target entity index.
 * @param mask          Trace mask (default MASK_SHOT).
 * @param ignoreEnt     Entity to ignore (typically the tracing client).
 * @return              True if any of the three check points are visible.
 */
stock bool AI_IsVisibleEntity(const float clientEyePos[3], int targetEntity, int mask = MASK_SHOT, int ignoreEnt = -1)
{
    if (!IsValidEntity(targetEntity))
        return false;

    float targetPos[3];
    GetEntPropVector(targetEntity, Prop_Data, "m_vecAbsOrigin", targetPos);

    // Check 1: abs origin
    Handle hResult = TR_TraceRayFilterEx(clientEyePos, targetPos, mask, RayType_EndPoint, AI_BaseTraceFilter, ignoreEnt);
    bool hit = ((TR_GetFraction(hResult) >= 0.99 && !TR_StartSolid(hResult)) || TR_GetEntityIndex(hResult) == targetEntity);
    delete hResult;
    if (hit)
        return true;

    // Check 2: abs origin + view offset (head)
    if (HasEntProp(targetEntity, Prop_Data, "m_vecViewOffset"))
    {
        float viewOffset[3];
        GetEntPropVector(targetEntity, Prop_Data, "m_vecViewOffset", viewOffset);
        float headPos[3];
        AddVectors(targetPos, viewOffset, headPos);

        hResult = TR_TraceRayFilterEx(clientEyePos, headPos, mask, RayType_EndPoint, AI_BaseTraceFilter, ignoreEnt);
        hit = ((TR_GetFraction(hResult) >= 0.99 && !TR_StartSolid(hResult)) || TR_GetEntityIndex(hResult) == targetEntity);
        delete hResult;
        if (hit)
            return true;
    }

    // Check 3: bounding box center
    char netClass[64];
    if (GetEntityNetClass(targetEntity, netClass, sizeof(netClass)))
    {
        int offset = FindSendPropInfo(netClass, "m_vecMins");
        if (offset != -1)
        {
            float mins[3], maxs[3], centeroid[3];
            GetEntDataVector(targetEntity, offset, mins);
            GetEntPropVector(targetEntity, Prop_Send, "m_vecMaxs", maxs);
            centeroid[0] = targetPos[0] + (mins[0] + maxs[0]) * 0.5;
            centeroid[1] = targetPos[1] + (mins[1] + maxs[1]) * 0.5;
            centeroid[2] = targetPos[2] + (mins[2] + maxs[2]) * 0.5;

            hResult = TR_TraceRayFilterEx(clientEyePos, centeroid, mask, RayType_EndPoint, AI_BaseTraceFilter, ignoreEnt);
            hit = ((TR_GetFraction(hResult) >= 0.99 && !TR_StartSolid(hResult)) || TR_GetEntityIndex(hResult) == targetEntity);
            delete hResult;
            if (hit)
                return true;
        }
    }

    return false;
}

// ============================================================================
// Aim Control
// ============================================================================

/**
 * Calculate the aim angle from one position to another.
 *
 * @param eyePos    Source position (eye position).
 * @param targetPos Target position to aim at.
 * @param result    Output angle vector (pitch, yaw, roll).
 */
stock void AI_GetAimAngle(const float eyePos[3], const float targetPos[3], float result[3])
{
    float dir[3];
    MakeVectorFromPoints(eyePos, targetPos, dir);
    GetVectorAngles(dir, result);
}

/**
 * Snap a bot's view to face a target position.
 * Smoothly interpolates using angle normalization to avoid snapping artifacts.
 *
 * @param client        Client index.
 * @param targetPos     World position to look at.
 * @param currentEyePos Client's current eye position.
 * @param currentEyeAng Client's current eye angles.
 */
stock void AI_SnapViewToPosition(int client, const float targetPos[3], const float currentEyePos[3], const float currentEyeAng[3])
{
    float desiredDir[3];
    MakeVectorFromPoints(currentEyePos, targetPos, desiredDir);
    GetVectorAngles(desiredDir, desiredDir);

    float eyeAngles[3];
    eyeAngles[0] = currentEyeAng[0] + AI_NormalizeAngleInternal(desiredDir[0] - currentEyeAng[0]);
    eyeAngles[1] = currentEyeAng[1] + AI_NormalizeAngleInternal(desiredDir[1] - currentEyeAng[1]);
    eyeAngles[2] = 0.0;

    TeleportEntity(client, NULL_VECTOR, eyeAngles, NULL_VECTOR);
}

/**
 * Internal angle normalizer for aim interpolation.
 * Normalizes an angle delta to [-180, 180] range.
 */
static stock float AI_NormalizeAngleInternal(float angle)
{
    angle = (angle - RoundToFloor(angle / 360.0) * 360.0);
    if (angle > 180.0) angle -= 360.0;
    else if (angle < -180.0) angle += 360.0;
    return angle;
}

/**
 * Get the position where a client is currently aiming (trace from eyes along eye angles).
 *
 * @param clientEyePos  Client's eye position.
 * @param clientEyeAng  Client's eye angles.
 * @param result        Output position where the trace hit.
 * @param ignoreEnt     Entity to ignore in the trace (the client).
 */
stock void AI_GetClientAimPosition(const float clientEyePos[3], const float clientEyeAng[3], float result[3], int ignoreEnt = -1)
{
    // Convert eye angles to direction vector — RayType_Infinite expects a direction, not angles
    float direction[3];
    GetAngleVectors(clientEyeAng, direction, NULL_VECTOR, NULL_VECTOR);

    Handle hResult = TR_TraceRayFilterEx(clientEyePos, direction, MASK_SHOT, RayType_Infinite, AI_BaseTraceFilter, ignoreEnt);
    TR_GetEndPosition(result, hResult);
    delete hResult;
}

// ============================================================================
// Attack Buttons
// ============================================================================

/**
 * Press the primary attack button (IN_ATTACK).
 *
 * @param client    Client index (unused, reserved for future validation).
 * @param buttons   Button bitmask to modify (passed by reference).
 */
stock void AI_PressAttackButton(int client, int &buttons)
{
    buttons |= IN_ATTACK;
}

/**
 * Press the secondary attack / shove button (IN_ATTACK2).
 *
 * @param client    Client index (unused, reserved for future validation).
 * @param buttons   Button bitmask to modify (passed by reference).
 */
stock void AI_PressShoveButton(int client, int &buttons)
{
    buttons |= IN_ATTACK2;
}

/**
 * Press the USE button (IN_USE).
 *
 * @param client    Client index (unused, reserved for future validation).
 * @param buttons   Button bitmask to modify (passed by reference).
 */
stock void AI_PressUseButton(int client, int &buttons)
{
    buttons |= IN_USE;
}
