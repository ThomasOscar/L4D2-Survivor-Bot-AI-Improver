#!/usr/bin/env python3
"""
L4D2 LLM Bot - Response Parser
"""

import json
import logging
from typing import Dict, Any, Optional

logger = logging.getLogger(__name__)

VALID_ACTIONS = [
    "help_teammate", "attack_si", "follow_team", "heal_teammate",
    "attack_tank", "attack_witch", "throw_grenade", "evade_threat",
    "pick_up_item", "give_item", "cover_fire", "use_defib", "hold_position"
]

VALID_PRIORITIES = ["critical", "high", "medium", "low"]


class ResponseParser:
    def parse(self, llm_response: str) -> Optional[Dict[str, Any]]:
        if not llm_response:
            return None
        json_str = self._extract_json(llm_response)
        if not json_str:
            return None
        try:
            decision = json.loads(json_str)
        except json.JSONDecodeError:
            return None
        if not self._validate_decision(decision):
            return None
        return decision
    
    def _extract_json(self, text: str) -> Optional[str]:
        text = text.strip()
        if text.startswith("{") and text.endswith("}"):
            return text
        brace_count = 0
        start = -1
        for i, c in enumerate(text):
            if c == "{":
                if start == -1:
                    start = i
                brace_count += 1
            elif c == "}":
                brace_count -= 1
                if brace_count == 0 and start != -1:
                    return text[start:i+1]
        return None
    
    def _validate_decision(self, decision: Dict[str, Any]) -> bool:
        if not decision.get("action"):
            return False
        if decision.get("priority", "medium") not in VALID_PRIORITIES:
            decision["priority"] = "medium"
        if "params" not in decision:
            decision["params"] = {}
        if "reason" not in decision:
            decision["reason"] = ""
        return True
    
    def get_fallback_decision(self, reason_or_state="API timeout", state=None) -> Dict[str, Any]:
        """State-aware fallback: considers threats/teammates instead of always follow_team"""
        if state is None and isinstance(reason_or_state, dict):
            state = reason_or_state
            reason = "API timeout"
        elif isinstance(reason_or_state, str):
            reason = reason_or_state
        else:
            reason = "API timeout"

        if state:
            threats = state.get("threats", [])
            teammates = state.get("teammates", [])
            active_threats = [t for t in threats if not t.get("ghost", False)]
            pinned = [m for m in teammates if m.get("pinned")]
            incap = [m for m in teammates if m.get("incap")]

            if active_threats:
                tanks = [t for t in active_threats if t.get("type") == "tank"]
                if tanks:
                    return {"action": "attack_tank", "params": {}, "priority": "critical",
                            "reason": f"Fallback: tank visible ({reason})"}
                return {"action": "attack_si", "params": {}, "priority": "high",
                        "reason": f"Fallback: {len(active_threats)} threats visible ({reason})"}
            if pinned:
                return {"action": "help_teammate", "params": {}, "priority": "critical",
                        "reason": f"Fallback: pinned teammate ({reason})"}
            if incap:
                return {"action": "help_teammate", "params": {}, "priority": "high",
                        "reason": f"Fallback: incap teammate ({reason})"}

        return {"action": "follow_team", "params": {}, "priority": "low", "reason": reason}

    def validate_against_state(self, decision: Dict[str, Any], state: Dict[str, Any]) -> Dict[str, Any]:
        """Post-validation: override impossible or dangerous LLM decisions"""
        if not decision or not state:
            return decision

        action = decision.get("action", "")
        bot = state.get("bot", {})

        # Bot is pinned → can only hold_position (can't help/attack/move)
        if bot.get("pinned") and action not in ("hold_position",):
            pin_type = bot.get("pin_type", "unknown")
            return {"action": "hold_position", "params": {"pin_type": pin_type},
                    "priority": "critical",
                    "reason": f"Override: bot pinned by {pin_type}, can't {action}"}

        # Bot is incap → can only hold_position
        if bot.get("incap") and action not in ("hold_position",):
            return {"action": "hold_position", "params": {},
                    "priority": "critical",
                    "reason": f"Override: bot incapacitated, can't {action}"}

        if action == "follow_team":
            threats = state.get("threats", [])
            active = [t for t in threats if not t.get("ghost", False)]
            if active:
                tanks = [t for t in active if t.get("type") == "tank"]
                if tanks:
                    return {"action": "attack_tank", "params": {}, "priority": "critical",
                            "reason": "Override: tank visible, LLM said follow_team"}
                closest = min(active, key=lambda t: t.get("dist", 9999))
                return {"action": "attack_si",
                        "params": {"target_type": closest.get("type"), "dist": closest.get("dist", 0)},
                        "priority": "high",
                        "reason": f"Override: {len(active)} threats visible ({closest.get('type')}@{closest.get('dist', 0):.0f}), LLM said follow_team"}

            teammates = state.get("teammates", [])
            pinned = [m for m in teammates if m.get("pinned")]
            if pinned:
                return {"action": "help_teammate", "params": {"target": pinned[0].get("name")},
                        "priority": "critical",
                        "reason": f"Override: {pinned[0].get('name')} pinned, LLM said follow_team"}

        return decision
