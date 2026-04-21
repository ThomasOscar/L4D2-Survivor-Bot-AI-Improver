#!/usr/bin/env python3
"""
L4D2 LLM Bot - TCP Server
"""

import asyncio
import json
import logging
from typing import Optional, Callable
from datetime import datetime

logger = logging.getLogger(__name__)

class TCPServer:
    def __init__(self, host: str = "0.0.0.0", port: int = 9876):
        self.host = host
        self.port = port
        self.server: Optional[asyncio.Server] = None
        self.clients = set()
        self.on_message: Optional[Callable] = None
        self.running = False
        
    async def start(self):
        self.server = await asyncio.start_server(
            self._handle_client, self.host, self.port
        )
        self.running = True
        addr = self.server.sockets[0].getsockname()
        logger.info(f"TCP Server started on {addr[0]}:{addr[1]}")
        async with self.server:
            await self.server.serve_forever()
    
    async def stop(self):
        self.running = False
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        logger.info("TCP Server stopped")
    
    async def _handle_client(self, reader, writer):
        addr = writer.get_extra_info("peername")
        logger.info(f"Client connected: {addr}")
        self.clients.add(writer)
        buffer = ""
        try:
            while self.running:
                data = await reader.read(4096)
                if not data:
                    break
                buffer += data.decode("utf-8")
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        # 支持两种格式: "STATE {...}" 或纯JSON
                        if line.startswith("STATE "):
                            json_part = line[6:]  # 去掉 "STATE " 前缀
                            message = {"type": "state", "data": json.loads(json_part)}
                            logger.info(f"Received STATE: {json_part[:1500]}")
                        else:
                            message = json.loads(line)
                        await self._process_message(message, writer)
                    except json.JSONDecodeError as e:
                        logger.error(f"JSON decode error: {e} - raw: {line[:100]}")
        except Exception as e:
            logger.error(f"Client handler error: {e}")
        finally:
            self.clients.discard(writer)
            writer.close()
            logger.info(f"Client disconnected: {addr}")
    
    async def _process_message(self, message: dict, writer):
        msg_type = message.get("type", "unknown")
        seq = message.get("seq", 0)
        if msg_type == "ping":
            response = {"type": "pong", "ts": datetime.now().timestamp(), "seq": seq}
            await self._send_response(writer, response)
        elif msg_type == "state":
            if self.on_message:
                try:
                    decision = await self.on_message(message.get("data", {}))
                    response = {"type": "decision", "data": decision, "seq": seq}
                except Exception as e:
                    logger.error(f"Decision error: {e}")
                    response = {"type": "error", "data": {"error": str(e)}, "seq": seq}
                await self._send_response(writer, response)
            else:
                # 没有决策处理器时返回简单确认
                response = {"type": "ack", "seq": seq}
                await self._send_response(writer, response)
    
    async def _send_response(self, writer, response: dict):
        try:
            data = json.dumps(response) + "\n"
            writer.write(data.encode("utf-8"))
            await writer.drain()
            logger.debug(f"Sent: {data[:100]}")
        except Exception as e:
            logger.error(f"Send response error: {e}")
