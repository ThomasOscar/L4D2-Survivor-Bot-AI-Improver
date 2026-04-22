"""
Round Stats - 回合统计数据持久化模块
保存每局游戏数据，用于后续分析和插件开发参考
"""

import sqlite3
import time
import json
import logging
from pathlib import Path

logger = logging.getLogger("round_stats")

DB_PATH = Path(__file__).parent / "data" / "round_stats.db"


class RoundStatsStore:
    """SQLite-based round statistics storage"""

    def __init__(self):
        self.conn = None

    async def connect(self):
        DB_PATH.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(DB_PATH))
        self.conn.row_factory = sqlite3.Row
        self._create_tables()
        logger.info("RoundStatsStore connected")

    async def disconnect(self):
        if self.conn:
            self.conn.close()
            self.conn = None

    def _create_tables(self):
        c = self.conn.cursor()
        c.execute("""
            CREATE TABLE IF NOT EXISTS rounds (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                map_name TEXT NOT NULL,
                mode TEXT NOT NULL,
                difficulty TEXT NOT NULL,
                start_time REAL NOT NULL,
                end_time REAL,
                duration_seconds REAL,
                outcome TEXT,
                total_deaths INTEGER DEFAULT 0,
                total_incaps INTEGER DEFAULT 0,
                total_heals INTEGER DEFAULT 0,
                total_si_kills INTEGER DEFAULT 0,
                total_common_kills INTEGER DEFAULT 0,
                total_item_pickups INTEGER DEFAULT 0
            )
        """)
        c.execute("""
            CREATE TABLE IF NOT EXISTS round_players (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                round_id INTEGER NOT NULL,
                player_name TEXT NOT NULL,
                deaths INTEGER DEFAULT 0,
                incaps INTEGER DEFAULT 0,
                pinned INTEGER DEFAULT 0,
                heals INTEGER DEFAULT 0,
                revives INTEGER DEFAULT 0,
                si_kills INTEGER DEFAULT 0,
                common_kills INTEGER DEFAULT 0,
                item_pickups INTEGER DEFAULT 0,
                FOREIGN KEY (round_id) REFERENCES rounds(id)
            )
        """)
        c.execute("""
            CREATE TABLE IF NOT EXISTS round_chapters (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                round_id INTEGER NOT NULL,
                chapter INTEGER NOT NULL,
                deaths INTEGER DEFAULT 0,
                incaps INTEGER DEFAULT 0,
                heals INTEGER DEFAULT 0,
                si_kills INTEGER DEFAULT 0,
                common_kills INTEGER DEFAULT 0,
                FOREIGN KEY (round_id) REFERENCES rounds(id)
            )
        """)
        c.execute("CREATE INDEX IF NOT EXISTS idx_rounds_map ON rounds(map_name)")
        c.execute("CREATE INDEX IF NOT EXISTS idx_rounds_time ON rounds(start_time)")
        self.conn.commit()

    def start_round(self, map_name: str, mode: str, difficulty: str) -> int:
        """Start a new round, return round_id"""
        c = self.conn.cursor()
        c.execute(
            "INSERT INTO rounds (map_name, mode, difficulty, start_time) VALUES (?, ?, ?, ?)",
            (map_name, mode, difficulty, time.time())
        )
        self.conn.commit()
        return c.lastrowid

    def end_round(self, round_id: int, outcome: str, totals: dict):
        """End a round with outcome and totals"""
        c = self.conn.cursor()
        c.execute("""
            UPDATE rounds SET end_time=?, duration_seconds=?, outcome=?,
                total_deaths=?, total_incaps=?, total_heals=?,
                total_si_kills=?, total_common_kills=?, total_item_pickups=?
            WHERE id=?
        """, (
            time.time(),
            time.time() - (self._get_start_time(round_id) or time.time()),
            outcome,
            totals.get("deaths", 0),
            totals.get("incaps", 0),
            totals.get("heals", 0),
            totals.get("si_kills", 0),
            totals.get("common_kills", 0),
            totals.get("item_pickups", 0),
            round_id
        ))
        self.conn.commit()

    def _get_start_time(self, round_id: int) -> float:
        c = self.conn.cursor()
        c.execute("SELECT start_time FROM rounds WHERE id=?", (round_id,))
        row = c.fetchone()
        return row[0] if row else None

    def save_player_stats(self, round_id: int, player_scores: list):
        """Save per-player stats for a round"""
        c = self.conn.cursor()
        for p in player_scores:
            c.execute("""
                INSERT INTO round_players (round_id, player_name, deaths, incaps, pinned,
                    heals, revives, si_kills, common_kills, item_pickups)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                round_id, p.get("name", "?"),
                p.get("deaths", 0), p.get("incap", 0), p.get("pinned", 0),
                p.get("heal", 0), p.get("revive", 0), p.get("si_kills", 0),
                p.get("common_kills", 0), p.get("item_pickups", 0)
            ))
        self.conn.commit()

    def save_chapter_stats(self, round_id: int, chapter_stats: dict):
        """Save chapter stats snapshot"""
        if not chapter_stats:
            return
        chap_totals = chapter_stats.get("chapter_totals", {})
        chapter = chapter_stats.get("current_chapter", 0)
        if chapter <= 0:
            return
        c = self.conn.cursor()
        c.execute("""
            INSERT OR REPLACE INTO round_chapters (round_id, chapter, deaths, incaps, heals, si_kills, common_kills)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (
            round_id, chapter,
            chap_totals.get("deaths", 0), chap_totals.get("incaps", 0),
            chap_totals.get("heals", 0), chap_totals.get("si_kills", 0),
            chap_totals.get("common_kills", 0)
        ))
        self.conn.commit()

    def get_round_history(self, limit: int = 20) -> list:
        """Get recent round history"""
        c = self.conn.cursor()
        c.execute("""
            SELECT r.*, GROUP_CONCAT(rp.player_name) as players
            FROM rounds r
            LEFT JOIN round_players rp ON r.id = rp.round_id
            GROUP BY r.id
            ORDER BY r.start_time DESC
            LIMIT ?
        """, (limit,))
        return [dict(row) for row in c.fetchall()]

    def get_current_game_stats(self, state: dict) -> dict:
        """Build current game stats from live state"""
        ps = state.get("player_scores", [])
        cs = state.get("chapter_stats", {})
        director = state.get("director", {})
        return {
            "map": state.get("map", ""),
            "mode": state.get("mode", ""),
            "difficulty": state.get("difficulty", ""),
            "chapter": director.get("chapter", cs.get("current_chapter", 0)),
            "flow_percent": director.get("flow_percent", 0),
            "director_stage": director.get("stage", "unknown"),
            "player_stats": ps,
            "round_totals": cs.get("round_totals", {}),
            "chapter_totals": cs.get("chapter_totals", {}),
        }
