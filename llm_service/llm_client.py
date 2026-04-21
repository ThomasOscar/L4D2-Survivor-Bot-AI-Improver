#!/usr/bin/env python3
"""
L4D2 LLM Bot - LLM Client
支持多种LLM API (DeepSeek, OpenAI兼容, Ollama)
"""

import asyncio
import aiohttp
import logging
from typing import Optional, Dict, Any
from dataclasses import dataclass

logger = logging.getLogger(__name__)

@dataclass
class LLMConfig:
    provider: str = "deepseek"
    api_key: str = ""
    base_url: str = "https://api.deepseek.com/v1"
    model: str = "deepseek-chat"
    max_tokens: int = 150
    temperature: float = 0.3
    timeout: float = 2.0


class LLMClient:
    def __init__(self, config: LLMConfig):
        self.config = config
        self.session: Optional[aiohttp.ClientSession] = None
        
    async def start(self):
        self.session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=self.config.timeout + 1)
        )
        
    async def stop(self):
        if self.session:
            await self.session.close()
            
    async def chat(self, system_prompt: str, user_prompt: str) -> Optional[Dict[str, Any]]:
        """调用LLM API"""
        if not self.session:
            await self.start()
            
        headers = {
            "Content-Type": "application/json",
        }
        
        if self.config.provider == "ollama":
            # Ollama API
            url = f"{self.config.base_url}/api/chat"
            payload = {
                "model": self.config.model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt}
                ],
                "stream": False
            }
        else:
            # OpenAI兼容 API
            headers["Authorization"] = f"Bearer {self.config.api_key}"
            url = f"{self.config.base_url}/chat/completions"
            payload = {
                "model": self.config.model,
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt}
                ],
                "max_tokens": self.config.max_tokens,
                "temperature": self.config.temperature
            }
        
        try:
            async with self.session.post(url, json=payload, headers=headers) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    if self.config.provider == "ollama":
                        return {"content": data.get("message", {}).get("content", "")}
                    else:
                        return data
                else:
                    error_text = await resp.text()
                    logger.error(f"LLM API error: {resp.status} - {error_text}")
                    return None
        except asyncio.TimeoutError:
            logger.error("LLM API timeout")
            return None
        except Exception as e:
            logger.error(f"LLM API error: {e}")
            return None
    
    def parse_response(self, response: Dict[str, Any]) -> str:
        """解析LLM响应，提取内容"""
        try:
            if "choices" in response:
                return response["choices"][0]["message"]["content"]
            elif "content" in response:
                return response["content"]
            return ""
        except Exception as e:
            logger.error(f"Parse response error: {e}")
            return ""
