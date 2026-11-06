"""Watch the serial log for gesture lines for a while.

Usage: gesture_probe.py [seconds]
"""
import sys
import time

sys.path.insert(0, "scripts")
import petctl

link = petctl.open_port("COM3")
seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
start = time.time()
data = b""
while time.time() - start < seconds:
    data += link.read(4096)
link.close()

text = data.decode("utf-8", "replace")
for line in text.splitlines():
    if any(key in line for key in ("Petted", "Fed", "Cleaned", "Played", "gesture", "Exhausted")):
        print(line)
print("---", len(data), "bytes captured")
