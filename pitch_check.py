"""Measure the pitch the pet's speaker actually produces.

Plays a reference tone through her own speaker with pet.say, then pulls the last
two seconds from her microphone ring and estimates the dominant frequency. A
1 kHz tone that comes back at 1 kHz means the whole audio path runs at the rate
it claims; 500 Hz or 2 kHz means it does not.
"""
import importlib.util
import json
import sys
import time

import numpy as np

sys.path.insert(0, "scripts")
spec = importlib.util.spec_from_file_location("petctl", "scripts/petctl.py")
petctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(petctl)

PORT = "COM3"
TONE = r"C:\Users\wangergou\Documents\kitty\tone1k16k.wav"

link = petctl.open_port(PORT)
try:
    print("playing the reference tone through her speaker")
    petctl.say_wav(link, TONE)

    print("pulling the last 2 s from the microphone ring")
    reply = petctl.call(link, 900, "pet.mic", {"seconds": 2})
    if reply is None or "error" in reply:
        raise SystemExit(f"pet.mic failed: {reply}")
    samples = reply["result"]["samples"]
    chunk = reply["result"]["chunk_samples"]

    import base64

    pieces = []
    index = 0
    offset = 0
    while offset < samples:
        response = petctl.call(link, 910 + index, "pet.mic_chunk", {"index": index})
        if response is None or "error" in response:
            raise SystemExit(f"pet.mic_chunk {index} failed: {response}")
        pieces.append(base64.b64decode(response["result"]["data"]))
        offset += chunk
        index += 1
finally:
    link.close()

pcm = b"".join(pieces)
data = np.frombuffer(pcm, dtype="<i2").astype(np.float32)
print(f"captured {len(data)} samples, peak {int(np.max(np.abs(data)))}")

window = data[: len(data) // 2 * 2] * np.hanning(len(data) // 2 * 2)
spectrum = np.abs(np.fft.rfft(window))
frequencies = np.fft.rfftfreq(len(window), d=1.0 / 16000)
peak_index = int(np.argmax(spectrum))
print(f"dominant frequency: {frequencies[peak_index]:.0f} Hz")
