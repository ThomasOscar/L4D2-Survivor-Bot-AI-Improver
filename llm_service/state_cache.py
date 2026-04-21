#!/usr/bin/env python3
"""
L4D2 LLM Bot - State Cache (Redis + in-memory fallback)

Caches game state and decisions for:
- Skip unchanged states (supplements _state_changed)
- Reuse decisions for similar states (state hash matching)
- Persist across service restarts
"""

import json
import logging
import hashlib
from typing import Optional, Dict, Any

logger = logging.getLogger(__name__)


class StateCache:
    """Redis-backed state cache with in-memory fallback."""

    def __init__(self, redis_url: str = "redis://localhost:6379", enabled: bool = True):
        self.enabled = enabled
        self.redis = None
        self._fallback_state: Dict[str, Any] = {}
        self._fallback_decisions: Dict[str, Any] = {}
        self._connected = False

        if enabled:
            try:
                import aioredis
                self._redis_url = redis_url
            except ImportError:
                logger.warning("aioredis not installed, using in-memory cache only")
                self.enabled = False

    async def connect(self):
        """Connect to Redis."""
        if not self.enabled:
            logger.info("StateCache: disabled, using in-memory fallback")
            return

        try:
            import aioredis
            self.redis = await aioredis.from_url(
                self._redis_url,
                encoding="utf-8",
                decode_responses=True
            )
            await self.redis.ping()
            self._connected = True
            logger.info(f"StateCache: connected to Redis at {self._redis_url}")
        except Exception as e:
            logger.warning(f"StateCache: Redis connection failed ({e}), using in-memory fallback")
            self._connected = False

    async def disconnect(self):
        """Disconnect from Redis."""
        if self.redis:
            await self.redis.close()
            self._connected = False

    async def get_last_state(self, bot_name: str = "default") -> Optional[dict]:
        """Get last cached state for a bot."""
        key = f"l4d2:state:{bot_name}"
        try:
            if self._connected and self.redis:
                data = await self.redis.get(key)
                if data:
                    return json.loads(data)
            return self._fallback_state.get(bot_name)
        except Exception as e:
            logger.debug(f"Cache get_last_state error: {e}")
            return self._fallback_state.get(bot_name)

    async def set_last_state(self, bot_name: str = "default", state: dict = None, ttl: int = 10):
        """Cache last state for a bot."""
        key = f"l4d2:state:{bot_name}"
        try:
            if self._connected and self.redis and state:
                await self.redis.set(key, json.dumps(state, default=str), ex=ttl)
            self._fallback_state[bot_name] = state
        except Exception as e:
            logger.debug(f"Cache set_last_state error: {e}")
            self._fallback_state[bot_name] = state

    def compute_state_hash(self, state: dict) -> str:
        """Compute a lightweight hash of key state fields for decision caching."""
        # Hash only the fields that matter for decision-making
        hash_fields = {
            "threats": state.get("threats", []),
            "witches": state.get("witches", []),
            "teammates": [
                {"name": m.get("name"), "pinned": m.get("pinned"), "incap": m.get("incap"), "bw": m.get("bw")}
                for m in state.get("teammates", [])
            ],
            "bot": {
                "hp": state.get("bot", {}).get("hp"),
                "incap": state.get("bot", {}).get("incap"),
                "bw": state.get("bot", {}).get("bw")
            },
            "common_count": state.get("common_count", 0),
            "events": state.get("events", [])
        }
        raw = json.dumps(hash_fields, sort_keys=True, default=str)
        return hashlib.md5(raw.encode()).hexdigest()

    async def get_decision(self, state_hash: str) -> Optional[dict]:
        """Get cached decision for a state hash."""
        key = f"l4d2:decision:{state_hash}"
        try:
            if self._connected and self.redis:
                data = await self.redis.get(key)
                if data:
                    return json.loads(data)
            return self._fallback_decisions.get(state_hash)
        except Exception as e:
            logger.debug(f"Cache get_decision error: {e}")
            return self._fallback_decisions.get(state_hash)

    async def set_decision(self, state_hash: str, decision: dict, ttl: int = 10):
        """Cache a decision for a state hash."""
        key = f"l4d2:decision:{state_hash}"
        try:
            if self._connected and self.redis:
                await self.redis.set(key, json.dumps(decision, default=str), ex=ttl)
            self._fallback_decisions[state_hash] = decision
            # Keep fallback dict bounded
            if len(self._fallback_decisions) > 50:
                oldest = list(self._fallback_decisions.keys())[:10]
                for k in oldest:
                    del self._fallback_decisions[k]
        except Exception as e:
            logger.debug(f"Cache set_decision error: {e}")
            self._fallback_decisions[state_hash] = decision

    @property
    def is_connected(self) -> bool:
        return self._connected
