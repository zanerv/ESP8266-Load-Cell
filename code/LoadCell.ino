// Required Libarys:
// async-mqtt-client:  https://codeload.github.com/marvinroger/async-mqtt-client/zip/master
// ESPAsyncTCP:        https://codeload.github.com/me-no-dev/ESPAsyncTCP/zip/master
// HX711:              https://codeload.github.com/bogde/HX711/zip/master

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <HX711.h>
#include <ArduinoOTA.h>
#include <stdlib.h>
#include "config.h"

#define FILTER_SIZE 15

HX711 scale(PIN_DOUT, PIN_CLK);

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiReconnectTimer;

bool firstRun = true;
unsigned char samples;
int sum, average, offset;
int min_mass = 15;
char oldResult[10];

void eeWriteInt(int pos, int val) {
    byte* p = (byte*) &val;
    EEPROM.write(pos, *p);
    EEPROM.write(pos + 1, *(p + 1));
    EEPROM.write(pos + 2, *(p + 2));
    EEPROM.write(pos + 3, *(p + 3));
    EEPROM.commit();
}

int eeGetInt(int pos) {
  int val;
  byte* p = (byte*) &val;
  *p        = EEPROM.read(pos);
  *(p + 1)  = EEPROM.read(pos + 1);
  *(p + 2)  = EEPROM.read(pos + 2);
  *(p + 3)  = EEPROM.read(pos + 3);
  return val;
}


void connectToWifi() {
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);

  WiFi.hostname(WIFI_CLIENT_ID);
  WiFi.mode(WIFI_STA);
  if (WIFI_STATIC_IP != 0) {
    WiFi.config(WIFI_CLIENT_IP, WIFI_GATEWAY_IP, WIFI_SUBNET_IP, WIFI_DNS_IP);
  }

  delay(100);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
  Serial.print("[WiFi] Connected, IP address: ");
  Serial.println(WiFi.localIP());
  wifiReconnectTimer.detach();
  connectToMqtt();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.println("[WiFi] Disconnected from Wi-Fi!");
  mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  wifiReconnectTimer.once(WIFI_RECONNECT_TIME, connectToWifi);
}

void connectToMqtt() {
  Serial.println("[MQTT] Connecting to MQTT...");
  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("[MQTT] Connected to MQTT!");

  mqttReconnectTimer.detach();

  Serial.print("Session present: ");
  Serial.println(sessionPresent);
  uint16_t packetIdSub = mqttClient.subscribe(MQTT_TOPIC_TARE, MQTT_TOPIC_TARE_QoS);
  Serial.print("Subscribing to ");
  Serial.println(MQTT_TOPIC_TARE);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("[MQTT] Disconnected from MQTT!");

  if (WiFi.isConnected()) {
    Serial.println("[MQTT] Trying to reconnect...");
    mqttReconnectTimer.once(MQTT_RECONNECT_TIME, connectToMqtt);
  }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  if (!strcmp(topic, MQTT_TOPIC_TARE)) {
    Serial.print("Zeroing: ");
    Serial.println(average / (float) 100);
    offset = average;
    if (SAVE_TARE != 0) {
      Serial.print("Saving to EEPROM...");
      eeWriteInt(EEPROM_ADDRESS, offset);
    }
  }
}

void setup() {
  pinMode(BUILTIN_LED, OUTPUT);  // initialize onboard LED as output
  Serial.begin(9600);
  EEPROM.begin(512);
  Serial.println("Startup!");

  wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  if (MQTT_USER != "") {
    mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  }

  if (SAVE_TARE == 1) {
    offset = eeGetInt(EEPROM_ADDRESS);

    Serial.print("Loading offset from EEPROM: ");
    Serial.println(offset / (float) 100);
  }

  scale.set_scale(CALIBRATION);

  if (OTA_PATCH == 1) {
    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else { // U_SPIFFS
        type = "filesystem";
      }

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        Serial.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        Serial.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        Serial.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        Serial.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        Serial.println("End Failed");
      }
    });

    ArduinoOTA.setHostname(WIFI_CLIENT_ID);
    ArduinoOTA.setPassword(OTA_PASS);


    ArduinoOTA.begin();
  }

  connectToWifi();
}

int compare(const void* a, const void* b)
{
     int int_a = * ( (int*) a );
     int int_b = * ( (int*) b );

     if ( int_a == int_b ) return 0;
     else if ( int_a < int_b ) return -1;
     else return 1;
}

void loop() {
  if (WiFi.isConnected() && (OTA_PATCH != 0)) {
    ArduinoOTA.handle();
  }

  // static filter variables
  static int filterSamples[FILTER_SIZE];
  static int filterHead = 0;

  // take sample and increment head
  filterSamples[filterHead] = scale.get_units() * 100;
  filterHead = (filterHead + 1) % FILTER_SIZE;

  // copy filter array
  int sortedSamples[FILTER_SIZE];
  for (int i = 0; i < FILTER_SIZE; i++)
  {
    sortedSamples[i] = filterSamples[i];
  }

  // sort the array copy
  qsort(sortedSamples, FILTER_SIZE, sizeof(int), compare);
  int median = sortedSamples[FILTER_SIZE / 2];

  if (firstRun) {
    firstRun = false;
    if (SAVE_TARE == 0) {
      median = offset;        
      return;
    }
  }

  char result[10];
  dtostrf(((median-offset) / (float) 100), 5, RESOLUTION, result);
  if (mqttClient.connected() && strcmp(result, oldResult)) {
  int int_result=atoi(result);
      if ( int_result <= min_mass ) {
        mqttClient.publish(MQTT_TOPIC_LOAD, MQTT_TOPIC_LOAD_QoS, true, "OFF");
      } else {
        mqttClient.publish(MQTT_TOPIC_LOAD, MQTT_TOPIC_LOAD_QoS, true, "ON");
      }
        mqttClient.publish(MQTT_TOPIC_MASS, MQTT_TOPIC_LOAD_QoS, true, result);
    // Serial.print("Pushing new result:");
    // Serial.println(result);
  }

  strncpy(oldResult, result, 10);

  delay(SAMPLE_PERIOD);
}
