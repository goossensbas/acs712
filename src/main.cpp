#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <ADS1X15.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

#define TAGO
#define LOCAL
#define BROKER_URL "mqtt.tago.io"
#define LOCAL_BROKER_URL "basandsanja.duckdns.org"
#define LOCAL_BROKER_PORT 1884
#define WASMACHINE_ID 2
#define SENSOR_ID 2
#define SESSION_ID 2000
#define END_OF_CYCLE 20000

// session id from 0000, 1000 and 2000 for each device.
// The session ID will only be rewritten when the file is erased.

// uncoomment or make a secret.h file with the following content:
// #ifndef STASSID
// #define STASSID "mySSID"
// #define STAPSK "myPass"
//  TagoIO device token
// #define TAGO_TOKEN "TagoToken"
// #endif

ADS1115 ADS(0x48);
LiquidCrystal_I2C lcd(0x27, 16, 2);
// change to 0x3F for meter 1!!
// slope = 1/accuracy in volt. For the 20A model the accuracy is 100mV/A
float slope = 10;
float intercept = 0.12;



unsigned long startTime = 0; // Variable to store the start time
unsigned long elapsedTime;
unsigned long printPeriod = 1000; // in milliseconds

// Track time in milliseconds since last reading
unsigned long previousMillis = 0;
unsigned long lastSample = 0;
unsigned long EndOfCycle = 0;

const char *ssid = STASSID;
const char *password = STAPSK;

WiFiClient espClient;

// callback function for mqtt. Obligatory for PubSubclient.
void callback1(char *topic, byte *payload, unsigned int length)
{
  // do something with the message

  String topicStr = topic;
  String recv_payload = String((char *)payload);

  Serial.println("mqtt_callback - message arrived - topic [" + topicStr +
                 "] payload [" + recv_payload + "]");
}
void callback2(char *topic, byte *payload, unsigned int length)
{
  // do something with the message

  String topicStr = topic;
  String recv_payload = String((char *)payload);

  Serial.println("mqtt_callback - message arrived - topic [" + topicStr +
                 "] payload [" + recv_payload + "]");
}
#ifdef TAGO
PubSubClient tago_client(BROKER_URL, 1883, callback1, espClient);
#endif
#ifdef LOCAL
PubSubClient local_client(LOCAL_BROKER_URL, LOCAL_BROKER_PORT, callback2, espClient);
#endif
float ADC_value = 0;
float AmpsRMS = 0;
int state = 0;
double sum;
double samples;
uint32_t session_id = SESSION_ID;

float ADC_vdd = 0;

int device_state;
int prev_device_state;

// SPIFFS function definitions
struct HoursOfOperationData
{
  unsigned long lastUpdate;       // Last update time (Unix timestamp)
  unsigned long hoursOfOperation; // Total hours of operation
};
HoursOfOperationData init_time;
uint32_t readNumberFile(fs::FS &fs, const char *path);
HoursOfOperationData readTimeFromFile(fs::FS &fs, const char *counterfile);
void writeTimeToFile(fs::FS &fs, const char *counterfile, const HoursOfOperationData &data);
void writeNumberToFile(fs::FS &fs, const char *counterfile, int session_id);
const char *counterfilename = "/counter.txt";
const char *sessionfilename = "/session_id.txt";

HoursOfOperationData TimeData;

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

  lcd.init();      // init the LCD
  lcd.backlight(); // Turn on the backlight on LCD.

  // start the filesystem. If there is an error, loop infinitely.
  if (!SPIFFS.begin(true))
  {
    Serial.println("An error occurred while mounting SPIFFS");
    lcd.print("Filesystem error!");
    while (true)
      ;
  }
  // Check if the time counter file exists.
  // IF the file exists, read the contents into the variable
  if (!SPIFFS.exists(counterfilename))
  {
    Serial.println("Counter File does not exist. Creating...");
    File file = SPIFFS.open(counterfilename, "w");
    if (!file)
    {
      Serial.println("Failed to create file");
      lcd.print("error counterfile");
      while (true)
        ;
    }
    else
    init_time.hoursOfOperation = 0;
    init_time.lastUpdate = 0;
    writeTimeToFile(SPIFFS, counterfilename, init_time);
    file.close();
  }
  TimeData = readTimeFromFile(SPIFFS, counterfilename);

  //RESET TIME
  //  init_time.hoursOfOperation = 0;
  //  init_time.lastUpdate = 0;
  //  writeTimeToFile(SPIFFS, counterfilename, init_time);

  // Check if the session ID file exists.
  // IF the file exists, read the contents into the variable
  if (!SPIFFS.exists(sessionfilename))
  {
    Serial.println("Session File does not exist. Creating...");
    File file = SPIFFS.open(sessionfilename, "w");
    if (!file)
    {
      Serial.println("Failed to create file");
      lcd.print("error sessionfile");
      while (true)
        ;
    }
    writeNumberToFile(SPIFFS, counterfilename, SESSION_ID);
    file.close();
  }
  session_id = readNumberFile(SPIFFS, sessionfilename);

  //RESET SESSION ID
  //writeNumberToFile(SPIFFS, sessionfilename, SESSION_ID);
  
  lcd.print("EcoWashMate");
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

  lcd.clear();
  lcd.print("Vdd = ");
  lcd.print((ADC_vdd * 0.0001875));
  lcd.print(" V");
  delay(1500);
  lcd.setCursor(0, 1);
  lcd.print("connecting to wifi");
  Serial.println("Setup WiFi and MQTT");
  setup_wifi();
  connect_mqtt();

  // reset ADC values for measuring current
  ADS.reset();
  ADS.setGain(0);     // 6.144 volt
  ADS.setDataRate(7); // 0 = slow   4 = medium   7 = fast
  ADS.setMode(0);     // continuous mode
  ADS.readADC(0);     // first read to trigger ADC
  startTime = millis();
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
        sum = sum + (ADC_value * ADC_value); // square value
        samples++;
      }
      if ((millis() - previousMillis) >= printPeriod)
      {                            // every printPeriod we do the calculation
        previousMillis = millis(); //   update time
        sum = sum / samples;       // get the average current measured
        // Serial.println(samples);
        // Serial.println(sum);
        Serial.print("current: ");
        Serial.print(AmpsRMS);
        Serial.println(" amps RMS");
        // calculate the RMS value. square root sum, multiply by voltage per value and multiply by slope (mV/A).
        // intercept is the zero adjustment.
        AmpsRMS = (sqrt(sum) * (6.144 / 32768) * slope) - intercept;

        if (AmpsRMS < 0)
        {
          AmpsRMS = 0;
        }
        lcd.setCursor(0, 0);
        lcd.print("current= ");
        lcd.print(AmpsRMS);
        lcd.print("A");
        sum = 0;
        samples = 0;
        state = 3;
      }
    }

    // state 3: send the values to MQTT broker
    if (state == 3)
    {

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
        state["value"] = "1"; // 1 = ON
        state["group"] = String(session_id);
        JsonObject meta = state.createNestedObject("metadata");
        meta["wasmachine_id"] = WASMACHINE_ID;
        meta["sensor_id"] = WASMACHINE_ID;
        meta["this_cycle_time"] = (elapsedTime / 1000);
        meta["total_time_operated"] = TimeData.hoursOfOperation / 1000;
        char JSONmessageBuffer[200];
        serializeJson(JSONbuffer, JSONmessageBuffer);
        #ifdef TAGO
        if (tago_client.publish("acs712/state", JSONmessageBuffer) == true)
        {
          Serial.println("The cycle has started");
          lcd.setCursor(0,1);
          lcd.print("cycle started");
        }
        #endif
        #ifdef LOCAL
        if (local_client.publish("meter2/state", JSONmessageBuffer) == true){
          Serial.println("published to local client");
        }
        #endif
      }

      // IF the device state has changed from OFF to ON, increment session ID and write it to file, update start time
      if (device_state == 1)
      {
        session_id = session_id + 1;
        writeNumberToFile(SPIFFS, sessionfilename, session_id);
        device_state = 2;
        startTime = millis();
        elapsedTime = 0;
      }

      // Only send sensor data if the machine is ON
      // send the value in mA as INT.
      if (device_state == 2 || device_state == 3)
      {
        #ifdef TAGO
        if(!(tago_client.connected())){
          connect_mqtt();
        }
        #endif
        #ifdef LOCAL
        if(!(local_client.connected())){
          connect_mqtt();
        }
        #endif
        StaticJsonDocument<200> JSONbuffer;               // make a json object
        JsonArray array = JSONbuffer.to<JsonArray>();     // create an array
        JsonObject amperage = array.createNestedObject(); // create a nested object in the array
        amperage["variable"] = "current";                 // add the variable info
        amperage["group"] = String(session_id);
        amperage["unit"] = "mA";
        amperage["value"] = int(AmpsRMS * 1000);
        JsonObject metadata = amperage.createNestedObject("metadata");
        metadata["sensor_id"] = SENSOR_ID;
        metadata["wasmachine_id"] = WASMACHINE_ID;
        metadata["this_cycle_time"] = (elapsedTime / 1000);
        metadata["total_time_operated"] = TimeData.hoursOfOperation / 1000;
        char JSONmessageBuffer[200];
        serializeJson(JSONbuffer, JSONmessageBuffer);
        // publish the serialised buffer to the broker
        #ifdef TAGO
        if (tago_client.publish("acs712", JSONmessageBuffer) == true)
        {
          Serial.println("Success sending message");
        }
        else
        {
          Serial.println("Error sending message");
        }
        #endif
        #ifdef LOCAL
        if (local_client.publish("meter2", JSONmessageBuffer) == true)
        {
          Serial.println("published to local client");
        }
        else
        {
          Serial.println("Error sending message to local client");
        }
        #endif
      }
      // IF the device is ON and the current is less than 0,5A for END_OF_CYCLE ms
      // THEN update the device state to OFF, and publish the state on the broker
      
      if (AmpsRMS < 0.5 && device_state == 2)
      {
        device_state = 3;
        EndOfCycle = millis();
      }
      if (device_state == 3){
        if ((millis() - EndOfCycle) >= END_OF_CYCLE)
        {
          device_state = 0;
          StaticJsonDocument<100> JSONbuffer;
          JsonArray array = JSONbuffer.to<JsonArray>();
          JsonObject state = array.createNestedObject();
          state["variable"] = "state";
          state["value"] = "2"; //2 = OFF
          state["group"] = String(session_id);
          JsonObject meta = state.createNestedObject("metadata");
          meta["wasmachine_id"] = WASMACHINE_ID;
          meta["sensor_id"] = WASMACHINE_ID;
          meta["this_cycle_time"] = (elapsedTime / 1000);
          meta["total_time_operated"] = TimeData.hoursOfOperation / 1000;
          char JSONmessageBuffer[100];
          serializeJson(JSONbuffer, JSONmessageBuffer);
          #ifdef TAGO
          if (tago_client.publish("acs712/state", JSONmessageBuffer) == true)
          {
            Serial.println("The cycle has ended");
            lcd.setCursor(0,1);
            lcd.print("cycle stopped");
          }
          #endif
          #ifdef LOCAL
          if (local_client.publish("meter2/state", JSONmessageBuffer) == true)
          {
            Serial.println("published to local client)");
            Serial.println("The cycle has ended");
            lcd.setCursor(0,1);
            lcd.print("cycle stopped");
          }
          #endif
          Serial.println(millis() - EndOfCycle);
          
          
          Serial.println("Statistics:");
          Serial.println("total seconds on:");
          Serial.println(TimeData.hoursOfOperation / 1000);
          Serial.println("last cycle in seconds:");
          Serial.println(TimeData.lastUpdate);
        }     
      }
      state = 0;
    }
    // If device is on, record the time that it was on.
    if (device_state == 2 || device_state == 3)
    {
      // Calculate the elapsed time since the start time
      elapsedTime = millis() - startTime;
      TimeData.hoursOfOperation += (elapsedTime - TimeData.lastUpdate);
      Serial.println(TimeData.hoursOfOperation);
      TimeData.lastUpdate = elapsedTime;
      writeTimeToFile(SPIFFS, counterfilename, TimeData);
    }
  }
}

void connect_mqtt()
{
  // connect to brokers with auth and set the LWT message
  #ifdef TAGO
  Serial.print("connecting to tago...");
  
  while (!tago_client.connect("meter2", "Token", TAGO_TOKEN, "meter2/state", 1, 1, "[{\"variable\":\"state\",\"value\":\"offline\"}]"))
    ;
  {
    Serial.print(".");
    delay(1000);
  }
  #endif
  #ifdef LOCAL
  Serial.print("connecting to local broker...");
  while (!local_client.connect("meter2", "meter2", MQTT_PASSWORD, "meter2/state", 1, 1, "[{\"variable\":\"state\",\"value\":\"offline\"}]"))
    ;
  {
    Serial.print(".");
    delay(1000);
  }
  #endif
}

void setup_wifi()
{
  // connect to wifi with ssid and password
  // start the DNS client to look up the IP address of the broker
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("connecting to ");
  lcd.setCursor(0, 1);
  lcd.print(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  delay(200);
  while (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("Connection Failed! Rebooting...");
    lcd.setCursor(0, 0);
    lcd.print("connection failed!");
    lcd.setCursor(0, 1);
    lcd.print("Rebooting...");
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

// function to measure VDD on channel 1 of the ADC converter
float measure_vdd(void)
{
  float ADC = 0;
  // Measure Vdd. take the average of 1000 samples.
  for (int i = 0; i < 1000; i++)
  {
    // wait for conversion to finish. (conversion takes 1160µs)
    delay(2);
    ADC = ADC + ADS.readADC(1);
  }
  ADC = ADC / 1000;
  return ADC;
}

// function to read the counter from the filesystem
uint32_t readNumberFile(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\r\n", path);
  File file = fs.open(path, FILE_READ);
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");
    return 0;
  }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  // Close the file
  file.close();
  // Read the file into a uint32_t variable
  uint32_t fileContent;
  fileContent = doc["session_id"];

  return fileContent;
}

HoursOfOperationData readTimeFromFile(fs::FS &fs, const char *counterfile)
{
  Serial.printf("Reading file: %s\r\n", counterfile);
  File file = fs.open(counterfile, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return {0, 0}; // Return default values
  }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error)
  {
    Serial.println("Failed to parse file");
    return {0, 0}; // Return default values
  }
  HoursOfOperationData data;
  data.lastUpdate = doc["lastUpdate"] ;
  data.hoursOfOperation  = doc["hoursOfOperation"] ;
  return data;
}

void writeTimeToFile(fs::FS &fs, const char *counterfile, const HoursOfOperationData &data)
{
  File file = SPIFFS.open(counterfile, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  StaticJsonDocument<256> doc;
  doc["lastUpdate"] = data.lastUpdate;
  doc["hoursOfOperation"] = data.hoursOfOperation ;

  if (serializeJson(doc, file) == 0)
  {
    Serial.println("Failed to write to file");
    return;
  }
  
  file.close();
}

void writeNumberToFile(fs::FS &fs, const char *counterfile, int session_id){
  File file = SPIFFS.open(counterfile, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
    StaticJsonDocument<256> doc;
  doc["session_id"] = session_id;

  if (serializeJson(doc, file) == 0)
  {
    Serial.println("Failed to write to file");
    return;
  }
  return;
}