#!/usr/bin/env python3
"""petctl — drive Kitty from a shell. This is the bridge an agent uses.

One JSON-RPC call per invocation: open the port without resetting her, send the
request, print the reply, close. It is deliberately stateless so any agent
framework (or a person in a terminal) can call it.

    python scripts/petctl.py status
    python scripts/petctl.py face emotion=happy intensity=0.9
    python scripts/petctl.py task result=done
    python scripts/petctl.py night on=true
    python scripts/petctl.py say --wav hello.wav

Field names with dashes or ":" are not needed; every parameter is key=value and
numbers/booleans are converted automatically.
"""
import argparse
import base64
import json
import math
import sys
import time

import serial

DEFAULT_PORT = "COM3"
METHODS = {
    "hello": "pet.hello",
    "status": "pet.status",
    "face": "pet.face",
    "look": "pet.look",
    "attention": "pet.attention",
    "feed": "pet.feed",
    "clean": "pet.clean",
    "play": "pet.play",
    "task": "pet.task",
    "sleep": "pet.sleep",
    "wake": "pet.wake",
    "night": "pet.night",
    "time": "pet.time",
    "volume": "pet.volume",
    "config": "pet.config",
    "ping": "pet.ping",
}


def open_port(port: str) -> serial.Serial:
    link = serial.Serial()
    link.port = port
    link.baudrate = 115200
    link.timeout = 0.3
    link.write_timeout = 5
    # Set before open: otherwise opening pulses DTR/RTS and resets the board.
    link.dtr = False
    link.rts = False
    link.open()
    return link


def write_line(link: serial.Serial, payload: bytes) -> None:
    # The device drains the port from its own task; a single big write would
    # overflow the 1 KB RX ring and the line would never complete.
    for offset in range(0, len(payload), 128):
        link.write(payload[offset:offset + 128])
        time.sleep(0.004)


def call(link: serial.Serial, request_id: int, method: str, params: dict, timeout: float = 8.0):
    document = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
    write_line(link, (json.dumps(document) + "\n").encode())
    deadline = time.time() + timeout
    buffer = b""
    while time.time() < deadline:
        buffer += link.read(4096)
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            line = line.strip()
            if not line.startswith(b"{"):
                continue  # a log line sharing the port
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                continue
            if reply.get("id") == request_id:
                return reply
    return None


def parse_value(text: str):
    lowered = text.lower()
    if lowered in {"true", "false"}:
        return lowered == "true"
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def read_wav(path: str):
    import wave

    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if width != 2:
        raise SystemExit(f"{path}: only 16-bit WAV is supported (got {width * 8}-bit)")
    samples = [
        int.from_bytes(frames[i:i + 2], "little", signed=True) for i in range(0, len(frames), 2)
    ]
    if channels > 1:
        samples = samples[::channels]
    if rate != 16000:
        samples = resample(samples, rate, 16000)
    return samples


def resample(samples, source_rate: int, target_rate: int):
    if source_rate == target_rate or not samples:
        return samples
    count = int(len(samples) * target_rate / source_rate)
    out = []
    for index in range(count):
        position = index * source_rate / target_rate
        base = int(position)
        fraction = position - base
        first = samples[base]
        second = samples[base + 1] if base + 1 < len(samples) else first
        out.append(int(first + (second - first) * fraction))
    return out


def say_wav(link: serial.Serial, path: str) -> None:
    samples = read_wav(path)
    print(f"saying {path}: {len(samples)} samples ({len(samples) / 16000:.2f} s)", file=sys.stderr)
    chunk = 1500  # 3 KB of base64 per request keeps each line small
    request_id = 100
    for offset in range(0, len(samples), chunk):
        piece = samples[offset:offset + chunk]
        raw = b"".join(int(value).to_bytes(2, "little", signed=True) for value in piece)
        params = {
            "pcm": base64.b64encode(raw).decode(),
            "first": offset == 0,
            "last": offset + len(piece) >= len(samples),
        }
        reply = call(link, request_id, "pet.say", params, timeout=10.0)
        if reply is None or "error" in reply:
            raise SystemExit(f"pet.say failed: {reply}")
        request_id += 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Talk to Kitty over the USB serial port")
    parser.add_argument("command", help=", ".join(sorted(METHODS)) + ", say")
    parser.add_argument("fields", nargs="*", help="key=value parameters")
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--wav", help="with 'say': play this WAV file")
    args = parser.parse_args()

    params = {}
    for field in args.fields:
        if "=" not in field:
            raise SystemExit(f"expected key=value, got {field!r}")
        key, value = field.split("=", 1)
        params[key] = parse_value(value)

    link = open_port(args.port)
    try:
        if args.command == "say":
            if not args.wav:
                raise SystemExit("say needs --wav <file.wav>")
            say_wav(link, args.wav)
            print(json.dumps({"ok": True, "said": args.wav}))
            return

        method = METHODS.get(args.command)
        if method is None:
            raise SystemExit(f"unknown command {args.command!r}; try one of {sorted(METHODS)}")
        reply = call(link, 1, method, params)
        if reply is None:
            raise SystemExit("no reply from the pet (is it plugged in and awake?)")
        print(json.dumps(reply, ensure_ascii=False))
    finally:
        link.close()


if __name__ == "__main__":
    main()
