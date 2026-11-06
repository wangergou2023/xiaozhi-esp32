"""Count which idle intents the pet picks over a window, to see if she dozes."""
import sys
import time

sys.path.insert(0, "scripts")
import petctl

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

link = petctl.open_port("COM3")
try:
    start = time.time()
    buf = b""
    while time.time() - start < seconds:
        buf += link.read(4096)
finally:
    link.close()

text = buf.decode("utf-8", "replace")
counts = {}
for line in text.splitlines():
    if "herself:" in line:
        intent = line.split("herself: ")[1].split()[0]
        counts[intent] = counts.get(intent, 0) + 1
print("total idle picks in %.0fs: %d" % (seconds, sum(counts.values())))
for k, v in sorted(counts.items()):
    print("  %-12s %d" % (k, v))
