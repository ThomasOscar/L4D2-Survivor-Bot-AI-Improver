/**
 * AI Math Utilities
 * 
 * Pure math helper functions for vector operations, angles, and clamping.
 * No global variable dependencies — all data passed via parameters.
 */

#if defined _math_utils_included
    #endinput
#endif
#define _math_utils_included

#include <sourcemod>
#include <sdktools>

/**
 * Get the distance between two 3D points.
 *
 * @param vec1      First point.
 * @param vec2      Second point.
 * @param squared   If true, return squared distance (faster, no sqrt).
 * @return          Distance between the two points.
 */
stock float AI_GetVectorDistance(const float vec1[3], const float vec2[3], bool squared = false)
{
    return GetVectorDistance(vec1, vec2, squared);
}

/**
 * Normalize a 3D vector in-place.
 *
 * @param vec   Vector to normalize (modified in place).
 * @return      Original magnitude of the vector.
 */
stock float AI_NormalizeVector(float vec[3])
{
    return NormalizeVector(vec, vec);
}

/**
 * Get the angle (in degrees) between two 3D direction vectors.
 *
 * @param vec1  First direction vector.
 * @param vec2  Second direction vector.
 * @return      Angle in degrees between the two vectors.
 */
stock float AI_GetAngleBetweenVectors(const float vec1[3], const float vec2[3])
{
    float norm1[3], norm2[3];
    norm1[0] = vec1[0]; norm1[1] = vec1[1]; norm1[2] = vec1[2];
    norm2[0] = vec2[0]; norm2[1] = vec2[1]; norm2[2] = vec2[2];
    NormalizeVector(norm1, norm1);
    NormalizeVector(norm2, norm2);

    float dot = GetVectorDotProduct(norm1, norm2);
    // Clamp to [-1, 1] to avoid NaN from ArcCosine
    if (dot > 1.0) dot = 1.0;
    else if (dot < -1.0) dot = -1.0;

    return RadToDeg(ArcCosine(dot));
}

/**
 * Clamp a float value between min and max.
 *
 * @param value     Value to clamp.
 * @param min       Minimum bound.
 * @param max       Maximum bound.
 * @return          Clamped value.
 */
stock float AI_ClampFloat(float value, float min, float max)
{
    return (value > max) ? max : ((value < min) ? min : value);
}

/**
 * Normalize an angle to the range [-180.0, 180.0].
 *
 * @param angle     Angle in degrees.
 * @return          Normalized angle.
 */
stock float AI_NormalizeAngle(float angle)
{
    angle = (angle - RoundToFloor(angle / 360.0) * 360.0);
    if (angle > 180.0) angle -= 360.0;
    else if (angle < -180.0) angle += 360.0;
    return angle;
}

/**
 * Check if a vector is non-zero (i.e. at least one component != 0).
 *
 * @param vec   Vector to check.
 * @return      True if at least one component is non-zero.
 */
stock bool AI_IsValidVector(const float vec[3])
{
    return (vec[0] != 0.0 || vec[1] != 0.0 || vec[2] != 0.0);
}

/**
 * Set all components of a vector to zero.
 *
 * @param vec   Vector to zero out (modified in place).
 */
stock void AI_SetVectorToZero(float vec[3])
{
    vec[0] = 0.0;
    vec[1] = 0.0;
    vec[2] = 0.0;
}

/**
 * Compute the dot product between a client's facing direction
 * and the direction from the client to a target position (2D, ignoring Z).
 *
 * @param clientOrigin  Client's world position.
 * @param clientAngles  Client's eye angles.
 * @param targetPos     Target position to compare against.
 * @return              Dot product [-1.0 .. 1.0]. 1.0 = directly facing target.
 */
stock float AI_GetViewDotProduct(const float clientOrigin[3], const float clientAngles[3], const float targetPos[3])
{
    float lineOfSight[3];
    MakeVectorFromPoints(clientOrigin, targetPos, lineOfSight);
    lineOfSight[2] = 0.0;
    NormalizeVector(lineOfSight, lineOfSight);

    float direction[3];
    direction[0] = clientAngles[0];
    direction[1] = clientAngles[1];
    direction[2] = clientAngles[2];
    GetAngleVectors(direction, direction, NULL_VECTOR, NULL_VECTOR);
    direction[2] = 0.0;
    NormalizeVector(direction, direction);

    return GetVectorDotProduct(lineOfSight, direction);
}

/**
 * Check if a target position is within a given field-of-view angle
 * from a client's perspective (2D check).
 *
 * @param clientOrigin  Client's world position.
 * @param clientAngles  Client's eye/abs angles.
 * @param targetPos     Target position to check.
 * @param fovDegrees    Half-angle of the FOV cone in degrees.
 * @return              True if the target is within the FOV.
 */
stock bool AI_IsInFieldOfView(const float clientOrigin[3], const float clientAngles[3], const float targetPos[3], float fovDegrees)
{
    float dot = AI_GetViewDotProduct(clientOrigin, clientAngles, targetPos);
    // Clamp to avoid NaN
    if (dot > 1.0) dot = 1.0;
    else if (dot < -1.0) dot = -1.0;
    return (RadToDeg(ArcCosine(dot)) <= fovDegrees);
}
