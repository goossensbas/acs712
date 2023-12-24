#include <Arduino.h>

#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <ADS1X15.h>
#include <LiquidCrystal_I2C.h>

#ifndef STASSID
#define STASSID "dlink_frankyd"
#define STAPSK ""
// TagoIO device token
#define TAGO_TOKEN "3ced64a7-20ec-44af-88de-058729507baf"
#endif

ADS1115 ADS(0x48);
LiquidCrystal_I2C lcd(0x3F, 16, 2);
// slope = 1/accuracy in volt. For the 20A model the accuracy is 100mV/A
float slope = 11;
float intercept = 0.33;

#define END_OF_CYCLE 6000

unsigned long printPeriod = 1000; // in milliseconds
// Track time in milliseconds since last reading
unsigned long previousMillis = 0;
unsigned long lastSample = 0;
unsigned long EndOfCycle = 0;

const char *ssid = STASSID;
const char *password = STAPSK;

WiFiClient espClient;

// callback function for mqtt. Obligatory for PubSubclient.
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

float ADC_vdd = 0;

int device_state;
int prev_device_state;

void connect_mqtt();
void setup_wifi();

void setup()
{
  Serial.begin(115200);
  Serial.println("Serial setup");
  ADS.begin();
  Serial.println("Setup ADC converter");
  ADS.setGain(0);     // 6.144 volt
  ADS.setDataRate(7); // 0 = slow   4 = medium   7 = fast
  ADS.setMode(0);     // continuous mode
  ADS.readADC(0);     // first read to trigger ADC

  lcd.init();

  lcd.backlight(); // Turn on the backlight on LCD.
  lcd.print("Team1 meter");
  // Show welcome message. Meanwhile wait for vdd to stabilise
  delay(2000);
  lcd.clear();
  lcd.print("calibrating...");

  // Measure Vdd. take the average of 1000 samples.
  for (int i = 0; i < 1000; i++)
  {
    // wait for conversion to finish. (conversion takes 1160µs)
    delay(2);
    ADC_vdd = ADC_vdd + ADS.readADC(1);
  }
  ADC_vdd = ADC_vdd / 1000;
  lcd.print("Vdd = ");
  lcd.print(ADC_vdd);
  lcd.print(" V");
  delay(1500);
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
    // state 1: reset sum and sample to 0 and go to state 2
    if (state == 1)
    {
      state = 2;
      previousMillis = millis();
      sum = 0;
      samples = 0;
    }
    // state 2: every 1160µs, we ask the ADC converter for a value.
    // Then we substract the DC offset to get the AC value.
    // Add every measurement to sum.
    if (state == 2)
    {
      uint32_t now = micros();
      if (now - lastSample >= 1160) //  almost exact 860 SPS
      {
        lastSample = now;
        ADC_value = ADS.getValue();
        ADC_value = ADC_value - (ADC_vdd / 2);

        sum = sum + (ADC_value * ADC_value);
        samples++;
      }
      if ((millis() - previousMillis) >= printPeriod)
      {                            // every printPeriod we do the calculation
        previousMillis = millis(); //   update time
        sum = sum / samples;       // get the average current measured
        Serial.println(samples);
        Serial.println(sum);
        AmpsRMS = (sqrt(sum) * (6.144 / 32768) * slope) - intercept; // calculate the RMS value
        Serial.print("current: ");
        Serial.print(AmpsRMS);
        Serial.println(" amps RMS");
        lcd.setCursor(0, 0);
        lcd.print("current = ");
        lcd.print(AmpsRMS);
        lcd.print(" A");
        sum = 0;
        samples = 0;
        state = 3;
      }
    }

    // state 3: send the values to MQTT broker
    if (state == 3)
    {
      // make a json object
      StaticJsonDocument<200> JSONbuffer;
      JsonArray array = JSONbuffer.to<JsonArray>();
      JsonObject amperage = array.createNestedObject();
      amperage["variable"] = "current";
      amperage["unit"] = "A";
      amperage["value"] = AmpsRMS;
      char JSONmessageBuffer[200];
      serializeJson(JSONbuffer, JSONmessageBuffer);
      // publish the serialised buffer to the broker
      if (client.publish("acs712", JSONmessageBuffer) == true)
      {
        Serial.println("Success sending message");
      }
      else
      {
        Serial.println("Error sending message");
      }
      Serial.print(device_state);
      // IF the device is OFF and the current is more than 0,5A
      // THEN update the device state to ON, and publish the state on the broker
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
      // IF the device is ON and the current is less than 0,5A for END_OF_CYCLE ms
      // THEN update the device state to OFF, and publish the state on the broker
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
  // connect to tagoIO broker with token auth and set the LWT
  Serial.print("connecting...");
  while (!client.connect("espcurrent", "Token", TAGO_TOKEN, "acl712/state", 1, 1, "[{\"variable\":\"state\",\"value\":\"offline\"}]"))
    ;
  {
    Serial.print(".");
    delay(1000);
  }
}

void setup_wifi()
{
  // connect to wifi with ssid and password
  // start the DNS client to look up the IP address of the broker
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