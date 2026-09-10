// =====================================================================
// HYDROSENSE AI - RECEIVER
//
// Parses the new packet format (Pckt, Temp, Delta, BaseDev, AI, Fault,
// Reason), shows it on the SH1106 OLED, and logs a CSV line per packet
// over Serial (capture with serial_logger.py) including RSSI and a
// packet-loss counter computed from gaps in the sequence number -
// this is your "actual measurement" evidence for the range test and
// packet-success-rate KPI.
// =====================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <LoRa.h>

#define ss 5
#define rst 14
#define dio0 2

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

long lastPacketNumber = -1;
unsigned long packetsReceived = 0;
unsigned long packetsLost = 0;

// Extracts the value after "key:" up to the next comma (or end of string)
String getField(const String &packet, const String &key)
{
  int pos = packet.indexOf(key);
  if (pos < 0) return "";
  pos += key.length();
  int end = packet.indexOf(',', pos);
  if (end < 0) end = packet.length();
  return packet.substring(pos, end);
}

void setup()
{
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(OLED_ADDR, true);

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("HYDROSENSE AI");
  display.println("----------------");
  display.println("LoRa: STARTING");
  display.display();

  LoRa.setPins(ss, rst, dio0);
  while (!LoRa.begin(433E6))
  {
    Serial.println("LoRa failed");
    delay(500);
  }
  LoRa.setSyncWord(0xA5);
  Serial.println("LoRa Receiver Ready");

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("HYDROSENSE AI");
  display.println("----------------");
  display.println("LoRa: READY");
  display.println("Waiting...");
  display.display();

  // CSV header for whatever's capturing this over serial_logger.py
  Serial.println("csv_header,millis,pckt,temp,delta,basedev,ai,fault,reason,rssi,packets_received,packets_lost,loss_pct_so_far");
}

void loop()
{
  int packetSize = LoRa.parsePacket();

  if (packetSize)
  {
    String packet = "";
    while (LoRa.available())
    {
      packet += (char)LoRa.read();
    }

    int rssi = LoRa.packetRssi();

    long pckt = getField(packet, "Pckt:").toInt();
    float temp = getField(packet, "Temp:").toFloat();
    float delta = getField(packet, "Delta:").toFloat();
    float baseDev = getField(packet, "BaseDev:").toFloat();
    String ai = getField(packet, "AI:");
    int fault = getField(packet, "Fault:").toInt();
    String reason = getField(packet, "Reason:");

    // ---- packet loss tracking (only meaningful once at least one packet seen) ----
    packetsReceived++;
    if (lastPacketNumber >= 0 && pckt > lastPacketNumber + 1)
    {
      packetsLost += (pckt - lastPacketNumber - 1);
    }
    lastPacketNumber = pckt;

    unsigned long totalExpected = packetsReceived + packetsLost;
    float lossPct = totalExpected > 0 ? (100.0f * packetsLost / totalExpected) : 0.0f;

    // ---- OLED ----
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("HYDROSENSE AI");
    display.println("----------------");
    display.print("PCKT: "); display.println(pckt);
    display.print("TEMP: "); display.print(temp, 2); display.println(" C");
    display.print("AI: "); display.print(ai);
    if (fault) display.println(" FLT"); else display.println("");
    display.print("RSSI: "); display.print(rssi); display.println(" dBm");
    display.print("LOSS: "); display.print(lossPct, 1); display.println(" %");
    display.display();

    // ---- CSV log line (this is your measurement evidence) ----
    Serial.print("csv_data,");
    Serial.print(millis()); Serial.print(",");
    Serial.print(pckt); Serial.print(",");
    Serial.print(temp, 2); Serial.print(",");
    Serial.print(delta, 2); Serial.print(",");
    Serial.print(baseDev, 2); Serial.print(",");
    Serial.print(ai); Serial.print(",");
    Serial.print(fault); Serial.print(",");
    Serial.print(reason); Serial.print(",");
    Serial.print(rssi); Serial.print(",");
    Serial.print(packetsReceived); Serial.print(",");
    Serial.print(packetsLost); Serial.print(",");
    Serial.println(lossPct, 2);
  }
}
