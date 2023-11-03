#include <Arduino.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <AsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include <PubSubClient.h>
#include "utils.h"
#include "esp_sleep.h"
#include "Preferences.h"
#include "SPIFFS.h"

#define LED 2
#define CALIBRATION_BUTTON 4
#define TIME_TO_SLEEP 1200 // seconds

Preferences preferences;
DNSServer dnsServer;
AsyncWebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);

long lastReading = 0;
char msg[50];

const bool SLEEP_MODE = true;
const String MQTT_USER = "plantwise";
const String AP_NAME = "LEAFBOX";
const String AP_PASSWORD = "";
const int MAX_PLANTS = 4;
const int SENSOR_PINS[] = {35, 34, 39, 36};
const int VALVE_PINS[] = {21, 22, 23, 19};

StaticJsonDocument<1024> config;
bool configured = true;
bool wifi_connected = false;
bool mqtt_configured = false;

// 0%: 1496 100%: 1042

class Plant
{
private:
  int sensorPin;
  int valvePin;
  int moisture0;
  int moisture100;
  int moisturePercentage;
  int readingAccuracy = 2;
  int lowerTreshold;
  int upperTreshold;
  int wateringTime;
  int wateringCycles = 0;

public:
  int id;
  bool configured;
  Plant()
  {
    this->configured = false;
  }

  Plant(int id, int sensorPin, int valvePin, int moisture0, int moisture100, int lowerTreshold, int upperTreshold, int wateringTime)
  {
    this->id = id;
    this->sensorPin = sensorPin;
    this->valvePin = valvePin;
    this->moisture0 = moisture0;
    this->moisture100 = moisture100;
    this->lowerTreshold = lowerTreshold;
    this->upperTreshold = upperTreshold;
    this->wateringTime = wateringTime;
    this->configured = true;
    Serial.print("Configured plant #");
    Serial.println(this->id);
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
      Serial.println("Awaiting calibration");
    }
    return moisturePercentage;
  }

  int getMoisturePercentage()
  {
    return moisturePercentage;
  }

  void startPump()
  {
    digitalWrite(valvePin, LOW);
  }

  void stopPump()
  {
    digitalWrite(valvePin, HIGH);
  }

  bool isWateringNeeded()
  {
    return lowerTreshold > readMoisturePercentage();
  }

  void loop()
  {
    while (isWateringNeeded() && wateringCycles < 4)
    {
      wateringCycles++;
      Serial.println("Watering...");
      startPump();
      delay(wateringTime * 1000);
      stopPump();
      delay(3000);
    }
  }

  void calibration()
  {
    digitalWrite(LED, HIGH);
    Serial.println("Put the sensor in the dry soil and press the button");
    while (digitalRead(CALIBRATION_BUTTON) == HIGH)
    {
      delay(100);
    }
    Serial.println("Calibrating...");
    int sum = 0;
    for (int i = 0; i < 5; i++)
    {
      sum += analogRead(sensorPin);
      delay(1000);
    }
    moisture0 = sum / 5;
    moisture0 -= 30;
    Serial.println("Put the sensor in the water and press the button");
    while (digitalRead(CALIBRATION_BUTTON) == HIGH)
    {
      delay(100);
    }
    Serial.println("Calibrating...");
    sum = 0;
    for (int i = 0; i < 5; i++)
    {
      sum += analogRead(sensorPin);
      delay(1000);
    }
    moisture100 = sum / 5;
    moisture100 += 30;
    Serial.println("Calibration finished");
    Serial.print("0%: " + String(moisture0) + " 100%: " + String(moisture100));
    digitalWrite(LED, LOW);
    delay(2000);
  }
};

Plant plants[MAX_PLANTS];

void clearPlants()
{
  for (int i = 0; i < MAX_PLANTS; i++)
  {
    plants[i] = Plant();
  }
}

void createPlantsFromConfig()
{
  clearPlants();

  int plantsConfigured = 0;

  JsonObject configObject = config.as<JsonObject>();
  for (JsonObject::iterator it = configObject.begin(); it != configObject.end(); ++it)
  {
    int plantId = atoi(it->key().c_str());
    JsonObject plantData = it->value().as<JsonObject>();

    // Check if any value is 0, skip creating the plant
    if (plantData["moistureMin"].as<int>() == 0 || plantData["moistureMax"].as<int>() == 0 || plantData["lowerTreshold"].as<int>() == 0 || plantData["upperTreshold"].as<int>() == 0 || plantData["wateringTime"].as<int>() == 0)
    {
      continue;
    }

    plants[plantId - 1] = Plant(
        plantId,
        SENSOR_PINS[plantId - 1],
        VALVE_PINS[plantId - 1],
        plantData["moistureMin"].as<int>(),
        plantData["moistureMax"].as<int>(),
        plantData["lowerTreshold"].as<int>(),
        plantData["upperTreshold"].as<int>(),
        plantData["wateringTime"].as<int>());
    plantsConfigured++;
  }

  Serial.print(plantsConfigured);
  Serial.println(" plants initialized from configuration.");
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
  else if (var == "CONFIGURED")
    return preferences.getBool("dev_configured", false) ? "Yes" : "No";
  else if (var == "MQTT_STATUS")
    return client.connected() ? "Connected to mqtt://" + preferences.getString("mqtt_server", "") : "Not connected";
  else if (var == "WIFI_PASSWORD")
    return preferences.getString("wifi_password", "");
  else if (var == "WIFI_CHANNEL")
    return preferences.getString("wifi_channel", "1");
  else if (var == "MQTT_SERVER")
    return preferences.getString("mqtt_server", "");
  else if (var == "MQTT_USER")
    return preferences.getString("mqtt_user", "");
  else if (var == "MQTT_PASSWORD")
    return preferences.getString("mqtt_password", "");
  else if (var == "AP_NAME")
    return preferences.getString("ap_name", AP_NAME);
  else if (var == "AP_PASSWORD")
    return preferences.getString("ap_password", AP_PASSWORD);
  else
    return String();
}

class CaptiveRequestHandler : public AsyncWebHandler
{
public:
  CaptiveRequestHandler()
  {}
  virtual ~CaptiveRequestHandler() {}
  bool canHandle(AsyncWebServerRequest *request)
  {
    //request->addInterestingHeader("ANY");
    return true;
  }
  void handleRequest(AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html","text/html", false); 
  }
};

String getWiFiNetworks()
{
  int numNetworks = WiFi.scanNetworks();
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
  Serial.print("Message on topic: ");
  Serial.println(topic);

  if (String(topic) == "esp/config")
  {
    deserializeJson(config, (char *)message);
    createPlantsFromConfig();
  }
}
void mqtt_reconnect()
{
  delay(200);
  client.disconnect();
  if (WiFi.status() != WL_CONNECTED)
    setup_wifi();
  String un = preferences.getString("mqtt_user", MQTT_USER) + "/" + String(random(0xffff), HEX);
  if (client.connect(un.c_str()))
  {
    Serial.println("✓ Connected to MQTT broker as " + un);
  }
  else
  {
    Serial.print("✗ Failed to connect to MQTT broker as ");
    Serial.print(un.c_str());
    Serial.print(" rc = ");
    Serial.print(client.state());
    Serial.println(" Retrying in 5 seconds...");
    delay(5000);
  }
}

void mqtt_config(String server, int port)
{
  client.setServer("192.168.0.69", 1883);
  client.setKeepAlive(240);
  client.setSocketTimeout(240);
  client.setBufferSize(500);
  client.subscribe("esp/#");
  client.setCallback(callback);
  mqtt_configured = true;
  Serial.println("✓ MQTT configuration successful");
}

void setupServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html", false, processor); 
            Serial.println("✓ Client connected"); });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/style.css", "text/css"); });
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String wifiNetworks = getWiFiNetworks();
    request->send(200, "application/json", wifiNetworks); });
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("ssid", true) && request->hasParam("channel", true) && request->hasParam("mqtt_server", true) && request->hasParam("mqtt_port", true) && request->hasParam("mqtt_user", true)) {
      AsyncWebParameter *ssid = request->getParam("ssid", true);
      AsyncWebParameter *password = request->getParam("password", true);
      AsyncWebParameter *channel = request->getParam("channel", true);
      AsyncWebParameter *mqtt_server = request->getParam("mqtt_server", true);
      AsyncWebParameter *mqtt_port = request->getParam("mqtt_port", true);
      AsyncWebParameter *mqtt_user = request->getParam("mqtt_user", true);
      AsyncWebParameter *mqtt_password = request->getParam("mqtt_password", true);
      AsyncWebParameter *ap_name = request->getParam("ap_name", true);
      AsyncWebParameter *ap_password = request->getParam("ap_password", true);

      Serial.println("ssid: " + ssid->value());
      Serial.println("password: " + password->value());
      Serial.println("channel: " + channel->value());
      Serial.println("mqtt_server: " + mqtt_server->value());
      Serial.println("mqtt_port: " + mqtt_port->value());
      Serial.println("mqtt_user: " + mqtt_user->value());
      Serial.println("mqtt_password: " + mqtt_password->value());
      Serial.println("ap_name: " + ap_name->value());
      Serial.println("ap_password: " + ap_password->value());

      preferences.putString("wifi_ssid", ssid->value());
      preferences.putString("wifi_password", password->value());
      preferences.putString("wifi_channel", channel->value());
      preferences.putString("mqtt_server", mqtt_server->value());
      preferences.putString("mqtt_port", mqtt_port->value());
      preferences.putString("mqtt_user", mqtt_user->value());
      preferences.putString("mqtt_password", mqtt_password->value());
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

  dnsServer.start(53, "*", WiFi.softAPIP());
  server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
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

void connection_setup(void *pvParameters)
{
  if (preferences.getBool("dev_configured", false))
  {
    setup_wifi();
    mqtt_config(preferences.getString("mqtt_server"), preferences.getString("mqtt_port").toInt());
  }
  else
  {
    Serial.println("✗ Connection not configured. WiFi and MQTT disconnected");
  }
  Serial.print("✓ All set. Sleeping core ");
  Serial.println(xPortGetCoreID());
  vTaskDelete(NULL);
}

void dnsServerTask(void *parameter)
{
  Serial.print("✓ DNS Server started at core ");
  Serial.println(xPortGetCoreID());
  for (;;)
  {
    dnsServer.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(20)); // Delay to avoid high CPU usage
  }
}

void setupPreferences()
{
  preferences.putString("wifi_ssid", "");
  preferences.putString("wifi_password", "");
  preferences.putString("wifi_channel", "1");
  preferences.putString("mqtt_server", "");
  preferences.putString("mqtt_port", "1883");
  preferences.putString("mqtt_user", "");
  preferences.putString("mqtt_password", "");
  preferences.putString("ap_name", AP_NAME);
  preferences.putString("ap_password", AP_PASSWORD);
  preferences.putBool("dev_configured", false);
  preferences.putBool("initialized", true);
  Serial.println("✓ Preferences initialized");
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
  pinMode(CALIBRATION_BUTTON, INPUT_PULLUP);
  digitalWrite(21, HIGH);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0);
  WiFi.mode(WIFI_AP_STA);
  digitalWrite(LED, HIGH);

  preferences.begin("device_config", false);
  if (!preferences.getBool("initialized", false))
    setupPreferences();
  setupServer();
  setup_ap();

  xTaskCreatePinnedToCore(
      connection_setup,  /* Task function. */
      "SetupConnection", /* name of task. */
      25384,             /* Stack size of task */
      NULL,              /* parameter of the task */
      2,                 /* priority of the task */
      NULL,              /* Task handle to keep track of created task */
      0);

  xTaskCreatePinnedToCore(
      dnsServerTask,   // Task function
      "dnsServerTask", // Task name
      10000,           // Stack size (adjust as needed)
      NULL,            // Task parameter
      1,               // Task priority
      NULL,            // Task handle
      0                // Core to pin the task to (0 or 1)
  );
}

unsigned long lastReadingMillis = 0;
unsigned long readingDelay = 5000;

void loop()
{
  if (preferences.getBool("dev_configured", false) && WiFi.isConnected() && !client.connected() && mqtt_configured)
  {
    mqtt_reconnect();
  }

  unsigned long currentMillis = millis();
  client.loop();

  if (currentMillis - lastReadingMillis >= readingDelay && false)
  {
    lastReadingMillis = currentMillis;

    bool anyPlantConfigured = false;

    for (Plant plant : plants)
    {
      if (plant.configured)
      {
        anyPlantConfigured = true;

        String topic = "esp/plant/" + String(plant.id) + "/moisture";
        if (client.publish(topic.c_str(), String(plant.readMoisturePercentage()).c_str()))
          Serial.println(String(plant.getMoisturePercentage()) + "% published");
        else
        {
          Serial.println("✗ Failed to publish");
          for (int i = 0; i < 3; i++)
          {
            if (client.connected())
              break;
            mqtt_reconnect();
            client.loop();
            delay(5000);
            if (client.publish(topic.c_str(), String(plant.readMoisturePercentage()).c_str()))
            {
              Serial.println(String(plant.getMoisturePercentage()) + "\% published");
              break;
            }
          }
        }
      }
    }
    // if (!anyPlantConfigured) {
    //   Serial.println("No plants configured");
    //   client.publish("esp/error", "Failed to read config!");
    //   sleep_for(120);
    // } else {
    //   sleep_for(TIME_TO_SLEEP);
    // }
  }
  if (Serial.available())
  {
    String command = Serial.readString();
    Serial.print("Command: ");
    Serial.println(command);
    if (command == "restart")
    {
      ESP.restart();
    }
    else if (command == "factory")
    {
      preferences.clear();
      delay(200);
      ESP.restart();
    }
  }
}
