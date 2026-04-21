#!/usr/bin/env python3
"""
L4D2 LLM Bot - Main Entry (v2.1 with Rule Engine + Debug + Smart Interval)
"""

import asyncio
import logging
import signal
import sys
import time
import yaml
from pathlib import Path

from tcp_server import TCPServer
from llm_client import LLMClient, LLMConfig
from prompt_builder import PromptBuilder
from response_parser import ResponseParser
from rule_engine import RuleEngine
from decision_logger import DecisionLogger
from debug_server import DebugServer

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("/opt/l4d2_llm_service/logs/service.log")
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
        self.prompt_builder: PromptBuilder = None
        self.response_parser: ResponseParser = None
        self.rule_engine: RuleEngine = None
        self.decision_logger: DecisionLogger = None
        self.debug_server: DebugServer = None
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
        logger.info("Starting LLM Decision Service v2.1 (Rule Engine + Debug + Smart Interval)...")

        server_config = self.config.get("server", {})
        self.tcp_server = TCPServer(
            host=server_config.get("host", "0.0.0.0"),
            port=server_config.get("port", 9876)
        )

        llm_config = self.config.get("llm", {})
        self.llm_client = LLMClient(LLMConfig(
            provider=llm_config.get("provider", "deepseek"),
            api_key=llm_config.get("api_key", ""),
            base_url=llm_config.get("base_url", "https://api.deepseek.com/v1"),
            model=llm_config.get("model", "deepseek-chat"),
            max_tokens=llm_config.get("max_tokens", 150),
            temperature=llm_config.get("temperature", 0.3),
            timeout=llm_config.get("timeout", 5.0)
        ))

        self.prompt_builder = PromptBuilder(
            system_prompt_path=self.config.get("prompt", {}).get("system_prompt_file", "prompts/system.txt")
        )
        self.response_parser = ResponseParser()
        self.rule_engine = RuleEngine()
        self.decision_logger = DecisionLogger(max_history=100)

        self.tcp_server.on_message = self._handle_state

        await self.llm_client.start()

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
        if self.llm_client:
            await self.llm_client.stop()
        logger.info("Service stopped")

    async def _handle_state(self, state: dict) -> dict:
        """处理游戏状态：变化检测 → 规则引擎 → LLM → 后处理校验"""
        try:
            now = time.time()
            self.last_state = state

            # Step 0: STATE 变化检测（无变化 + 最近有决策 → 复用）
            state_changed = self._state_changed(state)
            if not state_changed and self.last_decision:
                if now - self.last_action_time < FORCE_REEVAL_INTERVAL:
                    self.decision_logger.log(state, self.last_decision, "skipped")
                    return self.last_decision

            # Step 1: 规则引擎预判（即时，无 API 开销）
            t0 = time.time()
            rule_decision = self.rule_engine.evaluate(state)
            if rule_decision:
                elapsed = time.time() - t0
                self.decision_logger.log(state, rule_decision, "rule", elapsed)
                rule_decision["next_interval"] = self._compute_next_interval(state, rule_decision)
                self.last_decision = rule_decision
                self.last_action = rule_decision
                self.last_action_time = now
                logger.info(f"Rule decision: {rule_decision['action']} - {rule_decision.get('reason', '')}")
                return rule_decision

            # Step 2: 节流检查（规则引擎未命中才走到这）
            if now - self.last_llm_call_time < LLM_CALL_INTERVAL:
                if self.last_decision:
                    return self.last_decision
                fallback = self.response_parser.get_fallback_decision("waiting", state)
                fallback["next_interval"] = self._compute_next_interval(state, fallback)
                return fallback

            self.last_llm_call_time = now

            # Step 3: 构建 Prompt 并调用 LLM
            t0 = time.time()
            system_prompt = self.prompt_builder.get_system_prompt()
            user_prompt = self.prompt_builder.build(
                state,
                self.last_action,
                now - self.last_action_time
            )

            response = await self.llm_client.chat(system_prompt, user_prompt)
            elapsed = time.time() - t0

            if response:
                content = self.llm_client.parse_response(response)
                decision = self.response_parser.parse(content)

                if decision:
                    # Step 4: 后处理校验
                    decision = self.response_parser.validate_against_state(decision, state)
                    self.decision_logger.log(state, decision, "llm", elapsed)
                    decision["next_interval"] = self._compute_next_interval(state, decision)
                    self.last_action = decision
                    self.last_action_time = now
                    self.last_decision = decision
                    logger.info(f"LLM decision: {decision.get('action', 'unknown')} - {decision.get('reason', '')}")
                    return decision

            # Step 5: LLM 失败，状态感知 fallback
            logger.warning("LLM decision failed, using state-aware fallback")
            self.decision_logger.log_api_timeout()
            fallback = self.response_parser.get_fallback_decision("LLM failed", state)
            self.decision_logger.log(state, fallback, "fallback", elapsed)
            fallback["next_interval"] = self._compute_next_interval(state, fallback)
            self.last_decision = fallback
            return fallback

        except Exception as e:
            logger.error(f"Handle state error: {e}")
            fallback = self.response_parser.get_fallback_decision(str(e), self.last_state)
            fallback["next_interval"] = 3.0
            return fallback


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
