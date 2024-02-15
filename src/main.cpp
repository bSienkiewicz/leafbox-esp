#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <AsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include <WebSerial.h>
#include <PubSubClient.h>
#include "utils.h"
#include "esp_sleep.h"
#include "Preferences.h"
#include "SPIFFS.h"
#include "HTTPClient.h"
#include "time.h"

#define LED 2
#define CALIBRATION_BUTTON 4
#define TIME_TO_SLEEP 1200 // seconds
#define MQTT_COMMAND_TOPIC "esp/device/command"

Preferences preferences;
AsyncWebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);

char msg[50];

//// CONFIGURATION CONSTANTS
static String MQTT_HOST = "";
const bool SLEEP_MODE = false;
const String AP_NAME = "LEAFBOX";
const String AP_PASSWORD = "";
const int MAX_PLANTS = 4;
const int SENSOR_PINS[] = {34, 35, 32, 33};
const int VALVE_PINS[] = {19, 21, 22, 23};
const String CONFIG_ENDPOINT = "http://192.168.0.69:5000/api/config/";

//// LOCAL TIME CONSTANTS
const char *ntpServer = "pool.ntp.org";
const int gmtOffset_sec = 0; // 3600
const int daylightOffset_sec = 3600;

//// BOOLEAN FLAGS
bool configured = true;
bool wifi_connected = false;
bool mqtt_configured = false;
bool sockets_configured = false;
bool timeSynchronized = false;

//// SENSOR CALIBRATION VALUES
bool calibration_flags[2] = {false, false};
int calibration_values[2] = {0, 0};

/*          SOIL MOISTURE SENSOR          */
class Plant
{
private:
  int sensorPin;
  int valvePin;
  int moisture0;
  int moisture100;
  int moisturePercentage;
  int readingAccuracy = 20;
  int lowerTreshold;
  int upperTreshold;
  int wateringTime = 5;

public:
  int socket;
  int plantId;
  bool configured;
  time_t lastReading;
  int readingDelay;
  int readingDelayMult;
  SemaphoreHandle_t xSemaphore = xSemaphoreCreateBinary();

  Plant()
  {
    this->configured = false;
  }

  Plant(int socket, int sensorPin, int valvePin, int moisture0, int moisture100, int lowerTreshold, int upperTreshold, time_t lastReading, int plantId, int readingDelay, int readingDelayMult)
  {
    this->socket = socket;
    this->plantId = plantId;
    this->sensorPin = sensorPin;
    this->valvePin = valvePin;
    this->moisture0 = moisture0;
    this->moisture100 = moisture100;
    this->lowerTreshold = lowerTreshold;
    this->upperTreshold = upperTreshold;
    this->lastReading = lastReading;
    this->readingDelay = readingDelay;
    this->readingDelayMult = readingDelayMult;
    this->configured = true;
    WebSerial.print("Configured plant on socket #");
    WebSerial.println(this->socket);
    WebSerial.println("Reading delay: " + String(this->readingDelay * this->readingDelayMult) + " seconds");
  }

  int getSensorData()
  {
    return analogRead(sensorPin);
  }

  int readMoisturePercentage()
  {
    int reading = 0;
    for (int i = 0; i < readingAccuracy; i++)
    {
      reading += getSensorData();
      delay(10);
    }
    reading /= readingAccuracy;
    if (moisture0 - moisture100 > 0)
    {
      if (reading > moisture0)
        moisturePercentage = 0;
      else if (reading < moisture100)
        moisturePercentage = 100;
      else
        moisturePercentage = (int)((static_cast<float>(moisture0 - reading) / static_cast<float>(moisture0 - moisture100)) * 100);
    }
    else
    {
      WebSerial.println("Awaiting calibration");
    }
    return moisturePercentage;
  }

  int getMoisturePercentage()
  {
    return moisturePercentage;
  }

  String getSensorConfigValues()
  {
    return String(moisture0) + "|" + String(moisture100);
  }

  void startPump()
  {
    digitalWrite(valvePin, LOW);
  }

  void stopPump()
  {
    digitalWrite(valvePin, HIGH);
  }

  void waterPlant()
  {
    WebSerial.print("→ Watering plant #" + String(this->plantId) + " on socket #" + String(this->socket) + "... ");
    startPump();
    delay(wateringTime * 1000);
    stopPump();
    delay(3000);
  }

  bool isWateringNeeded()
  {
    return lowerTreshold > readMoisturePercentage();
  }

  void loop()
  {
    int wateringCycles = 0;
    publishReading(readMoisturePercentage());
    while (isWateringNeeded() && wateringCycles <= 3)
    {
      waterPlant();
      wateringCycles++;
    }
    if (wateringCycles > 0)
    {
      publishReading(readMoisturePercentage());
    }
    if (wateringCycles > 3)
    {
      WebSerial.println("✗ Failed to water plant #" + String(this->plantId) + " on socket #" + String(this->socket) + ". Max watering cycles reached.");
    }
  }

  void calibration()
  {
    // STEP 1
    WebSerial.println("Calibration step 1");
    int m0 = 0;
    int m100 = 0;
    unsigned long startTime = millis();
    while (calibration_flags[0] == false && millis() - startTime < 60000)
    {
      vTaskDelay(100);
    }
    if (calibration_flags[0] == false)
    {
      WebSerial.println("Calibration aborted during step 1");
      vTaskDelete(NULL);
    }
    for (int i = 0; i < 10; i++)
    {
      m0 += getSensorData();
      delay(100);
    }
    m0 /= 10;

    // STEP 2
    WebSerial.println("Calibration step 2");
    startTime = millis();
    while (calibration_flags[1] == false && millis() - startTime < 60000)
    {
      vTaskDelay(100);
    }
    if (calibration_flags[1] == false)
    {
      WebSerial.println("Calibration aborted during step 2");
      vTaskDelete(NULL);
    }
    for (int i = 0; i < 10; i++)
    {
      m100 += getSensorData();
      delay(100);
    }
    m100 /= 10;

    // STEP 3
    WebSerial.println("Calibration complete. New values: m0: " + String(m0) + " m100: " + String(m100));
    calibration_values[0] = m0;
    calibration_values[1] = m100;
    xSemaphoreGive(xSemaphore);
    vTaskDelete(NULL);
  }

  static void calibrationTask(void *pvParameters)
  {
    Plant *plant = (Plant *)pvParameters;
    plant->calibration();
  }

  void startCalibration()
  {
    xTaskCreatePinnedToCore(
        calibrationTask,   /* Function to run */
        "CalibrationTask", /* Name of the task */
        10000,             /* Stack size (in words) */
        this,              /* Task input parameter */
        1,                 /* Task priority */
        NULL,              /* Task handle */
        0                  /* Core number */
    );
  }

  void publishReading(int moisture)
  {
    if (!this->configured)
    {
      return;
    }
    time_t now;
    time(&now);
    String topic = "esp/plant/" + String(this->plantId) + "/moisture";
    if (client.publish(topic.c_str(), String(moisture).c_str()))
    {
      Serial.println("✓ Published M for #" + String(this->plantId) + ". Value: " + String(this->readMoisturePercentage()) + "%");
      WebSerial.println("✓ Published M for #" + String(this->plantId) + ". Value: " + String(this->readMoisturePercentage()) + "%");
    }
    else
    {
      WebSerial.println("✗ Failed to publish.");
    }
    this->lastReading = now;
  }
};

Plant plants[MAX_PLANTS];

/*          TEMPERATURE SENSOR          */

class TemperatureSensor
{
private:
  int sensorPin;
  OneWire oneWire;
  DallasTemperature sensor;

public:
  int socket;
  bool configured;
  time_t lastReading;
  int readingDelay = 2;
  int readingDelayMult = 1;

  TemperatureSensor()
  {
    this->configured = false;
  }

  TemperatureSensor(int socket, int sensorPin, int lastReading)
      : oneWire(sensorPin), sensor(&oneWire)
  {
    this->socket = socket;
    this->sensorPin = sensorPin;
    this->lastReading = lastReading;
    this->configured = true;

    this->sensor.begin();

    Serial.print("Configured temperature sensor on socket #");
    Serial.println(this->socket);
  }

  float readTemperature()
  {
    this->sensor.requestTemperatures();
    float tempC = this->sensor.getTempCByIndex(0);
    return tempC;
  }

  void publishReading(float temperature)
  {
    if (!this->configured)
    {
      return;
    }

    if (temperature == -127.0)
    {
      Serial.println("Error: Sensor not connected");
      return;
    }
    
    String topic = "esp/device/" + String(WiFi.macAddress()) + "/temperature";
    if (client.publish(topic.c_str(), String(temperature).c_str()))
    {
      Serial.println("✓ Published Temp. Value: " + String(this->readTemperature()) + "ºC");
      WebSerial.println("✓ Published Temp. Value: " + String(this->readTemperature()) + "ºC");
    }
    else
    {
      WebSerial.println("✗ Failed to publish.");
    }
    
    time_t now;
    time(&now);
    this->lastReading = now;
  }

  void loop()
  {
    publishReading(readTemperature());
  }
};

TemperatureSensor temperatureSensors[MAX_PLANTS];
Plant plants_to_water[MAX_PLANTS];

void recvMsg(uint8_t *data, size_t len)
{
  String d = "";
  for (int i = 0; i < len; i++)
  {
    d += char(data[i]);
  }
  WebSerial.println("[ " + d + " ]");
  if (d == "ping")
  {
    WebSerial.println("pong");
  }
  if (d == "reboot")
  {
    ESP.restart();
  }
  if (d == "status")
  {
    // check which
    int plants_c[4] = {0, 0, 0, 0};
    for (Plant &plant : plants)
    {
      if (!plant.configured)
        continue;
      plants_c[plant.socket - 1]++;
    }
    time_t now;
    time(&now);
    WebSerial.println("============ STATUS ============");
    WebSerial.println("Time: " + String(ctime(&now)));
    WebSerial.println("WiFi: " + String(WiFi.status()));
    WebSerial.println("MQTT: " + String(client.connected()));
    WebSerial.println("Device configured: " + String(preferences.getBool("dev_configured", false)));
    WebSerial.print("Sockets configured: ");
    for (int i = 0; i < 4; i++)
    {
      WebSerial.print(plants_c[i] > 0 ? "#" + String(i + 1) + " " : "");
    }
    WebSerial.println();
    WebSerial.println("============ TIMING ============");

    for (Plant &plant : plants)
    {
      if (!plant.configured)
        continue;
      time_t last_watered = plant.lastReading;

      time_t now;
      time(&now);

      WebSerial.println("→ #" + String(plant.socket) + " - Next reading in minutes: " + String((plant.readingDelay * plant.readingDelayMult - difftime(now, last_watered)) / 60) + ". Time Diff: " + String(difftime(now, last_watered)));
    }
    WebSerial.println("================================");
  }
  if (d == "analog")
  {
    for (Plant &plant : plants)
    {
      if (!plant.configured)
        continue;
      WebSerial.println("Plant #" + String(plant.plantId) + " on socket #" + String(plant.socket) + " - M: " + String(plant.readMoisturePercentage()) + "% || V: " + String(plant.getSensorData()));
    }
  }
}

void clearPlants()
{
  for (int i = 0; i < MAX_PLANTS; i++)
  {
    plants[i] = Plant();
  }
}

void clearTemperatureSensors()
{
  for (int i = 0; i < MAX_PLANTS; i++)
  {
    temperatureSensors[i] = TemperatureSensor();
  }
}

void initSocketsFromConfig(JsonObject config)
{
  Serial.println("Reinitializing sockets from config...");
  clearPlants();
  clearTemperatureSensors();

  int plantsConfigured = 0;
  int tempConfigured = 0;

  for (JsonObject::iterator it = config.begin(); it != config.end(); ++it)
  {
    int plant_socket = atoi(it->key().c_str());           // Get the plant ID from the key
    JsonObject socketData = it->value().as<JsonObject>(); // Get the plant data from the value

    if (socketData["sensorType"] == "soil")
    {
      if (socketData["moistureMin"].as<int>() == 0 || socketData["moistureMax"].as<int>() == 0)
      {
        continue;
      }

      time_t lastReading = 0;
      struct tm tm;

      if (!socketData["lastReading"].isNull())
      {
        memset(&tm, 0, sizeof(tm));
        strptime(socketData["lastReading"].as<String>().c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
        lastReading = mktime(&tm);
        lastReading += 1 * 60 * 60;
      }

      // Create the plants from the config
      plants[plant_socket - 1] = Plant(
          plant_socket, // 1
          SENSOR_PINS[plant_socket - 1],
          VALVE_PINS[plant_socket - 1],
          socketData["moistureMin"].as<int>(),
          socketData["moistureMax"].as<int>(),
          socketData["lowerTreshold"].as<int>(),
          socketData["upperTreshold"].as<int>(),
          lastReading,
          socketData["plantId"].as<int>(),
          socketData["readingDelay"].as<int>(),
          socketData["readingDelayMult"].as<int>());
      plantsConfigured++;
    }
    else if (socketData["sensorType"] == "temperature")
    {
      time_t lastReading = 0;
      struct tm tm;

      memset(&tm, 0, sizeof(tm));
      strptime(socketData["lastReading"].as<String>().c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
      lastReading = mktime(&tm);
      lastReading += 1 * 60 * 60;

      temperatureSensors[plant_socket - 1] = TemperatureSensor(
          plant_socket,
          SENSOR_PINS[plant_socket - 1],
          lastReading
          );
      tempConfigured++;
    }
  }

  time_t now;
  time(&now);
  sockets_configured = true;
  Serial.print("✓ Initialized ");
  Serial.print(plantsConfigured);
  Serial.print(" plants and ");
  Serial.print(tempConfigured);
  Serial.println(" temperature sensors");
  delay(100);
}

void flashLED(int times)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(LED, HIGH);
    delay(100);
    digitalWrite(LED, LOW);
    delay(100);
  }
}

void printLocalTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

time_t stringToTime(String timeString)
{
  struct tm tm;
  strptime(timeString.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
  return mktime(&tm) - (1 * 3600); // Subtract 2 hours to adjust for GMT
}

String processor(const String &var)
{
  if (var == "WIFI")
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      return "Connected to " + WiFi.SSID();
    }
    else
    {
      return "Not connected";
    }
  }
  else if (var == "LOCALIP")
  {
    return WiFi.localIP().toString();
  }
  else if (var == "CONFIGURED")
    return preferences.getBool("dev_configured", false) ? "Yes" : "No";
  else if (var == "MQTT_STATUS")
    return client.connected() ? "Connected to mqtt://" + preferences.getString("mqtt_host", "") : "Not connected";
  else if (var == "WIFI_SSID")
    return preferences.getString("wifi_ssid", "");
  else if (var == "WIFI_PASSWORD")
    return preferences.getString("wifi_password", "");
  else if (var == "WIFI_CHANNEL")
    return preferences.getString("wifi_channel", "1");
  else if (var == "MQTT_HOST")
    return preferences.getString("mqtt_host", "");
  else if (var == "AP_NAME")
    return preferences.getString("ap_name", AP_NAME);
  else if (var == "AP_PASSWORD")
    return preferences.getString("ap_password", AP_PASSWORD);
  else
    return String();
}

String getWiFiNetworks()
{
  Serial.print("Found WiFi networks... ");
  int numNetworks = WiFi.scanNetworks();
  Serial.println(numNetworks);
  String json = "[";
  for (int i = 0; i < numNetworks; i++)
  {
    if (i > 0)
    {
      json += ",";
    }
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  return json;
}
void setup_wifi()
{
  String ssid = preferences.getString("wifi_ssid");
  String password = preferences.getString("wifi_password");
  WiFi.begin(ssid, password);
  Serial.println("Connecting to " + ssid + " ...");

  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 20)
  {
    delay(500);
    Serial.print(".");
    counter++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("");
    Serial.println("✓ Connected to " + ssid + " with IP address: " + WiFi.localIP().toString());
    wifi_connected = true;
    digitalWrite(LED, LOW);
  }
  else
  {
    Serial.println("\n✗ Failed to connect to WiFi within 10 seconds.");
    WiFi.disconnect();
  }
}

void callback(char *topic, byte *message, unsigned int length)
{
  String topicString = String(topic);
  char messageArr[length + 1];
  memcpy(messageArr, message, length);
  messageArr[length] = '\0';
  String messageString = String(messageArr);

  std::vector<String> topicParts;

  int startIndex = 0;
  int endIndex = 0;

  while ((endIndex = topicString.indexOf('/', startIndex)) != -1)
  {
    topicParts.push_back(topicString.substring(startIndex, endIndex));
    startIndex = endIndex + 1;
  }

  topicParts.push_back(topicString.substring(startIndex));

  if (topicParts[2] == "command" && messageString[0] == '{')
  {
    StaticJsonDocument<1024> msg_doc;
    StaticJsonDocument<1024> msg_data;
    deserializeJson(msg_doc, messageString);
    JsonObject obj = msg_doc.as<JsonObject>();
    String type = obj["type"].as<String>();
    String mac = obj["mac"].as<String>();
    JsonObject data = obj["data"].as<JsonObject>();

    if (mac == WiFi.macAddress())
    {
      if (type == "config")
      {
        initSocketsFromConfig(data);
      }
      else if (type == "calibrate")
      {
        int step = data["step"].as<int>();
        int plant = data["plant"].as<int>();
        if (step == 1)
        {
          plants[plant - 1].startCalibration();
        }
        else if (step == 2)
        {
          calibration_flags[0] = true;
        }
        else if (step == 3)
        {
          calibration_flags[1] = true;
          xSemaphoreTake(plants[plant - 1].xSemaphore, portMAX_DELAY);
          String payload = "{\"mac\":\"" + mac + "\", \"socket\":" + String(plant) + ", \"values\":\"" + String(calibration_values[0]) + "|" + String(calibration_values[1]) + "\"}";
          client.publish("esp/device/calibration", payload.c_str());
          calibration_flags[0] = false;
          calibration_flags[1] = false;
        }
      }
      else if (type == "reboot")
      {
        ESP.restart();
      }
      else if (type == "reading")
      {
        int plant_id = data["plant_id"].as<int>();
        for (Plant &plant : plants)
        {
          if (!plant.configured)
            continue;
          if (plant.plantId == plant_id)
          {
            plant.publishReading(plant.readMoisturePercentage());
          }
        }
      }
      else if (type == "watering")
      {
        int plant_id = data["plant_id"].as<int>();
        for (Plant &plant : plants)
        {
          if (!plant.configured)
            continue;
          if (plant.plantId == plant_id)
          {
            plant.waterPlant();
            plant.publishReading(plant.readMoisturePercentage());
          }
        }
      }
    }
  }
}

void conn_reconnect()
{
  delay(200);
  client.disconnect();
  if (WiFi.status() != WL_CONNECTED)
    setup_wifi();
  String client_id = "leafbox-client-";
  client_id += String(random(0xffff), HEX);
  if (client.connect(client_id.c_str()))
  {
    client.setKeepAlive(240);
    client.setSocketTimeout(240);
    client.setBufferSize(1024);
    String subscription_topic = MQTT_COMMAND_TOPIC;
    client.subscribe(subscription_topic.c_str());
    client.setCallback(callback);
    Serial.println("✓ Connected to MQTT broker on " + MQTT_HOST + ":1883 as " + client_id);
  }
  else
  {
    Serial.print("✗ Failed to connect to MQTT broker as ");
    Serial.print(client_id.c_str());
    Serial.print(" on host ");
    Serial.print(MQTT_HOST);
    Serial.print(":1883");
    Serial.println(" Retrying in 5 seconds...");
    flashLED(3);
    delay(5000);
  }
}

void mqtt_config(int port)
{
  client.setServer(MQTT_HOST.c_str(), 1883);
  mqtt_configured = true;
  Serial.println("✓ MQTT configuration successful");
}

void setupServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html", false, processor); 
            WebSerial.println("✓ HTTP Client connected"); });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/style.css", "text/css"); });
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String wifiNetworks = getWiFiNetworks();
    request->send(200, "application/json", wifiNetworks); });
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("ssid", true) && request->hasParam("channel", true) && request->hasParam("mqtt_host", true)) {
      AsyncWebParameter *ssid = request->getParam("ssid", true);
      AsyncWebParameter *password = request->getParam("password", true);
      AsyncWebParameter *channel = request->getParam("channel", true);
      AsyncWebParameter *mqtt_host = request->getParam("mqtt_host", true);
      AsyncWebParameter *backend_p = request->getParam("backend_p", true);
      AsyncWebParameter *ap_name = request->getParam("ap_name", true);
      AsyncWebParameter *ap_password = request->getParam("ap_password", true);

      // DEBUG PRINTS
      // WebSerial.println("ssid: " + ssid->value());
      // WebSerial.println("password: " + password->value());
      // WebSerial.println("channel: " + channel->value());
      // WebSerial.println("mqtt_host: " + mqtt_host->value());
      // WebSerial.println("backend_p: " + backend_p->value());
      // WebSerial.println("ap_name: " + ap_name->value());
      // WebSerial.println("ap_password: " + ap_password->value());

      preferences.putString("wifi_ssid", ssid->value());
      preferences.putString("wifi_password", password->value());
      preferences.putString("wifi_channel", channel->value());
      preferences.putString("mqtt_host", mqtt_host->value());
      preferences.putString("backend_p", backend_p->value());
      preferences.putString("ap_name", ap_name->value());
      preferences.putString("ap_password", ap_password->value());
      preferences.putBool("dev_configured", true);

      request->send(200, "text/plain", "Configuration saved. Restarting the device in 5 seconds...");
      delay(5000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "Missing parameters");
    } });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    preferences.clear();
    request->send(200, "text/plain", "Device reset. Restarting in 5 seconds...");
    delay(5000);
    ESP.restart(); });

  // dnsServer.start(53, "*", WiFi.softAPIP());
  // server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
  server.begin();
  Serial.println("✓ Server setup successful");
}

void setup_ap()
{
  if (preferences.getBool("dev_configured", false))
  {
    WiFi.softAP(preferences.getString("ap_name").c_str(), preferences.getString("ap_password").c_str());
  }
  else
  {
    WiFi.softAP(AP_NAME.c_str(), AP_PASSWORD.c_str());
  }
  Serial.println("✓ AP started with name: " + WiFi.softAPSSID() + " and IP: " + WiFi.softAPIP().toString());
}

void connection_init()
{
  if (preferences.getBool("dev_configured", false))
  {
    setup_wifi();
    mqtt_config(1883);
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
  else
  {
    Serial.println("✗ Connection not configured. WiFi and MQTT not connected");
  }
  WebSerial.begin(&server);
  WebSerial.msgCallback(recvMsg);

  Serial.print("✓ Connection setup successful.");
}

void requestConfig()
{
  if (!client.connected() || WiFi.status() != WL_CONNECTED)
    conn_reconnect();
  if (!sockets_configured)
  {
    String request_topic = "esp/device/config/request";
    client.publish(request_topic.c_str(), WiFi.macAddress().c_str());
  }
  delay(1000);
}

void sustain_connection(void *pvParameters)
{
  while (true)
  {
    if (preferences.getBool("dev_configured", false) && WiFi.isConnected() && !client.connected() && mqtt_configured)
    {
      Serial.println("! MQTT disconnected. Reconnecting...");
      conn_reconnect();
    }
    if (!client.connected())
      conn_reconnect();
    if (!sockets_configured)
      requestConfig();
    client.loop();
    vTaskDelay(50);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  if (!SPIFFS.begin(true))
  {
    Serial.println("✗ An Error has occurred while mounting SPIFFS");
    return;
  }
  pinMode(LED, OUTPUT);
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  for (int i = 0; i < MAX_PLANTS; i++)
  {
    pinMode(SENSOR_PINS[i], INPUT);
    pinMode(VALVE_PINS[i], OUTPUT);
    digitalWrite(VALVE_PINS[i], HIGH);
  }
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0);
  WiFi.mode(WIFI_AP_STA);
  digitalWrite(LED, HIGH);
  preferences.begin("device_config", false);
  setupServer();
  setup_ap();
  MQTT_HOST = preferences.getString("mqtt_host", "");

  connection_init();

  Serial.println("⌚ synchronizing time...");
  while (!timeSynchronized)
  {
    time_t now;
    time(&now);
    if (now > 24 * 3600)
    {
      timeSynchronized = true;
      Serial.println("✓ Time synchronized. Current time: " + String(ctime(&now)));
    }
    delay(20);
  }

  xTaskCreatePinnedToCore(
      sustain_connection,  /* Function to run */
      "SustainConnection", /* Name of the task */
      10000,               /* Stack size (in words) */
      NULL,                /* Task input parameter */
      2,                   /* Task priority */
      NULL,                /* Task handle */
      0                    /* Core number */
  );
  Serial.println("✓ Setup complete. Device ready.");
}

unsigned long lastOutputMillis = 0;
unsigned long lastCheckMillis = 0;

void loop()
{
  for (Plant &plant : plants)
  {
    if (!plant.configured)
    {
      continue;
    }

    time_t last_watered = plant.lastReading;

    time_t now;
    time(&now);

    if ((difftime(now, last_watered) > plant.readingDelay * plant.readingDelayMult) || difftime(now, last_watered) < 0)
    {
      plant.loop();
    }
  }

  for (TemperatureSensor &sensor : temperatureSensors)
  {
    if (!sensor.configured)
    {
      continue;
    }

    time_t last_reading = sensor.lastReading;

    time_t now;
    time(&now);

    if ((difftime(now, last_reading) > sensor.readingDelay * sensor.readingDelayMult) || difftime(now, last_reading) < 0)
    {
      sensor.loop();
    }
  }

  delay(1000);

  // if (millis() - lastOutputMillis > 10000 && false)
  // {
  //   lastOutputMillis = millis();
  //   if (sockets_configured)
  //   {
  //     Serial.println("=====================================");
  //     for (Plant &plant : plants)
  //     {
  //       if (!plant.configured)
  //         continue;
  //       time_t last_watered = plant.lastReading;

  //       time_t now;
  //       time(&now);

  //       Serial.println("→ #" + String(plant.socket) + " - Next reading in minutes: " + String((plant.readingDelay * plant.readingDelayMult - difftime(now, last_watered)) / 60));
  //     }
  //     Serial.println("=====================================");
  //   }
  // }
}
