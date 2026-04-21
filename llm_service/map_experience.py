#!/usr/bin/env python3
"""
L4D2 LLM Bot - Map Experience Store

Stores and retrieves per-map, per-flow-range experience data in SQLite.
Tracks which decisions led to good or bad outcomes, enabling the LLM
to learn from past rounds on the same map.

Schema:
  map_experiences (
    id INTEGER PRIMARY KEY,
    map_name TEXT NOT NULL,
    flow_range TEXT NOT NULL,      -- e.g. "0-500", "500-1500"
    situation_hash TEXT NOT NULL,   -- hash of key state features
    action TEXT NOT NULL,
    target TEXT DEFAULT '',
    outcome TEXT NOT NULL,          -- "survived" / "incapped" / "team_wipe" / "escaped"
    survival_time REAL DEFAULT 0,  -- seconds survived after this decision
    si_kills_after INT DEFAULT 0,  -- SI kills within 10s after decision
    teammate_deaths INT DEFAULT 0, -- teammate deaths within 10s
    timestamp REAL NOT NULL
  )

Indexes: (map_name, flow_range), (situation_hash)
"""

import aiosqlite
import hashlib
import json
import logging
import os
import time
from typing import Dict, Any, List, Optional, Tuple

logger = logging.getLogger(__name__)

DB_PATH = os.environ.get("L4D2_EXPERIENCE_DB", "/opt/l4d2_llm_service/data/map_experience.db")


class MapExperienceStore:
    """SQLite-backed map experience storage and retrieval."""

    # Flow range bucket size
    FLOW_BUCKET = 1000

    def __init__(self, db_path: str = DB_PATH):
        self.db_path = db_path
        self.db: Optional[aiosqlite.Connection] = None
        # Pending outcome tracking: decision_id → (timestamp, state, decision)
        self._pending: Dict[int, Dict[str, Any]] = {}
        self._next_id = 0

    async def connect(self):
        """Open database and create tables if needed."""
        os.makedirs(os.path.dirname(self.db_path), exist_ok=True)
        self.db = await aiosqlite.connect(self.db_path)
        self.db.row_factory = aiosqlite.Row
        await self._create_tables()
        logger.info(f"MapExperienceStore connected: {self.db_path}")

    async def disconnect(self):
        if self.db:
            await self.db.close()
            self.db = None

    async def _create_tables(self):
        await self.db.execute("""
            CREATE TABLE IF NOT EXISTS map_experiences (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                map_name TEXT NOT NULL,
                flow_range TEXT NOT NULL,
                situation_hash TEXT NOT NULL,
                action TEXT NOT NULL,
                target TEXT DEFAULT '',
                outcome TEXT NOT NULL DEFAULT 'unknown',
                survival_time REAL DEFAULT 0,
                si_kills_after INT DEFAULT 0,
                teammate_deaths INT DEFAULT 0,
                timestamp REAL NOT NULL
            )
        """)
        await self.db.execute("""
            CREATE INDEX IF NOT EXISTS idx_map_flow
            ON map_experiences(map_name, flow_range)
        """)
        await self.db.execute("""
            CREATE INDEX IF NOT EXISTS idx_situation
            ON map_experiences(situation_hash)
        """)
        await self.db.commit()

    def _flow_to_range(self, flow: float) -> str:
        """Convert absolute flow distance to bucket range string."""
        bucket = int(flow // self.FLOW_BUCKET) * self.FLOW_BUCKET
        return f"{bucket}-{bucket + self.FLOW_BUCKET}"

    def _compute_situation_hash(self, state: Dict[str, Any]) -> str:
        """Hash key state features to group similar situations."""
        features = {
            "threats_count": len(state.get("threats", [])),
            "threats_types": sorted([t.get("type", "") for t in state.get("threats", []) if not t.get("ghost")]),
            "pinned_count": sum(1 for m in state.get("teammates", []) if m.get("pinned")),
            "incap_count": sum(1 for m in state.get("teammates", []) if m.get("incap")),
            "bot_incap": state.get("bot", {}).get("incap", False),
            "common_count": state.get("common_count", 0),
            "has_fire": len(state.get("fire_areas", [])) > 0,
            "has_acid": len(state.get("acid_areas", [])) > 0,
            "witch_angry": any(w.get("angry") for w in state.get("witches", [])),
        }
        raw = json.dumps(features, sort_keys=True)
        return hashlib.md5(raw.encode()).hexdigest()[:12]

    async def record_decision(self, state: Dict[str, Any], decision: Dict[str, Any]) -> int:
        """
        Record a decision that was made. Returns an internal ID
        that can be used later to record the outcome.
        """
        if not self.db:
            return -1

        map_name = state.get("map", "unknown")
        flow = state.get("bot", {}).get("flow", 0)
        flow_range = self._flow_to_range(flow)
        sit_hash = self._compute_situation_hash(state)
        action = decision.get("action", "unknown")
        target = decision.get("target", decision.get("params", {}).get("target", ""))

        # Insert with unknown outcome — will be updated when round progresses
        await self.db.execute(
            """INSERT INTO map_experiences
               (map_name, flow_range, situation_hash, action, target, outcome, timestamp)
               VALUES (?, ?, ?, ?, ?, 'pending', ?)""",
            (map_name, flow_range, sit_hash, action, target, time.time())
        )
        await self.db.commit()

        # Track pending for later outcome resolution
        self._next_id += 1
        self._pending[self._next_id] = {
            "map_name": map_name,
            "flow_range": flow_range,
            "situation_hash": sit_hash,
            "action": action,
            "timestamp": time.time(),
            "state_at_decision": state,
        }

        # Clean up old pending entries (>60s)
        now = time.time()
        expired = [k for k, v in self._pending.items() if now - v["timestamp"] > 60]
        for k in expired:
            del self._pending[k]

        return self._next_id

    async def update_outcome(self, decision_id: int, outcome: str,
                             survival_time: float = 0,
                             si_kills_after: int = 0,
                             teammate_deaths: int = 0):
        """Update the outcome of a previously recorded decision."""
        if not self.db or decision_id not in self._pending:
            return

        info = self._pending[decision_id]
        # Update the most recent matching row
        await self.db.execute(
            """UPDATE map_experiences
               SET outcome=?, survival_time=?, si_kills_after=?, teammate_deaths=?
               WHERE map_name=? AND flow_range=? AND situation_hash=?
                 AND action=? AND outcome='pending'
               ORDER BY timestamp DESC LIMIT 1""",
            (outcome, survival_time, si_kills_after, teammate_deaths,
             info["map_name"], info["flow_range"], info["situation_hash"], info["action"])
        )
        await self.db.commit()
        del self._pending[decision_id]

    async def record_round_outcome(self, map_name: str, outcome: str):
        """
        Called at round end. Updates all pending decisions for this map
        with the round outcome.
        """
        if not self.db:
            return

        now = time.time()
        await self.db.execute(
            """UPDATE map_experiences
               SET outcome=?
               WHERE map_name=? AND outcome='pending'""",
            (outcome, map_name)
        )
        await self.db.commit()

        # Clear pending for this map
        to_remove = [k for k, v in self._pending.items() if v["map_name"] == map_name]
        for k in to_remove:
            del self._pending[k]

        logger.info(f"Round outcome recorded: {map_name} → {outcome}")

    async def get_experience(self, state: Dict[str, Any], limit: int = 5) -> List[Dict[str, Any]]:
        """
        Retrieve relevant past experiences for the current state.
        Returns list of experience dicts with action, outcome, and frequency.
        """
        if not self.db:
            return []

        map_name = state.get("map", "unknown")
        flow = state.get("bot", {}).get("flow", 0)
        flow_range = self._flow_to_range(flow)
        sit_hash = self._compute_situation_hash(state)

        cursor = await self.db.execute(
            """SELECT action, target, outcome, COUNT(*) as count,
                      AVG(survival_time) as avg_survival,
                      SUM(CASE WHEN outcome='survived' THEN 1 ELSE 0 END) as survived_count
               FROM map_experiences
               WHERE map_name=? AND flow_range=? AND situation_hash=?
                 AND outcome != 'pending'
               GROUP BY action, target, outcome
               ORDER BY count DESC
               LIMIT ?""",
            (map_name, flow_range, sit_hash, limit)
        )
        rows = await cursor.fetchall()

        results = []
        for row in rows:
            results.append({
                "action": row["action"],
                "target": row["target"],
                "outcome": row["outcome"],
                "count": row["count"],
                "survival_rate": round(row["survived_count"] / max(row["count"], 1), 2),
                "avg_survival": round(row["avg_survival"], 1),
            })
        return results

    async def get_map_stats(self, map_name: str) -> Dict[str, Any]:
        """Get aggregate stats for a map."""
        if not self.db:
            return {}

        cursor = await self.db.execute(
            """SELECT
                 COUNT(*) as total_decisions,
                 COUNT(DISTINCT flow_range) as flow_sections,
                 SUM(CASE WHEN outcome='survived' THEN 1 ELSE 0 END) as survived,
                 SUM(CASE WHEN outcome='incapped' THEN 1 ELSE 0 END) as incapped,
                 SUM(CASE WHEN outcome='team_wipe' THEN 1 ELSE 0 END) as team_wipe,
                 SUM(CASE WHEN outcome='escaped' THEN 1 ELSE 0 END) as escaped
               FROM map_experiences
               WHERE map_name=? AND outcome != 'pending'""",
            (map_name,)
        )
        row = await cursor.fetchone()
        if not row or not row["total_decisions"]:
            return {"total_decisions": 0}

        total = row["total_decisions"]
        return {
            "total_decisions": total,
            "flow_sections": row["flow_sections"],
            "survival_rate": round(row["survived"] / total, 2) if total else 0,
            "escape_rate": round(row["escaped"] / total, 2) if total else 0,
            "team_wipe_rate": round(row["team_wipe"] / total, 2) if total else 0,
        }

    async def cleanup_old(self, max_age_days: int = 30):
        """Remove experiences older than max_age_days."""
        if not self.db:
            return
        cutoff = time.time() - (max_age_days * 86400)
        cursor = await self.db.execute(
            "DELETE FROM map_experiences WHERE timestamp < ? AND outcome != 'pending'",
            (cutoff,)
        )
        await self.db.commit()
        if cursor.rowcount:
            logger.info(f"Cleaned up {cursor.rowcount} old experience records")
