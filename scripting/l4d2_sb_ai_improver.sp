/*======================================================================================
	This is a modified version of Bot Improver
	
	Notable changes here:
	
	OnPlayerRunCmd()
	SurvivorBotThink() - slight change for witch targeting, also changed item scavenge behavior
	CheckEntityForStuff()
	CheckForItemsToScavenge()
	GetItemFromArray()
	GetWeaponClassname()
	GetWeaponMaxAmmo()
	GetWeaponTier()
	SurvivorHasPistol() and similar
	GetSurvivorTeamInventoryCount() - new
	GetClientDistanceToItem() - to replace GetEntityDistance()
	L4D2_OnFindScavengeItem()
	
	GetNavDistance() - to replace GetVectorTravelDistance(). If you pass an entity ID to it, it will remember
	if distance to the entity could not be measured, and won't hammer the server with more useless calculations. Yay!
	
	GetClientTravelDistance() - L4D2_IsReachable is used instead of L4D2_NavAreaBuildPath. It does essentially same thing,
	outputs same boolean, and does not cause as much lag as the other function.
	
	LBI_IsReachablePosition() - argument to ignore LOS when picking nearest nav area.
	LBI_IsPathToPositionDangerous() - L4D2_IsReachable is used instead of L4D2_NavAreaBuildPath. Additional cutoff for amount of processed nav areas.
	DTR_OnFindUseEntity() - prevent bots from grabbing items from absurd distances.

======================================================================================*/

#pragma newdecls required
#pragma semicolon 1

#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#include <dhooks>
#include <left4dhooks_vscript>
#include <profiler>
#include <adt_trie>
#include <socket>

#undef REQUIRE_EXTENSIONS
#include <actions>
#define REQUIRE_EXTENSIONS

#undef REQUIRE_PLUGIN
#include <vscript> //https://github.com/FortyTwoFortyTwo/VScript
#define REQUIRE_PLUGIN

public Plugin myinfo = 
{
	name 		= "[L4D2] Survivor Bot AI Improver",
	author 		= "Emana202, Kerouha",
	description = "Attempt at improving survivor bots' AI and behaviour as much as possible.",
	version 	= "1.6k",
	url 		= "https://forums.alliedmods.net/showthread.php?t=342872"
}

// =====================================================================
// LLM STRATEGIC DECISION MODULE
// =====================================================================
// This module adds a high-level LLM decision layer on top of IB's
// rule-based behavior. LLM provides strategic context that IB lacks:
// - Global situation assessment (should we push/hold/retreat?)
// - Team coordination (who helps, who fights?)
// - Terrain/event awareness (alarm cars, finales)
// - Adaptive priority (when to override IB's defaults)
//
// Architecture: LLM -> sets internal IB variables -> IB executes
// =====================================================================

#define LLM_STATE_BUFFER 3072
#define LLM_ACTION_MAX 32
#define LLM_TARGET_MAX 64
#define LLM_RECENT_EVENTS_MAX 8

// LLM ConVars
ConVar g_hCvar_LLM_Enabled;
ConVar g_hCvar_LLM_Host;
ConVar g_hCvar_LLM_Port;
ConVar g_hCvar_LLM_Interval;
ConVar g_hCvar_LLM_TestMode;

// LLM Socket
Socket g_hLLM_Socket = null;
bool g_bLLM_Connected = false;
Handle g_hLLM_ConnectTimer = null;
Handle g_hLLM_TestTimer = null;
float g_fLLM_LastSendTime = 0.0;

// LLM Decision State
char g_sLLM_Action[LLM_ACTION_MAX] = "follow_team";
char g_sLLM_Target[LLM_TARGET_MAX] = "";
float g_fLLM_ActionExpire = 0.0;
bool g_bLLM_ActionActive = false;
int g_iLLM_OverrideTarget = 0;     // entity ref for target override
float g_fLLM_OverrideMovePos[3];   // move position override
bool g_bLLM_OverrideMove = false;  // whether to override movement
int g_iLLM_TrackedBot = -1;        // which bot LLM controls (-1 = first found)
int g_iLLM_Seq = 0;               // STATE sequence number for request-response matching

// Player scoring (per-client index)
int g_iLLM_PlayerIncap[MAXPLAYERS+1];    // incap count this round
int g_iLLM_PlayerPinned[MAXPLAYERS+1];   // pinned count this round
int g_iLLM_PlayerRevive[MAXPLAYERS+1];   // revive/defib count this round
int g_iLLM_PlayerHeal[MAXPLAYERS+1];     // heal teammate count this round
int g_iLLM_PlayerSIKills[MAXPLAYERS+1];  // SI kill count this round
bool g_bLLM_WasPinned[MAXPLAYERS+1];     // previous pin state for edge detection

// LLM Terrain Awareness
bool g_bLLM_HasNarrowPassage = false;
bool g_bLLM_HasLedges = false;
bool g_bLLM_HasAlarmCars = false;
bool g_bLLM_HasCrescendo = false;
bool g_bLLM_IsFinale = false;

// LLM Recent Events (with TTL)
char g_sLLM_RecentEvents[LLM_RECENT_EVENTS_MAX][128];
float g_fLLM_EventTime[LLM_RECENT_EVENTS_MAX];
int g_iLLM_EventIndex = 0;
int g_iLLM_EventCount = 0;
#define LLM_EVENT_TTL 5.0


#define MAXENTITIES 					2048
#define MAXSURVIVORS 					32

#define MAP_SCAN_TIMER_INTERVAL			2.0
#define MAX_MAP_RANGE_SQR				1073741824.0 // 32768, if interested
#define CODE_BUFFER_LENGTH				1006

#define BOT_BOOMER_AVOID_RADIUS_SQR		65536.0 	// 256
#define BOT_GRENADE_CHECK_RADIUS_SQR	147456.0 	// 384
#define BOT_CMD_MOVE_INTERVAL 			0.5

#define HUMAN_HEIGHT					71.0
#define HUMAN_HALF_HEIGHT				35.5

#define DEBUG_NONE						0
#define DEBUG_NAV						(1 << 0)
#define DEBUG_MOVE						(1 << 1)
#define DEBUG_SCAVENGE					(1 << 2)
#define DEBUG_MISC						(1 << 3)
//#define DEBUG_HUD						(1 << 4)
#define DEBUG_WEP_DATA					(1 << 5)

#define HUD_FLAG_ALIGN_LEFT				256
#define HUD_FLAG_TEXT					8192
#define HUD_FLAG_NOTVISIBLE				16384

#define FLAG_NOITEM						0
#define FLAG_ITEM						(1 << 0)
#define FLAG_WEAPON						(1 << 1)
#define FLAG_CSS						(1 << 2)
#define FLAG_AMMO						(1 << 3)
#define FLAG_UPGRADE					(1 << 4)
#define FLAG_CARRY						(1 << 5)
#define FLAG_MELEE						(1 << 6)
#define FLAG_TIER1						(1 << 7)
#define FLAG_TIER2						(1 << 8)
#define FLAG_TIER3						(1 << 9)
#define FLAG_PISTOL						(1 << 10)
#define FLAG_PISTOL_EXTRA 				(1 << 11)
#define FLAG_SMG						(1 << 12)
#define FLAG_SHOTGUN					(1 << 13)
#define FLAG_ASSAULT					(1 << 14)
#define FLAG_SNIPER						(1 << 15)
#define FLAG_CHAINSAW					(1 << 16)
#define FLAG_GL							(1 << 17)
#define FLAG_M60						(1 << 18)
#define FLAG_HEAL						(1 << 19)
#define FLAG_GREN						(1 << 20)
#define FLAG_DEFIB						(1 << 21)
#define FLAG_MEDKIT						(1 << 22)
#define FLAG_LASER						(1 << 23)

#define STATE_NEEDS_COVER				(1 << 0)
#define STATE_NEEDS_AMMO				(1 << 1)
#define STATE_NEEDS_WEAPON				(1 << 2)
#define STATE_WOULD_HEAL				(1 << 3)
#define STATE_WOULD_PICK_MELEE 			(1 << 4)
#define STATE_WOULD_PICK_T3				(1 << 5)

#define PICKUP_PIPE						(1 << 0)
#define PICKUP_MOLO						(1 << 1)
#define PICKUP_BILE						(1 << 2)
#define PICKUP_MEDKIT					(1 << 3)
#define PICKUP_DEFIB					(1 << 4)
#define PICKUP_UPGRADE					(1 << 5)		//deployable ammo boxes
#define PICKUP_PILLS					(1 << 6)
#define PICKUP_ADREN					(1 << 7)
#define PICKUP_LASER					(1 << 8)
#define PICKUP_AMMOPACK 				(1 << 9)		//flame/frag rounds from deployed boxes
#define PICKUP_AMMO						(1 << 10)
#define PICKUP_CHAINSAW					(1 << 11)
#define PICKUP_SECONDARY 				(1 << 12)
#define PICKUP_PRIMARY					(1 << 13)

//0: Disable, 1: Pipe Bomb, 2: Molotov, 4: Bile Bomb, 8: Medkit, 16: Defibrillator, 32: UpgradePack, 64: Pain Pills
//128: Adrenaline, 256: Laser Sights, 512: Ammopack, 1024: Ammopile, 2048: Chainsaw, 4096: Secondary Weapons, 8192: Primary Weapons

static char IBWeaponName[56][32];

static const char IBVScriptCommands[][] =
{
	"MOVE",
	"USE"
};

enum
{
	L4D_SURVIVOR_NICK			= 1,
	L4D_SURVIVOR_ROCHELLE 		= 2,
	L4D_SURVIVOR_COACH			= 3,
	L4D_SURVIVOR_ELLIS			= 4,
	L4D_SURVIVOR_BILL			= 5,
	L4D_SURVIVOR_ZOEY			= 6,
	L4D_SURVIVOR_FRANCIS		= 7,
	L4D_SURVIVOR_LOUIS 			= 8
}

enum
{
	L4D_WEAPON_PREFERENCE_ASSAULTRIFLE 	= 1,
	L4D_WEAPON_PREFERENCE_SHOTGUN		= 2,
	L4D_WEAPON_PREFERENCE_SNIPERRIFLE	= 3,
	L4D_WEAPON_PREFERENCE_SMG			= 4,
	L4D_WEAPON_PREFERENCE_SECONDARY		= 5
}

/*============ IN-GAME CONVARS =======================================================================*/
static ConVar g_hCvar_GameDifficulty; 
static ConVar g_hCvar_SurvivorLimpHealth;
static ConVar g_hCvar_TankRockHealth;
static ConVar g_hCvar_ChaseBileRange;
static ConVar g_hCvar_ServerGravity;
static ConVar g_hCvar_BileCoverDuration_Bot;
static ConVar g_hCvar_BileCoverDuration_PZ;
static ConVar g_hCvar_ShovePenaltyMin_Coop;
static ConVar g_hCvar_ShovePenaltyMin_Versus;

static ConVar g_hCvar_MaxMeleeSurvivors; 
static ConVar g_hCvar_BotsShootThrough;
static ConVar g_hCvar_BotsFriendlyFire;
static ConVar g_hCvar_BotsDisabled;
static ConVar g_hCvar_BotsDontShoot;
static ConVar g_hCvar_BotsVomitBlindTime;

static char g_sCvar_GameDifficulty[12]; 
static int g_iCvar_SurvivorLimpHealth; 
static int g_iCvar_TankRockHealth; 
static float g_fCvar_ChaseBileRange; 
static float g_fCvar_ServerGravity; 
static float g_fCvar_BileCoverDuration_Bot;
static float g_fCvar_BileCoverDuration_PZ;
static int g_iCvar_ShovePenaltyMin;

static int g_iCvar_MaxMeleeSurvivors; 
static bool g_bCvar_BotsShootThrough;
static bool g_bCvar_BotsFriendlyFire;
static bool g_bCvar_BotsDisabled;
static bool g_bCvar_BotsDontShoot;
static float g_fCvar_BotsVomitBlindTime;

/*============ AMMO RELATED CONVARS =================================================================*/
static ConVar g_hCvar_MaxAmmo_Pistol;
static ConVar g_hCvar_MaxAmmo_AssaultRifle;
static ConVar g_hCvar_MaxAmmo_SMG;
static ConVar g_hCvar_MaxAmmo_M60;
static ConVar g_hCvar_MaxAmmo_Shotgun;
static ConVar g_hCvar_MaxAmmo_AutoShotgun;
static ConVar g_hCvar_MaxAmmo_HuntRifle;
static ConVar g_hCvar_MaxAmmo_SniperRifle;
static ConVar g_hCvar_MaxAmmo_PipeBomb;
static ConVar g_hCvar_MaxAmmo_Molotov;
static ConVar g_hCvar_MaxAmmo_VomitJar;
static ConVar g_hCvar_MaxAmmo_PainPills;
static ConVar g_hCvar_MaxAmmo_GrenLauncher;
static ConVar g_hCvar_MaxAmmo_Adrenaline;
static ConVar g_hCvar_MaxAmmo_Chainsaw;
static ConVar g_hCvar_MaxAmmo_AmmoPack;
static ConVar g_hCvar_MaxAmmo_Medkit;
static ConVar g_hCvar_Ammo_Type_Override;

static int g_iCvar_MaxAmmo_Pistol;
static int g_iCvar_MaxAmmo_AssaultRifle;
static int g_iCvar_MaxAmmo_SMG;
static int g_iCvar_MaxAmmo_M60;
static int g_iCvar_MaxAmmo_Shotgun;
static int g_iCvar_MaxAmmo_AutoShotgun;
static int g_iCvar_MaxAmmo_HuntRifle;
static int g_iCvar_MaxAmmo_SniperRifle;
static int g_iCvar_MaxAmmo_PipeBomb;
static int g_iCvar_MaxAmmo_Molotov;
static int g_iCvar_MaxAmmo_VomitJar;
static int g_iCvar_MaxAmmo_PainPills;
static int g_iCvar_MaxAmmo_GrenLauncher;
static int g_iCvar_MaxAmmo_Adrenaline;
static int g_iCvar_MaxAmmo_Chainsaw;
static int g_iCvar_MaxAmmo_AmmoPack;
static int g_iCvar_MaxAmmo_Medkit;
static char g_sCvar_Ammo_Type_Override[32];

/*============ MELEE RELATED CONVARS =================================================================*/
static ConVar g_hCvar_ImprovedMelee_MaxCount;

static ConVar g_hCvar_ImprovedMelee_Enabled;
static ConVar g_hCvar_ImprovedMelee_SwitchCount;
static ConVar g_hCvar_ImprovedMelee_SwitchRange;
static ConVar g_hCvar_ImprovedMelee_ApproachRange;
static ConVar g_hCvar_ImprovedMelee_AimRange;
static ConVar g_hCvar_ImprovedMelee_AttackRange;
static ConVar g_hCvar_ImprovedMelee_ShoveChance;

static bool g_bCvar_ImprovedMelee_Enabled;
static int g_iCvar_ImprovedMelee_SwitchCount; 
static int g_iCvar_ImprovedMelee_ShoveChance; 
static float g_fCvar_ImprovedMelee_SwitchRange; 
static float g_fCvar_ImprovedMelee_ApproachRange;
static float g_fCvar_ImprovedMelee_AimRange_Sqr;
static float g_fCvar_ImprovedMelee_AttackRange;

static ConVar g_hCvar_ImprovedMelee_ChainsawLimit;
static ConVar g_hCvar_ImprovedMelee_SwitchCount2;

static int g_iCvar_ImprovedMelee_ChainsawLimit;
static int g_iCvar_ImprovedMelee_SwitchCount2;
/*============ TARGET SELECTION CONVARS ==================================================================*/
ConVar g_hCvar_TargetSelection_Enabled;
ConVar g_hCvar_TargetSelection_ShootRange;
ConVar g_hCvar_TargetSelection_ShootRange2;
ConVar g_hCvar_TargetSelection_ShootRange3;
ConVar g_hCvar_TargetSelection_ShootRange4;
ConVar g_hCvar_TargetSelection_IgnoreDociles;

bool g_bCvar_TargetSelection_Enabled;
float g_fCvar_TargetSelection_ShootRange;
float g_fCvar_TargetSelection_ShootRange2;
float g_fCvar_TargetSelection_ShootRange3;
float g_fCvar_TargetSelection_ShootRange4;
bool g_bCvar_TargetSelection_IgnoreDociles;
/*----------------------------------------------------------------------------------------------------*/
static ConVar g_hCvar_Vision_FieldOfView;
static float g_fCvar_Vision_FieldOfView;

static ConVar g_hCvar_Vision_NoticeTimeScale;
static float g_fCvar_Vision_NoticeTimeScale;

/*============ TANK RELATED CONVARS ==================================================================*/
static ConVar g_hCvar_TankRock_ShootEnabled;
static ConVar g_hCvar_TankRock_ShootRange;
static bool g_bCvar_TankRock_ShootEnabled;
static float g_fCvar_TankRock_ShootRange_Sqr;
/*============ SAVE RELATED CONVARS ==================================================================*/
static ConVar g_hCvar_AutoShove_Enabled;
static int g_iCvar_AutoShove_Enabled;
/*----------------------------------------------------------------------------------------------------*/
static ConVar g_hCvar_FireBash_Chance1;
static ConVar g_hCvar_FireBash_Chance2;
static int g_iCvar_FireBash_Chance1;
static int g_iCvar_FireBash_Chance2;
/*----------------------------------------------------------------------------------------------------*/
static ConVar g_hCvar_HelpPinnedFriend_Enabled;
static ConVar g_hCvar_HelpPinnedFriend_ShootRange;
static ConVar g_hCvar_HelpPinnedFriend_ShoveRange;
static int g_iCvar_HelpPinnedFriend_Enabled;
//static float g_fCvar_HelpPinnedFriend_ShootRange;
static float g_fCvar_HelpPinnedFriend_ShootRange_Sqr;
static float g_fCvar_HelpPinnedFriend_ShoveRange_Sqr;
/*============ WEAPON RELATED CONVARS ================================================================*/
static ConVar g_hCvar_BotWeaponPreference_ForceMagnum;
static bool g_bCvar_BotWeaponPreference_ForceMagnum;

static ConVar g_hCvar_BotWeaponPreference_Rochelle;
static ConVar g_hCvar_BotWeaponPreference_Zoey; 
static ConVar g_hCvar_BotWeaponPreference_Ellis; 
static ConVar g_hCvar_BotWeaponPreference_Coach; 
static ConVar g_hCvar_BotWeaponPreference_Francis; 
static ConVar g_hCvar_BotWeaponPreference_Nick;
static ConVar g_hCvar_BotWeaponPreference_Louis; 
static ConVar g_hCvar_BotWeaponPreference_Bill;

static int g_iCvar_BotWeaponPreference_Rochelle;
static int g_iCvar_BotWeaponPreference_Zoey;
static int g_iCvar_BotWeaponPreference_Ellis; 
static int g_iCvar_BotWeaponPreference_Coach;
static int g_iCvar_BotWeaponPreference_Francis;
static int g_iCvar_BotWeaponPreference_Nick;
static int g_iCvar_BotWeaponPreference_Louis;
static int g_iCvar_BotWeaponPreference_Bill;
/*----------------------------------------------------------------------------------------------------*/
static ConVar g_hCvar_SwapSameTypePrimaries_T1;
static ConVar g_hCvar_SwapSameTypePrimaries_T2;

static bool g_bCvar_SwapSameTypePrimaries_T1;
static bool g_bCvar_SwapSameTypePrimaries_T2;
/*------------ TIER 3 --------------------------------------------------------------------------------*/
static ConVar g_hCvar_MaxWeaponTier3_M60;
static ConVar g_hCvar_MaxWeaponTier3_GLauncher;
static ConVar g_hCvar_T3_Refill;

static int g_iCvar_MaxWeaponTier3_M60;
static int g_iCvar_MaxWeaponTier3_GLauncher;
static int g_iCvar_T3_Refill;

/*============ GRENADE RELATED CONVARS ===============================================================*/
static ConVar g_hCvar_GrenadeThrow_Enabled;
static ConVar g_hCvar_GrenadeThrow_GrenadeTypes;
static ConVar g_hCvar_GrenadeThrow_ThrowRange;
static ConVar g_hCvar_GrenadeThrow_HordeSize; 
static ConVar g_hCvar_GrenadeThrow_NextThrowTime1;
static ConVar g_hCvar_GrenadeThrow_NextThrowTime2;
/*----------------------------------------------------------------------------------------------------*/
static bool g_bCvar_GrenadeThrow_Enabled;
static int g_iCvar_GrenadeThrow_GrenadeTypes; 
static float g_fCvar_GrenadeThrow_ThrowRange; 
static float g_fCvar_GrenadeThrow_ThrowRange_Sqr; 
static float g_fCvar_GrenadeThrow_ThrowRange_NoVisCheck; 
static float g_fCvar_GrenadeThrow_HordeSize; 
static float g_fCvar_GrenadeThrow_NextThrowTime1;
static float g_fCvar_GrenadeThrow_NextThrowTime2;
/*----------------------------------------------------------------------------------------------------*/
static ConVar g_hCvar_SwapSameTypeGrenades;
static bool g_bCvar_SwapSameTypeGrenades;

/*============ DEFIB RELATED CONVARS =================================================================*/
static ConVar g_hCvar_DefibRevive_Enabled; 
static ConVar g_hCvar_DefibRevive_ScanDist; 

static bool g_bCvar_DefibRevive_Enabled; 
static float g_fCvar_DefibRevive_ScanDist_Sqr;

/*============ ITEM SCAVENGE RELATED CONVARS =========================================================*/
static ConVar g_hCvar_ItemScavenge_Models; 
static ConVar g_hCvar_ItemScavenge_Items; 
static ConVar g_hCvar_ItemScavenge_ApproachRange; 
static ConVar g_hCvar_ItemScavenge_ApproachVisibleRange; 
static ConVar g_hCvar_ItemScavenge_PickupRange; 
static ConVar g_hCvar_ItemScavenge_MapSearchRange; 
static ConVar g_hCvar_ItemScavenge_NoHumansRangeMultiplier; 

static int g_iCvar_ItemScavenge_Models;
static int g_iCvar_ItemScavenge_Items;
static float g_fCvar_ItemScavenge_ApproachRange;
static float g_fCvar_ItemScavenge_ApproachVisibleRange;
static float g_fCvar_ItemScavenge_PickupRange;
static float g_fCvar_ItemScavenge_PickupRange_Sqr;
static float g_fCvar_ItemScavenge_MapSearchRange_Sqr;
static float g_fCvar_ItemScavenge_NoHumansRangeMultiplier;

/*============ WITCH RELATED CONVARS =========================================================*/
static ConVar g_hCvar_WitchBehavior_WalkWhenNearby;
static ConVar g_hCvar_WitchBehavior_AllowCrowning;

static float g_fCvar_WitchBehavior_WalkWhenNearby;
static int g_iCvar_WitchBehavior_AllowCrowning;

/*============ PERFOMANCE RELATED CONVARS =========================================================*/
static ConVar g_hCvar_NextProcessTime;
static float g_fCvar_NextProcessTime;

/*============ MISC CONVARS =========================================================*/
static ConVar g_hCvar_AcidEvasion;
static ConVar g_hCvar_SwitchOffCSSWeapons;
// static ConVar g_hCvar_KeepMovingInCombat;
static ConVar g_hCvar_ChargerEvasion;
static ConVar g_hCvar_DeployUpgradePacks;
static ConVar g_hCvar_DontSwitchToPistol;
static ConVar g_hCvar_TakeCoverFromRocks;
static ConVar g_hCvar_AvoidTanksWithProp;
static ConVar g_hCvar_NoFallDmgOnLadderFail;
static ConVar g_hCvar_HasEnoughAmmoRatio;

static ConVar g_hCvar_Nightmare;

static bool g_bCvar_AcidEvasion;
static bool g_bCvar_SwitchOffCSSWeapons;
static bool g_bCvar_ChargerEvasion;
static bool g_bCvar_DeployUpgradePacks;
static bool g_bCvar_DontSwitchToPistol;
static bool g_bCvar_TakeCoverFromRocks;
static bool g_bCvar_AvoidTanksWithProp;
static bool g_bCvar_NoFallDmgOnLadderFail;
static float g_fCvar_HasEnoughAmmoRatio;

static bool g_bCvar_Nightmare;

/*============ VARIABLES =========================================================*/
static bool g_bClient_IsLookingAtPosition[MAXPLAYERS+1];
static bool g_bClient_IsFiringWeapon[MAXPLAYERS+1];
static float g_fClient_ThinkFunctionDelay[MAXPLAYERS+1];

// -------------------------

static bool g_bBot_IsInCombat[MAXSURVIVORS+1];
static bool g_bBot_IsAvailable[MAXSURVIVORS+1];

// -------------------------

static bool g_bBot_IsWitchHarasser[MAXSURVIVORS+1];
static bool g_bBot_ForceBash[MAXSURVIVORS+1];
static bool g_bBot_ForceSwitchWeapon[MAXSURVIVORS+1];
static bool g_bBot_ForceWeaponReload[MAXSURVIVORS+1];
static bool g_bBot_WasUsingLadder[MAXSURVIVORS+1];

static bool g_bBot_IsFriendNearBoomer[MAXSURVIVORS+1];
static bool g_bBot_IsFriendNearThrowArea[MAXSURVIVORS+1];

static float g_fBot_NextPressAttackTime[MAXSURVIVORS+1];
static float g_fBot_VomitBlindedTime[MAXSURVIVORS+1];
static float g_fBot_PinnedReactTime[MAXSURVIVORS+1];
static float g_fBot_NextWeaponRangeSwitchTime[MAXSURVIVORS+1];
static float g_fBot_NextMoveCommandTime[MAXSURVIVORS+1];
static float g_fBot_PreventFireTime[MAXSURVIVORS+1];

static float g_fBot_BlockWeaponSwitchTime[MAXSURVIVORS+1];
static float g_fBot_BlockWeaponReloadTime[MAXSURVIVORS+1];

static int g_iBot_TargetInfected[MAXSURVIVORS+1];
static int g_iBot_WitchTarget[MAXSURVIVORS+1];
static int g_iBot_DefibTarget[MAXSURVIVORS+1];
static int g_iBot_IncapacitatedFriend[MAXSURVIVORS+1];

static int g_iBot_PinnedFriend[MAXSURVIVORS+1];
static int g_iBot_PinnedFriend_Attacker[MAXSURVIVORS+1];

static int g_iBot_TankTarget[MAXSURVIVORS+1];
static int g_iBot_TankRock[MAXSURVIVORS+1];
static int g_iBot_TankProp[MAXSURVIVORS+1];

static int g_iBot_ScavengeItem[MAXSURVIVORS+1];
static float g_fBot_ScavengeItemDist[MAXSURVIVORS+1];
static float g_fBot_NextScavengeItemScanTime[MAXSURVIVORS+1];

static float g_fBot_MeleeAttackTime[MAXSURVIVORS+1];
static float g_fBot_MeleeApproachTime[MAXSURVIVORS+1];

static int g_iBot_Grenade_ThrowTarget[MAXSURVIVORS+1];
static float g_fBot_Grenade_ThrowPos[MAXSURVIVORS+1][3];
static float g_fBot_Grenade_AimPos[MAXSURVIVORS+1][3];

static float g_fBot_Grenade_NextThrowTime;
static float g_fBot_Grenade_NextThrowTime_Molotov;

static float g_fBot_LookPosition[MAXSURVIVORS+1][3];
static float g_fBot_LookPosition_Duration[MAXSURVIVORS+1];

static float g_fBot_MovePos_Position[MAXSURVIVORS+1][3];
static float g_fBot_MovePos_Duration[MAXSURVIVORS+1];
static int g_iBot_MovePos_Priority[MAXSURVIVORS+1];
static float g_fBot_MovePos_Tolerance[MAXSURVIVORS+1];
static bool g_bBot_MovePos_IgnoreDamaging[MAXSURVIVORS+1];
static char g_sBot_MovePos_Name[MAXSURVIVORS+1][64];

static int g_iBot_NearbyFriends[MAXSURVIVORS+1];
static int g_iBot_NearbyInfectedCount[MAXSURVIVORS+1]; 
static int g_iBot_NearestInfectedCount[MAXSURVIVORS+1]; 
static int g_iBot_ThreatInfectedCount[MAXSURVIVORS+1]; 
static int g_iBot_GrenadeInfectedCount[MAXSURVIVORS+1];

// -------------------------

#define VISION_CHECK_MAXDIST		16777216.0
#define VISION_CHECK_LOSETIME		15.0

#define VISION_MEMORY_NOFOV			0
#define VISION_MEMORY_FOV			1

#define VISION_STATE_NOCONTACT		0
#define VISION_STATE_NOTICED		1
#define VISION_STATE_SPOTTED		2
#define VISION_STATE_LOSTSIGHT		3

// -------------------------

static int g_iBot_VisionMemory_State[MAXSURVIVORS+1][2][MAXENTITIES+1];
static float g_fBot_VisionMemory_Time[MAXSURVIVORS+1][2][MAXENTITIES+1];

// -------------------------

static int g_iInfectedBot_CurrentVictim[MAXSURVIVORS+1];
static bool g_bInfectedBot_IsThrowing[MAXSURVIVORS+1];
static float g_fInfectedBot_CoveredInVomitTime[MAXSURVIVORS+1];

// ----------------------------------------------------------------------------------------------------

static bool g_bMapStarted;
static char g_sCurrentMapName[128];
static bool g_bCutsceneIsPlaying;
static bool g_bTeamHasHumanPlayer;
static int g_iHasLeft4Bots;

static int g_iBotProcessing_ProcessedCount;
static float g_fBotProcessing_NextProcessTime;
static bool g_bBotProcessing_IsProcessed[MAXSURVIVORS+1];

// ----------------------------------------------------------------------------------------------------
// DEBUG / TESTING
// ----------------------------------------------------------------------------------------------------
static ConVar g_hCvar_Debug;
static int g_iCvar_Debug;
static int g_iCvar_DebugClient;

//static int g_iTester;
//static int g_iTeamLeader;
//static int g_iTimesPostponed;
//static int g_iTestSubject;

//static Handle g_hDebugHUDTimer;

Profiler g_pProf;

// ----------------------------------------------------------------------------------------------------
// CLIENT GLOBAL DATA
// ----------------------------------------------------------------------------------------------------
static float g_fClientEyePos[MAXPLAYERS+1][3];
static float g_fClientEyeAng[MAXPLAYERS+1][3];
static float g_fClientAbsOrigin[MAXPLAYERS+1][3];
static float g_fClientCenteroid[MAXPLAYERS+1][3];
static int g_iClientNavArea[MAXPLAYERS+1];
static int g_iClientInventory[MAXPLAYERS+1][6];
static int g_iClientInvFlags[MAXPLAYERS+1];
//static int g_iClientState[MAXPLAYERS+1];

// ----------------------------------------------------------------------------------------------------
// WEAPON GLOBAL DATA
// ----------------------------------------------------------------------------------------------------
static bool g_bInitCheckCases;
static bool g_bInitItemFlags;
static bool g_bInitMaxAmmo;
static bool g_bInitWeaponMap;
static bool g_bInitMeleeIDs;
static bool g_bInitMeleePrefs;

static bool g_bIsSemiAuto[56];
static int g_iWeaponID[MAXENTITIES+1];
static int g_iMeleeID[MAXENTITIES+1];
static int g_iMeleePreference[17];
static int g_iItemFlags[MAXENTITIES+1];
static int g_iMaxAmmo[56];
static int g_iWeaponTier[56];
static int g_iWeapon_Clip1[MAXENTITIES+1];
static int g_iWeapon_MaxAmmo[MAXENTITIES+1]; 
static int g_iWeapon_AmmoLeft[MAXENTITIES+1];
static int g_iItem_Used[MAXENTITIES+1]; // To fix bots grabbing same ammo upgrade repeatedly

static int g_iWeapon_PrimaryAttackHook[MAXENTITIES+1];

static Handle g_hCheckWeaponTimer;

// ----------------------------------------------------------------------------------------------------
// LOOKUP HASH MAPS
// ----------------------------------------------------------------------------------------------------
StringMap g_hItemFlagMap;
StringMap g_hWeaponMap;
StringMap g_hWeaponMdlMap;
StringMap g_hWeaponToIDMap;
StringMap g_hCheckCases;
StringMap g_hMeleeIDs;
StringMap g_hMeleeMdlToID;
StringMap g_hMeleePref;

// ----------------------------------------------------------------------------------------------------
// DATA FILES
// ----------------------------------------------------------------------------------------------------
static int g_iDataFileSection;
static int g_iDataFileValueID;

// ----------------------------------------------------------------------------------------------------
// VSCRIPT
// ----------------------------------------------------------------------------------------------------
static VScriptFunction g_vsCommand;
static VScriptExecute g_vsPathWithin;
static bool g_bInitPathWithin;

// ----------------------------------------------------------------------------------------------------
// ENTITY ARRAYLISTS
// ----------------------------------------------------------------------------------------------------
static ArrayList g_hMeleeList;
static ArrayList g_hPistolList;
static ArrayList g_hSMGList;
static ArrayList g_hShotgunT1List;
static ArrayList g_hShotgunT2List;
static ArrayList g_hAssaultRifleList;
static ArrayList g_hSniperRifleList;
static ArrayList g_hTier3List;
static ArrayList g_hGrenadeList;
static ArrayList g_hFirstAidKitList;
static ArrayList g_hDefibrillatorList;
static ArrayList g_hUpgradePackList;
static ArrayList g_hPainPillsList;
static ArrayList g_hAdrenalineList;

static ArrayList g_hAmmopileList;
static ArrayList g_hLaserSightList;
static ArrayList g_hDeployedAmmoPacks;
static ArrayList g_hForbiddenItemList;
static ArrayList g_hWeaponsToCheckLater;

static ArrayList g_hWitchList;

// ----------------------------------------------------------------------------------------------------
// PREVENT REPEATED UNSUCCESSFUL PATH DISTANCE CALCULATION
// ----------------------------------------------------------------------------------------------------
static ArrayList g_hBadPathEntities;
static Handle g_hClearBadPathTimer;

// ----------------------------------------------------------------------------------------------------
// CHARACTER MODEL BONES
// ----------------------------------------------------------------------------------------------------
static const char g_sBoneNames_Old[][] =
{
	"ValveBiped.Bip01_Head1", 
	"ValveBiped.Bip01_Spine", 
	"ValveBiped.Bip01_Spine1", 
	"ValveBiped.Bip01_Spine2", 
	"ValveBiped.Bip01_Spine4", 
	"ValveBiped.Bip01_L_UpperArm", 
	"ValveBiped.Bip01_L_Forearm", 
	"ValveBiped.Bip01_L_Hand", 
	"ValveBiped.Bip01_R_UpperArm", 
	"ValveBiped.Bip01_R_Forearm", 
	"ValveBiped.Bip01_R_Hand", 
	"ValveBiped.Bip01_Pelvis", 
	"ValveBiped.Bip01_L_Thigh", 
	"ValveBiped.Bip01_L_Knee", 
	"ValveBiped.Bip01_L_Foot", 
	"ValveBiped.Bip01_R_Thigh", 
	"ValveBiped.Bip01_R_Knee", 
	"ValveBiped.Bip01_R_Foot"
};
static const char g_sBoneNames_New[][] =
{
	"bip_head",
	"bip_spine_0",
	"bip_spine_1",
	"bip_spine_2",
	"bip_spine_3",
	"bip_upperArm_L",
	"bip_lowerArm_L",
	"bip_hand_L",
	"bip_upperArm_R",
	"bip_lowerArm_R",
	"bip_hand_R",
	"bip_pelvis",
	"bip_hip_L",
	"bip_knee_L",
	"bip_foot_L",
	"bip_hip_R",
	"bip_knee_R",
	"bip_foot_R"
};

static bool g_bLateLoad;
static bool g_bExtensionActions;
static bool g_bPluginVScript;

public APLRes AskPluginLoad2(Handle myself, bool late, char[] error, int err_max)
{
	EngineVersion test = GetEngineVersion();
	if (test != Engine_Left4Dead2)
	{
		strcopy(error, err_max, "Plugin only supports Left 4 Dead 2.");
		return APLRes_SilentFailure;
	}

	g_bLateLoad = late;
	return APLRes_Success;
}

public void OnPluginStart()
{
	// ----------------------------------------------------------------------------------------------------
	// GAMEDATA RELATED
	// ----------------------------------------------------------------------------------------------------
	Handle hGameConfig = LoadGameConfigFile("l4d2_improved_bots");
	if (!hGameConfig)SetFailState("Failed to find 'l4d2_improved_bots.txt' game config.");

	CreateAllSDKCalls(hGameConfig);
	CreateAllDetours(hGameConfig);

	delete hGameConfig;
	
	// ----------------------------------------------------------------------------------------------------
	// DATA / PREFERENCES
	// ----------------------------------------------------------------------------------------------------
	
	ParseDataFile();

	// ----------------------------------------------------------------------------------------------------
	// EVENT HOOKS
	// ----------------------------------------------------------------------------------------------------
	HookEvent("round_start", 			Event_OnRoundStart);
	HookEvent("round_end", 			Event_OnRoundEnd);
	HookEvent("mission_lost", 			Event_OnMissionLost);
	HookEvent("map_transition", 			Event_OnMapTransition);

	HookEvent("weapon_fire", 			Event_OnWeaponFire);
	HookEvent("player_death", 			Event_OnPlayerDeath);
	HookEvent("heal_success", 			Event_OnHealSuccess);
	HookEvent("player_use",				Event_OnPlayerUse);
	HookEvent("ability_use",			Event_OnAbilityUse);
	
	HookEvent("player_incapacitated_start",	Event_OnIncap);
	HookEvent("revive_success",			Event_OnRevive);
	HookEvent("defibrillator_used",		Event_OnRevive);
	
	HookEvent("lunge_pounce", 			Event_OnSurvivorGrabbed);
	HookEvent("tongue_grab", 			Event_OnSurvivorGrabbed);
	HookEvent("jockey_ride", 			Event_OnSurvivorGrabbed);
	HookEvent("charger_carry_start", 	Event_OnSurvivorGrabbed);

	HookEvent("charger_charge_start",	Event_OnChargeStart);
	
	HookEvent("witch_harasser_set", 	Event_OnWitchHaraserSet);
	
	RegAdminCmd("sm_ibcvars",	CmdDumpCvars,	ADMFLAG_GENERIC, "Dump some cvars");
	//RegAdminCmd("sm_ibsubject",	CmdSetTestSubj, ADMFLAG_GENERIC, "Set player to test against");

	// ----------------------------------------------------------------------------------------------------
	// CONSOLE VARIABLES
	// ----------------------------------------------------------------------------------------------------
	CreateAndHookConVars();
	AutoExecConfig(true, "l4d2_improved_bots");

	// ----------------------------------------------------------------------------------------------------
	// MISC
	// ----------------------------------------------------------------------------------------------------	
	if (g_bLateLoad)
	{
		for (int i = 1; i <= MaxClients; i++)
		{
			if (!IsClientInGame(i))continue;
			OnClientJoinServer(i);
		}
	}
	
	g_pProf = CreateProfiler();
	
	if (!g_bInitItemFlags)
	{
		InitItemFlagMap();
		PrintToServer("OnPluginStart: init g_hItemFlagMap");
	}

	// LLM Module Init
	LLM_Init();
}

void CreateAndHookConVars()
{
	g_hCvar_GameDifficulty 							= FindConVar("z_difficulty");
	g_hCvar_SurvivorLimpHealth 						= FindConVar("survivor_limp_health");
	g_hCvar_TankRockHealth 							= FindConVar("z_tank_throw_health");
	g_hCvar_ChaseBileRange							= FindConVar("z_notice_it_range");
	g_hCvar_ServerGravity							= FindConVar("sv_gravity");
	g_hCvar_BileCoverDuration_Bot					= FindConVar("vomitjar_duration_infected_bot");
	g_hCvar_BileCoverDuration_PZ					= FindConVar("vomitjar_duration_infected_pz");
	g_hCvar_ShovePenaltyMin_Coop					= FindConVar("z_gun_swing_coop_min_penalty");
	g_hCvar_ShovePenaltyMin_Versus					= FindConVar("z_gun_swing_vs_min_penalty");

	g_hCvar_MaxMeleeSurvivors 						= FindConVar("sb_max_team_melee_weapons");
	g_hCvar_BotsShootThrough 						= FindConVar("sb_allow_shoot_through_survivors");
	g_hCvar_BotsFriendlyFire 						= FindConVar("sb_friendlyfire");
	g_hCvar_BotsDisabled 							= FindConVar("sb_stop");
	g_hCvar_BotsDontShoot 							= FindConVar("sb_dont_shoot");
	g_hCvar_BotsVomitBlindTime 						= FindConVar("sb_vomit_blind_time");

	g_hCvar_MaxAmmo_Pistol							= FindConVar("ammo_pistol_max");
	g_hCvar_MaxAmmo_AssaultRifle					= FindConVar("ammo_assaultrifle_max");
	g_hCvar_MaxAmmo_SMG								= FindConVar("ammo_smg_max");
	g_hCvar_MaxAmmo_M60								= FindConVar("ammo_m60_max");
	g_hCvar_MaxAmmo_Shotgun							= FindConVar("ammo_shotgun_max");
	g_hCvar_MaxAmmo_AutoShotgun						= FindConVar("ammo_autoshotgun_max");
	g_hCvar_MaxAmmo_HuntRifle						= FindConVar("ammo_huntingrifle_max");
	g_hCvar_MaxAmmo_SniperRifle						= FindConVar("ammo_sniperrifle_max");
	g_hCvar_MaxAmmo_PipeBomb						= FindConVar("ammo_pipebomb_max");
	g_hCvar_MaxAmmo_Molotov							= FindConVar("ammo_molotov_max");
	g_hCvar_MaxAmmo_VomitJar						= FindConVar("ammo_vomitjar_max");
	g_hCvar_MaxAmmo_PainPills						= FindConVar("ammo_painpills_max");
	g_hCvar_MaxAmmo_GrenLauncher					= FindConVar("ammo_grenadelauncher_max");
	g_hCvar_MaxAmmo_Adrenaline						= FindConVar("ammo_adrenaline_max");
	g_hCvar_MaxAmmo_Chainsaw						= FindConVar("ammo_chainsaw_max");
	g_hCvar_MaxAmmo_AmmoPack						= FindConVar("ammo_ammo_pack_max");
	g_hCvar_MaxAmmo_Medkit							= FindConVar("ammo_firstaid_max");
	
	g_hCvar_Ammo_Type_Override 						= CreateConVar("ib_ammotype_override", "", "If your server has weapons with modified ammo types/amounts, put them here in a following format: \"weapon_id:ammo_max weapon_id:ammo_max ...\"", FCVAR_NOTIFY);

	g_hCvar_ImprovedMelee_Enabled 					= CreateConVar("ib_melee_enabled", "1", "Enables survivor bots' improved melee behaviour.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_ImprovedMelee_MaxCount 					= CreateConVar("ib_melee_max_team", "2", "The total number of melee weapons allowed on the team. <0: Bots never use melee>", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_SwitchCount 				= CreateConVar("ib_melee_switch_count", "4", "The nearby infected count required for bot to switch to their melee weapon.", FCVAR_NOTIFY, true, 1.0);
	g_hCvar_ImprovedMelee_SwitchRange 				= CreateConVar("ib_melee_switch_range", "200", "Range at which bot's target should be to switch to melee weapon.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_ApproachRange				= CreateConVar("ib_melee_approach_range", "250", "Range at which bot's target should be to approach it. <0: Disable Approaching>", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_AimRange 					= CreateConVar("ib_melee_aim_range", "125", "Range at which bot's target should be to start taking aim at it.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_AttackRange 				= CreateConVar("ib_melee_attack_range", "75", "Range at which bot's target should be to start attacking it.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_ShoveChance 				= CreateConVar("ib_melee_shove_chance", "3", "Chance for bot to bash target instead of attacking with melee. <0: Disable Bashing>", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_ImprovedMelee_ChainsawLimit 			= CreateConVar("ib_melee_chainsaw_limit", "1", "The total number of chainsaws allowed on the team. <0: Bots never use chainsaw>", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ImprovedMelee_SwitchCount2 				= CreateConVar("ib_melee_chainsaw_switch_count", "7", "The nearby infected count required for bot to switch to chainsaw.", FCVAR_NOTIFY, true, 1.0);

	g_hCvar_TargetSelection_Enabled					= CreateConVar("ib_targeting_enabled", "1", "Enables survivor bots' improved target selection.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_TargetSelection_ShootRange				= CreateConVar("ib_targeting_range", "1750", "Range at which target need to be for bots to start firing at it.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_TargetSelection_ShootRange2				= CreateConVar("ib_targeting_range_shotgun", "750", "Range at which target need to be for bots to start firing at it with shotgun.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_TargetSelection_ShootRange3				= CreateConVar("ib_targeting_range_sniperrifle", "2500", "Range at which target need to be for bots to start firing at it with sniper rifle.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_TargetSelection_ShootRange4				= CreateConVar("ib_targeting_range_pistol", "1000", "Range at which target need to be for bots to start firing at it with secondary weapon.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_TargetSelection_IgnoreDociles			= CreateConVar("ib_targeting_ignoredociles", "1", "If bots shouldn't target common infected that are currently not attacking survivors.", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_GrenadeThrow_Enabled 					= CreateConVar("ib_gren_enabled", "1", "Enables survivor bots throwing grenades.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_GrenadeThrow_GrenadeTypes				= CreateConVar("ib_gren_types", "7", "What grenades should survivor bots throw? <1: Pipe-Bomb, 2: Molotov, 4: Bile Bomb. Add numbers together.>", FCVAR_NOTIFY, true, 1.0, true, 7.0);
	g_hCvar_GrenadeThrow_ThrowRange					= CreateConVar("ib_gren_throw_range", "1500", "Range at which target needs to be for bot to throw grenade at it.", FCVAR_NOTIFY);
	g_hCvar_GrenadeThrow_HordeSize 					= CreateConVar("ib_gren_horde_size_multiplier", "5.0", "Infected count required to throw grenade Multiplier (Value * SurvivorCount).", FCVAR_NOTIFY, true, 1.0);
	g_hCvar_GrenadeThrow_NextThrowTime1 			= CreateConVar("ib_gren_next_throw_time_min", "15", "First number to pick to randomize next grenade throw time.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_GrenadeThrow_NextThrowTime2 			= CreateConVar("ib_gren_next_throw_time_max", "35", "Second number to pick to randomize next grenade throw time.", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_TankRock_ShootEnabled 					= CreateConVar("ib_shootattankrocks_enabled", "1", "Enables survivor bots shooting tank's thrown rocks.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_TankRock_ShootRange 					= CreateConVar("ib_shootattankrocks_range", "1500", "Range at which rock needs to be for bot to start shooting at it.", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_AutoShove_Enabled						= CreateConVar("ib_autoshove_enabled", "1", "Makes survivor bots automatically shove every nearby infected. <0: Disabled, 1: All infected, 2: Only if infected is behind them>", FCVAR_NOTIFY, true, 0.0, true, 2.0);

	g_hCvar_HelpPinnedFriend_Enabled				= CreateConVar("ib_help_pinned_enabled", "3", "Makes survivor bots force attack pinned survivor's SI if possible. <0: Disabled, 1: Shoot at attacker, 2: Shove the attacker if close enough. Add numbers together.>", FCVAR_NOTIFY, true, 0.0, true, 3.0);
	g_hCvar_HelpPinnedFriend_ShootRange				= CreateConVar("ib_help_pinned_shootrange", "1250", "Range at which bots will start firing at SI.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_HelpPinnedFriend_ShoveRange				= CreateConVar("ib_help_pinned_shoverange", "75", "Range at which bots will start to bash SI.", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_DefibRevive_Enabled						= CreateConVar("ib_defib_revive_enabled", "1", "Enable bots reviving dead players with defibrillators if they have one available.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_DefibRevive_ScanDist 					= CreateConVar("ib_defib_revive_distance", "2500", "Range at which survivor's dead body should be for bot to consider it reviveable.", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_FireBash_Chance1						= CreateConVar("ib_shove_chance_pump", "0", "Chance at which survivor bot may shove after firing a pump-action shotgun. <0: Disabled, 1: Always>", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_FireBash_Chance2						= CreateConVar("ib_shove_chance_css", "0", "Chance at which survivor bot may shove after firing a bolt-action sniper rifle. <0: Disabled, 1: Always>", FCVAR_NOTIFY, true, 0.0);
	
	g_hCvar_ItemScavenge_Items 						= CreateConVar("ib_grab_enabled", "16383", "Enable improved bot item scavenging for specified items.\n<0: Disabled, 1: Pipe Bomb, 2: Molotov, 4: Bile Bomb, 8: Medkit, 16: Defib, 32: UpgradePack, 64: Pills, 128: Adrenaline, 256: Laser Sights, 512: Ammopack, \n1024: Ammopile, 2048: Chainsaw, 4096: Secondary Weapons, 8192: Primary Weapons. Add numbers together>", FCVAR_NOTIFY, true, 0.0, true, 16383.0);
	g_hCvar_ItemScavenge_Models						= CreateConVar("ib_grab_models", "0", "If enabled, objects with certain models will be considered as scavengeable items.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_ItemScavenge_ApproachRange 				= CreateConVar("ib_grab_distance", "300", "Distance at which a not visible item should be for bot to move it.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ItemScavenge_ApproachVisibleRange 		= CreateConVar("ib_grab_visible_distance", "600", "Distance at which a visible item should be for bot to move it.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ItemScavenge_PickupRange 				= CreateConVar("ib_grab_pickup_distance", "90", "Distance at which item should be for bot to able to pick it up.", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ItemScavenge_MapSearchRange 			= CreateConVar("ib_grab_mapsearchdistance", "2000", "How close should the item be to the survivor bot to able to count it when searching?", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_ItemScavenge_NoHumansRangeMultiplier	= CreateConVar("ib_grab_nohumans_rangemultiplier", "2.0", "The bots' scavenge distance is multiplied to this value when there's no human players left in the team.", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_BotWeaponPreference_Nick 				= CreateConVar("ib_pref_nick", "1", "Bot Nick's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Rochelle 			= CreateConVar("ib_pref_rochelle", "1", "Bot Rochelle's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Coach 				= CreateConVar("ib_pref_coach", "2", "Bot Coach's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Ellis 				= CreateConVar("ib_pref_ellis", "3", "Bot Ellis's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Bill  				= CreateConVar("ib_pref_bill", "1", "Bot Bill's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Zoey 				= CreateConVar("ib_pref_zoey", "3", "Bot Zoey's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Francis 			= CreateConVar("ib_pref_francis", "2", "Bot Francis's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_Louis 				= CreateConVar("ib_pref_louis", "1", "Bot Louis's weapon preference. <0: Default, 1: Assault Rifle, 2: Shotgun, 3: Sniper Rifle, 4: SMG, 5: Secondary Weapon>", FCVAR_NOTIFY, true, 0.0, true, 5.0);
	g_hCvar_BotWeaponPreference_ForceMagnum 		= CreateConVar("ib_pref_magnums_only", "0", "If every survivor bot should only use magnum instead of regular pistol if possible.", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_HasEnoughAmmoRatio 						= CreateConVar("ib_hasenoughammo_ratio", "0.33", "If the survivor bot's primary ammo percentage is below this value, they'll consider replacing the current primary with the new one", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_SwapSameTypePrimaries_T1 				= CreateConVar("ib_mix_primaries_t1", "1", "If enabled, bots will try to not have a whole team consist of only SMGs or shotguns and pick up different ones if too much.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_SwapSameTypePrimaries_T2 				= CreateConVar("ib_mix_primaries_t2", "0", "Makes survivor bots change their primary weapon subtype if there's too much of the same one, Ex. change AK-47 to M16 or SPAS-12 to Autoshotgun.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_SwapSameTypeGrenades 					= CreateConVar("ib_mix_grenades", "1", "Makes survivor bots change their grenade type if there's too much of the same one, Ex. Pipe-Bomb to Molotov.", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_T3_Refill 								= CreateConVar("ib_t3_refill", "0", "Should bots pick up ammo when carrying a Tier 3 weapon? Keep disabled if your server does not allow that. <0: Disabled, 1: Grenade Launcher, 2: M60, 3: Both>", FCVAR_NOTIFY, true, 0.0, true, 3.0);
	g_hCvar_MaxWeaponTier3_M60 						= CreateConVar("ib_t3_limit_m60", "1", "The total number of M60s allowed on the team. <0: Bots never use M60>", FCVAR_NOTIFY, true, 0.0);
	g_hCvar_MaxWeaponTier3_GLauncher 				= CreateConVar("ib_t3_limit_gl", "0", "The total number of grenade launchers allowed on the team. <0: Bots never use grenade launcher>", FCVAR_NOTIFY, true, 0.0);
	
	g_hCvar_Vision_FieldOfView 						= CreateConVar("ib_vision_fov", "70.0", "The field of view of survivor bots.", FCVAR_NOTIFY, true, 0.0, true, 180.0);
	g_hCvar_Vision_NoticeTimeScale 					= CreateConVar("ib_vision_noticetimescale", "1.0", "The time required for bots to notice enemy target is multiplied to this value.", FCVAR_NOTIFY, true, 0.0, true, 4.0);
	
	g_hCvar_WitchBehavior_AllowCrowning				= CreateConVar("ib_witchbehavior_allowcrowning", "0", "(WIP) Allows survivor bots to crown witch on their path if they're holding any shotgun type weapon. <0: Disabled; 1: Only if survivor team doesn't have any human players; 2:Enabled>", FCVAR_NOTIFY, true, 0.0, true, 2.0);
	g_hCvar_WitchBehavior_WalkWhenNearby			= CreateConVar("ib_witchbehavior_walkwhennearby", "0", "Survivor bots will start walking near witch if they're this range near her and she's not disturbed. <0: Disabled>", FCVAR_NOTIFY, true, 0.0);

	g_hCvar_NoFallDmgOnLadderFail					= CreateConVar("ib_nofalldmgonladderfail", "1", "If enabled, survivor bots won't take fall damage if they were climbing a ladder just before that.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_DeployUpgradePacks						= CreateConVar("ib_deployupgradepacks", "1", "(WIP) If bots should deploy their upgrade pack when available and not in combat.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_AcidEvasion								= CreateConVar("ib_evade_spit", "1", "Enables survivor bots' improved spitter acid evasion", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_ChargerEvasion							= CreateConVar("ib_evade_charge", "1", "Enables survivor bots's charger dodging behavior.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_TakeCoverFromRocks						= CreateConVar("ib_takecoverfromtankrocks", "1", "If bots should take cover from tank's thrown rocks.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_AvoidTanksWithProp						= CreateConVar("ib_avoidtanksnearpunchableprops", "0", "(WIP) If bots should avoid and retreat from tanks that are nearby punchable props (like cars).", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	
	// g_hCvar_KeepMovingInCombat						= CreateConVar("ib_keepmovingincombat", "1", "If bots shouldn't stop moving in combat when there's no human players in team.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_SwitchOffCSSWeapons						= CreateConVar("ib_avoid_css", "0", "If bots should change their primary weapon to other one if they're using CSS weapons.", FCVAR_NOTIFY, true, 0.0, true, 1.0);
	g_hCvar_DontSwitchToPistol						= CreateConVar("ib_dontswitchtopistol", "1", "If bots shouldn't switch to their pistol while they have sniper rifle equipped.", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_NextProcessTime 						= CreateConVar("ib_process_time", "0.1", "Bots' data computing time delay (infected count, nearby friends, etc). Increasing the value might help increasing the game performance, but slow down bots.", FCVAR_NOTIFY, true, 0.033);
	g_hCvar_Debug 									= CreateConVar("ib_debug", "0", "Spam console/chat in hopes of finding a a clue for your problems. Prints WILL LAG on Windows GUI!", FCVAR_NOTIFY, true, 0.0, true, 1024.0);

	g_hCvar_Nightmare 								= CreateConVar("ib_nightmare", "0", "Enable if you're playing on NIGHTMARE modpack! Adjusts the bot's behaviors to fit better to it", FCVAR_NOTIFY, true, 0.0, true, 1.0);

	g_hCvar_GameDifficulty.AddChangeHook(OnConVarChanged);
	g_hCvar_SurvivorLimpHealth.AddChangeHook(OnConVarChanged);
	g_hCvar_TankRockHealth.AddChangeHook(OnConVarChanged);
	g_hCvar_ChaseBileRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ServerGravity.AddChangeHook(OnConVarChanged);
	g_hCvar_BileCoverDuration_Bot.AddChangeHook(OnConVarChanged);
	g_hCvar_BileCoverDuration_PZ.AddChangeHook(OnConVarChanged);
	g_hCvar_ShovePenaltyMin_Coop.AddChangeHook(OnConVarChanged);
	g_hCvar_ShovePenaltyMin_Versus.AddChangeHook(OnConVarChanged);
	
	g_hCvar_MaxAmmo_Pistol.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_AssaultRifle.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_SMG.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_M60.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_Shotgun.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_AutoShotgun.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_HuntRifle.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_SniperRifle.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_PipeBomb.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_Molotov.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_VomitJar.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_PainPills.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_GrenLauncher.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_Adrenaline.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_Chainsaw.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_AmmoPack.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxAmmo_Medkit.AddChangeHook(OnConVarChanged);
	
	g_hCvar_Ammo_Type_Override.AddChangeHook(OnConVarChanged);

	g_hCvar_MaxMeleeSurvivors.AddChangeHook(OnConVarChanged);
	g_hCvar_BotsShootThrough.AddChangeHook(OnConVarChanged);
	g_hCvar_BotsFriendlyFire.AddChangeHook(OnConVarChanged);
	g_hCvar_BotsDisabled.AddChangeHook(OnConVarChanged);
	g_hCvar_BotsDontShoot.AddChangeHook(OnConVarChanged);
	g_hCvar_BotsVomitBlindTime.AddChangeHook(OnConVarChanged);

	g_hCvar_ImprovedMelee_MaxCount.AddChangeHook(OnConVarChanged);

	g_hCvar_ImprovedMelee_Enabled.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_SwitchCount.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_SwitchRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_ApproachRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_AimRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_AttackRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_ShoveChance.AddChangeHook(OnConVarChanged);
	
	g_hCvar_ImprovedMelee_ChainsawLimit.AddChangeHook(OnConVarChanged);
	g_hCvar_ImprovedMelee_SwitchCount2.AddChangeHook(OnConVarChanged);

	g_hCvar_TargetSelection_Enabled.AddChangeHook(OnConVarChanged);
	g_hCvar_TargetSelection_ShootRange.AddChangeHook(OnConVarChanged);
	g_hCvar_TargetSelection_ShootRange2.AddChangeHook(OnConVarChanged);
	g_hCvar_TargetSelection_ShootRange3.AddChangeHook(OnConVarChanged);
	g_hCvar_TargetSelection_ShootRange4.AddChangeHook(OnConVarChanged);
	g_hCvar_TargetSelection_IgnoreDociles.AddChangeHook(OnConVarChanged);

	g_hCvar_GrenadeThrow_Enabled.AddChangeHook(OnConVarChanged);
	g_hCvar_GrenadeThrow_GrenadeTypes.AddChangeHook(OnConVarChanged);
	g_hCvar_GrenadeThrow_ThrowRange.AddChangeHook(OnConVarChanged);
	g_hCvar_GrenadeThrow_HordeSize.AddChangeHook(OnConVarChanged);
	g_hCvar_GrenadeThrow_NextThrowTime1.AddChangeHook(OnConVarChanged);
	g_hCvar_GrenadeThrow_NextThrowTime2.AddChangeHook(OnConVarChanged);

	g_hCvar_TankRock_ShootEnabled.AddChangeHook(OnConVarChanged);
	g_hCvar_TankRock_ShootRange.AddChangeHook(OnConVarChanged);

	g_hCvar_AutoShove_Enabled.AddChangeHook(OnConVarChanged);
	
	g_hCvar_HelpPinnedFriend_Enabled.AddChangeHook(OnConVarChanged);
	g_hCvar_HelpPinnedFriend_ShootRange.AddChangeHook(OnConVarChanged);
	g_hCvar_HelpPinnedFriend_ShoveRange.AddChangeHook(OnConVarChanged);

	g_hCvar_DefibRevive_Enabled.AddChangeHook(OnConVarChanged);
	g_hCvar_DefibRevive_ScanDist.AddChangeHook(OnConVarChanged);
	
	g_hCvar_FireBash_Chance1.AddChangeHook(OnConVarChanged);
	g_hCvar_FireBash_Chance2.AddChangeHook(OnConVarChanged);
	
	g_hCvar_ItemScavenge_Models.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_Items.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_ApproachRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_ApproachVisibleRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_PickupRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_MapSearchRange.AddChangeHook(OnConVarChanged);
	g_hCvar_ItemScavenge_NoHumansRangeMultiplier.AddChangeHook(OnConVarChanged);

	g_hCvar_BotWeaponPreference_ForceMagnum.AddChangeHook(OnConVarChanged);
	
	g_hCvar_BotWeaponPreference_Nick.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Louis.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Bill.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Rochelle.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Zoey.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Ellis.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Coach.AddChangeHook(OnConVarChanged);
	g_hCvar_BotWeaponPreference_Francis.AddChangeHook(OnConVarChanged);
	
	g_hCvar_SwapSameTypePrimaries_T1.AddChangeHook(OnConVarChanged);
	g_hCvar_SwapSameTypePrimaries_T2.AddChangeHook(OnConVarChanged);
	g_hCvar_SwapSameTypeGrenades.AddChangeHook(OnConVarChanged);

	g_hCvar_T3_Refill.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxWeaponTier3_M60.AddChangeHook(OnConVarChanged);
	g_hCvar_MaxWeaponTier3_GLauncher.AddChangeHook(OnConVarChanged);
	
	g_hCvar_Vision_FieldOfView.AddChangeHook(OnConVarChanged);
	g_hCvar_Vision_NoticeTimeScale.AddChangeHook(OnConVarChanged);
	
	g_hCvar_AcidEvasion.AddChangeHook(OnConVarChanged);
	// g_hCvar_KeepMovingInCombat.AddChangeHook(OnConVarChanged);
	g_hCvar_SwitchOffCSSWeapons.AddChangeHook(OnConVarChanged);
	g_hCvar_ChargerEvasion.AddChangeHook(OnConVarChanged);
	g_hCvar_DeployUpgradePacks.AddChangeHook(OnConVarChanged);
	g_hCvar_DontSwitchToPistol.AddChangeHook(OnConVarChanged);
	g_hCvar_TakeCoverFromRocks.AddChangeHook(OnConVarChanged);
	g_hCvar_AvoidTanksWithProp.AddChangeHook(OnConVarChanged);
	g_hCvar_NoFallDmgOnLadderFail.AddChangeHook(OnConVarChanged);
	g_hCvar_HasEnoughAmmoRatio.AddChangeHook(OnConVarChanged);

	g_hCvar_WitchBehavior_WalkWhenNearby.AddChangeHook(OnConVarChanged);
	g_hCvar_WitchBehavior_AllowCrowning.AddChangeHook(OnConVarChanged);
	
	g_hCvar_NextProcessTime.AddChangeHook(OnConVarChanged);
	g_hCvar_Debug.AddChangeHook(OnConVarChanged);

	g_hCvar_Nightmare.AddChangeHook(OnConVarChanged);
}

public void OnAllPluginsLoaded()
{
	g_bExtensionActions = LibraryExists("actionslib");
	g_bPluginVScript = LibraryExists("vscript");

	UpdateConVarValues();
}

public void OnConfigsExecuted()
{
	UpdateConVarValues();
}

void OnConVarChanged(Handle convar, const char[] oldValue, const char[] newValue)
{
	UpdateConVarValues();
}

void UpdateConVarValues()
{	
	g_hCvar_GameDifficulty.GetString(g_sCvar_GameDifficulty, sizeof(g_sCvar_GameDifficulty));
	g_iCvar_SurvivorLimpHealth 							= g_hCvar_SurvivorLimpHealth.IntValue;
	g_iCvar_TankRockHealth 								= g_hCvar_TankRockHealth.IntValue;
	g_fCvar_ChaseBileRange 								= g_hCvar_ChaseBileRange.FloatValue;
	g_fCvar_ServerGravity 								= g_hCvar_ServerGravity.FloatValue;
	g_fCvar_BileCoverDuration_Bot 						= g_hCvar_BileCoverDuration_Bot.FloatValue;
	g_fCvar_BileCoverDuration_PZ 						= g_hCvar_BileCoverDuration_PZ.FloatValue;
	g_iCvar_ShovePenaltyMin								= (L4D_IsVersusMode() ? g_hCvar_ShovePenaltyMin_Versus : g_hCvar_ShovePenaltyMin_Coop).IntValue;

	g_iCvar_MaxAmmo_Pistol								= g_hCvar_MaxAmmo_Pistol.IntValue;
	g_iCvar_MaxAmmo_AssaultRifle						= g_hCvar_MaxAmmo_AssaultRifle.IntValue;
	g_iCvar_MaxAmmo_SMG									= g_hCvar_MaxAmmo_SMG.IntValue;
	g_iCvar_MaxAmmo_M60									= g_hCvar_MaxAmmo_M60.IntValue;
	g_iCvar_MaxAmmo_Shotgun								= g_hCvar_MaxAmmo_Shotgun.IntValue;
	g_iCvar_MaxAmmo_AutoShotgun							= g_hCvar_MaxAmmo_AutoShotgun.IntValue;
	g_iCvar_MaxAmmo_HuntRifle							= g_hCvar_MaxAmmo_HuntRifle.IntValue;	
	g_iCvar_MaxAmmo_SniperRifle							= g_hCvar_MaxAmmo_SniperRifle.IntValue;
	g_iCvar_MaxAmmo_PipeBomb							= g_hCvar_MaxAmmo_PipeBomb.IntValue;	
	g_iCvar_MaxAmmo_Molotov								= g_hCvar_MaxAmmo_Molotov.IntValue;
	g_iCvar_MaxAmmo_VomitJar							= g_hCvar_MaxAmmo_VomitJar.IntValue;
	g_iCvar_MaxAmmo_PainPills							= g_hCvar_MaxAmmo_PainPills.IntValue;
	g_iCvar_MaxAmmo_GrenLauncher						= g_hCvar_MaxAmmo_GrenLauncher.IntValue;
	g_iCvar_MaxAmmo_Adrenaline							= g_hCvar_MaxAmmo_Adrenaline.IntValue;
	g_iCvar_MaxAmmo_Chainsaw							= g_hCvar_MaxAmmo_Chainsaw.IntValue;
	g_iCvar_MaxAmmo_AmmoPack							= g_hCvar_MaxAmmo_AmmoPack.IntValue;
	g_iCvar_MaxAmmo_Medkit								= g_hCvar_MaxAmmo_Medkit.IntValue;
	
	char sArgs[32];
	g_hCvar_Ammo_Type_Override.GetString( sArgs, sizeof(sArgs));
	if (strcmp(sArgs, g_sCvar_Ammo_Type_Override))
	{
		//if (g_bCvar_Debug)
		PrintToServer("UpdateConVarValues: InitMaxAmmo");
		strcopy(g_sCvar_Ammo_Type_Override, sizeof(g_sCvar_Ammo_Type_Override), sArgs);
		InitMaxAmmo();
	}

	g_bCvar_BotsShootThrough 							= g_hCvar_BotsShootThrough.BoolValue;
	g_bCvar_BotsFriendlyFire 							= g_hCvar_BotsFriendlyFire.BoolValue;
	g_bCvar_BotsDisabled								= g_hCvar_BotsDisabled.BoolValue;
	g_bCvar_BotsDontShoot								= g_hCvar_BotsDontShoot.BoolValue;
	g_fCvar_BotsVomitBlindTime							= g_hCvar_BotsVomitBlindTime.FloatValue;

	g_hCvar_MaxMeleeSurvivors.IntValue					= g_hCvar_ImprovedMelee_MaxCount.IntValue;
	g_iCvar_MaxMeleeSurvivors 							= g_hCvar_MaxMeleeSurvivors.IntValue;

	g_bCvar_ImprovedMelee_Enabled 						= g_hCvar_ImprovedMelee_Enabled.BoolValue;
	g_iCvar_ImprovedMelee_SwitchCount 					= g_hCvar_ImprovedMelee_SwitchCount.IntValue;
	g_iCvar_ImprovedMelee_ShoveChance 					= g_hCvar_ImprovedMelee_ShoveChance.IntValue;
	g_fCvar_ImprovedMelee_SwitchRange 					= (g_hCvar_ImprovedMelee_SwitchRange.FloatValue * g_hCvar_ImprovedMelee_SwitchRange.FloatValue);
	g_fCvar_ImprovedMelee_ApproachRange 				= (g_hCvar_ImprovedMelee_ApproachRange.FloatValue * g_hCvar_ImprovedMelee_ApproachRange.FloatValue);
	g_fCvar_ImprovedMelee_AimRange_Sqr					= (g_hCvar_ImprovedMelee_AimRange.FloatValue * g_hCvar_ImprovedMelee_AimRange.FloatValue);
	g_fCvar_ImprovedMelee_AttackRange 					= (g_hCvar_ImprovedMelee_AttackRange.FloatValue * g_hCvar_ImprovedMelee_AttackRange.FloatValue);
	
	g_iCvar_ImprovedMelee_ChainsawLimit 				= g_hCvar_ImprovedMelee_ChainsawLimit.IntValue;
	g_iCvar_ImprovedMelee_SwitchCount2 					= g_hCvar_ImprovedMelee_SwitchCount2.IntValue;

	g_bCvar_TargetSelection_Enabled						= g_hCvar_TargetSelection_Enabled.BoolValue;
	g_fCvar_TargetSelection_ShootRange					= (g_hCvar_TargetSelection_ShootRange.FloatValue * g_hCvar_TargetSelection_ShootRange.FloatValue);
	g_fCvar_TargetSelection_ShootRange2					= (g_hCvar_TargetSelection_ShootRange2.FloatValue * g_hCvar_TargetSelection_ShootRange2.FloatValue);
	g_fCvar_TargetSelection_ShootRange3					= (g_hCvar_TargetSelection_ShootRange3.FloatValue * g_hCvar_TargetSelection_ShootRange3.FloatValue);
	g_fCvar_TargetSelection_ShootRange4					= (g_hCvar_TargetSelection_ShootRange4.FloatValue * g_hCvar_TargetSelection_ShootRange4.FloatValue);
	g_bCvar_TargetSelection_IgnoreDociles				= g_hCvar_TargetSelection_IgnoreDociles.BoolValue;

	g_bCvar_BotWeaponPreference_ForceMagnum 			= g_hCvar_BotWeaponPreference_ForceMagnum.BoolValue;
	
	g_iCvar_BotWeaponPreference_Nick 					= g_hCvar_BotWeaponPreference_Nick.IntValue;
	g_iCvar_BotWeaponPreference_Louis 					= g_hCvar_BotWeaponPreference_Louis.IntValue;
	g_iCvar_BotWeaponPreference_Bill 					= g_hCvar_BotWeaponPreference_Bill.IntValue;
	g_iCvar_BotWeaponPreference_Rochelle 				= g_hCvar_BotWeaponPreference_Rochelle.IntValue;
	g_iCvar_BotWeaponPreference_Zoey 					= g_hCvar_BotWeaponPreference_Zoey.IntValue;
	g_iCvar_BotWeaponPreference_Ellis 					= g_hCvar_BotWeaponPreference_Ellis.IntValue;
	g_iCvar_BotWeaponPreference_Coach 					= g_hCvar_BotWeaponPreference_Coach.IntValue;
	g_iCvar_BotWeaponPreference_Francis 				= g_hCvar_BotWeaponPreference_Francis.IntValue;

	g_bCvar_GrenadeThrow_Enabled 						= g_hCvar_GrenadeThrow_Enabled.BoolValue;
	g_iCvar_GrenadeThrow_GrenadeTypes 					= g_hCvar_GrenadeThrow_GrenadeTypes.IntValue;
	g_fCvar_GrenadeThrow_ThrowRange 					= g_hCvar_GrenadeThrow_ThrowRange.FloatValue;
	g_fCvar_GrenadeThrow_ThrowRange_Sqr 				= (g_fCvar_GrenadeThrow_ThrowRange * g_fCvar_GrenadeThrow_ThrowRange);
	g_fCvar_GrenadeThrow_ThrowRange_NoVisCheck			= ((g_fCvar_GrenadeThrow_ThrowRange * 0.33) * (g_fCvar_GrenadeThrow_ThrowRange * 0.33));
	g_fCvar_GrenadeThrow_HordeSize 						= g_hCvar_GrenadeThrow_HordeSize.FloatValue;
	g_fCvar_GrenadeThrow_NextThrowTime1 				= g_hCvar_GrenadeThrow_NextThrowTime1.FloatValue;
	g_fCvar_GrenadeThrow_NextThrowTime2 				= g_hCvar_GrenadeThrow_NextThrowTime2.FloatValue;

	g_bCvar_TankRock_ShootEnabled						= g_hCvar_TankRock_ShootEnabled.BoolValue;
	g_fCvar_TankRock_ShootRange_Sqr						= (g_hCvar_TankRock_ShootRange.FloatValue * g_hCvar_TankRock_ShootRange.FloatValue);

	g_bCvar_DefibRevive_Enabled 						= g_hCvar_DefibRevive_Enabled.BoolValue;
	g_fCvar_DefibRevive_ScanDist_Sqr 					= (g_hCvar_DefibRevive_ScanDist.FloatValue * g_hCvar_DefibRevive_ScanDist.FloatValue);

	g_iCvar_FireBash_Chance1 							= g_hCvar_FireBash_Chance1.IntValue;
	g_iCvar_FireBash_Chance2 							= g_hCvar_FireBash_Chance2.IntValue;

	g_iCvar_AutoShove_Enabled 							= g_hCvar_AutoShove_Enabled.BoolValue;
	
	g_iCvar_HelpPinnedFriend_Enabled 					= g_hCvar_HelpPinnedFriend_Enabled.IntValue;
	//g_fCvar_HelpPinnedFriend_ShootRange 				= g_hCvar_HelpPinnedFriend_ShootRange.FloatValue;
	g_fCvar_HelpPinnedFriend_ShootRange_Sqr				= (g_hCvar_HelpPinnedFriend_ShootRange.FloatValue * g_hCvar_HelpPinnedFriend_ShootRange.FloatValue);
	
	g_iCvar_ItemScavenge_Models 						= g_hCvar_ItemScavenge_Models.IntValue;
	g_iCvar_ItemScavenge_Items 							= g_hCvar_ItemScavenge_Items.IntValue;
	g_fCvar_ItemScavenge_ApproachRange 					= g_hCvar_ItemScavenge_ApproachRange.FloatValue;
	g_fCvar_ItemScavenge_ApproachVisibleRange 			= g_hCvar_ItemScavenge_ApproachVisibleRange.FloatValue;
	g_fCvar_ItemScavenge_PickupRange					= g_hCvar_ItemScavenge_PickupRange.FloatValue;
	g_fCvar_ItemScavenge_PickupRange_Sqr				= (g_fCvar_ItemScavenge_PickupRange * g_fCvar_ItemScavenge_PickupRange);
	g_fCvar_ItemScavenge_MapSearchRange_Sqr				= (g_hCvar_ItemScavenge_MapSearchRange.FloatValue * g_hCvar_ItemScavenge_MapSearchRange.FloatValue);
	g_fCvar_ItemScavenge_NoHumansRangeMultiplier 		= g_hCvar_ItemScavenge_NoHumansRangeMultiplier.FloatValue;

	g_bCvar_SwapSameTypePrimaries_T1 					= g_hCvar_SwapSameTypePrimaries_T1.BoolValue;
	g_bCvar_SwapSameTypePrimaries_T2 					= g_hCvar_SwapSameTypePrimaries_T2.BoolValue;
	g_bCvar_SwapSameTypeGrenades 						= g_hCvar_SwapSameTypeGrenades.BoolValue;
	
	g_iCvar_T3_Refill									= g_hCvar_T3_Refill.IntValue;
	g_iCvar_MaxWeaponTier3_M60 							= g_hCvar_MaxWeaponTier3_M60.IntValue;
	g_iCvar_MaxWeaponTier3_GLauncher 					= g_hCvar_MaxWeaponTier3_GLauncher.IntValue;
	
	g_fCvar_Vision_FieldOfView 							= g_hCvar_Vision_FieldOfView.FloatValue;
	g_fCvar_Vision_NoticeTimeScale 						= g_hCvar_Vision_NoticeTimeScale.FloatValue;
	
	g_bCvar_AcidEvasion									= g_hCvar_AcidEvasion.BoolValue;
	g_bCvar_SwitchOffCSSWeapons							= g_hCvar_SwitchOffCSSWeapons.BoolValue;
	g_bCvar_ChargerEvasion								= g_hCvar_ChargerEvasion.BoolValue;
	g_bCvar_DeployUpgradePacks							= g_hCvar_DeployUpgradePacks.BoolValue;
	g_bCvar_DontSwitchToPistol							= g_hCvar_DontSwitchToPistol.BoolValue;
	g_bCvar_TakeCoverFromRocks							= g_hCvar_TakeCoverFromRocks.BoolValue;
	g_bCvar_AvoidTanksWithProp							= g_hCvar_AvoidTanksWithProp.BoolValue;
	g_bCvar_NoFallDmgOnLadderFail						= g_hCvar_NoFallDmgOnLadderFail.BoolValue;
	g_fCvar_HasEnoughAmmoRatio							= g_hCvar_HasEnoughAmmoRatio.FloatValue;

	// if (g_bMapStarted)
	// {
	// 	char sShouldHurryCode[64]; FormatEx(sShouldHurryCode, sizeof(sShouldHurryCode), "DirectorScript.GetDirectorOptions().cm_ShouldHurry <- %i", g_hCvar_KeepMovingInCombat.IntValue);
	// 	L4D2_ExecVScriptCode(sShouldHurryCode);
	// }

	g_fCvar_WitchBehavior_WalkWhenNearby 				= g_hCvar_WitchBehavior_WalkWhenNearby.FloatValue;
	g_iCvar_WitchBehavior_AllowCrowning 				= g_hCvar_WitchBehavior_AllowCrowning.IntValue;

	g_fCvar_NextProcessTime 							= g_hCvar_NextProcessTime.FloatValue;
	g_iCvar_Debug 										= g_hCvar_Debug.IntValue;
	//if(L4D_HasMapStarted())
	//{
	//	if (g_iCvar_Debug & DEBUG_HUD)
	//		DebugHUDShow();
	//	else
	//		DebugHUDHide();
	//}

	g_bCvar_Nightmare 									= g_hCvar_Nightmare.BoolValue;
}

static Handle g_hCalcAbsolutePosition;

static Handle g_hLookupBone; 
static Handle g_hGetBonePosition; 

static Handle g_hGetMaxClip1;
static Handle g_hFindUseEntity;
static Handle g_hIsInCombat;

static Handle g_hIsReachableNavArea; 
static Handle g_hIsAvailable; 

static Handle g_hMarkNavAreaAsBlocked;
//static Handle g_hSubdivideNavArea;

static Handle g_hSurvivorLegsRetreat;

static int g_iNavArea_Center;
static int g_iNavArea_Parent;
static int g_iNavArea_NWCorner;
static int g_iNavArea_SECorner;
static int g_iNavArea_InvDXCorners;
static int g_iNavArea_InvDYCorners;
static int g_iNavArea_DamagingTickCount;

void CreateAllSDKCalls(Handle hGameData)
{
	if ((g_iNavArea_Center = GameConfGetOffset(hGameData, "CNavArea::m_center")) == -1)
		SetFailState("Failed to get CNavArea::m_center offset.");
	if ((g_iNavArea_Parent = GameConfGetOffset(hGameData, "CNavArea::m_parent")) == -1)
		SetFailState("Failed to get CNavArea::m_parent offset.");
	if ((g_iNavArea_NWCorner = GameConfGetOffset(hGameData, "CNavArea::m_nwCorner")) == -1)
		SetFailState("Failed to get CNavArea::m_nwCorner offset.");
	if ((g_iNavArea_SECorner = GameConfGetOffset(hGameData, "CNavArea::m_seCorner")) == -1)
		SetFailState("Failed to get CNavArea::m_seCorner offset.");
	if ((g_iNavArea_InvDXCorners = GameConfGetOffset(hGameData, "CNavArea::m_invDxCorners")) == -1)
		SetFailState("Failed to get CNavArea::m_invDxCorners offset.");
	if ((g_iNavArea_InvDYCorners = GameConfGetOffset(hGameData, "CNavArea::m_invDyCorners")) == -1)
		SetFailState("Failed to get CNavArea::m_invDyCorners offset.");
	if ((g_iNavArea_DamagingTickCount = GameConfGetOffset(hGameData, "CNavArea::m_damagingTickCount")) == -1)
		SetFailState("Failed to get CNavArea::m_damagingTickCount offset.");

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CBaseEntity::CalcAbsolutePosition");
	if ((g_hCalcAbsolutePosition = EndPrepSDKCall()) == null) 
		SetFailState("Failed to create SDKCall for CBaseEntity::CalcAbsolutePosition signature!");

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CBaseAnimating::GetBonePosition");
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_Vector, SDKPass_ByRef, _, VENCODE_FLAG_COPYBACK);
	PrepSDKCall_AddParameter(SDKType_QAngle, SDKPass_ByRef, _, VENCODE_FLAG_COPYBACK);
	if ((g_hGetBonePosition = EndPrepSDKCall()) == null) 
		SetFailState("Failed to create SDKCall for CBaseAnimating::GetBonePosition signature!");

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CBaseAnimating::LookupBone");
	PrepSDKCall_AddParameter(SDKType_String, SDKPass_Pointer);
	PrepSDKCall_SetReturnInfo(SDKType_PlainOldData, SDKPass_Plain);
	if ((g_hLookupBone = EndPrepSDKCall()) == null) 
		SetFailState("Failed to create SDKCall for CBaseAnimating::LookupBone signature!");

	StartPrepSDKCall(SDKCall_Entity);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Virtual, "CBaseCombatWeapon::GetMaxClip1");
	PrepSDKCall_SetReturnInfo(SDKType_PlainOldData, SDKPass_Plain);
	if ((g_hGetMaxClip1 = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for CBaseCombatWeapon::GetMaxClip1 virtual!");

	StartPrepSDKCall(SDKCall_Player);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "SurvivorBot::IsReachable<CNavArea>");
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	if ((g_hIsReachableNavArea = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for SurvivorBot::IsReachable<CNavArea> signature!");

	StartPrepSDKCall(SDKCall_Player);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "SurvivorBot::IsAvailable");
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	if ((g_hIsAvailable = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for SurvivorBot::IsAvailable signature!");

	StartPrepSDKCall(SDKCall_Player);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CTerrorPlayer::FindUseEntity");
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_Float, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);
	PrepSDKCall_SetReturnInfo(SDKType_CBaseEntity, SDKPass_Pointer);
	if ((g_hFindUseEntity = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for CTerrorPlayer::FindUseEntity signature!");

	StartPrepSDKCall(SDKCall_Player);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CTerrorPlayer::IsInCombat");
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	if ((g_hIsInCombat = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for CTerrorPlayer::IsInCombat signature!");	

	StartPrepSDKCall(SDKCall_Raw);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "CNavArea::MarkAsBlocked");
	PrepSDKCall_AddParameter(SDKType_PlainOldData, SDKPass_Plain);
	PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer, VDECODE_FLAG_ALLOWNULL);
	PrepSDKCall_AddParameter(SDKType_Bool, SDKPass_Plain);
	PrepSDKCall_SetReturnInfo(SDKType_Bool, SDKPass_Plain);
	if ((g_hMarkNavAreaAsBlocked = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for CNavArea::MarkAsBlocked signature!");

	StartPrepSDKCall(SDKCall_Raw);
	PrepSDKCall_SetFromConf(hGameData, SDKConf_Signature, "SurvivorLegsRetreat::SurvivorLegsRetreat");
	PrepSDKCall_AddParameter(SDKType_CBaseEntity, SDKPass_Pointer);
	if ((g_hSurvivorLegsRetreat = EndPrepSDKCall()) == null)
		SetFailState("Failed to create SDKCall for SurvivorLegsRetreat::SurvivorLegsRetreat signature!");

}

static Handle g_hOnWpnPrimaryAttack;

static Handle g_hOnFindUseEntity;
static Handle g_hOnInfernoTouchNavArea;
static Handle g_hOnGetAvoidRange;

void CreateAllDetours(Handle hGameData)
{
	g_hOnWpnPrimaryAttack = DHookCreate(0, HookType_Entity, ReturnType_Void, ThisPointer_CBaseEntity, DHooks_OnWpnPrimaryAttack);
	if (!g_hOnWpnPrimaryAttack)
		SetFailState("Failed to setup hook for CBaseCombatWeapon::PrimaryAttack");
	if (!DHookSetFromConf(g_hOnWpnPrimaryAttack, hGameData, SDKConf_Virtual, "CBaseCombatWeapon::PrimaryAttack"))
		SetFailState("Failed to load CBaseCombatWeapon::PrimaryAttack offset from gamedata");

	g_hOnFindUseEntity = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_CBaseEntity, ThisPointer_CBaseEntity);
	if (!g_hOnFindUseEntity)SetFailState("Failed to setup detour for CTerrorPlayer::FindUseEntity");
	if (!DHookSetFromConf(g_hOnFindUseEntity, hGameData, SDKConf_Signature, "CTerrorPlayer::FindUseEntity"))
		SetFailState("Failed to load CTerrorPlayer::FindUseEntity signature from gamedata");
	DHookAddParam(g_hOnFindUseEntity, HookParamType_Float);
	DHookAddParam(g_hOnFindUseEntity, HookParamType_Float);
	DHookAddParam(g_hOnFindUseEntity, HookParamType_Float);
	DHookAddParam(g_hOnFindUseEntity, HookParamType_Bool);
	DHookAddParam(g_hOnFindUseEntity, HookParamType_Bool);
	if (!DHookEnableDetour(g_hOnFindUseEntity, true, DTR_OnFindUseEntity))
		SetFailState("Failed to detour CTerrorPlayer::FindUseEntity.");

	g_hOnInfernoTouchNavArea = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Bool, ThisPointer_CBaseEntity);
	if (!g_hOnInfernoTouchNavArea)SetFailState("Failed to setup detour for CInferno::IsTouching<CNavArea>");
	if (!DHookSetFromConf(g_hOnInfernoTouchNavArea, hGameData, SDKConf_Signature, "CInferno::IsTouching<CNavArea>"))
		SetFailState("Failed to load CInferno::IsTouching<CNavArea> signature from gamedata");
	DHookAddParam(g_hOnInfernoTouchNavArea, HookParamType_Int);
	if (!DHookEnableDetour(g_hOnInfernoTouchNavArea, true, DTR_OnInfernoTouchNavArea))
		SetFailState("Failed to detour CInferno::IsTouching<CNavArea>.");

	g_hOnGetAvoidRange = DHookCreateDetour(Address_Null, CallConv_THISCALL, ReturnType_Float, ThisPointer_CBaseEntity);
	if (!g_hOnGetAvoidRange)SetFailState("Failed to setup detour for SurvivorBot::GetAvoidRange");
	if (!DHookSetFromConf(g_hOnGetAvoidRange, hGameData, SDKConf_Signature, "SurvivorBot::GetAvoidRange"))
		SetFailState("Failed to load SurvivorBot::GetAvoidRange signature from gamedata");
	DHookAddParam(g_hOnGetAvoidRange, HookParamType_CBaseEntity);
	if (!DHookEnableDetour(g_hOnGetAvoidRange, true, DTR_OnSurvivorBotGetAvoidRange))
		SetFailState("Failed to detour SurvivorBot::GetAvoidRange.");	
}

bool ParseDataFile()
{
	char sPath[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, sPath, sizeof(sPath), "data/ib_data.cfg");
	if( FileExists(sPath) == false )
		SetFailState("\n==========\n[IB] Missing required file: \"%s\".\n==========", sPath);
	
	g_hWeaponToIDMap = CreateTrie();
	g_hWeaponMdlMap = CreateTrie();
	g_hMeleeMdlToID = CreateTrie();
	g_hMeleePref = CreateTrie();
	g_iDataFileSection = 0;

	SMCParser parser = new SMCParser();
	parser.OnEnterSection = DataFile_NewSection;
	parser.OnKeyValue = DataFile_KeyValue;
	parser.OnLeaveSection = DataFile_EndSection;
	parser.OnEnd = DataFile_End;

	char error[128];
	int line = 0, col = 0;
	SMCError result = parser.ParseFile(sPath, line, col);

	if( result != SMCError_Okay )
	{
		parser.GetErrorString(result, error, sizeof(error));
		SetFailState("%s on line %d, col %d of %s [%d]", error, line, col, sPath, result);
	}

	delete parser;
	return (result == SMCError_Okay);
}

SMCResult DataFile_NewSection(SMCParser parser, const char[] section, bool quotes)
{
	g_iDataFileValueID = 0;
	return SMCParse_Continue;
}

SMCResult DataFile_KeyValue(SMCParser parser, const char[] key, const char[] value, bool key_quotes, bool value_quotes)
{
	static int iValue;
	
	switch(g_iDataFileSection)
	{
		case 0:
		{
			iValue = StringToInt(value);
			//PrintToServer("ParseDataFile %s %d", key, iValue);
			g_hMeleePref.SetValue(key, iValue);
		}
		case 1:
		{
			strcopy( IBWeaponName[g_iDataFileValueID], 32, key );
			g_hWeaponToIDMap.SetValue(key, view_as<L4D2WeaponId>(g_iDataFileValueID));
			g_iDataFileValueID++;
		}
		case 2:
		{
			g_hWeaponMdlMap.SetString(key, value);
		}
		case 3:
		{
			g_hMeleeMdlToID.SetString(key, value);
		}
	}
	
	return SMCParse_Continue;
}

SMCResult DataFile_EndSection(SMCParser parser)
{
	g_iDataFileSection++;
	return SMCParse_Continue;
}

void DataFile_End(SMCParser parser, bool halted, bool failed)
{
	if( failed )
		SetFailState("Error: could not load data file.");
}

void CreateVScriptCommandDetour()
{
	g_vsCommand = VScript_CreateGlobalFunction("L4B_CommandIssued");
	g_vsCommand.SetParam(1, FIELD_INTEGER);
	g_vsCommand.SetParam(2, FIELD_INTEGER);
	g_vsCommand.SetParam(3, FIELD_INTEGER);
	g_vsCommand.Return = FIELD_BOOLEAN;
	g_vsCommand.SetFunctionEmpty();
	g_vsCommand.CreateDetour().Enable(Hook_Post, Detour_CommandIssued);
}

void Event_OnRoundStart(Event hEvent, const char[] sName, bool bBroadcast)
{
	ResetDataOnRoundChange();
}

void ResetDataOnRoundChange()
{
	g_iBotProcessing_ProcessedCount = 0;
	g_fBotProcessing_NextProcessTime = (GetGameTime() + g_fCvar_NextProcessTime);

	g_fBot_Grenade_NextThrowTime = g_fBot_Grenade_NextThrowTime_Molotov = (GetGameTime() + 5.0);

	for (int i = 1; i <= MaxClients; i++)
	{
		g_fClient_ThinkFunctionDelay[i] = (GetGameTime() + 5.0);

		if (IsClientInGame(i))
			ResetClientPluginVariables(i);
	}
}

public void OnClientPutInServer(int iClient)
{
	OnClientJoinServer(iClient);
}

void OnClientJoinServer(int iClient)
{
	SDKHook(iClient, SDKHook_WeaponSwitch, OnSurvivorSwitchWeapon);
	SDKHook(iClient, SDKHook_OnTakeDamage, OnSurvivorTakeDamage);
	g_fClient_ThinkFunctionDelay[iClient] = GetGameTime() + 5.0;
	ResetClientPluginVariables(iClient);
}

public void OnClientDisconnect(int iClient)
{
	SDKUnhook(iClient, SDKHook_WeaponSwitch, OnSurvivorSwitchWeapon);
	SDKUnhook(iClient, SDKHook_OnTakeDamage, OnSurvivorTakeDamage);
	g_fClient_ThinkFunctionDelay[iClient] = GetGameTime() + 5.0;
	ResetClientPluginVariables(iClient);
}

void ResetClientPluginVariables(int iClient)
{
	g_iBot_TargetInfected[iClient] = 0;	
	g_iBot_WitchTarget[iClient] = 0;	
	g_iBot_ThreatInfectedCount[iClient] = 0;
	g_iBot_NearbyInfectedCount[iClient] = 0;
	g_iBot_NearestInfectedCount[iClient] = 0;
	g_iBot_GrenadeInfectedCount[iClient] = 0;
	g_iBot_ScavengeItem[iClient] = 0;
	g_fBot_ScavengeItemDist[iClient] = -1.0;
	g_iBot_DefibTarget[iClient] = 0;
	g_iBot_Grenade_ThrowTarget[iClient] = 0;
	g_iBot_MovePos_Priority[iClient] = 0;
	
	g_sBot_MovePos_Name[iClient][0] = 0;
	g_fBot_MovePos_Tolerance[iClient] = -1.0;

	g_bBot_IsWitchHarasser[iClient] = false;
	g_bBot_ForceWeaponReload[iClient] = false;
	g_bBot_ForceSwitchWeapon[iClient] = false;
	g_bBot_ForceBash[iClient] = false;
	g_bBot_MovePos_IgnoreDamaging[iClient] = false;
	g_bBot_WasUsingLadder[iClient] = false;

	g_fBot_NextScavengeItemScanTime[iClient] = GetGameTime() + 1.0;
	g_fBot_NextMoveCommandTime[iClient] = GetGameTime() + BOT_CMD_MOVE_INTERVAL;

	g_fBot_PinnedReactTime[iClient] = 0.0;
	g_fBot_BlockWeaponReloadTime[iClient] = 0.0;
	g_fBot_BlockWeaponSwitchTime[iClient] = 0.0;
	g_fBot_VomitBlindedTime[iClient] = 0.0;
	g_fBot_MeleeApproachTime[iClient] = 0.0;
	g_fBot_MeleeAttackTime[iClient] = 0.0;
	g_fBot_LookPosition_Duration[iClient] = 0.0;
	g_fBot_NextPressAttackTime[iClient] = 0.0;
	g_fBot_MovePos_Duration[iClient] = 0.0;
	g_fBot_NextWeaponRangeSwitchTime[iClient] = 0.0;
	g_fBot_PreventFireTime[iClient] = 0.0;

	SetVectorToZero(g_fBot_Grenade_ThrowPos[iClient]);
	SetVectorToZero(g_fBot_Grenade_AimPos[iClient]);
	SetVectorToZero(g_fBot_LookPosition[iClient]);
	SetVectorToZero(g_fBot_MovePos_Position[iClient]);

	for (int i = 0; i < MAXENTITIES; i++)
	{
		g_iBot_VisionMemory_State[iClient][0][i] = g_iBot_VisionMemory_State[iClient][1][i] = 0;
		g_fBot_VisionMemory_Time[iClient][0][i] = g_fBot_VisionMemory_Time[iClient][1][i] = 0.0;
	}

	if (!IsValidClient(iClient) || !IsFakeClient(iClient) || GetClientTeam(iClient) != 2)
		return;

	L4D2_CommandABot(iClient, 0, BOT_CMD_RESET);
	g_bBotProcessing_IsProcessed[iClient] = false;
}

void Event_OnWeaponFire(Event hEvent, const char[] sName, bool bBroadcast)
{
	static int iItemFlags /*, iWeaponID */, iUserID, iClient;
	static char sWeaponName[64]/*, sClientName[128]*/;
	iUserID = hEvent.GetInt("userid");
	//iWeaponID = hEvent.GetInt("weaponid");
	iClient = GetClientOfUserId(iUserID);
	if (!IsFakeClient(iClient))return;
	
	hEvent.GetString("weapon", sWeaponName, sizeof(sWeaponName));
	
	if (!g_bInitItemFlags)
	{
		InitItemFlagMap();
		//if(g_bCvar_Debug)
		//	PrintToServer("Event_OnWeaponFire: g_hItemFlagMap not initialized, doing now");
	}
	g_hItemFlagMap.GetValue(sWeaponName, iItemFlags);

	if ( GetRandomInt(1, g_iCvar_FireBash_Chance1) == 1 && iItemFlags & FLAG_SHOTGUN && iItemFlags & FLAG_TIER1 )
	{
		g_bBot_ForceBash[iClient] = true;
	}
	else if ( GetRandomInt(1, g_iCvar_FireBash_Chance2) == 1 && iItemFlags & FLAG_SNIPER && iItemFlags & FLAG_CSS )
	{
		g_bBot_ForceBash[iClient] = true;
	}

	// if nightmare, don't spray 'n pray 'n waste ammo (unless out target is really close)
	int iCurTarget = g_iBot_TargetInfected[iClient];
	if (g_bCvar_Nightmare && IsWeaponSlotActive(iClient, 0) && (!iCurTarget || iCurTarget != g_iBot_TankTarget[iClient] || GetEntityDistance(iClient, iCurTarget, true) > 16384.0)) // 128
	{
		float fMinDelay = 0.1, fMaxDelay = 0.15;
		if ((iItemFlags & FLAG_SNIPER))
		{
			fMinDelay = 0.2;
			fMaxDelay = ((iItemFlags & FLAG_CSS) ? 0.5 : 0.33);
		}
		else if ((iItemFlags & FLAG_SHOTGUN))
		{
			fMinDelay = 0.33;
			fMaxDelay = 0.66;
		}

		g_fBot_PreventFireTime[iClient] = (GetGameTime() + GetRandomFloat(fMinDelay, fMaxDelay));
	}
	
	RequestFrame(NullifyAimPunch, iClient);
}

void NullifyAimPunch(int iClient)
{
	if(IsValidClient(iClient))
		SetEntPropVector(iClient, Prop_Send, "m_vecPunchAngle", view_as<float>({ 0.0, 0.0, 0.0 }));
}

void Event_OnPlayerDeath(Event hEvent, const char[] sName, bool bBroadcast)
{
	int iVictim = GetClientOfUserId(hEvent.GetInt("userid"));
	int iAttacker = GetClientOfUserId(hEvent.GetInt("attacker"));
	int iInfected = hEvent.GetInt("entityid");

	// LLM score: track SI kills by survivor
	if (iAttacker > 0 && iAttacker <= MaxClients && iVictim > 0 && iVictim <= MaxClients)
	{
		if (IsClientInGame(iAttacker) && IsClientSurvivor(iAttacker) && GetClientTeam(iVictim) == 3)
			g_iLLM_PlayerSIKills[iAttacker]++;
	}

	int iCurTarget = g_iBot_TargetInfected[iAttacker];
	if (iCurTarget == iVictim || iCurTarget == iInfected)
		g_iBot_TargetInfected[iAttacker] = 0;

	for (int i = 1; i <= MaxClients; i++)
	{
		g_iBot_VisionMemory_State[i][0][iVictim] = g_iBot_VisionMemory_State[i][1][iVictim] = 0;
		g_fBot_VisionMemory_Time[i][0][iVictim] = g_fBot_VisionMemory_Time[i][1][iVictim] = 0.0;
	}

	g_bInfectedBot_IsThrowing[iVictim] = false;
	g_fInfectedBot_CoveredInVomitTime[iVictim] = GetGameTime();
}

void Event_OnIncap(Event hEvent, const char[] sName, bool bBroadcast)
{
	static int iClient, iUserID, iSecondarySlot, iEntRef, iIndex;
	iUserID = hEvent.GetInt("userid");
	iClient = GetClientOfUserId(iUserID);

	// LLM score: track incap
	if (iClient > 0 && iClient <= MaxClients && IsClientInGame(iClient))
		g_iLLM_PlayerIncap[iClient]++;

	iSecondarySlot = GetWeaponInInventory(iClient, 1);
	if (iSecondarySlot)
	{
		iEntRef = EntIndexToEntRef(iSecondarySlot);
		iIndex = g_hForbiddenItemList.Push(iEntRef);
		g_hForbiddenItemList.Set(iIndex, iClient, 1);
	}
}

void Event_OnRevive(Event hEvent, const char[] sName, bool bBroadcast)
{
	static int iClient, iUserID, iOwner, iEntIndex;
	iUserID = hEvent.GetInt("subject");
	iClient = GetClientOfUserId(iUserID);

	// LLM score: track revive (the one being revived)
	if (iClient > 0 && iClient <= MaxClients && IsClientInGame(iClient))
		g_iLLM_PlayerRevive[iClient]++;
	
	for (int i = 0; i < g_hForbiddenItemList.Length; i++)
	{
		iEntIndex = EntRefToEntIndex(g_hForbiddenItemList.Get(i));
		iOwner = g_hForbiddenItemList.Get(i, 1);
		if (iEntIndex == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iEntIndex) || iClient == iOwner)
		{
			g_hForbiddenItemList.Erase(i);
			continue;
		}
	}
}

// LLM score: track heal teammate
void Event_OnHealSuccess(Event hEvent, const char[] sName, bool bBroadcast)
{
	int subject = GetClientOfUserId(hEvent.GetInt("subject"));
	int healer = GetClientOfUserId(hEvent.GetInt("userid"));
	if (healer > 0 && healer <= MaxClients && IsClientInGame(healer) && subject != healer)
		g_iLLM_PlayerHeal[healer]++;
}

// LLM: Send round outcome to Python service for experience learning
void LLM_SendRoundOutcome(const char[] outcome)
{
	if (g_hLLM_Socket == null || !g_hLLM_Socket.Connected) return;
	char mapName[128]; GetCurrentMap(mapName, sizeof(mapName));
	char msg[256];
	Format(msg, sizeof(msg), "ROUND_OUTCOME {\"map\":\"%s\",\"outcome\":\"%s\"}\n", mapName, outcome);
	g_hLLM_Socket.Send(msg, strlen(msg));
	PrintToServer("[LLM] Round outcome sent: %s → %s", mapName, outcome);
}

void Event_OnRoundEnd(Event hEvent, const char[] sName, bool bBroadcast)
{
	int reason = hEvent.GetInt("reason");
	char outcome[16] = "survived";
	if (reason == 1) strcopy(outcome, sizeof(outcome), "team_wipe");  // survivors wiped
	else if (reason == 2) strcopy(outcome, sizeof(outcome), "escaped");  // survivors escaped
	LLM_SendRoundOutcome(outcome);
}

void Event_OnMissionLost(Event hEvent, const char[] sName, bool bBroadcast)
{
	LLM_SendRoundOutcome("team_wipe");
}

void Event_OnMapTransition(Event hEvent, const char[] sName, bool bBroadcast)
{
	LLM_SendRoundOutcome("escaped");
}

// Mark entity as used by certain client
void Event_OnPlayerUse(Event hEvent, const char[] sName, bool bBroadcast)
{
	static int iClient, iEntity;
	iClient = GetClientOfUserId(hEvent.GetInt("userid"));
	iEntity = hEvent.GetInt("targetid");
	
	g_iItem_Used[iEntity] |= (1 << (iClient - 1));
	if (IsFakeClient(iClient) && iEntity == g_iBot_ScavengeItem[iClient] && g_iWeaponID[iEntity])
	{
		ClearMoveToPosition(iClient, "ScavengeItem");
		g_iBot_ScavengeItem[iClient] = 0;
		g_fBot_ScavengeItemDist[iClient] = -1.0;
	}
}

void Event_OnSurvivorGrabbed(Event hEvent, const char[] sName, bool bBroadcast)
{
	int iVictim = GetClientOfUserId(hEvent.GetInt("victim"));
	if (!IsValidClient(iVictim))return;
	
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i) || !IsFakeClient(i))continue;
		float fReactTime = GetRandomFloat(0.33, 0.66); if (i == iVictim)fReactTime *= 0.5;
		g_fBot_PinnedReactTime[i] = GetGameTime() + fReactTime;
	}
}

void Event_OnAbilityUse(Event hEvent, const char[] sName, bool bBroadcast)
{
	static char szAbility[32];
	hEvent.GetString("ability", szAbility, sizeof(szAbility));

	if (strcmp(szAbility, "ability_spit") != 0)
		return;

	int iSpitter = GetClientOfUserId(hEvent.GetInt("userid"));
	if (!IsValidClient(iSpitter))return;

	int iNavArea;
	float fCurDist, fLastDist = MAX_MAP_RANGE_SQR;
	float fEscapePos[3], fPathPos[3];
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i) || !IsFakeClient(i) || !FEntityInViewAngle(iSpitter, i, 10.0) || !IsVisibleEntity(iSpitter, i, MASK_SHOT_HULL))
			continue;

		for (int j = 0; j < 12; j++)
		{
			LBI_TryGetPathableLocationWithin(i, 250.0 + (50.0 * j), fPathPos);
			if (!IsValidVector(fPathPos))continue;

			fCurDist = GetClientTravelDistance(i, fPathPos, true);
			if (fCurDist >= fLastDist)continue;

			iNavArea = L4D_GetNearestNavArea(fPathPos);
			if (!iNavArea || LBI_IsDamagingNavArea(iNavArea, true) || !LBI_IsReachableNavArea(i, iNavArea))
				continue;

			fLastDist = fCurDist;
			fEscapePos = fPathPos;
		}

		if (IsValidVector(fEscapePos))
			SetMoveToPosition(i, fEscapePos, 4, "EscapeInferno", 0.0, 5.0, true, true);
	}
}

void Event_OnChargeStart(Event hEvent, const char[] sName, bool bBroadcast)
{
	if (!g_bCvar_ChargerEvasion)return;

	int iCharger = GetClientOfUserId(hEvent.GetInt("userid"));
	if (!IsValidClient(iCharger))return;

	float fChargerForward[3]; GetClientAbsAngles(iCharger, fChargerForward);
	GetAngleVectors(fChargerForward, fChargerForward, NULL_VECTOR, NULL_VECTOR);
	NormalizeVector(fChargerForward, fChargerForward);

	int iMoveArea;
	float fChargeDist, fChargeHitDist;
	float fChargeHitPos[3], fClientRight[3], fMovePos[3];
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i) || !IsFakeClient(i) || !FEntityInViewAngle(iCharger, i, 5.0) || GetClientDistance(i, iCharger, true) <= 36864.0 || !IsVisibleEntity(iCharger, i, MASK_PLAYERSOLID))
			continue;

		MakeVectorFromPoints(g_fClientAbsOrigin[i], g_fClientAbsOrigin[iCharger], fClientRight);
		GetAngleVectors(fClientRight, NULL_VECTOR, fClientRight, NULL_VECTOR);
		NormalizeVector(fClientRight, fClientRight);

		fChargeDist = GetVectorDistance(g_fClientCenteroid[i], g_fClientCenteroid[iCharger]);
		fChargeHitPos[0] = g_fClientCenteroid[iCharger][0] + (fChargerForward[0] * fChargeDist);
		fChargeHitPos[1] = g_fClientCenteroid[iCharger][1] + (fChargerForward[1] * fChargeDist);
		fChargeHitPos[2] = g_fClientCenteroid[iCharger][2] + (fChargerForward[2] * fChargeDist);

		fChargeHitDist = GetVectorDistance(g_fClientCenteroid[i], fChargeHitPos);
		for (int k = 1; k <= 2; k++)
		{
			fMovePos[0] = g_fClientAbsOrigin[i][0] + (fClientRight[0] * ((k == 1 ? 256.0 : -256.0) - fChargeHitDist));
			fMovePos[1] = g_fClientAbsOrigin[i][1] + (fClientRight[1] * ((k == 1 ? 256.0 : -256.0) - fChargeHitDist));
			fMovePos[2] = g_fClientAbsOrigin[i][2] + (fClientRight[2] * ((k == 1 ? 256.0 : -256.0) - fChargeHitDist));

			iMoveArea = L4D_GetNearestNavArea(fMovePos);
			if (iMoveArea)
			{
				LBI_GetClosestPointOnNavArea(iMoveArea, fMovePos, fMovePos);
				if (!FVectorInViewAngle(iCharger, fMovePos, 5.0) && LBI_IsReachableNavArea(i, iMoveArea))
				{
					float fMoveDist = GetClientTravelDistance(i, fMovePos, true);
					if (fMoveDist != -1.0 && fMoveDist <= 147456.0)
					{
						SetMoveToPosition(i, fMovePos, 3, "EvadeCharge");
						break;
					}
				}
			}

			if (k == 2)TakeCoverFromEntity(i, iCharger, 512.0);
		}
	}
}

void Event_OnWitchHaraserSet(Event hEvent, const char[] sName, bool bBroadcast)
{
	int iUserID = hEvent.GetInt("userid");
	int iClient = GetClientOfUserId(iUserID);
	if (!IsClientSurvivor(iClient))return;

	int iWitch = hEvent.GetInt("witchid");
	static int iWitchRef;

	for (int i = 0; i < g_hWitchList.Length; i++)
	{
		iWitchRef = EntRefToEntIndex(g_hWitchList.Get(i));
		if (iWitchRef == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iWitchRef))
		{
			g_hWitchList.Erase(i);
			continue;
		}

		if (iWitchRef == iWitch)
		{
			g_hWitchList.Set(i, iUserID, 1);
			break;
		}
	}
}

Action CmdDumpCvars(int client, int args)
{
	static char szOutputBuffer[64];
	
	GetConVarString(g_hCvar_AutoShove_Enabled, szOutputBuffer, 64);
	PrintToServer("g_hCvar_AutoShove_Enabled %s", szOutputBuffer);
	PrintToServer("g_iCvar_AutoShove_Enabled %d", g_iCvar_AutoShove_Enabled);
	
	GetConVarString(g_hCvar_ImprovedMelee_Enabled, szOutputBuffer, 64);
	PrintToServer("g_hCvar_ImprovedMelee_Enabled %s", szOutputBuffer);
	PrintToServer("g_bCvar_ImprovedMelee_Enabled %b", g_bCvar_ImprovedMelee_Enabled);
	
	GetConVarString(g_hCvar_TargetSelection_Enabled, szOutputBuffer, 64);
	PrintToServer("g_hCvar_TargetSelection_Enabled %s", szOutputBuffer);
	PrintToServer("g_bCvar_TargetSelection_Enabled %b", g_bCvar_TargetSelection_Enabled);
	
	GetConVarString(g_hCvar_ItemScavenge_Items, szOutputBuffer, 64);
	PrintToServer("g_hCvar_ItemScavenge_Items %s", szOutputBuffer);
	PrintToServer("g_iCvar_ItemScavenge_Items %d", g_iCvar_ItemScavenge_Items);
	return Plugin_Handled;
}

public Action OnPlayerRunCmd(int iClient, int &iButtons, int &iImpulse, float fVel[3], float fAngles[3])
{
	// Skip fake clients that aren't properly in game (e.g. LLM_Spectator)
	if (!IsClientInGame(iClient))
		return Plugin_Continue;

	GetClientEyePosition(iClient, g_fClientEyePos[iClient]);
	g_fClientEyeAng[iClient] = fAngles;
	GetClientAbsOrigin(iClient, g_fClientAbsOrigin[iClient]);
	GetEntityCenteroid(iClient, g_fClientCenteroid[iClient]);
	g_iClientNavArea[iClient] = L4D_GetLastKnownArea(iClient);
	
	if (!IsClientSurvivor(iClient))
		return Plugin_Continue;

	g_bClient_IsFiringWeapon[iClient] = false;
	g_bClient_IsLookingAtPosition[iClient] = false;
	
	g_iClientInvFlags[iClient] = 0;

	int iWpnSlot, iWpnSlots[6];

	// instead of comparing strings
	// we represent survivors’ inventory as bit flags
	for (int i = 0; i < 6; i++)
	{
		iWpnSlot = GetPlayerWeaponSlot(iClient, i);
		if (L4D_IsValidEnt(iWpnSlot))
		{
			iWpnSlots[i] = iWpnSlot;
			g_iClientInvFlags[iClient] |= g_iItemFlags[iWpnSlot];

			if (g_iWeaponID[iWpnSlot] == 1 
			&& (GetEntProp(iWpnSlot, Prop_Send, "m_isDualWielding") != 0 || GetEntProp(iWpnSlot, Prop_Send, "m_hasDualWeapons") != 0))
				g_iClientInvFlags[iClient] |= FLAG_PISTOL_EXTRA;
		}
		else
			iWpnSlots[i] = 0;
	}
	g_iClientInventory[iClient] = iWpnSlots;

	if (iWpnSlots[0])
	{
		g_iWeapon_Clip1[iWpnSlots[0]] = GetWeaponClip1(iWpnSlots[0]);
		g_iWeapon_MaxAmmo[iWpnSlots[0]] = GetWeaponMaxAmmo(iWpnSlots[0]);
		g_iWeapon_AmmoLeft[iWpnSlots[0]] = GetClientPrimaryAmmo(iClient);

		int iDHookID = g_iWeapon_PrimaryAttackHook[iWpnSlots[0]];
		if (iDHookID == 0)
		{
			iDHookID = DHookEntity(g_hOnWpnPrimaryAttack, false, iWpnSlots[0]);
			if (iDHookID == INVALID_HOOK_ID)
				g_iWeapon_PrimaryAttackHook[iWpnSlots[0]] = -1;
			else
				g_iWeapon_PrimaryAttackHook[iWpnSlots[0]] = iDHookID;
		}
	}
	if (iWpnSlots[1])
	{
		g_iWeapon_Clip1[iWpnSlots[1]] = GetWeaponClip1(iWpnSlots[1]);

		int iDHookID = g_iWeapon_PrimaryAttackHook[iWpnSlots[1]];
		if (iDHookID == 0)
		{
			iDHookID = DHookEntity(g_hOnWpnPrimaryAttack, false, iWpnSlots[1]);
			if (iDHookID == INVALID_HOOK_ID)
				g_iWeapon_PrimaryAttackHook[iWpnSlots[1]] = -1;
			else
				g_iWeapon_PrimaryAttackHook[iWpnSlots[1]] = iDHookID;
		}
	}

	if (!IsFakeClient(iClient))
		return Plugin_Continue;

	if (g_bCvar_BotsDisabled || g_bCutsceneIsPlaying || !g_iClientNavArea[iClient] || GetGameTime() <= g_fClient_ThinkFunctionDelay[iClient])
		return Plugin_Continue;

	// -------------------------

	if (g_iHasLeft4Bots == -1)
	{
		g_iHasLeft4Bots = 0;

		char sCodeBuffer[CODE_BUFFER_LENGTH];
		FormatEx(sCodeBuffer, CODE_BUFFER_LENGTH, 
			"if ((\"Left4Bots\" in getroottable()))\
				<RETURN>\"True\"</RETURN>"
		);

		char sOutputBuffer[CODE_BUFFER_LENGTH];
		L4D2_GetVScriptOutput(sCodeBuffer, sOutputBuffer, CODE_BUFFER_LENGTH);

		if (sOutputBuffer[0] != EOS)
			g_iHasLeft4Bots = 1;
	}

	// -------------------------

	static int iGameDifficulty, iAliveBots;
	static bool bShouldUseFlow;

	iAliveBots = SurvivorBotThink(iClient, iButtons, iWpnSlots, g_iClientInvFlags[iClient], iGameDifficulty, bShouldUseFlow);

	// LLM: Apply strategic overrides after IB's normal processing
	LLM_ApplyOverrides(iClient, iButtons, fVel, fAngles);

	// LLM: Send state periodically (call for all survivor bots to find tracked bot)
	if (IsClientSurvivor(iClient)) LLM_SendState();
	if (g_iBotProcessing_ProcessedCount >= iAliveBots)
	{
		iGameDifficulty = GetCurrentGameDifficulty();
		bShouldUseFlow = ShouldUseFlowDistance();

		g_iBotProcessing_ProcessedCount = 0;
		for (int i = 1; i <= MaxClients; i++)
			g_bBotProcessing_IsProcessed[i] = false;
	}
	
	return Plugin_Continue;
}

public void L4D_OnForceSurvivorPositions()
{
	g_bCutsceneIsPlaying = true;
}

public void L4D_OnReleaseSurvivorPositions()
{
	g_bCutsceneIsPlaying = false;
}

int GetWeaponInInventory(int iClient, int iSlot)
{
	int iWpn = g_iClientInventory[iClient][iSlot];
	return (!L4D_IsValidEnt(iWpn) ? 0 : iWpn);
}

public void L4D_OnCThrowActivate_Post(int iAbility)
{
	int iOwner = GetEntityOwner(iAbility);
	if (IsValidClient(iOwner))g_bInfectedBot_IsThrowing[iOwner] = true;
}

public void L4D_TankRock_OnRelease_Post(int iTank, int iRock, const float fVecPos[3], const float fVecAng[3], const float fVecVel[3], const float fVecRot[3])
{
	if (IsValidClient(iTank))g_bInfectedBot_IsThrowing[iTank] = false;
}

public void L4D2_OnStagger_Post(int iTarget, int iSource)
{
	g_bInfectedBot_IsThrowing[iTarget] = false;
}

stock void VScript_DebugDrawLine(const float fStartPos[3], const float fEndPos[3], int iColorR = 255, int iColorG = 255, int iColorB = 255, bool bZTest = false, float fDrawTime = 1.0)
{
	static char sScriptCode[256]; FormatEx(sScriptCode, sizeof(sScriptCode), "DebugDrawLine(Vector(%f, %f, %f), Vector(%f, %f, %f), %i, %i, %i, %s, %f)",
		fStartPos[0], fStartPos[1], fStartPos[2], fEndPos[0], fEndPos[1], fEndPos[2], iColorR, iColorG, iColorB, (bZTest ? "true" : "false"), fDrawTime);
	L4D2_ExecVScriptCode(sScriptCode);
}

stock void VScript_DebugDrawText(const float fOrigin[3], const char[] sText, bool bUseViewCheck = true, float fDrawTime = 1.0)
{
	static char sScriptCode[256]; FormatEx(sScriptCode, sizeof(sScriptCode), "DebugDrawText(Vector(%f, %f, %f), \"%s\", %s, %f)",
		fOrigin[0], fOrigin[1], fOrigin[2], sText, (bUseViewCheck ? "true" : "false"), fDrawTime);
	L4D2_ExecVScriptCode(sScriptCode);
}

void OnGrenadeThrown(int iClient, bool bIsMolotov)
{
	if (!(1 <= iClient <= MaxClients))
		return;

	g_iClientInventory[iClient][2] = 0;

	if (bIsMolotov)
		g_fBot_Grenade_NextThrowTime_Molotov = GetGameTime() + GetRandomFloat(g_fCvar_GrenadeThrow_NextThrowTime1, g_fCvar_GrenadeThrow_NextThrowTime2);
	else
		g_fBot_Grenade_NextThrowTime = GetGameTime() + GetRandomFloat(g_fCvar_GrenadeThrow_NextThrowTime1, g_fCvar_GrenadeThrow_NextThrowTime2);

	// PrintToServer("%N = %f, %d", iClient,
	// 	(bIsMolotov ? g_fBot_Grenade_NextThrowTime_Molotov : g_fBot_Grenade_NextThrowTime) - GetGameTime(),
	// 	bIsMolotov
	// );
}
public void L4D_PipeBombProjectile_Post(int client, int projectile, const float vecPos[3], const float vecAng[3], const float vecVel[3], const float vecRot[3])
{
	OnGrenadeThrown(client, false);
}
public void L4D2_VomitJarProjectile_Post(int client, int projectile, const float vecPos[3], const float vecAng[3], const float vecVel[3], const float vecRot[3])
{
	OnGrenadeThrown(client, false);
}
public void L4D_MolotovProjectile_Post(int client, int projectile, const float vecPos[3], const float vecAng[3], const float vecVel[3], const float vecRot[3])
{
	OnGrenadeThrown(client, true);
}

int SurvivorBotThink(int iClient, int &iButtons, int iWpnSlots[6], int iInvFlags, int iGameDifficulty, bool bShouldUseFlow)
{
	float fCurTime = GetGameTime();

	static int iAliveBots = 0;
	static int iTeamLeader, iTeamCount, iAlivePlayers;
	static int iWitchRef, iHarasserRef, iWitchHarasser;
	static float fLeaderDist;
	static bool bClientIsAttacking;

	static int iClientTeam, iClientAttacker;
	static L4D2ZombieClassType iClientClass;
	static float fLastDist, fClientDist[5];
	static bool bClientIsBot, bClientIsUsingAbility;

	static int iFindEntRef, iThrownPipeBomb;
	static float fCurDist, fTargetDist, fInfectedPos[3];
	static bool bBileWasThrown, bInfectedIsChasing, bInfectedIsVisible;

	// -------------------------

	if (!g_bBotProcessing_IsProcessed[iClient] && fCurTime >= g_fBotProcessing_NextProcessTime)
	{
		g_iBot_TankTarget[iClient] = 0;
		g_iBot_TankProp[iClient] = 0;
		g_iBot_TankRock[iClient] = 0;

		g_iBot_DefibTarget[iClient] = 0;
		g_iBot_PinnedFriend[iClient] = 0;
		g_iBot_PinnedFriend_Attacker[iClient] = 0;
		g_iBot_IncapacitatedFriend[iClient] = 0;
		g_iBot_WitchTarget[iClient] = 0;

		g_bBot_IsFriendNearThrowArea[iClient] = false;
		g_bBot_IsFriendNearBoomer[iClient] = false;

		g_iBot_NearbyFriends[iClient] = 0;
		g_iBot_ThreatInfectedCount[iClient] = 0;
		g_iBot_NearestInfectedCount[iClient] = 0;
		g_iBot_NearbyInfectedCount[iClient] = 0;
		g_iBot_GrenadeInfectedCount[iClient] = 0;

		g_bBotProcessing_IsProcessed[iClient] = true;
		g_iBotProcessing_ProcessedCount++;
		g_fBotProcessing_NextProcessTime = (fCurTime + g_fCvar_NextProcessTime);

		// -------------------------

		g_bBot_IsInCombat[iClient] = (SDKCall(g_hIsInCombat, iClient, false));
		g_bBot_IsAvailable[iClient] = (SDKCall(g_hIsAvailable, iClient));

		// -------------------------

		fTargetDist = 16777216.0; // 4096
		iThrownPipeBomb = (FindEntityByClassname(-1, "pipe_bomb_projectile"));
		bBileWasThrown = (FindEntityByClassname(-1, "info_goal_infected_chase") != -1);

		iFindEntRef = INVALID_ENT_REFERENCE;
		while ((iFindEntRef = FindEntityByClassname(iFindEntRef, "infected")) != INVALID_ENT_REFERENCE)
		{
			if (!IsCommonAlive(iFindEntRef) || !IsCommonAttacking(iFindEntRef))
				continue;
			
			GetEntityCenteroid(iFindEntRef, fInfectedPos);
			fCurDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fInfectedPos, true);
			bInfectedIsVisible = IsVisibleVector(iClient, fInfectedPos, MASK_VISIBLE_AND_NPCS);

			if (g_bCvar_GrenadeThrow_Enabled && iWpnSlots[2] && 
				(fCurDist <= g_fCvar_GrenadeThrow_ThrowRange_NoVisCheck || 
				bInfectedIsVisible && fCurDist <= g_fCvar_GrenadeThrow_ThrowRange_Sqr)
			)
				g_iBot_GrenadeInfectedCount[iClient]++;

			if (!bInfectedIsVisible)
				continue;

			if (fCurDist <= 15625.0) // 125
				g_iBot_ThreatInfectedCount[iClient]++;
			if (fCurDist <= 90000.0) // 300
				g_iBot_NearestInfectedCount[iClient]++;
			if (fCurDist <= 250000.0) // 500
				g_iBot_NearbyInfectedCount[iClient]++;

			if (fCurDist > fTargetDist)
				continue;

			bInfectedIsChasing = (fCurDist > 25600.0 && (bBileWasThrown || iThrownPipeBomb > 0 && GetEntityDistance(iFindEntRef, iThrownPipeBomb, true) <= 65536.0)); //160 & 256
			if (!bInfectedIsChasing && fCurDist > 9216.0) // 96
			{
				for (int i = 1; i <= MaxClients; i++)
				{
					bInfectedIsChasing = (iClient != i && IsClientSurvivor(i) && g_iBot_TargetInfected[i] == iFindEntRef && IsWeaponSlotActive(i, 1) && g_iBot_TargetInfected[i] && SurvivorHasMeleeWeapon(i) != 0);
					if (bInfectedIsChasing)break;
				}
			}

			if (bInfectedIsChasing)
				continue;

			g_iBot_TargetInfected[iClient] = iFindEntRef;
			fTargetDist = fCurDist;
		}

		// -------------------------

		iTeamLeader = iClient;
		bClientIsAttacking = false;
		g_bTeamHasHumanPlayer = false;

		iTeamCount = 0;
		iAlivePlayers = 1; // One is us
		iAliveBots = 0;

		fClientDist[0] = MAX_MAP_RANGE_SQR; // Pinned By Special Friend
		fClientDist[1] = MAX_MAP_RANGE_SQR; // Incapacitated Friend
		fClientDist[2] = MAX_MAP_RANGE_SQR; // Tank
		fClientDist[3] = MAX_MAP_RANGE_SQR; // Leader, No Flow
		fClientDist[4] = 0.0; // Leader, Flow

		for (int i = 1; i <= MaxClients; i++)
		{
			if (!IsClientInGame(i))
				continue;

			iClientTeam = GetClientTeam(i);
			if (iClientTeam == 2)iTeamCount++;

			if (!IsPlayerAlive(i))continue;
			fCurDist = GetClientDistance(iClient, i, true);
	
			if (iClientTeam == 2)
			{
				bClientIsBot = IsFakeClient(i);
				if (bClientIsBot)iAliveBots++;

				if (!g_bTeamHasHumanPlayer || !bClientIsBot)
				{
					if (bShouldUseFlow && !g_bTeamHasHumanPlayer)
					{
						fLeaderDist = L4D2Direct_GetFlowDistance(i);
						if (fLeaderDist > fClientDist[4])
						{
							iTeamLeader = i;
							fClientDist[4] = fLeaderDist;

							if (!g_bTeamHasHumanPlayer)
								g_bTeamHasHumanPlayer = !bClientIsBot;
						}
					}
					else
					{
						fLeaderDist = fCurDist;
						if (fLeaderDist < fClientDist[3])
						{
							iTeamLeader = i;
							fClientDist[3] = fLeaderDist;

							if (!g_bTeamHasHumanPlayer)
								g_bTeamHasHumanPlayer = !bClientIsBot;
						}
					}
				}

				if (i == iClient)continue;
				iAlivePlayers++;

				iClientAttacker = L4D_GetPinnedInfected(i);
				if (fCurDist < fClientDist[0] && iClientAttacker > 0)
				{
					g_iBot_PinnedFriend[iClient] = i;
					g_iBot_PinnedFriend_Attacker[iClient] = iClientAttacker;
					fClientDist[0] = fCurDist;
				}

				if (fCurDist < fClientDist[1] && L4D_IsPlayerIncapacitated(i))
				{
					g_iBot_IncapacitatedFriend[iClient] = i;
					fClientDist[1] = fCurDist;
				}

				if (!g_bBot_IsFriendNearBoomer[iClient])
				{
					for (int j = 1; j <= MaxClients; j++)
					{
						g_bBot_IsFriendNearBoomer[iClient] = (j != iClient && j != i && IsClientInGame(j) && GetClientTeam(j) == 3 && IsPlayerAlive(j) && !L4D_IsPlayerGhost(j) && 
							GetClientDistance(i, j, true) <= BOT_BOOMER_AVOID_RADIUS_SQR && L4D2_GetPlayerZombieClass(j) == L4D2ZombieClass_Boomer && IsVisibleEntity(j, i, MASK_VISIBLE_AND_NPCS)
						);
						if (g_bBot_IsFriendNearBoomer[iClient])break;
					}
				}

				if (fCurDist <= 262144.0 && !L4D_IsPlayerIncapacitated(i) && !L4D_IsPlayerPinned(i)) // 512
					g_iBot_NearbyFriends[iClient]++;

				if (!g_bBot_IsFriendNearThrowArea[iClient] && g_iBot_Grenade_ThrowTarget[iClient])
					g_bBot_IsFriendNearThrowArea[iClient] = (GetVectorDistance(g_fClientAbsOrigin[i], g_fBot_Grenade_ThrowPos[iClient], true) <= BOT_GRENADE_CHECK_RADIUS_SQR);
			}
			else if (iClientTeam == 3 && !L4D_IsPlayerGhost(i))
			{
				iClientClass = L4D2_GetPlayerZombieClass(i);
				if (iClientClass == L4D2ZombieClass_Tank)
				{
					if (fCurDist < fClientDist[2] && IsValidClient(g_iInfectedBot_CurrentVictim[i]) && !L4D_IsPlayerIncapacitated(i) && (fCurDist <= 1048576.0 || fCurDist <= 16777216.0 && IsVisibleEntity(iClient, i))) // 1024 & 4096
					{
						g_iBot_TankTarget[iClient] = i;
						fClientDist[2] = fCurDist;
					}
				}
				else 
				{
					bClientIsUsingAbility = IsUsingSpecialAbility(i);
					if (fCurDist < fTargetDist && (!bClientIsAttacking || bClientIsUsingAbility) && IsVisibleEntity(iClient, i))
					{
						g_iBot_TargetInfected[iClient] = i;
						fTargetDist = fCurDist;
						bClientIsAttacking = bClientIsUsingAbility;
					}
				}
			}
		}

		// -------------------------

		if (g_bCvar_DefibRevive_Enabled)
		{			
			fLastDist = MAX_MAP_RANGE_SQR;
			
			iFindEntRef = INVALID_ENT_REFERENCE;
			while ((iFindEntRef = FindEntityByClassname(iFindEntRef, "survivor_death_model")) != INVALID_ENT_REFERENCE)
			{
				fCurDist = GetEntityDistance(iClient, iFindEntRef, true);
				if (fCurDist < fLastDist)
				{
					g_iBot_DefibTarget[iClient] = iFindEntRef;				
					fLastDist = fCurDist;
				}
			}
		}

		if (g_iBot_TankTarget[iClient])
		{
			fLastDist = MAX_MAP_RANGE_SQR;
			iFindEntRef = INVALID_ENT_REFERENCE;
			while ((iFindEntRef = FindEntityByClassname(iFindEntRef, "tank_rock")) != INVALID_ENT_REFERENCE)
			{
				fCurDist = GetEntityDistance(iClient, iFindEntRef, true);
				if (fCurDist >= fLastDist || !IsVisibleEntity(iClient, iFindEntRef))
					continue;

				g_iBot_TankRock[iClient] = iFindEntRef;
				fLastDist = fCurDist;
			}

			fLastDist = MAX_MAP_RANGE_SQR;
			iFindEntRef = INVALID_ENT_REFERENCE;
			while ((iFindEntRef = FindEntityByClassname(iFindEntRef, "prop_car_alarm")) != INVALID_ENT_REFERENCE)
			{
				fCurDist = GetEntityDistance(g_iBot_TankTarget[iClient], iFindEntRef, true);
				if (fCurDist > 250000.0 || fCurDist >= fLastDist || !IsVisibleEntity(g_iBot_TankTarget[iClient], iFindEntRef) || !IsVisibleEntity(iClient, iFindEntRef)) // 500
					continue;

				g_iBot_TankProp[iClient] = iFindEntRef;
				fLastDist = fCurDist;
			}
			while ((iFindEntRef = FindEntityByClassname(iFindEntRef, "prop_physics")) != INVALID_ENT_REFERENCE)
			{
				fCurDist = GetEntityDistance(g_iBot_TankTarget[iClient], iFindEntRef, true);
				if (fCurDist > 250000.0 || fCurDist >= fLastDist || !GetEntProp(iFindEntRef, Prop_Send, "m_hasTankGlow") || !IsVisibleEntity(g_iBot_TankTarget[iClient], iFindEntRef) || !IsVisibleEntity(iClient, iFindEntRef)) // 500
					continue;

				g_iBot_TankProp[iClient] = iFindEntRef;
				fLastDist = fCurDist;
			}
		}

		fLastDist = MAX_MAP_RANGE_SQR;
		for (int i = 0; i < g_hWitchList.Length; i++)
		{
			iWitchRef = EntRefToEntIndex(g_hWitchList.Get(i));
			if (iWitchRef == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iWitchRef))
			{
				g_hWitchList.Erase(i);
				continue;
			}

			iHarasserRef = g_hWitchList.Get(i, 1);
			if (iHarasserRef != -1)
				iHarasserRef = GetClientOfUserId(iHarasserRef);

			if (iWitchHarasser && !iHarasserRef)
				continue;

			fCurDist = GetEntityDistance(iClient, iWitchRef, true);
			if (fCurDist >= fLastDist)continue;
			fLastDist = fCurDist;

			g_iBot_WitchTarget[iClient] = iWitchRef;
			iWitchHarasser = iHarasserRef;
		}
		g_bBot_IsWitchHarasser[iClient] = (iWitchHarasser == iClient);

		if (fCurTime > g_fBot_NextScavengeItemScanTime[iClient])
		{
			g_iBot_ScavengeItem[iClient] = CheckForItemsToScavenge(iClient);		
			g_fBot_NextScavengeItemScanTime[iClient] = (fCurTime + 1.0);
		}

		// PrintToServer("[%i] %N's Infected Count:\nThreat (125 hu.): %i\nNearest (300 hu.): %i\nNearby (500 hu.): %i\nFor Grenades (%.0f hu): %i\nShove Penalty: %i", 
		// 	g_iBotProcessing_ProcessedCount, iClient,
		// 	g_iBot_ThreatInfectedCount[iClient],
		// 	g_iBot_NearestInfectedCount[iClient],
		// 	g_iBot_NearbyInfectedCount[iClient],
		// 	g_fCvar_GrenadeThrow_ThrowRange,
		// 	g_iBot_GrenadeInfectedCount[iClient],
		// 	L4D_GetPlayerShovePenalty(iClient)
		// );
	}

	// -------------------------

	if (L4D_IsPlayerHangingFromLedge(iClient))
		return iAliveBots;

	if (GetEntityMoveType(iClient) == MOVETYPE_LADDER)
	{
		g_bBot_WasUsingLadder[iClient] = true;
		return iAliveBots;
	}

	if (g_bBot_WasUsingLadder[iClient] && GetEntityFlags(iClient) & FL_ONGROUND)
	{
		g_bBot_WasUsingLadder[iClient] = false;
	}

	if (IsValidVector(g_fBot_LookPosition[iClient]))
	{ 
		if (fCurTime < g_fBot_LookPosition_Duration[iClient])
			SnapViewToPosition(iClient, g_fBot_LookPosition[iClient]);
		else
			SetVectorToZero(g_fBot_LookPosition[iClient]);
	}

	int iCurWeapon = L4D_GetPlayerCurrentWeapon(iClient);
	if (iCurWeapon)
	{
		if (g_bBot_ForceWeaponReload[iClient])
		{
			iButtons |= IN_RELOAD;
			g_bBot_ForceWeaponReload[iClient] = false;
		}
		else if (fCurTime <= g_fBot_BlockWeaponReloadTime[iClient])
		{
			iButtons &= ~IN_RELOAD;
		}

		if (!SurvivorBot_CanFreelyFireWeapon(iClient))
			g_fBot_PreventFireTime[iClient] = (GetGameTime() + GetRandomFloat(0.1, 0.25));

		if (fCurTime <= g_fBot_PreventFireTime[iClient])
			iButtons &= ~IN_ATTACK;

		if (g_bBot_ForceBash[iClient])
		{
			g_bBot_ForceBash[iClient] = false;
			iButtons |= IN_ATTACK2;
		}
		
		if (iCurWeapon == iWpnSlots[0])
		{
			// if nightmare, don't reload our primary weapon 'til its clip is zero
			if (g_bCvar_Nightmare)
			{
				if (g_iWeapon_Clip1[iCurWeapon] > 0 && !(iInvFlags & FLAG_SHOTGUN))
					iButtons &= ~IN_RELOAD;

				int iTarget = (L4D_IsValidEnt(g_iBot_TargetInfected[iClient]) ? g_iBot_TargetInfected[iClient] : 0);
				if (fCurTime <= g_fBot_PreventFireTime[iClient] && !g_iBot_PinnedFriend[iClient] && !g_iBot_IncapacitatedFriend[iClient] && !g_iBot_WitchTarget[iClient] && !g_iBot_TankTarget[iClient] && 
						iTarget && !(1 <= iTarget <= MaxClients) && 
						(iInvFlags & (FLAG_PISTOL | FLAG_PISTOL_EXTRA) && g_iBot_NearbyInfectedCount[iClient] <= GetCommonHitsUntilDown(iClient) && 
						GetEntityDistance(iClient, iTarget, true) <= 262144.0 // 512
					|| iInvFlags & FLAG_MELEE && GetEntityDistance(iClient, iTarget, true) <= g_fCvar_ImprovedMelee_ApproachRange)
				)
					SwitchWeaponSlot(iClient, 1);
			}

			if (GetBotWeaponPreference(iClient) == L4D_WEAPON_PREFERENCE_SECONDARY)
			{
				SwitchWeaponSlot(iClient, 1);
			}
			// if we're not occupied and our primary is loaded, switch to pistol (to reload)
			else if (fCurTime > GetWeaponNextFireTime(iCurWeapon) && !IsWeaponReloading(iCurWeapon, false)
				&& LBI_IsSurvivorBotAvailable(iClient) && !LBI_IsSurvivorInCombat(iClient) && ( iInvFlags & FLAG_M60 || GetWeaponClip1(iCurWeapon) == GetWeaponClipSize(iCurWeapon) )
				&& iInvFlags & (FLAG_PISTOL | FLAG_PISTOL_EXTRA) && GetWeaponClip1(iWpnSlots[1]) != GetWeaponClipSize(iWpnSlots[1]))
			{
				g_bBot_ForceSwitchWeapon[iClient] = true;
				SwitchWeaponSlot(iClient, 1);
			}
		}
	}

	int iPinnedFriend = g_iBot_PinnedFriend[iClient];
	int iPinnedAttacker = g_iBot_PinnedFriend_Attacker[iClient];
	if (!IsValidClient(iPinnedFriend) || !IsValidClient(iPinnedAttacker) || !IsPlayerAlive(iPinnedAttacker))
	{
		iPinnedFriend = g_iBot_PinnedFriend[iClient] = 0;
		iPinnedAttacker = g_iBot_PinnedFriend_Attacker[iClient] = 0;
	}
	else if (fCurTime > g_fBot_PinnedReactTime[iClient])
	{
		static float fAttackerAimPos[3];
		GetTargetAimPart(iClient, iPinnedAttacker, fAttackerAimPos);			

		float fFriendDist = GetVectorDistance(g_fClientEyePos[iClient], g_fClientCenteroid[iPinnedFriend], true);
		bool bCanShoot = (g_iCvar_HelpPinnedFriend_Enabled & (1 << 0) != 0);
		if (bCanShoot)
		{
			bCanShoot = (iCurWeapon && fFriendDist <= g_fCvar_HelpPinnedFriend_ShootRange_Sqr
				&& (iCurWeapon != iWpnSlots[1] || !SurvivorHasMeleeWeapon(iClient) || GetClientDistance(iClient, iPinnedAttacker, true) <= g_fCvar_ImprovedMelee_ApproachRange)
				&& SurvivorBot_AbleToShootWeapon(iClient) && CheckIfCanRescueImmobilizedFriend(iClient)
			);
		}

		int iCanShove;
		if (g_iCvar_HelpPinnedFriend_Enabled & (1 << 1) != 0)
			iCanShove = (fFriendDist <= g_fCvar_HelpPinnedFriend_ShoveRange_Sqr ? 1 : (GetVectorDistance(g_fClientEyePos[iClient], g_fClientCenteroid[iPinnedAttacker], true) <= g_fCvar_HelpPinnedFriend_ShoveRange_Sqr ? 2 : 0));

		L4D2ZombieClassType iZombieClass = L4D2_GetPlayerZombieClass(iPinnedAttacker);
		if (iZombieClass != L4D2ZombieClass_Smoker)
		{
			if (iWpnSlots[0] && GetWeaponAmmoLeft(iWpnSlots[0]) > 0 && iCurWeapon == iWpnSlots[1] && SurvivorHasMeleeWeapon(iClient) && fFriendDist > g_fCvar_ImprovedMelee_SwitchRange)
			{
				g_bBot_ForceSwitchWeapon[iClient] = true;
				SwitchWeaponSlot(iClient, 0);
			}
			else if (bCanShoot && fFriendDist <= 262144.0 && iCurWeapon == iWpnSlots[0] && IsWeaponReloading(iCurWeapon) && GetWeaponClip1(iWpnSlots[1]) > 0 && !SurvivorHasShotgun(iClient) && !SurvivorHasSniperRifle(iClient) && !SurvivorHasMeleeWeapon(iClient)) // 512
			{
				g_bBot_ForceSwitchWeapon[iClient] = true;
				SwitchWeaponSlot(iClient, 1);
			}

			if (iCanShove != 0 && iZombieClass != L4D2ZombieClass_Charger && !L4D_IsPlayerIncapacitated(iClient))
			{
				SnapViewToPosition(iClient, (iCanShove == 1 ? g_fClientCenteroid[iPinnedFriend] : g_fClientCenteroid[iPinnedAttacker]));
				iButtons |= IN_ATTACK2;
			}
			else if (bCanShoot && HasVisualContactWithEntity(iClient, iPinnedAttacker, false, fAttackerAimPos))
			{
				SnapViewToPosition(iClient, fAttackerAimPos);
				PressAttackButton(iClient, iButtons); // help pinned
			}
		}
		else
		{
			if (iCanShove != 0 && !L4D_IsPlayerIncapacitated(iClient))
			{
				SnapViewToPosition(iClient, (iCanShove == 1 ? g_fClientCenteroid[iPinnedFriend] : g_fClientCenteroid[iPinnedAttacker]));
				iButtons |= IN_ATTACK2;
			}
			else if (bCanShoot)
			{
				if (HasVisualContactWithEntity(iClient, iPinnedAttacker, false, fAttackerAimPos))
				{
					SnapViewToPosition(iClient, fAttackerAimPos);
					PressAttackButton(iClient, iButtons); // help pinned
				}
				else 
				{
					float fTipPos[3]; GetEntPropVector(L4D_GetPlayerCustomAbility(iPinnedAttacker), Prop_Send, "m_tipPosition", fTipPos);
					if (!IsValidVector(fTipPos))fTipPos = g_fClientEyePos[iPinnedFriend];	

					if (IsVisibleVector(iClient, fTipPos))
					{
						float fMidPos[3];
						fMidPos[0] = ((g_fClientEyePos[iPinnedAttacker][0] + fTipPos[0]) / 2.0);
						fMidPos[1] = ((g_fClientEyePos[iPinnedAttacker][1] + fTipPos[1]) / 2.0);
						fMidPos[2] = ((g_fClientEyePos[iPinnedAttacker][2] + fTipPos[2]) / 2.0);

						SnapViewToPosition(iClient, (IsVisibleVector(iClient, fMidPos) ? fMidPos : fTipPos));
						PressAttackButton(iClient, iButtons); // help pinned
					}
				}
			}
		}
	}
	
	if (IsValidVector(g_fBot_MovePos_Position[iClient]))
	{
		float fMovePos[3]; fMovePos = g_fBot_MovePos_Position[iClient];
		float fMoveDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fMovePos, true);						
		float fMoveTolerance = g_fBot_MovePos_Tolerance[iClient];
		float fMoveDuration = g_fBot_MovePos_Duration[iClient];

		if (fCurTime > fMoveDuration || fMoveTolerance >= 0.0 && fMoveDist <= (fMoveTolerance*fMoveTolerance) || 
			!g_bBot_MovePos_IgnoreDamaging[iClient] && LBI_IsDamagingPosition(fMovePos) || !LBI_IsReachablePosition(iClient, fMovePos, false) || 
			iPinnedFriend && L4D_GetPinnedInfected(iPinnedFriend) && L4D2_GetPlayerZombieClass(L4D_GetPinnedInfected(iPinnedFriend)) != L4D2ZombieClass_Smoker
		)
		{
			ClearMoveToPosition(iClient);
		}
		else if (fCurTime > g_fBot_NextMoveCommandTime[iClient])
		{
			L4D2_CommandABot(iClient, 0, BOT_CMD_MOVE, fMovePos);
			g_fBot_NextMoveCommandTime[iClient] = fCurTime + BOT_CMD_MOVE_INTERVAL;
		}
	}

	int iTankTarget = g_iBot_TankTarget[iClient];
	if (IsValidClient(iTankTarget))
	{
		int iVictim = g_iInfectedBot_CurrentVictim[iTankTarget];
		float fTankDist = GetClientDistance(iClient, iTankTarget, true);
		float fHeightDist = (fTankDist + (g_fClientAbsOrigin[iClient][2] - g_fClientAbsOrigin[iTankTarget][2]));
		
		// 1280 & 256
		if (fHeightDist <= 1638400.0 && (g_bCvar_AvoidTanksWithProp && g_iBot_TankProp[iClient] || g_bCvar_TakeCoverFromRocks && g_bInfectedBot_IsThrowing[iTankTarget]) && (iVictim == iClient || GetClientDistance(iClient, iVictim, true) <= 65536.0) && IsVisibleEntity(iClient, iTankTarget, MASK_SHOT_HULL))
		{
			if (strcmp(g_sBot_MovePos_Name[iClient], "TakeCover") != 0)
			{
				TakeCoverFromEntity(iClient, (g_iBot_TankProp[iClient] > 0 ? g_iBot_TankProp[iClient] : iTankTarget), 768.0);
			}
		}
		else
		{
			bool bTankVisible = IsVisibleEntity(iClient, iTankTarget, MASK_SHOT_HULL);
			if (!g_bInfectedBot_IsThrowing[iTankTarget] || !bTankVisible)
			{
				ClearMoveToPosition(iClient, "TakeCover");	
			}
			// 384 & 768
			if (fTankDist <= 147456.0 || fTankDist <= 589824.0 && bTankVisible)
			{
				L4D2_CommandABot(iClient, iTankTarget, BOT_CMD_RETREAT);
			}
		}

		int iRock = g_iBot_TankRock[iClient];
		if (g_bCvar_TankRock_ShootEnabled && g_iCvar_TankRockHealth > 0 && L4D_IsValidEnt(iRock))
		{
			static float fRockPos[3];
			GetEntityCenteroid(iRock, fRockPos);

			if (GetVectorDistance(g_fClientEyePos[iClient], fRockPos, true) <= g_fCvar_TankRock_ShootRange_Sqr && SurvivorBot_AbleToShootWeapon(iClient) && 
				(iCurWeapon != iWpnSlots[1] || !SurvivorHasMeleeWeapon(iClient)) && !IsSurvivorBusy(iClient) && HasVisualContactWithEntity(iClient, iRock, false, fRockPos))
			{
				SnapViewToPosition(iClient, fRockPos);
				PressAttackButton(iClient, iButtons); // shoot tank rock
			}
		}
	}
	else
	{
		iTankTarget = g_iBot_TankTarget[iClient] = 0;
	}

	int iWitchTarget = g_iBot_WitchTarget[iClient];
	if (L4D_IsValidEnt(iWitchTarget) && GetEntityHealth(iWitchTarget) > 0)
	{
		float fRage = (GetEntPropFloat(iWitchTarget, Prop_Send, "m_rage") + GetEntPropFloat(iWitchTarget, Prop_Send, "m_wanderrage"));
		float fWitchOrigin[3]; GetEntityAbsOrigin(iWitchTarget, fWitchOrigin);
		float fWitchDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fWitchOrigin, true);

		float fFirePos[3]; 
		GetTargetAimPart(iClient, iWitchTarget, fFirePos);

		int iHasShotgun = SurvivorHasShotgun(iClient);
		if (fRage >= 1.0)
		{
			if ((iCurWeapon == iWpnSlots[0] || iCurWeapon == iWpnSlots[1]) && SurvivorBot_AbleToShootWeapon(iClient))
			{
				float fShootRange = g_fCvar_TargetSelection_ShootRange;
				if (iCurWeapon == iWpnSlots[1])
				{
					if (!SurvivorHasMeleeWeapon(iClient))fShootRange = g_fCvar_TargetSelection_ShootRange4;
					else fShootRange = g_fCvar_ImprovedMelee_AttackRange;
				}
				else if (iHasShotgun)fShootRange = g_fCvar_TargetSelection_ShootRange2;
				else if (SurvivorHasSniperRifle(iClient))fShootRange = g_fCvar_TargetSelection_ShootRange3;

				bool bWitchVisible = HasVisualContactWithEntity(iClient, iWitchTarget, false);
				if (fWitchDist <= fShootRange && bWitchVisible)
				{
					SnapViewToPosition(iClient, fFirePos);				
					bool bFired = PressAttackButton(iClient, iButtons); // shoot witch
					if (iHasShotgun == 1 && bFired)g_bBot_ForceBash[iClient] = true;

					if (fShootRange != g_fCvar_TargetSelection_ShootRange2 && fShootRange != g_fCvar_ImprovedMelee_AttackRange)
					{
						ClearMoveToPosition(iClient, "GoToWitch");
					}
				}
				else if (iWitchHarasser != iClient && fWitchDist <= 4000000.0) // 2000
				{
					SetMoveToPosition(iClient, fWitchOrigin, 3, "GoToWitch", 0.0, (( bWitchVisible && (iWitchHarasser == -1 || !L4D_IsPlayerIncapacitated(iWitchHarasser)) ) ? (fShootRange > 36864.0 ? 36864.0 : fShootRange) : 0.0), true);
				}
			}

			if (iWitchHarasser == iClient && fWitchDist <= 1048576.0 && !L4D_IsPlayerIncapacitated(iClient)) // 1024
				L4D2_CommandABot(iClient, iWitchTarget, BOT_CMD_RETREAT);
		}
		else 
		{
			float fWalkDist = g_fCvar_WitchBehavior_WalkWhenNearby;
			if (fWalkDist != 0.0 && fWitchDist <= (fWalkDist * fWalkDist) && fRage <= 0.33 && !LBI_IsSurvivorInCombat(iClient))
				iButtons |= IN_SPEED;

			int iCrowning = g_iCvar_WitchBehavior_AllowCrowning;
			if ((iCrowning == 2 || iCrowning == 1 && !g_bTeamHasHumanPlayer) && iCurWeapon == iWpnSlots[0] && fWitchDist <= 1048576.0 && !L4D_IsPlayerOnThirdStrike(iClient) && (!IsValidClient(iTeamLeader) || fWitchDist <= 262144.0) && !IsWeaponReloading(iCurWeapon, false) && iHasShotgun && IsVisibleEntity(iClient, iWitchTarget)) // 1024 & 512
			{
				if (fWitchDist <= 16777216.0) // 4096
				{
					ClearMoveToPosition(iClient, "GoToWitch");
					SnapViewToPosition(iClient, fFirePos);

					bool bFired = PressAttackButton(iClient, iButtons); // shoot witch
					if (iHasShotgun == 1 && bFired)
						g_bBot_ForceBash[iClient] = true;
				}
				else if (LBI_IsSurvivorBotAvailable(iClient))
				{
					bool bApproachWitch = !bShouldUseFlow;
					if (!bApproachWitch)
					{
						Address pArea = L4D2Direct_GetTerrorNavArea(fWitchOrigin);
						bApproachWitch = (pArea != Address_Null && L4D2Direct_GetTerrorNavAreaFlow(pArea) >= L4D2Direct_GetFlowDistance(iClient));
					}
					if (bApproachWitch)
						SetMoveToPosition(iClient, fWitchOrigin, 2, "GoToWitch", 0.0, 0.0, true);

					if (fWitchDist <= 16384.0) // 128
						SnapViewToPosition(iClient, fFirePos);
				}
			}
		}
	}

	int iInfectedTarg = g_iBot_TargetInfected[iClient];
	bool bIsTargetPlayer = (1 <= iInfectedTarg <= MaxClients);
	if (L4D_IsValidEnt(iInfectedTarg) && (bIsTargetPlayer && IsClientInGame(iInfectedTarg) && IsPlayerAlive(iInfectedTarg) || !bIsTargetPlayer && IsCommonAlive(iInfectedTarg)))
	{
		static float fTargetPos[3]; GetEntityCenteroid(iInfectedTarg, fTargetPos);
		static float fInfectedOrigin[3]; GetEntityAbsOrigin(iInfectedTarg, fInfectedOrigin);
		fTargetDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fInfectedOrigin, true);

		L4D2ZombieClassType iInfectedClass;
		if (bIsTargetPlayer)
			iInfectedClass = L4D2_GetPlayerZombieClass(iInfectedTarg);

		int iMeleeType = SurvivorHasMeleeWeapon(iClient);
		if (g_bCvar_ImprovedMelee_Enabled && iMeleeType != 0 && iInfectedClass != L4D2ZombieClass_Boomer)
		{
			if (iCurWeapon == iWpnSlots[1])
			{
				static float fAimPosition[3];
				GetClosestPointToEntity(g_fClientEyePos[iClient], iInfectedTarg, fAimPosition);

				float fMeleeDistance = GetVectorDistance(g_fClientEyePos[iClient], fAimPosition, true);
				if (fMeleeDistance <= g_fCvar_ImprovedMelee_AimRange_Sqr)
				{
					g_fBot_BlockWeaponSwitchTime[iClient] = (fCurTime + (iMeleeType == 2 ? 8.0 : 5.0));
					SnapViewToPosition(iClient, fAimPosition);
				}

				if (fCurTime > g_fBot_PreventFireTime[iClient] && fMeleeDistance <= g_fCvar_ImprovedMelee_AttackRange && (iGameDifficulty == 4 || (!IsSurvivorBusy(iClient)
					|| g_iBot_ThreatInfectedCount[iClient] >= GetCommonHitsUntilDown(iClient, (float(g_iBot_NearbyFriends[iClient]) / (iTeamCount - 1))))))
				{
					float fAttackTime = (iMeleeType == 2 ? GetRandomFloat(0.33, 0.8) : GetRandomFloat(0.1, 0.33));
					g_fBot_MeleeAttackTime[iClient] = (fCurTime + fAttackTime);
				}

				if (fCurTime < g_fBot_MeleeAttackTime[iClient])
				{
					bool bShouldShove = (iInfectedClass != L4D2ZombieClass_Charger && GetRandomInt(1, (iMeleeType == 2 ? 200 : g_iCvar_ImprovedMelee_ShoveChance)) == 1
						&& (bIsTargetPlayer && !L4D_IsPlayerStaggering(iInfectedTarg) || !IsCommonStumbling(iInfectedTarg))
						&& L4D_GetPlayerShovePenalty(iClient) < (g_iCvar_ShovePenaltyMin - 1));
					iButtons |= (bShouldShove ? IN_ATTACK2 : IN_ATTACK);
				}

				bool bStopApproaching = true;
				if (g_fCvar_ImprovedMelee_ApproachRange > 0.0 && GetClientRealHealth(iClient) > 30 && fCurTime > g_fBot_MeleeApproachTime[iClient] && !IsSurvivorBotBlindedByVomit(iClient) && !iPinnedFriend
					&& (!iTankTarget || GetClientDistance(iClient, iTankTarget, true) > 1048576.0) && LBI_IsReachableEntity(iClient, iInfectedTarg) && !IsFinaleEscapeVehicleArrived()
					&& (!bIsTargetPlayer || (L4D_IsPlayerStaggering(iInfectedTarg) || L4D_GetPinnedSurvivor(iInfectedTarg) != 0) && !L4D_IsAnySurvivorInCheckpoint()))
				{
					static float fMovePos[3];
					GetEntityAbsOrigin(iInfectedTarg, fMovePos);

					Address pArea;
					if (!bShouldUseFlow || (pArea = L4D2Direct_GetTerrorNavArea(fMovePos)) == Address_Null || L4D2Direct_GetTerrorNavAreaFlow(pArea) >= (L4D2Direct_GetFlowDistance(iClient) - SquareRoot(g_fCvar_ImprovedMelee_ApproachRange*0.5)))
					{
						fLeaderDist = ((!bIsTargetPlayer && iTeamLeader != iClient && IsValidClient(iTeamLeader)) ? GetClientTravelDistance(iTeamLeader, fMovePos, true) : -2.0);
						if (fLeaderDist == -2.0 || fLeaderDist != -1.0 && fLeaderDist <= (g_fCvar_ImprovedMelee_ApproachRange * 0.75))
						{
							float fTravelDist = GetNavDistance(fMovePos, g_fClientAbsOrigin[iClient], iInfectedTarg);
							if (fTravelDist != -1.0 && fTravelDist <= g_fCvar_ImprovedMelee_ApproachRange)
							{
								SetMoveToPosition(iClient, fMovePos, 2, "ApproachMelee");
								bStopApproaching = false;
							}
						}
					}
				}
				if (bStopApproaching)ClearMoveToPosition(iClient, "ApproachMelee");
			}
			else if (iCurWeapon == iWpnSlots[0])
			{
				int iMeleeSwitchCount = ((iMeleeType != 2) ? g_iCvar_ImprovedMelee_SwitchCount : g_iCvar_ImprovedMelee_SwitchCount2);
				float fMeleeSwitchRange = (g_fCvar_ImprovedMelee_SwitchRange * ((iMeleeType == 2) ? 1.5 : (iInvFlags & FLAG_SHOTGUN ? 0.66 : 1.0)));

				if (fTargetDist <= fMeleeSwitchRange && !iTankTarget && (!(iInvFlags & FLAG_SHOTGUN) || GetWeaponClip1(iCurWeapon) <= 0 ))
				{ 
					if (bIsTargetPlayer)
					{
						if (g_iInfectedBot_CurrentVictim[iInfectedTarg] != iClient || L4D_GetPinnedSurvivor(iInfectedTarg) != 0 || L4D_IsPlayerStaggering(iInfectedTarg))
						{
							SwitchWeaponSlot(iClient, 1);
							g_fBot_MeleeApproachTime[iClient] = fCurTime + ((iMeleeType == 2) ? 2.0 : 0.1);
						}
					}
					else if (g_iBot_NearbyInfectedCount[iClient] >= iMeleeSwitchCount && !iPinnedFriend &&
						(GetCurrentGameDifficulty() == 4 || !IsSurvivorBusy(iClient) || g_iBot_ThreatInfectedCount[iClient] >= GetCommonHitsUntilDown(iClient, 0.75)))
					{
						SwitchWeaponSlot(iClient, 1);
						g_fBot_MeleeApproachTime[iClient] = fCurTime + ((iMeleeType == 2) ? 2.0 : 0.66);
					}
				}
			}
		}

		if ((g_iCvar_AutoShove_Enabled == 1 || g_iCvar_AutoShove_Enabled == 2 && !FVectorInViewAngle(iClient, fTargetPos)) && fTargetDist <= 6400.0 && !L4D_IsPlayerIncapacitated(iClient)
			&& (!IsSurvivorBusy(iClient) || g_iBot_ThreatInfectedCount[iClient] >= GetCommonHitsUntilDown(iClient, (float(g_iBot_NearbyFriends[iClient]) / (iTeamCount - 1))))
			&& (!(iInvFlags & FLAG_MELEE) || iCurWeapon != iWpnSlots[1]))
		{
			if (IsSurvivorCarryingProp(iClient) || (!bIsTargetPlayer || 
				iInfectedClass != L4D2ZombieClass_Charger && iInfectedClass != L4D2ZombieClass_Tank && 
				!L4D_IsPlayerStaggering(iInfectedTarg) && !IsUsingSpecialAbility(iInfectedTarg)) && 
				GetRandomInt(1, 4) == 1 && L4D_GetPlayerShovePenalty(iClient) < (g_iCvar_ShovePenaltyMin - 1)
			)
			{
				SnapViewToPosition(iClient, fTargetPos);
				iButtons |= IN_ATTACK2;
			}
			else if (SurvivorBot_AbleToShootWeapon(iClient))
			{
				SnapViewToPosition(iClient, fTargetPos);
				PressAttackButton(iClient, iButtons);
			}
		}
	}
	else
	{
		iInfectedTarg = g_iBot_TargetInfected[iClient] = 0;
		bIsTargetPlayer = false;
	}

	if (iInfectedTarg || iTankTarget)
	{
		if (g_bCvar_TargetSelection_Enabled && !iPinnedFriend && !IsSurvivorBusy(iClient))
		{
			int iFireTarget = iInfectedTarg;
			if (iFireTarget)
			{
				if (g_bCvar_TargetSelection_IgnoreDociles && !bIsTargetPlayer && !IsCommonAttacking(iFireTarget))
					iFireTarget = 0;

				if (iTankTarget && (fTargetDist > 1048576.0 || GetClientDistance(iClient, iTankTarget, true) < fTargetDist)) // 1024
					iFireTarget = iTankTarget;
			}
			else if (iTankTarget)
			{
				iFireTarget = iTankTarget;
			}

			if (iFireTarget && HasVisualContactWithEntity(iClient, iFireTarget, (iFireTarget != iTankTarget)))
			{
				L4D2ZombieClassType iInfectedClass;
				if (iFireTarget == iInfectedTarg && bIsTargetPlayer)
					iInfectedClass = L4D2_GetPlayerZombieClass(iFireTarget);

				float fFirePos[3]; GetTargetAimPart(iClient, iFireTarget, fFirePos);
				fTargetDist = GetVectorDistance(g_fClientEyePos[iClient], fFirePos, true);

				if (iInfectedClass != L4D2ZombieClass_Boomer || !g_bBot_IsFriendNearBoomer[iClient] && fTargetDist > BOT_BOOMER_AVOID_RADIUS_SQR)
				{
					if (iCurWeapon == iWpnSlots[0])
					{
						float fShootRange = g_fCvar_TargetSelection_ShootRange;

						if (iInvFlags & FLAG_SHOTGUN)
						{
							fShootRange = g_fCvar_TargetSelection_ShootRange2;
							if (!(iInvFlags & FLAG_MELEE) && fTargetDist <= g_fCvar_TargetSelection_ShootRange4 && fTargetDist > fShootRange
								&& fCurTime > g_fBot_NextWeaponRangeSwitchTime[iClient] && !IsWeaponReloading(iCurWeapon, false))
							{
								g_fBot_NextWeaponRangeSwitchTime[iClient] = fCurTime + GetRandomFloat(1.0, 3.0);
								g_bBot_ForceSwitchWeapon[iClient] = true;
								SwitchWeaponSlot(iClient, 1);
							}
						}
						else if (iInvFlags & FLAG_SNIPER)
						{
							fShootRange = g_fCvar_TargetSelection_ShootRange3;
						}

						if (fTargetDist <= fShootRange && SurvivorBot_AbleToShootWeapon(iClient))
						{
							SnapViewToPosition(iClient, fFirePos);
							PressAttackButton(iClient, iButtons);
						}
					}
					else if (iCurWeapon == iWpnSlots[1] && !(iInvFlags & FLAG_MELEE))
					{
						if (iInvFlags & FLAG_SHOTGUN && fTargetDist <= (g_fCvar_TargetSelection_ShootRange2 * 0.75) && fCurTime > g_fBot_NextWeaponRangeSwitchTime[iClient]
							&& GetClientPrimaryAmmo(iClient) > 0 && !IsWeaponReloading(iCurWeapon))
						{
							g_fBot_NextWeaponRangeSwitchTime[iClient] = fCurTime + GetRandomFloat(1.0, 3.0);
							g_bBot_ForceSwitchWeapon[iClient] = true;
							SwitchWeaponSlot(iClient, 0);
						}

						if (L4D_IsPlayerIncapacitated(iClient) || fTargetDist <= (g_fCvar_TargetSelection_ShootRange4*g_fCvar_TargetSelection_ShootRange4) && SurvivorBot_AbleToShootWeapon(iClient))
						{
							SnapViewToPosition(iClient, fFirePos);
							PressAttackButton(iClient, iButtons);
						}
					}
				}
			}
		}

		if (g_bCvar_GrenadeThrow_Enabled && iWpnSlots[2])
		{
			float fThrowPosition[3];
			int iThrowTarget, iGrenadeType = SurvivorHasGrenade(iClient);
			bool bTargetIsTank;

			if ((iGrenadeType != 1 || g_bCvar_Nightmare) && iTankTarget && !L4D_IsPlayerIncapacitated(iTankTarget) && 
				GetEntityHealth(iTankTarget) > RoundFloat(GetEntityMaxHealth(iTankTarget) * 0.33) && 
				GetClientDistance(iClient, iTankTarget, true) <= g_fCvar_GrenadeThrow_ThrowRange_Sqr)
			{
				iThrowTarget = iTankTarget;
				bTargetIsTank = true;
				GetEntityAbsOrigin(iTankTarget, fThrowPosition);
			}
			else
			{
				int iPossibleTarget = ((iGrenadeType != 2 && !g_bCvar_Nightmare) ? GetFarthestInfected(iClient, g_fCvar_GrenadeThrow_ThrowRange) : iInfectedTarg);
				if (!iPossibleTarget && g_bCvar_Nightmare)
				{
					iPossibleTarget = iWitchTarget;
					bTargetIsTank = true;
				}

				if (iPossibleTarget)
				{
					iThrowTarget = iPossibleTarget;
					GetEntityAbsOrigin(iPossibleTarget, fThrowPosition);
				}
			}
			g_iBot_Grenade_ThrowTarget[iClient] = iThrowTarget;

			if (iThrowTarget)
			{
				if (g_bCvar_Nightmare || iGrenadeType == 2)
				{
					float fThrowAngles[3], fTargetForward[3], fMidPos[3];
					MakeVectorFromPoints(fThrowPosition, g_fClientEyePos[iClient], fThrowAngles);
					NormalizeVector(fThrowAngles, fThrowAngles);
					GetVectorAngles(fThrowAngles, fThrowAngles);

					GetAngleVectors(fThrowAngles, fTargetForward, NULL_VECTOR, NULL_VECTOR);
					NormalizeVector(fTargetForward, fTargetForward);

					float fThrowDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fThrowPosition);
					fMidPos[0] = fThrowPosition[0] + (fTargetForward[0] * (fThrowDist * 0.5));
					fMidPos[1] = fThrowPosition[1] + (fTargetForward[1] * (fThrowDist * 0.5));
					fMidPos[2] = fThrowPosition[2] + (fTargetForward[2] * (fThrowDist * 0.5));

					if (GetVectorDistance(g_fClientAbsOrigin[iClient], fMidPos, true) > BOT_GRENADE_CHECK_RADIUS_SQR)
					{
						float fTraceStart[3]; fTraceStart = fMidPos; fTraceStart[2] + HUMAN_HALF_HEIGHT;
						float fAngleDown[3] = {90.0, 0.0, 0.0};
						Handle hGroundCheck = TR_TraceRayFilterEx(fTraceStart, fAngleDown, MASK_PLAYERSOLID, RayType_Infinite, Base_TraceFilter);
						float fTrHitPos[3]; TR_GetEndPosition(fTrHitPos, hGroundCheck); delete hGroundCheck;

						float fHeightDiff = FloatAbs(fTrHitPos[2] - fThrowPosition[2]);
						if (fHeightDiff < 96.0)
						{
							fThrowPosition = fMidPos;
						}
					}
				}

				if (iCurWeapon == iWpnSlots[2])
				{
					int iThrowArea = L4D_GetNearestNavArea(fThrowPosition, _, true, false, _, 3);
					if (iThrowArea)
						LBI_GetClosestPointOnNavArea(iThrowArea, fThrowPosition, fThrowPosition);

					static float fThrowVel[3];
					CalculateTrajectory(g_fClientEyePos[iClient], fThrowPosition, 700.0, 0.4, fThrowVel);
					AddVectors(g_fClientEyePos[iClient], fThrowVel, g_fBot_Grenade_AimPos[iClient]);

					static float fThrowTrajectory[3];
					fThrowTrajectory = fThrowPosition;
					fThrowTrajectory[2] += (g_fBot_Grenade_AimPos[iClient][2] - g_fClientEyePos[iClient][2]);

					Handle hCeilingCheck = TR_TraceRayFilterEx(g_fClientEyePos[iClient], fThrowTrajectory, MASK_SOLID, RayType_EndPoint, Base_TraceFilter);
					g_fBot_Grenade_AimPos[iClient][2] *= TR_GetFraction(hCeilingCheck);
					delete hCeilingCheck;

					g_fBot_Grenade_ThrowPos[iClient] = fThrowPosition;
					if (g_iBot_ThreatInfectedCount[iClient] < GetCommonHitsUntilDown(iClient, 0.33) && 
						CheckIsUnableToThrowGrenade(
							iClient, 
							iThrowTarget, 
							iGrenadeType, 
							fThrowPosition, 
							bTargetIsTank)
						)
					{
						g_bBot_ForceSwitchWeapon[iClient] = true;
						SwitchWeaponSlot(iClient, ((GetClientPrimaryAmmo(iClient) > 0) ? 0 : 1));

						if (iGrenadeType == 2)
						{
							if (fCurTime > g_fBot_Grenade_NextThrowTime_Molotov)
							{
								g_fBot_Grenade_NextThrowTime_Molotov = (fCurTime + GetRandomFloat(4.0, 7.5));
							}
						}
						else if (fCurTime > g_fBot_Grenade_NextThrowTime)
						{
							g_fBot_Grenade_NextThrowTime = (fCurTime + GetRandomFloat(2.5, 4.0));
						}
					}
					else
					{
						SnapViewToPosition(iClient, g_fBot_Grenade_AimPos[iClient]);
						PressAttackButton(iClient, iButtons); //throw grenade
					}
				}
				else if (CheckCanThrowGrenade(
					iClient, 
					iThrowTarget, 
					iGrenadeType, 
					fThrowPosition, 
					bTargetIsTank)
				)
				{
					SwitchWeaponSlot(iClient, 2);
				}
			}
		}
	}

	if (L4D_IsPlayerIncapacitated(iClient) || L4D_GetPinnedInfected(iClient))
		return iAliveBots;

	if (g_bCvar_DeployUpgradePacks && iWpnSlots[0] && iInvFlags & FLAG_UPGRADE && !LBI_IsSurvivorInCombat(iClient))
	{
		bool bHasDeployedPackNearby = false;
		int iActiveDeployers = (GetTeamActiveItemCount(L4D2WeaponId_IncendiaryAmmo) + GetTeamActiveItemCount(L4D2WeaponId_FragAmmo));
		if (g_hDeployedAmmoPacks)
		{
			for (int i = 0; i < g_hDeployedAmmoPacks.Length; i++)
			{
				if (GetEntityDistance(iClient, g_hDeployedAmmoPacks.Get(i), true) > 589824.0) // 768
					continue;
				
				bHasDeployedPackNearby = true;
				break;
			}
		}
		
		if (!bHasDeployedPackNearby)
		{
			int iPrimSlot, iPrimaryCount, iUpgradedCount;
			for (int i = 1; i <= MaxClients; i++)
			{
				if (!IsClientSurvivor(i))
					continue;

				iPrimSlot = GetWeaponInInventory(i, 0);
				if (!iPrimSlot)
					continue;

				iPrimaryCount++;
				if (GetEntProp(iPrimSlot, Prop_Send, "m_nUpgradedPrimaryAmmoLoaded", 1) > 0)
					iUpgradedCount++;
			}

			bool bCanSwitch = (iUpgradedCount < RoundFloat(iAlivePlayers * 0.25) && iPrimaryCount >= RoundFloat(iTeamCount * 0.25) && !iTankTarget);
			if (iCurWeapon == iWpnSlots[3])
			{
				if (iActiveDeployers > 1 || IsValidClient(iTeamLeader) && GetClientDistance(iClient, iTeamLeader, true) >= 65536.0 || !bCanSwitch) // 256
					SwitchWeaponSlot(iClient, (GetClientPrimaryAmmo(iClient) > 0 ? 0 : 1));
				else
					PressAttackButton(iClient, iButtons); // deploy ammo pack
			}
			else if (bCanSwitch && iActiveDeployers == 0 && LBI_IsSurvivorBotAvailable(iClient) && (!IsValidClient(iTeamLeader) || GetClientDistance(iClient, iTeamLeader, true) <= 36864.0)) // 192
			{
				SwitchWeaponSlot(iClient, 3);
			}
		}
	}

	int iDefibTarget = g_iBot_DefibTarget[iClient];
	if (!L4D_IsValidEnt(iDefibTarget))
	{
		g_iBot_DefibTarget[iClient] = 0;
	}
	else if (g_bCvar_DefibRevive_Enabled && !iTankTarget && !iPinnedFriend && iInvFlags & FLAG_DEFIB && !LBI_IsSurvivorInCombat(iClient))
	{
		float fDefibPos[3]; GetEntityAbsOrigin(iDefibTarget, fDefibPos);
		float fDefibDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fDefibPos, true);

		if (fDefibDist <= g_fCvar_DefibRevive_ScanDist_Sqr && !LBI_IsDamagingPosition(fDefibPos))
		{
			if (L4D2_GetPlayerUseActionTarget(iClient) == iDefibTarget || fDefibDist <= 9216.0) // 96
			{
				if (iCurWeapon != iWpnSlots[3])
				{
					SwitchWeaponSlot(iClient, 3);
				}
				else
				{
					SnapViewToPosition(iClient, fDefibPos);
					PressAttackButton(iClient, iButtons); // use defib
				}
			}
			else if (!g_iBot_IncapacitatedFriend[iClient] && !IsSurvivorBotBlindedByVomit(iClient) && !IsFinaleEscapeVehicleArrived() && 
				LBI_IsSurvivorBotAvailable(iClient) && LBI_IsReachablePosition(iClient, fDefibPos) && 
				g_iBot_NearbyInfectedCount[iClient] < GetCommonHitsUntilDown(iClient, 0.66)
			)
			{
				SetMoveToPosition(iClient, fDefibPos, 2, "DefibPlayer");
			}
		}
	}

	int iScavengeItem = g_iBot_ScavengeItem[iClient];
	if (iScavengeItem)
	{
		if (!L4D_IsValidEnt(iScavengeItem) || 1 <= GetEntityOwner(iScavengeItem) <= MaxClients)
		{
			ClearMoveToPosition(iClient, "ScavengeItem");
			g_iBot_ScavengeItem[iClient] = 0;
			g_fBot_ScavengeItemDist[iClient] = -1.0;
		}
		else if (!IsSurvivorBotBlindedByVomit(iClient) && !IsSurvivorBusy(iClient, true, true, true))
		{
			static float fItemPos[3];
			GetEntityCenteroid(iScavengeItem, fItemPos);

			if (GetVectorDistance(g_fClientEyePos[iClient], fItemPos, true) <= g_fCvar_ItemScavenge_PickupRange_Sqr && 
				IsVisibleEntity(iClient, iScavengeItem))
			{
				BotLookAtPosition(iClient, fItemPos, 0.33);

				if (IntervalHasPassed(0.2) && LBI_FindUseEntity(iClient, g_fCvar_ItemScavenge_PickupRange) == iScavengeItem)
					iButtons |= IN_USE;
			}
			else
			{
				static bool bAllowScavenge, bCanRegroup;
				static int iScavengeArea, iLeaderArea, iHits;
				static float fMaxDist, fDist, fRangeMult, fDistanceToRegroup, fScavengePos[3];
				
				bAllowScavenge = true;
				fScavengePos = fItemPos;
				fDistanceToRegroup = -1.0;
				fMaxDist = g_fCvar_ItemScavenge_ApproachVisibleRange;

				fRangeMult = 1.0;
				if (!g_bTeamHasHumanPlayer)
					fRangeMult *= g_fCvar_ItemScavenge_NoHumansRangeMultiplier;

				iScavengeArea = L4D_GetNearestNavArea(fItemPos, 140.0, true, true, false);
				iLeaderArea = g_iClientNavArea[iTeamLeader];
				if (iScavengeArea && iLeaderArea)
				{										
					LBI_GetClosestPointOnNavArea(iScavengeArea, fItemPos, fScavengePos);
					if (g_bTeamHasHumanPlayer && !LBI_IsNavAreaPartiallyVisible(iScavengeArea, g_fClientEyePos[iClient], iClient))
						fMaxDist = g_fCvar_ItemScavenge_ApproachRange;

					if (g_iBot_NearbyInfectedCount[iClient] > 0)
					{
						iHits = GetCommonHitsUntilDown(iClient, 0.66);
						fDist = (fMaxDist / ((g_iBot_NearbyInfectedCount[iClient] + 1) / iHits));
						if (fMaxDist > fDist)fMaxDist = fDist;
					}

					bCanRegroup = L4D2_NavAreaBuildPath(view_as<Address>(iScavengeArea), view_as<Address>(iLeaderArea), (fMaxDist * fRangeMult), 2, false);
					if (bCanRegroup)
						fDistanceToRegroup = GetNavDistance(fScavengePos, g_fClientAbsOrigin[iTeamLeader], iScavengeItem, false);				}
				else
					bAllowScavenge = false;

				if (bAllowScavenge && bCanRegroup && fDistanceToRegroup != -1.0 && fDistanceToRegroup < (fMaxDist * fRangeMult) && 
					!LBI_IsDamagingPosition(fScavengePos) && !IsFinaleEscapeVehicleArrived() &&
					(!iTankTarget || GetClientDistance(iClient, iTankTarget, true) > 262144.0 && GetVectorDistance(g_fClientAbsOrigin[iTankTarget], fScavengePos, true) > 147456.0) // 512 & 384
				)
				{
					SetMoveToPosition(iClient, fScavengePos, 1, "ScavengeItem");
				}
				else
				{
					ClearMoveToPosition(iClient, "ScavengeItem");
					g_iBot_ScavengeItem[iClient] = 0;
					g_fBot_ScavengeItemDist[iClient] = -1.0;
				}
			}
		}
	}

	return iAliveBots;
}

Action OnSurvivorSwitchWeapon(int iClient, int iWeapon) 
{
	if (!IsClientSurvivor(iClient) || !IsFakeClient(iClient) || g_bBot_ForceSwitchWeapon[iClient] || !L4D_IsValidEnt(iWeapon) || L4D_IsPlayerIncapacitated(iClient))
	{
		g_bBot_ForceSwitchWeapon[iClient] = false;
		return Plugin_Continue;
	}

	int iCurWeapon = L4D_GetPlayerCurrentWeapon(iClient);
	if (iCurWeapon == -1 || iWeapon == iCurWeapon || GetWeaponClip1(iCurWeapon) < 0)
	{
		g_bBot_ForceSwitchWeapon[iClient] = false;
		return Plugin_Continue;
	}

	if (iWeapon == GetWeaponInInventory(iClient, 0))
	{
		if (GetBotWeaponPreference(iClient) == L4D_WEAPON_PREFERENCE_SECONDARY)
		{
			SwitchWeaponSlot(iClient, 1);
			return Plugin_Handled;
		}

		if (iCurWeapon == GetWeaponInInventory(iClient, 1))
		{
			if (g_iClientInvFlags[iClient] & FLAG_MELEE)
			{
				if (GetGameTime() <= g_fBot_BlockWeaponSwitchTime[iClient])
				{
					return Plugin_Handled;
				}
			}
			else if (IsWeaponReloading(iCurWeapon) || GetWeaponClip1(iCurWeapon) != GetWeaponClipSize(iCurWeapon) && !LBI_IsSurvivorInCombat(iClient))
			{
				g_bBot_ForceWeaponReload[iClient] = true;
				return Plugin_Handled;
			}
		}
	}
	else if (iWeapon == GetWeaponInInventory(iClient, 1)) 
	{
		if (iCurWeapon == GetWeaponInInventory(iClient, 0) && GetClientPrimaryAmmo(iClient) > 0)
		{
			if (g_iBot_NearbyInfectedCount[iClient] < g_iCvar_ImprovedMelee_SwitchCount2 && g_iClientInvFlags[iClient] & FLAG_CHAINSAW)
				return Plugin_Handled;

			if (GetBotWeaponPreference(iClient) != L4D_WEAPON_PREFERENCE_SECONDARY && (!(g_iClientInvFlags[iClient] & FLAG_MELEE)
				|| g_iBot_NearbyInfectedCount[iClient] < g_iCvar_ImprovedMelee_SwitchCount)
				&& ((g_bCvar_DontSwitchToPistol && g_iClientInvFlags[iClient] & FLAG_SNIPER) || g_iClientInvFlags[iClient] & FLAG_SHOTGUN))
			{
				return Plugin_Handled;
			}
		}
	}

	if (g_iBot_DefibTarget[iClient] && iCurWeapon == GetWeaponInInventory(iClient, 3) && g_iClientInvFlags[iClient] & FLAG_DEFIB)
	{
		return Plugin_Handled;
	}

	g_bBot_ForceSwitchWeapon[iClient] = false;
	return Plugin_Continue;
}

Action OnSurvivorTakeDamage(int iClient, int &iAttacker, int &iInflictor, float &fDamage, int &iDamageType) 
{
	if (GetClientTeam(iClient) != 2 || !IsPlayerAlive(iClient))
		return Plugin_Continue;

	if (IsWitch(iAttacker))
	{
		int iWitchRef;
		for (int i = 0; i < g_hWitchList.Length; i++)
		{
			iWitchRef = EntRefToEntIndex(g_hWitchList.Get(i));
			if (iWitchRef == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iWitchRef))
			{
				g_hWitchList.Erase(i);
				continue;
			}
			if (iWitchRef == iAttacker)
			{
				g_hWitchList.Set(i, GetClientUserId(iClient), 1);
				break;
			}
		}
	}

	if (!IsFakeClient(iClient))
		return Plugin_Continue;

	if (IsSurvivorBotBlindedByVomit(iClient) && (IsValidClient(iAttacker) && GetClientTeam(iAttacker) == 3 || IsCommonInfected(iAttacker)))
	{
		float fLookPos[3]; GetEntityCenteroid(iAttacker, fLookPos);
		BotLookAtPosition(iClient, fLookPos, 1.0);
		g_bBot_ForceBash[iClient] = true;
	}

	if (g_bCvar_NoFallDmgOnLadderFail && iDamageType & DMG_FALL && g_bBot_WasUsingLadder[iClient])
	{
		fDamage = 0.0;
		return Plugin_Changed; 
	}

	if (!g_bCvar_AcidEvasion || !L4D_IsValidEnt(iInflictor) || strcmp(g_sBot_MovePos_Name[iClient], "EscapeInferno") == 0)
		return Plugin_Continue; 

	static char sInfClass[16]; GetEntityClassname(iInflictor, sInfClass, sizeof(sInfClass));
	if (strcmp(sInfClass, "insect_swarm") != 0 && strcmp(sInfClass, "inferno") != 0)return Plugin_Continue; 

	int iNavArea;
	float fCurDist, fLastDist = -1.0;
	float fEscapePos[3], fPathPos[3];
	for (int i = 0; i < 12; i++)
	{
		LBI_TryGetPathableLocationWithin(iClient, 250.0 + (50.0 * i), fPathPos);
		
		if (!IsValidVector(fPathPos))continue;

		fCurDist = GetClientTravelDistance(iClient, fPathPos, true);
		if (fLastDist != -1.0 && fCurDist >= fLastDist)
			continue;

		iNavArea = L4D_GetNearestNavArea(fPathPos);
		if (!iNavArea || LBI_IsDamagingNavArea(iNavArea, true) || !LBI_IsReachableNavArea(iClient, iNavArea))
			continue;

		fLastDist = fCurDist;
		fEscapePos = fPathPos;
	}

	if (IsValidVector(fEscapePos))
		SetMoveToPosition(iClient, fEscapePos, 4, "EscapeInferno", 0.0, 5.0, true, true);

	return Plugin_Continue; 
}

Action OnWitchTakeDamage(int iWitch, int &iAttacker, int &iInflictor, float &fDamage, int &iDamageType) 
{
	if (iDamageType & (DMG_BULLET | DMG_BLAST | DMG_BLAST_SURFACE))
		CreateTimer(0.1, CheckWitchStumble, iWitch);

	return Plugin_Continue; 
}

public Action CheckWitchStumble(Handle timer, int iWitch)
{
	if (L4D_IsValidEnt(iWitch))
	{
		static int iWitchRef;
		static float fWitchRage;
		
		for (int i = 0; i < g_hWitchList.Length; i++)
		{
			iWitchRef = EntRefToEntIndex(g_hWitchList.Get(i));
			if (iWitchRef == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iWitchRef))
			{
				g_hWitchList.Erase(i);
				continue;
			}

			if (iWitchRef == iWitch)
			{
				fWitchRage = GetEntPropFloat(iWitch, Prop_Send, "m_rage");
				if (g_hWitchList.Get(i, 1) == 0 && fWitchRage > 0.99)
				{
					g_hWitchList.Set(i, -1, 1);
					SDKUnhook(iWitch, SDKHook_OnTakeDamage, OnWitchTakeDamage);
					break;
				}
			}
		}
	}
	return Plugin_Handled;
}

bool TakeCoverFromPosition(int iClient, float fPosition[3], float fSearchDist = 384.0)
{
	float fSelfToPos[3]; MakeVectorFromPoints(g_fClientEyePos[iClient], fPosition, fSelfToPos);
	NormalizeVector(fSelfToPos, fSelfToPos);

	float fDot, fPathPos[3], fPathOffset[3], fSelfToMovePos[3];
	for (int i = 0; i < 10; i++)
	{
		LBI_TryGetPathableLocationWithin(iClient, fSearchDist, fPathPos);
		if (!IsValidVector(fPathPos) || LBI_IsDamagingPosition(fPathPos))continue;

		fPathOffset = fPathPos; fPathOffset[2] + HUMAN_HALF_HEIGHT;

		MakeVectorFromPoints(g_fClientEyePos[iClient], fPathPos, fSelfToMovePos);
		NormalizeVector(fSelfToMovePos, fSelfToMovePos);

		fDot = GetVectorDotProduct(fSelfToPos, fSelfToMovePos);
		if (fDot > 0.2 || GetVectorVisible(fPosition, fPathOffset))continue;

		SetMoveToPosition(iClient, fPathPos, 3, "TakeCover");
		return true;
	}

	return false;
}

bool TakeCoverFromEntity(int iClient, int iEntity, float fSearchDist = 384.0)
{
	float fEntityPos[3];
	if (IsValidClient(iEntity))GetClientEyePosition(iEntity, fEntityPos);
	else GetEntityCenteroid(iEntity, fEntityPos);
	return (TakeCoverFromPosition(iClient, fEntityPos, fSearchDist));
}

// ----------------------------------------------------------------------------------------------------

// AI Move types from the lowest to the highest priority one
// HighPriority is used in spit/charger dodging and other high priority MOVEs
// enum AI_MOVE_TYPE {
// 	None = 0,
// 	Order = 1,
// 	Pickup = 2,
// 	Defib = 3,
// 	Door = 4, // This is used for saferoom doors only (other doors are handled as orders and so with Order priority)
// 	HighPriority = 5
// }

bool IsBusyDoingL4BStuff(int iClient)
{
	if (g_iHasLeft4Bots != 1)
		return false;

	char sInitCode[CODE_BUFFER_LENGTH];
	FormatEx(sInitCode, CODE_BUFFER_LENGTH, "local ply = GetPlayerFromUserID(%i);\
		ply.ValidateScriptScope();\
		local scope = ply.GetScriptScope();", 
		GetClientUserId(iClient)
	);

	// -------------------------

	char sOutputBuffer[CODE_BUFFER_LENGTH];
	char sCodeBuffer[CODE_BUFFER_LENGTH];

	// Move Type
	FormatEx(sCodeBuffer, CODE_BUFFER_LENGTH, "%s\
		<RETURN>scope.MoveType</RETURN>", 
	sInitCode);

	L4D2_GetVScriptOutput(sCodeBuffer, sOutputBuffer, CODE_BUFFER_LENGTH);
	int iMoveType = StringToInt(sOutputBuffer);

	// Current Order
	FormatEx(sCodeBuffer, CODE_BUFFER_LENGTH, "%s\
		local order = scope.CurrentOrder;\
		if (order && order.OrderType)\
			<RETURN>order.OrderType</RETURN>",
	sInitCode);

	char sOrderType[32];
	L4D2_GetVScriptOutput(sCodeBuffer, sOrderType, CODE_BUFFER_LENGTH);

	// PrintToServer("%N's L4B2 Stuff: MoveType = %i	OrderType = %s", iClient, 
	// 	iMoveType,
	// 	sOrderType
	// );

	return (iMoveType > 0 && iMoveType != 2 && (iMoveType != 1 || strcmp(sOrderType, "lead") != 0) && 
		strcmp(sOrderType, "witch") != 0 && strcmp(sOrderType, "destroy") != 0
	);
}

// ----------------------------------------------------------------------------------------------------

void ClearMoveToPosition(int iClient, const char[] sCheckName = "")
{
	if (g_iCvar_Debug & DEBUG_MOVE && ( g_iCvar_DebugClient == 0 || iClient == g_iCvar_DebugClient ))
	{
		static int iAbort;
		static char szOutputBuffer[128], sClientName[128];
		
		iAbort = view_as<int>(sCheckName[0] != 0 && strcmp(g_sBot_MovePos_Name[iClient], sCheckName) != 0)
			| view_as<int>(!IsValidVector(g_fBot_MovePos_Position[iClient])) << 1
			| view_as<int>(LBI_IsDamagingNavArea(g_iClientNavArea[iClient])) << 2;
		
		szOutputBuffer[0] = EOS;
		sClientName[0] = EOS;
		GetClientName(iClient, sClientName, sizeof(sClientName));
		
		Format(szOutputBuffer, sizeof(szOutputBuffer), "ClearMoveToPosition %d %s type %s%s%s%s%s", iClient, sClientName,
			(sCheckName[0] != 0 ? sCheckName : "not specified"), (iAbort != 0 ? ", aborted " : ""),
			(iAbort & 1 ? "(different move cmd)" : ""),(iAbort & 2 ? "(invalid pos)" : ""),(iAbort & 4 ? "(damaging nav)" : ""));
		PrintToServer(szOutputBuffer);
		
		if (iAbort != 0)
			return;
	}
	else if ( sCheckName[0] != 0 && strcmp(g_sBot_MovePos_Name[iClient], sCheckName) != 0
			|| !IsValidVector(g_fBot_MovePos_Position[iClient])
			|| LBI_IsDamagingNavArea(g_iClientNavArea[iClient]) )
		return;

	g_iBot_MovePos_Priority[iClient] = -1;
	g_fBot_MovePos_Duration[iClient] = GetGameTime();
	g_fBot_MovePos_Tolerance[iClient] = -1.0;
	g_bBot_MovePos_IgnoreDamaging[iClient] = false;
	g_sBot_MovePos_Name[iClient][0] = 0;

	SetVectorToZero(g_fBot_MovePos_Position[iClient]);
	L4D2_CommandABot(iClient, 0, BOT_CMD_RESET);
}

void SetMoveToPosition(int iClient, float fMovePos[3], int iPriority, const char[] sName = "", float fAddDuration = 0.66, float fDistTolerance = -1.0, bool bIgnoreDamaging = false, bool bIgnoreCheckpoints = false)
{
	static int iAbort;
	static float fNavDist, fTravelDist, fMaxSpeed, fMoveTime;
	static char szOutputBuffer[320], sClientName[128];
	
	if (g_iCvar_Debug & DEBUG_MOVE && ( g_iCvar_DebugClient == 0 || iClient == g_iCvar_DebugClient ))
	{	
		iAbort = view_as<int>(iPriority < g_iBot_MovePos_Priority[iClient])
			| view_as<int>(IsValidVector(g_fBot_MovePos_Position[iClient])) << 1
			| view_as<int>(fDistTolerance >= 0.0 && GetVectorDistance(g_fClientAbsOrigin[iClient], fMovePos, true) <= (fDistTolerance*fDistTolerance)) << 2
			| view_as<int>(!bIgnoreDamaging && (LBI_IsDamagingNavArea(g_iClientNavArea[iClient]) || LBI_IsDamagingPosition(fMovePos))) << 3
			| view_as<int>(!bIgnoreCheckpoints && LBI_IsPositionInsideCheckpoint(g_fClientAbsOrigin[iClient]) && !LBI_IsPositionInsideCheckpoint(fMovePos)) << 4;
		
		szOutputBuffer[0] = EOS;
		sClientName[0] = EOS;
	}
	else if ( iPriority < g_iBot_MovePos_Priority[iClient] || IsValidVector(g_fBot_MovePos_Position[iClient])
			|| fDistTolerance >= 0.0 && GetVectorDistance(g_fClientAbsOrigin[iClient], fMovePos, true) <= (fDistTolerance*fDistTolerance)
			|| !bIgnoreDamaging && (LBI_IsDamagingNavArea(g_iClientNavArea[iClient]) || LBI_IsDamagingPosition(fMovePos))
			|| !bIgnoreCheckpoints && LBI_IsPositionInsideCheckpoint(g_fClientAbsOrigin[iClient]) && !LBI_IsPositionInsideCheckpoint(fMovePos)
			|| IsBusyDoingL4BStuff(iClient) && iPriority <= 2 )
		return;

	//float fTravelDist = GetClientTravelDistance(iClient, fMovePos, true);
	fNavDist = GetNavDistance(g_fClientAbsOrigin[iClient], fMovePos, _, false);
	fTravelDist = fNavDist <= 0.0 ? GetVectorDistance(g_fClientAbsOrigin[iClient], fMovePos) : fNavDist;
	fMaxSpeed = GetClientMaxSpeed(iClient);
	fMoveTime = fTravelDist / fMaxSpeed + fAddDuration;
	
	if (g_iCvar_Debug & DEBUG_MOVE && ( g_iCvar_DebugClient == 0 || iClient == g_iCvar_DebugClient ))
	{
		GetClientName(iClient, sClientName, sizeof(sClientName));
		
		Format(szOutputBuffer, sizeof(szOutputBuffer), "SetMoveToPosition %d %s %d %s fAddDur %.2f fDistTol %.2f%s%s\
			\nfNavDist %.2f fTravelDist %.2f fMaxSpeed %.2f fMoveTime %.2f iAbort %d\n%s%s%s%s%s",
			iClient, sClientName, iPriority, sName, fAddDuration, fDistTolerance,
			(bIgnoreDamaging ? "(ignore damaging)" : ""), (bIgnoreCheckpoints ? "(ignore checkpoint)" : ""),
			fNavDist, fTravelDist, fMaxSpeed, fMoveTime, iAbort,
			(iAbort & 1 ? "(lower priority)" : ""),(iAbort & 2 ? "(valid pos)" : ""),(iAbort & 4 ? "(close enough)" : ""),
			(iAbort & 8 ? "(damaging position)" : ""),(iAbort & 16 ? "(outside of checkpoint)" : ""));
		PrintToServer(szOutputBuffer);
		
		if (iAbort != 0)
			return;
	}

	strcopy(g_sBot_MovePos_Name[iClient], 64, sName);
	//g_fBot_MovePos_Duration[iClient] = GetGameTime() + (fTravelDist / (fMaxSpeed*fMaxSpeed)) + fAddDuration;
	g_fBot_MovePos_Duration[iClient] = GetGameTime() + fMoveTime;
	g_fBot_MovePos_Position[iClient] = fMovePos;
	g_iBot_MovePos_Priority[iClient] = iPriority;
	g_fBot_MovePos_Tolerance[iClient] = fDistTolerance;
	g_bBot_MovePos_IgnoreDamaging[iClient] = bIgnoreDamaging;
}

void InitPathWithin()
{
	/*
	::IBPathWithin <- function (iClient, fRadius)
	{
		local ply = GetPlayerFromUserID(iClient);
		return ply.TryGetPathableLocationWithin(fRadius)
	}
	*/
	
	HSCRIPT script = VScript_CompileScript("::IBPathWithin <- function (iClient, fRadius) { return GetPlayerFromUserID(iClient).TryGetPathableLocationWithin(fRadius) }");
	VScriptExecute execute = new VScriptExecute(script);
	ScriptStatus_t valid = execute.Execute();
	delete execute;
	script.ReleaseScript();
	PrintToServer("InitPathWithin: execute status %d, status_done: %b", valid, (valid == SCRIPT_DONE));
	
	g_vsPathWithin = new VScriptExecute(HSCRIPT_RootTable.GetValue("IBPathWithin"));
	
	g_bInitPathWithin = true;
}

void LBI_TryGetPathableLocationWithin(int iClient, float fRadius, float fBuffer[3])
{
	// this shit is ass, but in case if the vscript plugin isn't installed, run this:
	if (!g_bPluginVScript)
	{
		static char sCodeBuffer[512];
		FormatEx(sCodeBuffer, sizeof(sCodeBuffer), "local ply = GetPlayerFromUserID(%i);\
			local location = ply.TryGetPathableLocationWithin(%f);\
			local spaceChar = 32;\
			<RETURN>location.x.tostring() + spaceChar.tochar() + location.y.tostring() + spaceChar.tochar() + location.z.tostring()</RETURN>",
			GetClientUserId(iClient), fRadius
		);

		char szOutputBuffer[512];
		L4D2_GetVScriptOutput(sCodeBuffer, szOutputBuffer, sizeof(szOutputBuffer));

		char szOrigin[64];
		// X
		SplitString(szOutputBuffer, " ", szOrigin, sizeof(szOrigin));
		fBuffer[0] = StringToFloat(szOrigin);
		FormatEx(szOrigin, sizeof(szOrigin), "%s ", szOrigin);
		ReplaceString(szOutputBuffer, sizeof(szOutputBuffer), szOrigin, "");

		// Y
		SplitString(szOutputBuffer, " ", szOrigin, sizeof(szOrigin));
		fBuffer[1] = StringToFloat(szOrigin);
		FormatEx(szOrigin, sizeof(szOrigin), "%s ", szOrigin);
		ReplaceString(szOutputBuffer, sizeof(szOutputBuffer), szOrigin, "");

		// Z
		fBuffer[2] = StringToFloat(szOutputBuffer);
		return;
	}

	static int iUserID;

	if(!g_bInitPathWithin)
		InitPathWithin();
	
	iUserID = GetClientUserId(iClient);

	g_vsPathWithin.SetParam(1, FIELD_INTEGER, iUserID);
	g_vsPathWithin.SetParam(2, FIELD_FLOAT, fRadius);
	g_vsPathWithin.Execute();
	g_vsPathWithin.GetReturnVector(fBuffer);
}

bool SurvivorBot_IsTargetShootable(int iClient, int iTarget, int iCurWeapon)
{
	bool bInViewCone = (GetClientAimTarget(iClient, false) == iTarget);	
	if (!bInViewCone)
	{
		static float fEyeFwd[3];
		GetAngleVectors(g_fClientEyeAng[iClient], fEyeFwd, NULL_VECTOR, NULL_VECTOR);

		float fTargetDist = GetVectorDistance(g_fClientEyePos[iClient], g_fClientCenteroid[iTarget]);
		static float fAimPos[3];
		fAimPos[0] = (g_fClientEyePos[iClient][0] + fEyeFwd[0] * fTargetDist);
		fAimPos[1] = (g_fClientEyePos[iClient][1] + fEyeFwd[1] * fTargetDist);
		fAimPos[2] = (g_fClientEyePos[iClient][2] + fEyeFwd[2] * fTargetDist);

		static float fTargetPos[3];
		GetClosestPointToEntity(fAimPos, iTarget, fTargetPos);

		float fCone = 1.66;
		if (g_iClientInvFlags[iClient] & FLAG_SHOTGUN && iCurWeapon == GetWeaponInInventory(iClient, 0))
			fCone *= 2.0;

		fCone *= (1024.0 / GetVectorDistance(g_fClientEyePos[iClient], fTargetPos));
		bInViewCone = (FVectorInViewCone(iClient, fTargetPos, fCone) && IsVisibleEntity(iClient, iTarget));
	}
	if (!bInViewCone)
		return true;

	int iTeam = GetClientTeam(iTarget);
	if (iTeam == 2 && !L4D_IsPlayerPinned(iTarget))
	{
		if ((!g_bCvar_BotsShootThrough || g_bCvar_BotsFriendlyFire) && 
			(iCurWeapon == GetWeaponInInventory(iClient, 0) || iCurWeapon == GetWeaponInInventory(iClient, 1) && !(g_iClientInvFlags[iClient] & FLAG_MELEE))
		)
			return false;
	}
	else if (iTeam == 3 && !L4D_IsPlayerGhost(iTarget))
	{
		L4D2ZombieClassType iClass = L4D2_GetPlayerZombieClass(iTarget);
		if (iClass == L4D2ZombieClass_Boomer && GetClientDistance(iClient, iTarget, true) <= BOT_BOOMER_AVOID_RADIUS_SQR)
			return false;

		// if nightmare, don't bother shooting at howlers and wasting ammo
		if (g_bCvar_Nightmare && iClass == L4D2ZombieClass_Jockey)
			return false;
	}
	
	return true;
}

bool SurvivorBot_CanFreelyFireWeapon(int iClient)
{	
	int iCurWeapon = L4D_GetPlayerCurrentWeapon(iClient);
	if (iCurWeapon == GetWeaponInInventory(iClient, 0))
	{
		int iClip = GetWeaponClip1(iCurWeapon);
		float fAimPos[3]; GetClientAimPosition(iClient, fAimPos);
		
		if (g_bCvar_BotsFriendlyFire && iClip != 0 && g_iClientInvFlags[iClient] & FLAG_GL && 
			GetVectorDistance(fAimPos, g_fClientCenteroid[iClient], true) <= 90000.0 && // 300
			GetVectorVisible(fAimPos, g_fClientCenteroid[iClient]))
		{
			if (g_iBot_TargetInfected[iClient] && (!(g_iClientInvFlags[iClient] & FLAG_MELEE)
			|| GetVectorDistance(fAimPos, g_fClientCenteroid[iClient], true) <= g_fCvar_ImprovedMelee_SwitchRange))
				SwitchWeaponSlot(iClient, 1);

			return false;
		}

		if (iClip < 2 && IsWeaponReloading(iCurWeapon, false) && g_iClientInvFlags[iClient] & FLAG_SHOTGUN)
			return false;
	}

	for (int i = 1; i <= MaxClients; i++)
	{
		if (i == iClient || !IsClientInGame(i) || !IsPlayerAlive(i))
			continue;

		if (SurvivorBot_IsTargetShootable(iClient, i, iCurWeapon))
			continue;
		
		return false;
	}

	return true;
}

bool SurvivorBot_AbleToShootWeapon(int iClient)
{
	return (!IsWeaponReloading(L4D_GetPlayerCurrentWeapon(iClient)) && !L4D_IsPlayerStaggering(iClient));
}

void GetClosestPointToEntity(float fFromPos[3], int iTarget, float fAimPos[3])
{
	float fBoneDist, fBonePos[3], fAimPartPos[3], fLastDist = MAX_MAP_RANGE_SQR;
	if (SDKCall(g_hLookupBone, iTarget, "ValveBiped.Bip01_Pelvis") != -1)
	{
		for (int i = 0; i < sizeof(g_sBoneNames_Old); i++)
		{
			if (!LBI_GetBonePosition(iTarget, g_sBoneNames_Old[i], fBonePos))
				continue;

			fBoneDist = GetVectorDistance(fFromPos, fBonePos, true);
			if (fBoneDist >= fLastDist)continue;

			fLastDist = fBoneDist;
			fAimPartPos = fBonePos;
		}
	}
	else
	{
		for (int i = 0; i < sizeof(g_sBoneNames_New); i++)
		{
			if (!LBI_GetBonePosition(iTarget, g_sBoneNames_New[i], fBonePos))
				continue;

			fBoneDist = GetVectorDistance(fFromPos, fBonePos, true);
			if (fBoneDist >= fLastDist)continue;

			fLastDist = fBoneDist;
			fAimPartPos = fBonePos;
		}
	}
	fAimPos = fAimPartPos;
}

void GetTargetAimPart(int iClient, int iTarget, float fAimPos[3])
{
	if (IsWeaponSlotActive(iClient, 0) && g_iClientInvFlags[iClient] & FLAG_GL && (!IsValidClient(iTarget) || L4D2_GetPlayerZombieClass(iTarget) != L4D2ZombieClass_Jockey))
	{
		GetEntityAbsOrigin(iTarget, fAimPos);
		return;
	}

	static char sAimBone[64];
	float fDist = GetEntityDistance(iClient, iTarget, true);
	bool bIsUsingOldSkeleton = (SDKCall(g_hLookupBone, iTarget, "ValveBiped.Bip01_Pelvis") != -1);
	if (IsWitch(iTarget) && fDist <= 65536.0 && IsWeaponSlotActive(iClient, 0) && g_iClientInvFlags[iClient] & FLAG_SHOTGUN || (L4D_IsPlayerIncapacitated(iClient)
		&& fDist <= 147456.0 || L4D2_IsRealismMode() && fDist <= 262144.0) && (!IsValidClient(iTarget) || L4D2_GetPlayerZombieClass(iTarget) != L4D2ZombieClass_Tank))
	{
		sAimBone = (bIsUsingOldSkeleton ? "ValveBiped.Bip01_Head1" : "bip_head");
	}
	else
	{
		sAimBone = (bIsUsingOldSkeleton ? "ValveBiped.Bip01_Spine2" : "bip_spine_2");
	}

	float fAimPartPos[3]; 
	LBI_GetBonePosition(iTarget, sAimBone, fAimPartPos);

	if (!IsVisibleVector(iClient, fAimPartPos))
	{
		bool bVisibleOther = false;
		if (bIsUsingOldSkeleton)
		{
			for (int i = 0; i < sizeof(g_sBoneNames_Old); i++)
			{
				if (!LBI_GetBonePosition(iTarget, g_sBoneNames_Old[i], fAimPartPos))
				continue;

				if (IsVisibleVector(iClient, fAimPartPos))
				{
					bVisibleOther = true;
					break;
				}
			}
		}
		else
		{
			for (int i = 0; i < sizeof(g_sBoneNames_New); i++)
			{
				if (!LBI_GetBonePosition(iTarget, g_sBoneNames_New[i], fAimPartPos))
				continue;

				if (IsVisibleVector(iClient, fAimPartPos))
				{
					bVisibleOther = true;
					break;
				}
			}
		}
		if (!bVisibleOther)return;
	}

	fAimPos = fAimPartPos;
}

bool CheckIfCanRescueImmobilizedFriend(int iClient)
{
	if (IsSurvivorBotBlindedByVomit(iClient))
		return false;

	if (!L4D_IsPlayerIncapacitated(iClient)) 
	{		
		if (g_iBot_ThreatInfectedCount[iClient] >= GetCommonHitsUntilDown(iClient, 0.33))
			return false;

		if (IsSurvivorBusy(iClient))
			return false;
	}

	return true;
}

void BotLookAtPosition(int iClient, float fLookPos[3], float fLookDuration = 0.33)
{
	g_fBot_LookPosition[iClient] = fLookPos;
	g_fBot_LookPosition_Duration[iClient] = GetGameTime() + fLookDuration;
}

bool IsUsingSpecialAbility(int iClient)
{
	if (!IsSpecialInfected(iClient))
		return false;

	int iAbilityEntity = L4D_GetPlayerCustomAbility(iClient);
	if (iAbilityEntity == -1)return false;

	static char sProperty[16];
	switch(L4D2_GetPlayerZombieClass(iClient))
	{
		case L4D2ZombieClass_Boomer: 	sProperty = "m_isSpraying";
		case L4D2ZombieClass_Hunter: 	sProperty = "m_isLunging";
		case L4D2ZombieClass_Jockey: 	sProperty = "m_isLeaping";
		case L4D2ZombieClass_Charger: 	sProperty = "m_isCharging";
		case L4D2ZombieClass_Smoker: 	sProperty = "m_tongueState";
		default: 						return false;
	}

	if (!HasEntProp(iAbilityEntity, Prop_Send, sProperty))
		return false;

	return (GetEntProp(iAbilityEntity, Prop_Send, sProperty) > 0);
}

int CalculateGrenadeThrowInfectedCount()
{
	int iFreeSurvivors;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i) || L4D_IsPlayerBoomerBiled(i) || L4D_IsPlayerIncapacitated(i) || L4D_IsPlayerPinned(i) || GetClientRealHealth(i) <= RoundFloat(g_iCvar_SurvivorLimpHealth * 0.8))
			continue;

		iFreeSurvivors++;
		if ( IsWeaponSlotActive(i, 1) && g_iClientInvFlags[i] & FLAG_CHAINSAW )
			iFreeSurvivors++;
	}

	float fCountScale = g_fCvar_GrenadeThrow_HordeSize;
	int iFinalCount = RoundFloat(iFreeSurvivors * fCountScale);
	if (iFinalCount < 1)iFinalCount = RoundFloat(fCountScale);
	if (L4D2_IsTankInPlay())iFinalCount = RoundFloat(iFinalCount * 0.66);
	return iFinalCount;
}

bool CheckCanThrowGrenade(int iClient, int iTarget, int iGrenadeType, float fThrowPos[3], bool bIsThrowTargetTank)
{
	if (IsWeaponReloading(L4D_GetPlayerCurrentWeapon(iClient)))
		return false;

	if (g_iBot_ThreatInfectedCount[iClient] >= GetCommonHitsUntilDown(iClient, 0.33))
		return false;

	if (IsSurvivorBusy(iClient, _, true, true))
		return false;

	int iGrenadeBit = (iGrenadeType == 2 ? 1 : iGrenadeType == 3 ? 2 : 0);
	if (g_iCvar_GrenadeThrow_GrenadeTypes & (1 << iGrenadeBit) == 0)
		return false;

	if (iGrenadeType == 2) 
	{
		if (g_bBot_IsFriendNearThrowArea[iClient])
			return false;

		if ((fThrowPos[2] - g_fClientAbsOrigin[iClient][2]) > 256.0)
			return false;

		if (GetGameTime() < g_fBot_Grenade_NextThrowTime_Molotov)
			return false;

		if (IsEntityOnFire(iTarget))
			return false;

		if (GetVectorDistance(g_fClientAbsOrigin[iClient], fThrowPos, true) <= BOT_GRENADE_CHECK_RADIUS_SQR)
			return false;

		if (IsFinaleEscapeVehicleArrived())
			return false;
	}
	else
	{
		if (GetGameTime() < g_fBot_Grenade_NextThrowTime)
			return false;

		if (iGrenadeType == 1 && !g_bCvar_Nightmare)
		{
			if (bIsThrowTargetTank)
				return false;

			for (int i = 1; i <= MaxClients; i++)
			{
				if (IsClientSurvivor(i) && L4D_IsPlayerBoomerBiled(i))
					return false;
			}
		}
		else if (iGrenadeType == 3 && bIsThrowTargetTank)
		{
			if (GetGameTime() <= g_fInfectedBot_CoveredInVomitTime[iTarget])
				return false;

			if (GetInfectedCount(iTarget, g_fCvar_ChaseBileRange, 10, _, false) < 10)
				return false;
		}
	}

	if ((bIsThrowTargetTank || g_bCvar_Nightmare) && !IsVisibleEntity(iClient, iTarget, MASK_SHOT_HULL))
		return true;

	if (!bIsThrowTargetTank)
	{
		int iThrowCount = CalculateGrenadeThrowInfectedCount();		
		if (g_iBot_GrenadeInfectedCount[iClient] < iThrowCount)
			return false;

		int iChaseEnt = INVALID_ENT_REFERENCE;
		float fItRange = (g_fCvar_ChaseBileRange*g_fCvar_ChaseBileRange);
		while ((iChaseEnt = FindEntityByClassname(iChaseEnt, "info_goal_infected_chase")) != INVALID_ENT_REFERENCE)
		{
			if (GetEntityDistance(iChaseEnt, iTarget, true) <= fItRange)
				return false;
		}

		iChaseEnt = INVALID_ENT_REFERENCE;
		while ((iChaseEnt = FindEntityByClassname(iChaseEnt, "pipe_bomb_projectile")) != INVALID_ENT_REFERENCE)
		{
			if (GetEntityDistance(iChaseEnt, iTarget, true) <= 1048576.0) // 1024
				return false;
		}
	}

	int iActiveGrenades = (
		GetTeamActiveItemCount(L4D2WeaponId_PipeBomb) + 
		GetTeamActiveItemCount(L4D2WeaponId_Molotov) + 
		GetTeamActiveItemCount(L4D2WeaponId_Vomitjar)
	);
	if (iActiveGrenades > 0)
		return false;

	return true;
}

bool CheckIsUnableToThrowGrenade(int iClient, int iTarget, int iGrenadeType, float fThrowPos[3], bool bIsThrowTargetTank)
{
	if (iGrenadeType == 1 && !g_bCvar_Nightmare)
	{
		if (bIsThrowTargetTank)
			return true;

		for (int i = 1; i <= MaxClients; i++)
		{
			if (IsClientSurvivor(i) && L4D_IsPlayerBoomerBiled(i))
				return true;
		}
	}
	else if (iGrenadeType == 2)
	{
		if (!bIsThrowTargetTank && !g_bCvar_Nightmare)
			return true;

		if (g_bBot_IsFriendNearThrowArea[iClient])
			return true;

		if ((fThrowPos[2] - g_fClientAbsOrigin[iClient][2]) > 256.0)
			return true;

		if (IsEntityOnFire(iTarget))
			return true;

		if (GetVectorDistance(g_fClientAbsOrigin[iClient], fThrowPos, true) <= BOT_GRENADE_CHECK_RADIUS_SQR)
			return true;
	}
	else if (iGrenadeType == 3 && bIsThrowTargetTank)
	{
		if (GetGameTime() <= g_fInfectedBot_CoveredInVomitTime[iTarget])
			return true;

		if (GetInfectedCount(iTarget, g_fCvar_ChaseBileRange, 10, _, false) < 10)
			return true;
	}

	if ((bIsThrowTargetTank || g_bCvar_Nightmare) && !IsVisibleEntity(iClient, iTarget, MASK_SHOT_HULL))
		return true;
	
	if (!bIsThrowTargetTank)
	{
		int iThrowCount = CalculateGrenadeThrowInfectedCount();
		if (g_iBot_GrenadeInfectedCount[iClient] < RoundFloat(iThrowCount * 0.33))
			return true;

		int iChaseEnt = INVALID_ENT_REFERENCE;
		float fItRange = (g_fCvar_ChaseBileRange*g_fCvar_ChaseBileRange);
		while ((iChaseEnt = FindEntityByClassname(iChaseEnt, "info_goal_infected_chase")) != INVALID_ENT_REFERENCE)
		{
			if (GetEntityDistance(iChaseEnt, iTarget, true) <= fItRange)
				return true;
		}

		iChaseEnt = INVALID_ENT_REFERENCE;
		while ((iChaseEnt = FindEntityByClassname(iChaseEnt, "pipe_bomb_projectile")) != INVALID_ENT_REFERENCE)
		{
			if (GetEntityDistance(iChaseEnt, iTarget, true) <= 1048576.0) // 1024
				return true;
		}
	}

	int iActiveGrenades = (
		GetTeamActiveItemCount(L4D2WeaponId_PipeBomb) + 
		GetTeamActiveItemCount(L4D2WeaponId_Molotov) + 
		GetTeamActiveItemCount(L4D2WeaponId_Vomitjar)
	);
	if (iActiveGrenades > 1)
		return true;

	return false;
}

void CalculateTrajectory(float fStartPos[3], float fEndPos[3], float fVelocity, float fGravityScale = 1.0, float fResult[3])
{
	MakeVectorFromPoints(fStartPos, fEndPos, fResult);
	fResult[2] = 0.0;

	float fPos_X = GetVectorLength(fResult);
	float fPos_Y = fEndPos[2] - fStartPos[2];

	float fGravity = (g_fCvar_ServerGravity * fGravityScale);

	float fSqrtCalc1 = (fVelocity * fVelocity * fVelocity * fVelocity);
	float fSqrtCalc2 = fGravity * ((fGravity * (fPos_X * fPos_X)) + (2.0 * fPos_Y * (fVelocity * fVelocity)));

	float fCalcSum = (fSqrtCalc1 - fSqrtCalc2);	
	if (fCalcSum < 0.0)fCalcSum = FloatAbs(fCalcSum);

	float fAngSqrt = SquareRoot(fCalcSum);
	float fAngPos = ArcTangent(((fVelocity * fVelocity) + fAngSqrt) / (fGravity * fPos_X));
	float fAngNeg = ArcTangent(((fVelocity * fVelocity) - fAngSqrt) / (fGravity * fPos_X));

	float fPitch = ((fAngPos > fAngNeg) ? fAngNeg : fAngPos);
	fResult[2] = (Tangent(fPitch) * fPos_X);

	NormalizeVector(fResult, fResult);
	ScaleVector(fResult, fVelocity);
}

float GetCommonInfectedDamage()
{
	switch(GetCurrentGameDifficulty())
	{
		case 1:	return 1.0;
		case 3:	return 5.0;
		case 4: return 20.0;
		default: return 2.0;
	}
}

int GetCommonHitsUntilDown(int iClient, float fScale = 1.0)
{
	int iHits = RoundToFloor((GetClientRealHealth(iClient) / GetCommonInfectedDamage()) * fScale);
	if (iHits < 1)iHits = 1;
	return iHits;
}

float GetClientMaxSpeed(int iClient)
{
	return (GetEntPropFloat(iClient, Prop_Send, "m_flMaxspeed") * GetEntPropFloat(iClient, Prop_Data, "m_flLaggedMovementValue"));
}

bool PressAttackButton(int iClient, int &buttons, float fFireRate = -1.0)
{
	if (g_bClient_IsFiringWeapon[iClient])
		return false;
	
	static int iWeapon, iPistol;
	static float fClampDist, fCycleTime, fNextFireT, fAimPos[3];
	static L4D2WeaponId iWeaponID;
	iWeapon = L4D_GetPlayerCurrentWeapon(iClient);
	if (iWeapon == -1)return false;

	if (IsFakeClient(iClient) && (g_bCvar_BotsDontShoot || GetGameTime() <= g_fBot_PreventFireTime[iClient]))
		return false;

	iWeaponID = view_as<L4D2WeaponId>(g_iWeaponID[iWeapon]);
	fNextFireT = fFireRate;
	iPistol = (iWeaponID == L4D2WeaponId_Pistol ? 1 : iWeaponID == L4D2WeaponId_PistolMagnum ? 2 : 0);
	if (fNextFireT <= 0.0 && (iPistol != 0 || GetWeaponTier(iWeapon) > 0))
	{
		fCycleTime = GetWeaponCycleTime(iWeapon);
		if (iPistol == 1 && g_iClientInvFlags[iClient] & FLAG_PISTOL_EXTRA)
			fCycleTime *= 2.5;
		GetClientAimPosition(iClient, fAimPos);

		fClampDist = 1800.0;
		if (iPistol == 2)
			fClampDist *= 0.5;
		else if (GetEntProp(iWeapon, Prop_Send, "m_upgradeBitVec") & L4D2_WEPUPGFLAG_LASER)
			fClampDist *= 2.0;

		fNextFireT = (fCycleTime * (GetVectorDistance(g_fClientEyePos[iClient], fAimPos) / fClampDist));
	}

	if (fNextFireT < GetGameFrameTime())
	{
		if (g_bIsSemiAuto[iWeaponID])
			fNextFireT = GetGameFrameTime();
	}

	if (fNextFireT <= 0.0 || GetGameTime() > g_fBot_NextPressAttackTime[iClient])
	{
		if (GetWeaponClip1(iWeapon) > 0)g_fBot_BlockWeaponReloadTime[iClient] = GetGameTime() + 2.0;
		buttons |= IN_ATTACK;
		g_bClient_IsFiringWeapon[iClient] = true;
		g_fBot_NextPressAttackTime[iClient] = GetGameTime() + fNextFireT;
		return true;
	}
	return false;
}

int GetWeaponAmmoType(int iWeapon)
{
	if (!HasEntProp(iWeapon, Prop_Send, "m_iPrimaryAmmoType"))return -1;
	return (GetEntProp(iWeapon, Prop_Send, "m_iPrimaryAmmoType"));
}

int GetClientPrimaryAmmo(int iClient)
{	
	int iPrimaryWeapon = GetWeaponInInventory(iClient, 0);
	if (!iPrimaryWeapon)return -1;

	int iAmmoType = GetWeaponAmmoType(iPrimaryWeapon);
	if (iAmmoType == -1)return -1;

	return (GetEntProp(iClient, Prop_Send, "m_iAmmo", _, iAmmoType));
}

public Action L4D_OnVomitedUpon(int victim, int &attacker, bool &boomerExplosion)
{
	if (IsFakeClient(victim) && GetGameTime() >= g_fBot_VomitBlindedTime[victim])
		g_fBot_VomitBlindedTime[victim] = GetGameTime() + g_fCvar_BotsVomitBlindTime;

	return Plugin_Continue;
}

public Action L4D2_OnHitByVomitJar(int victim, int &attacker)
{
	float fInterval = 180.0;
	if (IsSpecialInfected(victim))
		fInterval = (IsFakeClient(victim) ? g_fCvar_BileCoverDuration_Bot : g_fCvar_BileCoverDuration_PZ);

	g_fInfectedBot_CoveredInVomitTime[victim] = GetGameTime() + fInterval;
	return Plugin_Continue;
}

bool L4D_IsPlayerBoomerBiled(int iClient)
{
	return (GetGameTime() <= GetEntPropFloat(iClient, Prop_Send, "m_itTimer", 1));
}

bool BotShouldScavengeItem(int iClient, int iItem, int iItemTier, int iScavengeItem, int iPrimarySlot)
{
	static int iTier3Primary, iSecondarySlot, iItemFlags, iWpnTier, iBotPreference;
	iItemFlags = g_iItemFlags[iItem];

	if (g_bCvar_SwitchOffCSSWeapons && iItemFlags & FLAG_CSS)
		return false;

	if (iPrimarySlot)
	{
		if (iItemTier > 0 && L4D_IsValidEnt(iScavengeItem) && (g_iWeaponID[iScavengeItem] == 54 || GetWeaponTier(iScavengeItem) > 0) ) // L4D2WeaponId_Ammo = 54
			return false;

		iWpnTier = GetWeaponTier(iPrimarySlot);
		iBotPreference = GetBotWeaponPreference(iClient);
		iTier3Primary = SurvivorHasTier3Weapon(iClient);

		if (iBotPreference != 0)
		{
			if (!iTier3Primary && (iWpnTier == 2 || iWpnTier == 1 && iBotPreference == L4D_WEAPON_PREFERENCE_SMG) && WeaponHasEnoughAmmo(iPrimarySlot))
			{
				if (iBotPreference != L4D_WEAPON_PREFERENCE_ASSAULTRIFLE && iItemFlags & FLAG_ASSAULT)
					return false;
				if (iBotPreference != L4D_WEAPON_PREFERENCE_SHOTGUN && iItemFlags & FLAG_SHOTGUN && iItemFlags & FLAG_TIER2)
					return false;
				if (iBotPreference != L4D_WEAPON_PREFERENCE_SNIPERRIFLE && iItemFlags & FLAG_SNIPER)
					return false;
			}
		}

		//if(g_bCvar_Debug)
		//	PrintToServer("WeaponHasEnoughAmmo %b ItemTier %d", WeaponHasEnoughAmmo(iPrimarySlot), iItemTier);

		if (iItemTier > 0)
		{
			if (WeaponHasEnoughAmmo(iPrimarySlot)
				&& (iTier3Primary == 1 && GetSurvivorTeamInventoryCount(FLAG_GL) <= g_iCvar_MaxWeaponTier3_GLauncher
				|| iTier3Primary == 2 && GetSurvivorTeamInventoryCount(FLAG_M60) <= g_iCvar_MaxWeaponTier3_M60))
				return false;

			if (GetWeaponAmmoLeft(iPrimarySlot) < (g_iWeapon_MaxAmmo[iPrimarySlot] / 3) && IsNearAmmoPile(iClient))
				return false;
		}

		if (g_bCvar_Nightmare && iItemFlags & FLAG_AMMO)
		{
			bool bHumanUsedAmmo = true;
			for (int i = 1; i <= MaxClients; i++)
			{
				if (i == iClient || !IsClientSurvivor(i) || IsFakeClient(i) || !GetWeaponInInventory(i, 0))
					continue;

				if (!(g_iItem_Used[iItem] & (1 << (i - 1))) && GetSpawnerItemCount(iItem) <= 4)
				{
					bHumanUsedAmmo = false;
					break;
				}
			}
			
			// if nightmare, don't refill and use up ammopile until human players do
			if (!bHumanUsedAmmo)return false;
		}
	}
	else if (g_bCvar_Nightmare && iItemTier > 0)
	{
		bool bHumanHasNoWpn = false;
		for (int i = 1; i <= MaxClients; i++)
		{
			if (i == iClient || !IsClientSurvivor(i) || IsFakeClient(i))
				continue;

			if (!GetWeaponInInventory(i, 0))
			{
				bHumanHasNoWpn = true;
				break;
			}
		}

		// if nightmare, don't pickup any primary weapons until human players do
		if (bHumanHasNoWpn)return false;	
	}

	iSecondarySlot = GetWeaponInInventory(iClient, 1);
	if (iSecondarySlot)
	{
		// if i have magnum and prefer it to pistol
		if (g_bCvar_BotWeaponPreference_ForceMagnum && iItemFlags & FLAG_PISTOL && SurvivorHasPistol(iClient) == 3)
			return false;

		// if it's melee AND i don't have melee AND team has enough melee OR i have chainsaw AND not too many chainsaw/melee in team
		if (iItemFlags & FLAG_MELEE && (!(g_iClientInvFlags[iClient] & FLAG_MELEE) && (GetSurvivorTeamInventoryCount(FLAG_MELEE) >= g_iCvar_MaxMeleeSurvivors)
			|| g_iClientInvFlags[iClient] & FLAG_CHAINSAW && GetSurvivorTeamInventoryCount(FLAG_CHAINSAW) <= g_iCvar_ImprovedMelee_ChainsawLimit))
			return false;

		// if it's pistols AND i have chainsaw AND it has fuel AND not too many chainsaw in team
		if (iItemFlags & (FLAG_PISTOL | FLAG_PISTOL_EXTRA) && g_iClientInvFlags[iClient] & FLAG_CHAINSAW
			&& g_iWeapon_Clip1[iSecondarySlot] > RoundFloat(GetWeaponMaxAmmo(iSecondarySlot) * 0.25)
			&& GetSurvivorTeamInventoryCount(FLAG_CHAINSAW) <= g_iCvar_ImprovedMelee_ChainsawLimit
		)
			return false;
	}

	if (iItemFlags & (FLAG_MEDKIT | FLAG_DEFIB) && g_iClientInvFlags[iClient] & FLAG_UPGRADE && IsWeaponSlotActive(iClient, 3))
		return false;

	if (g_iBot_DefibTarget[iClient] && iItemFlags & FLAG_MEDKIT && g_iClientInvFlags[iClient] & FLAG_DEFIB )
		return false;

	return true;
}

public Action L4D2_OnFindScavengeItem(int iClient, int &iItem)
{
	if (iItem <= 0)
		return Plugin_Continue;
	
	static int iPrimarySlot, iPrimaryAmmo, iItemTier, iScavengeItem;
	static char szWeaponName[64];

	iPrimarySlot = GetWeaponInInventory(iClient, 0);	
	iPrimaryAmmo = 0;
	iScavengeItem = g_iBot_ScavengeItem[iClient];
	iItemTier = GetWeaponTier(iItem);
	GetEdictClassname(iItem, szWeaponName, sizeof(szWeaponName));

	if ((g_iWeaponID[iItem] <= 0 || iItemTier == -1) && !strcmp(szWeaponName, "weapon_spawn"))
	{
		static float fItemPos[3];
		GetEntityAbsOrigin(iItem, fItemPos);

		CheckEntityForStuff(iItem, szWeaponName);
		// PrintToServer("OnFindScavengeItem: %d %s wepid %d tier %d\npos %.2f %.2f %.2f", iItem, szWeaponName, g_iWeaponID[iItem], iItemTier, fItemPos[0], fItemPos[1], fItemPos[2]);
		return Plugin_Handled;
	}

	if (g_iCvar_Debug & DEBUG_SCAVENGE && (!g_iCvar_DebugClient || iClient == g_iCvar_DebugClient))
	{
		static char szEntClass[64], szEntClassname[64];		
		if (iPrimarySlot)
			strcopy(szEntClass, 64, IBWeaponName[g_iWeaponID[iPrimarySlot]]);
		if (L4D_IsValidEnt(iScavengeItem))
			GetEdictClassname(iScavengeItem, szEntClassname, sizeof(szEntClassname));

		PrintToServer("OnFindScavengeItem: %N has %s ammo %d, goes for %s weapon ID %d, ScavengeItem %s", iClient, szEntClass, iPrimaryAmmo, szWeaponName, g_iWeaponID[iItem], szEntClassname);
	}

	if (!BotShouldScavengeItem(iClient, iItem, iItemTier, iScavengeItem, iPrimarySlot))
	{
		iItem = g_iBot_ScavengeItem[iClient];
		return (!iItem ? Plugin_Handled : Plugin_Changed);
	}

	return Plugin_Continue;
}

int GetWeaponAmmoLeft(int iWeapon, bool bIncludeClip = true)
{
	int iAmmoLeft = g_iWeapon_AmmoLeft[iWeapon];
	if (bIncludeClip)iAmmoLeft += g_iWeapon_Clip1[iWeapon];
	return iAmmoLeft;
}

bool WeaponHasEnoughAmmo(int iWeapon, bool bIncludeClip = true)
{
	int iMaxAmmo = g_iWeapon_MaxAmmo[iWeapon];
	int iCurAmmo = GetWeaponAmmoLeft(iWeapon, bIncludeClip);
	return (iMaxAmmo <= 0 || iCurAmmo >= RoundFloat(iMaxAmmo * g_fCvar_HasEnoughAmmoRatio));
}

stock bool IsEntityWeapon(int iEntity, bool bNoSpawn = false)
{
	if (!L4D_IsValidEnt(iEntity))
		return false;

	char sEntClass[64]; GetWeaponClassname(iEntity, sEntClass, sizeof(sEntClass));
	if (strcmp(sEntClass, "predicted_viewmodel") == 0 || bNoSpawn && strcmp(sEntClass, "weapon_spawn") == 0)
		return false;

	ReplaceString(sEntClass, sizeof(sEntClass), "_spawn", "", false);
	return (L4D2_IsValidWeaponName(sEntClass));
}

int CheckForItemsToScavenge(int iClient)
{
	static int iItem, iArrayItem, iItemBits, iItemFlags,
		iPrimarySlot, iTier3Primary, iMinAmmo, iWpnPreference,
		iSecondarySlot, iMeleeCount, iChainsawCount, iMeleeType, iCurrentMeleePref,
		iGrenadeSlot, iGrenadeTypeLimit,
		iMedkitSlot, iPillsSlot;

	static ArrayList hItemList, hWpnWithAmmoList, hFoundMeleeList;

	hItemList = new ArrayList();
	hWpnWithAmmoList = new ArrayList();
	hFoundMeleeList = new ArrayList();

	iItem = 0;
	iItemBits = g_iCvar_ItemScavenge_Items;
	// =========================

	iPrimarySlot = GetWeaponInInventory(iClient, 0);	
	iSecondarySlot = GetWeaponInInventory(iClient, 1);
	iGrenadeSlot = GetWeaponInInventory(iClient, 2);
	iMedkitSlot = GetWeaponInInventory(iClient, 3);
	iPillsSlot = GetWeaponInInventory(iClient, 4);
	
	iTier3Primary = SurvivorHasTier3Weapon(iClient);
	iWpnPreference = GetBotWeaponPreference(iClient);

	if (!iPrimarySlot && iItemBits & PICKUP_PRIMARY)	// if can pick primary AND no primary
	{
		bool bHumanHasNoWpn = false;
		// if nightmare, don't pickup any primary weapons until human players do
		if (g_bCvar_Nightmare)
		{
			for (int i = 1; i <= MaxClients; i++)
			{
				if (i == iClient || !IsClientSurvivor(i) || IsFakeClient(i))
					continue;

				if (!GetWeaponInInventory(i, 0))
				{
					bHumanHasNoWpn = true;
					break;
				}
			}
		}

		if (!bHumanHasNoWpn)
		{
			if ((iArrayItem = GetItemFromArray(g_hAssaultRifleList, iClient)))
				hItemList.Push(iArrayItem);

			if ((iArrayItem = GetItemFromArray(g_hShotgunT2List, iClient)))
				hItemList.Push(iArrayItem);

			if ((iArrayItem = GetItemFromArray(g_hSniperRifleList, iClient)))
				hItemList.Push(iArrayItem);

			if ((iArrayItem = GetItemFromArray(g_hShotgunT1List, iClient)))
				hItemList.Push(iArrayItem);

			if ((iArrayItem = GetItemFromArray(g_hSMGList, iClient)))
				hItemList.Push(iArrayItem);
		}
	}

	if (!iTier3Primary && iWpnPreference != L4D_WEAPON_PREFERENCE_SECONDARY)
	{
		ArrayList hWeaponList;
		bool bHasWep = false;

		switch(iWpnPreference)
		{
			case L4D_WEAPON_PREFERENCE_ASSAULTRIFLE:
			{
				hWeaponList = g_hAssaultRifleList;
				bHasWep = SurvivorHasAssaultRifle(iClient);
			}
			case L4D_WEAPON_PREFERENCE_SHOTGUN:
			{
				hWeaponList = g_hShotgunT2List;
				bHasWep = (SurvivorHasShotgun(iClient) > 1);
			}
			case L4D_WEAPON_PREFERENCE_SNIPERRIFLE:
			{
				hWeaponList = g_hSniperRifleList;
				bHasWep = SurvivorHasSniperRifle(iClient);
			}
			case L4D_WEAPON_PREFERENCE_SMG:
			{
				hWeaponList = g_hSMGList;
				bHasWep = SurvivorHasSMG(iClient);
			}
		}

		if (!bHasWep && iItemBits & PICKUP_PRIMARY || GetWeaponTier(iPrimarySlot) == 1 && iWpnPreference != L4D_WEAPON_PREFERENCE_SMG)
		{
			GetItemFromArray(hWeaponList, iClient, _, _, _, _, _, _, hWpnWithAmmoList);

			if (hWpnWithAmmoList.Length > 0)
			{
				iArrayItem = 0;

				int iCurItem;
				for (int i = 0; i < hWpnWithAmmoList.Length; i++)
				{
					iCurItem = hWpnWithAmmoList.Get(i);
					if (!WeaponHasEnoughAmmo(iCurItem) && !IsNearAmmoPile(iCurItem, iClient))
						continue;

					iArrayItem = iCurItem;
				}

				if (iArrayItem)
					hItemList.Push(iArrayItem);
			}
			hWpnWithAmmoList.Clear();
		}

		if (GetSurvivorTeamInventoryCount(FLAG_GL) < g_iCvar_MaxWeaponTier3_GLauncher)
		{
			if ((iArrayItem = GetItemFromArray(g_hTier3List, iClient, _, 21))) // L4D2WeaponId_GrenadeLauncher
				hItemList.Push(iArrayItem);
		}

		if (GetSurvivorTeamInventoryCount(FLAG_M60) < g_iCvar_MaxWeaponTier3_M60)
		{
			if ((iArrayItem = GetItemFromArray(g_hTier3List, iClient, _, 37))) // L4D2WeaponId_RifleM60
				hItemList.Push(iArrayItem);
		}
	}

	if (!iMedkitSlot)
	{
		if (iItemBits & PICKUP_MEDKIT && (iArrayItem = GetItemFromArray(g_hFirstAidKitList, iClient)))
			hItemList.Push(iArrayItem);

		if (iItemBits & PICKUP_UPGRADE && (iArrayItem = GetItemFromArray(g_hUpgradePackList, iClient)))
			hItemList.Push(iArrayItem);
	}
	if (!iMedkitSlot && iItemBits & PICKUP_DEFIB || g_iBot_DefibTarget[iClient] && !(g_iClientInvFlags[iClient] & FLAG_DEFIB))
	{
		if ((iArrayItem = GetItemFromArray(g_hDefibrillatorList, iClient)))
			hItemList.Push(iArrayItem);
	}

	if (!iPillsSlot)
	{
		if (iItemBits & PICKUP_PILLS && (iArrayItem = GetItemFromArray(g_hPainPillsList, iClient)))
			hItemList.Push(iArrayItem);

		if (iItemBits & PICKUP_ADREN && (iArrayItem = GetItemFromArray(g_hAdrenalineList, iClient)))
			hItemList.Push(iArrayItem);
	}

	if (!iGrenadeSlot)
	{
		if (iItemBits & PICKUP_PIPE && (iArrayItem = GetItemFromArray(g_hGrenadeList, iClient, _, 14)))
			hItemList.Push(iArrayItem);

		if (iItemBits & PICKUP_MOLO && (iArrayItem = GetItemFromArray(g_hGrenadeList, iClient, _, 13)))
			hItemList.Push(iArrayItem);

		if (iItemBits & PICKUP_BILE && (iArrayItem = GetItemFromArray(g_hGrenadeList, iClient, _, 25)))
			hItemList.Push(iArrayItem);
	}
	else if (g_bCvar_SwapSameTypeGrenades)
	{
		iGrenadeTypeLimit = RoundFloat(GetSurvivorTeamInventoryCount(FLAG_GREN) * 0.55);
		if (iGrenadeTypeLimit < 1)iGrenadeTypeLimit = 1;

		if (GetSurvivorTeamItemCount(view_as<L4D2WeaponId>(g_iWeaponID[iGrenadeSlot])) > iGrenadeTypeLimit)
		{
			if ((iArrayItem = GetItemFromArray(g_hGrenadeList, iClient, _, -g_iWeaponID[iGrenadeSlot])))
				hItemList.Push(iArrayItem);
		}
	}

	iMinAmmo = 0;
	if (iPrimarySlot)
	{
		iItemFlags = g_iItemFlags[iPrimarySlot];
		iMinAmmo = GetWeaponMaxAmmo(iPrimarySlot);

		if (iItemBits & PICKUP_AMMO && (!iTier3Primary || iTier3Primary & g_iCvar_T3_Refill))	// if can pick up ammo
		{
			if (!L4D_IsInFirstCheckpoint(iClient))
			{
				if (!LBI_IsSurvivorBotAvailable(iClient))
					iMinAmmo = RoundFloat(iMinAmmo * g_fCvar_HasEnoughAmmoRatio);
				else
					iMinAmmo = RoundFloat(iMinAmmo * 0.8);
			}

			//if(g_bCvar_Debug)
			//{
			//	char sClientName[128];
			//	GetClientName(iClient, sClientName, sizeof(sClientName));
			//	PrintToServer("%s iMinAmmo %d primary ammo %d", sClientName, iMinAmmo, GetClientPrimaryAmmo(iClient));
			//}

			if (GetClientPrimaryAmmo(iClient) < iMinAmmo && (iArrayItem = GetItemFromArray(g_hAmmopileList, iClient)))
			{
				bool bHumanUsedAmmo = true;
				// if nightmare, don't refill and use up ammopile until human players do
				if (g_bCvar_Nightmare)
				{
					for (int i = 1; i <= MaxClients; i++)
					{
						if (i == iClient || !IsClientSurvivor(i) || IsFakeClient(i) || !GetWeaponInInventory(i, 0))
							continue;

						if (!(g_iItem_Used[iItem] & (1 << (i - 1))) && GetSpawnerItemCount(iItem) <= 4)
						{
							bHumanUsedAmmo = false;
							break;
						}
					}
				}
				if (bHumanUsedAmmo)
					hItemList.Push(iArrayItem);
			}
		}

		if (!iTier3Primary)
		{
			if (g_bCvar_SwapSameTypePrimaries_T1 && iWpnPreference != L4D_WEAPON_PREFERENCE_SMG)
			{
				int iSMGCount = GetSurvivorTeamInventoryCount(FLAG_SMG);
				int iShotgunCount = GetSurvivorTeamInventoryCount(FLAG_SHOTGUN, FLAG_TIER1);

				int iTier1Limit = RoundToCeil((iSMGCount + iShotgunCount) * 0.5);
				if (iTier1Limit < 1)iTier1Limit = 1;

				ArrayList hWepArray;
				if (iShotgunCount > iTier1Limit && iItemFlags & (FLAG_SHOTGUN | FLAG_TIER1))
					hWepArray = g_hSMGList;
				else if (iSMGCount > iTier1Limit && iItemFlags & FLAG_SMG)
					hWepArray = g_hShotgunT1List;
				GetItemFromArray(hWepArray, iClient, _, _, _, _, _, _, hWpnWithAmmoList);
				
				if (hWpnWithAmmoList.Length > 0)
				{
					iArrayItem = 0;

					int iCurItem;
					for (int i = 0; i < hWpnWithAmmoList.Length; i++)
					{
						iCurItem = hWpnWithAmmoList.Get(i);
						if (!WeaponHasEnoughAmmo(iCurItem) && !IsNearAmmoPile(iCurItem, iClient))
							continue;

						iArrayItem = iCurItem;
					}

					if (iArrayItem)
						hItemList.Push(iArrayItem);
				}
				hWpnWithAmmoList.Clear();
			}

			if (g_bCvar_SwapSameTypePrimaries_T2)
			{
				int iWepLimit = -1;
				ArrayList hWepArray;
				if (SurvivorHasShotgun(iClient))
				{
					hWepArray = g_hShotgunT2List;
					iWepLimit = RoundFloat(GetSurvivorTeamInventoryCount(FLAG_SHOTGUN, FLAG_TIER2) * 0.5);
				}
				else if (SurvivorHasAssaultRifle(iClient))
				{
					hWepArray = g_hAssaultRifleList;
					iWepLimit = RoundFloat(GetSurvivorTeamInventoryCount(FLAG_ASSAULT) * 0.5);
				}
				else if (SurvivorHasSniperRifle(iClient))
				{
					hWepArray = g_hSniperRifleList;
					iWepLimit = RoundFloat(GetSurvivorTeamInventoryCount(FLAG_SNIPER) * 0.5);
				}

				if (iWepLimit != -1 && iWepLimit < 1)
					iWepLimit = 1;

				int iPrimaryCount = GetSurvivorTeamItemCount(L4D2_GetWeaponId(iPrimarySlot));
				if (iPrimaryCount > iWepLimit)
				{
					GetItemFromArray(hWepArray, iClient, _, -g_iWeaponID[iPrimarySlot], _, _, _, _, hWpnWithAmmoList);
					
					if (hWpnWithAmmoList.Length > 0)
					{
						iArrayItem = 0;

						int iCurItem;
						for (int i = 0; i < hWpnWithAmmoList.Length; i++)
						{
							iCurItem = hWpnWithAmmoList.Get(i);
							if (!WeaponHasEnoughAmmo(iCurItem) && !IsNearAmmoPile(iCurItem, iClient))
								continue;

							iArrayItem = iCurItem;
						}

						if (iArrayItem)
							hItemList.Push(iArrayItem);
					}
					hWpnWithAmmoList.Clear();
				}
			}
		}

		int iUpgradeBits = GetEntProp(iPrimarySlot, Prop_Send, "m_upgradeBitVec");
		if (iItemBits & PICKUP_LASER && !(iUpgradeBits & L4D2_WEPUPGFLAG_LASER))
		{
			if ((iArrayItem = GetItemFromArray(g_hLaserSightList, iClient)))
				hItemList.Push(iArrayItem);
		}
		if (iItemBits & PICKUP_AMMOPACK && !(iUpgradeBits & (L4D2_WEPUPGFLAG_INCENDIARY | L4D2_WEPUPGFLAG_EXPLOSIVE)) )
		{
			if ((iArrayItem = GetItemFromArray(g_hDeployedAmmoPacks, iClient)))
				hItemList.Push(iArrayItem);
		}
	}

	if (iSecondarySlot)
	{
		iMeleeCount = GetSurvivorTeamInventoryCount(FLAG_MELEE, -FLAG_CHAINSAW);
		iChainsawCount = GetSurvivorTeamInventoryCount(FLAG_CHAINSAW);
		iMeleeType = SurvivorHasMeleeWeapon(iClient);

		static bool bFoundMelee;
		if (iMeleeType != 0)
		{
			if (g_iCvar_Debug & DEBUG_WEP_DATA && g_iCvar_Debug & DEBUG_SCAVENGE)
			{
				static char szEntClassname[64];
				GetEdictClassname(iSecondarySlot, szEntClassname, sizeof(szEntClassname));
				PrintToServer("%N %d %s MeleeID %d", iClient, iSecondarySlot, szEntClassname, g_iMeleeID[iSecondarySlot]);
			}

			iCurrentMeleePref = (g_iMeleeID[iSecondarySlot] < 0 ? -1 : GetMeleePreference(iSecondarySlot));

			if (iMeleeType != 2)
			{
				bFoundMelee = false;

				// look for chainsaw
				if (iChainsawCount < g_iCvar_ImprovedMelee_ChainsawLimit && iItemBits & PICKUP_CHAINSAW)
				{
					iArrayItem = GetItemFromArray(g_hMeleeList, iClient, _, 20); // L4D2WeaponId_Chainsaw
					if (iArrayItem && g_iWeapon_Clip1[iArrayItem] > RoundFloat(GetWeaponMaxAmmo(iArrayItem) * 0.25))
					{
						bFoundMelee = true;
						hItemList.Push(iArrayItem);
					}
				}

				// look for better melee
				if (!bFoundMelee && iCurrentMeleePref != -1 && iItemBits & PICKUP_SECONDARY)
				{
					GetItemFromArray(g_hMeleeList, iClient, _, 19, _, _, _, _, hFoundMeleeList); // L4D2WeaponId_Melee

					if (hFoundMeleeList.Length > 0)
					{
						iArrayItem = 0;
						
						int iCurItem, iCurPref, iLastPref = iCurrentMeleePref;
						for (int i = 0; i < hFoundMeleeList.Length; i++)
						{
							iCurItem = hFoundMeleeList.Get(i);
							iCurPref = GetMeleePreference(iCurItem);

							if (iCurPref <= iLastPref)
								continue;

							iArrayItem = iCurItem;
							iLastPref = iCurPref;
						}

						if (iArrayItem)
						{
							bFoundMelee = true;
							hItemList.Push(iArrayItem);
						}
					}
					hFoundMeleeList.Clear();
				}
			}
			// drop excess or low fuel chainsaw for other melee/pistols
			else if (iMeleeType == 2 && (iChainsawCount > g_iCvar_ImprovedMelee_ChainsawLimit || g_iWeapon_Clip1[iSecondarySlot] <= RoundFloat(GetWeaponMaxAmmo(iSecondarySlot) * 0.25)) )
			{
				bFoundMelee = false;
				if (iMeleeCount < g_iCvar_MaxMeleeSurvivors)
				{
					if ((iArrayItem = GetItemFromArray(g_hMeleeList, iClient, _, 19))) // L4D2WeaponId_Melee
					{
						bFoundMelee = true;
						hItemList.Push(iArrayItem); 
					}
				}

				if (!bFoundMelee && (iArrayItem = GetItemFromArray(g_hPistolList, iClient)))
					hItemList.Push(iArrayItem);
			}

			// drop excess melee for any pistol
			if ((iMeleeCount + iChainsawCount) > g_iCvar_MaxMeleeSurvivors && (iMeleeType != 2 || iChainsawCount > g_iCvar_ImprovedMelee_ChainsawLimit))
			{
				if ((iArrayItem = GetItemFromArray(g_hPistolList, iClient)))
					hItemList.Push(iArrayItem);
			}
		}
		else if (iItemBits & PICKUP_SECONDARY)
		{
			if ((iMeleeCount + iChainsawCount) < g_iCvar_MaxMeleeSurvivors && (iArrayItem = GetItemFromArray(g_hMeleeList, iClient, _, 19))) // L4D2WeaponId_Melee
				hItemList.Push(iArrayItem);

			int iHasPistol = SurvivorHasPistol(iClient);
			if (iHasPistol)
			{
				if (g_bCvar_BotWeaponPreference_ForceMagnum && iHasPistol != 3 || GetSurvivorTeamItemCount(L4D2WeaponId_PistolMagnum) == 0)
				{
					if ((iArrayItem = GetItemFromArray(g_hPistolList, iClient, _, 32))) // L4D2WeaponId_PistolMagnum
						hItemList.Push(iArrayItem);
				}
				else if (iHasPistol == 1)
				{
					if ((iArrayItem = GetItemFromArray(g_hPistolList, iClient, _, 1))) // L4D2WeaponId_Pistol
						hItemList.Push(iArrayItem);
				} 
			}
		}
	}

	if (hItemList.Length > 0)
	{
		int iCurItem;
		float fCurDist, fLastDist = -1.0;
		bool bIsFree;

		for (int i = 0; i < hItemList.Length; i++)
		{
			iCurItem = hItemList.Get(i);

			fCurDist = GetClientDistanceToItem(iClient, iCurItem, true);
			if (fLastDist != -1.0 && fCurDist >= fLastDist)
				continue;

			bIsFree = true;
			iItemFlags = g_iItemFlags[iCurItem];
			for (int j = 1; j <= MaxClients; j++)
			{
				if (j != iClient && g_iBot_ScavengeItem[j] == iCurItem && 
					!(iItemFlags & FLAG_AMMO) && !(iItemFlags & FLAG_LASER) && GetSpawnerItemCount(iCurItem) <= 1
				)
				{
					bIsFree = false;
					break;
				}
			}
			if (!bIsFree)continue;

			iItem = iCurItem;
			fLastDist = fCurDist;
		}
		g_fBot_ScavengeItemDist[iClient] = fLastDist;
	}
	hItemList.Clear();

	if (iItem && g_iCvar_Debug & DEBUG_SCAVENGE && (!g_iCvar_DebugClient || iClient == g_iCvar_DebugClient))
	{
		static char szEntClassname[128];		
		GetEdictClassname(iItem, szEntClassname, sizeof(szEntClassname));

		PrintToServer("CheckForItemsToScavenge: %N MinAmmo %d Ammo %d HasTier3 %d iItem %d %s", iClient,
		iMinAmmo, GetClientPrimaryAmmo(iClient), iTier3Primary, iItem, szEntClassname);
	}
	
	return iItem;
}

int GetBotWeaponPreference(int iClient)
{
	switch(GetSurvivorType(iClient))
	{
		case L4D_SURVIVOR_ROCHELLE:	return g_iCvar_BotWeaponPreference_Rochelle;
		case L4D_SURVIVOR_COACH:	return g_iCvar_BotWeaponPreference_Coach;
		case L4D_SURVIVOR_ELLIS:	return g_iCvar_BotWeaponPreference_Ellis;
		case L4D_SURVIVOR_BILL:		return g_iCvar_BotWeaponPreference_Bill;
		case L4D_SURVIVOR_ZOEY:		return g_iCvar_BotWeaponPreference_Zoey;
		case L4D_SURVIVOR_FRANCIS:	return g_iCvar_BotWeaponPreference_Francis;
		case L4D_SURVIVOR_LOUIS:	return g_iCvar_BotWeaponPreference_Louis;
		default:					return g_iCvar_BotWeaponPreference_Nick;
	}
}

int GetSpawnerItemCount(int iSpawner)
{
	return (!HasEntProp(iSpawner, Prop_Data, "m_itemCount") ? 9999 : GetEntProp(iSpawner, Prop_Data, "m_itemCount"));
}

bool IsNearAmmoPile(int iEntity, int iOwner = 0)
{
	int iAmmoPile = GetItemFromArray(g_hAmmopileList, iEntity, 1024.0, _, _, (1 <= iEntity <= MaxClients), false, false);
	return (iAmmoPile && (!iOwner || LBI_IsReachableEntity(iOwner, iAmmoPile)));
}

// 4th argument is now a number instead of a string
// if number >0, look for a specific weapon ID
// if number is negative, particular weapon ID is skipped/avoided
// check for sModelName is never used, let's keep it that way ;)
int GetItemFromArray(ArrayList hArrayList, int iClient, float fDistance = -1.0, int iWeaponID = 0, const char[] sModelName = "", bool bCheckIsReachable = true, bool bCheckIsVisible = true, bool bReturnClosest = true, ArrayList hFoundList = null)
{
	if (!hArrayList || hArrayList.Length <= 0)return 0;
	if (fDistance == -1.0)fDistance = g_fCvar_ItemScavenge_ApproachVisibleRange;

	static float fCheckDist, fApproachDist, fCurDist, fPickupRange, fClientPos[3], fEntityPos[3];
	static int iCloseItem, iEntRef, iEntIndex, iNavArea, iUseCount, iItemFlags;
	static bool bIsCoop, bInCheckpoint, bInCheckpoint2, bIsTaken, bInUseRange, bIsPlayer;

 	//char sWeaponName[MAX_TARGET_LENGTH];
	static char sEntityModel[PLATFORM_MAX_PATH];

	GetEntityAbsOrigin(iClient, fClientPos);
	bIsPlayer = (1 <= iClient <= MaxClients);
	if (bIsPlayer)
	{
		bIsCoop = L4D2_IsGenericCooperativeMode(); 
		bInCheckpoint2 = LBI_IsPositionInsideCheckpoint(g_fClientAbsOrigin[iClient]);
		fPickupRange = g_fCvar_ItemScavenge_PickupRange_Sqr;
	}

	iCloseItem = 0;
	fCheckDist = MAX_MAP_RANGE_SQR;

	for (int i = 0; i < hArrayList.Length; i++)
	{
		iEntRef = hArrayList.Get(i);
		
		if (g_hForbiddenItemList.FindValue(iEntRef) != -1)
		{
			if (g_iCvar_Debug & DEBUG_SCAVENGE)
			{
				iEntIndex = EntRefToEntIndex(iEntRef);
				PrintToServer("Will not allow snatching %s", IBWeaponName[g_iWeaponID[iEntIndex]]);
			}
			continue;
		}
		
		iEntIndex = EntRefToEntIndex(iEntRef);
		if (iEntIndex == INVALID_ENT_REFERENCE || IsValidClient(GetEntityOwner(iEntIndex)))
			continue;
		
		iItemFlags = g_iItemFlags[iEntIndex];
		if (g_bCvar_SwitchOffCSSWeapons && iItemFlags & FLAG_CSS)
			continue;

		iUseCount = GetSpawnerItemCount(iEntIndex);
		if (iUseCount == 0)continue;

		if (!GetEntityAbsOrigin(iEntIndex, fEntityPos) || GetVectorDistance(fClientPos, fEntityPos, true) > g_fCvar_ItemScavenge_MapSearchRange_Sqr)
			continue;

		if (sModelName[0] != 0)
		{
			GetEntityModelname(iEntIndex, sEntityModel, sizeof(sEntityModel));
			if (strcmp(sEntityModel, sModelName, false) != 0)continue;
		}
		
		//if(g_bCvar_Debug)
		//	PrintToServer("%s %b",IBWeaponName[g_iWeaponID[iEntIndex]], iItemFlags);
	
		//skip if ammo upgrade is already used
		if (g_iItem_Used[iEntIndex] & (1 << (iClient - 1)) && iItemFlags & FLAG_AMMO && iItemFlags & FLAG_UPGRADE)
			continue;
		
		//skip if weapon ID is avoided
		if (iWeaponID < 0 && g_iWeaponID[iEntIndex] == -iWeaponID)
			continue;
		
		//skip if WRONG weapon ID
		if (iWeaponID > 0 && g_iWeaponID[iEntIndex] != iWeaponID)
			continue;

		fApproachDist = fDistance; 
		if (bIsPlayer)
		{
			if (GetWeaponTier(iEntIndex) > 0 && !WeaponHasEnoughAmmo(iEntIndex))
				continue;

			if (iUseCount == 1 && iItemFlags & FLAG_AMMO)
			{
				bIsTaken = false;
				for (int j = 1; j <= MaxClients; j++)
				{
					if (j == iClient || !IsClientSurvivor(j) || !IsFakeClient(j) || iEntIndex != g_iBot_ScavengeItem[j] || !g_iBot_ScavengeItem[j])
						continue;

					bIsTaken = true;
					break;
				}
				if (bIsTaken)continue;
			}

			bInUseRange = (GetVectorDistance(g_fClientEyePos[iClient], fEntityPos, true) <= fPickupRange);
			if (!bInUseRange)
			{
				if (bIsCoop && bInCheckpoint2 && !(bInCheckpoint = LBI_IsPositionInsideCheckpoint(fEntityPos)))
					continue;

				if (bCheckIsReachable && !LBI_IsReachableEntity(iClient, iEntIndex))
					continue;

				if (bCheckIsVisible && fDistance > g_fCvar_ItemScavenge_ApproachRange)
				{ 
					iNavArea = L4D_GetNearestNavArea(fEntityPos, g_fCvar_ItemScavenge_ApproachVisibleRange, true, false, true);
					if (iNavArea && !LBI_IsNavAreaPartiallyVisible(iNavArea, g_fClientEyePos[iClient], iClient))
					{
						fApproachDist = g_fCvar_ItemScavenge_ApproachRange;
					}
				}
			}
		}

		fCurDist = GetVectorDistance(fClientPos, fEntityPos, true);
		if (!g_bTeamHasHumanPlayer)
			fApproachDist *= g_fCvar_ItemScavenge_NoHumansRangeMultiplier;

		if (!bInUseRange && !bInCheckpoint && fCurDist > (fApproachDist*fApproachDist) || fCurDist >= fCheckDist)
			continue;

		if (hFoundList)
			hFoundList.Push(iEntIndex);

		if (!bReturnClosest)
			return iEntIndex;

		iCloseItem = iEntIndex;
		fCheckDist = fCurDist;
	}
	
	return iCloseItem;
}

int GetEntityOwner(int iEntity)
{
	int iOwner = GetEntPropEnt(iEntity, Prop_Data, "m_hOwnerEntity");
	if (!IsClientSurvivor(iOwner) || L4D_GetPlayerCurrentWeapon(iOwner) == iEntity)
		return iOwner;

	for (int i = 0; i <= 5; i++)
	{
		if (GetWeaponInInventory(iOwner, i) == iEntity)
			return iOwner;
	}
	return 0;
}

public void OnEntityCreated(int iEntity, const char[] sClassname)
{
	if (iEntity <= 0 || iEntity > MAXENTITIES)
		return;
		
	g_iItem_Used[iEntity] = 0; // Clear used item bitfield
	g_iWeaponID[iEntity] = 0;
	g_iMeleeID[iEntity] = -1;
	g_iItemFlags[iEntity] = 0;
	
	CheckEntityForStuff(iEntity, sClassname);
}

void CheckEntityForStuff(int iEntity, const char[] sClassname)
{
	if (!g_bInitCheckCases)
	{
		InitCheckCases();
		//PrintToServer("CheckEntityForStuff: InitCheckCases");
	}

	static int iCheckCase;
	if (!g_hCheckCases.GetValue(sClassname, iCheckCase))
		iCheckCase = 0;

	switch(iCheckCase)
	{
		case 1:	//witch
		{
			if (g_hWitchList)
			{
				int iWitchRef;
				for (int i = 0; i < g_hWitchList.Length; i++)
				{
					iWitchRef = EntRefToEntIndex(g_hWitchList.Get(i));
					if (iWitchRef == INVALID_ENT_REFERENCE || !L4D_IsValidEnt(iWitchRef))
					{
						g_hWitchList.Erase(i);
						continue;
					}

					if (iWitchRef == iEntity)
						return;
				}

				int iIndex = g_hWitchList.Push(EntIndexToEntRef(iEntity));
				g_hWitchList.Set(iIndex, 0, 1);

				SDKHook(iEntity, SDKHook_OnTakeDamage, OnWitchTakeDamage);
				return;
			}
		}

		case 2:	//ammo
		{
			PushEntityIntoArray(g_hAmmopileList, iEntity);
			g_iWeaponID[iEntity] = 54; // L4D2WeaponId_Ammo
			g_iItemFlags[iEntity] = FLAG_ITEM | FLAG_AMMO;
			return;
		}

		case 3:	//laser sight
		{
			PushEntityIntoArray(g_hLaserSightList, iEntity);
			g_iItemFlags[iEntity] = FLAG_ITEM | FLAG_LASER;
			return;
		}

		case 4:	//deployed ammo upgrade
		{
			PushEntityIntoArray(g_hDeployedAmmoPacks, iEntity);
			g_iWeaponID[iEntity] = 22; // L4D2WeaponId_AmmoPack, idk if this makes sense actually
			g_iItemFlags[iEntity] = FLAG_ITEM | FLAG_AMMO | FLAG_UPGRADE;
			return;
		}
	}	

	static char sWeaponName[64];
	if (!GetWeaponClassname(iEntity, sWeaponName, sizeof(sWeaponName)))
		return;
	
	static L4D2WeaponId iWeaponID;
	iWeaponID = L4D2WeaponId_None;	
	if(!g_hWeaponToIDMap.GetValue(sWeaponName, iWeaponID))
		return;

	g_iWeaponID[iEntity] = view_as<int>(iWeaponID);
	if (iWeaponID == L4D2WeaponId_Melee)
	{
		if(g_iCvar_Debug & DEBUG_WEP_DATA)
			PrintToServer("CheckEntityForStuff: %d %s %d %s",iEntity, sClassname, iWeaponID, sWeaponName);
		if(!g_bInitMeleeIDs) InitMeleeIDs();
		g_iMeleeID[iEntity] = GetMeleeID(iEntity);
	}
	
	if (!g_bInitItemFlags)
	{
		InitItemFlagMap();
		//PrintToServer("CheckEntityForStuff: InitItemFlagMap");
	}

	static int iItemFlags;
	iItemFlags = 0;
	g_hItemFlagMap.GetValue(sWeaponName, iItemFlags);
	g_iItemFlags[iEntity] = iItemFlags;

	if (iWeaponID > L4D2WeaponId_Machinegun && iWeaponID < L4D2WeaponId_Ammo)
		return;
	
	switch(iWeaponID)
	{
		case L4D2WeaponId_None: return;
		
		case L4D2WeaponId_Pistol, L4D2WeaponId_PistolMagnum:
			PushEntityIntoArray(g_hPistolList, iEntity);
		
		case L4D2WeaponId_Melee, L4D2WeaponId_Chainsaw:
			PushEntityIntoArray(g_hMeleeList, iEntity);
		
		case L4D2WeaponId_Smg, L4D2WeaponId_SmgSilenced, L4D2WeaponId_SmgMP5:
			PushEntityIntoArray(g_hSMGList, iEntity);
		
		case L4D2WeaponId_Pumpshotgun, L4D2WeaponId_ShotgunChrome:
			PushEntityIntoArray(g_hShotgunT1List, iEntity);
		
		case L4D2WeaponId_Rifle, L4D2WeaponId_RifleAK47, L4D2WeaponId_RifleDesert, L4D2WeaponId_RifleSG552:
			PushEntityIntoArray(g_hAssaultRifleList, iEntity);
		
		case L4D2WeaponId_Autoshotgun, L4D2WeaponId_ShotgunSpas:
			PushEntityIntoArray(g_hShotgunT2List, iEntity);
		
		case L4D2WeaponId_HuntingRifle, L4D2WeaponId_SniperMilitary, L4D2WeaponId_SniperScout, L4D2WeaponId_SniperAWP:
			PushEntityIntoArray(g_hSniperRifleList, iEntity);
		
		case L4D2WeaponId_PipeBomb, L4D2WeaponId_Molotov, L4D2WeaponId_Vomitjar:
			PushEntityIntoArray(g_hGrenadeList, iEntity);
		
		case L4D2WeaponId_RifleM60, L4D2WeaponId_GrenadeLauncher:
			PushEntityIntoArray(g_hTier3List, iEntity);
		
		case L4D2WeaponId_FragAmmo, L4D2WeaponId_IncendiaryAmmo:
			PushEntityIntoArray(g_hUpgradePackList, iEntity);
		
		case L4D2WeaponId_FirstAidKit:
			PushEntityIntoArray(g_hFirstAidKitList, iEntity);
		
		case L4D2WeaponId_Defibrillator:
			PushEntityIntoArray(g_hDefibrillatorList, iEntity);
		
		case L4D2WeaponId_PainPills:
			PushEntityIntoArray(g_hPainPillsList, iEntity);
		
		case L4D2WeaponId_Adrenaline:
			PushEntityIntoArray(g_hAdrenalineList, iEntity);
	}

	if (iWeaponID == L4D2WeaponId_Chainsaw)
	{
		g_iWeapon_Clip1[iEntity] = g_iCvar_MaxAmmo_Chainsaw;
		g_iWeapon_MaxAmmo[iEntity] = g_iCvar_MaxAmmo_Chainsaw;
		g_iWeapon_AmmoLeft[iEntity] = g_iCvar_MaxAmmo_Chainsaw;
		g_iMeleeID[iEntity] = 16;
		return;
	}
	
	if (g_iWeaponTier[iWeaponID])
	{
		g_iWeapon_Clip1[iEntity] = L4D2_GetIntWeaponAttribute(sWeaponName, L4D2IWA_ClipSize);
		g_iWeapon_MaxAmmo[iEntity] = GetWeaponMaxAmmo(iEntity);
		g_iWeapon_AmmoLeft[iEntity] = g_iWeapon_MaxAmmo[iEntity];
	}
}

bool ShouldUseFlowDistance()
{
	if (!L4D_IsSurvivalMode() && !L4D2_IsScavengeMode())
	{
		int iFinStage = L4D2_GetCurrentFinaleStage();
		return (iFinStage == 18 || iFinStage == 0);
	}

	return false;
}

void PushEntityIntoArray(ArrayList hArrayList, int iEntity)
{
	if (!hArrayList)return;
	int iEntRef = EntIndexToEntRef(iEntity);
	int iArrayEnt = hArrayList.FindValue(iEntRef);
	if (iArrayEnt == -1)hArrayList.Push(iEntRef);
}

public void OnEntityDestroyed(int iEntity)
{
	if (iEntity <= 0 || iEntity > MAXENTITIES) 
		return;
		
	g_iItem_Used[iEntity] = 0; // Clear used item bitfield
	g_iWeaponID[iEntity] = 0;
	g_iMeleeID[iEntity] = -1;
	g_iItemFlags[iEntity] = 0;

	ValidateEntityRemovalInArray(g_hMeleeList, iEntity);
	ValidateEntityRemovalInArray(g_hPistolList, iEntity);
	ValidateEntityRemovalInArray(g_hSMGList, iEntity);
	ValidateEntityRemovalInArray(g_hShotgunT1List, iEntity);
	ValidateEntityRemovalInArray(g_hShotgunT2List, iEntity);
	ValidateEntityRemovalInArray(g_hAssaultRifleList, iEntity);
	ValidateEntityRemovalInArray(g_hSniperRifleList, iEntity);
	ValidateEntityRemovalInArray(g_hTier3List, iEntity);
	ValidateEntityRemovalInArray(g_hFirstAidKitList, iEntity);
	ValidateEntityRemovalInArray(g_hDefibrillatorList, iEntity);
	ValidateEntityRemovalInArray(g_hPainPillsList, iEntity);
	ValidateEntityRemovalInArray(g_hAdrenalineList, iEntity);
	ValidateEntityRemovalInArray(g_hGrenadeList, iEntity);
	ValidateEntityRemovalInArray(g_hUpgradePackList, iEntity);

	ValidateEntityRemovalInArray(g_hAmmopileList, iEntity);
	ValidateEntityRemovalInArray(g_hLaserSightList, iEntity);
	ValidateEntityRemovalInArray(g_hDeployedAmmoPacks, iEntity);
	ValidateEntityRemovalInArray(g_hForbiddenItemList, iEntity);

	for (int i = 1; i <= MaxClients; i++)
	{
		g_iBot_VisionMemory_State[i][0][iEntity] = g_iBot_VisionMemory_State[i][1][iEntity] = 0;
		g_fBot_VisionMemory_Time[i][0][iEntity] = g_fBot_VisionMemory_Time[i][1][iEntity] = 0.0;
	}
}

void ValidateEntityRemovalInArray(ArrayList hArrayList, int iEntity)
{
	if (!hArrayList)return;
	int iArrayEnt = hArrayList.FindValue(EntIndexToEntRef(iEntity));
	if (iArrayEnt != -1)hArrayList.Erase(iArrayEnt);
}

public void OnMapStart()
{
	g_bMapStarted = true;
	g_iHasLeft4Bots = -1;

	GetCurrentMap(g_sCurrentMapName, sizeof(g_sCurrentMapName));
	for (int i = 1; i <= MaxClients; i++)g_fClient_ThinkFunctionDelay[i] = GetGameTime() + (g_bLateLoad ? 1.0 : 10.0);
	CreateEntityArrayLists();

	// LLM Module
	LLM_OnMapStart();

	if (g_bPluginVScript)
	{
		CreateVScriptCommandDetour();
		g_vsCommand.Register();
	}

	InitMeleeIDs();

	static char sEntClassname[64];
	for (int i = 0; i < MAXENTITIES; i++)
	{
		g_iItem_Used[i] = 0;
		g_iWeaponID[i] = 0;
		g_iMeleeID[i] = -1;
		g_iItemFlags[i] = 0;
		if (!L4D_IsValidEnt(i))continue;
		GetEntityClassname(i, sEntClassname, sizeof(sEntClassname));
		CheckEntityForStuff(i, sEntClassname);
	}
}

void CheckWeaponsLater()
{
	static int iEntIndex;
	static char sEntClassname[64];
	
	int count = 0;
	int i = 0;
	while (i < g_hWeaponsToCheckLater.Length)
	{
		iEntIndex = EntRefToEntIndex(g_hWeaponsToCheckLater.Get(i));
		if (iEntIndex != INVALID_ENT_REFERENCE && L4D_IsValidEnt(iEntIndex))
		{
			GetEntityClassname(iEntIndex, sEntClassname, sizeof(sEntClassname));
			CheckEntityForStuff(iEntIndex, sEntClassname);
			if (g_iWeaponID[iEntIndex] != 0)
			{
				g_hWeaponsToCheckLater.Erase(i);
				count++;
			}
			else
			{
				PrintToServer("CheckWeaponsLater: %d %s WeaponID %d MeleeID %d, stopping and checking again soon\nprocessed %d items",
					iEntIndex, sEntClassname, g_iWeaponID[iEntIndex], g_iMeleeID[iEntIndex], count);
				return;
			}
		}
		else
		{
			g_hWeaponsToCheckLater.Erase(i);
			count++;
		}
	}
	g_hCheckWeaponTimer = INVALID_HANDLE;
	PrintToServer("CheckWeaponsLater: list cleared, processed %d items", count);
}

public Action CheckWeaponsEvenLater(Handle timer)
{
	CheckWeaponsLater();
	return Plugin_Handled;
}

public void OnMapEnd()
{
	g_bMapStarted = false;
	g_sCurrentMapName[0] = 0;
	ClearEntityArrayLists();
	ClearHashMaps();
	
	g_bInitMeleePrefs = false;
	g_bInitPathWithin = false;
	g_hClearBadPathTimer = INVALID_HANDLE;
	g_hCheckWeaponTimer = INVALID_HANDLE;
}

void CreateEntityArrayLists()
{
	g_hMeleeList 			= new ArrayList();
	g_hPistolList 			= new ArrayList();
	g_hSMGList 				= new ArrayList();
	g_hShotgunT1List 		= new ArrayList();
	g_hShotgunT2List 		= new ArrayList();
	g_hAssaultRifleList 	= new ArrayList();
	g_hSniperRifleList 		= new ArrayList();
	g_hTier3List 			= new ArrayList();
	g_hAmmopileList 		= new ArrayList();
	g_hUpgradePackList 		= new ArrayList();
	g_hLaserSightList 		= new ArrayList();
	g_hFirstAidKitList 		= new ArrayList();
	g_hDefibrillatorList 	= new ArrayList();
	g_hPainPillsList 		= new ArrayList();
	g_hAdrenalineList 		= new ArrayList();
	g_hGrenadeList 			= new ArrayList();
	g_hDeployedAmmoPacks 	= new ArrayList();
	g_hForbiddenItemList 	= new ArrayList(2);
	g_hWitchList 			= new ArrayList(2);
	g_hBadPathEntities 		= new ArrayList();
	g_hWeaponsToCheckLater 	= new ArrayList();
}

void ClearEntityArrayLists()
{
	g_hMeleeList.Clear();
	g_hPistolList.Clear();
	g_hSMGList.Clear();
	g_hShotgunT1List.Clear();
	g_hShotgunT2List.Clear();
	g_hAssaultRifleList.Clear();
	g_hSniperRifleList.Clear();
	g_hTier3List.Clear();
	g_hAmmopileList.Clear();
	g_hUpgradePackList.Clear();
	g_hLaserSightList.Clear();
	g_hFirstAidKitList.Clear();
	g_hDefibrillatorList.Clear();
	g_hPainPillsList.Clear();
	g_hAdrenalineList.Clear();
	g_hGrenadeList.Clear();
	g_hDeployedAmmoPacks.Clear();
	g_hForbiddenItemList.Clear();
	g_hWitchList.Clear();
	g_hBadPathEntities.Clear();
	g_hWeaponsToCheckLater.Clear();
}

void ClearHashMaps()
{
	if(g_hMeleeIDs != INVALID_HANDLE)
		g_hMeleeIDs.Clear();
	if(g_hCheckCases != INVALID_HANDLE)
		g_hCheckCases.Clear();
	if(g_hItemFlagMap != INVALID_HANDLE)
		g_hItemFlagMap.Clear();
	if(g_hWeaponMap != INVALID_HANDLE)
		g_hWeaponMap.Clear();
	
	g_bInitMeleeIDs = false;
	g_bInitCheckCases = false;
	g_bInitMaxAmmo = false;
	g_bInitItemFlags = false;
	g_bInitWeaponMap = false;
}

void InitItemFlagMap()
{
	g_hItemFlagMap = CreateTrie();
	g_hItemFlagMap.SetValue("weapon_melee"			, FLAG_WEAPON | FLAG_MELEE );
	g_hItemFlagMap.SetValue("weapon_chainsaw"		, FLAG_WEAPON | FLAG_MELEE | FLAG_CHAINSAW);
	g_hItemFlagMap.SetValue("weapon_pistol"			, FLAG_WEAPON | FLAG_PISTOL );
	g_hItemFlagMap.SetValue("weapon_pistol_magnum"	, FLAG_WEAPON | FLAG_PISTOL_EXTRA );
	g_hItemFlagMap.SetValue("weapon_smg"			, FLAG_WEAPON | FLAG_SMG | FLAG_TIER1 );
	g_hItemFlagMap.SetValue("weapon_smg_silenced"	, FLAG_WEAPON | FLAG_SMG | FLAG_TIER1 );
	g_hItemFlagMap.SetValue("weapon_smg_mp5"		, FLAG_WEAPON | FLAG_SMG | FLAG_TIER1 | FLAG_CSS );
	g_hItemFlagMap.SetValue("weapon_pumpshotgun"	, FLAG_WEAPON | FLAG_SHOTGUN | FLAG_TIER1 );
	g_hItemFlagMap.SetValue("weapon_shotgun_chrome"	, FLAG_WEAPON | FLAG_SHOTGUN | FLAG_TIER1 );
	g_hItemFlagMap.SetValue("weapon_autoshotgun"	, FLAG_WEAPON | FLAG_SHOTGUN | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_shotgun_spas"	, FLAG_WEAPON | FLAG_SHOTGUN | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_rifle"			, FLAG_WEAPON | FLAG_ASSAULT | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_rifle_ak47"		, FLAG_WEAPON | FLAG_ASSAULT | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_rifle_desert"	, FLAG_WEAPON | FLAG_ASSAULT | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_rifle_sg552"	, FLAG_WEAPON | FLAG_ASSAULT | FLAG_TIER2 | FLAG_CSS );
	g_hItemFlagMap.SetValue("weapon_hunting_rifle"	, FLAG_WEAPON | FLAG_SNIPER | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_sniper_military", FLAG_WEAPON | FLAG_SNIPER | FLAG_TIER2 );
	g_hItemFlagMap.SetValue("weapon_sniper_scout"	, FLAG_WEAPON | FLAG_SNIPER | FLAG_TIER2 | FLAG_CSS );
	g_hItemFlagMap.SetValue("weapon_sniper_awp"		, FLAG_WEAPON | FLAG_SNIPER | FLAG_TIER2 | FLAG_CSS );
	g_hItemFlagMap.SetValue("weapon_first_aid_kit"	, FLAG_ITEM | FLAG_HEAL | FLAG_MEDKIT);
	g_hItemFlagMap.SetValue("weapon_defibrillator"	, FLAG_ITEM | FLAG_HEAL | FLAG_DEFIB);
	g_hItemFlagMap.SetValue("weapon_pain_pills"		, FLAG_ITEM | FLAG_HEAL );
	g_hItemFlagMap.SetValue("weapon_adrenaline"		, FLAG_ITEM | FLAG_HEAL );
	g_hItemFlagMap.SetValue("weapon_molotov"		, FLAG_WEAPON | FLAG_GREN );
	g_hItemFlagMap.SetValue("weapon_pipe_bomb"		, FLAG_WEAPON | FLAG_GREN );
	g_hItemFlagMap.SetValue("weapon_vomitjar"		, FLAG_WEAPON | FLAG_GREN );
	g_hItemFlagMap.SetValue("weapon_gascan"			, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_propanetank"	, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_oxygentank"		, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_gnome"			, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_cola_bottles"	, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_fireworkcrate"	, FLAG_ITEM | FLAG_CARRY );
	g_hItemFlagMap.SetValue("weapon_grenade_launcher", FLAG_WEAPON | FLAG_GL | FLAG_TIER3 );
	g_hItemFlagMap.SetValue("weapon_rifle_m60"		, FLAG_WEAPON | FLAG_M60 | FLAG_TIER3 );
	g_hItemFlagMap.SetValue("weapon_upgradepack_incendiary"	, FLAG_ITEM | FLAG_UPGRADE );
	g_hItemFlagMap.SetValue("weapon_upgradepack_explosive"	, FLAG_ITEM | FLAG_UPGRADE );
	g_hItemFlagMap.SetValue("weapon_ammo"			, FLAG_ITEM | FLAG_AMMO );
	g_hItemFlagMap.SetValue("weapon_ammo_pack"		, FLAG_ITEM | FLAG_AMMO | FLAG_UPGRADE );
	g_hItemFlagMap.SetValue("upgrade_item"			, FLAG_ITEM | FLAG_UPGRADE );
	g_bInitItemFlags = true;
}

void InitMeleeIDs()
{
	g_hMeleeIDs = CreateTrie();
	int iTable = FindStringTable("meleeweapons");
	
	if( iTable == INVALID_STRING_TABLE ) // Default to known IDs
	{
		PrintToServer("InitMeleeIDs: no meleeweapons table!!!");
		g_hMeleeIDs.SetValue("fireaxe",				0);
		g_hMeleeIDs.SetValue("frying_pan",			1);
		g_hMeleeIDs.SetValue("machete",				2);
		g_hMeleeIDs.SetValue("baseball_bat",		3);
		g_hMeleeIDs.SetValue("crowbar",				4);
		g_hMeleeIDs.SetValue("cricket_bat",			5);
		g_hMeleeIDs.SetValue("tonfa",				6);
		g_hMeleeIDs.SetValue("katana",				7);
		g_hMeleeIDs.SetValue("electric_guitar",		8);
		g_hMeleeIDs.SetValue("knife",				9);
		g_hMeleeIDs.SetValue("golfclub",			10);
		g_hMeleeIDs.SetValue("pitchfork",			11);
		g_hMeleeIDs.SetValue("shovel",				12);
	} else {
		// Get actual IDs
		int iNum = GetStringTableNumStrings(iTable);
		char sName[PLATFORM_MAX_PATH];

		for( int i = 0; i < iNum; i++ )
		{
			ReadStringTable(iTable, i, sName, sizeof(sName));
			//PrintToServer("InitMeleeIDs: %s id %d", sName, i);
			g_hMeleeIDs.SetValue(sName, i);
		}
	}
	g_hMeleeIDs.SetValue("chainsaw", 16);
	
	g_bInitMeleeIDs = true;
}

void InitMeleePrefs()
{
	static char szOutputBuffer[32];
	static int iMeleeID, iValue;
	
	for (int i = 0; i < 16; i++)
		g_iMeleePreference[i] = -1;
	
	Handle hKeys = CreateTrieSnapshot(g_hMeleePref);
	int size = GetTrieSize(g_hMeleePref);
	//PrintToServer("InitMeleePrefs: g_hMeleePref size %d", size);
	for (int i = 0; i < size; i++)
	{
		GetTrieSnapshotKey(hKeys, i, szOutputBuffer, sizeof(szOutputBuffer));
		if (g_hMeleeIDs.GetValue(szOutputBuffer, iMeleeID))
		{
			g_hMeleePref.GetValue(szOutputBuffer, iValue);
			//PrintToServer("%d %s %d", iMeleeID, szOutputBuffer, iValue);
			g_iMeleePreference[iMeleeID] = iValue;
		}
	}
	delete hKeys;
	g_bInitMeleePrefs = true;
}

void InitCheckCases()
{
	g_hCheckCases = CreateTrie();
	g_hCheckCases.SetValue("witch", 1 );
	g_hCheckCases.SetValue("weapon_ammo_spawn", 2 );
	g_hCheckCases.SetValue("upgrade_laser_sight", 3 );
	g_hCheckCases.SetValue("upgrade_ammo_explosive", 4 );
	g_hCheckCases.SetValue("upgrade_ammo_incendiary", 4 );
	g_bInitCheckCases = true;
}

void InitMaxAmmo()
{
	int iMaxAmmo, iAmmoOverride[56];
	char sArgs[16][8], szOutputBuffer[2][4];
	L4D2WeaponId iWeaponID;
	
	if ( strlen(g_sCvar_Ammo_Type_Override) && ExplodeString(g_sCvar_Ammo_Type_Override, " ", sArgs, sizeof(sArgs), sizeof(sArgs[]), true) )
	{
		for (int i = 0; i < sizeof(sArgs); i++)
		{
			if (ExplodeString(sArgs[i], ":", szOutputBuffer, sizeof(szOutputBuffer), sizeof(szOutputBuffer[]), true) == 2)
				iAmmoOverride[StringToInt(szOutputBuffer[0])] = StringToInt(szOutputBuffer[1]);
		}
	}
	
	for (int i = 0; i < 56; i++) // L4D2WeaponId_MAX is 56
	{
		iWeaponID = view_as<L4D2WeaponId>(i);
		iMaxAmmo = -1;
		if (iAmmoOverride[i])
		{
			g_iMaxAmmo[i] = iAmmoOverride[i];
			//if (g_bCvar_Debug)
			//	PrintToServer("InitMaxAmmo: %s max ammo %d (override)", IBWeaponName[i], g_iMaxAmmo[i]);
			continue;
		}
		switch(iWeaponID)
		{
			case L4D2WeaponId_Pistol, L4D2WeaponId_PistolMagnum:
				iMaxAmmo = g_iCvar_MaxAmmo_Pistol;
			case L4D2WeaponId_Smg, L4D2WeaponId_SmgSilenced, L4D2WeaponId_SmgMP5:
				iMaxAmmo = g_iCvar_MaxAmmo_SMG;
			case L4D2WeaponId_Pumpshotgun, L4D2WeaponId_ShotgunChrome:
				iMaxAmmo = g_iCvar_MaxAmmo_Shotgun;
			case L4D2WeaponId_Autoshotgun, L4D2WeaponId_ShotgunSpas:
				iMaxAmmo = g_iCvar_MaxAmmo_AutoShotgun;
			case L4D2WeaponId_Rifle, L4D2WeaponId_RifleAK47, L4D2WeaponId_RifleDesert, L4D2WeaponId_RifleSG552:
				iMaxAmmo = g_iCvar_MaxAmmo_AssaultRifle;
			case L4D2WeaponId_HuntingRifle:
				iMaxAmmo = g_iCvar_MaxAmmo_HuntRifle;
			case L4D2WeaponId_SniperMilitary, L4D2WeaponId_SniperScout, L4D2WeaponId_SniperAWP:
				iMaxAmmo = g_iCvar_MaxAmmo_SniperRifle;
			case L4D2WeaponId_GrenadeLauncher:
				iMaxAmmo = g_iCvar_MaxAmmo_GrenLauncher;
			case L4D2WeaponId_RifleM60:
				iMaxAmmo = g_iCvar_MaxAmmo_M60;
			case L4D2WeaponId_FirstAidKit:
				iMaxAmmo = g_iCvar_MaxAmmo_Medkit;
			case L4D2WeaponId_Adrenaline:
				iMaxAmmo = g_iCvar_MaxAmmo_Adrenaline;
			case L4D2WeaponId_PainPills:
				iMaxAmmo = g_iCvar_MaxAmmo_PainPills;
			case L4D2WeaponId_FragAmmo, L4D2WeaponId_IncendiaryAmmo:
				iMaxAmmo = g_iCvar_MaxAmmo_AmmoPack;
			case L4D2WeaponId_Chainsaw:
				iMaxAmmo = g_iCvar_MaxAmmo_Chainsaw;
			case L4D2WeaponId_PipeBomb:
				iMaxAmmo = g_iCvar_MaxAmmo_PipeBomb;
			case L4D2WeaponId_Molotov:
				iMaxAmmo = g_iCvar_MaxAmmo_Molotov;
			case L4D2WeaponId_Vomitjar:
				iMaxAmmo = g_iCvar_MaxAmmo_VomitJar;
		}
		g_iMaxAmmo[i] = iMaxAmmo;
		//if (g_bCvar_Debug && iMaxAmmo > -1)
		//	PrintToServer("InitMaxAmmo: %s max ammo %d", IBWeaponName[i], g_iMaxAmmo[i]);
	}
	
	for (int i = 0; i > MAXENTITIES; i++)
	{
		g_iWeapon_MaxAmmo[i] = g_iMaxAmmo[g_iWeaponID[i]];
	}
	g_bInitMaxAmmo = true;
}

void InitWeaponAndTierMap()
{
	//g_iWeaponTier[L4D2WeaponId_None] = -1;
	//g_iWeaponTier[L4D2WeaponId_Pistol] = 0;
	//g_iWeaponTier[L4D2WeaponId_Smg] = 1;
	//g_iWeaponTier[L4D2WeaponId_Pumpshotgun] = 1;
	//g_iWeaponTier[L4D2WeaponId_Autoshotgun] = 2;
	//g_iWeaponTier[L4D2WeaponId_Rifle] = 2;
	//g_iWeaponTier[L4D2WeaponId_HuntingRifle] = 2;
	//g_iWeaponTier[L4D2WeaponId_SmgSilenced] = 1;
	//g_iWeaponTier[L4D2WeaponId_ShotgunChrome] = 1;
	//g_iWeaponTier[L4D2WeaponId_RifleDesert] = 2;
	//g_iWeaponTier[L4D2WeaponId_SniperMilitary] = 2;
	//g_iWeaponTier[L4D2WeaponId_ShotgunSpas] = 2;
	//g_iWeaponTier[L4D2WeaponId_FirstAidKit] = 0;
	//g_iWeaponTier[L4D2WeaponId_Molotov] = 0;
	//g_iWeaponTier[L4D2WeaponId_PipeBomb] = 0;
	//g_iWeaponTier[L4D2WeaponId_PainPills] = 0;
	//g_iWeaponTier[L4D2WeaponId_Gascan] = 0;
	//g_iWeaponTier[L4D2WeaponId_PropaneTank] = 0;
	//g_iWeaponTier[L4D2WeaponId_OxygenTank] = 0;
	//g_iWeaponTier[L4D2WeaponId_Melee] = 0;
	//g_iWeaponTier[L4D2WeaponId_Chainsaw] = 0;
	g_iWeaponTier[L4D2WeaponId_GrenadeLauncher] = 3;
	g_iWeaponTier[L4D2WeaponId_AmmoPack] = -1;
	//g_iWeaponTier[L4D2WeaponId_Adrenaline] = 0;
	//g_iWeaponTier[L4D2WeaponId_Defibrillator] = 0;
	//g_iWeaponTier[L4D2WeaponId_Vomitjar] = 0;
	//g_iWeaponTier[L4D2WeaponId_RifleAK47] = 0;
	//g_iWeaponTier[L4D2WeaponId_GnomeChompski] = 0;
	//g_iWeaponTier[L4D2WeaponId_ColaBottles] = 0;
	//g_iWeaponTier[L4D2WeaponId_FireworksBox] = 0;
	//g_iWeaponTier[L4D2WeaponId_IncendiaryAmmo] = 0;
	//g_iWeaponTier[L4D2WeaponId_FragAmmo] = 0;
	//g_iWeaponTier[L4D2WeaponId_PistolMagnum] = 0;
	//g_iWeaponTier[L4D2WeaponId_SmgMP5] = 1;
	//g_iWeaponTier[L4D2WeaponId_RifleSG552] = 2;
	//g_iWeaponTier[L4D2WeaponId_SniperAWP] = 2;
	//g_iWeaponTier[L4D2WeaponId_SniperScout] = 2;
	g_iWeaponTier[L4D2WeaponId_RifleM60] = 3;
	g_iWeaponTier[L4D2WeaponId_Machinegun] = -1;
	g_iWeaponTier[L4D2WeaponId_FatalVomit] = -1;
	g_iWeaponTier[L4D2WeaponId_ExplodingSplat] = -1;
	g_iWeaponTier[L4D2WeaponId_LungePounce] = -1;
	g_iWeaponTier[L4D2WeaponId_Lounge] = -1;
	g_iWeaponTier[L4D2WeaponId_FullPull] = -1;
	g_iWeaponTier[L4D2WeaponId_Choke] = -1;
	g_iWeaponTier[L4D2WeaponId_ThrowingRock] = -1;
	g_iWeaponTier[L4D2WeaponId_TurboPhysics] = -1;
	g_iWeaponTier[L4D2WeaponId_Ammo] = -1;
	g_iWeaponTier[L4D2WeaponId_UpgradeItem] = -1;
	
	g_bIsSemiAuto[L4D2WeaponId_Pistol] = true;
	g_bIsSemiAuto[L4D2WeaponId_PistolMagnum] = true;
	g_bIsSemiAuto[L4D2WeaponId_Pumpshotgun] = true;
	g_bIsSemiAuto[L4D2WeaponId_ShotgunChrome] = true;
	g_bIsSemiAuto[L4D2WeaponId_Autoshotgun] = true;
	g_bIsSemiAuto[L4D2WeaponId_ShotgunSpas] = true;
	g_bIsSemiAuto[L4D2WeaponId_HuntingRifle] = true;
	g_bIsSemiAuto[L4D2WeaponId_SniperMilitary] = true;
	g_bIsSemiAuto[L4D2WeaponId_SniperScout] = true;
	g_bIsSemiAuto[L4D2WeaponId_SniperAWP] = true;
	g_bIsSemiAuto[L4D2WeaponId_GrenadeLauncher] = true;
	g_bIsSemiAuto[L4D2WeaponId_PainPills] = true;
	g_bIsSemiAuto[L4D2WeaponId_Adrenaline] = true;
	g_bIsSemiAuto[L4D2WeaponId_PipeBomb] = true;
	g_bIsSemiAuto[L4D2WeaponId_Molotov] = true;
	g_bIsSemiAuto[L4D2WeaponId_Vomitjar] = true;
	
	g_hWeaponMap = CreateTrie();
	
	for (int i = 0; i < 56; i++) // L4D2WeaponId_MAX is 56
		g_hWeaponMap.SetValue(IBWeaponName[i], true);
	
	if (L4D_HasMapStarted())
		UpdateWeaponTiers();
	else
		RequestFrame(UpdateWeaponTiers);
	
	g_bInitWeaponMap = true;
}

void UpdateWeaponTiers()
{
	for (int i = 0; i < 38; i++) // 37 L4D2WeaponId_RifleM60
	{
		if(g_iWeaponTier[i] != 3 && g_iWeaponTier[i] != -1)
		{
			g_iWeaponTier[i] = L4D2_GetIntWeaponAttribute(IBWeaponName[i], L4D2IWA_Tier);
			//PrintToServer("UpdateWeaponTiers: %d %s %d, mapstart %b", i, IBWeaponName[i], g_iWeaponTier[i], L4D_HasMapStarted());
		}
	}
}

void GetEntityModelname(int iEntity, char[] sModelName, int iMaxLength)
{
	GetEntPropString(iEntity, Prop_Data, "m_ModelName", sModelName, iMaxLength);
}

float GetClientDistance(int iClient, int iTarget, bool bSquared = false)
{
	return GetVectorDistance(g_fClientAbsOrigin[iClient], g_fClientAbsOrigin[iTarget], bSquared);
}

float GetEntityDistance(int iEntity, int iTarget, bool bSquared = false)
{
	float fEntityPos[3]; GetEntityAbsOrigin(iEntity, fEntityPos);
	float fTargetPos[3]; GetEntityAbsOrigin(iTarget, fTargetPos);
	return (GetVectorDistance(fEntityPos, fTargetPos, bSquared));
}

bool IsValidVector(const float fVector[3])
{
	int iCheck;
	for (int i = 0; i < 3; ++i)
	{
		if (fVector[i] != 0.0000)break;
		++iCheck;
	}
	return view_as<bool>(iCheck != 3);
}

int GetEntityHealth(int iEntity)
{
	return (GetEntProp(iEntity, Prop_Data, "m_iHealth"));
}

int GetEntityMaxHealth(int iEntity)
{
	return (GetEntProp(iEntity, Prop_Data, "m_iMaxHealth"));
}

float GetWeaponCycleTime(int iWeapon)
{
	static char sWeaponName[64];
	GetWeaponClassname(iWeapon, sWeaponName, sizeof(sWeaponName));
	if (!L4D2_IsValidWeapon(sWeaponName))return -1.0;
	return L4D2_GetFloatWeaponAttribute(sWeaponName, L4D2FWA_CycleTime);
}

//Get max ammo for entity depending on weapon ID
//
//When this is called, we assume that weapon id is known, and available in g_iWeaponID
//
//Instead of figuring out weapon name or ammo type from prop data, we use "lookup table", that's initiated once per round at worst
//
int GetWeaponMaxAmmo(int iWeapon)
{
	if (!g_bInitMaxAmmo)
	{
		InitMaxAmmo();
		PrintToServer("GetWeaponMaxAmmo: InitMaxAmmo");
	}
	
	return g_iMaxAmmo[g_iWeaponID[iWeapon]];
}

int GetWeaponTier(int iWeapon)
{
	return g_iWeaponTier[g_iWeaponID[iWeapon]];
}

bool IsSurvivorCarryingProp(int iClient)
{
	return (IsWeaponSlotActive(iClient, 5));
}

//	Get weapon classname from entity ID
//	Return 0 if weapon is not recognized
//	1 if it's weapon proper
//	-1 if the entity is not exactly a weapon (may be weapon_spawn)
int GetWeaponClassname(int iWeapon, char[] szOutputBuffer, int iMaxLength)
{
	//static char classname[64];
	//strcopy( classname, sizeof(classname), szOutputBuffer );
	
	if ( !GetEdictClassname(iWeapon, szOutputBuffer, iMaxLength) )
		return 0;
	
	if (!g_bInitWeaponMap)
	{
		InitWeaponAndTierMap();
		PrintToServer("GetWeaponClassname: InitWeaponAndTierMap");
	}
	if( g_hWeaponMap.ContainsKey(szOutputBuffer) ) // if it's a weapon name already, just get on with it
	{
		//if (g_bCvar_Debug)
		//PrintToServer("Classname %s exists as key, id %d", szOutputBuffer, iWeapon);
		
		return 1;
	}
	
	if (strcmp(szOutputBuffer, "weapon_spawn") == 0)
	{
		int iWeaponID = GetEntProp(iWeapon, Prop_Send, "m_weaponID");
		if (iWeaponID == 0)
		{
			PushEntityIntoArray(g_hWeaponsToCheckLater, iWeapon);
			if (g_hCheckWeaponTimer == INVALID_HANDLE)
				g_hCheckWeaponTimer = CreateTimer(0.1, CheckWeaponsEvenLater);
			return 0;
		}
		
		strcopy( szOutputBuffer, iMaxLength, IBWeaponName[iWeaponID] );
		
		//if (g_bCvar_Debug)
		//PrintToServer("Got weapon name %s from IBWeaponName for id %d classname %s", IBWeaponName[iWeaponID], iWeapon, classname);
		
		return -1;
	}
	
	for (int i = strlen(szOutputBuffer); i > 0; --i)
    {    
        if(szOutputBuffer[i] == '_')
        {
            szOutputBuffer[i] = EOS;
            break;
        }
    }
	if( g_hWeaponMap.ContainsKey(szOutputBuffer) )
	{
		//if (g_bCvar_Debug)
		//PrintToServer("Got weapon name %s from WeaponMap for id %d classname %s", szOutputBuffer, iWeapon, classname);
		
		return -1;
	}
	
	if(g_iCvar_ItemScavenge_Models)
	{
		static char sWeaponModel[PLATFORM_MAX_PATH];
		GetEntityModelname(iWeapon, sWeaponModel, sizeof(sWeaponModel));
		
		if( g_hWeaponMdlMap.GetString(sWeaponModel, szOutputBuffer, iMaxLength) )
		{
			//if (g_bCvar_Debug)
			//	PrintToServer("Judged weapon class %s by model from id %d classname %s!", szOutputBuffer, iWeapon, classname);
			
			return -1;
		}
	}
	
	//if (g_bCvar_Debug)
	//	PrintToServer("Could not recognize weapon from entity %d %s! buffer %s model %s", iWeapon, classname, szOutputBuffer, sWeaponModel);
	
	return 0;
}

//	Determine melee ID ""once"" on entity creation
//	scratch that; volvo gotta mangle entities across multiple frames
int GetMeleeID(int iEntity, bool bRechecking = false)
{
	static char sModelName[64], sEntClassname[64], szOutputBuffer[32];
	static int iMeleeID;
	
	iMeleeID = -1;
	szOutputBuffer[0] = EOS;
	
	GetEdictClassname(iEntity, sEntClassname, sizeof(sEntClassname));
	if( bRechecking && strncmp(sEntClassname, "weapon_melee", 12) != 0 )
	{
		if(g_iCvar_Debug & DEBUG_WEP_DATA)
			PrintToServer("GetMeleeID: ent %d %s is no more a melee weapon", iEntity, sEntClassname);
		return -1;
	}
	GetEntPropString(iEntity, Prop_Data, "m_ModelName", sModelName, sizeof(sModelName));
	if(sModelName[0] == EOS)
	{
		if(g_iCvar_Debug & DEBUG_WEP_DATA)
			PrintToServer("GetMeleeID: ent %d %s got no model%s", iEntity, sEntClassname, bRechecking ? " when rechecking!" : "");
		RequestFrame(RecheckMelee, iEntity);
		return -1;
	}
	
	g_hMeleeMdlToID.GetString(sModelName, szOutputBuffer, sizeof(szOutputBuffer));
	g_hMeleeIDs.GetValue(szOutputBuffer, iMeleeID);
	
	if(g_iCvar_Debug & DEBUG_WEP_DATA)
		PrintToServer("GetMeleeID: model %s\nent %d melee #%d %s %s", sModelName, iEntity, iMeleeID, szOutputBuffer, bRechecking ? "(rechecked)" : "");
	
	return iMeleeID;
}

void RecheckMelee(int iEntity)
{
	if(IsValidEdict(iEntity))
		g_iMeleeID[iEntity] = GetMeleeID(iEntity, true);
}

int GetMeleePreference(int iEntity)
{
	static int iMeleeID;
	
	if(!g_bInitMeleePrefs) InitMeleePrefs();
	iMeleeID = g_iMeleeID[iEntity];
	
	return (iMeleeID != -1) ? g_iMeleePreference[iMeleeID] : 0;
}

int GetSurvivorType(int iClient)
{
	static char sModelname[PLATFORM_MAX_PATH]; GetClientModel(iClient, sModelname, sizeof(sModelname));
	switch(sModelname[29])
	{
		case 'b': 	return L4D_SURVIVOR_NICK;
		case 'd': 	return L4D_SURVIVOR_ROCHELLE;
		case 'c': 	return L4D_SURVIVOR_COACH;
		case 'h': 	return L4D_SURVIVOR_ELLIS;
		case 'v': 	return L4D_SURVIVOR_BILL;
		case 'n': 	return L4D_SURVIVOR_ZOEY;
		case 'e': 	return L4D_SURVIVOR_FRANCIS;
		case 'a': 	return L4D_SURVIVOR_LOUIS;
		default:	return 0;
	}
}

bool L4D_IsValidEnt(int iEntity)
{
	return (iEntity > 0 && (iEntity <= MAXENTITIES && IsValidEdict(iEntity) || iEntity > MAXENTITIES && IsValidEntity(iEntity)));
}

bool IsCommonInfected(int iEntity)
{
	static char sEntClass[64];
	GetEntityClassname(iEntity, sEntClass, sizeof(sEntClass));

	return (strcmp(sEntClass, "infected") == 0);
}

bool IsCommonAttacking(int iEntity)
{
	return (GetEntProp(iEntity, Prop_Send, "m_mobRush") == 1 || GetEntProp(iEntity, Prop_Send, "m_clientLookatTarget") != -1);
}

bool IsCommonAlive(int iEntity)
{
	return (GetEntProp(iEntity, Prop_Data, "m_lifeState") == 0 && GetEntProp(iEntity, Prop_Send, "m_bIsBurning") == 0);
}

bool IsCommonStumbling(int iEntity)
{
	if (!g_bExtensionActions)return false;
	return (ActionsManager.GetAction(iEntity, "InfectedShoved") != INVALID_ACTION);
}

int GetFarthestInfected(int iClient, float fDistance = -1.0)
{
	int iInfected = 0;

	float fInfectedDist; 
	float fInfectedPos[3];
	float fLastDist = -1.0;
	
	int i = INVALID_ENT_REFERENCE;
	while ((i = FindEntityByClassname(i, "infected")) != INVALID_ENT_REFERENCE)
	{
		if (!IsCommonAlive(i))
			continue;
		
		GetEntityCenteroid(i, fInfectedPos);
		fInfectedDist = GetVectorDistance(g_fClientEyePos[iClient], fInfectedPos, true);
		if (fDistance > 0.0 && fInfectedDist > (fDistance*fDistance) || fLastDist != -1.0 && fInfectedDist <= fLastDist || !IsVisibleVector(iClient, fInfectedPos))
			continue;

		iInfected = i;
		fLastDist = fInfectedDist;
	}

	return iInfected;
}

// ----------------------------------------------------------------------------------------------------

void CheckEntityForVisibility(int iClient, int iEntity, bool bFOVOnly = false, float fOverridePos[3] = NULL_VECTOR)
{
	float fCheckPos[3];
	if (IsNullVector(fOverridePos))
		GetEntityCenteroid(iEntity, fCheckPos);
	else 
		fCheckPos = fOverridePos;
	
	if (bFOVOnly && !FVectorInViewCone(iClient, fCheckPos))
		return;

	float fNoticeTime;
	bool bIsVisible = IsVisibleEntity(iClient, iEntity);
	float fEntityDist = GetVectorDistance(g_fClientEyePos[iClient], fCheckPos, true);
	float fDot = RadToDeg(ArcCosine(GetLineOfSightDotProduct(iClient, fCheckPos)));

	// -------------------------

	// static char szTargetName[MAX_NAME_LENGTH];
	// if (1 <= iEntity <= MaxClients)
	// {
	// 	FormatEx(szTargetName, sizeof(szTargetName), "%N (%i)", iEntity, iEntity);
	// }
	// else
	// {
	// 	GetEntityClassname(iEntity, szTargetName, sizeof(szTargetName));
	// 	FormatEx(szTargetName, sizeof(szTargetName), "%s (%i)", szTargetName, iEntity);
	// }

	// PrintToServer("%N -> %s:\nFOV = %i (%.1f)\nNo FOV = %i (%.1f)",
	// 	iClient, szTargetName,
	// 	g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity], g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_FOV][iEntity],
	// 	g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity], g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_NOFOV][iEntity]
	// );

	// -------------------------

	int iVisionState;
	float fVisionTime = g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_FOV][iEntity];

	if (GetGameTime() >= fVisionTime)
	{
		fNoticeTime = ClampFloat(0.5 * (fDot / g_fCvar_Vision_FieldOfView) + (fEntityDist / VISION_CHECK_MAXDIST), 0.15, 0.75);
		fNoticeTime *= g_fCvar_Vision_NoticeTimeScale;

		iVisionState = g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity];
		switch(iVisionState)
		{
			case VISION_STATE_NOCONTACT:
			{
				if (bIsVisible)
				{
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity] = 1;
					g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_FOV][iEntity] = (GetGameTime() + fNoticeTime);
				}
			}
			case VISION_STATE_NOTICED:
			{
				g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity] = (bIsVisible ? VISION_STATE_SPOTTED : VISION_STATE_NOCONTACT);
			}
			case VISION_STATE_SPOTTED:
			{
				if (!bIsVisible)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity] = VISION_STATE_LOSTSIGHT;
				else
					g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_FOV][iEntity] = (GetGameTime() + fNoticeTime);

			}
			case VISION_STATE_LOSTSIGHT:
			{
				if (bIsVisible)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity] = VISION_STATE_SPOTTED;
				else if ((GetGameTime() - fVisionTime) >= VISION_CHECK_LOSETIME)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity] = VISION_STATE_NOCONTACT;
			}
		}
	}

	// -------------------------

	fVisionTime = g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_NOFOV][iEntity];

	if (!bFOVOnly && GetGameTime() >= fVisionTime)
	{
		fNoticeTime = ClampFloat(0.75 * (fDot / 165.0) + (fEntityDist / VISION_CHECK_MAXDIST), 0.15, 1.5);
		fNoticeTime *= g_fCvar_Vision_NoticeTimeScale;

		iVisionState = g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity];
		switch(iVisionState)
		{
			case VISION_STATE_NOCONTACT:
			{
				if (bIsVisible)
				{
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_NOTICED;
					g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_NOFOV][iEntity] = (GetGameTime() + fNoticeTime);
				}
			}
			case VISION_STATE_NOTICED:
			{
				if (bIsVisible)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_SPOTTED;
				else
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_NOCONTACT;
			}
			case VISION_STATE_SPOTTED:
			{
				if (!bIsVisible)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_LOSTSIGHT;
				else
					g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_NOFOV][iEntity] = (GetGameTime() + ClampFloat(fNoticeTime * 0.33, 0.1, fNoticeTime));

			}
			case VISION_STATE_LOSTSIGHT:
			{
				if (bIsVisible)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_SPOTTED;
				else if ((GetGameTime() - fVisionTime) >= VISION_CHECK_LOSETIME)
					g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity] = VISION_STATE_NOCONTACT;
			}
		}
	}

	// -------------------------

	// PrintToServer("%N -> %s:\nFOV = %i (%.1f)\nNo FOV = %i (%.1f)\n",
	// 	iClient, szTargetName,
	// 	g_iBot_VisionMemory_State[iClient][VISION_MEMORY_FOV][iEntity], g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_FOV][iEntity],
	// 	g_iBot_VisionMemory_State[iClient][VISION_MEMORY_NOFOV][iEntity], g_fBot_VisionMemory_Time[iClient][VISION_MEMORY_NOFOV][iEntity]
	// );
}

bool HasVisualContactWithEntity(int iClient, int iEntity, bool bFOVState = true, float fOverridePos[3] = NULL_VECTOR)
{
	CheckEntityForVisibility(iClient, iEntity, bFOVState, fOverridePos);	
	return (g_iBot_VisionMemory_State[iClient][bFOVState ? VISION_MEMORY_FOV : VISION_MEMORY_NOFOV][iEntity] == VISION_STATE_SPOTTED);
}

// ----------------------------------------------------------------------------------------------------

stock float ClampFloat(float fValue, float fMin, float fMax)
{
	return (fValue > fMax) ? fMax : ((fValue < fMin) ? fMin : fValue);
}

stock int GetClosestInfected(int iClient, float fDistance = -1.0)
{
	static int iInfected, iCloseInfected, iThrownPipeBomb;
	static float fInfectedDist, fLastDist;
	static bool bClientIsAttacking, bIsChasingSomething, bBileWasThrown;

	bIsChasingSomething = false;
	iThrownPipeBomb = (FindEntityByClassname(-1, "pipe_bomb_projectile"));
	bBileWasThrown = (FindEntityByClassname(-1, "info_goal_infected_chase") != -1);
	iCloseInfected = 0;
	fLastDist = -1.0;
	iInfected = INVALID_ENT_REFERENCE;
	while ((iInfected = FindEntityByClassname(iInfected, "infected")) != INVALID_ENT_REFERENCE)
	{
		if (!IsCommonAlive(iInfected))
			continue;

		fInfectedDist = GetEntityDistance(iClient, iInfected, true);
		if (fDistance > 0.0 && fInfectedDist > (fDistance*fDistance) || fLastDist != -1.0 && fInfectedDist >= fLastDist)
			continue;

		bIsChasingSomething = (fInfectedDist > 25600.0 && (bBileWasThrown || iThrownPipeBomb > 0 && GetEntityDistance(iInfected, iThrownPipeBomb, true) <= 65536.0));
		if (!bIsChasingSomething && fInfectedDist > 9216.0)
		{
			for (int i = 1; i <= MaxClients; i++)
			{
				bIsChasingSomething = (iClient != i && IsClientSurvivor(i) && g_iBot_TargetInfected[i] == iInfected && IsWeaponSlotActive(i, 1) && g_iBot_TargetInfected[i] && SurvivorHasMeleeWeapon(i) != 0);
				if (bIsChasingSomething)break;
			}
		}

		if (bIsChasingSomething || !IsVisibleEntity(iClient, iInfected))
			continue;

		iCloseInfected = iInfected;
		fLastDist = fInfectedDist;
	}

	bClientIsAttacking = false;
	for (iInfected = 1; iInfected <= MaxClients; iInfected++)
	{
		if (!IsSpecialInfected(iInfected) || L4D2_GetPlayerZombieClass(iInfected) == L4D2ZombieClass_Tank || bClientIsAttacking && !IsUsingSpecialAbility(iInfected))
			continue;

		fInfectedDist = GetClientDistance(iClient, iInfected, true);
		if (fDistance > 0.0 && fInfectedDist > (fDistance*fDistance) || fLastDist != -1.0 && fInfectedDist >= fLastDist || !IsVisibleEntity(iClient, iInfected, MASK_VISIBLE_AND_NPCS))
			continue;

		iCloseInfected = iInfected;
		fLastDist = fInfectedDist;
		bClientIsAttacking = IsUsingSpecialAbility(iInfected);
	}

	return iCloseInfected;
}

int GetInfectedCount(int iClient, float fDistanceLimit = -1.0, int iMaxLimit = -1, bool bVisible = true, bool bAttackingOnly = true)
{
	static int i, iCount;
	static float fClientPos[3], fInfectedPos[3];
	GetEntityCenteroid(iClient, fClientPos);
	
	i = INVALID_ENT_REFERENCE;
	iCount = 0;
	while ((i = FindEntityByClassname(i, "infected")) != INVALID_ENT_REFERENCE)
	{
		if (!IsCommonAlive(i) || bAttackingOnly && !IsCommonAttacking(i))
			continue;

		GetEntityCenteroid(i, fInfectedPos);
		if (fDistanceLimit > 0.0 && GetVectorDistance(fClientPos, fInfectedPos, true) > (fDistanceLimit*fDistanceLimit) || bVisible && (IsValidClient(iClient) && !IsVisibleVector(iClient, fInfectedPos, MASK_VISIBLE_AND_NPCS) || !GetVectorVisible(fClientPos, fInfectedPos)))
			continue;

		iCount++;
		if (iMaxLimit > 0 && iCount >= iMaxLimit)return iCount;
	}

	return iCount;
}

bool IsSpecialInfected(int iClient)
{
	return (IsValidClient(iClient) && GetClientTeam(iClient) == 3 && IsPlayerAlive(iClient) && !L4D_IsPlayerGhost(iClient));
}

bool IsWitch(int iEntity)
{
	static char sClass[12]; GetEntityClassname(iEntity, sClass, sizeof(sClass));
	return (strcmp(sClass, "witch") == 0);
}

bool IsSurvivorBusy(int iClient, bool bHoldingGrenade = false, bool bHoldingMedkit = false, bool bHoldingPills = false)
{
	if (bHoldingGrenade && IsWeaponSlotActive(iClient, 2) || bHoldingMedkit && IsWeaponSlotActive(iClient, 3) || bHoldingPills && IsWeaponSlotActive(iClient, 4))
		return true;

	L4D2UseAction iUseAction = L4D2_GetPlayerUseAction(iClient);
	if (iUseAction != L4D2UseAction_None && (bHoldingMedkit || iUseAction != L4D2UseAction_Healing && iUseAction != L4D2UseAction_Defibing))
		return true;

	if (L4D_GetPlayerReviveTarget(iClient) > 0)
		return true;

	if (L4D_IsPlayerStaggering(iClient))
		return true;

	return false;
}

bool IsVisibleVector(int iClient, float fPos[3], int iMask = MASK_SHOT)
{
	if (IsFakeClient(iClient) && GetClientTeam(iClient) == 2 && IsSurvivorBotBlindedByVomit(iClient))
		return false;

	Handle hResult = TR_TraceRayFilterEx(g_fClientEyePos[iClient], fPos, iMask, RayType_EndPoint, Base_TraceFilter);
	float fFraction = TR_GetFraction(hResult); delete hResult;
	return (fFraction == 1.0);
}

bool IsVisibleEntity(int iClient, int iTarget, int iMask = MASK_SHOT)
{
	if (IsFakeClient(iClient) && GetClientTeam(iClient) == 2 && IsSurvivorBotBlindedByVomit(iClient))
		return false;

	float fTargetPos[3];
	GetEntityAbsOrigin(iTarget, fTargetPos);

	Handle hResult = TR_TraceRayFilterEx(g_fClientEyePos[iClient], fTargetPos, iMask, RayType_EndPoint, Base_TraceFilter, iTarget);
	bool bDidHit = (TR_GetFraction(hResult) == 1.0 && !TR_StartSolid(hResult) || TR_GetEntityIndex(hResult) == iTarget); delete hResult;
	if (!bDidHit)
	{
		float fViewOffset[3]; 
		GetEntPropVector(iTarget, Prop_Data, "m_vecViewOffset", fViewOffset);
		AddVectors(fTargetPos, fViewOffset, fTargetPos);

		hResult = TR_TraceRayFilterEx(g_fClientEyePos[iClient], fTargetPos, iMask, RayType_EndPoint, Base_TraceFilter, iTarget);
		bDidHit = (TR_GetFraction(hResult) == 1.0 && !TR_StartSolid(hResult) || TR_GetEntityIndex(hResult) == iTarget); delete hResult;
		if (!bDidHit)
		{
			GetEntityCenteroid(iTarget, fTargetPos);
			
			hResult = TR_TraceRayFilterEx(g_fClientEyePos[iClient], fTargetPos, iMask, RayType_EndPoint, Base_TraceFilter, iTarget);
			bDidHit = (TR_GetFraction(hResult) == 1.0 && !TR_StartSolid(hResult) || TR_GetEntityIndex(hResult) == iTarget); delete hResult;
		}
	}
	return (bDidHit);
}

float GetLineOfSightDotProduct(int iClient, const float fVecSpot[3])
{
	float fLineOfSight[3]; 
	MakeVectorFromPoints(g_fClientEyePos[iClient], fVecSpot, fLineOfSight);
	NormalizeVector(fLineOfSight, fLineOfSight);

	float fEyeDirection[3];
	GetAngleVectors(g_fClientEyeAng[iClient], fEyeDirection, NULL_VECTOR, NULL_VECTOR);
	NormalizeVector(fEyeDirection, fEyeDirection);

	return GetVectorDotProduct(fLineOfSight, fEyeDirection);
}

bool FVectorInViewCone(int iClient, const float fVecSpot[3], float fCone = -1.0)
{
	if (fCone == -1.0)fCone = g_fCvar_Vision_FieldOfView;
	return (RadToDeg(ArcCosine(GetLineOfSightDotProduct(iClient, fVecSpot))) <= fCone);
}

float GetViewAnglesDotProduct(int iClient, const float fVecSpot[3])
{
	float fLineOfSight[3]; 
	MakeVectorFromPoints(g_fClientAbsOrigin[iClient], fVecSpot, fLineOfSight);
	fLineOfSight[2] = 0.0;
	NormalizeVector(fLineOfSight, fLineOfSight);

	float fDirection[3]; GetClientAbsAngles(iClient, fDirection);
	GetAngleVectors(fDirection, fDirection, NULL_VECTOR, NULL_VECTOR);
	fDirection[2] = 0.0;
	NormalizeVector(fDirection, fDirection);

	return GetVectorDotProduct(fLineOfSight, fDirection);
}

bool FVectorInViewAngle(int iClient, const float fVecSpot[3], float fAngle = -1.0)
{
	if (fAngle == -1.0)fAngle = g_fCvar_Vision_FieldOfView;
	return (RadToDeg(ArcCosine(GetViewAnglesDotProduct(iClient, fVecSpot))) <= fAngle);
}

bool FEntityInViewAngle(int iClient, int iEntity, float fAngle = -1.0)
{
	if (fAngle == -1.0)fAngle = g_fCvar_Vision_FieldOfView;
	float fEntityAbsOrigin[3]; GetEntityCenteroid(iEntity, fEntityAbsOrigin);
	return (RadToDeg(ArcCosine(GetViewAnglesDotProduct(iClient, fEntityAbsOrigin))) <= fAngle);
}

void SnapViewToPosition(int iClient, const float fPos[3])
{
	if (g_bClient_IsLookingAtPosition[iClient])
		return;
	
	float fDesiredDir[3];
	MakeVectorFromPoints(g_fClientEyePos[iClient], fPos, fDesiredDir);
	GetVectorAngles(fDesiredDir, fDesiredDir);

	float fEyeAngles[3];
	fEyeAngles[0] = (g_fClientEyeAng[iClient][0] + NormalizeAngle(fDesiredDir[0] - g_fClientEyeAng[iClient][0]));
	fEyeAngles[1] = (g_fClientEyeAng[iClient][1] + NormalizeAngle(fDesiredDir[1] - g_fClientEyeAng[iClient][1]));
	fEyeAngles[2] = 0.0;

	TeleportEntity(iClient, NULL_VECTOR, fEyeAngles, NULL_VECTOR);
	g_bClient_IsLookingAtPosition[iClient] = true;
}

float NormalizeAngle(float fAngle)
{
	fAngle = (fAngle - RoundToFloor(fAngle / 360.0) * 360.0);
	if (fAngle > 180.0)fAngle -= 360.0;
	else if (fAngle < -180.0)fAngle += 360.0;
	return fAngle;
}

void GetClientAimPosition(int iClient, float fAimPos[3])
{
	Handle hResult = TR_TraceRayFilterEx(g_fClientEyePos[iClient], g_fClientEyeAng[iClient], MASK_SHOT, RayType_Infinite, Base_TraceFilter);
	TR_GetEndPosition(fAimPos, hResult); delete hResult;
}

bool GetVectorVisible(float fStart[3], float fEnd[3], int iMask = MASK_VISIBLE_AND_NPCS)
{
	Handle hResult = TR_TraceRayFilterEx(fStart, fEnd, iMask, RayType_EndPoint, Base_TraceFilter);
	float fFraction = TR_GetFraction(hResult); delete hResult; return (fFraction == 1.0);
}

bool Base_TraceFilter(int iEntity, int iContentsMask, int iData)
{
	return (iEntity == iData || HasEntProp(iEntity, Prop_Data, "m_eDoorState") && L4D_GetDoorState(iEntity) != DOOR_STATE_OPENED);
}

void SwitchWeaponSlot(int iClient, int iSlot)
{
	int iWeapon = GetWeaponInInventory(iClient, iSlot);
	if (!iWeapon || L4D_GetPlayerCurrentWeapon(iClient) == iWeapon)
		return;

	static char sWeaponName[64]; GetEdictClassname(iWeapon, sWeaponName, sizeof(sWeaponName));
	FakeClientCommand(iClient, "use %s", sWeaponName);
}

bool IsWeaponSlotActive(int iClient, int iSlot)
{
	return (GetWeaponInInventory(iClient, iSlot) == L4D_GetPlayerCurrentWeapon(iClient));
}

bool SurvivorHasSMG(int iClient)
{
	return ( g_iClientInvFlags[iClient] & FLAG_SMG != 0 );
}

bool SurvivorHasAssaultRifle(int iClient)
{
	static int iSlot, iItemFlags;

	iSlot = GetWeaponInInventory(iClient, 0);
	if (!iSlot)return false;
	
	iItemFlags = g_iClientInvFlags[iClient];
	return (iItemFlags & FLAG_ASSAULT ? true : false);
}

int SurvivorHasShotgun(int iClient)
{
	return ((g_iClientInvFlags[iClient] & FLAG_SHOTGUN != 0) + (g_iClientInvFlags[iClient] & FLAG_SHOTGUN && g_iClientInvFlags[iClient] & FLAG_TIER2));
}

// Used to return int, which was unused
bool SurvivorHasSniperRifle(int iClient)
{
	return (g_iClientInvFlags[iClient] & FLAG_SNIPER != 0);
}

int SurvivorHasTier3Weapon(int iClient)
{
	return ((g_iClientInvFlags[iClient] & FLAG_TIER3 != 0) + (g_iClientInvFlags[iClient] & FLAG_M60 != 0));
}

int SurvivorHasGrenade(int iClient)
{
	int iSlot = GetWeaponInInventory(iClient, 2);
	if (!iSlot)return 0;

	static char sWepName[64]; 
	GetEdictClassname(iSlot, sWepName, sizeof(sWepName));

	switch(sWepName[7])
	{
		case 'p': return 1;
		case 'm': return 2;
		case 'v': return 3;
		default: return 0;
	}
}

//returns the same thing! :)
stock int SurvivorHasHealthKit(int iClient)
{
	return ((g_iClientInvFlags[iClient] >> 22 & 1) + (g_iClientInvFlags[iClient] >> 20 & 2) + (g_iClientInvFlags[iClient] >> 4 & 1) * 3);
}

int SurvivorHasMeleeWeapon(int iClient)
{
	return ((g_iClientInvFlags[iClient] >> 6 & 1) + (g_iClientInvFlags[iClient] >> 16 & 1));
}

int SurvivorHasPistol(int iClient)
{
	static int iSlot, iItemFlags;

	iSlot = GetWeaponInInventory(iClient, 1);
	if (!iSlot || SurvivorHasMeleeWeapon(iClient))
		return 0;

	iItemFlags = g_iClientInvFlags[iClient];
	if (iItemFlags & FLAG_PISTOL_EXTRA && !(iItemFlags & FLAG_PISTOL))
		return 3;
	else
		return ((GetEntProp(iSlot, Prop_Send, "m_isDualWielding") != 0 || GetEntProp(iSlot, Prop_Send, "m_hasDualWeapons") != 0) ? 2 : (iItemFlags & FLAG_PISTOL ? 1 : 0));
}

int GetTeamActiveItemCount(const L4D2WeaponId iWeaponID)
{
	int iCount, iWeaponSlot, iCurWeapon;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i))continue;
		iCurWeapon = L4D_GetPlayerCurrentWeapon(i);

		for (int j = 0; j < 5; j++)
		{
			iWeaponSlot = GetWeaponInInventory(i, j);
			if (iCurWeapon == iWeaponSlot && L4D2_GetWeaponId(iWeaponSlot) == iWeaponID)
			{
				iCount++; 
				break;
			}
		}
	}
	return iCount;
}

int GetSurvivorTeamItemCount(const L4D2WeaponId iWeaponID)
{
	static int iCount, iWeaponSlot;
	iCount = 0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i))
			continue;

		for (int j = 0; j < 5; j++)
		{
			iWeaponSlot = GetWeaponInInventory(i, j);
			if (iWeaponSlot && L4D2_GetWeaponId(iWeaponSlot) == iWeaponID)
			{
				iCount++; 
				break;
			}
		}
	}
	return iCount;
}

/*
	Use this whenever you need to count survivors with an item of certain category(inventory flag)
	e.g. an assault rifle, a melee weapon, a grenade etc
	
	Second argument ~ "AND"
	Sending multiple flags in one argument ~ "OR"
	Negative argument ~ "NOT", don't put multiple flags under one argument, 
	e.g.
	(FLAG_PISTOL | FLAG_PISTOL_EXTRA) will count survivors that carry either pistol(s) or Magnum
	(FLAG_SHOTGUN, FLAG_TIER1) – survivors with Tier 1 shotguns
	(-FLAG_PISTOL, FLAG_PISTOL_EXTRA) – survivors with Magnum specifically
*/
int GetSurvivorTeamInventoryCount(int iFlag, int iFlag2 = 0)
{
	static int iCount;
	static bool bNegate, bNegate2;
	
	bNegate = (iFlag < 0);
	bNegate2 = (iFlag2 < 0);
	
	iCount = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientSurvivor(i))
			continue;
		if ( (bNegate ? ~g_iClientInvFlags[i] & -iFlag : g_iClientInvFlags[i] & iFlag)
			&& (iFlag2 ? (bNegate2 ? ~g_iClientInvFlags[i] & -iFlag2 : g_iClientInvFlags[i] & iFlag2) : 1) )
		{
			iCount++;
		}
	}
	return iCount;
}

bool IsWeaponReloading(int iWeapon, bool bIgnoreShotguns = true)
{
	if (!L4D_IsValidEnt(iWeapon) || !HasEntProp(iWeapon, Prop_Data, "m_bInReload"))
		return false;

	bool bInReload = !!GetEntProp(iWeapon, Prop_Data, "m_bInReload");
	if (bInReload && bIgnoreShotguns)
	{
		static int iItemFlags;
		iItemFlags = g_iItemFlags[iWeapon];
		
		return !(iItemFlags & FLAG_SHOTGUN);
	}
	return (bInReload);
}

stock float GetWeaponNextFireTime(int iWeapon)
{
	return (GetEntPropFloat(iWeapon, Prop_Send, "m_flNextPrimaryAttack"));
}

stock int GetWeaponClip1(int iWeapon) 
{
	return (GetEntProp(iWeapon, Prop_Send, "m_iClip1"));
}

stock int GetWeaponClipSize(int iWeapon)
{
	return (SDKCall(g_hGetMaxClip1, iWeapon));
}

stock int GetTeamPlayerCount(int iTeam, bool bOnlyAlive=false, bool bOnlyBots=false)
{
	int iCount;
	int iTeamCount = GetTeamClientCount(iTeam);
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && (!bOnlyAlive || IsPlayerAlive(i)) && (!bOnlyBots || IsFakeClient(i)) && GetClientTeam(i) == iTeam)
		{
			iCount++;
			if (iCount >= iTeamCount)break;
		}
	}
	return iCount;
}

stock int IsFinaleEscapeVehicleArrived()
{
	return (L4D2_IsGenericCooperativeMode() && L4D_IsMissionFinalMap() && L4D2_GetCurrentFinaleStage() == 6);
}

stock int GetCurrentGameDifficulty()
{
	if (!L4D2_IsGenericCooperativeMode())return 2;
	switch(g_sCvar_GameDifficulty[0])
	{
		case 'E', 'e': return 1;
		case 'H', 'h': return 3;
		case 'I', 'i': return 4;
		default: return 2;
	}
}

stock int GetClientRealHealth(int iClient)
{
	return RoundFloat(GetClientHealth(iClient) + L4D_GetTempHealth(iClient));
}

bool IsSurvivorBotBlindedByVomit(int iClient)
{
	return (GetGameTime() < g_fBot_VomitBlindedTime[iClient]);
}

stock bool IsEntityOnFire(int iEntity)
{
	return (GetEntityFlags(iEntity) & FL_ONFIRE) != 0;
}

stock bool IsValidClient(int iClient) 
{
	return (1 <= iClient <= MaxClients && IsClientInGame(iClient)); 
}

stock bool IsClientSurvivor(int iClient)
{
	return (IsValidClient(iClient) && GetClientTeam(iClient) == 2 && IsPlayerAlive(iClient));
}

stock void SetVectorToZero(float fVec[3])
{
	for (int i = 0; i < 3; i++)fVec[i] = 0.0;
}

stock bool GetEntityAbsOrigin(int iEntity, float fResult[3])
{
	if (!L4D_IsValidEnt(iEntity))return false;
	SDKCall(g_hCalcAbsolutePosition, iEntity);
	GetEntPropVector(iEntity, Prop_Data, "m_vecAbsOrigin", fResult);
	return (IsValidVector(fResult));
}

stock bool GetEntityCenteroid(int iEntity, float fResult[3])
{
	int iOffset; static char sClass[64];
	GetEntityAbsOrigin(iEntity, fResult);

	if (!GetEntityNetClass(iEntity, sClass, sizeof(sClass)) || (iOffset = FindSendPropInfo(sClass, "m_vecMins")) == -1)
		return false;

	float fMins[3], fMaxs[3];
	GetEntDataVector(iEntity, iOffset, fMins);
	GetEntPropVector(iEntity, Prop_Send, "m_vecMaxs", fMaxs);

	fResult[0] += (fMins[0] + fMaxs[0]) * 0.5;
	fResult[1] += (fMins[1] + fMaxs[1]) * 0.5;
	fResult[2] += (fMins[2] + fMaxs[2]) * 0.5;

	return true;
}

bool IntervalHasPassed(float fInterval)
{
    return ((FloatFraction(GetGameTime() / fInterval) + GetGameFrameTime()) >= 1.0);
}

void LBI_GetNavAreaCenter(int iNavArea, float fResult[3])
{
	Address hAddress = view_as<Address>(iNavArea);	
	fResult[0] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_Center), NumberType_Int32));
	fResult[1] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_Center + 4), NumberType_Int32));
	fResult[2] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_Center + 8), NumberType_Int32));
}

int LBI_GetNavAreaParent(int iNavArea)
{
	return (LoadFromAddress(view_as<Address>(iNavArea) + view_as<Address>(g_iNavArea_Parent), NumberType_Int32));
}

bool LBI_IsDamagingNavArea(int iNavArea, bool bIgnoreWitches = false)
{	
	int iTickCount = LoadFromAddress(view_as<Address>(iNavArea) + view_as<Address>(g_iNavArea_DamagingTickCount), NumberType_Int32);
	if (GetGameTickCount() <= iTickCount)
	{
		if (!bIgnoreWitches && L4D2_GetWitchCount() > 0)
		{
			int iWitch = INVALID_ENT_REFERENCE;
			float fWitchPos[3], fClosePoint[3];
			while ((iWitch = FindEntityByClassname(iWitch, "witch")) != INVALID_ENT_REFERENCE)
			{
				GetEntityAbsOrigin(iWitch, fWitchPos);
				LBI_GetClosestPointOnNavArea(iNavArea, fWitchPos, fClosePoint);
				if (GetVectorDistance(fWitchPos, fClosePoint, true) <= 14400.0)return false;
			}
		}
		return true;
	}
	return false;
}

bool LBI_IsDamagingPosition(const float fPos[3])
{
	int iCloseArea = L4D_GetNearestNavArea(fPos);
	return (iCloseArea && LBI_IsDamagingNavArea(iCloseArea));
}

void LBI_GetNavAreaCorners(int iNavArea, float fNWCorner[3], float fSECorner[3])
{
	Address hAddress = view_as<Address>(iNavArea);
	for (int i = 0; i < 3; i++)
	{
		fNWCorner[i] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner + (4 * i)), NumberType_Int32));
		fSECorner[i] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner + (4 * i)), NumberType_Int32));
	}
}

void LBI_GetClosestPointOnNavArea(int iNavArea, const float fPos[3], float fClosePoint[3])
{
	float fNWCorner[3], fSECorner[3];
	LBI_GetNavAreaCorners(iNavArea, fNWCorner, fSECorner);

	float fNewPos[3];
	fNewPos[0] = fsel((fPos[0] - fNWCorner[0]), fPos[0], fNWCorner[0]);
	fNewPos[0] = fsel((fNewPos[0] - fSECorner[0]), fSECorner[0], fNewPos[0]);
	
	fNewPos[1] = fsel((fPos[1] - fNWCorner[1]), fPos[1], fNWCorner[1]);
	fNewPos[1] = fsel((fNewPos[1] - fSECorner[1]), fSECorner[1], fNewPos[1]);

	fNewPos[2] = LBI_GetNavAreaZ(iNavArea, fNewPos[0], fNewPos[1]);

	fClosePoint = fNewPos;
}

float LBI_GetNavAreaZ(int iNavArea, float x, float y)
{
	float fNWCorner[3], fSECorner[3];
	LBI_GetNavAreaCorners(iNavArea, fNWCorner, fSECorner);

	float fInvDXCorners = view_as<float>(LoadFromAddress(view_as<Address>(iNavArea) + view_as<Address>(g_iNavArea_InvDXCorners), NumberType_Int32));
	float fInvDYCorners = view_as<float>(LoadFromAddress(view_as<Address>(iNavArea) + view_as<Address>(g_iNavArea_InvDYCorners), NumberType_Int32));

	float u = (x - fNWCorner[0]) * fInvDXCorners;
	float v = (y - fNWCorner[1]) * fInvDYCorners;
	
	u = fsel(u, u, 0.0);
	u = fsel(u - 1.0, 1.0, u);

	v = fsel(v, v, 0.0);
	v = fsel(v - 1.0, 1.0, v);

	float fNorthZ = fNWCorner[2] + u * (fSECorner[2] - fNWCorner[2]);
	float fSouthZ = fNWCorner[2] + u * (fSECorner[2] - fNWCorner[2]);

	return (fNorthZ + v * (fSouthZ - fSouthZ));
}

stock float fsel(float fComparand, float fValGE, float fLT)
{
	return (fComparand >= 0.0 ? fValGE : fLT);
}

void LBI_GetNavAreaCorner(int iNavArea, int iCorner, float fResult[3])
{
	Address hAddress = view_as<Address>(iNavArea);
	switch(iCorner)
	{
		case 0:
		{
			fResult[0] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner), NumberType_Int32));
			fResult[1] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner+4), NumberType_Int32));
			fResult[2] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner+8), NumberType_Int32));
		}
		case 1:
		{
			fResult[0] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner), NumberType_Int32));
			fResult[1] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner+4), NumberType_Int32));
			fResult[2] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner+8), NumberType_Int32));
		}
		case 2:
		{
			fResult[0] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_NWCorner), NumberType_Int32));
			fResult[1] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner+4), NumberType_Int32));
			fResult[2] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner+8), NumberType_Int32));
		}
		case 3:
		{
			fResult[0] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner), NumberType_Int32));
			fResult[1] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner+4), NumberType_Int32));
			fResult[2] = view_as<float>(LoadFromAddress(hAddress + view_as<Address>(g_iNavArea_SECorner+8), NumberType_Int32));
		}
		default:
		{
			return;
		}
	}
}

bool LBI_IsNavAreaPartiallyVisible(int iNavArea, const float fEyePos[3], int iIgnoreEntity = -1)
{
	float fOffset = (0.75 * HUMAN_HEIGHT);
	
	float fCenter[3]; LBI_GetNavAreaCenter(iNavArea, fCenter);
	fCenter[2] += fOffset;

	Handle hResult = TR_TraceRayFilterEx(fEyePos, fCenter, MASK_VISIBLE_AND_NPCS, RayType_EndPoint, CTraceFilterNoNPCsOrPlayer, iIgnoreEntity);
	float fFraction = TR_GetFraction(hResult); delete hResult;
	if (fFraction == 1.0)return true;

	float fCorner[3];
	for (int i = 0; i < 4; ++i)
	{
		LBI_GetNavAreaCorner(iNavArea, i, fCorner);
		fCorner[2] += fOffset;

		hResult = TR_TraceRayFilterEx(fEyePos, fCorner, MASK_VISIBLE_AND_NPCS, RayType_EndPoint, CTraceFilterNoNPCsOrPlayer, iIgnoreEntity);
		fFraction = TR_GetFraction(hResult); delete hResult;
		if (fFraction == 1.0)return true;
	}

	return false;
}

bool CTraceFilterNoNPCsOrPlayer(int iEntity, int iContentsMask, int iIgnore)
{
	static char sClassname[64]; GetEntityClassname(iEntity, sClassname, sizeof(sClassname));
	if (strncmp(sClassname, "func_door", 9) == 0 || strncmp(sClassname, "prop_door", 9) == 0)
		return false;

	if (strcmp(sClassname, "func_brush") == 0)
	{
		int iSolidity = GetEntProp(iEntity, Prop_Data, "m_iSolidity");
		return (iSolidity == 2);
	}

	if ((strcmp(sClassname, "func_breakable_surf") == 0 || strcmp(sClassname, "func_breakable") == 0 && GetEntityHealth(iEntity) > 0) && GetEntProp(iEntity, Prop_Data, "m_takedamage") == 2)
		return false;

	if (strcmp(sClassname, "func_playerinfected_clip") == 0)
		return false;

	return (iEntity != iIgnore && !IsValidClient(iEntity));
}

bool LBI_IsPositionInsideCheckpoint(const float fPos[3])
{
	if (!L4D2_IsGenericCooperativeMode())
		return false;

	Address pNavArea = view_as<Address>(L4D_GetNearestNavArea(fPos));
	if (pNavArea == Address_Null)return false;

	int iAttributes = L4D_GetNavArea_SpawnAttributes(pNavArea);
	return ((iAttributes & NAV_SPAWN_FINALE) == 0 && (iAttributes & NAV_SPAWN_CHECKPOINT) != 0);
}

float GetClientTravelDistance(int iClient, float fGoalPos[3], bool bSquared = false)
{
	if (!g_bMapStarted)return -1.0;
	
	static bool bIsReachable;
	static int iStartArea, iGoalArea, iArea, iCount;
	static float t;

	StartProfiling(g_pProf);
	iStartArea = g_iClientNavArea[iClient];
	if (!iStartArea)
	{
		StopProfiling(g_pProf);
		t = GetProfilerTime(g_pProf);
		if(g_iCvar_Debug & DEBUG_NAV && t > 0.001)
			PrintToServer("GetClientTravelDist took %.8f seconds !iStartArea", t);
		return -1.0;
	}

	iGoalArea = L4D_GetNearestNavArea(fGoalPos, _, true, true, false, GetClientTeam(iClient)); // need to think about which checkLOS and checkGround bools to put here
	if (!iGoalArea)
	{
		StopProfiling(g_pProf);
		t = GetProfilerTime(g_pProf);
		if(g_iCvar_Debug & DEBUG_NAV && t > 0.001)
			PrintToServer("GetClientTravelDist took %.8f seconds !iGoalArea", t);
		return -1.0;
	}

	//if (!L4D2_NavAreaBuildPath(view_as<Address>(iStartArea), view_as<Address>(iGoalArea), 0.0, GetClientTeam(iClient), false))
	bIsReachable = L4D2_IsReachable(iClient, fGoalPos);
	if (!bIsReachable)
	{
		StopProfiling(g_pProf);
		t = GetProfilerTime(g_pProf);
		if(g_iCvar_Debug & DEBUG_NAV && t > 0.001)
		{
			char sClientName[128];
			GetClientName(iClient, sClientName, sizeof(sClientName));
			PrintToServer("GetClientTravelDist took %.8f seconds !IsReachable, Client %s, bIsReachable %b, fGoalPos %.1f %.1f %.1f", t, sClientName, bIsReachable, fGoalPos[0], fGoalPos[1], fGoalPos[2]);
		}
		return -1.0;
	}

	iArea = LBI_GetNavAreaParent(iGoalArea);
	if (!iArea)
	{
		StopProfiling(g_pProf);
		t = GetProfilerTime(g_pProf);
		if(g_iCvar_Debug & DEBUG_NAV && t > 0.001)
			PrintToServer("GetClientTravelDist took %.8f seconds !iArea", t);
		return GetVectorDistance(g_fClientAbsOrigin[iClient], fGoalPos, bSquared);
	}

	float fClosePoint[3]; LBI_GetClosestPointOnNavArea(iArea, fGoalPos, fClosePoint);
	float fDistance = GetVectorDistance(fClosePoint, fGoalPos, bSquared);
	float fParentCenter[3];
	
	iCount = 0;
	for (; LBI_GetNavAreaParent(iArea); iArea = LBI_GetNavAreaParent(iArea))
	{
		if (iCount > 50)
			break;
		LBI_GetClosestPointOnNavArea(LBI_GetNavAreaParent(iArea), fGoalPos, fParentCenter);
		LBI_GetClosestPointOnNavArea(iArea, fGoalPos, fClosePoint);
		fDistance += GetVectorDistance(fClosePoint, fParentCenter, bSquared);
		iCount++;
	}
	//if(g_bCvar_Debug && iCount > 20)
	//	PrintToServer("GetClientTravelDist %d iterations of lag loop", iCount);

	LBI_GetClosestPointOnNavArea(iArea, fGoalPos, fClosePoint);
	fDistance += GetVectorDistance(g_fClientAbsOrigin[iClient], fClosePoint, bSquared);
	
	StopProfiling(g_pProf);
	t = GetProfilerTime(g_pProf);
	if(g_iCvar_Debug & DEBUG_NAV && t > 0.001)
	{
		char sClientName[128];
		GetClientName(iClient, sClientName, sizeof(sClientName));
		PrintToServer("GetClientTravelDist took %.8f seconds, iCount %d, Client %s, bIsReachable %b, fDistance %.2f, fGoalPos %.1f %.1f %.1f",
			t, iCount, sClientName, bIsReachable, fDistance, fGoalPos[0], fGoalPos[1], fGoalPos[2]);
	}
	return fDistance;
}

float GetClientDistanceToItem(int iClient, int iEntity, bool bIgnoreNavBlockers = true)
{
	if (!g_bMapStarted)
		return -1.0;
	
	static bool bIsReachable;
	static int iGoalArea;
	static float fDistance, fEntityPos[3], fTargetPos[3]/*, t*/;
	
	//StartProfiling(g_pProf);
	//get or calculate target position
	iGoalArea = 0;
	if(IsValidClient(iEntity))
		fTargetPos = g_fClientAbsOrigin[iEntity];
	else
	{
		GetEntityAbsOrigin(iEntity, fEntityPos);
		iGoalArea = L4D_GetNearestNavArea(fEntityPos, 300.0, true, true, false, GetClientTeam(iClient));
		if (!iGoalArea)
			return -1.0;
		LBI_GetClosestPointOnNavArea(iGoalArea, fTargetPos, fTargetPos);
	}
	
	//if bot, use L4D2_IsReachable
	if (IsFakeClient(iClient))
	{
		bIsReachable = L4D2_IsReachable(iClient, fEntityPos);
		if (!bIsReachable)
			return -1.0;
	}

	fDistance = L4D2_NavAreaTravelDistance(g_fClientAbsOrigin[iClient], fTargetPos, bIgnoreNavBlockers);
	if (iGoalArea)
		fDistance += GetVectorDistance(fTargetPos, fEntityPos, false);
	
	//StopProfiling(g_pProf);
	//t = GetProfilerTime(g_pProf);
	//if(g_bCvar_Debug && t > 0.001)
	//{
	//	char sClientName[128], sClassname[64];
	//	GetClientName(iClient, sClientName, sizeof(sClientName));
	//	GetEntityClassname(iEntity, sClassname, sizeof(sClassname));
	//	PrintToServer("GetClientDistanceToItem took %.8f seconds, %d Client %s, Target %s, fDistance %.2f, fTargetPos %.1f %.1f %.1f", t, iClient, sClientName, sClassname, fDistance, fTargetPos[0], fTargetPos[1], fTargetPos[2]);
	//}
	
	return fDistance;
}

//to replace GetVectorTravelDistance()
float GetNavDistance(float fStartPos[3], float fGoalPos[3], int iEntity = -1, bool bCheckLOS = true)
{
	if (!g_bMapStarted)return -1.0;
	
	static int iStartArea, iGoalArea;
	static float fDistance;
	static char sEntClassname[64];
	
	sEntClassname[0] = EOS;
	if (iEntity != -1)
		GetEntityClassname(iEntity, sEntClassname, sizeof(sEntClassname));
	
	if (iEntity != -1 && g_hBadPathEntities.FindValue(EntIndexToEntRef(iEntity)) != -1)
	{
		if(g_iCvar_Debug & DEBUG_NAV)
			PrintToServer("GetNavDistance: entity %d %s is in bad pathing table", iEntity, sEntClassname);
		return -1.0;
	}

	iStartArea = L4D_GetNearestNavArea(fStartPos, 140.0, true, bCheckLOS, false, 2);
	if (!iStartArea)
	{
		if(g_iCvar_Debug & DEBUG_NAV)
			PrintToServer("GetNavDistance: could not find iStartArea, fStartPos %.2f %.2f %.2f ent %d %s", fStartPos[0], fStartPos[1], fStartPos[2], iEntity, sEntClassname);
		if(iEntity != -1)
			PushEntityIntoArray(g_hBadPathEntities, iEntity);
		return -1.0;
	}
	
	iGoalArea = L4D_GetNearestNavArea(fGoalPos, 140.0, true, bCheckLOS, false, 2);
	if (!iGoalArea)
	{
		if(g_iCvar_Debug & DEBUG_NAV)
			PrintToServer("GetNavDistance: could not find iGoalArea, fGoalPos %.2f %.2f %.2f ent %d %s", fGoalPos[0], fGoalPos[1], fGoalPos[2], iEntity, sEntClassname);
		if(iEntity != -1)
			PushEntityIntoArray(g_hBadPathEntities, iEntity);
		return -1.0;
	}
	else
	{
		LBI_GetClosestPointOnNavArea(iGoalArea, fGoalPos, fGoalPos);
	}
	
	fDistance = L4D2_NavAreaTravelDistance(fStartPos, fGoalPos, true);
	if (g_iCvar_Debug & DEBUG_NAV && fDistance < 0.0)
		PrintToServer("GetNavDistance: fDistance %.2f fStartPos %.2f %.2f %.2f ent %d %s", fDistance, fStartPos[0], fStartPos[1], fStartPos[2], iEntity, sEntClassname);
	
	if (iEntity != -1 && fDistance < 0.0)
	{
		PushEntityIntoArray(g_hBadPathEntities, iEntity);
		if (g_hClearBadPathTimer == INVALID_HANDLE)
			g_hClearBadPathTimer = CreateTimer(0.1, ClearBadPathEntsTable);
	}
	
	return fDistance;
}

public Action ClearBadPathEntsTable(Handle timer)
{
	g_hBadPathEntities.Clear();
	g_hClearBadPathTimer = INVALID_HANDLE;
	return Plugin_Handled;
}

bool LBI_GetBonePosition(int iEntity, const char[] sBoneName, float fBuffer[3])
{
	if (!L4D_IsValidEnt(iEntity))return false;

	int iBoneIndex = SDKCall(g_hLookupBone, iEntity, sBoneName);
	if (iBoneIndex == -1)return false;

	static float fUnusedAngles[3];
	SDKCall(g_hGetBonePosition, iEntity, iBoneIndex, fBuffer, fUnusedAngles);

	return (IsValidVector(fBuffer));
}

bool LBI_IsSurvivorInCombat(int iClient)
{
	return g_bBot_IsInCombat[iClient];
}

bool LBI_IsSurvivorBotAvailable(int iClient)
{
	return g_bBot_IsAvailable[iClient];
}

int LBI_FindUseEntity(int iClient, float fCheckDist = 96.0, float fFloat_1 = 0.0, float fFloat_2 = 0.0, bool bBool_1 = false, bool bBool_2 = false)
{
	return (SDKCall(g_hFindUseEntity, iClient, fCheckDist, fFloat_1, fFloat_2, bBool_1, bBool_2));
}

bool LBI_IsReachableNavArea(int iClient, int iGoalArea, int iStartArea = -1)
{
	int iLastArea = g_iClientNavArea[iClient];
	if (!iLastArea)return false;
	
	if (iStartArea == -1)iStartArea = iLastArea;
	return (iStartArea && (iStartArea == iGoalArea || SDKCall(g_hIsReachableNavArea, iClient, iStartArea, iGoalArea)));
}

// don't fucking break the game if the position is 1mm inside of a prop, or 1mm below ground, a'ight??
bool LBI_IsReachablePosition(int iClient, const float fPos[3], bool bCheckLOS = true)
{
	int iNearArea = L4D_GetNearestNavArea(fPos, 200.0, true, bCheckLOS, false, 0);
	return (iNearArea && LBI_IsReachableNavArea(iClient, iNearArea));
}

bool LBI_IsReachableEntity(int iClient, int iEntity)
{
	if (!L4D_IsValidEnt(iEntity) || IsValidClient(iEntity) && !g_iClientNavArea[iEntity])return false;
	float fEntityPos[3]; GetEntityAbsOrigin(iEntity, fEntityPos);
	return (LBI_IsReachablePosition(iClient, fEntityPos));
}

MRESReturn Detour_CommandIssued(DHookReturn hReturn, DHookParam hParam)
{
	static int iCommand, iUserID, iClient, iTarget;
	iCommand = hParam.Get(1);
	iUserID = hParam.Get(2);
	iTarget = hParam.Get(3);
	iClient = GetClientOfUserId(iUserID);
	
	if (g_iCvar_Debug & DEBUG_MOVE && ( g_iCvar_DebugClient == 0 || iClient == g_iCvar_DebugClient ))
	{
		char sEntClassname[64], sClientName[128];
		if (iClient > -1)
			GetClientName(iClient, sClientName, sizeof(sClientName));
		if (iTarget > -1)
			GetEntityClassname(iTarget, sEntClassname, sizeof(sEntClassname));
		PrintToServer("Received %s command for %d %s, target %d %s", IBVScriptCommands[iCommand-1], iClient, (iClient > 0 ? sClientName : "everyone"), iTarget, (iTarget != -1 ? sEntClassname : "not defined"));
	}
	
	if (iCommand == 1)
	{
		if (iClient <= 0)
		{
			for (int i = 1; i <= MaxClients; i++)
			{
				if (!IsClientSurvivor(i) || !IsFakeClient(i))
					continue;
				ClearMoveToPosition(i);
			}
		}
		else
			ClearMoveToPosition(iClient);
	}
	
	hReturn.Value = true;
	return MRES_Supercede;
}

MRESReturn DHooks_OnWpnPrimaryAttack(int iEntity)
{
	int iClient = GetEntityOwner(iEntity);
	if (IsClientSurvivor(iClient) && GetGameTime() <= g_fBot_PreventFireTime[iClient])
		return MRES_Supercede;

	return MRES_Ignored;
}

MRESReturn DTR_OnSurvivorBotGetAvoidRange(int iClient, Handle hReturn, Handle hParams)
{
	int iTarget = DHookGetParam(hParams, 1); 
	float fAvoidRange = DHookGetReturn(hReturn);
	float fInitRange = fAvoidRange;

	if (fAvoidRange == 125.0)
	{
		if (iTarget <= MaxClients)
		{
			if (g_bCvar_Nightmare && L4D2_GetPlayerZombieClass(iTarget) == L4D2ZombieClass_Jockey)
				fAvoidRange = 500.0;
			else
				fAvoidRange = ((!IsUsingSpecialAbility(iTarget) && IsValidClient(L4D_GetPinnedSurvivor(iTarget))) ? 0.0 : 200.0);

		}
		else if (SurvivorHasMeleeWeapon(iClient))
		{
			fAvoidRange = 50.0;
		}
	}
	else if (fAvoidRange == 450.0)
	{
		int iVictim = g_iInfectedBot_CurrentVictim[iTarget];
		fAvoidRange = ((iVictim == iClient || iVictim > 0 && GetClientDistance(iVictim, iClient, true) <= 160000.0) ? 700.0 : 300.0); // 400
	}
	else if (fAvoidRange == 300.0 && g_iBot_WitchTarget[iClient] != -1 && g_bBot_IsWitchHarasser[iClient])
	{
		fAvoidRange = 750.0;
	}

	if (IsSurvivorCarryingProp(iClient))
	{
		fAvoidRange += 200.0;
	}

	// PrintToServer("%N = %i, %f/%f", iClient, iTarget, fAvoidRange, fInitRange);

	if (fInitRange != fAvoidRange)
	{
		DHookSetReturn(hReturn, fAvoidRange);
		return MRES_ChangedOverride;
	}

	return MRES_Ignored;
}

MRESReturn DTR_OnInfernoTouchNavArea(int iInferno, Handle hReturn, Handle hParams)
{
	bool bIsTouching = DHookGetReturn(hReturn);
	if (!bIsTouching)return MRES_Ignored;

	int iNavArea = DHookGetParam(hParams, 1);
	if (!iNavArea)return MRES_Ignored;

	float fAreaPos[3];
	bool bCanBlock = true;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsFakeClient(i) || !IsPlayerAlive(i) || GetClientTeam(i) != 2 || L4D_IsPlayerIncapacitated(i) || L4D_IsPlayerPinned(i))
			continue;

		bCanBlock = (g_iClientNavArea[i] != iNavArea);
		if (!bCanBlock)break;

		LBI_GetClosestPointOnNavArea(iNavArea, g_fClientAbsOrigin[i], fAreaPos);
		bCanBlock = (GetVectorDistance(g_fClientAbsOrigin[i], fAreaPos, true) > 9216.0); // 96
		if (!bCanBlock)break;
	}

	if (bCanBlock)
		SDKCall(g_hMarkNavAreaAsBlocked, iNavArea, 2, iInferno, true);

	return MRES_Ignored;
}

MRESReturn DTR_OnFindUseEntity(int iClient, Handle hReturn, Handle hParams)
{
	static int iScavengeItem;
	static float fDistance, fScavengePos[3];

	if (!IsValidClient(iClient) || !IsFakeClient(iClient))
		return MRES_Ignored;

	iScavengeItem = g_iBot_ScavengeItem[iClient];
	if (!L4D_IsValidEnt(iScavengeItem))
		return MRES_Ignored;

	GetEntityCenteroid(iScavengeItem, fScavengePos);
	fDistance = GetVectorDistance(g_fClientEyePos[iClient], fScavengePos, true);

	if (fDistance > g_fCvar_ItemScavenge_PickupRange_Sqr)
	{
		// if (g_iCvar_Debug & DEBUG_SCAVENGE && (!g_iCvar_DebugClient || iClient == g_iCvar_DebugClient))
		// {
		// 	static char szEntClassname[64]; 
		// 	GetEdictClassname(iScavengeItem, szEntClassname, sizeof(szEntClassname));

		// 	PrintToServer("DTR_OnFindUseEntity: Preventing %N from grabbing %s, distance %.2f", iClient, szEntClassname, SquareRoot(fDistance));
		// }

		return MRES_Ignored;
	}

	DHookSetReturn(hReturn, iScavengeItem);
	return MRES_ChangedOverride;
}

int LBI_IsPathToPositionDangerous(int iClient, float fGoalPos[3])
{
	if (!g_bMapStarted)
		return -1;

	static int iClientArea, iGoalArea, iParent, iCount, iTank;
	static float fTankDist, fGoalDist, fGoalOffset[3], fAreaPos[3];
	
	if (g_iBot_TankTarget[iClient])
	{
		ArrayList hTankList = new ArrayList();
		
		fGoalOffset = fGoalPos;
		fGoalOffset[2] += HUMAN_HALF_HEIGHT;

		for (int i = 1; i <= MaxClients; i++)
		{
			if (i == iClient || !IsClientInGame(i) || !IsPlayerAlive(i) || L4D2_GetPlayerZombieClass(i) != L4D2ZombieClass_Tank || L4D_IsPlayerIncapacitated(i))
				continue;

			fGoalDist = GetVectorDistance(g_fClientAbsOrigin[iClient], fGoalPos, true);
			// 300 & 150
			if (fGoalDist <= 90000.0 && GetVectorDistance(g_fClientAbsOrigin[i], fGoalPos, true) <= 22500.0 && IsVisibleVector(i, fGoalOffset, MASK_VISIBLE_AND_NPCS))
			{
				delete hTankList;
				return i;
			}

			// 500
			if (fGoalDist > 250000.0)
			{
				fTankDist = GetClientTravelDistance(i, g_fClientAbsOrigin[iClient], true);
				// 250 & 750
				if (fTankDist <= 62500.0 || fTankDist <= 562500.0 && g_iInfectedBot_CurrentVictim[i] == iClient && IsVisibleEntity(iClient, i, MASK_VISIBLE_AND_NPCS))
				{
					delete hTankList;
					return i;
				}
			}

			hTankList.Push(i);
		}

		iClientArea = g_iClientNavArea[iClient];
		if (!iClientArea)
		{
			delete hTankList;
			return -1;
		}

		iGoalArea = L4D_GetNearestNavArea(fGoalPos, _, true, true, true);
		if (!iGoalArea)
		{
			delete hTankList;
			return -1;
		}

		// if (!L4D2_IsReachable(iClient, fGoalPos))
		// This is needed for the code below to work
		if (!L4D2_NavAreaBuildPath(view_as<Address>(iClientArea), view_as<Address>(iGoalArea), 0.0, 2, false))
		{
			delete hTankList;
			return -1;
		}
		
		iParent = LBI_GetNavAreaParent(iGoalArea);
		if (iParent)
		{
			// iCount = 0;
			for (; LBI_GetNavAreaParent(iParent); iParent = LBI_GetNavAreaParent(iParent))
			{
				// if (iCount > 25)
				// 	//i ain't calculating all that
				// 	//happy for you though
				// 	//or sorry that happened
				// 	break;
				for (int i = 0; i < hTankList.Length; i++)
				{
					iTank = hTankList.Get(i);

					if (g_iClientNavArea[iTank] != iParent)
					{
						LBI_GetClosestPointOnNavArea(iParent, g_fClientAbsOrigin[iTank], fAreaPos);
						if (g_iInfectedBot_CurrentVictim[iTank] != iClient && GetVectorDistance(g_fClientAbsOrigin[iTank], fAreaPos, true) > 22500.0)continue;
					}

					delete hTankList;
					return (g_iInfectedBot_CurrentVictim[iTank] == iClient ? iTank : 0);
				}
				iCount++;
			}
			// if(g_iCvar_Debug & DEBUG_NAV && iCount > 10)
			// 	PrintToServer("IsPathToPositionDangerous: %d iterations of lag loop", iCount);
		}
		delete hTankList;
	}

	return -1;
}

public Action L4D2_OnChooseVictim(int iInfected, int &iTarget)
{
	g_iInfectedBot_CurrentVictim[iInfected] = iTarget;
	return Plugin_Continue;
}

public void OnActionCreated(BehaviorAction hAction, int iActor, const char[] sName)
{
	if (strcmp(sName[8], "LegsRegroup") == 0)
	{
		hAction.OnUpdatePost = OnRegroupWithTeamAction;
	}
	else if (strcmp(sName[8], "LiberateBesiegedFriend") == 0)
	{
		hAction.OnUpdatePost = OnMoveToIncapacitatedFriendAction;
	}
}

Action OnRegroupWithTeamAction(BehaviorAction hAction, int iActor, float fInterval, ActionResult hResult)
{
	int iLeader = (hAction.Get(0x34) & 0xFFF);
	if (!IsValidClient(iLeader))return Plugin_Continue;

	int iPathDangerous = LBI_IsPathToPositionDangerous(iActor, g_fClientAbsOrigin[iLeader]);
	if (iPathDangerous == -1)return Plugin_Continue;

	if (iPathDangerous != 0)
	{
		hResult.type = CHANGE_TO;
		hResult.action = CreateSurvivorLegsRetreatAction(iPathDangerous);
		return Plugin_Handled;
	}

	// hResult.type = DONE;
	// hResult.action = INVALID_ACTION;
	// return Plugin_Changed;

	return Plugin_Continue;
}

Action OnMoveToIncapacitatedFriendAction(BehaviorAction hAction, int iActor, float fInterval, ActionResult hResult)
{
	int iFriend = (hAction.Get(0x34) & 0xFFF);
	if (!IsValidClient(iFriend) || L4D_GetPlayerReviveTarget(iActor) == iFriend || GetClientDistance(iActor, iFriend, true) <= 15625.0 && IsVisibleEntity(iActor, iFriend)) // 125
		return Plugin_Continue;

	int iPathDangerous = LBI_IsPathToPositionDangerous(iActor, g_fClientAbsOrigin[iFriend]);
	if (iPathDangerous == -1)return Plugin_Continue;

	if (iPathDangerous != 0)
	{
		hResult.type = CHANGE_TO;
		hResult.action = CreateSurvivorLegsRetreatAction(iPathDangerous);
		return Plugin_Handled;
	}

	// hResult.type = DONE;
	// hResult.action = INVALID_ACTION;
	// return Plugin_Changed;
	
	return Plugin_Continue;
}

BehaviorAction CreateSurvivorLegsRetreatAction(int iThreat)
{
	BehaviorAction hAction = ActionsManager.Allocate(0x745A);
	SDKCall(g_hSurvivorLegsRetreat, hAction, iThreat);
	return hAction;
}

// =====================================================================
// LLM STRATEGIC DECISION MODULE - Implementation
// =====================================================================

void LLM_Init()
{
	g_hCvar_LLM_Enabled = CreateConVar("ib_llm_enabled", "1", "Enable LLM strategic decision layer (0=off, 1=on)");
	g_hCvar_LLM_Host = CreateConVar("ib_llm_host", "172.20.0.1", "LLM Python service host");
	g_hCvar_LLM_Port = CreateConVar("ib_llm_port", "9876", "LLM Python service port");
	g_hCvar_LLM_Interval = CreateConVar("ib_llm_interval", "3.0", "Seconds between LLM state updates");

	// Test mode: auto-spawn SI for LLM testing without human players
	g_hCvar_LLM_TestMode = CreateConVar("ib_llm_testmode", "0", "Auto-spawn SI for LLM testing (0=off, 1=on)");

	// Admin commands for manual testing
	RegAdminCmd("sm_llm_spawn_si", Cmd_LLM_SpawnSI, ADMFLAG_ROOT, "Spawn a random SI near survivors");
	RegAdminCmd("sm_llm_spawn_tank", Cmd_LLM_SpawnTank, ADMFLAG_ROOT, "Spawn a tank near survivors");
	RegAdminCmd("sm_llm_spawn_witch", Cmd_LLM_SpawnWitch, ADMFLAG_ROOT, "Spawn a witch near survivors");
	RegAdminCmd("sm_llm_status", Cmd_LLM_Status, ADMFLAG_ROOT, "Show LLM module status");

	// Hook events for LLM awareness
	HookEvent("witch_harasser_set", LLM_EventWitchHarassed);
	HookEvent("tank_spawn", LLM_EventTankSpawn);
	HookEvent("choke_start", LLM_EventChokeStart);
	HookEvent("lunge_pounce", LLM_EventLungePounce);
	HookEvent("jockey_ride", LLM_EventJockeyRide);
	HookEvent("charger_pummel_start", LLM_EventChargerPummel);

	PrintToServer("[LLM] Module initialized (ib_llm_enabled=%d)", g_hCvar_LLM_Enabled.IntValue);
}

// Called from OnMapStart
void LLM_OnMapStart()
{
	SetConVarInt(FindConVar("sv_hibernate_when_empty"), 0);
	SetConVarInt(FindConVar("sb_all_bot_game"), 1);
	SetConVarInt(FindConVar("allow_all_bot_survivor_team"), 1);

	LLM_LoadTerrain();
	LLM_Connect();
	g_iLLM_EventCount = 0;
	g_iLLM_EventIndex = 0;
	for (int i = 0; i < LLM_RECENT_EVENTS_MAX; i++) g_fLLM_EventTime[i] = 0.0;
	g_iLLM_TrackedBot = -1;
	g_iLLM_Seq = 0;
	// Reset player scores
	for (int i = 0; i <= MAXPLAYERS; i++)
	{
		g_iLLM_PlayerIncap[i] = 0;
		g_iLLM_PlayerPinned[i] = 0;
		g_iLLM_PlayerRevive[i] = 0;
		g_iLLM_PlayerHeal[i] = 0;
		g_iLLM_PlayerSIKills[i] = 0;
		g_bLLM_WasPinned[i] = false;
	}

	// Force game start: L4D2 doesn't spawn survivor bots without a human player.
	// Create a temporary fake client to trigger the game round, then kick it.
	// sb_all_bot_game=1 keeps bots alive after the launcher disconnects.
	CreateTimer(8.0, LLM_TimerForceStart, _, TIMER_FLAG_NO_MAPCHANGE);

	// Test mode: auto-spawn SI periodically for LLM testing
	delete g_hLLM_TestTimer;
	g_hLLM_TestTimer = CreateTimer(20.0, LLM_TimerTestSpawn, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);

	PrintToServer("[LLM] OnMapStart - LLM layer active");
}

public Action LLM_TimerForceStart(Handle timer)
{
	// Check if survivor bots already exist (e.g. human player connected)
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 2 && IsPlayerAlive(i))
		{
			PrintToServer("[LLM] Survivor bots already exist, skipping force start");
			return Plugin_Stop;
		}
	}

	PrintToServer("[LLM] Force starting game (no survivor bots found)...");

	// Create fake clients to fill all 4 survivor slots.
	// L4D2 replaces fake clients on team 2 with real survivor bots.
	// We kick the fake clients after a short delay to let the game process.
	int fakeClients[4];
	int fakeCount = 0;
	for (int slot = 0; slot < 4; slot++)
	{
		char name[32];
		Format(name, sizeof(name), "LLM_Boot_%d", slot);
		int fc = CreateFakeClient(name);
		if (fc != 0)
		{
			ChangeClientTeam(fc, 2);
			fakeClients[fakeCount] = fc;
			fakeCount++;
		}
		else
		{
			break;
		}
	}

	if (fakeCount > 0)
	{
		PrintToServer("[LLM] Created %d fake clients on team 2", fakeCount);
		// Kick after 2 seconds to let game spawn real survivor bots
		for (int i = 0; i < fakeCount; i++)
		{
			CreateTimer(2.0, LLM_TimerKickLauncher, fakeClients[i], TIMER_FLAG_NO_MAPCHANGE);
		}
	}
	else
	{
		PrintToServer("[LLM] CreateFakeClient failed");
	}
	return Plugin_Stop;
}

public Action LLM_TimerKickLauncher(Handle timer, int client)
{
	if (client > 0 && client <= MaxClients && IsClientInGame(client))
	{
		KickClient(client, "done");
	}

	// Count survivor bots after each kick
	int count = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 2 && IsPlayerAlive(i))
			count++;
	}
	if (count > 0)
		PrintToServer("[LLM] After kick: %d survivor bots active", count);
	return Plugin_Stop;
}

// ===================== Socket =====================

void LLM_Connect()
{
	g_bLLM_Connected = false;
	if (g_hLLM_Socket != null) { g_hLLM_Socket.Disconnect(); delete g_hLLM_Socket; }
	g_hLLM_Socket = new Socket(SOCKET_TCP, LLM_OnSocketError);
	if (g_hLLM_Socket != null)
	{
		char host[64]; g_hCvar_LLM_Host.GetString(host, sizeof(host));
		int port = g_hCvar_LLM_Port.IntValue;
		g_hLLM_Socket.Connect(LLM_OnConnected, LLM_OnReceive, LLM_OnDisconnected, host, port);
		PrintToServer("[LLM] Connecting to %s:%d...", host, port);
		// Connection timeout: if not connected in 10s, retry
		delete g_hLLM_ConnectTimer;
		g_hLLM_ConnectTimer = CreateTimer(10.0, LLM_TimerConnectTimeout);
	}
}

public void LLM_OnConnected(Socket socket, any arg)
{
	g_bLLM_Connected = true;
	delete g_hLLM_ConnectTimer;
	PrintToServer("[LLM] Connected to Python server!");
}

public void LLM_OnReceive(Socket socket, const char[] data, const int size, any arg)
{
	LLM_ParseDecision(data, size);
}

public void LLM_OnDisconnected(Socket socket, any arg)
{
	g_bLLM_Connected = false;
	PrintToServer("[LLM] Disconnected");
	CreateTimer(5.0, LLM_TimerReconnect);
}

public void LLM_OnSocketError(Socket socket, const int errorType, const int errorNum, any arg)
{
	g_bLLM_Connected = false;
	PrintToServer("[LLM] Socket error: type=%d num=%d", errorType, errorNum);
	CreateTimer(5.0, LLM_TimerReconnect);
}

public Action LLM_TimerReconnect(Handle timer)
{
	if (!g_bLLM_Connected) LLM_Connect();
	return Plugin_Stop;
}

public Action LLM_TimerConnectTimeout(Handle timer)
{
	g_hLLM_ConnectTimer = null;
	if (!g_bLLM_Connected)
	{
		PrintToServer("[LLM] Connection timeout, retrying...");
		LLM_Connect();
	}
	return Plugin_Stop;
}

// ===================== State Collection =====================

void LLM_SendState()
{
	if (!g_bLLM_Connected || !g_hCvar_LLM_Enabled.BoolValue) return;

	float interval = g_hCvar_LLM_Interval.FloatValue;
	if (GetGameTime() - g_fLLM_LastSendTime < interval) return;
	g_fLLM_LastSendTime = GetGameTime();

	// Find tracked bot
	if (g_iLLM_TrackedBot == -1 || !IsClientInGame(g_iLLM_TrackedBot))
	{
		g_iLLM_TrackedBot = -1;
		g_iLLM_Seq = 0;
		for (int i = 1; i <= MaxClients; i++)
		{
			if (IsClientInGame(i) && IsFakeClient(i) && IsClientSurvivor(i) && IsPlayerAlive(i))
			{
				g_iLLM_TrackedBot = i;
				PrintToServer("[LLM] Tracked bot: %d (%N)", i, i);
				break;
			}
		}
		if (g_iLLM_TrackedBot == -1) return;
	}

	int bot = g_iLLM_TrackedBot;
	char state[LLM_STATE_BUFFER];
	LLM_CollectState(bot, state, sizeof(state));

	if (g_hLLM_Socket != null && g_bLLM_Connected)
	{
		g_hLLM_Socket.Send(state);
	}
}

void LLM_CollectState(int bot, char[] buffer, int maxlen)
{
	char mapName[128]; GetCurrentMap(mapName, sizeof(mapName));
	char gameMode[32] = "coop";
	ConVar hMode = FindConVar("mp_gamemode");
	if (hMode != null) hMode.GetString(gameMode, sizeof(gameMode));
	char difficulty[16] = "normal";
	ConVar hDiff = FindConVar("z_difficulty");
	if (hDiff != null) hDiff.GetString(difficulty, sizeof(difficulty));

	char botName[64]; GetClientName(bot, botName, sizeof(botName));
	int health = GetClientHealth(bot);
	float tempHealth = GetEntPropFloat(bot, Prop_Send, "m_healthBuffer");
	bool incap = view_as<bool>(GetEntProp(bot, Prop_Send, "m_isIncapacitated"));
	bool bw = view_as<bool>(GetEntProp(bot, Prop_Send, "m_bIsOnThirdStrike"));
	float pos[3]; GetClientAbsOrigin(bot, pos);
	float angles[3]; GetClientEyeAngles(bot, angles);

	// Weapons
	char primary[64]="none", secondary[64]="none", grenade[32]="none";
	char healthItem[32]="none", pillsItem[32]="none";
	int primaryAmmo = 0;
	int reserveAmmo = 0;
	int w;
	w = GetPlayerWeaponSlot(bot, 0);
	if (w != -1) { GetEntityClassname(w, primary, sizeof(primary)); primaryAmmo = GetEntProp(w, Prop_Send, "m_iClip1"); reserveAmmo = GetEntProp(w, Prop_Send, "m_iExtra1"); }
	w = GetPlayerWeaponSlot(bot, 1);
	if (w != -1) GetEntityClassname(w, secondary, sizeof(secondary));
	w = GetPlayerWeaponSlot(bot, 2);
	if (w != -1) GetEntityClassname(w, grenade, sizeof(grenade));
	w = GetPlayerWeaponSlot(bot, 3);
	if (w != -1) GetEntityClassname(w, healthItem, sizeof(healthItem));
	w = GetPlayerWeaponSlot(bot, 4);
	if (w != -1) GetEntityClassname(w, pillsItem, sizeof(pillsItem));

	// Teammates
	char mates[1024]="";
	int mc = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (i == bot || !IsClientInGame(i) || !IsClientSurvivor(i)) continue;
		char mn[64]; GetClientName(i, mn, sizeof(mn));
		int mhp = GetClientHealth(i);
		float mTempHp = GetEntPropFloat(i, Prop_Send, "m_healthBuffer");
		bool mi = view_as<bool>(GetEntProp(i, Prop_Send, "m_isIncapacitated"));
		bool mb = view_as<bool>(GetEntProp(i, Prop_Send, "m_bIsOnThirdStrike"));
		float mp[3]; GetClientAbsOrigin(i, mp);
		float dist = GetVectorDistance(pos, mp);
		bool pin = LLM_IsPinned(i);
		char mPinType[16]; LLM_GetPinType(i, mPinType, sizeof(mPinType));
		// Teammate primary weapon
		char mWeapon[64] = "none";
		int mw = GetPlayerWeaponSlot(i, 0);
		if (mw != -1) GetEntityClassname(mw, mWeapon, sizeof(mWeapon));
		if (mc > 0) Format(mates, sizeof(mates), "%s,", mates);
		Format(mates, sizeof(mates), "%s{\"name\":\"%s\",\"hp\":%d,\"temp_hp\":%.0f,\"incap\":%s,\"bw\":%s,\"pinned\":%s,\"pin_type\":\"%s\",\"weapon\":\"%s\",\"dist\":%.0f}",
			mates, mn, mhp, mTempHp, mi?"true":"false", mb?"true":"false", pin?"true":"false", mPinType, mWeapon, dist);
		mc++;
	}

	// SI Threats
	char threats[512]="";
	int tc = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || GetClientTeam(i) != 3 || !IsPlayerAlive(i)) continue;
		int zc = GetEntProp(i, Prop_Send, "m_zombieClass");
		char zn[32];
		switch (zc) {
			case 1: zn="smoker"; case 2: zn="boomer"; case 3: zn="hunter";
			case 4: zn="spitter"; case 5: zn="jockey"; case 6: zn="charger";
			case 8: zn="tank"; default: zn="unknown";
		}
		float tp[3]; GetClientAbsOrigin(i, tp);
		float dist = GetVectorDistance(pos, tp);
		bool ghost = view_as<bool>(GetEntProp(i, Prop_Send, "m_isGhost"));
		int thp = GetClientHealth(i);
		if (tc > 0) Format(threats, sizeof(threats), "%s,", threats);
		Format(threats, sizeof(threats), "%s{\"type\":\"%s\",\"hp\":%d,\"dist\":%.0f,\"ghost\":%s}",
			threats, zn, thp, dist, ghost?"true":"false");
		tc++;
	}

	// Witches
	char witches[256]="";
	int wc = 0;
	int ent = -1;
	while ((ent = FindEntityByClassname(ent, "witch")) != -1)
	{
		// Witch uses m_iMaxHealth on Prop_Data, not m_iHealth on Prop_Send
		int whp = 0;
		if (HasEntProp(ent, Prop_Send, "m_iHealth"))
			whp = GetEntProp(ent, Prop_Send, "m_iHealth");
		else if (HasEntProp(ent, Prop_Data, "m_iMaxHealth"))
			whp = GetEntProp(ent, Prop_Data, "m_iMaxHealth");
		float wp[3]; GetEntPropVector(ent, Prop_Data, "m_vecAbsOrigin", wp);
		float dist = GetVectorDistance(pos, wp);
		bool witchAngry = false;
		if (HasEntProp(ent, Prop_Send, "m_rage"))
		{
			float rage = GetEntPropFloat(ent, Prop_Send, "m_rage");
			if (rage > 0.0) witchAngry = true;
		}
		if (wc > 0) Format(witches, sizeof(witches), "%s,", witches);
		Format(witches, sizeof(witches), "%s{\"hp\":%d,\"dist\":%.0f,\"angry\":%s}", witches, whp, dist, witchAngry?"true":"false");
		wc++;
	}

	// Terrain
	char terrain[128];
	Format(terrain, sizeof(terrain), "\"narrow\":%s,\"ledge\":%s,\"alarm_car\":%s,\"finale\":%s",
		g_bLLM_HasNarrowPassage?"true":"false", g_bLLM_HasLedges?"true":"false",
		g_bLLM_HasAlarmCars?"true":"false", g_bLLM_IsFinale?"true":"false");

	// Events (with TTL: skip events older than LLM_EVENT_TTL)
	char events[256]="";
	int ec = 0;
	float nowTime = GetGameTime();
	for (int i = 0; i < g_iLLM_EventCount; i++)
	{
		int idx = (g_iLLM_EventIndex - g_iLLM_EventCount + i + LLM_RECENT_EVENTS_MAX) % LLM_RECENT_EVENTS_MAX;
		if (nowTime - g_fLLM_EventTime[idx] > LLM_EVENT_TTL) continue;  // 过期事件跳过
		if (ec > 0) Format(events, sizeof(events), "%s,", events);
		Format(events, sizeof(events), "%s\"%s\"", events, g_sLLM_RecentEvents[idx]);
		ec++;
	}

	// Items (nearby pickup items within 500 units)
	char items[512]="";
	int ic = 0;
	char itemClasses[][] = {
		"weapon_first_aid_kit", "weapon_pain_pills", "weapon_adrenaline",
		"weapon_defibrillator", "weapon_pipe_bomb", "weapon_molotov",
		"weapon_vomitjar", "weapon_rifle", "weapon_shotgun",
		"weapon_smg", "weapon_hunting_rifle", "weapon_sniper_military",
		"weapon_rifle_ak47", "weapon_rifle_desert", "weapon_rifle_sg552",
		"weapon_shotgun_chrome", "weapon_pumpshotgun", "weapon_autoshotgun",
		"weapon_shotgun_spas", "weapon_smg_silenced", "weapon_smg_mp5",
		"weapon_grenade_launcher", "weapon_rifle_m60", "weapon_melee",
		"weapon_pistol", "weapon_pistol_magnum", "weapon_chainsaw"
	};
	for (int cls = 0; cls < sizeof(itemClasses) && ic < 8; cls++)
	{
		int itemEnt = -1;
		while ((itemEnt = FindEntityByClassname(itemEnt, itemClasses[cls])) != -1)
		{
			float ip[3]; GetEntPropVector(itemEnt, Prop_Data, "m_vecAbsOrigin", ip);
			float idist = GetVectorDistance(pos, ip);
			if (idist > 500.0) continue;
			if (ic > 0) Format(items, sizeof(items), "%s,", items);
			Format(items, sizeof(items), "%s{\"name\":\"%s\",\"dist\":%.0f}", items, itemClasses[cls], idist);
			ic++;
			if (ic >= 8) break;
		}
	}

	// Common infected count (300 units)
	int commonCount = 0;
	int commonEnt = -1;
	while ((commonEnt = FindEntityByClassname(commonEnt, "infected")) != -1 && commonCount < 30)
	{
		float cp[3]; GetEntPropVector(commonEnt, Prop_Data, "m_vecAbsOrigin", cp);
		if (GetVectorDistance(pos, cp) <= 300.0) commonCount++;
	}

	// Fire areas (200 units)
	char fireAreas[256]="";
	int fc = 0;
	int fireEnt = -1;
	while ((fireEnt = FindEntityByClassname(fireEnt, "inferno")) != -1 && fc < 4)
	{
		float fp[3]; GetEntPropVector(fireEnt, Prop_Data, "m_vecAbsOrigin", fp);
		float fdist = GetVectorDistance(pos, fp);
		if (fdist <= 200.0)
		{
			if (fc > 0) Format(fireAreas, sizeof(fireAreas), "%s,", fireAreas);
			Format(fireAreas, sizeof(fireAreas), "%s{\"dist\":%.0f}", fireAreas, fdist);
			fc++;
		}
	}

	// Acid areas (200 units)
	char acidAreas[256]="";
	int ac = 0;
	int acidEnt = -1;
	while ((acidEnt = FindEntityByClassname(acidEnt, "insect_swarm")) != -1 && ac < 4)
	{
		float ap[3]; GetEntPropVector(acidEnt, Prop_Data, "m_vecAbsOrigin", ap);
		float adist = GetVectorDistance(pos, ap);
		if (adist <= 200.0)
		{
			if (ac > 0) Format(acidAreas, sizeof(acidAreas), "%s,", acidAreas);
			Format(acidAreas, sizeof(acidAreas), "%s{\"dist\":%.0f}", acidAreas, adist);
			ac++;
		}
	}

	// Server config
	bool friendlyFire = false;
	ConVar hFF = FindConVar("sv_friendly_fire");
	if (hFF != null && hFF.BoolValue) friendlyFire = true;

	// Build JSON (with seq + flow)
	g_iLLM_Seq++;
	float flowDist = L4D2Direct_GetFlowDistance(bot);
	Format(buffer, maxlen, "STATE {\"seq\":%d,\"map\":\"%s\",\"mode\":\"%s\",\"difficulty\":\"%s\",", g_iLLM_Seq, mapName, gameMode, difficulty);
	Format(buffer, maxlen, "%s\"bot\":{\"name\":\"%s\",\"hp\":%d,\"temp_hp\":%.0f,\"incap\":%s,\"bw\":%s,\"pos\":[%.0f,%.0f,%.0f],\"angles\":[%.1f,%.1f],\"flow\":%.0f,", buffer, botName, health, tempHealth, incap?"true":"false", bw?"true":"false", pos[0], pos[1], pos[2], angles[1], angles[0], flowDist);
	Format(buffer, maxlen, "%s\"weapons\":{\"primary\":\"%s\",\"secondary\":\"%s\",\"grenade\":\"%s\",\"health\":\"%s\",\"pills\":\"%s\"},\"ammo\":%d,\"reserve\":%d},", buffer, primary, secondary, grenade, healthItem, pillsItem, primaryAmmo, reserveAmmo);
	Format(buffer, maxlen, "%s\"teammates\":[%s],\"threats\":[%s],\"witches\":[%s],", buffer, mates, threats, witches);
	// Player scores (all alive survivors)
	char scores[1024]="";
	int sc = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsClientSurvivor(i) || !IsPlayerAlive(i)) continue;
		// Pin edge detection: count transition from not-pinned to pinned
		bool nowPinned = LLM_IsPinned(i);
		if (nowPinned && !g_bLLM_WasPinned[i])
			g_iLLM_PlayerPinned[i]++;
		g_bLLM_WasPinned[i] = nowPinned;
		char sn[64]; GetClientName(i, sn, sizeof(sn));
		if (sc > 0) Format(scores, sizeof(scores), "%s,", scores);
		Format(scores, sizeof(scores), "%s{\"name\":\"%s\",\"incap\":%d,\"pinned\":%d,\"revive\":%d,\"heal\":%d,\"si_kills\":%d}",
			scores, sn, g_iLLM_PlayerIncap[i], g_iLLM_PlayerPinned[i], g_iLLM_PlayerRevive[i], g_iLLM_PlayerHeal[i], g_iLLM_PlayerSIKills[i]);
		sc++;
	}

	Format(buffer, maxlen, "%s\"common_count\":%d,\"terrain\":{%s},\"fire_areas\":[%s],\"acid_areas\":[%s],\"events\":[%s],\"items\":[%s],\"server\":{\"ff\":%s},\"player_scores\":[%s],\"action\":\"%s\"}\n", buffer, commonCount, terrain, fireAreas, acidAreas, events, items, friendlyFire?"true":"false", scores, g_sLLM_Action);
}

bool LLM_IsPinned(int client)
{
	if (GetEntPropEnt(client, Prop_Send, "m_jockeyAttacker") != -1) return true;
	if (GetEntPropEnt(client, Prop_Send, "m_pummelAttacker") != -1) return true;
	if (GetEntPropEnt(client, Prop_Send, "m_tongueOwner") != -1) return true;
	if (GetEntPropEnt(client, Prop_Send, "m_pounceAttacker") != -1) return true;
	if (GetEntPropEnt(client, Prop_Send, "m_carryAttacker") != -1) return true;
	return false;
}

// Returns the pin type string for a pinned client, or empty string if not pinned
void LLM_GetPinType(int client, char[] pinType, int maxlen)
{
	pinType[0] = '\0';
	int attacker;
	attacker = GetEntPropEnt(client, Prop_Send, "m_jockeyAttacker");
	if (attacker != -1) { strcopy(pinType, maxlen, "jockey"); return; }
	attacker = GetEntPropEnt(client, Prop_Send, "m_pummelAttacker");
	if (attacker != -1) { strcopy(pinType, maxlen, "charger"); return; }
	attacker = GetEntPropEnt(client, Prop_Send, "m_tongueOwner");
	if (attacker != -1) { strcopy(pinType, maxlen, "smoker"); return; }
	attacker = GetEntPropEnt(client, Prop_Send, "m_pounceAttacker");
	if (attacker != -1) { strcopy(pinType, maxlen, "hunter"); return; }
	attacker = GetEntPropEnt(client, Prop_Send, "m_carryAttacker");
	if (attacker != -1) { strcopy(pinType, maxlen, "charger"); return; }
}

int LLM_FindNearestWitch(int bot)
{
	float botPos[3]; GetClientAbsOrigin(bot, botPos);
	float bestDist = 99999.0;
	int bestWitch = 0;

	int ent = FindEntityByClassname(-1, "witch");
	while (ent != -1)
	{
		if (IsValidEntity(ent))
		{
			float wp[3]; GetEntPropVector(ent, Prop_Send, "m_vecOrigin", wp);
			float dist = GetVectorDistance(botPos, wp);
			if (dist < bestDist) { bestDist = dist; bestWitch = ent; }
		}
		ent = FindEntityByClassname(ent, "witch");
	}
	return bestWitch;
}

// ===================== Decision Parsing =====================

void LLM_ParseDecision(const char[] data, int size)
{
	char buf[1024];
	strcopy(buf, sizeof(buf), data);

	int pos = StrContains(buf, "\"action\"");
	if (pos == -1) return;

	char action[32];
	int start = pos + 9;
	while (start < size && (buf[start]==' '||buf[start]==':'||buf[start]=='"'||buf[start]=='\''))
		start++;
	int end = start;
	while (end < size && buf[end]!='"'&&buf[end]!='\''&&buf[end]!=','&&buf[end]!='}'&&buf[end]!='\n')
		end++;
	if (end <= start) return;
	int len = end - start;
	if (len >= sizeof(action)) len = sizeof(action) - 1;
	strcopy(action, len + 1, buf[start]);

	bool changed = strcmp(g_sLLM_Action, action) != 0;
	strcopy(g_sLLM_Action, sizeof(g_sLLM_Action), action);
	g_fLLM_ActionExpire = GetGameTime() + 5.0;
	g_bLLM_ActionActive = true;

	// Parse target
	g_sLLM_Target[0] = '\0';
	int tp = StrContains(buf, "\"target\"");
	if (tp != -1)
	{
		int ts = tp + 9;
		while (ts < size && (buf[ts]==' '||buf[ts]==':'||buf[ts]=='"'||buf[ts]=='\'')) ts++;
		int te = ts;
		while (te < size && buf[te]!='"'&&buf[te]!='\''&&buf[te]!=','&&buf[te]!='}') te++;
		int tlen = te - ts;
		if (tlen >= sizeof(g_sLLM_Target)) tlen = sizeof(g_sLLM_Target) - 1;
		if (tlen > 0) strcopy(g_sLLM_Target, tlen + 1, buf[ts]);
	}

	// Apply strategic override to IB internals
	if (changed) LLM_ApplyStrategy(action);

	// Parse next_interval from LLM response and adjust send frequency
	int ni = StrContains(buf, "\"next_interval\"");
	if (ni != -1)
	{
		int ns = ni + 15;
		while (ns < size && (buf[ns]==' '||buf[ns]==':'||buf[ns]=='"')) ns++;
		char nval[16]; int nv = 0;
		while (ns < size && buf[ns]>='0' && buf[ns]<='9' && nv < 15) { nval[nv] = buf[ns]; nv++; ns++; }
		if (buf[ns]=='.') { if (nv < 15) { nval[nv] = '.'; nv++; } ns++; }
		while (ns < size && buf[ns]>='0' && buf[ns]<='9' && nv < 15) { nval[nv] = buf[ns]; nv++; ns++; }
		nval[nv] = '\0';
		float nextInt = StringToFloat(nval);
		PrintToServer("[LLM] next_interval parsed: '%s' = %.1f", nval, nextInt);
		if (nextInt >= 1.0 && nextInt <= 10.0)
		{
			g_hCvar_LLM_Interval.SetFloat(nextInt);
			PrintToServer("[LLM] interval set to %.1f", nextInt);
		}
	}

	// Parse seq from response for request-response matching
	int si = StrContains(buf, "\"seq\"");
	if (si != -1)
	{
		int ss = si + 5;
		while (ss < size && (buf[ss]==' '||buf[ss]==':'||buf[ss]=='"')) ss++;
		char sval[16]; int sv = 0;
		while (ss < size && buf[ss]>='0' && buf[ss]<='9' && sv < 15) { sval[sv] = buf[ss]; sv++; ss++; }
		sval[sv] = '\0';
		int respSeq = StringToInt(sval);
		if (respSeq > 0)
			PrintToServer("[LLM] seq matched: sent=%d, resp=%d %s", g_iLLM_Seq, respSeq, respSeq==g_iLLM_Seq ? "OK" : "MISMATCH");
	}

	PrintToServer("[LLM] Decision: %s target=%s", g_sLLM_Action, g_sLLM_Target);
}

// ===================== Strategy -> IB Internal Override =====================

/**
 * Core integration: LLM decision directly sets IB's internal target/move variables.
 * This is the key advantage of integrating inside IB rather than external plugin.
 */
// Clear IB's current move command so SetMoveToPosition won't reject our override
void LLM_ClearMoveOverride(int bot)
{
	g_iBot_MovePos_Priority[bot] = -1;
	g_fBot_MovePos_Duration[bot] = GetGameTime();
	g_fBot_MovePos_Tolerance[bot] = -1.0;
	g_bBot_MovePos_IgnoreDamaging[bot] = false;
	g_sBot_MovePos_Name[bot][0] = 0;
	SetVectorToZero(g_fBot_MovePos_Position[bot]);
	L4D2_CommandABot(bot, 0, BOT_CMD_RESET);
}

void LLM_ApplyStrategy(const char[] action)
{
	int bot = g_iLLM_TrackedBot;
	if (bot == -1) return;

	g_bLLM_OverrideMove = false;
	g_iLLM_OverrideTarget = 0;

	if (strcmp(action, "help_teammate") == 0)
	{
		// Move toward nearest pinned/incap teammate
		// IB's own scan will handle targeting the attacker once bot is close
		int bestPin = 0;
		float bestDist = 99999.0;
		float botPos[3]; GetClientAbsOrigin(bot, botPos);

		for (int i = 1; i <= MaxClients; i++)
		{
			if (i == bot || !IsClientInGame(i) || !IsClientSurvivor(i)) continue;
			bool pinned = LLM_IsPinned(i);
			bool incap = view_as<bool>(GetEntProp(i, Prop_Send, "m_isIncapacitated"));
			if (!pinned && !incap) continue;

			float fp[3]; GetClientAbsOrigin(i, fp);
			float dist = GetVectorDistance(botPos, fp);
			if (dist < bestDist) { bestDist = dist; bestPin = i; }
		}

		if (bestPin > 0)
		{
			float fp[3]; GetClientAbsOrigin(bestPin, fp);
			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, fp, 4, "LLM_Help", 0.0, 3.0, true, true);

			// Also find attacker and set target for IB's shooting logic
			int attacker = L4D_GetPinnedInfected(bestPin);
			if (attacker > 0)
			{
				g_iBot_TargetInfected[bot] = attacker;
				g_iBot_PinnedFriend[bot] = bestPin;
				g_iBot_PinnedFriend_Attacker[bot] = attacker;
			}
		}
		else
		{
			// No currently pinned teammate (SI may have been killed since LLM decision)
			// Fall back: move toward nearest teammate to stay with the team
			int bestMate = 0;
			float bestMateDist = 99999.0;
			float matePos[3];
			float botPos2[3]; GetClientAbsOrigin(bot, botPos2);
			for (int i = 1; i <= MaxClients; i++)
			{
				if (i == bot || !IsClientInGame(i) || !IsClientSurvivor(i)) continue;
				float tp[3]; GetClientAbsOrigin(i, tp);
				float dist = GetVectorDistance(botPos2, tp);
				if (dist < bestMateDist) { bestMateDist = dist; bestMate = i; }
			}
			if (bestMate > 0)
			{
				GetClientAbsOrigin(bestMate, matePos);
				LLM_ClearMoveOverride(bot);
				SetMoveToPosition(bot, matePos, 3, "LLM_Help", 0.0, 3.0, true, true);
			}
		}
	}
	else if (strcmp(action, "attack_si") == 0 || strcmp(action, "attack_tank") == 0)
	{
		// Find nearest SI/Tank, move toward it and set as target
		int bestSI = 0;
		float bestDist = 99999.0;
		float botPos[3]; GetClientAbsOrigin(bot, botPos);

		for (int i = 1; i <= MaxClients; i++)
		{
			if (!IsClientInGame(i) || GetClientTeam(i) != 3 || !IsPlayerAlive(i)) continue;
			if (view_as<bool>(GetEntProp(i, Prop_Send, "m_isGhost"))) continue;
			if (strcmp(action, "attack_tank") == 0 && GetEntProp(i, Prop_Send, "m_zombieClass") != 8) continue;

			float sp[3]; GetClientAbsOrigin(i, sp);
			float dist = GetVectorDistance(botPos, sp);
			if (dist < bestDist) { bestDist = dist; bestSI = i; }
		}

		if (bestSI > 0)
		{
			float sp[3]; GetClientAbsOrigin(bestSI, sp);
			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, sp, 4, "LLM_Attack", 0.0, 3.0, true, true);
			g_iBot_TargetInfected[bot] = bestSI;
		}
	}
	else if (strcmp(action, "evade_threat") == 0)
	{
		// Set move override to retreat
		int threat = 0;
		float botPos[3]; GetClientAbsOrigin(bot, botPos);
		float threatPos[3];

		for (int i = 1; i <= MaxClients; i++)
		{
			if (!IsClientInGame(i) || GetClientTeam(i) != 3 || !IsPlayerAlive(i)) continue;
			float sp[3]; GetClientAbsOrigin(i, sp);
			float dist = GetVectorDistance(botPos, sp);
			if (dist < 500.0) { threat = i; threatPos = sp; break; }
		}

		if (threat > 0)
		{
			float dir[3];
			dir[0] = botPos[0] - threatPos[0];
			dir[1] = botPos[1] - threatPos[1];
			dir[2] = 0.0;
			NormalizeVector(dir, dir);
			g_fLLM_OverrideMovePos[0] = botPos[0] + dir[0] * 300.0;
			g_fLLM_OverrideMovePos[1] = botPos[1] + dir[1] * 300.0;
			g_fLLM_OverrideMovePos[2] = botPos[2];
			g_bLLM_OverrideMove = true;

			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, g_fLLM_OverrideMovePos, 4, "LLM_Evade", 0.0, 3.0, true, true);
		}
	}
	else if (strcmp(action, "follow_team") == 0)
	{
		// Reset overrides, let IB default behavior take over
	}
	else if (strcmp(action, "heal_teammate") == 0)
	{
		// Find lowest HP teammate and set IB to move toward them
		int bestTarget = 0;
		float bestHP = 999.0;
		for (int i = 1; i <= MaxClients; i++)
		{
			if (i == bot || !IsClientInGame(i) || !IsClientSurvivor(i)) continue;
			float hp = float(GetClientHealth(i));
			if (hp < bestHP) { bestHP = hp; bestTarget = i; }
		}
		if (bestTarget > 0 && bestHP < 80.0)
		{
			float tp[3]; GetClientAbsOrigin(bestTarget, tp);
			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, tp, 3, "LLM_Heal", 0.0, 5.0, true, true);
		}
	}
	else if (strcmp(action, "attack_witch") == 0)
	{
		// Find nearest witch and set as target
		int witchRef = LLM_FindNearestWitch(bot);
		if (witchRef > 0)
		{
			float wp[3]; GetEntPropVector(witchRef, Prop_Send, "m_vecOrigin", wp);
			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, wp, 3, "LLM_Witch", 0.0, 5.0, true, true);
		}
	}
	else if (strcmp(action, "pick_up_item") == 0)
	{
		// Let IB's built-in scavenge handle this
	}
	else if (strcmp(action, "throw_grenade") == 0)
	{
		// IB handles grenade throwing via ib_gren_enabled
	}
	else if (strcmp(action, "cover_fire") == 0 || strcmp(action, "hold_position") == 0)
	{
		// Stay near current position, engage threats - IB handles targeting
	}
	else if (strcmp(action, "give_item") == 0)
	{
		// Move toward nearest teammate that needs items
		int bestTarget = 0;
		float bestDist = 99999.0;
		float botPos[3]; GetClientAbsOrigin(bot, botPos);
		for (int i = 1; i <= MaxClients; i++)
		{
			if (i == bot || !IsClientInGame(i) || !IsClientSurvivor(i)) continue;
			float tp[3]; GetClientAbsOrigin(i, tp);
			float dist = GetVectorDistance(botPos, tp);
			if (dist < bestDist) { bestDist = dist; bestTarget = i; }
		}
		if (bestTarget > 0)
		{
			float tp[3]; GetClientAbsOrigin(bestTarget, tp);
			LLM_ClearMoveOverride(bot);
			SetMoveToPosition(bot, tp, 2, "LLM_Give", 0.0, 5.0, true, true);
		}
	}
	else if (strcmp(action, "use_defib") == 0)
	{
		// Find dead teammate body - IB's ib_defib_revive handles this
	}
}

// ===================== LLM Override in OnPlayerRunCmd =====================

/**
 * Call this from OnPlayerRunCmd AFTER SurvivorBotThink to apply LLM overrides.
 * This allows IB to do its normal processing, then LLM can fine-tune.
 */
void LLM_ApplyOverrides(int iClient, int &iButtons, float fVel[3], float fAngles[3])
{
	if (!g_hCvar_LLM_Enabled.BoolValue) return;
	if (iClient != g_iLLM_TrackedBot) return;
	if (!g_bLLM_ActionActive) return;

	// Check expiry
	if (GetGameTime() > g_fLLM_ActionExpire)
	{
		g_bLLM_ActionActive = false;
		strcopy(g_sLLM_Action, sizeof(g_sLLM_Action), "follow_team");
		return;
	}

	// Movement override (for evade_threat, etc.)
	if (g_bLLM_OverrideMove)
	{
		// IB's SetMoveToPosition handles this via L4D2_CommandABot
		g_bLLM_OverrideMove = false;
	}
}

// ===================== Terrain =====================

void LLM_LoadTerrain()
{
	g_bLLM_HasNarrowPassage = false;
	g_bLLM_HasLedges = false;
	g_bLLM_HasAlarmCars = false;
	g_bLLM_HasCrescendo = false;
	g_bLLM_IsFinale = false;

	char mapName[128]; GetCurrentMap(mapName, sizeof(mapName));
	int len = strlen(mapName);
	if (len >= 2 && mapName[len-1] == '5') g_bLLM_IsFinale = true;
	if (StrContains(mapName, "c1m") == 0) { g_bLLM_HasNarrowPassage = true; g_bLLM_HasLedges = true; }
	if (StrContains(mapName, "c2m") == 0) g_bLLM_HasAlarmCars = true;
	if (StrContains(mapName, "c3m") == 0) g_bLLM_HasAlarmCars = true;
	if (StrContains(mapName, "c5m") == 0) g_bLLM_HasLedges = true;

	PrintToServer("[LLM] Terrain: %s narrow=%d ledge=%d alarm=%d finale=%d",
		mapName, g_bLLM_HasNarrowPassage, g_bLLM_HasLedges, g_bLLM_HasAlarmCars, g_bLLM_IsFinale);
}

// ===================== Events =====================

void LLM_AddEvent(const char[] event)
{
	Format(g_sLLM_RecentEvents[g_iLLM_EventIndex], 128, "%s", event);
	g_fLLM_EventTime[g_iLLM_EventIndex] = GetGameTime();
	g_iLLM_EventIndex = (g_iLLM_EventIndex + 1) % LLM_RECENT_EVENTS_MAX;
	if (g_iLLM_EventCount < LLM_RECENT_EVENTS_MAX) g_iLLM_EventCount++;
}

public void LLM_EventWitchHarassed(Event event, const char[] name, bool dontBroadcast) { LLM_AddEvent("witch_angry"); }
public void LLM_EventTankSpawn(Event event, const char[] name, bool dontBroadcast) { LLM_AddEvent("tank_spawn"); }

public void LLM_EventChokeStart(Event event, const char[] name, bool dontBroadcast)
{
	int v = GetClientOfUserId(event.GetInt("victim"));
	if (v > 0 && v != g_iLLM_TrackedBot) { char n[64]; GetClientName(v,n,sizeof(n)); char b[128]; Format(b,sizeof(b),"smoker_grab_%s",n); LLM_AddEvent(b); }
}
public void LLM_EventLungePounce(Event event, const char[] name, bool dontBroadcast)
{
	int v = GetClientOfUserId(event.GetInt("victim"));
	if (v > 0 && v != g_iLLM_TrackedBot) { char n[64]; GetClientName(v,n,sizeof(n)); char b[128]; Format(b,sizeof(b),"hunter_pounce_%s",n); LLM_AddEvent(b); }
}
public void LLM_EventJockeyRide(Event event, const char[] name, bool dontBroadcast)
{
	int v = GetClientOfUserId(event.GetInt("victim"));
	if (v > 0 && v != g_iLLM_TrackedBot) { char n[64]; GetClientName(v,n,sizeof(n)); char b[128]; Format(b,sizeof(b),"jockey_ride_%s",n); LLM_AddEvent(b); }
}
public void LLM_EventChargerPummel(Event event, const char[] name, bool dontBroadcast)
{
	int v = GetClientOfUserId(event.GetInt("victim"));
	if (v > 0 && v != g_iLLM_TrackedBot) { char n[64]; GetClientName(v,n,sizeof(n)); char b[128]; Format(b,sizeof(b),"charger_pummel_%s",n); LLM_AddEvent(b); }
}

// ===================== Test Mode: Auto-spawn SI =====================

public Action LLM_TimerTestSpawn(Handle timer)
{
	if (!g_hCvar_LLM_Enabled.BoolValue || !g_hCvar_LLM_TestMode.BoolValue)
		return Plugin_Continue;

	// Count current SI
	int siCount = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 3 && IsPlayerAlive(i))
			siCount++;
	}

	// Only spawn if fewer than 3 SI alive
	if (siCount < 3)
	{
		LLM_SpawnNearSurvivor();
	}

	return Plugin_Continue;
}

void LLM_SpawnNearSurvivor()
{
	// Find a survivor bot and their position
	int survivor = -1;
	float survivorPos[3];
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 2 && IsPlayerAlive(i))
		{
			survivor = i;
			GetClientAbsOrigin(i, survivorPos);
			break;
		}
	}
	if (survivor == -1) return;

	// Pick random SI class: 1=Smoker, 2=Boomer, 3=Hunter, 4=Spitter, 5=Jockey, 6=Charger
	int zombieClass = GetRandomInt(1, 6);

	// Try to get a valid PZ spawn position using left4dhooks native
	float spawnPos[3];
	if (!L4D_GetRandomPZSpawnPosition(survivor, zombieClass, 10, spawnPos))
	{
		// Fallback: offset from survivor position
		spawnPos = survivorPos;
		spawnPos[0] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
		spawnPos[1] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
		spawnPos[2] += 10.0;
	}

	float spawnAng[3];
	spawnAng[1] = GetRandomFloat(0.0, 360.0);

	int entity = L4D2_SpawnSpecial(zombieClass, spawnPos, spawnAng);
	if (entity > 0)
		PrintToServer("[LLM] Test: spawned SI class %d at %.0f %.0f %.0f (entity %d)", zombieClass, spawnPos[0], spawnPos[1], spawnPos[2], entity);
	else
		PrintToServer("[LLM] Test: L4D2_SpawnSpecial failed for class %d", zombieClass);
}


// Admin commands
public Action Cmd_LLM_SpawnSI(int client, int args)
{
	LLM_SpawnNearSurvivor();
	ReplyToCommand(client, "[LLM] Spawned SI near survivor");
	return Plugin_Handled;
}

public Action Cmd_LLM_SpawnTank(int client, int args)
{
	int survivor = -1;
	float survivorPos[3];
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 2 && IsPlayerAlive(i))
		{
			survivor = i;
			GetClientAbsOrigin(i, survivorPos);
			break;
		}
	}
	if (survivor == -1)
	{
		ReplyToCommand(client, "[LLM] No survivor found");
		return Plugin_Handled;
	}

	float spawnPos[3];
	if (!L4D_GetRandomPZSpawnPosition(survivor, 8, 10, spawnPos))
	{
		spawnPos = survivorPos;
		spawnPos[0] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
		spawnPos[1] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
		spawnPos[2] += 10.0;
	}

	float spawnAng[3];
	spawnAng[1] = GetRandomFloat(0.0, 360.0);

	int entity = L4D2_SpawnTank(spawnPos, spawnAng);
	if (entity > 0)
		PrintToServer("[LLM] Spawned tank at %.0f %.0f %.0f (entity %d)", spawnPos[0], spawnPos[1], spawnPos[2], entity);
	else
		PrintToServer("[LLM] L4D2_SpawnTank failed");

	ReplyToCommand(client, "[LLM] Spawning tank");
	return Plugin_Handled;
}

public Action Cmd_LLM_SpawnWitch(int client, int args)
{
	int survivor = -1;
	float survivorPos[3];
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && IsFakeClient(i) && GetClientTeam(i) == 2 && IsPlayerAlive(i))
		{
			survivor = i;
			GetClientAbsOrigin(i, survivorPos);
			break;
		}
	}
	if (survivor == -1)
	{
		ReplyToCommand(client, "[LLM] No survivor found");
		return Plugin_Handled;
	}

	float spawnPos[3];
	// Witch is not a client-based SI, use position offset
	spawnPos = survivorPos;
	spawnPos[0] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
	spawnPos[1] += GetRandomFloat(300.0, 600.0) * (GetRandomInt(0, 1) ? 1.0 : -1.0);
	spawnPos[2] += 10.0;

	float spawnAng[3];
	spawnAng[1] = GetRandomFloat(0.0, 360.0);

	int entity = L4D2_SpawnWitch(spawnPos, spawnAng);
	if (entity > 0)
		PrintToServer("[LLM] Spawned witch at %.0f %.0f %.0f (entity %d)", spawnPos[0], spawnPos[1], spawnPos[2], entity);
	else
		PrintToServer("[LLM] L4D2_SpawnWitch failed");

	ReplyToCommand(client, "[LLM] Spawning witch");
	return Plugin_Handled;
}

public Action Cmd_LLM_Status(int client, int args)
{
	ReplyToCommand(client, "[LLM] Enabled: %d | Connected: %d | TrackedBot: %d | Action: %s",
		g_hCvar_LLM_Enabled.IntValue, g_bLLM_Connected, g_iLLM_TrackedBot, g_sLLM_Action);

	int survivors = 0, si = 0;
	for (int i = 1; i <= MaxClients; i++)
	{
		if (!IsClientInGame(i) || !IsFakeClient(i)) continue;
		if (GetClientTeam(i) == 2 && IsPlayerAlive(i)) survivors++;
		if (GetClientTeam(i) == 3 && IsPlayerAlive(i)) si++;
	}
	ReplyToCommand(client, "[LLM] Survivors: %d | SI alive: %d | TestMode: %d",
		survivors, si, g_hCvar_LLM_TestMode.IntValue);
	return Plugin_Handled;
}
