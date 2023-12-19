#include <Arduino.h>

#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <ADS1X15.h>

#ifndef STASSID
#define STASSID "dlink_frankyd"
#define STAPSK ""
#endif

ADS1115 ADS(0x48);

float slope = 11.02;
float intercept = 0.70;
#define GRID_VOLTAGE 230
#define END_OF_CYCLE 6000
#define WASMACHINE_ID "wasmachine 1"

unsigned long printPeriod = 1000; // in milliseconds
// Track time in milliseconds since last reading
unsigned long previousMillis = 0;
unsigned long lastSample = 0;
unsigned long EndOfCycle = 0;

const char *ssid = STASSID;
const char *password = STAPSK;

WiFiClient espClient;

void callback(char *topic, byte *payload, unsigned int length)
{
  // do something with the message

  String topicStr = topic;
  String recv_payload = String((char *)payload);

  Serial.println("mqtt_callback - message arrived - topic [" + topicStr +
                 "] payload [" + recv_payload + "]");
}

PubSubClient client("mqtt.tago.io", 1883, callback, espClient);

float ADC_value = 0;
float AmpsRMS = 0;
int state = 0;
double sum;
double samples;

int device_state;
int prev_device_state;

void connect_mqtt();
void setup_wifi();

void setup()
{
  Serial.begin(9600);
  Serial.println("Serial setup");
  ADS.begin();
  Serial.println("Setup ADC converter");
  ADS.setGain(0);     // 6.144 volt
  ADS.setDataRate(7); // 0 = slow   4 = medium   7 = fast
  ADS.setMode(0);     // continuous mode
  ADS.readADC(0);     // first read to trigger
  Serial.println("Setup WiFi and MQTT");
  setup_wifi();
  connect_mqtt();
}

void loop()
{
  while (true)
  {
    if (state == 0)
    {
      state = 1;
    }

    if (state == 1)
    {
      state = 2;
      previousMillis = millis();
      sum = 0;
      samples = 0;
    }
    if (state == 2)
    {
      uint32_t now = micros();
      if (now - lastSample >= 1160) //  almost exact 860 SPS
      {
        lastSample = now;
        ADC_value = ADS.getValue();
        ADC_value = ADC_value - 13200;

        sum = sum + (ADC_value * ADC_value);
        samples++;
      }
      if ((millis() - previousMillis) >= printPeriod)
      {                            // every printPeriod we do the calculation
        previousMillis = millis(); //   update time
        sum = sum / samples;
        Serial.println(samples);
        Serial.println(sum);
        AmpsRMS = (sqrt(sum) * (6.144 / 32768) * slope) - intercept;
        Serial.print("current: ");
        Serial.print(AmpsRMS);
        Serial.println(" amps RMS");
        sum = 0;
        samples = 0;
        state = 3;
      }
    }
    if (state == 3)
    {
      StaticJsonDocument<200> JSONbuffer;
      JsonArray array = JSONbuffer.to<JsonArray>();
      JsonObject amperage = array.createNestedObject();
      amperage["variable"] = "current";
      amperage["unit"] = "A";
      amperage["value"] = AmpsRMS;
      char JSONmessageBuffer[200];
      serializeJson(JSONbuffer, JSONmessageBuffer);

      if (client.publish("acs712", JSONmessageBuffer) == true)
      {
        Serial.println("Success sending message");
      }
      else
      {
        Serial.println("Error sending message");
      }
      Serial.print(device_state);
      if (AmpsRMS >= 0.5 && device_state == 0)
      {
        device_state = 1;
        StaticJsonDocument<200> JSONbuffer;
        JsonArray array = JSONbuffer.to<JsonArray>();
        JsonObject state = array.createNestedObject();
        state["variable"] = "state";
        state["value"] = "on";
        char JSONmessageBuffer[200];
        serializeJson(JSONbuffer, JSONmessageBuffer);
        if (client.publish("acs712/state", JSONmessageBuffer) == true)
        {
          Serial.println("The cycle has started");
        }
      }

      if (AmpsRMS < 0.5 && device_state == 1)
      {
        if ((millis() - EndOfCycle) >= END_OF_CYCLE)
        {
          device_state = 0;
          StaticJsonDocument<200> JSONbuffer;
          JsonArray array = JSONbuffer.to<JsonArray>();
          JsonObject state = array.createNestedObject();
          state["variable"] = "state";
          state["value"] = "off";
          char JSONmessageBuffer[200];
          serializeJson(JSONbuffer, JSONmessageBuffer);
          if (client.publish("acs712/state", JSONmessageBuffer) == true)
          {
            Serial.println("The cycle has ended");
          }
          EndOfCycle = millis();
        }
        else
        {
          EndOfCycle = millis();
        }
      }
      state = 0;
    }
  }
}

void connect_mqtt()
{
  //  while (serverIp.toString() == "0.0.0.0") {
  //    Serial.println("Resolving host mqtt.tago.io");
  //    delay(250);
  //    serverIp = MDNS.queryHost("mqtt.tago.io");
  //}
  Serial.print("connecting...");
  while (!client.connect("espcurrent", "Token", "3ced64a7-20ec-44af-88de-058729507baf", "acl712/state", 1, 1, "[{\"variable\":\"state\",\"value\":\"offline\"}]"))
    ;
  {
    Serial.print(".");
    delay(1000);
  }
}

void setup_wifi()
{
  // if (!WiFi.config(local_IP, gateway, subnet, DNS))
  //{
  //   Serial.println("STA Failed to configure");
  // }
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  delay(200);
  while (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  while (mdns_init() != ESP_OK)
  {
    delay(1000);
    Serial.println("Starting MDNS...");
  }

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}