#!/usr/bin/env python3
"""
L4D2 LLM Bot - Model Router

Routes LLM requests to the best available provider based on strategy.
Supports priority, round_robin, and adaptive routing.
"""

import asyncio
import logging
import time
from typing import Optional, Dict, Any, List
from dataclasses import dataclass, field

from llm_client import LLMClient, LLMConfig

logger = logging.getLogger(__name__)


@dataclass
class ProviderHealth:
    """Track health metrics for a provider."""
    success_count: int = 0
    fail_count: int = 0
    last_fail_time: float = 0.0
    avg_latency: float = 0.0
    _latencies: list = field(default_factory=list)

    def record_success(self, latency: float):
        self.success_count += 1
        self._latencies.append(latency)
        if len(self._latencies) > 20:
            self._latencies = self._latencies[-20:]
        self.avg_latency = sum(self._latencies) / len(self._latencies)

    def record_failure(self):
        self.fail_count += 1
        self.last_fail_time = time.time()

    def is_healthy(self) -> bool:
        """Consider unhealthy if last 5 requests all failed, or last failure < 10s ago."""
        if self.success_count + self.fail_count == 0:
            return True  # No data yet, assume healthy
        if self.fail_count > 5 and self.success_count == 0:
            return False
        # Cooldown after failure
        if self.last_fail_time > 0 and time.time() - self.last_fail_time < 10:
            return False
        return True


class ModelRouter:
    """Routes LLM requests to the best available provider."""

    def __init__(self, config: dict):
        self.providers: List[Dict[str, Any]] = []  # [{client, config, health, tier}]
        self.strategy = config.get("strategy", "priority")
        self._load_providers(config)

    def _load_providers(self, config: dict):
        """Load providers from config."""
        models = config.get("models", [])
        if not models:
            # Fallback: create single provider from legacy config
            legacy = config.get("llm", {})
            if legacy:
                models = [legacy]

        for i, model_cfg in enumerate(models):
            llm_config = LLMConfig(
                provider=model_cfg.get("provider", "openai"),
                api_key=model_cfg.get("api_key", ""),
                base_url=model_cfg.get("base_url", "https://api.deepseek.com/v1"),
                model=model_cfg.get("model", "deepseek-chat"),
                max_tokens=model_cfg.get("max_tokens", 150),
                temperature=model_cfg.get("temperature", 0.3),
                timeout=model_cfg.get("timeout", 5.0)
            )
            client = LLMClient(llm_config)
            self.providers.append({
                "client": client,
                "name": model_cfg.get("name", f"provider_{i}"),
                "tier": model_cfg.get("tier", "standard"),
                "priority": model_cfg.get("priority", i + 1),
                "enabled": model_cfg.get("enabled", True),
                "health": ProviderHealth()
            })

    async def start(self):
        """Start all provider clients."""
        for p in self.providers:
            if p["enabled"]:
                await p["client"].start()
        logger.info(f"ModelRouter started: {len(self.providers)} providers, strategy={self.strategy}")

    async def stop(self):
        """Stop all provider clients."""
        for p in self.providers:
            await p["client"].stop()

    async def chat(self, system_prompt: str, user_prompt: str,
                   state: Optional[dict] = None) -> tuple:
        """
        Route a chat request to the best available provider.
        Returns (response_dict, provider_name) or (None, None) if all fail.
        """
        if self.strategy == "adaptive" and state:
            order = self._route_adaptive(state)
        elif self.strategy == "round_robin":
            order = self._route_round_robin()
        else:
            order = self._route_priority()

        for provider in order:
            if not provider["enabled"]:
                continue
            if not provider["health"].is_healthy():
                logger.debug(f"Skipping unhealthy provider: {provider['name']}")
                continue

            t0 = time.time()
            try:
                response = await provider["client"].chat(system_prompt, user_prompt)
                elapsed = time.time() - t0
                if response:
                    provider["health"].record_success(elapsed)
                    logger.info(f"LLM response from {provider['name']} in {elapsed:.2f}s")
                    return response, provider["name"]
                else:
                    provider["health"].record_failure()
                    logger.warning(f"Provider {provider['name']} returned None")
            except Exception as e:
                provider["health"].record_failure()
                logger.warning(f"Provider {provider['name']} error: {e}")

        logger.error("All LLM providers failed")
        return None, None

    def parse_response(self, response: Dict[str, Any], provider_name: str) -> str:
        """Parse response using the appropriate provider's client."""
        for p in self.providers:
            if p["name"] == provider_name:
                return p["client"].parse_response(response)
        # Fallback
        return self.providers[0]["client"].parse_response(response) if self.providers else ""

    def _route_priority(self) -> list:
        """Return providers sorted by priority."""
        return sorted(self.providers, key=lambda p: p["priority"])

    def _route_round_robin(self) -> list:
        """Rotate provider order each call."""
        if self.providers:
            first = self.providers.pop(0)
            self.providers.append(first)
        return self.providers

    def _route_adaptive(self, state: dict) -> list:
        """Route based on scene complexity."""
        complexity = self._assess_complexity(state)
        tier_order = self._tier_order_for_complexity(complexity)

        # Sort providers: matching tier first, then by priority
        def sort_key(p):
            tier_idx = tier_order.index(p["tier"]) if p["tier"] in tier_order else 99
            return (tier_idx, p["priority"])

        return sorted(self.providers, key=sort_key)

    def _assess_complexity(self, state: dict) -> float:
        """Assess scene complexity on 0-1 scale."""
        score = 0.0
        threats = state.get("threats", [])
        active_threats = [t for t in threats if not t.get("ghost")]
        teammates = state.get("teammates", [])

        # More active threats → more complex
        score += min(len(active_threats) * 0.15, 0.4)
        # Pinned teammates → complex
        pinned = [m for m in teammates if m.get("pinned")]
        score += min(len(pinned) * 0.2, 0.3)
        # Multiple threats of different types → complex
        threat_types = set(t.get("type") for t in active_threats)
        score += min(len(threat_types) * 0.1, 0.2)
        # Low HP → complex
        bot = state.get("bot", {})
        if bot.get("hp", 100) < 30:
            score += 0.1

        return min(score, 1.0)

    @staticmethod
    def _tier_order_for_complexity(complexity: float) -> list:
        """Map complexity score to preferred model tier order."""
        if complexity < 0.3:
            return ["lightweight", "standard", "premium"]
        elif complexity < 0.6:
            return ["standard", "lightweight", "premium"]
        else:
            return ["premium", "standard", "lightweight"]

    def get_status(self) -> list:
        """Return health status of all providers."""
        result = []
        for p in self.providers:
            h = p["health"]
            result.append({
                "name": p["name"],
                "tier": p["tier"],
                "enabled": p["enabled"],
                "healthy": h.is_healthy(),
                "success": h.success_count,
                "fail": h.fail_count,
                "avg_latency": round(h.avg_latency, 3)
            })
        return result
