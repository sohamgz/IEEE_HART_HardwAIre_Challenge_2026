"""
AI-Enabled Watershed Guardian (AI-WG) - Simulation & Analysis File
Team HydroSense AI - IEEE HART HardwAIre Challenge 2026

Submitted as the "Ansys source file or equivalent" simulation deliverable.
Ansys HFSS/OptiSLang were not used (full antenna EM simulation was outside
this build cycle's scope). Instead, this file provides:

  1. A LoRa link-budget model (equivalent purpose to HFSS antenna/range
     analysis) projecting coverage range under several terrain scenarios,
     calibrated to our actual firmware's radio settings.
  2. A parametric sensitivity sweep across path-loss exponent (equivalent
     purpose to OptiSLang's sensitivity analysis).
  3. A datasheet-based energy consumption model for the sensor node,
     weighted by our measured transmit duty cycle.
  4. Direct analysis of real field-logged data (test_log.csv, captured via
     serial_logger.py during bench testing) to ground the above models in
     actual hardware behavior rather than pure theory.

Run: python3 simulation.py
"""

import math
import csv
import re
from pathlib import Path

# =====================================================================
# 1. LINK BUDGET / COVERAGE RANGE MODEL
# =====================================================================
# Calibrated to our actual sender.ino: no LoRa.setTxPower() call (library
# default 17 dBm), no setSpreadingFactor()/setSignalBandwidth() call
# (library defaults: SF7, BW125kHz), sync word 0xA5, 433 MHz.

TX_POWER_DBM = 17
TX_ANTENNA_GAIN_DBI = 0     # basic wire monopole, no gain
RX_ANTENNA_GAIN_DBI = 0
RX_SENSITIVITY_DBM = -123   # SX1276/SX1278 typical sensitivity, SF7/BW125kHz
FADE_MARGIN_DB = 10         # safety margin for multipath/interference
FREQ_MHZ = 433

def max_allowed_path_loss():
    return (TX_POWER_DBM + TX_ANTENNA_GAIN_DBI + RX_ANTENNA_GAIN_DBI
            - RX_SENSITIVITY_DBM - FADE_MARGIN_DB)

def path_loss_at_1m(freq_mhz=FREQ_MHZ):
    # Free-space path loss at 1m reference distance
    return 20 * math.log10(1) + 20 * math.log10(freq_mhz) - 27.55

def range_for_path_loss_exponent(n, freq_mhz=FREQ_MHZ):
    """Log-distance path loss model: PL(d) = PL(d0) + 10*n*log10(d/d0)"""
    mapl = max_allowed_path_loss()
    pl_d0 = path_loss_at_1m(freq_mhz)
    log_d = (mapl - pl_d0) / (10 * n)
    return 10 ** log_d  # meters

TERRAIN_SCENARIOS = {
    "Open field, line-of-sight (n=2.2)": 2.2,
    "Suburban / light obstruction (n=3.0)": 3.0,
    "Forest / vegetated riverbank (n=4.0)  <- our deployment target": 4.0,
    "Dense forest / heavy multi-wall indoor (n=5.0)": 5.0,
}

def print_link_budget():
    print("=" * 78)
    print("1. LINK BUDGET / COVERAGE RANGE (sensitivity analysis over terrain)")
    print("=" * 78)
    print(f"TX power: {TX_POWER_DBM} dBm | RX sensitivity: {RX_SENSITIVITY_DBM} dBm "
          f"| Fade margin: {FADE_MARGIN_DB} dB")
    print(f"Max allowed path loss: {max_allowed_path_loss():.1f} dB\n")
    print(f"{'Scenario':55s} {'Range (m)':>10s} {'Range (cm)':>14s}")
    for name, n in TERRAIN_SCENARIOS.items():
        d = range_for_path_loss_exponent(n)
        print(f"{name:55s} {d:10.1f} {d*100:14.0f}")
    print()
    print("Reported KPI value uses the forest/vegetated-riverbank scenario,")
    print("matching our actual proposed deployment environment.\n")


# =====================================================================
# 2. ENERGY CONSUMPTION MODEL (deep-sleep architecture)
# =====================================================================
# Datasheet-based (no in-circuit current measurement instrument available
# this build cycle). Firmware now puts the ESP32 into deep sleep between
# samples (esp_deep_sleep_start(), timer wakeup) and the LoRa radio into
# its sleep mode (LoRa.sleep()) before sleeping, rather than staying
# continuously active as in the earlier revision.

V_BATTERY = 3.7             # nominal Li-ion voltage
I_ACTIVE_MA = 130            # ESP32 + LoRa init/sample/compute, awake portion
I_DEEP_SLEEP_MA = 0.1        # ESP32 deep sleep w/ RTC timer wake (typical datasheet figure)
I_LORA_SLEEP_MA = 0.0002     # SX1276/SX1278 sleep mode, effectively negligible
ACTIVE_DURATION_S = 1.5      # sensor conversion + LoRa init + occasional TX, approx

def avg_power_mw(sample_interval_s):
    active_frac = ACTIVE_DURATION_S / sample_interval_s
    sleep_frac = 1 - active_frac
    i_avg = active_frac * I_ACTIVE_MA + sleep_frac * (I_DEEP_SLEEP_MA + I_LORA_SLEEP_MA)
    return i_avg * V_BATTERY, i_avg

ENERGY_SCENARIOS = {
    "Pre-deep-sleep firmware (ESP32 continuously active, old revision)": None,  # handled separately below
    "Demo/test setting (5s sample interval, deep sleep)": 5,
    "Production setting per proposal (2 min interval, deep sleep)": 120,
    "Production setting per proposal (5 min interval, deep sleep)": 300,
}

def print_energy_model():
    print("=" * 78)
    print("2. ENERGY CONSUMPTION MODEL (deep-sleep architecture)")
    print("=" * 78)

    # legacy continuously-active figure, for comparison only
    legacy_i = I_ACTIVE_MA + 1.6  # old model: ESP32 active + LoRa standby baseline
    legacy_p = legacy_i * V_BATTERY
    print(f"{'Scenario':62s} {'mA avg':>8s} {'mW avg':>8s} {'mWh/hr':>8s}")
    print(f"{'Pre-deep-sleep firmware (continuously active)':62s} {legacy_i:8.2f} {legacy_p:8.2f} {legacy_p:8.2f}")

    for name, interval in ENERGY_SCENARIOS.items():
        if interval is None:
            continue
        p, i = avg_power_mw(interval)
        reduction = 100 * (1 - p / legacy_p)
        print(f"{name:62s} {i:8.4f} {p:8.3f} {p:8.3f}   (-{reduction:.1f}% vs pre-sleep)")
    print()
    print("Reported KPI value uses the 2-minute production interval scenario,")
    print("the conservative end of the proposal's stated 2-5 minute sampling range.")
    print("NOTE: still a datasheet-based calculation, not an instrument-measured")
    print("figure - current draw has not been verified with a multimeter this cycle.\n")


# =====================================================================
# 3. REAL FIELD DATA ANALYSIS (from serial_logger.py capture)
# =====================================================================

def analyze_field_log(csv_path):
    print("=" * 78)
    print(f"3. REAL FIELD DATA ANALYSIS ({csv_path.name})")
    print("=" * 78)

    if not csv_path.exists():
        print(f"[!] {csv_path} not found - place your serial_logger.py CSV")
        print("    output next to this script and re-run.\n")
        return

    rssi_values = []
    anomaly_count = 0
    normal_count = 0
    packets_lost_final = 0
    packets_received_final = 0

    pattern = re.compile(
        r"csv_data,(\d+),(\d+),([-\d.]+),([-\d.]+),([-\d.]+),(\w+),(\d+),(\w+),(-?\d+),(\d+),(\d+),([\d.]+)"
    )

    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            raw = row.get("raw_line", "")
            m = pattern.search(raw)
            if not m:
                continue
            (_millis, _pckt, _temp, _delta, _basedev, ai, _fault, _reason,
             rssi, received, lost, _losspct) = m.groups()
            rssi_values.append(int(rssi))
            if ai == "ANOMALY":
                anomaly_count += 1
            else:
                normal_count += 1
            packets_received_final = int(received)
            packets_lost_final = int(lost)

    if not rssi_values:
        print("No csv_data rows found/parsed in this log.\n")
        return

    print(f"Parsed {len(rssi_values)} received packets")
    print(f"  RSSI: min={min(rssi_values)} dBm, max={max(rssi_values)} dBm, "
          f"avg={sum(rssi_values)/len(rssi_values):.1f} dBm")
    print(f"  Classifications: {anomaly_count} ANOMALY, {normal_count} NORMAL")
    print(f"  Packets received: {packets_received_final}, lost: {packets_lost_final}")

    if min(rssi_values) < -95:
        print("\n  [!] RSSI drops below -95 dBm observed in this log even at short")
        print("      range. This is weaker than expected for close-proximity LoRa")
        print("      and is noted as a known hardware/antenna investigation item -")
        print("      the link-budget projections above use the module's datasheet")
        print("      sensitivity, not this degraded field figure, since the cause")
        print("      (likely antenna connection/tuning) had not been isolated within")
        print("      this build cycle.")
    print()


# =====================================================================
# MAIN
# =====================================================================

if __name__ == "__main__":
    print_link_budget()
    print_energy_model()
    analyze_field_log(Path(__file__).parent / "test_log.csv")
    print("=" * 78)
    print("End of simulation output.")
    print("=" * 78)
