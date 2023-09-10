#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "DHT.h"

#include "config.h"

// setup INPUT pins
#define PIN_DOOR_REED D0
#define PIN_DHT D1
#define PIN_PIR D2

// setup OUTPUT pins
#define PIN_OPENER_ACTION D5
#define PIN_OPENER_LIGHT D6
#define PIN_OPENER_LOCK D7


// setup DHT
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);


// setup MQTT availability info
const char* birth_message = "online";
const char* lwt_message = "offline";

// setup MQTT topic info
const String availability_topic = room + "/" + mqtt_client_id + "/" + "availability";
const String mqtt_command_topic = room + "/" + host + "/opener/cmd";
const String mqtt_state_topic = room + "/" + host + "/state";

const char* availability_topic_str = availability_topic.c_str();
const char* mqtt_command_topic_str = mqtt_command_topic.c_str();
const char* mqtt_state_topic_str = mqtt_state_topic.c_str();

// define the CMDs for opener
#define CMD_ACTION "action"
#define CMD_RESET_LOCK_LIGHT "reset_lock_light"

// setup state variables
String door_state = "closed";
String door_state_prev = "closed";
long door_state_time = 0;

String motion = "yes";
String motion_prev = "";
long motion_time = 0;

String opener_lock = "lock off";
String opener_light = "light off";

float humidity;
float temperature;


// setup other vars
long last_publish_time = 0;
long last_dht_read_time = 0;

// setup some constants
const int REED_DEBOUNCE_TIME = 3000;
const int MOTION_DEBOUNCE_TIME = 10000;
const int BUFFER_SIZE = 300;
const int REPORTING_DELAY = 150000;
const int DHT_READ_DELAY = 10000;


WiFiClient espClient;
PubSubClient client(espClient);


void setup() {
  Serial.begin(115200);
  while(!Serial) { }
  
  // setup pin modes
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_OPENER_ACTION, OUTPUT);
  digitalWrite(PIN_OPENER_ACTION, LOW);
  pinMode(PIN_OPENER_LIGHT, OUTPUT);
  digitalWrite(PIN_OPENER_LIGHT, LOW);
  pinMode(PIN_OPENER_LOCK, OUTPUT);
  digitalWrite(PIN_OPENER_LOCK, LOW);
  
  pinMode(PIN_DOOR_REED, INPUT_PULLDOWN_16);

  setup_wifi(); 

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // setup OTA
  ArduinoOTA.setHostname(host);

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
  ArduinoOTA.begin();

  // wait for DHT22 to stabilize
  delay(5000);
}


void setup_wifi() {

  delay(10);
  // Connect to WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.hostname(host);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("WiFi connected to: ");
  Serial.print(ssid);
  Serial.println("");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}


void callback(char* topic, byte* payload, unsigned int length) {
  /*Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  
  Serial.println();*/

  String topicToProcess = topic;

  payload[length] = '\0';
  String payloadToProcess = String((char *)payload);
 
  triggerDoorAction(topicToProcess, payloadToProcess);
}



void triggerDoorAction(String topic, String payload){
  Serial.print("topic: ");
  Serial.println(topic);

  Serial.print("payload: ");
  Serial.println(payload);
  
  if(payload == CMD_ACTION){
    digitalWrite(PIN_OPENER_ACTION, HIGH);
    delay(250);
    digitalWrite(PIN_OPENER_ACTION, LOW);
  }
  else if(payload == "light on" || payload == "light off"){
    digitalWrite(PIN_OPENER_LIGHT, HIGH);
    delay(250);
    digitalWrite(PIN_OPENER_LIGHT, LOW);
    opener_light = payload;
    publish_state();
  }
  else if(payload == "lock on" || payload == "lock off"){
    digitalWrite(PIN_OPENER_LOCK, HIGH);
    delay(1500);
    digitalWrite(PIN_OPENER_LOCK, LOW); 
    opener_lock = payload;
    publish_state();
  }
  else if(payload == CMD_RESET_LOCK_LIGHT){
    opener_light = "light off";
    opener_lock = "lock off";
    publish_state();
  }
}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(mqtt_client_id, mqtt_username, mqtt_password, availability_topic_str, 0, true, lwt_message)) {
      Serial.println("Connected!");

      // Publish the birth message on connect/reconnect
      publish_birth_message();

      // Subscribe to the action topics to listen for action messages
      Serial.print("Subscribing to ");
      Serial.print(mqtt_command_topic);
      Serial.println("...");
      client.subscribe(mqtt_command_topic_str);

      // Publish the current door status on connect/reconnect to ensure status is synced with whatever happened while disconnected
      //publish_door1_status();
    } 
    else {
      Serial.print("MQTT failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void publish_birth_message() {
  // Publish the birthMessage
  Serial.print("Publishing birth message \"");
  Serial.print(birth_message);
  Serial.print("\" to ");
  Serial.print(availability_topic);
  Serial.println("...");
  client.publish(availability_topic_str, birth_message, true);
}


void publish_state(){
  Serial.println("Publish state");
  Serial.println((String)door_state);
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();

  root["door_state"] = (String)door_state;
  root["rssi"] = (String)WiFi.RSSI();
  root["motion"] = (String)motion;
  root["temperature"] = (String)temperature;
  root["humidity"] = (String)humidity;
  root["opener_lock"] = (String)opener_lock;
  root["opener_light"] = (String)opener_light;
  
  char buffer[root.measureLength() + 1];
  root.printTo(buffer, sizeof(buffer));

  client.publish(mqtt_state_topic_str, buffer, true);
}


void checkDoorStatus(){
  unsigned int current_time = millis();
  // if within bounce time ignore
  if (current_time - door_state_time >= REED_DEBOUNCE_TIME) {
    // get status of sensor
    if(digitalRead(PIN_DOOR_REED)) door_state = "closed";
    else door_state = "open";

    // check if its different
    if (door_state != door_state_prev) {
      publish_state();
      door_state_prev = door_state;
      door_state_time = current_time;
    }
  }
}


void readPIR(){
  unsigned int current_time = millis();

  if(digitalRead(PIN_PIR) == HIGH){
    motion = "yes";
    motion_time = current_time;
  }

  if(current_time - motion_time > MOTION_DEBOUNCE_TIME){
    motion = "no";
  }
  
  if(motion != motion_prev){
    motion_prev = motion;
    publish_state();
  }
}


void readDHT(){
  float h = dht.readHumidity();
  float t = dht.readTemperature(true);

  // Check if any reads failed and exit early (to try again).
  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }
  humidity = h;
  temperature = t;
}


void loop() {
  ArduinoOTA.handle();
  
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  long now = millis();
  if(temperature == 0 || now - last_dht_read_time > DHT_READ_DELAY){
    readDHT();
    last_dht_read_time = now;
  }
  
  if ( (now - last_publish_time > REPORTING_DELAY) ) {
    publish_state();
    last_publish_time = now;
  }

  // check door
  checkDoorStatus();
  readPIR();
  
}
