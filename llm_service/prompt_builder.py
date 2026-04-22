#!/usr/bin/env python3
"""L4D2 LLM Bot - Prompt Builder (v2.0 — complete STATE field coverage)"""

import json
from typing import Dict, Any, Optional


class PromptBuilder:
    def __init__(self, system_prompt_path: str = "prompts/system.txt"):
        self.system_prompt = self._load_system_prompt(system_prompt_path)
        
    def _load_system_prompt(self, path: str) -> str:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return f.read().strip()
        except FileNotFoundError:
            return self._default_system_prompt()
    
    def _default_system_prompt(self) -> str:
        return "You are a L4D2 survivor bot decision maker. Reply in JSON format."
    
    def _clean_weapon_name(self, name: str) -> str:
        if name == "none" or not name:
            return "none"
        return name.replace("weapon_", "")
    
    def build(self, state: Dict[str, Any], last_action: Optional[Dict] = None, 
              last_action_time_ago: float = 0) -> str:
        parts = []
        
        # [Map]
        map_name = state.get("map", "unknown")
        mode = state.get("mode", "coop")
        difficulty = state.get("difficulty", "normal")
        parts.append(f"[Map] {map_name} | Mode: {mode} | Difficulty: {difficulty}")
        
        # [Terrain]
        terrain = state.get("terrain", {})
        terrain_flags = []
        if terrain.get("narrow"): terrain_flags.append("narrow_passage")
        if terrain.get("ledge"): terrain_flags.append("ledge/cliff")
        if terrain.get("alarm_car"): terrain_flags.append("alarm_cars")
        if terrain.get("crescendo"): terrain_flags.append("crescendo_event")
        if terrain.get("finale"): terrain_flags.append("FINALE")
        if terrain_flags:
            parts.append(f"[Terrain] {', '.join(terrain_flags)}")
        else:
            parts.append("[Terrain] open_area")
        
        # [Director]
        director = state.get("director", "")
        if director:
            parts.append(f"[Director] {director}")
        
        # [Self]
        bot = state.get("bot", {})
        bot_name = bot.get("name", "Bot")
        bot_hp = bot.get("hp", 100)
        bot_temp_hp = bot.get("temp_hp", 0)
        bot_incap = bot.get("incap", False)
        bot_bw = bot.get("bw", False)
        bot_pinned = bot.get("pinned", False)
        bot_pin_type = bot.get("pin_type", "")
        bot_flow = bot.get("flow", 0)
        weapons = bot.get("weapons", {})
        ammo = bot.get("ammo", 0)
        reserve = bot.get("reserve", 0)
        
        status_parts = []
        if bot_incap: status_parts.append("INCAPACITATED")
        if bot_bw: status_parts.append("BLACK&WHITE")
        if bot_pinned: status_parts.append(f"PINNED by {bot_pin_type}" if bot_pin_type else "PINNED")
        
        hp_display = f"HP={bot_hp}"
        if bot_temp_hp > 0:
            hp_display += f"+{bot_temp_hp:.0f}temp"
        
        parts.append(f"\n[Self] {bot_name}: {hp_display} | flow={bot_flow:.0f}{(' ['+','.join(status_parts)+']') if status_parts else ''}")
        parts.append(f"  Primary: {self._clean_weapon_name(weapons.get('primary','none'))} (ammo:{ammo}, reserve:{reserve})")
        parts.append(f"  Secondary: {self._clean_weapon_name(weapons.get('secondary','none'))}")
        parts.append(f"  Grenade: {self._clean_weapon_name(weapons.get('grenade','none'))}")
        parts.append(f"  Health item: {self._clean_weapon_name(weapons.get('health','none'))}")
        parts.append(f"  Pills: {self._clean_weapon_name(weapons.get('pills','none'))}")
        
        # [Teammates] — with pin_type, weapon, temp_hp
        teammates = state.get("teammates", [])
        if teammates:
            parts.append("\n[Teammates]")
            for m in teammates:
                status = []
                if m.get("incap"): status.append("INCAP")
                if m.get("bw"): status.append("B&W")
                pin_type = m.get("pin_type", "")
                if m.get("pinned"): status.append(f"PINNED by {pin_type}" if pin_type else "PINNED")
                status_str = f" [{','.join(status)}]" if status else ""
                
                m_hp = m.get("hp", 0)
                m_temp = m.get("temp_hp", 0)
                hp_str = f"HP={m_hp}"
                if m_temp > 0:
                    hp_str += f"+{m_temp:.0f}temp"
                
                m_weapon = self._clean_weapon_name(m.get("weapon", "none"))
                parts.append(f"  {m.get('name','?')}: {hp_str}, weapon={m_weapon}, dist={m.get('dist',0):.0f}{status_str}")
        else:
            parts.append("\n[Teammates] None nearby")
        
        # [Threats]
        threats = state.get("threats", [])
        if threats:
            parts.append("\n[Threats]")
            for t in threats:
                ghost = " (ghost)" if t.get("ghost") else ""
                parts.append(f"  {t.get('type','?')}: HP={t.get('hp',0)}, dist={t.get('dist',0):.0f}{ghost}")
        else:
            parts.append("\n[Threats] None visible")
        
        # [Witches]
        witches = state.get("witches", [])
        if witches:
            parts.append("\n[Witches]")
            for w in witches:
                angry = " ANGRY!" if w.get("angry") else " calm"
                parts.append(f"  Witch: HP={w.get('hp',0)}, dist={w.get('dist',0):.0f}{angry}")
        
        # [Environment] — common_count, fire, acid
        common_count = state.get("common_count", 0)
        fire_areas = state.get("fire_areas", [])
        acid_areas = state.get("acid_areas", [])
        env_parts = []
        if common_count > 0:
            env_parts.append(f"common_infected_nearby={common_count}" + (" (HORDE!)" if common_count > 10 else ""))
        if fire_areas:
            env_parts.append(f"fire_zones={len(fire_areas)}")
        if acid_areas:
            env_parts.append(f"acid_zones={len(acid_areas)}")
        if env_parts:
            parts.append(f"\n[Environment] {', '.join(env_parts)}")
        
        # [Server Config]
        server = state.get("server", {})
        ff = server.get("ff", False)
        if ff:
            parts.append(f"[Server] Friendly fire ON — watch your fire!")
        
        # [Nearby Items]
        items = state.get("items", [])
        if items:
            parts.append("\n[Nearby Items]")
            for it in items:
                parts.append(f"  {self._clean_weapon_name(it.get('name','?'))}: dist={it.get('dist',0):.0f}")
        
        # [Recent Events]
        events = state.get("events", [])
        if events:
            parts.append(f"\n[Recent Events] {', '.join(events)}")
        
        # [Current Action]
        current_action = state.get("action", "none")
        if current_action and current_action != "none":
            parts.append(f"\n[Current Action] {current_action}")
        
        # [Last Decision]
        if last_action:
            parts.append(f"\n[Last Decision] {last_action.get('action','?')} ({last_action_time_ago:.1f}s ago)")
        
        # [Available Actions]
        parts.append("\n[Available Actions]")
        parts.append("  help_teammate - 救助被控/倒地队友")
        parts.append("  attack_si - 攻击特感")
        parts.append("  attack_tank - 集火Tank")
        parts.append("  attack_witch - 攻击Witch(已惊扰)")
        parts.append("  follow_team - 跟随队伍")
        parts.append("  heal_teammate - 治疗队友")
        parts.append("  evade_threat - 躲避威胁(Tank石头/酸液/火焰)")
        parts.append("  hold_position - 原地防守")
        parts.append("  throw_grenade - 投掷手雷/胆汁/土制炸弹")
        parts.append("  pick_up_item - 拾取物品")
        parts.append("  give_item - 给队友物品")
        parts.append("  cover_fire - 掩护射击")
        parts.append("  use_defib - 使用除颤器救死亡队友")
        
        parts.append('\nReply JSON: {"action":"...","target":"...","reason":"..."}')
        
        return "\n".join(parts)
    
    def get_system_prompt(self) -> str:
        return self.system_prompt
