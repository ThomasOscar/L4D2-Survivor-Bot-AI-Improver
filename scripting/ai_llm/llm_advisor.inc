/**
 * L4D2 AI Bot - LLM Strategic Advisor
 * 
 * Connects to the Python LLM service via TCP (SocketExtension).
 * Collects game state every ~15 seconds, sends to LLM, receives strategic suggestions.
 * 
 * Design principles:
 *   - LLM is an "advisor", not a "controller"
 *   - Suggestions are stored in BotState; task modules reference but don't blindly follow
 *   - Emergency situations (pinned/incapped/Tank) always use local task priority
 *   - Bot behavior is 100% normal when LLM is disconnected
 *   - Call frequency: every 15-30 seconds (strategic, not real-time)
 *
 * Depends on: <socket> extension (same as old l4d2_sb_ai_improver.sp)
 */

#if defined _ai_llm_advisor_included
 #endinput
#endif
#define _ai_llm_advisor_included

// ============================================================
// Constants
// ============================================================

#define LLM_STATE_BUFFER_SIZE   4096
#define LLM_RECV_BUFFER_SIZE    2048
#define LLM_DEFAULT_HOST        "127.0.0.1"
#define LLM_DEFAULT_PORT        9876
#define LLM_SEND_INTERVAL       15.0    // seconds between state sends
#define LLM_RECONNECT_DELAY     5.0     // seconds before reconnect attempt (initial)
#define LLM_RECONNECT_MAX_DELAY 120.0   // max reconnect delay after repeated failures
#define LLM_CONNECT_TIMEOUT     10.0    // seconds before connect timeout
#define LLM_HEARTBEAT_INTERVAL  30.0    // seconds between pings
#define LLM_HEARTBEAT_TIMEOUT   60.0    // seconds without pong before reconnect
#define LLM_SUGGESTION_EXPIRY   30.0    // seconds before a suggestion expires

// ============================================================
// ConVars (LLM-specific)
// ============================================================

ConVar g_cvLLMHost;
ConVar g_cvLLMPort;
ConVar g_cvLLMInterval;

// ============================================================
// Socket State
// ============================================================

Socket g_hLLMSocket = null;
bool   g_bLLMConnected = false;
Handle g_hLLMConnectTimer = null;
Handle g_hLLMSendTimer = null;
Handle g_hLLMHeartbeatTimer = null;
float  g_fLLMLastPong = 0.0;
int    g_iLLMConsecutiveErrors = 0;     // Track consecutive errors for backoff

// ============================================================
// Public Interface
// ============================================================

/**
 * Initialize LLM advisor. Called from OnPluginStart.
 */
void LLMAdvisor_Init() {
    g_cvLLMHost     = CreateConVar("ai_llm_host", LLM_DEFAULT_HOST, "LLM service host", FCVAR_PROTECTED);
    g_cvLLMPort     = CreateConVar("ai_llm_port", "9876", "LLM service port", FCVAR_PROTECTED, true, 1.0, true, 65535.0);
    g_cvLLMInterval = CreateConVar("ai_llm_interval", "15.0", "Seconds between LLM state sends", FCVAR_NOTIFY, true, 5.0, true, 60.0);

    LogMessage("[LLM Advisor] Initialized (host=%s, port=%d)", LLM_DEFAULT_HOST, LLM_DEFAULT_PORT);
}

/**
 * Shutdown LLM advisor. Called from OnPluginEnd.
 */
void LLMAdvisor_Shutdown() {
    LLM_DisconnectCleanup();
    LogMessage("[LLM Advisor] Shutdown.");
}

/**
 * Called on map start — attempt connection if LLM is enabled.
 */
void LLMAdvisor_OnMapStart() {
    if (!g_bLLMEnabled) return;

    // Delay connection slightly to let map settle
    CreateTimer(3.0, Timer_LLMAdvisor_DelayedConnect, _, TIMER_FLAG_NO_MAPCHANGE);
}

/**
 * Called when LLM is disabled via ConVar — stop all socket activity.
 */
void LLMAdvisor_OnDisabled() {
    LLM_DisconnectCleanup();
    LogMessage("[LLM Advisor] Disabled via ConVar — disconnected and stopped reconnecting.");
}

/**
 * Check if LLM is currently connected.
 */
bool LLMAdvisor_IsConnected() {
    return g_bLLMConnected;
}

// ============================================================
// Connection Management
// ============================================================

static void LLM_AttemptConnect() {
    if (!g_bLLMEnabled) return;
    if (g_bLLMConnected) return;

    // Cancel any pending connect timeout timer
    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = null;

    // Note: Don't try to Disconnect()/delete old socket here.
    // After OnSocketError, the socket handle is already invalidated by the extension.
    // We nulled g_hLLMSocket in the error callback to prevent double-close.
    // Just create a new socket.

    g_hLLMSocket = new Socket(SOCKET_TCP, LLMAdvisor_OnSocketError);
    if (g_hLLMSocket == null) {
        LogError("[LLM Advisor] Failed to create socket");
        CreateTimer(LLM_RECONNECT_DELAY, Timer_LLMAdvisor_Reconnect, _, TIMER_FLAG_NO_MAPCHANGE);
        return;
    }

    char host[64];
    g_cvLLMHost.GetString(host, sizeof(host));
    int port = g_cvLLMPort.IntValue;

    g_hLLMSocket.Connect(LLMAdvisor_OnConnected, LLMAdvisor_OnReceive, LLMAdvisor_OnDisconnected, host, port);
    PrintToServer("[LLM Advisor] Connecting to %s:%d...", host, port);

    // Connection timeout
    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = CreateTimer(LLM_CONNECT_TIMEOUT, Timer_LLMAdvisor_ConnectTimeout, _, TIMER_FLAG_NO_MAPCHANGE);
}

static void LLM_DisconnectCleanup() {
    g_bLLMConnected = false;

    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = null;

    delete g_hLLMSendTimer;
    g_hLLMSendTimer = null;

    delete g_hLLMHeartbeatTimer;
    g_hLLMHeartbeatTimer = null;

    if (g_hLLMSocket != null) {
        g_hLLMSocket.Disconnect();
        delete g_hLLMSocket;
        g_hLLMSocket = null;
    }
}

// ============================================================
// Socket Callbacks
// ============================================================

public void LLMAdvisor_OnConnected(Socket socket, any arg) {
    g_bLLMConnected = true;
    g_fLLMLastPong = GetGameTime();
    g_iLLMConsecutiveErrors = 0;     // Reset error counter on successful connect
    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = null;

    PrintToServer("[LLM Advisor] Connected to LLM service!");

    // Start periodic state send timer
    float interval = g_cvLLMInterval.FloatValue;
    if (interval < 5.0) interval = LLM_SEND_INTERVAL;
    delete g_hLLMSendTimer;
    g_hLLMSendTimer = CreateTimer(interval, Timer_LLMAdvisor_SendState, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);

    // Start heartbeat timer
    delete g_hLLMHeartbeatTimer;
    g_hLLMHeartbeatTimer = CreateTimer(LLM_HEARTBEAT_INTERVAL, Timer_LLMAdvisor_Heartbeat, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

public void LLMAdvisor_OnReceive(Socket socket, const char[] data, const int size, any arg) {
    // Check for pong (heartbeat response)
    if (StrContains(data, "\"pong\"") != -1) {
        g_fLLMLastPong = GetGameTime();
        return;
    }

    // Parse LLM decision
    LLMAdvisor_ParseDecision(data, size);
}

public void LLMAdvisor_OnDisconnected(Socket socket, any arg) {
    g_bLLMConnected = false;
    g_hLLMSocket = null;

    delete g_hLLMSendTimer;
    g_hLLMSendTimer = null;
    delete g_hLLMHeartbeatTimer;
    g_hLLMHeartbeatTimer = null;
    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = null;

    if (g_bLLMEnabled) {
        PrintToServer("[LLM Advisor] Disconnected from LLM service.");
        CreateTimer(LLM_RECONNECT_DELAY, Timer_LLMAdvisor_Reconnect, _, TIMER_FLAG_NO_MAPCHANGE);
    }
}

public void LLMAdvisor_OnSocketError(Socket socket, const int errorType, const int errorNum, any arg) {
    g_bLLMConnected = false;
    
    // Socket handle is invalidated by the extension after error — null it out
    // to prevent double-close in LLM_AttemptConnect
    g_hLLMSocket = null;
    
    // Cancel any pending connect timeout timer
    delete g_hLLMConnectTimer;
    g_hLLMConnectTimer = null;

    // Clean up send/heartbeat timers
    delete g_hLLMSendTimer;
    g_hLLMSendTimer = null;
    delete g_hLLMHeartbeatTimer;
    g_hLLMHeartbeatTimer = null;

    // Only log and retry if LLM is still enabled
    if (g_bLLMEnabled) {
        g_iLLMConsecutiveErrors++;
        // Only log first 3 errors, then every 10th error to avoid log spam
        if (g_iLLMConsecutiveErrors <= 3 || (g_iLLMConsecutiveErrors % 10) == 0) {
            LogError("[LLM Advisor] Socket error: type=%d num=%d (attempt %d)",
                     errorType, errorNum, g_iLLMConsecutiveErrors);
        }
        // Exponential backoff: 5s, 10s, 20s, 40s... up to 120s
        float delay = float(1 << (g_iLLMConsecutiveErrors - 1)) * LLM_RECONNECT_DELAY;
        if (delay > LLM_RECONNECT_MAX_DELAY) delay = LLM_RECONNECT_MAX_DELAY;
        CreateTimer(delay, Timer_LLMAdvisor_Reconnect, _, TIMER_FLAG_NO_MAPCHANGE);
    }
}

// ============================================================
// Timers
// ============================================================

public Action Timer_LLMAdvisor_DelayedConnect(Handle timer) {
    LLM_AttemptConnect();
    return Plugin_Stop;
}

public Action Timer_LLMAdvisor_Reconnect(Handle timer) {
    if (!g_bLLMConnected && g_bLLMEnabled) {
        LLM_AttemptConnect();
    }
    return Plugin_Stop;
}

public Action Timer_LLMAdvisor_ConnectTimeout(Handle timer) {
    g_hLLMConnectTimer = null;
    if (!g_bLLMConnected && g_bLLMEnabled) {
        PrintToServer("[LLM Advisor] Connection timeout, retrying...");
        LLM_AttemptConnect();
    }
    return Plugin_Stop;
}

public Action Timer_LLMAdvisor_SendState(Handle timer) {
    if (!g_bLLMConnected || !g_bLLMEnabled) return Plugin_Continue;
    LLMAdvisor_CollectAndSend();
    return Plugin_Continue;
}

public Action Timer_LLMAdvisor_Heartbeat(Handle timer) {
    if (g_hLLMSocket == null || !g_bLLMConnected) {
        g_hLLMHeartbeatTimer = null;
        return Plugin_Stop;
    }

    // Check heartbeat timeout
    if ((GetGameTime() - g_fLLMLastPong) > LLM_HEARTBEAT_TIMEOUT) {
        PrintToServer("[LLM Advisor] Heartbeat timeout, reconnecting...");
        LLM_DisconnectCleanup();
        CreateTimer(3.0, Timer_LLMAdvisor_Reconnect, _, TIMER_FLAG_NO_MAPCHANGE);
        return Plugin_Stop;
    }

    // Send ping
    char ping[128];
    Format(ping, sizeof(ping), "{\"type\":\"ping\",\"time\":%.2f}\n", GetGameTime());
    g_hLLMSocket.Send(ping);

    return Plugin_Continue;
}

// ============================================================
// State Collection & Send
// ============================================================

static void LLMAdvisor_CollectAndSend() {
    if (g_hLLMSocket == null || !g_bLLMConnected) return;

    char state[LLM_STATE_BUFFER_SIZE];
    LLMAdvisor_BuildStateJSON(state, sizeof(state));

    g_hLLMSocket.Send(state);

    if (g_bDebugMode) {
        PrintToServer("[LLM Advisor] Sent state to LLM service.");
    }
}

/**
 * Build a JSON state payload with all bot and game info.
 * Simplified version — sends essential data for strategic decisions.
 */
static void LLMAdvisor_BuildStateJSON(char[] buffer, int maxlen) {
    // Map info
    char mapName[128];
    GetCurrentMap(mapName, sizeof(mapName));

    // Build bots array
    char bots[2048];
    bots[0] = '\0';
    int botCount = 0;

    for (int i = 1; i <= MaxClients; i++) {
        if (!IsActiveSurvivorBot(i)) continue;

        char name[64];
        GetClientName(i, name, sizeof(name));
        int hp = GetClientHealth(i);
        bool incap = GetEntProp(i, Prop_Send, "m_isIncapacitated") != 0;
        float pos[3];
        GetClientAbsOrigin(i, pos);

        // Weapon info (primary)
        char primary[64];
        primary[0] = '\0';
        int w = GetPlayerWeaponSlot(i, 0);
        if (w != -1) GetEntityClassname(w, primary, sizeof(primary));
        if (primary[0] == '\0') strcopy(primary, sizeof(primary), "none");

        // Current LLM suggestion status
        char curAction[32];
        curAction[0] = '\0';
        if (g_BotState[i].hasLLMSuggestion) {
            strcopy(curAction, sizeof(curAction), g_BotState[i].llmAction);
        }

        // Controlled by SI?
        int pinned = g_BotState[i].controlledByType;

        if (botCount > 0) Format(bots, sizeof(bots), "%s,", bots);
        Format(bots, sizeof(bots),
            "%s{\"name\":\"%s\",\"hp\":%d,\"incap\":%s,\"pos\":[%.0f,%.0f,%.0f],\"weapon\":\"%s\",\"pinned\":%d,\"cur_action\":\"%s\"}",
            bots, name, hp, incap ? "true" : "false",
            pos[0], pos[1], pos[2], primary, pinned, curAction);
        botCount++;
    }

    // Build threats array (special infected)
    char threats[1024];
    threats[0] = '\0';
    int threatCount = 0;

    for (int i = 1; i <= MaxClients; i++) {
        if (!IsClientInGame(i) || GetClientTeam(i) != 3 || !IsPlayerAlive(i)) continue;

        int zClass = GetEntProp(i, Prop_Send, "m_zombieClass");
        char zName[16];
        switch (zClass) {
            case 1: strcopy(zName, sizeof(zName), "smoker");
            case 2: strcopy(zName, sizeof(zName), "boomer");
            case 3: strcopy(zName, sizeof(zName), "hunter");
            case 4: strcopy(zName, sizeof(zName), "spitter");
            case 5: strcopy(zName, sizeof(zName), "jockey");
            case 6: strcopy(zName, sizeof(zName), "charger");
            case 8: strcopy(zName, sizeof(zName), "tank");
            default: strcopy(zName, sizeof(zName), "unknown");
        }

        float pos[3];
        GetClientAbsOrigin(i, pos);
        int hp = GetClientHealth(i);

        if (threatCount > 0) Format(threats, sizeof(threats), "%s,", threats);
        Format(threats, sizeof(threats),
            "%s{\"type\":\"%s\",\"hp\":%d,\"pos\":[%.0f,%.0f,%.0f]}",
            threats, zName, hp, pos[0], pos[1], pos[2]);
        threatCount++;
    }

    // Assemble final JSON
    Format(buffer, maxlen,
        "{\"type\":\"state\",\"map\":\"%s\",\"time\":%.1f,\"bots\":[%s],\"threats\":[%s]}\n",
        mapName, GetGameTime(), bots, threats);
}

// ============================================================
// Decision Parsing
// ============================================================

/**
 * Parse LLM decision JSON and store suggestions in BotState.
 * Supports multi-bot format:
 *   {"decisions":[{"bot":"Ellis","action":"attack_si","target":"Smoker"}, ...]}
 * And single format:
 *   {"action":"hold_position","target":""}
 */
static void LLMAdvisor_ParseDecision(const char[] data, int size) {
    char buf[LLM_RECV_BUFFER_SIZE];
    int copyLen = size;
    if (copyLen >= sizeof(buf)) copyLen = sizeof(buf) - 1;
    strcopy(buf, copyLen + 1, data);

    // Try multi-bot decisions array format
    int decPos = StrContains(buf, "\"decisions\"");
    if (decPos != -1) {
        LLMAdvisor_ParseMultiBotDecisions(buf, sizeof(buf));
        return;
    }

    // Fallback: single action applied to all bots
    char action[32];
    char target[64];
    if (!LLMAdvisor_ExtractField(buf, sizeof(buf), "action", action, sizeof(action))) return;
    LLMAdvisor_ExtractField(buf, sizeof(buf), "target", target, sizeof(target));

    // Apply to all active bots
    float curTime = GetGameTime();
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsActiveSurvivorBot(i)) continue;
        LLMAdvisor_ApplySuggestion(i, action, target, curTime);
    }
}

/**
 * Parse multi-bot decisions: {"decisions":[{"bot":"Ellis","action":"..","target":".."},..]}
 */
static void LLMAdvisor_ParseMultiBotDecisions(const char[] buf, int bufLen) {
    float curTime = GetGameTime();
    int searchStart = 0;

    // Find start of decisions array
    int decPos = StrContains(buf, "\"decisions\"");
    if (decPos == -1) return;
    searchStart = decPos + 11;

    int parsed = 0;
    while (parsed < MAXPLAYERS) {
        // Find next "bot" field
        int bp = StrContains(buf[searchStart], "\"bot\"");
        if (bp == -1) break;
        bp += searchStart;

        // Extract bot name
        char botName[64];
        if (!LLMAdvisor_ExtractValueAt(buf, bufLen, bp + 4, botName, sizeof(botName))) break;

        // Extract action
        char action[32];
        action[0] = '\0';
        int afterBot = bp + 4 + strlen(botName) + 10;  // rough offset past bot value
        if (afterBot >= bufLen) break;
        int ap = StrContains(buf[afterBot], "\"action\"");
        if (ap != -1) {
            ap += afterBot;
            LLMAdvisor_ExtractValueAt(buf, bufLen, ap + 8, action, sizeof(action));
        }

        // Extract target
        char target[64];
        target[0] = '\0';
        int afterAction = afterBot + 40;  // rough offset past action
        if (afterAction < bufLen) {
            int tp = StrContains(buf[afterAction], "\"target\"");
            if (tp != -1) {
                tp += afterAction;
                LLMAdvisor_ExtractValueAt(buf, bufLen, tp + 8, target, sizeof(target));
            }
        }

        // Find the bot client by name
        int client = LLMAdvisor_FindBotByName(botName);
        if (client > 0) {
            LLMAdvisor_ApplySuggestion(client, action, target, curTime);
            if (g_bDebugMode) {
                PrintToServer("[LLM Advisor] Bot %s: action=%s target=%s", botName, action, target);
            }
        }

        // Advance search
        searchStart = bp + 20;
        parsed++;
    }
}

// ============================================================
// Suggestion Application
// ============================================================

/**
 * Store LLM suggestion into a bot's state.
 * Task modules can then reference g_BotState[client].llmAction/llmTarget.
 */
static void LLMAdvisor_ApplySuggestion(int client, const char[] action, const char[] target, float curTime) {
    if (action[0] == '\0') return;

    strcopy(g_BotState[client].llmAction, sizeof(BotState::llmAction), action);
    strcopy(g_BotState[client].llmTarget, sizeof(BotState::llmTarget), target);
    g_BotState[client].hasLLMSuggestion = true;
    g_BotState[client].llmSuggestionTime = curTime;
}

/**
 * Check and expire stale LLM suggestions.
 * Called from BotState_CacheBasicData or a periodic check.
 */
void LLMAdvisor_ExpireStale(int client) {
    if (!g_BotState[client].hasLLMSuggestion) return;

    if ((GetGameTime() - g_BotState[client].llmSuggestionTime) > LLM_SUGGESTION_EXPIRY) {
        g_BotState[client].hasLLMSuggestion = false;
        g_BotState[client].llmAction[0] = '\0';
        g_BotState[client].llmTarget[0] = '\0';
    }
}

// ============================================================
// JSON Parsing Helpers (lightweight, no json.inc dependency)
// ============================================================

/**
 * Extract a string field value from a JSON-like buffer.
 * Looks for "fieldName":"value" pattern.
 *
 * @return true if found
 */
static bool LLMAdvisor_ExtractField(const char[] buf, int bufLen, const char[] fieldName,
                                      char[] out, int outLen) {
    char search[64];
    Format(search, sizeof(search), "\"%s\"", fieldName);

    int pos = StrContains(buf, search);
    if (pos == -1) return false;

    return LLMAdvisor_ExtractValueAt(buf, bufLen, pos + strlen(search), out, outLen);
}

/**
 * Extract a JSON string value starting from a given position.
 * Scans past :, spaces, and opening quote to find the value.
 *
 * @return true if a value was extracted
 */
static bool LLMAdvisor_ExtractValueAt(const char[] buf, int bufLen, int startPos,
                                        char[] out, int outLen) {
    int i = startPos;
    // Skip : and whitespace
    while (i < bufLen && (buf[i] == ' ' || buf[i] == ':' || buf[i] == '\t')) i++;

    if (i >= bufLen) return false;

    // Check for opening quote
    if (buf[i] == '"') {
        i++;
        int start = i;
        while (i < bufLen && buf[i] != '"' && buf[i] != '\0') i++;
        int len = i - start;
        if (len <= 0 || len >= outLen) {
            if (len >= outLen) len = outLen - 1;
            if (len <= 0) return false;
        }
        strcopy(out, len + 1, buf[start]);
        return true;
    }

    // Unquoted value (number, bool, etc.)
    int start = i;
    while (i < bufLen && buf[i] != ',' && buf[i] != '}' && buf[i] != ']' &&
           buf[i] != ' ' && buf[i] != '\0') i++;
    int len = i - start;
    if (len <= 0) return false;
    if (len >= outLen) len = outLen - 1;
    strcopy(out, len + 1, buf[start]);
    return true;
}

/**
 * Find a bot client index by in-game name.
 *
 * @return client index, or -1 if not found
 */
static int LLMAdvisor_FindBotByName(const char[] botName) {
    char name[64];
    for (int i = 1; i <= MaxClients; i++) {
        if (!IsActiveSurvivorBot(i)) continue;
        GetClientName(i, name, sizeof(name));
        if (StrEqual(name, botName, false)) return i;
    }
    return -1;
}
