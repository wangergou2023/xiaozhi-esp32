#!/usr/bin/env python3
"""Play the part of the host agent: talk JSON-RPC to Kitty over the USB port.

This is the "fake agent" from docs/pet-design.md §14 (V1): it exercises every
primitive the firmware exposes, so the protocol can be validated without any
agent framework.

Protocol (§9): one JSON document per line. `pet.hello` takes the body over,
`pet.ping` has to arrive at least every three seconds, and the device also
writes its log lines to the same port -- anything that is not JSON is ignored.

Usage:
    python scripts/pet_agent.py [COM3]
"""
import json
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"


class Kitty:
    def __init__(self, port: str):
        self.link = serial.Serial(port, 115200, timeout=0.2, write_timeout=3)
        # Never touch DTR/RTS: on USB-Serial/JTAG they are wired to reset and boot.
        self.link.dtr = False
        self.link.rts = False
        self.next_id = 1
        self.buffer = b""

    def call(self, method: str, **params) -> int:
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        self.next_id += 1
        self.link.write((json.dumps(request) + "\n").encode())
        return self.next_id - 1

    def pump(self, seconds: float) -> list:
        """Read for a while; print and return every protocol document."""
        documents = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.buffer += self.link.read(4096)
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                line = line.strip()
                if not line.startswith(b"{"):
                    continue  # a log line sharing the port
                try:
                    document = json.loads(line)
                except json.JSONDecodeError:
                    continue
                documents.append(document)
                print(json.dumps(document, ensure_ascii=False))
        return documents

    def close(self) -> None:
        self.link.close()


def main() -> None:
    kitty = Kitty(PORT)

    print("== hello: hand over the body ==")
    kitty.call("pet.hello")
    time.sleep(0.3)
    kitty.pump(0.7)

    print("== status ==")
    kitty.call("pet.status")
    time.sleep(0.2)
    kitty.pump(0.5)

    print("== face, look, attention ==")
    kitty.call("pet.face", emotion="happy", intensity=0.9)
    time.sleep(0.3)
    kitty.call("pet.look", dx=-60, dy=20)
    time.sleep(0.3)
    kitty.call("pet.attention", on=True)
    time.sleep(0.3)
    kitty.pump(0.5)

    print("== care ==")
    kitty.call("pet.feed")
    time.sleep(0.2)
    kitty.call("pet.play")
    time.sleep(0.2)
    kitty.pump(0.4)

    print("== work costs her energy (§6) ==")
    kitty.call("pet.task", result="done")
    time.sleep(0.2)
    kitty.call("pet.task", result="error")
    time.sleep(0.2)
    kitty.pump(0.4)

    print("== sleep and wake ==")
    kitty.call("pet.sleep")
    time.sleep(0.3)
    kitty.call("pet.wake")
    time.sleep(0.3)
    kitty.pump(0.4)

    print("== unsupported primitives answer with an error ==")
    kitty.call("pet.say", text="hello")
    time.sleep(0.2)
    kitty.call("pet.teleport")
    time.sleep(0.2)
    kitty.pump(0.5)

    print("== heartbeat: six pings keep her ==")
    for _ in range(6):
        kitty.call("pet.ping")
        time.sleep(1.0)
        kitty.pump(0.05)

    print("== silence: after three seconds she is her own again ==")
    kitty.pump(4.5)

    kitty.close()


if __name__ == "__main__":
    main()
