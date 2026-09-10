# AI-Enabled Watershed Guardian (AI-WG)

**An edge-AI LoRa sensor network for real-time river temperature monitoring — fully offline, solar-powered, and built for IEEE HART's HardwAIre Challenge 2026.**

Team HydroSense AI

---

## The Problem

Rivers and reservoirs downstream of industrial zones are vulnerable to sudden chemical discharge, thermal effluent releases, or unpermitted industrial outflow — events that often go undetected until visible damage has already occurred. Continuous water-quality monitoring along remote riverbanks is expensive and, critically, most remote sites simply have no WiFi or cellular coverage at all.

AI-WG monitors water temperature continuously, runs anomaly detection **entirely on-device**, and only radios a result when something is actually wrong — no cloud, no internet, no infrastructure beyond the two nodes themselves.

## How It Works

```
                 HYDROSENSE AI PIPELINE

       ┌─────────────────────────────┐
       │       WATER / SENSOR        │
       │        DS18B20               │
       │     Temperature Sensor      │
       └──────────────┬──────────────┘
                       │ Temperature
                       ▼
       ┌─────────────────────────────┐
       │       SENDER (ESP32)        │
       │  1. Read temperature        │
       │  2. Calculate delta_1       │
       │  3. Update adaptive baseline│
       │  4. Calculate deviation     │
       │  5. Run decision tree       │
       │  6. NORMAL / ANOMALY        │
       │  7. Deep sleep until next   │
       │     sample cycle            │
       └──────────────┬──────────────┘
                       │ LoRa 433 MHz
                       │ (only on anomaly,
                       │  fault, or heartbeat)
                       ▼
       ┌─────────────────────────────┐
       │      RECEIVER (ESP32)       │
       │  Decode packet, show on     │
       │  OLED, track RSSI & loss    │
       └─────────────────────────────┘
```

- **Edge AI, not cloud AI.** A shallow decision tree (max depth 4), trained offline in scikit-learn, is converted to plain `if/else` C logic and compiled directly into the sender's firmware. No ML runtime, no network round-trip.
- **Adaptive baseline.** The sender establishes a startup baseline from its first 15 readings, then slowly drifts it toward normal environmental change via an EMA update — but **freezes the baseline the moment an anomaly is detected**, so a sustained pollution event can't be absorbed as "the new normal."
- **AI-gated transmission.** The sender only transmits when the classifier flags `ANOMALY`, a dual-probe fault is detected, or a periodic heartbeat is due — not on a fixed schedule. This is the core mechanism tying AI directly to the system's energy budget.
- **Deep sleep.** Between samples the ESP32 fully powers down (`esp_deep_sleep_start()`) rather than idling, waking only to sample, decide, and optionally transmit. State that must survive across sleep (baseline, counters) lives in RTC memory, since deep sleep is a full reboot.
- **Fully offline.** Zero WiFi, zero cellular, zero cloud dependency anywhere in the pipeline — by design, for deployment in exactly the kind of remote, unconnected terrain this is built for.

## Repository Contents

| File | Description |
|---|---|
| `sender.ino` | Sensor node firmware — DS18B20 probe(s), adaptive baseline, on-device decision tree, AI-gated LoRa transmission, deep sleep |
| `receiver.ino` | Reader node firmware — decodes packets, displays on SH1106 OLED, tracks RSSI and packet-loss rate |
| `serial_logger.py` | PC-side tool to capture either board's Serial output to a timestamped CSV for bench/field testing |
| `simulation.py` | Link-budget coverage-range model + energy consumption model + real field-log analysis (submitted as the "Ansys or equivalent" simulation deliverable) |
| `AI-WG_BOM.xlsx` | Bill of materials with live formulas, sourced from actual component invoice |
| `AI-WG_Project_Description.docx` | 2-page project description (scenario, solution, AI impact, KPI status, honest gap analysis vs. original proposal) |
| `test_log.csv` | Real field-captured sensor/RSSI/packet-loss log from bench testing |

## Hardware

- ESP32 dev board (×2 — sender and receiver)
- DS18B20 waterproof temperature probe (dual-probe cross-validation supported, optional)
- RA-02 / SX1278 LoRa module, 433 MHz (×2)
- SH1106 128×64 OLED display (receiver only)
- 3× 18650 Li-ion cells + holder, solar panel, buck/boost converter (sender)
- Waterproof enclosure

Full itemized costs are in `AI-WG_BOM.xlsx`.

## Packet Format

```
Pckt:<seq>,Temp:<°C>,Delta:<°C>,BaseDev:<°C>,AI:<NORMAL|ANOMALY>,Fault:<0|1>,Reason:<ANOMALY|FAULT|HEARTBEAT|ALWAYS_ON>
```

## Getting Started

1. Wire the DS18B20 probe (and optional second probe) to the sender's `PRIMARY_ONEWIRE_PIN` / `SECONDARY_ONEWIRE_PIN`, and the LoRa module per the `ss`/`rst`/`dio0` pin definitions at the top of each `.ino` file.
2. Flash `receiver.ino` to one ESP32, `sender.ino` to the other. Both must share the same `LORA_FREQUENCY` (433 MHz) and sync word (`0xA5`).
3. Power the receiver, then the sender — the sender takes its first 15 readings (~75s at the demo 5s sample interval) to establish a baseline before it will transmit anything.
4. To capture logs for testing/analysis, run:
```
   pip install pyserial
   python serial_logger.py <PORT> 115200 output.csv
```
5. To reproduce the coverage-range and energy figures, run:
```
   python simulation.py
```

### Config flags worth knowing about (top of `sender.ino`)

| Flag | Purpose |
|---|---|
| `ENABLE_DUAL_PROBE` | Set `0` if only one DS18B20 is wired |
| `ALWAYS_TRANSMIT_MODE` | Set `1` for range testing or as the fixed-interval baseline in an A/B energy comparison; `0` for normal AI-gated operation |
| `SAMPLE_INTERVAL_MS` | `5000` for visible demo/testing behavior; `120000`–`300000` (2–5 min) for real deployment per the original design target |

## Current KPI Status

| KPI | Value | Basis |
|---|---|---|
| Cost | $57 (₹5,460) | Actual BOM, sender + receiver, incl. GST, excl. shared tools |
| Energy Consumption | ~6 mWh/hr | Datasheet-based calc, deep sleep + AI-gated TX, 2-min production interval |
| Coverage Range | 41,736 cm (~417 m) | Link-budget simulation, forest/vegetated-terrain path-loss model |
| Size | 765 cm3 | — |

## Known Limitations / Honest Status

This is a hackathon-timeline prototype, not a finished product. Being upfront about what's not yet done:

- Primary probe is DS18B20-class (±0.5°C), not the TSYS01-class (±0.1°C) precision probe originally proposed.
- Dual-probe fault-detection logic exists in firmware; physical dual-probe installation on the deployed unit is a final check remaining.
- Reader is OLED-only — no Flask dashboard yet.
- No on-device SD/flash logging yet; full-rate logs are currently captured via PC-side serial capture during bench testing.
- No battery voltage/state-of-charge monitoring in firmware yet.
- Regional ISM-band duty-cycle compliance has not been formally verified against regulation (a heuristic minimum transmit-interval floor stands in for now).
- The energy A/B comparison used one node run sequentially in two firmware modes, not two physical nodes side-by-side.
- Field-measured range has not reached the ≥500m target; the reported figure is a link-budget projection, and a real-world RSSI anomaly seen during short-range bench testing (weaker signal than expected even at close range) has not yet been root-caused.
- Classifier recall on held-out test data is 88.5%, trained on a hybrid dataset (modeled normal behavior + synthetic anomalies) rather than real multi-day river logging.
- Long-term field validation and additional water-quality parameters beyond temperature are planned for the next development phase.

