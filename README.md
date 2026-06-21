# Julight

A [Monkey C](https://developer.garmin.com/connect-iq/monkey-c/) (Garmin Connect IQ) app for collecting sensor data on a Garmin device and sending it to a remote endpoint over **HTTPS** or **Bluetooth**.

## Goal

Capture sensor data — primarily the **accelerometer** (~25 Hz) and **heart rate** — on a Garmin wearable and transmit it off-device for further processing.

## Approaches

### A) Foreground "session" app (recommended for accelerometer)

- **App type:** Watch App. The user opens it and it runs in the foreground while collecting.
- Register `Sensor.registerSensorDataListener({:period => 1, :accelerometer => {...}})` for ~25 Hz accelerometer data, plus `Sensor.enableSensorEvents` / heart-rate.
- Batch samples and POST them with `Communications.makeWebRequest(url, params, options, callback)` over HTTPS.
- **Downside:** only collects while the app is open on screen.

### B) Background sync (for low-rate data like periodic HR)

- A foreground app records into a FIT file or `Storage`, and a `ServiceDelegate` registered via `Background.registerForTemporalEvent(new Time.Duration(300))` wakes every 5 minutes to POST the latest values.
- Good for "send current HR / steps every few minutes."
- **Not** suitable for raw accelerometer data.

## Status

Early planning. No source code yet.

## Tooling

- [Connect IQ SDK](https://developer.garmin.com/connect-iq/sdk/)
- Monkey C language
