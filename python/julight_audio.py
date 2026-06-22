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
AUDIO_MAX    = 200       # ceiling for music brightness; 255 stays reserved for
                         # other feedback (calibration flashes / system signals)
CONTRAST     = 1.8       # >1 deepens the lulls so beats stand out more evidently
ATTACK       = 0.6       # envelope rise smoothing (0..1, higher = snappier)
DECAY        = 0.15      # envelope fall smoothing (0..1, lower = longer tail)
AGC_DECAY    = 0.999     # how slowly the running peak forgets (auto-gain)
NOISE_FLOOR  = 1e-6      # ignore near-silence so quiet parts stay dark


# Output devices we want to avoid auto-selecting: monitor / HDMI / DisplayPort
# audio sinks that usually have no real speakers attached.
# Note: the MME back-end truncates device names to 31 chars, so long names like
# "...Sonido Intel(R) para pantallas" arrive cut off before "pantallas" -- match
# on "intel" too (Intel audio is display/HDMI; the analog codec here is Realtek).
_DISPLAY_HINTS = ("pantalla", "display", "hdmi", "digital", "nvidia",
                  "para pantallas", "intel")
# Output devices we prefer when auto-selecting (real speakers / headphones).
_SPEAKER_HINTS = ("altavoces", "speakers", "headphones", "auricular",
                  "realtek")


def pick_output_device():
    """Auto-select a sensible audio output device index, or None on failure.

    The system default output is often a monitor's display-audio sink (e.g.
    "Sonido Intel para pantallas"), which has no speakers, so playback is
    silent. We score every output-capable device: penalise display/HDMI sinks,
    reward real speaker/headphone names, and keep the system default if it
    already looks like a real output.
    """
    import sounddevice as sd

    try:
        devices = sd.query_devices()
        hostapis = sd.query_hostapis()
    except Exception:
        return None

    default_out = sd.default.device[1] if sd.default.device else None

    def is_display(name):
        n = name.lower()
        return any(h in n for h in _DISPLAY_HINTS)

    def is_speaker(name):
        n = name.lower()
        return any(h in n for h in _SPEAKER_HINTS)

    best, best_score = None, -1e9
    for idx, dev in enumerate(devices):
        if dev.get("max_output_channels", 0) < 1:
            continue
        name = dev.get("name", "")
        score = 0.0
        if is_display(name):
            score -= 100
        if is_speaker(name):
            score += 50
        # Prefer WASAPI/DirectSound over the legacy MME/WDM-KS back-ends.
        api = hostapis[dev["hostapi"]]["name"].lower()
        if "wasapi" in api:
            score += 8
        elif "directsound" in api:
            score += 4
        if idx == default_out:
            score += 5            # tie-breaker toward the OS default
        if score > best_score:
            best, best_score = idx, score

    # If the OS default is positively a real speaker/headphone output, keep it:
    # it respects the user's chosen device and per-app volume. (We require a
    # positive speaker match rather than merely "not display" because truncated
    # MME names can hide a display sink.)
    if default_out is not None and 0 <= default_out < len(devices):
        dname = devices[default_out].get("name", "")
        if is_speaker(dname) and not is_display(dname):
            return default_out
    return best


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

    # Expand dynamics first (CONTRAST > 1 darkens the quiet parts so beats pop),
    # then the perceptual gamma, then scale into the reserved music range so the
    # light never hits full 255 (kept for other feedback).
    shaped = norm ** CONTRAST
    bright = (shaped ** (1.0 / GAMMA)) * AUDIO_MAX
    return int(bright + 0.5), agc_peak


def main():
    ap = argparse.ArgumentParser(description="Stream MP3 bass energy to the Julight ESP32 over UDP.")
    ap.add_argument("mp3", help="path to the MP3 file")
    ap.add_argument("--ip", required=True, help="ESP32 IP address (printed on its Serial Monitor at boot)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default {DEFAULT_PORT})")
    ap.add_argument("--gain", type=float, default=1.0, help="extra brightness multiplier (default 1.0)")
    ap.add_argument("--no-audio", action="store_true", help="analyse + stream only, don't play sound locally")
    ap.add_argument("--device", type=int, default=None,
                    help="output device index (default: auto-pick real speakers; see --list-devices)")
    ap.add_argument("--list-devices", action="store_true",
                    help="print available audio devices and exit")
    args = ap.parse_args()

    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        return

    print(f"Loading {args.mp3} ...")
    y, sr = load_mono(args.mp3)
    total = len(y)
    print(f"  {total / sr:.1f}s @ {sr} Hz")

    print(f"Opening UDP socket -> {args.ip}:{args.port}")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.ip, args.port)

    # Shared playback position (in samples). Updated by the audio callback when
    # playing, or by the clock when --no-audio. The sender reads it each frame.
    pos = {"n": 0}
    start_t = None

    stream = None
    if not args.no_audio:
        print("Initialising audio (sounddevice) ...")
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

        device = args.device if args.device is not None else pick_output_device()
        if device is not None:
            try:
                info = sd.query_devices(device)
                print(f"Audio output -> [{device}] {info['name']}")
            except Exception:
                print(f"Audio output -> device {device}")
        else:
            print("Audio output -> system default")

        # Some Windows output devices (shared-mode WASAPI/MME) only accept their
        # configured rate -- often 48000 Hz -- and reject the file's native rate
        # with "Invalid sample rate [PaErrorCode -9997]". Probe the device and,
        # if it won't take `sr`, resample the audio (and analysis) to a rate it
        # does accept. The callback closes over `y`/`total`, so reassigning them
        # here is enough to keep playback and analysis in sync.
        try:
            sd.check_output_settings(device=device, samplerate=sr, channels=1)
        except Exception:
            probe = sd.query_devices(device if device is not None else None,
                                     kind="output")
            dev_sr = int(probe["default_samplerate"])
            print(f"  device rejects {sr} Hz; resampling audio to {dev_sr} Hz")
            import librosa
            y = librosa.resample(y, orig_sr=sr, target_sr=dev_sr).astype(np.float32)
            sr = dev_sr
            total = len(y)
            pos["n"] = 0

        stream = sd.OutputStream(samplerate=sr, channels=1, callback=callback,
                                 blocksize=1024, device=device)
        print("Starting playback ...")
        stream.start()
    else:
        print("No-audio mode: analysing + streaming only")
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
        if stream is not None:
            stream.stop()
            stream.close()
        sock.sendto(bytes([0]), dest)              # leave the light dark on exit
        sock.close()


if __name__ == "__main__":
    main()
