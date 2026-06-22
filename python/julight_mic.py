#!/usr/bin/env python3
"""Stream live microphone bass energy to the Julight ESP32 as a brightness animation.

Listens to an input device (microphone / line-in / loopback), runs a short FFT over
the most recent samples each frame, maps the low-frequency (bass / kick) energy to a
0..255 brightness value, and fires it to the ESP32 over UDP at ~FPS frames per second
-- exactly like julight_audio.py, but the audio comes from the mic in real time
instead of an MP3 file.

The analysis + shaping (bass band, auto-gain, contrast/gamma curve) is shared with
julight_audio.py via bass_brightness(); only the *source* differs: a rolling buffer
of the latest FFT_SIZE samples filled by the input-stream callback.

Each UDP datagram is a single byte: brightness 0..255.

Usage:
    python julight_mic.py --ip 192.168.1.42
    python julight_mic.py --ip 192.168.1.42 --gain 1.5 --device 2
    python julight_mic.py --list-devices

The ESP32 prints its IP and UDP port at boot (Serial Monitor @ 115200).

Dependencies:  pip install -r requirements.txt   (numpy, sounddevice)
"""

import argparse
import socket
import sys
import time

import numpy as np

# Reuse the tunables + analysis from the file-playback script so both stay in sync.
from julight_audio import (
    DEFAULT_PORT,
    FPS,
    FFT_SIZE,
    AUDIO_MAX,
    ATTACK,
    DECAY,
    NOISE_FLOOR,
    bass_brightness,
)


def pick_input_device():
    """Auto-select a sensible audio input device index, or None to use the default."""
    import sounddevice as sd

    try:
        devices = sd.query_devices()
    except Exception:
        return None

    default_in = sd.default.device[0] if sd.default.device else None
    if default_in is not None and 0 <= default_in < len(devices):
        if devices[default_in].get("max_input_channels", 0) >= 1:
            return default_in

    # Otherwise take the first device that can capture audio.
    for idx, dev in enumerate(devices):
        if dev.get("max_input_channels", 0) >= 1:
            return idx
    return None


def main():
    ap = argparse.ArgumentParser(description="Stream live mic bass energy to the Julight ESP32 over UDP.")
    ap.add_argument("--ip", required=True, help="ESP32 IP address (printed on its Serial Monitor at boot)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default {DEFAULT_PORT})")
    ap.add_argument("--gain", type=float, default=1.0, help="extra brightness multiplier (default 1.0)")
    ap.add_argument("--device", type=int, default=None,
                    help="input device index (default: auto-pick; see --list-devices)")
    ap.add_argument("--list-devices", action="store_true",
                    help="print available audio devices and exit")
    args = ap.parse_args()

    try:
        import sounddevice as sd
    except Exception as e:
        print(f"sounddevice is required for mic capture: {e}", file=sys.stderr)
        sys.exit(1)

    if args.list_devices:
        print(sd.query_devices())
        return

    device = args.device if args.device is not None else pick_input_device()
    if device is not None:
        try:
            info = sd.query_devices(device)
            sr = int(info["default_samplerate"])
            print(f"Audio input -> [{device}] {info['name']} @ {sr} Hz")
        except Exception:
            sr = 44100
            print(f"Audio input -> device {device} @ {sr} Hz (assumed)")
    else:
        info = sd.query_devices(kind="input")
        sr = int(info["default_samplerate"])
        print(f"Audio input -> system default @ {sr} Hz")

    print(f"Opening UDP socket -> {args.ip}:{args.port}")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.ip, args.port)

    # Rolling buffer of the most recent FFT_SIZE samples. The input callback writes
    # the newest block in; the sender loop reads the whole buffer each frame. A
    # name rebind under the GIL is atomic, so no explicit lock is needed.
    state = {"buf": np.zeros(FFT_SIZE, dtype=np.float32)}

    def callback(indata, frames, time_info, status):
        if status:
            print(status, file=sys.stderr)
        x = indata[:, 0]
        if len(x) >= FFT_SIZE:
            state["buf"] = x[-FFT_SIZE:].astype(np.float32).copy()
        else:
            # Slide the window: drop the oldest len(x) samples, append the new block.
            state["buf"] = np.concatenate([state["buf"][len(x):], x.astype(np.float32)])

    agc_peak = NOISE_FLOOR
    env = 0.0
    period = 1.0 / FPS
    next_t = time.monotonic()

    print(f"Streaming to {args.ip}:{args.port} at {FPS} fps (Ctrl-C to stop)")
    stream = sd.InputStream(samplerate=sr, channels=1, callback=callback,
                            blocksize=0, device=device)
    stream.start()
    try:
        while True:
            window = state["buf"]

            target, agc_peak = bass_brightness(window, sr, agc_peak)
            target = min(float(AUDIO_MAX), target * args.gain)

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
        stream.stop()
        stream.close()
        sock.sendto(bytes([0]), dest)              # leave the light dark on exit
        sock.close()


if __name__ == "__main__":
    main()
