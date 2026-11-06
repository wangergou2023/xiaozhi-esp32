"""Poll pet.battery while the battery is unplugged, to see which pin follows it."""
import sys
import time

sys.path.insert(0, "scripts")
import petctl

link = petctl.open_port("COM3")
try:
    start = time.time()
    while time.time() - start < 90:
        reply = petctl.call(link, 1, "pet.battery", {})
        scan = (reply.get("result") or {}).get("scan") or []
        line = "  ".join("gpio%d=%dmV" % (s["gpio"], s["mv"]) for s in scan)
        print("%5.0fs  %s" % (time.time() - start, line), flush=True)
        time.sleep(3)
finally:
    link.close()
