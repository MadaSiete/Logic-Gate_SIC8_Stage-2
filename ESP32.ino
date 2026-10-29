#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// --- Definisi Pin GPIO ---
#define DHTPIN 15
#define DHTTYPE DHT22
#define POT_CURRENT_PIN 34
#define RELAY_PIN 26
#define SERVO_PIN 18
#define BUZZER_PIN 25
#define LED_GREEN 12
#define LED_YELLOW 14
#define LED_RED 27

// --- Kredensial WiFi Wokwi  ---
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// --- Public Broker HiveMQ ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "sic8/ev_bms/telemetry";

// --- Inisialisasi Module ---
DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;
LiquidCrystal_I2C lcd(0x27, 20, 4);
Servo ventServo;
WiFiClient espClient;
PubSubClient client(espClient);

float prevTemp = 25.0;
bool firstRead = true;
unsigned long lastMsg = 0;

// --- Setup Wifi ---
void setup_wifi() {
  Serial.print("Menghubungkan ke WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Terhubung! IP: " + WiFi.localIP().toString());
}

// --- Menghubungkan MQTT ---
void reconnect_mqtt() {
  while (!client.connected()) {
    Serial.print("Menghubungkan Smart BMS ke Public HiveMQ...");
    String clientId = "ESP32-SmartBMS-" + String(random(0xffff), HEX);
    
    // Koneksi langsung tanpa username & password
    if (client.connect(clientId.c_str())) {
      Serial.println("Terhubung ke Broker MQTT!");
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" coba lagi dalam 3 detik");
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  
  Wire.begin(21, 22);
  dht.begin();
  lcd.init();
  lcd.backlight();
  ventServo.attach(SERVO_PIN);

  if (!mpu.begin()) {
    Serial.println("Gagal menemukan chip MPU6050!");
  }
  
  lcd.setCursor(0, 0);
  lcd.print("IoT SMART EV BMS");
  lcd.setCursor(0, 1);
  lcd.print("Real-Time Monitor...");
  
  setup_wifi();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(512); 
  lcd.clear();
}

void loop() {
  if (!client.connected()) {
    reconnect_mqtt();
  }
  client.loop();

  unsigned long now = millis();
  if (now - lastMsg >= 1500) {
    lastMsg = now;

    // Baca Sensor DHT22 (Suhu & Kelembaban Battery Pack)
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (isnan(temp) || isnan(hum)) {
      Serial.println("Error membaca DHT22!");
      return;
    }

    // Hitung Rate-of-Rise Suhu (Delta T)
    if (firstRead) { prevTemp = temp; firstRead = false; }
    float deltaT = temp - prevTemp;
    prevTemp = temp;

    // Baca Simulasi Arus Beban dari Potentiometer 
    int adcVal = analogRead(POT_CURRENT_PIN);
    float currentAmp = (adcVal / 4095.0) * 100.0;

    // Baca Sensor MPU6050 
    sensors_event_t a, g, temp_mpu;
    mpu.getEvent(&a, &g, &temp_mpu);
    float gForce = sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z)) / 9.81;

    // Evaluasi Logika Proteksi Smart BMS & Aktuator
    String status = "NORMAL";
    bool contactorOn = true;
    int servoAngle = 0;

    if (temp >= 60.0 || currentAmp >= 85.0 || gForce >= 2.5) {
      status = "CRITICAL";
      contactorOn = false; 
      servoAngle = 90;     
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_RED, HIGH);
      tone(BUZZER_PIN, 2000, 1000);
    } 
    else if (temp >= 45.0 || deltaT >= 3.0 || currentAmp >= 60.0 || gForce >= 1.8) {
      status = "WARNING";
      contactorOn = true;
      servoAngle = 45;   
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_YELLOW, HIGH);
      digitalWrite(LED_RED, LOW);
      tone(BUZZER_PIN, 1000, 300);
    } 
    else {
      status = "NORMAL";
      contactorOn = true;
      servoAngle = 0;
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_RED, LOW);
      noTone(BUZZER_PIN);
    }

    digitalWrite(RELAY_PIN, contactorOn ? HIGH : LOW);
    ventServo.write(servoAngle);

    // Update Tampilan LCD 20x4
    lcd.setCursor(0, 0);
    lcd.printf("T:%.1fC dT:%+.1fC  ", temp, deltaT);
    lcd.setCursor(0, 1);
    lcd.printf("I:%.1fA  Vib:%.2fg ", currentAmp, gForce);
    lcd.setCursor(0, 2);
    lcd.printf("BMS : %-12s", status.c_str());
    lcd.setCursor(0, 3);
    lcd.printf("Relay:%s Vent:%ddeg ", contactorOn ? "ON " : "OFF", servoAngle);

    // Payload JSON untuk Dashboard
    StaticJsonDocument<256> doc;
    doc["temp"] = round(temp * 10) / 10.0;
    doc["delta_t"] = round(deltaT * 10) / 10.0;
    doc["hum"] = round(hum * 10) / 10.0;
    doc["current"] = round(currentAmp * 10) / 10.0;
    doc["vibration_g"] = round(gForce * 100) / 100.0;
    doc["status"] = status;
    doc["contactor_on"] = contactorOn;
    doc["vent_angle"] = servoAngle;

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);

    // Output ke Serial Monitor
    Serial.print("[BMS TELEMETRY] ");
    Serial.println(jsonBuffer);

    // Publish ke Public HiveMQ Broker
    client.publish(mqtt_topic, jsonBuffer);
  }
}
