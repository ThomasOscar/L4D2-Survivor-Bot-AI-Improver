#!/usr/bin/env python3
"""
L4D2 LLM Bot - Web Interface Server

Full web UI + API for LLM bot management.
Port: 9877
"""

import json
import os
import time
import logging
from aiohttp import web
from typing import Optional

logger = logging.getLogger(__name__)

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")


class DebugServer:
    def __init__(self, decision_logger, service_ref, host="0.0.0.0", port=9877):
        self.logger = decision_logger
        self.service = service_ref
        self.host = host
        self.port = port
        self.app: Optional[web.Application] = None
        self.runner: Optional[web.AppRunner] = None
        self.last_prompt = None
        self.last_response_raw = None

    async def start(self):
        self.app = web.Application()
        # Static files
        self.app.router.add_static("/static", WEB_DIR)
        # Pages
        self.app.router.add_get("/", self._page_index)
        self.app.router.add_get("/bot", self._page_bot)
        self.app.router.add_get("/map", self._page_map)
        self.app.router.add_get("/ai-view", self._page_ai_view)
        self.app.router.add_get("/config", self._page_config)
        self.app.router.add_get("/history", self._page_history)
        self.app.router.add_get("/stats", self._page_stats)
        # API endpoints
        self.app.router.add_get("/api/status", self._api_status)
        self.app.router.add_get("/api/bot", self._api_bot)
        self.app.router.add_get("/api/map", self._api_map)
        self.app.router.add_get("/api/ai-view", self._api_ai_view)
        self.app.router.add_get("/api/history", self._api_history)
        self.app.router.add_get("/api/stats", self._api_stats)
        self.app.router.add_get("/api/config", self._api_config)
        self.app.router.add_post("/api/config/prompt", self._api_update_prompt)
        self.runner = web.AppRunner(self.app)
        await self.runner.setup()
        site = web.TCPSite(self.runner, self.host, self.port)
        await site.start()
        logger.info(f"Web server started on {self.host}:{self.port}")

    async def stop(self):
        if self.runner:
            await self.runner.cleanup()
            logger.info("Web server stopped")

    # --- Page handlers (serve SPA shell) ---

    async def _serve_page(self, page_name):
        path = os.path.join(WEB_DIR, "index.html")
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return web.Response(text=f.read(), content_type="text/html")
        return web.Response(text=f"<h1>Page: {page_name}</h1><p>Frontend not built yet</p>", content_type="text/html")

    async def _page_index(self, request):
        return await self._serve_page("index")

    async def _page_bot(self, request):
        return await self._serve_page("bot")

    async def _page_map(self, request):
        return await self._serve_page("map")

    async def _page_ai_view(self, request):
        return await self._serve_page("ai-view")

    async def _page_config(self, request):
        return await self._serve_page("config")

    async def _page_history(self, request):
        return await self._serve_page("history")

    async def _page_stats(self, request):
        return await self._serve_page("stats")

    # --- API handlers ---

    async def _api_status(self, request):
        svc = self.service
        state = svc.last_state if svc else None
        result = {
            "service": "l4d2_llm_bot",
            "version": "3.0",
            "running": svc.running if svc else False,
            "llm_connected": svc.llm_client is not None if svc else False,
            "last_action": svc.last_action.get("action") if svc and svc.last_action else None,
            "uptime": round(time.time() - self.logger.stats["start_time"], 0),
        }
        if state:
            bot = state.get("bot", {})
            result.update({
                "current_map": state.get("map", "?"),
                "difficulty": state.get("difficulty", "?"),
                "bot_name": bot.get("name", "?"),
                "bot_hp": bot.get("hp", 0),
                "bot_temp_hp": bot.get("temp_hp", 0),
                "bot_incap": bot.get("incap", False),
                "bot_bw": bot.get("bw", False),
                "bot_pinned": bot.get("pinned", False),
                "bot_pin_type": bot.get("pin_type", ""),
                "threats_visible": len([t for t in state.get("threats", []) if not t.get("ghost")]),
                "teammates_pinned": len([m for m in state.get("teammates", []) if m.get("pinned")]),
                "witches_visible": len(state.get("witches", [])),
                "items_nearby": len(state.get("items", [])),
                "common_count": state.get("common_count", 0),
                "director_stage": state.get("director", "unknown"),
                "player_scores": state.get("player_scores", []),
            })
        return web.json_response(result)

    async def _api_bot(self, request):
        svc = self.service
        if not svc or not svc.last_state:
            return web.json_response({"error": "no data"})
        state = svc.last_state
        bot = state.get("bot", {})
        return web.json_response({
            "name": bot.get("name", "?"),
            "hp": bot.get("hp", 0),
            "temp_hp": bot.get("temp_hp", 0),
            "incap": bot.get("incap", False),
            "bw": bot.get("bw", False),
            "position": bot.get("pos", [0, 0, 0]),
            "weapons": bot.get("weapons", {}),
            "ammo": bot.get("ammo", 0),
            "reserve": bot.get("reserve", 0),
            "pinned": bot.get("pinned", False),
            "pin_type": bot.get("pin_type", ""),
            "teammates": state.get("teammates", []),
            "player_scores": state.get("player_scores", []),
            "current_action": svc.last_action.get("action") if svc.last_action else "none",
            "current_priority": svc.last_action.get("priority") if svc.last_action else "none",
            "current_reason": svc.last_action.get("reason") if svc.last_action else "",
        })

    async def _api_map(self, request):
        svc = self.service
        if not svc or not svc.last_state:
            return web.json_response({"error": "no data"})
        state = svc.last_state
        return web.json_response({
            "map": state.get("map", "?"),
            "mode": state.get("mode", "?"),
            "difficulty": state.get("difficulty", "?"),
            "terrain": state.get("terrain", {}),
            "events": state.get("events", []),
            "threats": state.get("threats", []),
            "witches": state.get("witches", []),
            "items": state.get("items", []),
            "common_count": state.get("common_count", 0),
            "fire_areas": state.get("fire_areas", []),
            "acid_areas": state.get("acid_areas", []),
            "director": state.get("director", "unknown"),
        })

    async def _api_ai_view(self, request):
        """Return what the AI sees: raw STATE + prompt + last decision"""
        svc = self.service
        if not svc or not svc.last_state:
            return web.json_response({"error": "no data"})
        state = svc.last_state
        # Build the prompt that would be sent to LLM
        prompt = ""
        if svc.prompt_builder:
            prompt = svc.prompt_builder.build(state, svc.last_action, time.time() - svc.last_action_time)
        return web.json_response({
            "raw_state": state,
            "system_prompt": svc.prompt_builder.get_system_prompt() if svc.prompt_builder else "",
            "user_prompt": prompt,
            "last_decision": svc.last_action,
            "decision_source": "rule" if svc.last_action and svc.last_action.get("priority") == "high" and "Override" not in svc.last_action.get("reason", "") else "llm",
        })

    async def _api_history(self, request):
        count = int(request.query.get("count", "30"))
        count = min(count, 100)
        history = self.logger.get_history(count)
        return web.json_response(history)

    async def _api_stats(self, request):
        stats = self.logger.get_stats()
        return web.json_response(stats)

    async def _api_config(self, request):
        """Return current config: ConVars (from last state) + system prompt"""
        svc = self.service
        prompt_text = ""
        prompt_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompts", "system.txt")
        if os.path.exists(prompt_path):
            with open(prompt_path, "r", encoding="utf-8") as f:
                prompt_text = f.read()
        return web.json_response({
            "system_prompt": prompt_text,
            "config": svc.config if svc else {},
            "llm_call_interval": 3.0,
            "force_reeval_interval": 10.0,
        })

    async def _api_update_prompt(self, request):
        """Update system prompt via POST"""
        try:
            data = await request.json()
            new_prompt = data.get("prompt", "").strip()
            if not new_prompt:
                return web.json_response({"error": "empty prompt"}, status=400)
            prompt_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompts", "system.txt")
            with open(prompt_path, "w", encoding="utf-8") as f:
                f.write(new_prompt)
            # Reload in service
            svc = self.service
            if svc and svc.prompt_builder:
                svc.prompt_builder.system_prompt = new_prompt
            logger.info("System prompt updated via web interface")
            return web.json_response({"status": "ok"})
        except Exception as e:
            return web.json_response({"error": str(e)}, status=500)
