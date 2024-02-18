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

#ifndef STASSID
#define STASSID "AP_BAS_go"
#define STAPSK "0499619079"
// TagoIO device token
#define TAGO_TOKEN "3ced64a7-20ec-44af-88de-058729507baf"
#endif

ADS1115 ADS(0x48);
LiquidCrystal_I2C lcd(0x3F, 16, 2);
// slope = 1/accuracy in volt. For the 20A model the accuracy is 100mV/A
float slope = 10;
float intercept = 0.09;

#define END_OF_CYCLE 10000

unsigned long printPeriod = 1000; // in milliseconds
unsigned long previous_calibration = 0;
unsigned long calibration_time = 60000; //period for recalibrating vdd in ms.
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
uint32_t session_id = 0;
uint32_t hours_operated = 0;

float ADC_vdd = 0;

int device_state;
int prev_device_state;

//SPIFFS function definitions
uint32_t readNumberFile(fs::FS &fs, const char * path);
const char* counterfilename = "/counter.txt";
const char* sessionfilename = "/session_id.txt";


void connect_mqtt();
void setup_wifi();
float measure_vdd(void);

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

  lcd.init();         //init the LCD
  lcd.backlight();    // Turn on the backlight on LCD.

//start the filesystem. If there is an error, loop infinitely.
  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS");
    lcd.print("Filesystem error!");
    while(true);
  }
  // Check if the counter file exists.
  // IF the file exists, read the contents into the variable 
  if (!SPIFFS.exists(counterfilename)) {
    Serial.println("Counter File does not exist. Creating...");
    File file = SPIFFS.open(counterfilename, "w");
    if (!file) {
      Serial.println("Failed to create file");
      lcd.print("error counterfile");
      while(true);
    }
    file.println(hours_operated);
    file.close();
  }
  else{
    hours_operated = readNumberFile(SPIFFS, counterfilename);
  }
  // Check if the session ID file exists.
  // IF the file exists, read the contents into the variable 
  if (!SPIFFS.exists(sessionfilename)) {
    Serial.println("Session File does not exist. Creating...");
    File file = SPIFFS.open(sessionfilename, "w");
    if (!file) {
      Serial.println("Failed to create file");
      lcd.print("error sessionfile");
      while(true);
    }
    file.println(session_id);
    file.close();
  }
  else{
    session_id = readNumberFile(SPIFFS, sessionfilename);
  }
  lcd.print("EcoWashMate");
// Show welcome message. Meanwhile wait for vdd to stabilise
  delay(2000);
  lcd.clear();
  lcd.print("calibrating...");

//function to measure vdd
  ADC_vdd = measure_vdd();

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
        state["value"] = "on";
        char JSONmessageBuffer[200];
        serializeJson(JSONbuffer, JSONmessageBuffer);
        if (client.publish("acs712/state", JSONmessageBuffer) == true)
        {
          Serial.println("The cycle has started");
        }
      }

      //IF the device state has changed from OFF to ON, add 1 to session ID
      if (device_state == 1)
      {
        session_id = session_id + 1;
        device_state = 2;
      }

      //Only send sensor data if the machine is ON
      if (device_state == 2){
        
        StaticJsonDocument<200> JSONbuffer;                           // make a json object
        JsonArray array = JSONbuffer.to<JsonArray>();                 // create an array
        JsonObject amperage = array.createNestedObject();             //create a nested object in the array
        amperage["variable"] = "current";                             //add the variable info
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
      }
      // IF the device is ON and the current is less than 0,5A for END_OF_CYCLE ms
      // THEN update the device state to OFF, and publish the state on the broker
      if (AmpsRMS < 0.5 && device_state == 2)
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
   // every calibration_time we do the reading of VDD
  if ((millis() - previous_calibration) >= calibration_time)
    {                           
        previous_calibration = millis(); //   update time
        ADC_vdd = measure_vdd();
     }
  }
}

void connect_mqtt()
{
  // connect to tagoIO broker with token auth and set the LWT message
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

//function to measure VDD on channel 1 of the ADC converter
float measure_vdd(void){
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

//function to read the counter from the filesystem
uint32_t readNumberFile(fs::FS &fs, const char * path){
    Serial.printf("Reading file: %s\r\n", path);
    File file = fs.open(path);
    if(!file || file.isDirectory()){
        Serial.println("- failed to open file for reading");
        return 0;
    }
  // Read the file into a uint32_t variable
  uint32_t fileContent;
  size_t bytesRead = file.readBytes((char*)&fileContent, sizeof(fileContent));

  // Close the file
  file.close();
  return fileContent;
}