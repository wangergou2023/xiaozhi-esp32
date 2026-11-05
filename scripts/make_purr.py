"""Turn the public-domain purring recording into the night bed the firmware embeds.

Source: https://commons.wikimedia.org/wiki/File:Purring_cat.oga
   "A purring cat", recorded 2007 by Mysid, released into the public domain.

The whole recording is kept (9.66 s at 16 kHz mono), the ends are crossfaded so
the loop point is inaudible, and it is low-passed at 1.2 kHz because a purr lives
below about 1 kHz while the recording's hiss does not.

    python scripts/make_purr.py            # writes main/pet/assets/purr.pcm
"""
import wave

import numpy as np
import soundfile as sf

SOURCE = "purr/purring_cat.oga"
OUT_PCM = "xiaozhi-esp32/main/pet/assets/purr.pcm"
TARGET_RATE = 16000
LOOP_SECONDS = None  # None: keep the whole recording


def load_mono(path):
    data, rate = sf.read(path, dtype="float32")
    if data.ndim > 1:
        data = data.mean(axis=1)
    return data, rate


def resample(signal, source_rate, target_rate):
    if source_rate == target_rate:
        return signal
    count = int(len(signal) * target_rate / source_rate)
    positions = np.linspace(0, len(signal) - 1, count)
    return np.interp(positions, np.arange(len(signal)), signal)


def lowpass(signal, rate, cutoff_hz, taps=257):
    """Windowed-sinc low-pass; keeps the rumble, drops the hiss."""
    if cutoff_hz >= rate / 2:
        return signal
    n = np.arange(taps) - (taps - 1) / 2
    cutoff = cutoff_hz / rate
    kernel = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(taps)
    kernel /= kernel.sum()
    return np.convolve(signal, kernel, mode="same")


def best_window(signal, rate, seconds):
    """The steadiest stretch: highest, most constant energy."""
    length = int(seconds * rate)
    if length >= len(signal):
        return 0
    step = rate // 4
    best_index, best_score = 0, -1.0
    for start in range(0, len(signal) - length, step):
        window = signal[start:start + length]
        frames = window[: len(window) // (rate // 20) * (rate // 20)]
        blocks = frames.reshape(-1, rate // 20)  # 50 ms blocks
        energies = np.sqrt((blocks ** 2).mean(axis=1))
        if energies.mean() <= 0:
            continue
        score = energies.mean() / (1.0 + energies.std())
        if score > best_score:
            best_score, best_index = score, start
    return best_index


def purr_period(signal, rate):
    """Autocorrelation between 10 ms and 100 ms -> one purr cycle."""
    segment = signal[: rate // 2]
    segment = segment - segment.mean()
    correlation = np.correlate(segment, segment, mode="full")[len(segment) - 1:]
    low = int(0.010 * rate)
    high = int(0.100 * rate)
    lag = low + int(np.argmax(correlation[low:high]))
    return lag, rate / lag


def main():
    data, rate = load_mono(SOURCE)
    print(f"source: {len(data) / rate:.2f} s at {rate} Hz")

    if LOOP_SECONDS is None:
        # Keep the whole recording; only blend the ends so the loop does not click.
        loop = data.copy()
        print(f"keeping the whole recording: {len(loop) / rate:.2f} s")
    else:
        index = best_window(data, rate, LOOP_SECONDS)
        print(f"steady window starts at {index / rate:.2f} s")
        period, purr_hz = purr_period(data[index:index + rate], rate)
        print(f"purr cycle: {period} samples (~{purr_hz:.1f} Hz)")
        length = int(round(LOOP_SECONDS * rate / period)) * period
        loop = data[index:index + length].copy()

    loop = resample(loop, rate, TARGET_RATE)
    loop = lowpass(loop, TARGET_RATE, 1200.0)

    # Crossfade the ends over 120 ms so the loop point is inaudible.
    fade = int(0.12 * TARGET_RATE)
    ramp = np.linspace(0.0, 1.0, fade)
    loop[:fade] = loop[:fade] * ramp + loop[-fade:] * (1.0 - ramp)
    loop = loop[: len(loop) - fade]

    peak = float(np.max(np.abs(loop))) or 1.0
    loop = loop / peak * 0.85
    print(f"loop: {len(loop) / TARGET_RATE:.2f} s at {TARGET_RATE} Hz")

    pcm = (loop * 32767.0).astype("<i2")
    with open(OUT_PCM, "wb") as handle:
        handle.write(pcm.tobytes())
    print(f"wrote {OUT_PCM}: {len(pcm) * 2} bytes")

    with wave.open("purr/purr_preview.wav", "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(TARGET_RATE)
        handle.writeframes(pcm.tobytes())

    spectrum = np.abs(np.fft.rfft(loop * np.hanning(len(loop))))
    freqs = np.fft.rfftfreq(len(loop), 1 / TARGET_RATE)
    band = lambda low, high: spectrum[(freqs >= low) & (freqs < high)].sum()
    total = spectrum.sum()
    print(f"energy below 200 Hz: {band(0, 200) / total * 100:.0f}%, "
          f"200-1000 Hz: {band(200, 1000) / total * 100:.0f}%, "
          f"above 1 kHz: {band(1000, 8000) / total * 100:.0f}%")


if __name__ == "__main__":
    main()
