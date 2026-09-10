// =====================================================================
// HYDROSENSE AI - SENDER (edge-AI gated LoRa transmission)
//
// Implements, in one firmware:
//   1. Adaptive baseline (slow EMA update, FROZEN during active anomaly)
//   2. AI-gated transmission (only TX on anomaly, probe fault, or heartbeat)
//   3. Duty-cycle safety floor (prevents repeated-anomaly TX spam)
//   4. Optional dual-probe cross-validation / fault flagging
//   5. Full-rate local logging over Serial (capture with serial_logger.py
//      on a laptop during bench/field tests -> becomes your
//      "actual measurement" evidence file for submission)
// =====================================================================

#include <LoRa.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- Pins ----------------
#define ss   5
#define rst  14
#define dio0 2

#define PRIMARY_ONEWIRE_PIN   4
#define SECONDARY_ONEWIRE_PIN 15   // set ENABLE_DUAL_PROBE 0 if not wired

// ---------------- Feature toggles ----------------
#define ENABLE_DUAL_PROBE 1        // 1 = use secondary probe for cross-check
#define PROBE_FAULT_THRESHOLD_C 1.0f  // if probes diverge more than this -> fault

// Set to 1 for: (a) range testing, where you want a packet every cycle
// regardless of AI output so you get continuous RSSI/distance data, or
// (b) as the fixed-interval "Node A" baseline for your A/B energy test.
// Set to 0 for normal AI-gated operation (the real proposal behavior).
#define ALWAYS_TRANSMIT_MODE 0

// ---------------- Timing (DEMO values - tighten for real deployment) ----------------
const unsigned long SAMPLE_INTERVAL_MS   = 5000UL;     // how often we sample+log
const unsigned long HEARTBEAT_INTERVAL_MS = 600000UL;  // 10 min demo (use 86400000UL for daily in production)
const unsigned long MIN_TX_INTERVAL_MS    = 15000UL;   // floor: never TX more often than this, even during sustained anomaly

// ---------------- Baseline ----------------
const int   BASELINE_STARTUP_SAMPLES = 15;
const float BASELINE_EMA_ALPHA       = 0.02f;   // slow adaptive follow rate for NORMAL readings

// ---------------- Globals ----------------
OneWire oneWirePrimary(PRIMARY_ONEWIRE_PIN);
DallasTemperature sensorsPrimary(&oneWirePrimary);

#if ENABLE_DUAL_PROBE
OneWire oneWireSecondary(SECONDARY_ONEWIRE_PIN);
DallasTemperature sensorsSecondary(&oneWireSecondary);
#endif

float baselineTemperature   = 0.0f;
float previousTemperature   = 0.0f;
bool  baselineEstablished    = false;
int   startupSampleCount     = 0;
float startupSampleSum       = 0.0f;

unsigned long lastTxTimeMs      = 0;
unsigned long loopCounter       = 0;   // every sample cycle, for duty-cycle stats
unsigned long txCounter         = 0;   // every actual transmission
unsigned long txPacketNumber    = 0;   // sequence number, increments only on TX (lets receiver measure packet loss)

// ---------------- Decision tree (unchanged - trained offline) ----------------
bool predictAnomaly(float delta1, float baselineDeviation)
{
  if (baselineDeviation <= 0.243303f)
  {
    if (baselineDeviation <= -0.275202f)
    {
      return true; // ANOMALY
    }
    else
    {
      if (delta1 <= -0.509880f)
      {
        return true; // ANOMALY
      }
      else
      {
        return false; // NORMAL
      }
    }
  }
  else
  {
    return true; // ANOMALY
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial);

  sensorsPrimary.begin();
#if ENABLE_DUAL_PROBE
  sensorsSecondary.begin();
#endif

  Serial.println("LoRa Sender - HydroSense AI (adaptive baseline + AI-gated TX)");

  LoRa.setPins(ss, rst, dio0);
  while (!LoRa.begin(433E6))
  {
    Serial.println(".");
    delay(500);
  }
  LoRa.setSyncWord(0xA5);
  Serial.println("LoRa Initializing OK!");

  // CSV header for whatever's capturing this over serial_logger.py
  Serial.println("csv_header,loop,millis,temp1,temp2,probe_fault,delta1,baseline,baseline_dev,ai_result,transmitted,reason,tx_pckt_num");
}

float readPrimaryTemp()
{
  sensorsPrimary.requestTemperatures();
  return sensorsPrimary.getTempCByIndex(0);
}

// DS18B20 returns exactly -127.0 when the probe can't be read
// (loose wire, bad contact, conversion not finished). Treat that
// as invalid so a single bad sample can't poison the baseline average.
bool isValidTemp(float t)
{
  return t > -55.0f && t < 125.0f; // DS18B20's real operating range
}

#if ENABLE_DUAL_PROBE
float readSecondaryTemp()
{
  sensorsSecondary.requestTemperatures();
  return sensorsSecondary.getTempCByIndex(0);
}
#endif

void sendPacket(float temp, float delta1, float baselineDev, bool anomaly, bool probeFault, const char* reason)
{
  txPacketNumber++;

  LoRa.beginPacket();
  LoRa.print("Pckt:");   LoRa.print(txPacketNumber);
  LoRa.print(",Temp:");  LoRa.print(temp, 2);
  LoRa.print(",Delta:"); LoRa.print(delta1, 2);
  LoRa.print(",BaseDev:"); LoRa.print(baselineDev, 2);
  LoRa.print(",AI:");    LoRa.print(anomaly ? "ANOMALY" : "NORMAL");
  LoRa.print(",Fault:"); LoRa.print(probeFault ? 1 : 0);
  LoRa.print(",Reason:"); LoRa.print(reason);
  LoRa.endPacket();

  txCounter++;
  lastTxTimeMs = millis();
}

void loop()
{
  loopCounter++;

  float temp1 = readPrimaryTemp();
  float temp = temp1;
  bool probeFault = false;

#if ENABLE_DUAL_PROBE
  float temp2 = readSecondaryTemp();
  if (!isnan(temp2))
  {
    if (fabs(temp1 - temp2) > PROBE_FAULT_THRESHOLD_C)
    {
      probeFault = true; // probes disagree - flag instead of silently trusting temp1
    }
    else
    {
      temp = (temp1 + temp2) / 2.0f; // agree closely enough - average for a slightly cleaner reading
    }
  }
#else
  float temp2 = NAN;
#endif

  // ---------------- Startup baseline (unchanged mechanism) ----------------
  if (!baselineEstablished)
  {
    if (!isValidTemp(temp))
    {
      Serial.println("Invalid reading during baseline calibration - skipped, not counted");
      delay(SAMPLE_INTERVAL_MS);
      return; // don't let a bad sample pull the average off
    }

    startupSampleSum += temp;
    startupSampleCount++;
    previousTemperature = temp;

    if (startupSampleCount >= BASELINE_STARTUP_SAMPLES)
    {
      baselineTemperature = startupSampleSum / BASELINE_STARTUP_SAMPLES;
      baselineEstablished = true;
      Serial.print("Baseline established: ");
      Serial.println(baselineTemperature, 3);
    }
    delay(SAMPLE_INTERVAL_MS);
    return; // don't run AI logic until baseline exists
  }

  // ---------------- Features ----------------
  float delta1 = temp - previousTemperature;
  float baselineDeviation = temp - baselineTemperature;

  bool anomaly = predictAnomaly(delta1, baselineDeviation);

  // ---------------- Adaptive baseline update ----------------
  // Only drift the baseline on NORMAL, non-fault readings.
  // Freezing during ANOMALY prevents a sustained event from being
  // absorbed into "the new normal" and silently stopping detection.
  if (!anomaly && !probeFault)
  {
    baselineTemperature += BASELINE_EMA_ALPHA * (temp - baselineTemperature);
  }

  // ---------------- AI-gated transmission decision ----------------
  unsigned long now = millis();
  bool heartbeatDue = (now - lastTxTimeMs) >= HEARTBEAT_INTERVAL_MS;
  bool minIntervalOk = (now - lastTxTimeMs) >= MIN_TX_INTERVAL_MS;

  bool shouldTransmit = false;
  const char* reason = "NONE";

#if ALWAYS_TRANSMIT_MODE
  shouldTransmit = true;
  reason = "ALWAYS_ON";
#else
  if ((anomaly || probeFault) && minIntervalOk)
  {
    shouldTransmit = true;
    reason = probeFault ? "FAULT" : "ANOMALY";
  }
  else if (heartbeatDue)
  {
    shouldTransmit = true;
    reason = "HEARTBEAT";
  }
#endif

  if (shouldTransmit)
  {
    sendPacket(temp, delta1, baselineDeviation, anomaly, probeFault, reason);
  }

  // ---------------- Full-rate local logging (every cycle, TX or not) ----------------
  Serial.print("csv_data,");
  Serial.print(loopCounter); Serial.print(",");
  Serial.print(now); Serial.print(",");
  Serial.print(temp1, 3); Serial.print(",");
  Serial.print(isnan(temp2) ? -999.0 : temp2, 3); Serial.print(",");
  Serial.print(probeFault ? 1 : 0); Serial.print(",");
  Serial.print(delta1, 3); Serial.print(",");
  Serial.print(baselineTemperature, 3); Serial.print(",");
  Serial.print(baselineDeviation, 3); Serial.print(",");
  Serial.print(anomaly ? "ANOMALY" : "NORMAL"); Serial.print(",");
  Serial.print(shouldTransmit ? 1 : 0); Serial.print(",");
  Serial.print(reason); Serial.print(",");
  Serial.println(shouldTransmit ? txPacketNumber : 0);

  // ---------------- Periodic duty-cycle stats (piggybacks on heartbeat) ----------------
  if (shouldTransmit && strcmp(reason, "HEARTBEAT") == 0)
  {
    float reductionPct = 100.0f * (1.0f - ((float)txCounter / (float)loopCounter));
    Serial.print("duty_cycle_stats,loops:"); Serial.print(loopCounter);
    Serial.print(",tx_count:"); Serial.print(txCounter);
    Serial.print(",reduction_vs_always_on_pct:"); Serial.println(reductionPct, 1);
  }

  previousTemperature = temp;
  delay(SAMPLE_INTERVAL_MS);
}
