#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <LoRa.h>

// ============================================================
//         POLESENTINEL GATEWAY (WIFI & FIREBASE)
// ============================================================

// --- LoRa Pins ---
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 26

// --- Wi-Fi Credentials ---
const char* ssid = "YOUR_WIFI_HOTSPOT_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// --- Firebase Configuration ---
// Paste your copied URL here, and ADD "/poles/SL-001.json" to the very end.
// Example: "https://polesentinel-1234-default-rtdb.firebaseio.com/poles/SL-001.json"
const char* firebaseURL = "YOUR_FIREBASE_URL_HERE/poles/SL-001.json";

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("\n======================================");
  Serial.println("  POLESENTINEL CLOUD GATEWAY READY    ");
  Serial.println("======================================");

  // 1. Connect to Wi-Fi
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // 2. Initialize LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("❌ LoRa Init Failed.");
    while (1);
  }
  Serial.println("✅ LoRa Ready! Listening for edge nodes...\n");
}

void loop() {
  // Check if a LoRa packet has arrived
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    
    String incomingPayload = "";
    while (LoRa.available()) {
      incomingPayload += (char)LoRa.read();
    }
    
    Serial.print("📡 Received: ");
    Serial.println(incomingPayload);

    // Only upload if it's a valid PoleSentinel packet and Wi-Fi is connected
    if (incomingPayload.startsWith("SL-") && WiFi.status() == WL_CONNECTED) {
      
      HTTPClient http;
      http.begin(firebaseURL);
      http.addHeader("Content-Type", "application/json");

      // Package the raw string into a simple JSON format
      // Looks like: {"payload": "SL-001,HEALTHY,16500..."}
      String jsonPayload = "{\"payload\": \"" + incomingPayload + "\"}";

      // Use HTTP PUT to overwrite the existing data for this pole
      int httpResponseCode = http.PUT(jsonPayload);

      if (httpResponseCode > 0) {
        Serial.print("☁️ Firebase Updated. Code: ");
        Serial.println(httpResponseCode);
      } else {
        Serial.print("❌ Firebase Error: ");
        Serial.println(httpResponseCode);
      }
      
      http.end();
    }
  }
}