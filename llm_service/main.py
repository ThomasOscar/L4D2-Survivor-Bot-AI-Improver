#!/usr/bin/env python3
"""
L4D2 LLM Bot - Main Entry (v3.0 with Model Router + Rule Engine + State Cache)
"""

import asyncio
import logging
from logging.handlers import RotatingFileHandler
import signal
import sys
import time
import yaml
from pathlib import Path

from tcp_server import TCPServer
from llm_client import LLMClient, LLMConfig
from model_router import ModelRouter
from prompt_builder import PromptBuilder
from response_parser import ResponseParser
from rule_engine import RuleEngine
from decision_logger import DecisionLogger
from debug_server import DebugServer
from state_cache import StateCache
from player_scorer import PlayerScorer
from map_experience import MapExperienceStore

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        RotatingFileHandler(
            "/opt/l4d2_llm_service/logs/service.log",
            maxBytes=5*1024*1024,  # 5MB
            backupCount=3,
            encoding="utf-8"
        )
    ]
)
logger = logging.getLogger(__name__)

# LLM调用最小间隔（秒），防止频繁调用浪费API额度
LLM_CALL_INTERVAL = 3.0
# STATE 无变化时强制重新评估的最大间隔
FORCE_REEVAL_INTERVAL = 10.0


class LLMDecisionService:
    def __init__(self, config_path: str = "config.yaml"):
        self.config = self._load_config(config_path)
        self.tcp_server: TCPServer = None
        self.llm_client: LLMClient = None
        self.model_router: ModelRouter = None
        self.prompt_builder: PromptBuilder = None
        self.response_parser: ResponseParser = None
        self.rule_engine: RuleEngine = None
        self.decision_logger: DecisionLogger = None
        self.debug_server: DebugServer = None
        self.state_cache: StateCache = None
        self.player_scorer: PlayerScorer = None
        self.map_experience: MapExperienceStore = None
        self.last_action = None
        self.last_action_time = 0
        self.last_llm_call_time = 0
        self.last_decision = None
        self.running = False
        self.last_state = None
        self.last_state_hash = None

    def _load_config(self, path: str) -> dict:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return yaml.safe_load(f)
        except FileNotFoundError:
            logger.warning(f"Config file not found: {path}, using defaults")
            return {}

    def _state_changed(self, state: dict) -> bool:
        """Check if key STATE fields changed compared to last state."""
        if not self.last_state:
            return True
        # Threats
        threats = state.get("threats", [])
        old_threats = self.last_state.get("threats", [])
        if len(threats) != len(old_threats):
            return True
        for t, ot in zip(threats, old_threats):
            if (t.get("type") != ot.get("type") or
                t.get("ghost") != ot.get("ghost") or
                abs(t.get("dist", 0) - ot.get("dist", 0)) > 200):
                return True
        # Witches
        witches = state.get("witches", [])
        old_witches = self.last_state.get("witches", [])
        if len(witches) != len(old_witches):
            return True
        for w, ow in zip(witches, old_witches):
            if w.get("angry") != ow.get("angry"):
                return True
        # Teammates: pinned/incap/bw
        mates = state.get("teammates", [])
        old_mates = self.last_state.get("teammates", [])
        if len(mates) != len(old_mates):
            return True
        for m, om in zip(mates, old_mates):
            if (m.get("pinned") != om.get("pinned") or
                m.get("incap") != om.get("incap") or
                m.get("bw") != om.get("bw") or
                abs(m.get("hp", 0) - om.get("hp", 0)) > 20):
                return True
        # Bot: hp/incap/bw
        bot = state.get("bot", {})
        old_bot = self.last_state.get("bot", {})
        if (bot.get("hp") != old_bot.get("hp") or
            bot.get("incap") != old_bot.get("incap") or
            bot.get("bw") != old_bot.get("bw")):
            return True
        # Environment: common_count (significant change > 5)
        if abs(state.get("common_count", 0) - self.last_state.get("common_count", 0)) > 5:
            return True
        # Environment: fire_areas/acid_areas (presence change)
        if len(state.get("fire_areas", [])) != len(self.last_state.get("fire_areas", [])):
            return True
        if len(state.get("acid_areas", [])) != len(self.last_state.get("acid_areas", [])):
            return True
        return False

    def _compute_next_interval(self, state: dict, decision: dict) -> float:
        """Determine next STATE send interval based on current situation."""
        threats = state.get("threats", [])
        active_threats = [t for t in threats if not t.get("ghost", False)]
        teammates = state.get("teammates", [])
        pinned = [m for m in teammates if m.get("pinned")]
        incap = [m for m in teammates if m.get("incap")]

        if active_threats or pinned or incap:
            return 1.5  # Active danger → fast refresh
        if state.get("events"):
            return 2.5  # Recent events → medium refresh
        action = decision.get("action", "")
        if action in ("evade_threat", "hold_position"):
            return 2.0
        return 5.0  # Peaceful → slow refresh

    async def start(self):
        logger.info("Starting LLM Decision Service v3.0 (Model Router + Rule Engine + Debug)...")

        server_config = self.config.get("server", {})
        self.tcp_server = TCPServer(
            host=server_config.get("host", "0.0.0.0"),
            port=server_config.get("port", 9876)
        )

        # Initialize Model Router (supports multi-provider)
        routing_config = self.config.get("routing", {})
        # Merge models list and legacy llm config into routing config
        if "models" in self.config:
            routing_config["models"] = self.config["models"]
        if "llm" in self.config:
            routing_config["llm"] = self.config["llm"]
        self.model_router = ModelRouter(routing_config)
        await self.model_router.start()

        # Legacy llm_client no longer needed - ModelRouter handles all LLM calls
        # Keep reference for parse_response backward compat only

        self.prompt_builder = PromptBuilder(
            system_prompt_path=self.config.get("prompt", {}).get("system_prompt_file", "prompts/system.txt")
        )
        self.response_parser = ResponseParser()
        self.rule_engine = RuleEngine()
        self.decision_logger = DecisionLogger(max_history=100)

        # State cache (Redis + in-memory fallback)
        cache_cfg = self.config.get("cache", {})
        self.state_cache = StateCache(
            redis_url=cache_cfg.get("redis_url", "redis://localhost:6379"),
            enabled=cache_cfg.get("enabled", False)
        )
        await self.state_cache.connect()

        # Player scorer (five-dimensional scoring)
        self.player_scorer = PlayerScorer()

        # Map experience store (SQLite)
        self.map_experience = MapExperienceStore()
        await self.map_experience.connect()

        self.tcp_server.on_message = self._handle_state
        self.tcp_server.on_round_outcome = self._handle_round_outcome

        # Start debug HTTP server
        self.debug_server = DebugServer(
            decision_logger=self.decision_logger,
            service_ref=self,
            host="0.0.0.0",
            port=9877
        )
        await self.debug_server.start()

        self.running = True
        logger.info("Service initialized, starting TCP server...")

        await self.tcp_server.start()

    async def stop(self):
        logger.info("Stopping LLM Decision Service...")
        stats = self.decision_logger.get_stats()
        logger.info(f"Stats - Rule: {stats.get('rule',0)}, LLM: {stats.get('llm',0)}, "
                     f"Fallback: {stats.get('fallback',0)}, Skipped: {stats.get('state_unchanged_skipped',0)}")
        self.running = False
        if self.debug_server:
            await self.debug_server.stop()
        if self.tcp_server:
            await self.tcp_server.stop()
        if self.model_router:
            await self.model_router.stop()
        if self.state_cache:
            await self.state_cache.disconnect()
        if self.map_experience:
            await self.map_experience.disconnect()
        logger.info("Service stopped")

    async def _handle_state(self, state: dict) -> dict:
        """处理游戏状态：变化检测 → 规则引擎 → LLM → 后处理校验"""
        try:
            now = time.time()
            self.last_state = state
            req_seq = state.get("seq", 0)  # Extract seq for request-response matching

            # Log player score summary periodically (every 5th decision)
            if state.get("player_scores") and not hasattr(self, '_score_log_counter'):
                self._score_log_counter = 0
            if state.get("player_scores"):
                self._score_log_counter += 1
                if self._score_log_counter % 5 == 1:
                    logger.info(self.player_scorer.get_score_summary(state))

            # Step 0: STATE 变化检测（无变化 + 最近有决策 → 复用）
            state_changed = self._state_changed(state)
            if not state_changed and self.last_decision:
                if now - self.last_action_time < FORCE_REEVAL_INTERVAL:
                    self.decision_logger.log(state, self.last_decision, "skipped")
                    self.last_decision["seq"] = req_seq
                    return self.last_decision

            # Step 1: 规则引擎预判（即时，无 API 开销）
            t0 = time.time()
            rule_decision = self.rule_engine.evaluate(state)
            if rule_decision:
                elapsed = time.time() - t0
                self.decision_logger.log(state, rule_decision, "rule", elapsed)
                rule_decision["next_interval"] = self._compute_next_interval(state, rule_decision)
                rule_decision["seq"] = req_seq
                self.last_decision = rule_decision
                self.last_action = rule_decision
                self.last_action_time = now
                logger.info(f"Rule decision: {rule_decision['action']} - {rule_decision.get('reason', '')}")
                # Record to map experience
                await self.map_experience.record_decision(state, rule_decision)
                return rule_decision

            # Step 2: 节流检查（规则引擎未命中才走到这）
            if now - self.last_llm_call_time < LLM_CALL_INTERVAL:
                if self.last_decision:
                    self.last_decision["seq"] = req_seq
                    return self.last_decision
                fallback = self.response_parser.get_fallback_decision("waiting", state)
                fallback["next_interval"] = self._compute_next_interval(state, fallback)
                fallback["seq"] = req_seq
                return fallback

            self.last_llm_call_time = now

            # Step 3: 构建 Prompt 并调用 LLM (via ModelRouter)
            t0 = time.time()
            system_prompt = self.prompt_builder.get_system_prompt()

            # Inject player score hint into user prompt
            score_hint = self.player_scorer.get_decision_hint(state)
            user_prompt = self.prompt_builder.build(
                state,
                self.last_action,
                now - self.last_action_time
            )
            hints = []
            if score_hint:
                hints.append(f"[Team Assessment] {score_hint}")

            # Inject map experience hint
            past_exp = await self.map_experience.get_experience(state, limit=3)
            if past_exp:
                exp_lines = []
                for exp in past_exp:
                    exp_lines.append(
                        f"- {exp['action']}({exp['target']}): "
                        f"{exp['outcome']} x{exp['count']} (survival_rate={exp['survival_rate']})"
                    )
                hints.append("[Map Experience]\n" + "\n".join(exp_lines))

            if hints:
                user_prompt = "\n\n".join(hints) + "\n\n" + user_prompt

            response, provider_name = await self.model_router.chat(system_prompt, user_prompt, state)
            elapsed = time.time() - t0

            if response:
                content = self.model_router.parse_response(response, provider_name)
                decision = self.response_parser.parse(content)

                if decision:
                    # Step 4: 后处理校验
                    decision = self.response_parser.validate_against_state(decision, state)
                    self.decision_logger.log(state, decision, "llm", elapsed)
                    decision["next_interval"] = self._compute_next_interval(state, decision)
                    decision["seq"] = req_seq
                    self.last_action = decision
                    self.last_action_time = now
                    self.last_decision = decision
                    logger.info(f"LLM decision: {decision.get('action', 'unknown')} - {decision.get('reason', '')}")
                    # Record to map experience
                    await self.map_experience.record_decision(state, decision)
                    return decision

            # Step 5: LLM 失败，状态感知 fallback
            logger.warning("LLM decision failed, using state-aware fallback")
            self.decision_logger.log_api_timeout()
            fallback = self.response_parser.get_fallback_decision("LLM failed", state)
            self.decision_logger.log(state, fallback, "fallback", elapsed)
            fallback["next_interval"] = self._compute_next_interval(state, fallback)
            fallback["seq"] = req_seq
            self.last_decision = fallback
            return fallback

        except Exception as e:
            logger.error(f"Handle state error: {e}")
            fallback = self.response_parser.get_fallback_decision(str(e), self.last_state)
            fallback["next_interval"] = 3.0
            fallback["seq"] = state.get("seq", 0)
            return fallback

    async def _handle_round_outcome(self, data: dict):
        """Handle round outcome from game (for map experience learning)."""
        map_name = data.get("map", "unknown")
        outcome = data.get("outcome", "unknown")
        logger.info(f"Round outcome: {map_name} → {outcome}")
        if self.map_experience:
            await self.map_experience.record_round_outcome(map_name, outcome)


async def main():
    service = LLMDecisionService()

    loop = asyncio.get_event_loop()

    def signal_handler():
        logger.info("Received shutdown signal")
        asyncio.create_task(service.stop())

    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, signal_handler)

    try:
        await service.start()
    except Exception as e:
        logger.error(f"Service error: {e}")
    finally:
        await service.stop()


if __name__ == "__main__":
    asyncio.run(main())
