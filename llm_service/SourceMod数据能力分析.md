# L4D2 SourceMod 数据能力全面分析

> **版本**: v1.0
> **日期**: 2026-04-22
> **范围**: l4d2-server-test 容器 SourceMod 插件生态 + IB 插件数据能力深度排查
> **目的**: 明确当前可获取/已获取/未获取的游戏数据，指导沙盘增强和 LLM 决策优化

---

## 目录

1. [服务器 SourceMod 插件生态概览](#1-服务器-sourcemod-插件生态概览)
2. [IB 插件 STATE 数据能力分析](#2-ib-插件-state-数据能力分析)
3. [关键发现：已获取但未发送的数据](#3-关键发现已获取但未发送的数据)
4. [Left4DHooks 原生能力：已用 vs 未用](#4-left4dhooks-原生能力已用-vs-未用)
5. [IB 插件内部状态变量](#5-ib-插件内部状态变量)
6. [实体属性完整清单](#6-实体属性完整清单)
7. [其他插件可提供的数据](#7-其他插件可提供的数据)
8. [数据增强优先级建议](#8-数据增强优先级建议)

---

## 1. 服务器 SourceMod 插件生态概览

### 1.1 核心扩展 (Extensions)

| 扩展 | 能力 | LLM 可利用 |
|:---|:---|:---|
| **left4dhooks** | 274 个 Native + 224 个 Forward，L4D2 游戏函数完整 Hook | 核心依赖 |
| **socket** | TCP/UDP 网络通信 | LLM 通信通道 |
| **dhooks** | 动态函数 Hook (Dynamic Hooks) | 虚函数调用拦截 |
| **steamworks** | Steam API 接入 | 玩家 Steam 信息 |
| **system2** | 系统命令 + HTTP 请求 | 可替代 socket 做 HTTP |
| **regex** | 正则表达式 | 文本解析 |
| **actions** | SourceMod Actions 系统 | Bot 行为树 |
| **profiler** | 性能分析 | 调试用 |
| **vscript** | VScript 脚本执行 | 获取游戏内部数据 |

### 1.2 自定义 L4D2 插件 (30+)

| 插件 | 关键能力 | 可为 LLM 提供的数据 |
|:---|:---|:---|
| **l4d2_sb_ai_improver.smx** | Bot AI 改进 + LLM 决策模块 | **核心插件，见第 2 节** |
| **left4dhooks.smx** | L4D2 Hook 库 | 所有 L4D2 游戏事件/状态 |
| **WeaponHandling.smx** | 武器速度/时序修改 | 武器射速/换弹速度/近战攻速 |
| **l4d2_skill_detect.smx** | 技能动作检测 | Skeet/Crown/Level/Deathcharge 等高阶操作检测 |
| **l4d2_points_system.smx** | 积分商城系统 | 玩家积分/购买记录/经济状态 |
| **l4d2_simulation.smx** | 个人难度模拟 | 每人独立难度值 |
| **l4d2_damage_show.smx** | 伤害数字显示 | 伤害量/爆头/距离 |
| **l4d2_item_hint.smx** | 物品标记系统 | 物品位置/类型/玩家标记 |
| **l4d2_infected_hp.smx** | 特感 HP 显示 | SI 精确血量 |
| **l4d2_nativevote.smx** | 原生投票 API | 游戏内投票系统 |
| **Defib_Fix.smx** | 除颤器修复 | 死亡模型位置 |
| **LMCCore.smx** | 模型替换 | 模型覆盖信息 |
| **ThirdPersonShoulder_Detect.smx** | 第三人称检测 | 玩家视角模式 |
| **Charger_Collision_patch.smx** | Charger 碰撞修复 | Charger 冲锋状态 |
| **Witch_Target_patch.smx** | Witch 目标修复 | Witch 目标选择 |
| **l4d2_survivorai_triggerfix.smx** | Bot AI 触发器修复 | 触发器交互修正 |

### 1.3 SourceMod 基础插件 (16 个)

admin-flatfile, adminhelp, adminmenu, antiflood, basebans, basechat, basecomm, basecommands, basetriggers, basevotes, clientprefs, funcommands, funvotes, playercommands, reservedslots, sounds — 均为管理功能，与 LLM 无关。

---

## 2. IB 插件 STATE 数据能力分析

### 2.1 当前 STATE JSON 完整字段

来源: `l4d2_sb_ai_improver.sp` 的 `LLM_CollectState()` 函数 (Line 7750-7996)

```
STATE JSON 结构:
├── seq               // g_iLLM_Seq++ 递增序号
├── map               // GetCurrentMap() 当前地图
├── mode              // mp_gamemode ConVar 游戏模式
├── difficulty         // z_difficulty ConVar 难度
├── bot               // Bot 自身状态
│   ├── name          // GetClientName
│   ├── hp            // GetClientHealth
│   ├── temp_hp       // m_healthBuffer
│   ├── incap         // m_isIncapacitated
│   ├── bw            // m_bIsOnThirdStrike (黑白)
│   ├── pos[3]        // GetClientAbsOrigin [x,y,z] ✅ 有坐标
│   ├── angles[2]     // GetClientEyeAngles [yaw,pitch] ✅ 有朝向
│   ├── flow          // L4D2Direct_GetFlowDistance 地图进度
│   ├── pinned        // LLM_IsPinned 是否被控
│   ├── pin_type      // LLM_GetPinType 被控类型
│   ├── weapons       // 武器槽位
│   │   ├── primary   // GetPlayerWeaponSlot(0)
│   │   ├── secondary // GetPlayerWeaponSlot(1)
│   │   ├── grenade   // GetPlayerWeaponSlot(2)
│   │   ├── health    // GetPlayerWeaponSlot(3)
│   │   └── pills     // GetPlayerWeaponSlot(4)
│   ├── ammo          // m_iClip1 当前弹夹
│   └── reserve       // m_iExtra1 备弹
├── teammates[]       // 队友数组
│   ├── name
│   ├── hp
│   ├── temp_hp
│   ├── incap
│   ├── bw
│   ├── pinned
│   ├── pin_type
│   ├── weapon        // 仅主武器
│   └── dist          // ⚠️ 仅有距离，无坐标/方向
├── threats[]         // 特感数组
│   ├── type          // m_zombieClass 映射名称
│   ├── hp
│   ├── dist          // ⚠️ 仅有距离，无坐标/方向
│   └── ghost         // m_isGhost
├── witches[]         // 女巫数组
│   ├── hp
│   ├── dist          // ⚠️ 仅有距离，无坐标/方向
│   └── angry         // m_rage > 0.0 的布尔值
├── common_count      // 300单位内普通感染数
├── terrain           // 地形标记(硬编码)
│   ├── narrow
│   ├── ledge
│   ├── alarm_car
│   ├── crescendo
│   └── finale
├── fire_areas[]      // 火焰区域
│   └── dist          // ⚠️ 仅有距离
├── acid_areas[]      // 酸液区域
│   └── dist          // ⚠️ 仅有距离
├── events[]          // 最近事件 (5秒TTL)
├── items[]           // 500单位内物品
│   ├── name
│   └── dist          // ⚠️ 仅有距离，无坐标
├── server
│   └── ff            // sv_friendly_fire
├── player_scores[]   // 评分数据
│   ├── name
│   ├── incap
│   ├── pinned
│   ├── revive
│   ├── heal
│   └── si_kills
└── action            // 当前 LLM 决策
```

### 2.2 数据获取 vs 数据发送 对照表

| 实体类型 | 获取了 3D 坐标 | 发送了坐标 | 发送了方向 | 发送了距离 | 丢失信息 |
|:---|:---|:---|:---|:---|:---|
| Bot 自身 | ✅ `GetClientAbsOrigin` | ✅ `pos[3]` | ✅ `angles[2]` | - | - |
| 队友 | ✅ `GetClientAbsOrigin(i, mp)` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |
| 特感 SI | ✅ `GetClientAbsOrigin(i, tp)` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |
| 女巫 | ✅ `GetEntPropVector("m_vecAbsOrigin")` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |
| 物品 | ✅ `GetEntPropVector("m_vecAbsOrigin")` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |
| 火焰区 | ✅ `GetEntPropVector("m_vecAbsOrigin")` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |
| 酸液区 | ✅ `GetEntPropVector("m_vecAbsOrigin")` | ❌ | ❌ | ✅ `dist` | **完整 3D 位置** |

---

## 3. 关键发现：已获取但未发送的数据

### 3.1 🔴 最高优先级：实体 3D 坐标

**现状**: `LLM_CollectState()` 对所有实体都调用了 `GetClientAbsOrigin()` 或 `GetEntPropVector("m_vecAbsOrigin")` 获取坐标，然后仅计算 `GetVectorDistance()` 发送距离，坐标被丢弃。

**影响**:
- LLM 无法区分威胁在左/右/前/后
- 沙盘无法精确标注实体位置（只能环形排列）
- 无法计算实体之间的相对方向关系
- 无法判断威胁是否在 Bot 视线方向

**修复难度**: 低 — 只需在 JSON Format 中加入坐标值

**修改位置** (Line 7805, 7828, 7854, 7900):
```
// 队友: 加入 pos 字段
"{\"name\":\"%s\",\"hp\":%d,...,\"dist\":%.0f,\"pos\":[%.0f,%.0f,%.0f]}"
// mp[0], mp[1], mp[2] 已在 Line 7796 获取

// 特感: 加入 pos 字段
"{\"type\":\"%s\",\"hp\":%d,\"dist\":%.0f,\"ghost\":%s,\"pos\":[%.0f,%.0f,%.0f]}"
// tp[0], tp[1], tp[2] 已在 Line 7823 获取

// 女巫: 加入 pos 字段
"{\"hp\":%d,\"dist\":%.0f,\"angry\":%s,\"pos\":[%.0f,%.0f,%.0f]}"
// wp[0], wp[1], wp[2] 已在 Line 7845 获取

// 物品: 加入 pos 字段
"{\"name\":\"%s\",\"dist\":%.0f,\"pos\":[%.0f,%.0f,%.0f]}"
// ip[0], ip[1], ip[2] 已在 Line 7896 获取
```

### 3.2 🟠 高优先级：Bot 移动目标与指令

**现状**: Bot 内部有完整的移动目标信息，但完全未发送。

| 变量 | 类型 | 含义 | 当前发送 |
|:---|:---|:---|:---|
| `g_fBot_MovePos_Position[3]` | float[3] | 移动目标坐标 | ❌ |
| `g_sBot_MovePos_Name[64]` | char[64] | 移动指令名 | ❌ |
| `g_iBot_MovePos_Priority` | int | 移动优先级 | ❌ |
| `g_fBot_MovePos_Tolerance` | float | 到达容差 | ❌ |
| `g_bBot_MovePos_IgnoreDamaging` | bool | 是否忽略危险区域 | ❌ |

`g_sBot_MovePos_Name` 的可能值:
"EscapeInferno", "EvadeCharge", "GoToWitch", "TakeCover", "ScavengeItem", "DefibPlayer", "HealTeammate" 等

**价值**: 
- 沙盘可显示 Bot 正在前往的目标点（路径终点）
- LLM 可理解 Bot 当前的底层行为意图
- 可在沙盘上画出 Bot 的移动路线

### 3.3 🟠 高优先级：Bot 看向位置与战斗状态

| 变量 | 类型 | 含义 | 当前发送 |
|:---|:---|:---|:---|
| `g_fBot_LookPosition[3]` | float[3] | Bot 看向的坐标 | ❌ |
| `g_bBot_IsInCombat` | bool | 是否战斗中 | ❌ |
| `g_bBot_IsAvailable` | bool | 是否可用(非忙) | ❌ |
| `g_iBot_TargetInfected` | int | 当前攻击的特感实体 | ❌ |
| `g_bClient_IsFiringWeapon` | bool | 是否正在射击 | ❌ |

**价值**: 
- `LookPosition` 在沙盘上显示 Bot 瞄准方向
- `IsInCombat` 帮助 LLM 判断当前状态
- `TargetInfected` 可获取目标特感的坐标，在沙盘上连线

### 3.4 🟡 中优先级：SI 能力状态

**现状**: SI 发送了 `type/hp/dist/ghost`，但缺少能力释放状态。

| 属性 | 含义 | 当前发送 |
|:---|:---|:---|
| `m_isSpraying` (Boomer) | 正在呕吐 | ❌ |
| `m_isLunging` (Hunter) | 正在扑击 | ❌ |
| `m_isLeaping` (Jockey) | 正在跳跃 | ❌ |
| `m_isCharging` (Charger) | 正在冲锋 | ❌ |
| `m_tongueState` (Smoker) | 舌头状态 | ❌ |

可通过 `IsUsingSpecialAbility()` (Line 3920-3940) 获取。

**价值**: LLM 可判断 SI 是否已释放能力（如 Charger 冲锋中可安全绕后）

### 3.5 🟡 中优先级：女巫愤怒值

**现状**: 获取了 `m_rage` 浮点值但降级为布尔值 `angry`。

| 属性 | 实际获取 | 当前发送 |
|:---|:---|:---|
| `m_rage` | `GetEntPropFloat(ent, Prop_Send, "m_rage")` | `angry: rage > 0.0` (布尔) |

**价值**: 女巫愤怒值 0-1 变化表示即将暴怒，LLM 可提前决策避让

### 3.6 🟡 中优先级：导演系统精确数据

| 数据 | 获取方式 | 当前发送 |
|:---|:---|:---|
| `L4D2Direct_GetPendingMobCount()` | 返回待生成尸潮数量 | 降级为 "peak"/"build" 标签 |
| `L4D2Direct_GetMapMaxFlowDistance()` | 地图最大 Flow 距离 | ❌ 完全未获取 |
| `L4D2Direct_GetVSTankFlowPercent()` | Tank 生成进度 | ❌ |
| `L4D2Direct_GetVSWitchToSpawn()` | 是否将生成女巫 | ❌ |

**价值**: 
- `PendingMobCount` 精确判断尸潮规模
- `MapMaxFlowDistance` 可计算 Bot 地图进度百分比
- Tank/Witch 生成预测可帮助 LLM 提前决策

### 3.7 🟢 低优先级：队友完整装备与状态

| 数据 | 获取方式 | 当前发送 |
|:---|:---|:---|
| 队友副武器 | `GetPlayerWeaponSlot(i, 1)` | ❌ 仅主武器 |
| 队友投掷物 | `GetPlayerWeaponSlot(i, 2)` | ❌ |
| 队友医疗品 | `GetPlayerWeaponSlot(i, 3)` | ❌ |
| 队友止痛药 | `GetPlayerWeaponSlot(i, 4)` | ❌ |
| 队友 Flow | `L4D2Direct_GetFlowDistance(i)` | ❌ |
| 队友位置 | `GetClientAbsOrigin(i)` | ❌ 仅 dist |

### 3.8 🟢 低优先级：武器详细状态

| 数据 | 获取方式 | 当前发送 |
|:---|:---|:---|
| 换弹状态 | `IsWeaponReloading()` (Line 6622) | ❌ |
| 下次开火时间 | `m_flNextPrimaryAttack` (Line 6638) | ❌ |
| 武器升级 | `m_upgradeBitVec` (激光/燃烧/爆炸弹) | ❌ |
| 双持状态 | `m_isDualWielding` | ❌ |
| 武器等级 | `GetWeaponTier()` 1/2/3 | ❌ |

---

## 4. Left4DHooks 原生能力：已用 vs 未用

### 4.1 LLM_CollectState 已使用的 Native

| Native | 用途 | 行号 |
|:---|:---|:---|
| `L4D2Direct_GetFlowDistance(client)` | bot.flow | 7958 |
| `L4D2Direct_GetPendingMobCount()` | 导演阶段判断 | 7966 |

**仅 2 个！** 274 个 Left4DHooks Native 中仅用了 2 个在 STATE 采集中。

### 4.2 IB 插件核心使用的 Native（但未发送给 LLM）

| Native | 用途 | 行号 | LLM 可用价值 |
|:---|:---|:---|:---|
| `L4D_GetNearestNavArea()` | 获取最近导航区域 | 1844,1892,3051,... | 当前导航区域信息 |
| `L4D2_NavAreaBuildPath()` | 构建导航路径 | 3249,7399 | **Bot 到目标的路径** |
| `L4D2_NavAreaTravelDistance()` | 计算导航距离 | 7053,7114 | 更精确的距离 |
| `L4D2_IsReachable()` | 判断位置可达性 | 6966,7048 | 目标是否可到达 |
| `L4D_GetPinnedSurvivor(infected)` | 获取被控生还者 | 2838,2869 | 哪个队友被哪个 SI 控 |
| `L4D_IsPlayerBoomerBiled()` | 被胆汁覆盖 | 3947,4009 | 队友视野状态 |
| `L4D_IsPlayerHangingFromLedge()` | 悬挂边缘 | 2478 | 需要救援的队友 |
| `L4D2_GetVScriptOutput()` | VScript 输出 | 2051,3520 | 游戏内部数据 |
| `L4D_GetPlayerCustomAbility()` | SI 自定义能力 | 2625,3922 | SI 能力冷却 |
| `L4D_IsAnySurvivorInCheckpoint()` | 是否在安全屋 | 2838 | 安全屋判断 |

### 4.3 完全未使用的有价值 Native

| Native | 价值 | 实现难度 |
|:---|:---|:---|
| `L4D2Direct_GetMapMaxFlowDistance()` | 地图进度百分比 = flow/maxFlow | 极低 |
| `L4D2Direct_GetVSTankFlowPercent()` | Tank 生成预测（对抗模式） | 低 |
| `L4D2Direct_GetVSWitchToSpawn()` | 女巫即将生成预测 | 低 |
| `L4D_GetCurrentChapter()` | 当前章节编号 | 极低 |
| `L4D_GetMaxMapDistance()` | 最大 Flow 距离 | 极低 |
| `L4D2_IsSurvivalMode()` | 生存模式检测 | 极低 |
| `L4D2_IsScavengeMode()` | 搜刮模式检测 | 极低 |
| `L4D_GetSurvivorSet()` | L4D1/L4D2 角色集 | 低 |
| `L4D2_GetWitchCount()` | 地图女巫总数 | 低 |

---

## 5. IB 插件内部状态变量

### 5.1 Bot 行为状态 (Line 492-562)

| 变量 | 类型 | 含义 | 发送给 LLM? | 沙盘可显示? |
|:---|:---|:---|:---|:---|
| `g_bClient_IsLookingAtPosition` | bool | 是否看向某位置 | ❌ | 瞄准方向线 |
| `g_bClient_IsFiringWeapon` | bool | 是否正在射击 | ❌ | 射击状态图标 |
| `g_bBot_ForceBash` | bool | 强制推击 | ❌ | 推击状态 |
| `g_bBot_ForceWeaponReload` | bool | 强制换弹 | ❌ | 换弹状态 |
| `g_bBot_ForceSwitchWeapon` | bool | 强制切换武器 | ❌ | 切换状态 |
| `g_bBot_WasUsingLadder` | bool | 在爬梯子 | ❌ | 梯子状态 |
| `g_bBot_IsInCombat` | bool | **战斗中** | ❌ | **战斗图标** |
| `g_bBot_IsAvailable` | bool | 是否可用 | ❌ | 状态标识 |
| `g_bBot_IsWitchHarasser` | bool | 是否在骚扰女巫 | ❌ | 行为标注 |
| `g_bBot_IsFriendNearBoomer` | bool | 队友在呕吐范围 | ❌ | 风险提示 |
| `g_bBot_IsFriendNearThrowArea` | bool | 队友在投掷范围 | ❌ | 风险提示 |
| `g_fBot_VomitBlindedTime` | float | 被胆汁致盲结束时间 | ❌ | 致盲状态 |
| `g_fBot_LookPosition[3]` | float[3] | **看向的坐标** | ❌ | **瞄准方向** |
| `g_fBot_MovePos_Position[3]` | float[3] | **移动目标坐标** | ❌ | **路径终点** |
| `g_fBot_MovePos_Duration` | float | 移动持续时间 | ❌ | - |
| `g_iBot_MovePos_Priority` | int | 移动优先级 | ❌ | 优先级标注 |
| `g_fBot_MovePos_Tolerance` | float | 到达容差 | ❌ | - |
| `g_bBot_MovePos_IgnoreDamaging` | bool | 忽略危险区域 | ❌ | - |
| `g_sBot_MovePos_Name[64]` | char[64] | **移动指令名** | ❌ | **行为标注** |
| `g_fBot_PreventFireTime` | float | 禁止开火时间 | ❌ | - |
| `g_fBot_NextPressAttackTime` | float | 下次攻击时间 | ❌ | - |
| `g_iBot_TargetInfected` | int | **当前攻击目标** | ❌ | **攻击连线** |
| `g_iBot_WitchTarget` | int | 当前女巫目标 | ❌ | 女巫攻击标注 |
| `g_iBot_DefibTarget` | int | 除颤目标 | ❌ | 除颤目标标注 |
| `g_iBot_IncapacitatedFriend` | int | 倒地队友目标 | ❌ | 救人目标标注 |
| `g_iBot_ScavengeItem` | int | 拾取目标 | ❌ | 拾取目标标注 |
| `g_iBot_NearbyFriends` | int | 附近队友数 | ❌ | - |
| `g_iBot_NearbyInfectedCount` | int | 附近感染数 | ❌ | 风险评估 |
| `g_iBot_NearestInfectedCount` | int | 最近感染数 | ❌ | 即时威胁 |
| `g_iBot_ThreatInfectedCount` | int | 威胁感染数 | ❌ | **威胁等级** |

### 5.2 导航数据 (Line 622, 1290-1388)

| 变量/偏移 | 类型 | 含义 | 发送给 LLM? |
|:---|:---|:---|:---|
| `g_iClientNavArea[MAXPLAYERS+1]` | int | 每个客户端当前导航区域 | ❌ |
| `g_iNavArea_Center` | offset | CNavArea 中心点 | ❌ |
| `g_iNavArea_Parent` | offset | CNavArea 父区域 | ❌ |
| `g_iNavArea_NWCorner` | offset | CNavArea 西北角 | ❌ |
| `g_iNavArea_SECorner` | offset | CNavArea 东南角 | ❌ |
| `g_iNavArea_DamagingTickCount` | offset | 危险区域标记 | ❌ |

---

## 6. 实体属性完整清单

### 6.1 生还者 (Survivor) 属性

| 属性 | 代码 | 类型 | 已发送? |
|:---|:---|:---|:---|
| `m_iHealth` | GetClientHealth | int | ✅ hp |
| `m_healthBuffer` | GetEntPropFloat, Prop_Send | float | ✅ temp_hp |
| `m_isIncapacitated` | GetEntProp, Prop_Send | bool | ✅ incap |
| `m_bIsOnThirdStrike` | GetEntProp, Prop_Send | bool | ✅ bw |
| `m_vecAbsOrigin` | GetClientAbsOrigin | float[3] | ✅ pos (仅Bot) |
| `m_angAbsRotation` | GetClientEyeAngles | float[3] | ✅ angles (仅Bot) |
| `m_jockeyAttacker` | GetEntPropEnt, Prop_Send | entity | ✅ (pin_type) |
| `m_tongueOwner` | GetEntPropEnt, Prop_Send | entity | ✅ (pin_type) |
| `m_pummelAttacker` | GetEntPropEnt, Prop_Send | entity | ✅ (pin_type) |
| `m_carryAttacker` | GetEntPropEnt, Prop_Send | entity | ✅ (pin_type) |
| `m_reviveTarget` | GetEntPropEnt, Prop_Send | entity | ❌ |
| `m_iCurrentUseAction` | L4D2_GetPlayerUseAction | int | ❌ |
| `m_flLaggedMovementValue` | GetEntPropFloat, Prop_Data | float | ❌ (速度倍率) |
| `m_flMaxspeed` | GetEntPropFloat, Prop_Send | float | ❌ (最大速度) |
| `m_vecVelocity` | GetEntPropVector | float[3] | ❌ (速度向量) |
| `m_fFlags` | GetEntProp | int | ❌ (地面/蹲下/跳跃) |
| `m_hActiveWeapon` | GetEntPropEnt | entity | ❌ |
| `m_iAmmo` | GetEntProp | int[] | ❌ |
| `m_upgradeBitVec` | GetEntProp | int | ❌ (激光/燃烧弹) |

### 6.2 特感 (Special Infected) 属性

| 属性 | 代码 | 类型 | 已发送? |
|:---|:---|:---|:---|
| `m_iHealth` | GetClientHealth | int | ✅ hp |
| `m_zombieClass` | GetEntProp, Prop_Send | int | ✅ type |
| `m_isGhost` | GetEntProp, Prop_Send | bool | ✅ ghost |
| `m_vecAbsOrigin` | GetClientAbsOrigin | float[3] | ❌ (仅算距离) |
| `m_isSpraying` (Boomer) | GetEntProp, Prop_Send | bool | ❌ |
| `m_isLunging` (Hunter) | GetEntProp, Prop_Send | bool | ❌ |
| `m_isLeaping` (Jockey) | GetEntProp, Prop_Send | bool | ❌ |
| `m_isCharging` (Charger) | GetEntProp, Prop_Send | bool | ❌ |
| `m_tongueState` (Smoker) | GetEntProp, Prop_Send | int | ❌ |

### 6.3 女巫 (Witch) 属性

| 属性 | 代码 | 类型 | 已发送? |
|:---|:---|:---|:---|
| `m_iHealth` | GetEntProp | int | ✅ hp |
| `m_vecAbsOrigin` | GetEntPropVector | float[3] | ❌ (仅算距离) |
| `m_rage` | GetEntPropFloat, Prop_Send | float | ❌ (降级为布尔) |
| `m_wanderrage` | GetEntPropFloat | float | ❌ |
| `m_iMaxHealth` | GetEntProp | int | ❌ |

### 6.4 物品 (Item) 属性

| 属性 | 代码 | 类型 | 已发送? |
|:---|:---|:---|:---|
| classname | GetEntityClassname | string | ✅ name |
| `m_vecAbsOrigin` | GetEntPropVector | float[3] | ❌ (仅算距离) |

---

## 7. 其他插件可提供的数据

### 7.1 l4d2_skill_detect.smx

检测高阶操作事件，可作为 LLM 额外输入：

| Forward | 含义 | LLM 价值 |
|:---|:---|:---|
| `OnSkeet` | 空中击杀 Hunter | 队友技能评估 |
| `OnSkeetMelee` | 近战 Skeet | 队友近战能力 |
| `OnCrown` | 一枪爆头 Witch | 队友精准度 |
| `OnDrawCrown` | 惊扰后爆头 Witch | 队友精准度 |
| `OnLevel` | 近战砍 Charger | 队友近战时机 |
| `OnDeathCharge` | 死亡冲锋 | Charger 威胁严重度 |
| `OnShove` | 推击 SI | 战斗节奏 |

### 7.2 WeaponHandling.smx

| Forward | 含义 | LLM 价值 |
|:---|:---|:---|
| `WH_OnMeleeSwing` | 近战挥击 | Bot 近战节奏 |
| `WH_OnReload` | 换弹 | Bot 换弹状态 |
| `WH_OnGetRateOfFire` | 射速 | 武器输出效率 |

### 7.3 l4d2_points_system.smx

| Native | 含义 | LLM 价值 |
|:---|:---|:---|
| `PS_GetPoints(client)` | 玩家积分 | 经济状态 |
| `PS_GetBought(client)` | 已购买物品 | 装备状态 |

### 7.4 l4d2_simulation.smx

| Native | 含义 | LLM 价值 |
|:---|:---|:---|
| `GetCustomizeDifficulty(client)` | 个人难度 | 每人难度差异 |

---

## 8. 数据增强优先级建议

### 8.1 立即可做 (仅需修改 JSON Format，零逻辑改动)

| # | 数据 | 修改位置 | 影响 | 工作量 |
|:---|:---|:---|:---|:---|
| 1 | **队友 pos[3]** | Line 7805 | 沙盘精确定位 | 5 分钟 |
| 2 | **特感 pos[3]** | Line 7828 | 沙盘精确定位 | 5 分钟 |
| 3 | **女巫 pos[3]** | Line 7854 | 沙盘精确定位 | 5 分钟 |
| 4 | **物品 pos[3]** | Line 7900 | 沙盘精确定位 | 5 分钟 |
| 5 | **火焰/酸液 pos[3]** | Line 7916-7945 | 沙盘精确定位 | 5 分钟 |
| 6 | **女巫 rage 值** | Line 7850 | LLM 提前预警 | 3 分钟 |
| 7 | **PendingMobCount 精确值** | Line 7966 | 尸潮规模感知 | 3 分钟 |
| 8 | **MapMaxFlowDistance** | 新增调用 | 进度百分比 | 5 分钟 |

### 8.2 短期可做 (需要少量代码，不影响现有逻辑)

| # | 数据 | 实现方式 | 价值 |
|:---|:---|:---|:---|
| 9 | **Bot 移动目标 pos+name** | 读取 g_fBot_MovePos_Position + g_sBot_MovePos_Name | 沙盘路径显示 |
| 10 | **Bot 看向位置** | 读取 g_fBot_LookPosition | 沙盘瞄准方向 |
| 11 | **Bot 战斗状态** | 读取 g_bBot_IsInCombat 等 | LLM 状态感知 |
| 12 | **Bot 攻击目标** | 读取 g_iBot_TargetInfected + 获取坐标 | 沙盘攻击连线 |
| 13 | **SI 能力状态** | IsUsingSpecialAbility() | LLM 战术判断 |
| 14 | **队友完整装备** | GetPlayerWeaponSlot(1-4) | LLM 协作决策 |
| 15 | **队友 Flow** | L4D2Direct_GetFlowDistance(i) | 队友进度 |

### 8.3 中期可做 (需要新增逻辑)

| # | 数据 | 实现方式 | 价值 |
|:---|:---|:---|:---|
| 16 | **导航路径点** | L4D2_NavAreaBuildPath + LBI_GetNavAreaCenter | 沙盘路径线 |
| 17 | **威胁方向角** | 坐标差计算 atan2 | LLM 方向感知 |
| 18 | **武器换弹/射击状态** | IsWeaponReloading() + m_flNextPrimaryAttack | LLM 战斗判断 |
| 19 | **实体视线检测** | TR_TraceRay | 是否可见 |
| 20 | **地图进度百分比** | flow / maxFlow × 100% | LLM 进度感知 |

---

*文档结束*
