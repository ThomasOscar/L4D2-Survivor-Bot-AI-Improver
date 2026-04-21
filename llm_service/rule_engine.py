#!/usr/bin/env python3
"""
L4D2 LLM Bot - Rule Engine (Deterministic Pre-filter)

Handles clear-cut decisions without LLM API calls.
Only delegates ambiguous/complex scenarios to LLM.
"""

import logging
from typing import Dict, Any, Optional

logger = logging.getLogger(__name__)


class RuleEngine:
    """Deterministic rule engine for obvious game decisions."""

    def evaluate(self, state: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        """
        Evaluate state against deterministic rules.
        Returns a decision dict if a rule matches, None to delegate to LLM.
        """
        if not state:
            return None

        threats = state.get("threats", [])
        witches = state.get("witches", [])
        teammates = state.get("teammates", [])
        bot = state.get("bot", {})

        # Rule 0: Self incapacitated → hold_position (can't do anything else)
        if bot.get("incap"):
            logger.info("Rule: hold_position (self incapacitated)")
            return {
                "action": "hold_position",
                "params": {},
                "priority": "critical",
                "reason": "Self incapacitated, waiting for rescue"
            }

        # Filter out ghost SI (not yet spawned, can't attack)
        active_threats = [t for t in threats if not t.get("ghost", False)]

        # Rule 1: Tank visible → attack_tank (critical)
        tanks = [t for t in active_threats if t.get("type") == "tank"]
        if tanks:
            closest = min(tanks, key=lambda t: t.get("dist", 9999))
            logger.info(f"Rule: attack_tank (dist={closest.get('dist', 0):.0f})")
            return {
                "action": "attack_tank",
                "params": {"target_type": "tank", "dist": closest.get("dist", 0)},
                "priority": "critical",
                "reason": f"Tank visible at dist {closest.get('dist', 0):.0f}"
            }

        # Rule 2: Visible non-ghost SI → attack_si (high)
        if active_threats:
            # Prioritize: smoker/hunter/charger (pinning) > jockey > boomer/spitter
            priority_types = ["charger", "smoker", "hunter", "jockey", "boomer", "spitter"]
            target = None
            for ptype in priority_types:
                candidates = [t for t in active_threats if t.get("type") == ptype]
                if candidates:
                    target = min(candidates, key=lambda t: t.get("dist", 9999))
                    break
            if not target:
                target = min(active_threats, key=lambda t: t.get("dist", 9999))

            logger.info(f"Rule: attack_si {target.get('type')} (dist={target.get('dist', 0):.0f})")
            return {
                "action": "attack_si",
                "params": {"target_type": target.get("type"), "dist": target.get("dist", 0)},
                "priority": "high",
                "reason": f"Visible {target.get('type')} at dist {target.get('dist', 0):.0f}"
            }

        # Rule 3: Angry witch → attack_witch (high)
        angry_witches = [w for w in witches if w.get("angry")]
        if angry_witches:
            closest = min(angry_witches, key=lambda w: w.get("dist", 9999))
            logger.info(f"Rule: attack_witch (dist={closest.get('dist', 0):.0f})")
            return {
                "action": "attack_witch",
                "params": {"dist": closest.get("dist", 0)},
                "priority": "high",
                "reason": f"Angry witch at dist {closest.get('dist', 0):.0f}"
            }

        # Rule 4: Pinned teammate → help_teammate (critical)
        pinned = [m for m in teammates if m.get("pinned")]
        if pinned:
            closest = min(pinned, key=lambda m: m.get("dist", 9999))
            logger.info(f"Rule: help_teammate pinned={closest.get('name')}")
            return {
                "action": "help_teammate",
                "params": {"target": closest.get("name")},
                "priority": "critical",
                "reason": f"{closest.get('name')} is pinned"
            }

        # Rule 5: Incapacitated teammate → help_teammate (high)
        incap = [m for m in teammates if m.get("incap") and not m.get("pinned")]
        if incap:
            closest = min(incap, key=lambda m: m.get("dist", 9999))
            logger.info(f"Rule: help_teammate incap={closest.get('name')}")
            return {
                "action": "help_teammate",
                "params": {"target": closest.get("name")},
                "priority": "high",
                "reason": f"{closest.get('name')} is incapacitated"
            }

        # Rule 6: B&W teammate with health item → heal_teammate (high)
        bot_weapons = bot.get("weapons", {})
        has_health_item = bot_weapons.get("health", "none") != "none"
        bw_mates = [m for m in teammates if m.get("bw") and m.get("hp", 100) < 40]
        if bw_mates and has_health_item:
            target = min(bw_mates, key=lambda m: m.get("dist", 9999))
            logger.info(f"Rule: heal_teammate bw={target.get('name')}")
            return {
                "action": "heal_teammate",
                "params": {"target": target.get("name")},
                "priority": "high",
                "reason": f"{target.get('name')} is B&W (HP={target.get('hp', 0)})"
            }

        # No rule matched → delegate to LLM
        return None
