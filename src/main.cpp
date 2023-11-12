#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

#ifndef STASSID
#define STASSID "Loki"
#define STAPSK "0499619079"

#define ADC_PIN 34
#endif

const char *ssid = STASSID;
const char *password = STAPSK;

WiFiClient espClient;
IPAddress local_IP(192, 168, 0, 20);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress DNS(8, 8, 8, 8);

void callback(char *topic, byte *payload, unsigned int length)
{
  // do something with the message

  String topicStr = topic;
  String recv_payload = String((char *)payload);

  Serial.println("mqtt_callback - message arrived - topic [" + topicStr +
                 "] payload [" + recv_payload + "]");
}

PubSubClient client("mqtt.tago.io", 1883, callback, espClient);

int mVperAmp = 77; // use 100 for 20A Module and 66 for 30A Module
int watt = 0;
double Voltage = 0;
double VRMS = 0;
double AmpsRMS = 0;
float getVPP();
void connect_mqtt();
void setup_wifi();

void setup()
{
  Serial.begin(9600);
  setup_wifi();
  connect_mqtt();
}

void loop()
{
  // if MQTT is not connected, reconnect
  if (!client.connected())
  {
    connect_mqtt();
  }
  client.loop();

  Voltage = getVPP();
  VRMS = (Voltage / 2.0) * 0.707;
  AmpsRMS = ((VRMS * 1000) / mVperAmp) - 1.2; // This is the correction factor for the sensor
  watt = (AmpsRMS * 240 / 1);
  // note : 1.2 is my own empirically established calibration factor
  // as the voltage measured at D34 depends on the length of the OUT-to-D34 wire

  // allocate the memory for the document
  StaticJsonDocument<200> JSONbuffer;
  JsonArray array = JSONbuffer.to<JsonArray>();
  JsonObject amperage = array.createNestedObject();
  amperage["variable"] = "current";
  amperage["unit"] = "A";
  amperage["value"] = AmpsRMS;
  JsonObject wattage = array.createNestedObject();
  wattage["variable"] = "wattage";
  wattage["unit"] = "W";
  wattage["value"] = watt;
  char JSONmessageBuffer[200];
  serializeJson(JSONbuffer, JSONmessageBuffer);

  if (client.publish("acl712", JSONmessageBuffer) == true)
  {
    Serial.println("Success sending message");
  }
  else
  {
    Serial.println("Error sending message");
  }

  Serial.println("-------------");

  Serial.print(AmpsRMS);
  Serial.print(" A RMS  ---  ");

  Serial.print(watt);
  Serial.println(" W");
}

float getVPP()
{
  float result;
  int readValue;       // value read from the sensor
  int maxValue = 0;    // store max value here
  int minValue = 4096; // store min value here

  readValue = analogRead(ADC_PIN);  //discard first reading
  uint32_t start_time = millis();
  while ((millis() - start_time) < 1000) // sample for 1 Sec. we get around 12000 samples per sec.
  {
    readValue = analogRead(ADC_PIN);
    // see if you have a new maxValue
    if (readValue > maxValue)
    {
      /*record the maximum sensor value*/
      maxValue = readValue;
    }
    if (readValue < minValue)
    {
      /*record the minimum sensor value*/
      minValue = readValue;
    }
  }

  // Subtract min from max
  result = ((maxValue - minValue) * 3.3) / 4096.0;
  Serial.print("measured values: ");
  Serial.println(maxValue);
  Serial.println(minValue);
  Serial.println(result);

  return result;
}

void connect_mqtt()
{
  //  while (serverIp.toString() == "0.0.0.0") {
  //    Serial.println("Resolving host mqtt.tago.io");
  //    delay(250);
  //    serverIp = MDNS.queryHost("mqtt.tago.io");
  //}
  Serial.print("connecting...");
  while (!client.connect("espheating", "Token", "84510911-1ec0-43fc-934c-45638df09402", "acl712/status", 1, 1, "offline"))
  {
    Serial.print(".");
    delay(1000);
  }
}

void setup_wifi()
{
  if (!WiFi.config(local_IP, gateway, subnet, DNS))
  {
    Serial.println("STA Failed to configure");
  }
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