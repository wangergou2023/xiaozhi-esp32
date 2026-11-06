"""Open the configuration hotspot and dump the serial log for a while."""
import sys
import time

sys.path.insert(0, "scripts")
import petctl

link = petctl.open_port("COM3")
print(petctl.call(link, 1, "pet.config", {"on": True}), flush=True)

start = time.time()
data = b""
while time.time() - start < 40:
    data += link.read(4096)
link.close()

with open("ap_debug.log", "wb") as handle:
    handle.write(data)
print("captured", len(data), "bytes")
