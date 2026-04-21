#!/usr/bin/env python3
"""
L4D2 LLM Bot - Player Scorer

Five-dimensional player scoring system:
  1. incap: times incapacitated (negative indicator)
  2. pinned: times pinned by SI (negative indicator)
  3. revive: times revived teammates (positive indicator)
  4. heal: times healed teammates (positive indicator)
  5. si_kills: SI kill count (positive indicator)

Produces per-player composite scores and team-level summary
that can be injected into LLM system prompt to influence decisions.
"""

import logging
from typing import Dict, Any, List, Optional

logger = logging.getLogger(__name__)


class PlayerScorer:
    """Computes player scores and team-level assessment from STATE data."""

    # Weight configuration: positive actions get positive weights, negative events get negative
    WEIGHTS = {
        "heal": 3.0,       # healing is the most valuable team action
        "revive": 2.5,     # reviving saves teammates
        "si_kills": 1.5,   # killing SI contributes to team safety
        "incap": -2.0,     # getting incap indicates vulnerability
        "pinned": -1.0,    # getting pinned is less severe than incap
    }

    def score_player(self, player: Dict[str, Any]) -> Dict[str, Any]:
        """
        Compute composite score for a single player.

        Returns dict with:
          - name, raw stats, composite score, rating (S/A/B/C/D)
        """
        name = player.get("name", "unknown")
        heal = player.get("heal", 0)
        revive = player.get("revive", 0)
        si_kills = player.get("si_kills", 0)
        incap = player.get("incap", 0)
        pinned = player.get("pinned", 0)

        composite = (
            heal * self.WEIGHTS["heal"]
            + revive * self.WEIGHTS["revive"]
            + si_kills * self.WEIGHTS["si_kills"]
            + incap * self.WEIGHTS["incap"]
            + pinned * self.WEIGHTS["pinned"]
        )

        # Rating tier
        if composite >= 10:
            rating = "S"
        elif composite >= 5:
            rating = "A"
        elif composite >= 2:
            rating = "B"
        elif composite >= 0:
            rating = "C"
        else:
            rating = "D"

        return {
            "name": name,
            "heal": heal,
            "revive": revive,
            "si_kills": si_kills,
            "incap": incap,
            "pinned": pinned,
            "composite": round(composite, 1),
            "rating": rating,
        }

    def score_team(self, player_scores: List[Dict[str, Any]]) -> Dict[str, Any]:
        """
        Compute team-level metrics from all player scores.

        Returns dict with:
          - team_score: sum of all composite scores
          - team_rating: S/A/B/C/D
          - vulnerable_players: list of names with D rating
          - mvp: player with highest composite (if positive)
          - needs_protection: players with high incap count (>=2)
        """
        if not player_scores:
            return {
                "team_score": 0,
                "team_rating": "C",
                "vulnerable_players": [],
                "mvp": None,
                "needs_protection": [],
            }

        team_score = sum(p["composite"] for p in player_scores)

        # Team rating
        avg = team_score / len(player_scores)
        if avg >= 8:
            team_rating = "S"
        elif avg >= 4:
            team_rating = "A"
        elif avg >= 2:
            team_rating = "B"
        elif avg >= 0:
            team_rating = "C"
        else:
            team_rating = "D"

        vulnerable = [p["name"] for p in player_scores if p["rating"] == "D"]
        needs_protection = [p["name"] for p in player_scores if p["incap"] >= 2]

        # MVP: highest composite with positive score
        mvp = None
        positive = [p for p in player_scores if p["composite"] > 0]
        if positive:
            mvp = max(positive, key=lambda p: p["composite"])["name"]

        return {
            "team_score": round(team_score, 1),
            "team_rating": team_rating,
            "vulnerable_players": vulnerable,
            "mvp": mvp,
            "needs_protection": needs_protection,
        }

    def get_decision_hint(self, state: Dict[str, Any]) -> Optional[str]:
        """
        Generate a short decision hint based on player scores.
        Returns None if no special hint is needed.

        These hints are injected into the LLM prompt to influence
        decision-making based on team composition and vulnerability.
        """
        player_scores_raw = state.get("player_scores", [])
        if not player_scores_raw:
            return None

        scored = [self.score_player(p) for p in player_scores_raw]
        team = self.score_team(scored)

        hints = []

        # Team is struggling → prioritize survival
        if team["team_rating"] in ("D",):
            hints.append("Team is struggling (low scores). Prioritize survival and healing over aggressive plays.")

        # Vulnerable players need protection
        if team["needs_protection"]:
            names = ", ".join(team["needs_protection"])
            hints.append(f"Protect vulnerable teammates: {names} (frequent incap). Stay closer and provide cover.")

        # If bot itself is the weakest → avoid frontline
        bot_name = state.get("bot", {}).get("name", "")
        bot_score = next((s for s in scored if s["name"] == bot_name), None)
        if bot_score and bot_score["rating"] == "D":
            hints.append("Your bot is the weakest link. Avoid frontline combat, focus on support (heal/revive).")

        # If bot has high heal score → prioritize healing role
        if bot_score and bot_score["heal"] >= 2:
            hints.append("You are the primary healer. Prioritize keeping teammates alive.")

        return " ".join(hints) if hints else None

    def get_score_summary(self, state: Dict[str, Any]) -> str:
        """
        Generate a human-readable score summary for debug/logging.
        """
        player_scores_raw = state.get("player_scores", [])
        if not player_scores_raw:
            return "No player scores available"

        scored = [self.score_player(p) for p in player_scores_raw]
        team = self.score_team(scored)

        lines = [f"Team Score: {team['team_score']} ({team['team_rating']})"]
        for s in scored:
            lines.append(
                f"  {s['name']}: {s['composite']:.1f} ({s['rating']}) "
                f"- heal:{s['heal']} revive:{s['revive']} kills:{s['si_kills']} "
                f"incap:{s['incap']} pinned:{s['pinned']}"
            )
        if team["mvp"]:
            lines.append(f"  MVP: {team['mvp']}")
        if team["needs_protection"]:
            lines.append(f"  Needs protection: {', '.join(team['needs_protection'])}")

        return "\n".join(lines)
