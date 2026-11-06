#!/usr/bin/env python3
"""A stdio MCP server that exposes Kitty's agent protocol as tools.

DSH (and any other MCP client) talks JSON-RPC on stdin/stdout; this process turns
each tool call into one pet.* request on the USB serial port and returns the
device's answer as text.

    {"jsonrpc":"2.0","id":1,"method":"initialize",...}
    {"jsonrpc":"2.0","id":2,"method":"tools/list"}
    {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"pet_status","arguments":{}}}

Port: $KITTY_PORT, default COM3.
"""
import json
import os
import sys
import threading
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import petctl  # noqa: E402  (the serial bridge, shared with the CLI)

SERVER_NAME = "kitty"
SERVER_VERSION = "1.0.0"
PROTOCOL_VERSION = "2024-11-05"
PORT = os.environ.get("KITTY_PORT", petctl.DEFAULT_PORT)

TOOLS = [
    {
        "name": "pet_status",
        "description": "Read Kitty's body: hunger, energy, clean, affection, mood, "
                       "growth stage, mode, whether she is asleep, how many times she "
                       "was called by name, and whether it is night.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_face",
        "description": "Set her expression. The art has motion rows, not emotion faces, "
                       "so an emotion picks the closest motion group.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "emotion": {
                    "type": "string",
                    "description": "happy / sad / angry / tired / thinking / neutral ...",
                },
                "intensity": {"type": "number", "minimum": 0, "maximum": 1,
                              "description": "0 calm, 1 animated"},
            },
            "required": ["emotion"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_look",
        "description": "Nudge her face a few pixels. The whole face moves; the eyes are "
                       "part of the frame.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "dx": {"type": "number", "minimum": -100, "maximum": 100},
                "dy": {"type": "number", "minimum": -100, "maximum": 100},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_attention",
        "description": "Make her look up at the user (or drop it again).",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean"}},
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_feed",
        "description": "Feed her. As the agent this only counts half and stops at 85; "
                       "a human can do better. Refused while she sleeps.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_clean",
        "description": "Clean her. Agent care gives half and caps at 85.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_play",
        "description": "Play with her: affection up, a little energy and hunger spent.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_task",
        "description": "Report a finished job. done: energy -5, affection +2. "
                       "error/interrupted: energy -8, affection -1.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "result": {"type": "string", "enum": ["done", "error", "interrupted"]},
            },
            "required": ["result"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_sleep",
        "description": "Put her to sleep by force.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_wake",
        "description": "Wake her up. This also ends night mode.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "pet_night",
        "description": "Enter or leave the night: screen dims, LED off, purr bed on, "
                       "and only wake/status/ping/night answer while it lasts.",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean"}},
            "required": ["on"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_volume",
        "description": "Set her speaker volume, 0-100.",
        "inputSchema": {
            "type": "object",
            "properties": {"value": {"type": "number", "minimum": 0, "maximum": 100}},
            "required": ["value"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_time",
        "description": "Give her the wall clock. She has no RTC and no network, so "
                       "without this the sleep schedule never fires.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "epoch": {"type": "number", "description": "seconds since 1970 (UTC)"},
                "tz_offset": {"type": "number",
                              "description": "minutes from UTC, e.g. 480 for UTC+8"},
            },
            "required": ["epoch"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_say",
        "description": "Make her speak: base64 PCM16 mono at 16 kHz. Stream long "
                       "clips in chunks with first/last flags.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pcm": {"type": "string", "description": "base64 PCM16 mono 16 kHz"},
                "first": {"type": "boolean"},
                "last": {"type": "boolean"},
            },
            "required": ["pcm"],
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_mic_stats",
        "description": "What did the room just sound like? Dominant frequency, rms and "
                       "peak of the last seconds from her microphone.",
        "inputSchema": {
            "type": "object",
            "properties": {"seconds": {"type": "number", "minimum": 1, "maximum": 5}},
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_config",
        "description": "Open (or close) her configuration hotspot: SSID Kitty-XXXX, "
                       "password kitty1234, page at http://192.168.4.1/. It closes "
                       "itself after three minutes or when the form is saved.",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean"}},
            "additionalProperties": False,
        },
    },
    {
        "name": "pet_ping",
        "description": "Heartbeat. Also resets her three-second host timeout.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
]

METHODS = {
    "pet_status": "pet.status",
    "pet_face": "pet.face",
    "pet_look": "pet.look",
    "pet_attention": "pet.attention",
    "pet_feed": "pet.feed",
    "pet_clean": "pet.clean",
    "pet_play": "pet.play",
    "pet_task": "pet.task",
    "pet_sleep": "pet.sleep",
    "pet_wake": "pet.wake",
    "pet_night": "pet.night",
    "pet_volume": "pet.volume",
    "pet_time": "pet.time",
    "pet_say": "pet.say",
    "pet_mic_stats": "pet.mic_stats",
    "pet_config": "pet.config",
    "pet_ping": "pet.ping",
}


def reply(message_id, result):
    payload = {"jsonrpc": "2.0", "id": message_id, "result": result}
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


def error(message_id, code, message):
    payload = {"jsonrpc": "2.0", "id": message_id, "error": {"code": code, "message": message}}
    sys.stdout.write(json.dumps(payload) + "\n")
    sys.stdout.flush()


class Connection:
    """One open port, one heartbeat.

    She hands control to whoever pings within three seconds, so a server that
    opened the port per call would release her between calls: every face would
    snap back a moment later. Holding the port and pinging keeps her in the
    agent's hands for as long as DSH is running.
    """

    def __init__(self, port):
        self.port = port
        self.link = None
        self.lock = threading.Lock()
        self.next_id = 1
        self.stop = threading.Event()
        self.thread = None

    def _request(self, method, params, timeout=8.0):
        request_id = self.next_id
        self.next_id += 1
        return petctl.call(self.link, request_id, method, params, timeout=timeout)

    def _heartbeat(self):
        while not self.stop.wait(1.0):
            with self.lock:
                if self.link is None:
                    continue
                try:
                    self._request("pet.ping", {}, timeout=2.0)
                except Exception:
                    self._drop()

    def _drop(self):
        if self.link is not None:
            try:
                self.link.close()
            except Exception:
                pass
            self.link = None

    def call(self, method, params):
        with self.lock:
            if self.link is None:
                self.link = petctl.open_port(self.port)
                # Take the body over: without pet.hello she stays autonomous and
                # would repaint her own face over whatever we set a tick later.
                self._request("pet.hello", {})
                self.thread = threading.Thread(target=self._heartbeat, daemon=True)
                self.thread.start()
            try:
                return self._request(method, params)
            except Exception:
                self._drop()  # next call reopens; report this one as failed
                raise

    def close(self):
        self.stop.set()
        with self.lock:
            self._drop()


CONNECTION = Connection(PORT)


def call_device(tool_name, arguments):
    method = METHODS[tool_name]
    params = {key: value for key, value in (arguments or {}).items() if value is not None}
    answer = CONNECTION.call(method, params)
    if answer is None:
        return {"content": [{"type": "text", "text": f"no reply from the pet on {PORT}"}],
                "isError": True}
    if "error" in answer:
        return {"content": [{"type": "text", "text": json.dumps(answer["error"], ensure_ascii=False)}],
                "isError": True}
    return {"content": [{"type": "text", "text": json.dumps(answer.get("result"), ensure_ascii=False)}]}


def handle(message):
    method = message.get("method")
    message_id = message.get("id")

    if method == "initialize":
        reply(message_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })
        return
    if method in ("notifications/initialized", "initialized"):
        return
    if method == "ping":
        reply(message_id, {})
        return
    if method == "tools/list":
        reply(message_id, {"tools": TOOLS})
        return
    if method == "tools/call":
        params = message.get("params") or {}
        name = params.get("name")
        if name not in METHODS:
            error(message_id, -32602, f"unknown tool {name!r}")
            return
        try:
            reply(message_id, call_device(name, params.get("arguments")))
        except Exception as failure:  # keep the server alive; report to the client
            traceback.print_exc(file=sys.stderr)
            reply(message_id, {"content": [{"type": "text", "text": f"{type(failure).__name__}: {failure}"}],
                               "isError": True})
        return
    if method is None:
        return  # a response to something we never sent; ignore
    error(message_id, -32601, f"method not found: {method}")


def main():
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                error(None, -32700, "parse error")
                continue
            handle(message)
    finally:
        # Closing the port hands her back to her own life.
        CONNECTION.close()


if __name__ == "__main__":
    main()
