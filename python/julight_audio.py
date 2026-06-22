#!/usr/bin/env python3
"""Stream MP3 bass energy to the Julight ESP32 as a live brightness animation.

Plays an MP3 through your speakers and, in lock-step with playback, runs a short
FFT over the audio around the current play position, maps the low-frequency
(bass / kick) energy to a 0..255 brightness value, and fires it to the ESP32 over
UDP at ~FPS frames per second. The ESP32 (julight_light.ino) writes that byte to
the light and falls back to its heart-rate pulse a fraction of a second after the
stream stops.

Each UDP datagram is a single byte: brightness 0..255.

Usage:
    python julight_audio.py song.mp3 --ip 192.168.1.42
    python julight_audio.py song.mp3 --ip 192.168.1.42 --gain 1.5 --no-audio

The ESP32 prints its IP and UDP port at boot (Serial Monitor @ 115200).

Dependencies:  pip install -r requirements.txt   (numpy, librosa, sounddevice)
librosa decodes MP3 via ffmpeg/audioread, so ffmpeg must be on your PATH.
"""

import argparse
import socket
import sys
import time

import numpy as np

# --- Tunables --------------------------------------------------------------
DEFAULT_PORT = 4210      # must match AUDIO_UDP_PORT in julight_light.ino
FPS          = 60        # brightness frames sent per second
FFT_SIZE     = 2048      # samples per analysis window (~46 ms @ 44.1 kHz)
BASS_LO_HZ   = 30.0      # bass band low edge
BASS_HI_HZ   = 150.0     # bass band high edge
GAMMA        = 2.2       # perceptual brightness curve (LED looks linear to the eye)
ATTACK       = 0.6       # envelope rise smoothing (0..1, higher = snappier)
DECAY        = 0.15      # envelope fall smoothing (0..1, lower = longer tail)
AGC_DECAY    = 0.999     # how slowly the running peak forgets (auto-gain)
NOISE_FLOOR  = 1e-6      # ignore near-silence so quiet parts stay dark


def load_mono(path):
    """Load an MP3 to a mono float32 array + sample rate (resampled handled by librosa)."""
    import librosa  # imported here so --help works without the heavy dep
    y, sr = librosa.load(path, sr=None, mono=True)
    return y.astype(np.float32), int(sr)


def bass_brightness(window, sr, agc_peak):
    """Return (brightness 0..255, updated agc_peak) for one analysis window."""
    # Window the slice (Hann) and take the real FFT magnitude spectrum.
    w = window * np.hanning(len(window))
    mag = np.abs(np.fft.rfft(w))
    freqs = np.fft.rfftfreq(len(window), d=1.0 / sr)

    band = (freqs >= BASS_LO_HZ) & (freqs <= BASS_HI_HZ)
    energy = float(np.mean(mag[band])) if band.any() else 0.0

    if energy < NOISE_FLOOR:
        return 0, agc_peak

    # Auto-gain: normalise against a slowly-decaying running peak so any track
    # uses the full brightness range without hand-tuning the gain per song.
    agc_peak = max(energy, agc_peak * AGC_DECAY)
    norm = energy / agc_peak if agc_peak > 0 else 0.0
    norm = min(1.0, max(0.0, norm))

    bright = (norm ** (1.0 / GAMMA)) * 255.0
    return int(bright + 0.5), agc_peak


def main():
    ap = argparse.ArgumentParser(description="Stream MP3 bass energy to the Julight ESP32 over UDP.")
    ap.add_argument("mp3", help="path to the MP3 file")
    ap.add_argument("--ip", required=True, help="ESP32 IP address (printed on its Serial Monitor at boot)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default {DEFAULT_PORT})")
    ap.add_argument("--gain", type=float, default=1.0, help="extra brightness multiplier (default 1.0)")
    ap.add_argument("--no-audio", action="store_true", help="analyse + stream only, don't play sound locally")
    args = ap.parse_args()

    print(f"Loading {args.mp3} ...")
    y, sr = load_mono(args.mp3)
    total = len(y)
    print(f"  {total / sr:.1f}s @ {sr} Hz")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.ip, args.port)

    # Shared playback position (in samples). Updated by the audio callback when
    # playing, or by the clock when --no-audio. The sender reads it each frame.
    pos = {"n": 0}
    start_t = None

    stream = None
    if not args.no_audio:
        try:
            import sounddevice as sd
        except Exception as e:
            print(f"sounddevice unavailable ({e}); falling back to --no-audio", file=sys.stderr)
            args.no_audio = True

    if not args.no_audio:
        def callback(outdata, frames, time_info, status):
            n = pos["n"]
            chunk = y[n:n + frames]
            if len(chunk) < frames:                 # last block: pad with silence
                out = np.zeros(frames, dtype=np.float32)
                out[:len(chunk)] = chunk
                outdata[:, 0] = out
                pos["n"] = total
                raise sd.CallbackStop()
            outdata[:, 0] = chunk
            pos["n"] = n + frames

        stream = sd.OutputStream(samplerate=sr, channels=1, callback=callback, blocksize=1024)
        stream.start()
    else:
        start_t = time.monotonic()

    agc_peak = NOISE_FLOOR
    env = 0.0
    period = 1.0 / FPS
    next_t = time.monotonic()
    half = FFT_SIZE // 2

    print(f"Streaming to {args.ip}:{args.port} at {FPS} fps (Ctrl-C to stop)")
    try:
        while True:
            if args.no_audio:
                n = int((time.monotonic() - start_t) * sr)
                pos["n"] = n
            n = pos["n"]
            if n >= total:
                break

            # Analysis window centred on the current play position.
            lo = max(0, n - half)
            window = y[lo:lo + FFT_SIZE]
            if len(window) < FFT_SIZE:
                window = np.pad(window, (0, FFT_SIZE - len(window)))

            target, agc_peak = bass_brightness(window, sr, agc_peak)
            target = min(255.0, target * args.gain)

            # Asymmetric envelope: fast attack on hits, slow decay for a tail.
            coeff = ATTACK if target > env else DECAY
            env += (target - env) * coeff
            sock.sendto(bytes([int(env) & 0xFF]), dest)

            next_t += period
            sleep = next_t - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
            else:
                next_t = time.monotonic()          # we fell behind; resync
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        if stream is not None:
            stream.stop()
            stream.close()
        sock.sendto(bytes([0]), dest)              # leave the light dark on exit
        sock.close()


if __name__ == "__main__":
    main()
