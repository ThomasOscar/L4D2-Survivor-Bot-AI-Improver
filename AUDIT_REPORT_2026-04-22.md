# L4D2 LLM Bot 系统交叉审核报告

> **审核日期**: 2026-04-22
> **审核范围**: /opt/l4d2_llm_service/完整版开发文档.md vs 实际代码
> **审核方式**: 多轮深入分析、字段级交叉核对、运行时验证

---

## 一、审核发现的缺漏汇总

### 1. 文档状态标记错误（严重）

| 章节 | 文档标记 | 实际状态 | 修复 |
| :--- | :--- | :--- | :--- |
| 11. 玩家评分体系 | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 14. 地图经验学习 | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 2. 系统架构 - Redis | ⚠️ 未启用 | ✅ 已连接(升级后) | 已修复 |
| 3.3 `self.angles` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `teammates[].pin_type` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `teammates[].weapon` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `teammates[].temp_hp` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `common_count_nearby` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `fire_areas` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `acid_areas` | ❌ 未实现 | ✅ 已实现 | 已修复 |
| 3.3 `server_config` | ❌ 未实现 | ✅ 已实现(server.ff) | 已修复 |

### 2. 代码逻辑缺陷（中等）

| 问题 | 位置 | 影响 | 修复 |
| :--- | :--- | :--- | :--- |
| `player_incapacitated_start` 事件名不存在 | l4d2_sb_ai_improver.sp:804 | incap 计数永远不增加 | → `player_incapacitated` |
| revive 计数奖励被救的人 | Event_OnRevive:1737 | 评分逻辑相反 | → `userid`(救人者) |

### 3. Python 兼容性问题（中等）

| 问题 | 位置 | 影响 | 修复 |
| :--- | :--- | :--- | :--- |
| `aioredis` 不兼容 Python 3.12 | state_cache.py | Redis 无法连接 | → `redis.asyncio` |
| `aioredis` 仍在 requirements.txt | requirements.txt | 安装失败 | 已移除 |

### 4. 功能缺失（轻微）

| 问题 | 位置 | 影响 | 修复 |
| :--- | :--- | :--- | :--- |
| STATE 变化检测缺少环境字段 | main.py:78 | common_count/fire/acid 变化被忽略 | 已添加 |
| DebugServer 端口硬编码 | main.py:197 | 无法通过配置修改端口 | → config.yaml |

---

## 二、已验证的一致性项

### SourceMod 插件

- ✅ 所有 STATE JSON 字段（seq/map/mode/difficulty/bot/teammates/threats/witches/terrain/events/items/server/player_scores/action）
- ✅ LLM_GetPinType() 返回 4 种类型 + 空字符串
- ✅ LLM_IsPinned() 边缘检测 + g_bLLM_WasPinned[]
- ✅ 5 维评分数组 + 回合重置
- ✅ ROUND_OUTCOME (mission_lost→team_wipe, map_transition→escaped)
- ✅ 事件 Hook（player_incapacitated, player_death, heal_success, revive_success, defibrillator_used）

### Python 服务

- ✅ ModelRouter (priority/round_robin/adaptive)
- ✅ RuleEngine (7 条规则 + pin_type 优先级)
- ✅ PlayerScorer (五维评分 + S/A/B/C/D)
- ✅ MapExperienceStore (SQLite + Flow 分桶 + Situation Hash)
- ✅ StateCache (Redis + in-memory fallback)
- ✅ DecisionLogger ( Ring buffer + 统计)
- ✅ DebugServer (7 页面 + REST API)

---

## 三、Git 提交记录

```
1060a38 config: DebugServer端口可配置化 — 从config.yaml读取host/port
002e214 fix: 事件Hook修正 — player_incapacitated_start→player_incapacitated, revive计数改为奖励救人者(userid)
bf813ee docs: 交叉审核修复文档状态标记 + 字段对照表
8826454 fix: Python 3.12 兼容性 + STATE 变化检测增强
```

---

## 四、仍存在的已知限制

| 限制 | 说明 | 优先级 |
| :--- | :--- | :--- |
| `ib_llm_testmode` ConVar 命名 | 文档与实际代码一致（都是 `ib_` 前缀），但设计文档 v1.0 中规划的是 `llm_bot_` 前缀 | 低 |
| config.yaml game 段未使用 | collect_interval/decision_ttl/fallback_action 未被 main.py 读取，使用硬编码 | 低 |
| 向量数据库 | 设计文档规划 ChromaDB/Qdrant，实际使用 SQLite + Situation Hash | 低 |
| 多模型客户端 | 仅实现了 OpenAI 兼容接口，未实现 DeepSeek/硅基流动专有 API | 低 |
| 三层梯队/L1-L10 | 设计文档规划，未实现 | 未来 |

---

*审核完成。所有关键缺漏已修复并提交到 Git 仓库。*
