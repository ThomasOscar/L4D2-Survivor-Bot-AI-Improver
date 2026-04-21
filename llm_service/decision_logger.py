#!/usr/bin/env python3
"""
L4D2 LLM Bot - Decision Logger

Structured logging with history buffer and runtime statistics.
"""

import time
import json
from collections import deque
from typing import Dict, Any, Optional, List


class DecisionLogger:
    def __init__(self, max_history: int = 100):
        self.history: deque = deque(maxlen=max_history)
        self.stats = {
            "rule": 0,
            "llm": 0,
            "fallback": 0,
            "api_timeout": 0,
            "api_success": 0,
            "api_total_time": 0.0,
            "state_unchanged_skipped": 0,
            "total_decisions": 0,
            "start_time": time.time()
        }

    @staticmethod
    def _summarize_state(state: Dict[str, Any]) -> Dict[str, Any]:
        """Extract lightweight summary from full STATE to avoid storing huge JSON."""
        if not state:
            return {}
        threats = state.get("threats", [])
        witches = state.get("witches", [])
        teammates = state.get("teammates", [])
        bot = state.get("bot", {})
        return {
            "threat_count": len([t for t in threats if not t.get("ghost")]),
            "threat_types": [t.get("type") for t in threats if not t.get("ghost")],
            "witch_count": len(witches),
            "pinned_count": len([m for m in teammates if m.get("pinned")]),
            "incap_count": len([m for m in teammates if m.get("incap")]),
            "bot_hp": bot.get("hp", 0),
            "bot_incap": bot.get("incap", False),
            "bot_bw": bot.get("bw", False),
            "map": state.get("map", "?"),
        }

    def log(self, state: Dict[str, Any], decision: Dict[str, Any],
            source: str, elapsed: float = 0.0) -> None:
        """Record a decision with state summary."""
        entry = {
            "time": time.time(),
            "time_str": time.strftime("%H:%M:%S"),
            "state": self._summarize_state(state),
            "decision": {
                "action": decision.get("action", "?"),
                "priority": decision.get("priority", "?"),
                "reason": decision.get("reason", "")[:120],
            },
            "source": source,
            "elapsed_ms": round(elapsed * 1000, 1),
        }
        self.history.append(entry)
        self.stats["total_decisions"] += 1

        if source == "rule":
            self.stats["rule"] += 1
        elif source == "llm":
            self.stats["llm"] += 1
            self.stats["api_success"] += 1
            self.stats["api_total_time"] += elapsed
        elif source == "fallback":
            self.stats["fallback"] += 1
        elif source == "skipped":
            self.stats["state_unchanged_skipped"] += 1

    def log_api_timeout(self) -> None:
        self.stats["api_timeout"] += 1

    def get_stats(self) -> Dict[str, Any]:
        s = dict(self.stats)
        s["uptime_seconds"] = round(time.time() - s["start_time"], 0)
        if s["api_success"] > 0:
            s["avg_api_time_ms"] = round(s["api_total_time"] / s["api_success"] * 1000, 1)
        else:
            s["avg_api_time_ms"] = 0
        total = s["rule"] + s["llm"] + s["fallback"]
        if total > 0:
            s["rule_pct"] = round(s["rule"] / total * 100, 1)
            s["llm_pct"] = round(s["llm"] / total * 100, 1)
            s["fallback_pct"] = round(s["fallback"] / total * 100, 1)
        # Action distribution
        action_dist = {}
        for entry in self.history:
            a = entry.get("decision", {}).get("action", "?")
            action_dist[a] = action_dist.get(a, 0) + 1
        s["action_distribution"] = action_dist
        return s

    def get_history(self, count: int = 20) -> List[Dict[str, Any]]:
        items = list(self.history)
        return items[-count:] if count < len(items) else items
