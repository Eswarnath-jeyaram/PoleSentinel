<div align="center">

# 🛰️ PoleSentinel

**Predictive safety and maintenance monitoring for streetlight infrastructure**

A low-cost retrofit that turns an ordinary streetlight pole into a continuously
monitored asset — detecting vehicle impacts, structural lean and luminaire
faults before anyone has to drive out and look at it.

![Platform](https://img.shields.io/badge/platform-ESP32-000000?style=flat-square)
![Language](https://img.shields.io/badge/firmware-C%2B%2B-00599C?style=flat-square)
![Radio](https://img.shields.io/badge/radio-LoRa%20433MHz-ff6b35?style=flat-square)
![Cloud](https://img.shields.io/badge/cloud-Firebase%20RTDB-FFCA28?style=flat-square)
![Frontend](https://img.shields.io/badge/frontend-Vanilla%20JS-f7df1e?style=flat-square)
![Status](https://img.shields.io/badge/status-working%20prototype-3fb27f?style=flat-square)

</div>

---

## Contents

- [The problem](#the-problem)
- [What PoleSentinel does](#what-polesentinel-does)
- [System architecture](#system-architecture)
- [Detection logic](#detection-logic)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Telemetry format](#telemetry-format)
- [Dashboard](#dashboard)
- [Getting started](#getting-started)
- [Calibration](#calibration)
- [Engineering notes](#engineering-notes)
- [Measured performance](#measured-performance)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)
- [Repository layout](#repository-layout)
- [License](#license)

---

## The problem

Streetlight infrastructure is maintained **reactively**. A fault is usually
discovered only after the light has already failed, the pole has already been
hit, or a resident has already complained.

Manual inspection of thousands of poles is expensive and still leaves long
blind windows between visits. A pole that is struck by a vehicle at 11pm and
left leaning at 9° is invisible to the municipality until someone reports it.

> **Core question:** how do you detect unsafe or failing streetlight
> infrastructure *before* complete failure, using a retrofit cheap enough to
> deploy at city scale?

---

## What PoleSentinel does

PoleSentinel bolts a sensing node onto an existing pole and watches two things
at once:

| Domain | Sensor | What it sees |
|---|---|---|
| **Mechanical** | MPU6050 (6-axis IMU) | vibration, impact spikes, orientation drift |
| **Electrical** | ACS712 (hall-effect current) | luminaire load current vs. its calibrated band |

Neither signal alone is very interesting. **Correlating them is the point.**

| Mechanical | Electrical | Interpretation |
|---|---|---|
| normal | normal | healthy |
| spike, then recovery | normal | vehicle impact |
| slow orientation drift | normal | foundation movement / progressive lean |
| normal | out of band | luminaire or wiring fault |
| spike + orientation shift | out of band | severe event — dispatch immediately |

The node classifies the event **at the edge** and transmits a latched verdict,
not a firehose of raw samples.

---

## System architecture

```
┌─────────────────────────── POLE NODE ───────────────────────────┐
│                                                                 │
│   MPU6050 ──I²C──┐                                              │
│                  ├──► ESP32 ──► filter · baseline · classify    │
│   ACS712  ──ADC──┘              │                               │
│                                 ▼                               │
│                          latched event flags                    │
│                                 │                               │
└─────────────────────────────────┼───────────────────────────────┘
                                  │  LoRa 433 MHz, 1 Hz
                                  │  comma-separated payload
┌─────────────────────────────────┼───────────────────────────────┐
│                                 ▼          GATEWAY NODE         │
│                          ESP32 + RA-02                          │
│                                 │                               │
│                    Wi-Fi ──► HTTP PUT ──► Firebase RTDB         │
│                                                                 │
└─────────────────────────────────┼───────────────────────────────┘
                                  │  REST, polled at 1 Hz
                                  ▼
                        ┌───────────────────┐
                        │  Web console      │
                        │  (Vercel, static) │
                        └───────────────────┘
```

**Why the edge does the classifying.** If the radio drops a packet, a
transient event computed in the cloud would be lost forever. Latching the flag
on the node means a 200 ms impact still reaches the dashboard even if the next
three transmissions fail.

**Why LoRa.** Cellular per-pole is unaffordable at fleet scale and Wi-Fi does
not reach. LoRa gives kilometre-class range at sub-milliwatt average power,
and one gateway serves an entire neighbourhood of poles.

---

## Detection logic

Five independent detectors run on the node:

### 1 · Vehicle impact
A sudden acceleration spike **that recovers toward baseline within 800 ms**.
The recovery window is what separates a real strike from a passing truck — a
truck produces vibration that decays with no lasting orientation change.

### 2 · Pole displaced
Orientation more than **8°** from the vector captured at installation, held
continuously for over a second. Computed as the angle between the current and
baseline acceleration vectors:

```
θ = arccos( (a⃗ · a⃗₀) / (|a⃗| · |a⃗₀|) )
```

This is immune to which way the sensor happens to be mounted — it only cares
about deviation from wherever it started.

### 3 · Progressive lean
A sustained **4°+** deviation with no impact signature. Slow movement without
a strike suggests foundation settling, so this schedules an inspection rather
than an emergency callout.

### 4 · Luminaire fault
Load current outside its calibrated band. See
[Calibration](#calibration) — the thresholds are derived from measured noise,
not guessed.

### 5 · Sustained vibration
Broadband energy above the quiet baseline, measured as the residual after a
low-pass filter:

```
vibration = | a⃗ − lowpass(a⃗) |
```

---

## Hardware

| Part | Role | Notes |
|---|---|---|
| ESP32 DevKit V1 ×2 | node + gateway | one transmits, one bridges to Wi-Fi |
| MPU6050 (GY-521) | 6-axis IMU | **power from 5V/VIN**, not 3.3V — see notes |
| ACS712-30A | current sense | hall-effect, galvanically isolated from load |
| LoRa RA-02 ×2 | radio link | 433 MHz, SX1278, **3.3V only** |
| 10 µF electrolytic | rail decoupling | prevents LoRa init brownout |
| 10 kΩ ×2 | ADC divider | ACS712 out → GPIO34 |
| 220 Ω + LED | demo load | stands in for the luminaire |

---

## Wiring

### Pole node

```
MPU6050            ESP32
  VCC  ──────────► VIN (5V)     ⚠ NOT 3.3V — onboard LDO needs headroom
  GND  ──────────► GND
  SDA  ──────────► GPIO 21
  SCL  ──────────► GPIO 22

LoRa RA-02         ESP32
  VCC  ──────────► 3.3V         ⚠ 5V will destroy the module
  GND  ──────────► GND
  SCK  ──────────► GPIO 18
  MISO ──────────► GPIO 19
  MOSI ──────────► GPIO 23
  NSS  ──────────► GPIO 5
  RST  ──────────► GPIO 14
  DIO0 ──────────► GPIO 26

ACS712             ESP32
  VCC  ──────────► 5V
  GND  ──────────► GND
  OUT  ──[10k]──► GPIO 34 ──[10k]──► GND
```

### Load circuit — keep this isolated

```
  PSU (+) ──► ACS712 terminal 1
              ACS712 terminal 2 ──► 220Ω ──► LED(+)
                                             LED(−) ──► PSU (−)
```

> ⚠️ **The load supply and the ESP32 supply must not share a breadboard rail.**
> Bridging them feeds unregulated supply voltage onto VIN. During development
> this project measured **7 V on the MPU6050 VCC pin** from exactly this
> mistake. Use separate rails; the ACS712 is the only component that legally
> spans both sides.

> ⚠️ Never wire mains voltage to a breadboard prototype. The demo uses a safe
> low-voltage DC load. Real deployment requires certified isolation and a
> licensed electrician.

---

## Telemetry format

One comma-separated line per second, 10 fields:

```
SL-001,HEALTHY,16384,0.82,-1.4,0,0,0,0,0
   │      │      │     │    │  │ │ │ │ └─ vibration     (0|1)
   │      │      │     │    │  │ │ │ └─── electrical    (0|1)
   │      │      │     │    │  │ │ └───── lean warning  (0|1)
   │      │      │     │    │  │ └─────── displaced     (0|1, latched)
   │      │      │     │    │  └───────── impact        (0|1, latched)
   │      │      │     │    └──────────── current Δ     (ADC counts)
   │      │      │     └───────────────── tilt          (degrees)
   │      │      └─────────────────────── accel magnitude (mg)
   │      └────────────────────────────── overall status
   └───────────────────────────────────── pole id
```

Firebase document shape:

```json
{
  "poles": {
    "SL-001": {
      "payload": "SL-001,HEALTHY,16384,0.82,-1.4,0,0,0,0,0",
      "ts": 1758240000
    }
  }
}
```

---

## Dashboard

A single static `index.html` — no build step, no framework, deploys to Vercel
as-is.

**What it shows**

- **Verdict bar** — overall state plus a weighted 0–100 health index
- **Live instruments** — tilt (with an SVG gauge that mirrors the real angle),
  acceleration and current deviation, each with a 60-sample sparkline
- **Threat matrix** — the five detectors, latched, with plain-language
  descriptions of what each one actually means
- **Event log** — state *transitions* only, so it reads like an ops log rather
  than a scrolling dump
- **Node panel** — packet age, packets seen, link uptime, gap count

**Staleness watchdog.** If no packet arrives for 6 seconds the console flips
to `OFFLINE` and states that the readings are last-known, not live. This
matters: a safety dashboard that keeps showing a cheerful green `HEALTHY`
after the node has died is worse than no dashboard at all.

**Alarm acknowledgement.** Latched impact and displacement flags can be
cleared by an operator from the UI, so a crew can close out an incident
without power-cycling the pole.

---

## Getting started

### Firmware

1. Install the Arduino IDE with the **esp32** board package.
2. Install libraries: `Adafruit MPU6050`, `Adafruit Unified Sensor`,
   `LoRa` (Sandeep Mistry).
3. Flash `firmware/pole_node/` to the node ESP32.
4. Flash `firmware/gateway/` to the gateway ESP32, filling in your Wi-Fi SSID,
   password and Firebase URL.

### Dashboard

```bash
git clone https://github.com/<you>/polesentinel.git
cd polesentinel/dashboard
# edit URL_DB in index.html to point at your Firebase RTDB
npx vercel deploy
```

Or drag the `dashboard/` folder onto vercel.com.

### Verify the I²C bus first

If the node reports `IMU SENSOR FAULT`, run `tools/i2c_scanner/` before
debugging anything else. A healthy MPU6050 answers at `0x68`
(or `0x69` if AD0 is pulled high).

---

## Calibration

Thresholds in this project are **measured, not guessed.** Run
`tools/acs_diagnostic/`, which prints mean, standard deviation, min and max
per window.

Procedure:

1. Log ~30 s with the load connected → `healthyMean`, `healthyStd`
2. Log ~30 s with the load disconnected → `faultMean`, `faultStd`
3. Confirm the states actually separate:

```
| healthyMean − faultMean |  ≫  ( healthyStd + faultStd )
```

If they overlap, no software threshold will ever be reliable — reduce noise in
hardware first (smaller series resistors, or 0.1 µF from the ADC pin to GND).

4. Derive thresholds from your own noise floor:

```
ON_THRESHOLD  = healthyStd × 4      // trip
OFF_THRESHOLD = healthyStd × 1.5    // release (hysteresis)
```

Then set the IMU baseline by holding the node still at installation — that
stored vector becomes "vertical" for this pole.

---

## Engineering notes

Problems hit during the build and how they were solved. These are the
interesting bits.

**LoRa brownout on init.** The radio's TX current spike collapsed the 3.3 V
rail during `LoRa.begin()`. Fixed with a 10 µF electrolytic across the rail.

**MPU6050 silent on a 3.3 V supply.** The GY-521 breakout's onboard LDO needs
input headroom above 3.3 V. Moving VCC to VIN/5 V brought it up immediately —
the chip is still driven at 3.3 V internally and its I²C lines stay 3.3 V-safe.

**I²C bus freeze.** A wedged transaction would hang the loop forever. Every
read now returns a status and a timeout is set, so a bad transaction degrades
to a `SENSOR FAULT` flag instead of a dead node.

**SPI too fast for a breadboard.** Contact resistance on jumper wires
corrupted LoRa transfers. Dropping SPI to 1 MHz made the link reliable.

**The ACS712 signal was smaller than the noise.** A 30 A sensor reading a
~20 mA LED produces a few millivolts. Solved with two-stage averaging
(15 subgroups × 10 samples) which cuts the effective noise by √N, plus
hysteresis and consecutive-sample confirmation so the status cannot flicker.

**ACS712 thermal drift.** The baseline drifted ~17 ADC counts during the first
15 seconds of warm-up — comparable to the fault signal itself. Fixed with a
15 s settling delay before calibration, and a very slow adaptive baseline
(α = 0.0005) that tracks long-term drift but is **frozen whenever a fault is
active or being confirmed**, so a real failure can never be absorbed into the
baseline and silently reclassified as healthy.

---

## Measured performance

Real numbers from the prototype, ACS712-30A with a 20 mA LED load:

| Quantity | Value |
|---|---|
| Healthy load, mean ADC | ~1491 counts |
| Disconnected, mean ADC | ~1480 counts |
| Signal separation | **~11 counts** |
| Raw per-sample noise (σ) | ~5 counts |
| Noise after 150-sample averaging (σ/√N) | **~0.4 counts** |
| Effective separation | **~27σ** |
| Detection latency | ~1.5 s |
| Warm-up drift, first 15 s | ~17 counts |

The 1.5 s latency is a deliberate trade: for a sensor this far outside its
intended range, heavy averaging is the only thing that makes the measurement
trustworthy, and 1.5 s is irrelevant for a maintenance decision.

---

## Known limitations

Stated plainly, because a prototype that oversells itself is worse than one
that doesn't.

- **Thresholds are demonstration values.** Real deployment requires
  per-pole-design thresholds validated against structural engineering data.
  Nothing here is a certified safety limit.
- **The ACS712-30A is badly oversized** for the demo load. A real 100 W
  luminaire draws ~0.45 A, which makes detection *easier*, not harder — but an
  INA219 or a correctly-sized ACS712-5A would be the right part.
- **Single pole, single gateway.** The architecture supports many nodes; only
  one has been built.
- **No packet authentication.** LoRa payloads are unsigned plaintext, so a
  deployment would need message signing to resist spoofing.
- **Polling, not streaming.** Firebase supports server-sent events; REST
  polling was chosen for prototype simplicity and does not scale to a fleet.
- **No structural validation.** Tilt angles are geometrically correct but have
  not been correlated against actual pole failure modes.

---

## Roadmap

- [ ] Replace REST polling with Firebase SSE streaming
- [ ] Multi-pole fleet view with map and maintenance ranking
- [ ] Correctly-sized current sensor (INA219 / ACS712-5A)
- [ ] LDR for day/night context on the current reading
- [ ] Solar + battery for an energy-independent node
- [ ] Migrate from raw LoRa to LoRaWAN
- [ ] Signed payloads
- [ ] Trend-based forecasting — "this pole needs inspection within N days"

---

## Repository layout

```
polesentinel/
├── firmware/
│   ├── pole_node/          # sensing + classification + LoRa TX
│   └── gateway/            # LoRa RX + Wi-Fi + Firebase PUT
├── tools/
│   ├── i2c_scanner/        # verify the IMU is on the bus
│   └── acs_diagnostic/     # measure ADC statistics for calibration
├── dashboard/
│   └── index.html          # static console, deploys to Vercel
├── docs/
│   └── wiring.png
└── README.md
```

---

## License

MIT — see [`LICENSE`](LICENSE).

---

<div align="center">

Built at **Chennai Institute of Technology** · Department of Electronics and
Communication Engineering

</div>
