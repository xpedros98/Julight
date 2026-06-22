# Julight

A [Monkey C](https://developer.garmin.com/connect-iq/monkey-c/) (Garmin Connect IQ) app
that turns a **fenix 7X Solar** into a BLE **central** which connects to a nearby
**ESP32** (BLE peripheral), streams sensor data to it over a GATT link, and reads
signal strength (RSSI) as a rough proximity/distance measure.

## What it does (Option 2 — GATT data link + distance)

The watch:

1. **Scans** for the ESP32, matched by a custom service UUID.
2. Reads **RSSI** from the scan result → crude log-distance estimate shown on screen.
3. **Connects** (pairs) to the ESP32.
4. **Enables notifications** on the notify characteristic.
5. **Writes** sensor payloads to the data characteristic (button press sends a test
   packet today; the 25 Hz accelerometer / heart-rate batching plugs in next — see
   Roadmap).

> Note: a Garmin watch can only act as a BLE *central* — it cannot advertise as a
> peripheral. That's why the other end must be an advertiser (ESP32/nRF/beacon), not a
> second Garmin watch.

## BLE contract (must match the ESP32 firmware)

| Role                | UUID                                   |
| ------------------- | -------------------------------------- |
| Service             | `c3a1f200-9b0e-4f1a-8a01-0e1d2c3b4a59` |
| Data (watch→ESP32, write)   | `c3a1f201-9b0e-4f1a-8a01-0e1d2c3b4a59` |
| Notify (ESP32→watch)        | `c3a1f202-9b0e-4f1a-8a01-0e1d2c3b4a59` |

The ESP32 must **advertise the service UUID** so the watch can find it, expose the data
characteristic as **writable**, and the notify characteristic as **notify** (with a
`0x2902` CCCD descriptor).

## Project layout

```
manifest.xml          app id, fenix7x product, BLE + Sensor permissions
monkey.jungle         build config
source/
  JulightApp.mc       app entry; creates the BleManager
  BleManager.mc       scan / connect / RSSI / GATT read-write (the core)
  JulightView.mc      shows state, RSSI and estimated distance
  JulightDelegate.mc  SELECT = send test packet, BACK = exit
resources/            strings + launcher icon
build.sh              build / run helper
```

## Building

Requires the Connect IQ SDK and a developer key (kept outside the repo at
`~/.garmin_keys/developer_key.der`).

```bash
./build.sh        # compile to bin/Julight.prg
./build.sh run    # compile + launch the simulator
```

Or use the Garmin Monkey C VS Code extension (F5 to run).

## Roadmap

- [x] BLE scan + connect + RSSI/distance display
- [x] Write test payload to the ESP32 over GATT
- [ ] Register the 25 Hz accelerometer listener and batch samples
- [ ] Add heart-rate + stream batched sensor data to the ESP32
- [ ] RSSI smoothing / calibration for a more stable distance estimate
- [ ] Reconnect / power handling
- [x] WiFi audio mode: stream MP3 FFT (bass) → light brightness over UDP (see `python/`)
