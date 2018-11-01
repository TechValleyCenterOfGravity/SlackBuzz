//TVCOG Door Buzz SlackBot

#include <Arduino.h>

#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>

#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Hash.h>
#include <time.h>

//#define WIFI_SSID       "******"
//#define WIFI_PASSWORD   "******"
//#define SLACK_BOT_TOKEN     "your-bot-token-here" // Follow https://my.slack.com/services/new/bot to create a new bot
//#define OTA_PORT an_integer_above_1000
//#define OTA_HOSTNAME "esp-8266"
//#define OTA_PASSWORD "SuperSecret"
//#define MEMBER_CHANNEL "SomeChannel"
//#define BUZZ_PHRASE "Not_the_Phrase"
//The above lines are placed in secret.h
#include "secret.h"

//#define SLACK_SSL_FINGERPRINT "AC 95 5A 58 B8 4E 0B CD B3 97 D2 88 68 F5 CA C1 0A 81 E3 6E" // If Slack changes their SSL fingerprint, you would need to update this
#define SLACK_SSL_FINGERPRINT "C1 0D 53 49 D2 3E E5 2B A2 61 D5 9E 6F 99 0D 3D FD 8B B2 B3"

#define Debug 0
#define IfDBG() if(Debug)

#define WORD_SEPERATORS "., \"'()[]<>;:-+&?!\n\t"
#define Relay_Pin 0

#define permission_check_delay 3600 // update permission list once an hour 

typedef struct DP{
  time_t lastupdate;
  int count;
  char Door_Openers[100][10];
}DP;

DP Door_permissions={0,};

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;

long nextCmdId = 1;
bool connected = false;
uint32_t pullstart = 0; 

/**
  Sends a ping message to Slack. Call this function immediately after establishing
  the WebSocket connection, and then every 5 seconds to keep the connection alive.
*/
void sendPing() {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["type"] = "ping";
  root["id"] = nextCmdId++;
  String json;
  root.printTo(json);
  webSocket.sendTXT(json);
}

bool isMember(const char* MemberId){
  for(int i=0;i<Door_permissions.count;i++){
//    Serial.println(MemberId);
    if(strncmp(Door_permissions.Door_Openers[i],MemberId,9)==0)return 1;
  }
  return 0;
}

void update_Door_openers(){
  // Check if an hour has't elapsed and return 
  if((time(nullptr)-Door_permissions.lastupdate)<permission_check_delay)return;
  
  HTTPClient http;
  String payload;
  
  // configure traged server and url
  http.begin("https://slack.com/api/conversations.members?token="SLACK_BOT_TOKEN"&channel="MEMBER_CHANNEL,SLACK_SSL_FINGERPRINT); //HTTPS
  
  IfDBG() Serial.print("[HTTP] GET... members list\n");
  // start connection and send HTTP header
  int httpCode = http.GET();
  
  // httpCode will be negative on error
  if (httpCode > 0) {
    // file found at server
    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
    }
  } else {
    IfDBG() Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();

  DynamicJsonBuffer jsonBuffer;
  JsonObject& resp = jsonBuffer.parseObject(payload);
  
  IfDBG() Serial.printf("Freeheap:%d\r\n",ESP.getFreeHeap());
  
  if(resp.get<bool>("ok")==true){
    for (int i=0;i<resp["members"].size();i++){
      strncpy(Door_permissions.Door_Openers[i],resp["members"][i],10);
    }
    Door_permissions.count=resp["members"].size();
    Door_permissions.lastupdate=time(nullptr);
    IfDBG() Serial.printf("door_lastup:%d\r\n",Door_permissions.lastupdate);
    IfDBG() Serial.printf("time:%d\r\n",time(nullptr));
//    for (int i=0;i<resp["members"].size();i++){
//      Serial.printf("%s\r\n",Door_permissions.Door_Openers[i]);
//    }
  }
}

void Buzz_Door(void){
  IfDBG() Serial.println("BuzzBuzz\r\n");
  pullstart=millis();
  digitalWrite(Relay_Pin, LOW);
}

/**
  Animate a NeoPixel ring color change.
  Setting `zebra` to true skips every other led.
*/
void drawColor(uint32_t color, bool zebra) {
  IfDBG() Serial.println("hi");
}

/**
  Looks for color names in the incoming slack messages and
  animates the ring accordingly. You can include several
  colors in a single message, e.g. `red blue zebra black yellow rainbow`
*/
void processSlackMessage(char *payload) {
  char *nextWord = NULL;
  DynamicJsonBuffer jsonmsgBuffer;
  JsonObject& msgj = jsonmsgBuffer.parseObject(payload);
  if(strcmp(msgj["type"],"message")==0){
    String tchannel = msgj["channel"];
    const char* user=msgj.get<const char*>("user");

    if(tchannel.startsWith("D")){
      if(msgj["text"]==BUZZ_PHRASE){
        if(isMember(user))Buzz_Door();
      }
    }
  }
}

/**
  Called on each web socket event. Handles disconnection, and also
  incoming messages from slack.
*/
void webSocketEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED:
      IfDBG() Serial.printf("[WebSocket] Disconnected :-( \n");
      connected = false;
      break;

    case WStype_CONNECTED:
      IfDBG() Serial.printf("[WebSocket] Connected to: %s\n", payload);
      sendPing();
      break;

    case WStype_TEXT:
      IfDBG() Serial.printf("[WebSocket] Message: %s\r\n", payload);
      processSlackMessage((char*)payload);
      break;
  }
}

/**
  Establishes a bot connection to Slack:
  1. Performs a REST call to get the WebSocket URL
  2. Conencts the WebSocket
  Returns true if the connection was established successfully.
*/
bool connectToSlack() {
  // Step 1: Find WebSocket address via RTM API (https://api.slack.com/methods/rtm.connect)
  HTTPClient http;
  http.begin("https://slack.com/api/rtm.connect?token=" SLACK_BOT_TOKEN, SLACK_SSL_FINGERPRINT);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    IfDBG() Serial.printf("HTTP GET failed with code %d\n", httpCode);
    return false;
  }

  WiFiClient *client = http.getStreamPtr();
  client->find("wss:\\/\\/");
  String host = client->readStringUntil('\\');
  String path = client->readStringUntil('"');
  path.replace("\\/", "/");

  // Step 2: Open WebSocket connection and register event handler
  IfDBG() Serial.println("WebSocket Host=" + host + " Path=" + path);
  webSocket.beginSSL(host, 443, path, "", "");
  webSocket.onEvent(webSocketEvent);
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  pinMode(Relay_Pin, OUTPUT);
  digitalWrite(Relay_Pin, HIGH);

  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  while (WiFiMulti.run() != WL_CONNECTED) {
    delay(100);
  }

  ArduinoOTA.onStart([]() {
    IfDBG() Serial.println("Start");
    digitalWrite(Relay_Pin, HIGH);
  });
  
  ArduinoOTA.onEnd([]() {
    IfDBG() Serial.println("\nEnd");
    digitalWrite(Relay_Pin, HIGH);
  });
  
  ArduinoOTA.begin();

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}



unsigned long lastPing = 0;

/**
  Sends a ping every 5 seconds, and handles reconnections
*/
void loop() {
 
  ArduinoOTA.handle();
  webSocket.loop();

  if(digitalRead(Relay_Pin)==LOW){
    if((millis()-pullstart)>5000)digitalWrite(Relay_Pin, HIGH);
  }

  if (connected) {
    // Send ping every 5 seconds, to keep the connection alive
    if (millis() - lastPing > 5000) {
      sendPing();
      lastPing = millis();
    }
    //check for and update Door opening permissions once an hour.
    update_Door_openers();
  } else {
    // Try to connect / reconnect to slack
    connected = connectToSlack();
    if (!connected) {
      delay(500);
    }
  }
}
