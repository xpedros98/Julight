# Julight audio → light (WiFi)

`julight_audio.py` plays an MP3 and streams its **bass energy** to the ESP32 as a
live brightness animation over **WiFi UDP**. Each datagram is one byte (0..255)
that the firmware writes straight to the light, overriding the heart-rate pulse
while the stream runs.

## Setup

1. In `esp32/julight_light/julight_light.ino`, set `WIFI_SSID` / `WIFI_PASSWORD`
   and flash the ESP32. It prints its **IP** and UDP port on the Serial Monitor
   (115200 baud) at boot, e.g. `Audio: streaming brightness bytes to UDP 192.168.1.42:4210`.
2. Install deps (ffmpeg must be on PATH for MP3 decoding):

   ```
   pip install -r requirements.txt
   ```

## Run

```
python julight_audio.py song.mp3 --ip 192.168.1.42
```

Useful flags:

| Flag         | Meaning                                                       |
| ------------ | ------------------------------------------------------------ |
| `--ip`       | ESP32 IP (required, shown on its Serial Monitor)             |
| `--port`     | UDP port (default 4210; must match `AUDIO_UDP_PORT`)         |
| `--gain`     | extra brightness multiplier (default 1.0)                    |
| `--no-audio` | analyse + stream only, don't play sound on this machine      |

The PC and the ESP32 must be on the **same network**. Tune the response in the
`# --- Tunables ---` block at the top of the script (band edges, FPS, attack/decay,
gamma). When the script exits it sends a final `0` so the light goes dark and the
firmware returns to the heart-rate pulse.
