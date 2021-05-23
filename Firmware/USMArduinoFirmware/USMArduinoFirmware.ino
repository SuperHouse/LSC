/**
  Universal State Monitor
  
  Monitor buttons, switches, or contacts and report associated events.

  Uses MCP23017 I2C I/O buffers to detect digital signals being pulled to 
  GND and publishes event reports to an MQTT broker.

  The TYPE of each individual input can be configured by publishing
  an MQTT message to a topic of the form;

    [BASETOPIC/]conf/<DEVICEID>/<INDEX>
    
  where;

    BASETOPIC:  Optional base topic prepended to device topics
    DEVICEID:   ID derived from the MAC address of the device
    INDEX:      Index of the input to configure (1-96)
    
  The message should be a JSON payload of the form;
  
    {"TYPE":"CONTACT", "INVT":1}

  where;

    TYPE:       Optional, one of BUTTON, CONTACT, SWITCH or TOGGLE.
    INVT:       Optional, set to 1 if events should be inverted
    
  A retained message will ensure the USM auto-configures on startup.
  
  The event report is to a topic of the form;

    [BASETOPIC/]stat/<DEVICEID>/<INDEX>

  where;
  
    BASETOPIC:  Optional base topic prepended to device topics
    DEVICEID:   ID derived from the MAC address of the device
    INDEX:      Index of the input causing the event (1-96)

  The message is a JSON payload of the form; 

    {"PORT":24, "CHANNEL":2, "INDEX":94, "TYPE":"BUTTON", "EVENT":"SINGLE"}

  where EVENT can be one of (depending on type);

    BUTTON:     SINGLE, DOUBLE, TRIPLE, QUAD, PENTA, or HOLD
    CONTACT:    OPEN or CLOSED
    SWTICH:     ON or OFF
    TOGGLE:     TOGGLE

  Compile options:
    Arduino Uno or Arduino Mega 2560

  External dependencies. Install using the Arduino library manager:
      "Adafruit_MCP23017"
      "ArduinoJSON"
      "PubSubClient" by Nick O'Leary

  Bundled dependencies. No need to install separately:
      "USM_Input" by ben.jones12@gmail.com, forked from mdButton library

  Based on the Light Switch Controller hardware found here:
    www.superhouse.tv/lightswitch

  Bugs/Features:
   - See GitHub issues list.

  Written by Jonathan Oxer for www.superhouse.tv
    https://github.com/superhouse/

  Copyright 2019-2021 SuperHouse Automation Pty Ltd
*/

/*--------------------------- Version ------------------------------------*/
#define VERSION "4.0"

/*--------------------------- Configuration ------------------------------*/
// Should be no user configuration in this file, everything should be in;
#include "config.h"

/*--------------------------- Libraries ----------------------------------*/
#include <Wire.h>                     // For I2C
#include <Ethernet.h>                 // For networking
#include <PubSubClient.h>             // For MQTT
#include <ArduinoJson.h>              // For MQTT message parsing
#include "Adafruit_MCP23017.h"        // For MCP23017 I/O buffers
#include "USM_Input.h"                // For input handling (embedded)

/*--------------------------- Constants ----------------------------------*/
// Each MCP23017 has 16 inputs
#define MCP_PIN_COUNT     16

// Max number of seconds to backoff when trying to connect to MQTT
#define MAX_BACKOFF_SECS  5

// List of I2C addresses we might be interested in 
//  - 0x20-0x27 are the possible 8x MCP23017 chips
byte I2C_ADDRESS[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27 };

/*--------------------------- Global Variables ---------------------------*/
// Each bit corresponds to an MCP found on the IC2 bus
uint8_t g_mcps_found = 0;

// Unique device id (last 3 HEX pairs of MAC address)
char g_device_id[7];

// If no mqtt_client_id set in config.h defaults to "USM-<g_device_id>"
char g_mqtt_client_id[16];

// LWT published to <mqtt_lwt_base_topic>/<mqtt_client_id>
char g_mqtt_lwt_topic[32];

// When reconnecting to MQTT broker backoff in 1s increments
uint8_t g_mqtt_backoff = 0;

// Last time the watchdog was reset
uint32_t g_watchdog_last_reset_ms = 0L;

/*--------------------------- Function Signatures ------------------------*/
void mqttCallback(char * topic, byte * payload, int length);

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_MCP23017 mcp23017[MCP_MAX_COUNT];

// Input handlers
USM_Input usmInput[MCP_MAX_COUNT];

// Ethernet client
EthernetClient ethernet;

// MQTT client
PubSubClient mqtt_client(mqtt_broker, mqtt_port, mqttCallback, ethernet);

/*--------------------------- Program ------------------------------------*/
/**
  Setup
*/
void setup()
{
  // Start the I2C bus
  Wire.begin();

  // Startup logging to serial
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println();
  Serial.println(F("======================="));
  Serial.println(F("     SuperHouse.TV"));
  Serial.println(F("Universal State Monitor"));
  Serial.print  (F("         v"));
  Serial.println(VERSION);
  Serial.println(F("======================="));

  // Set up watchdog
  initialiseWatchdog();

  // Scan the I2C bus for any MCP23017s and initialise them
  scanI2CBus();

  // Determine MAC address
  byte mac[6];
  if (ENABLE_MAC_ADDRESS_ROM)
  {
    Serial.print(F("Getting MAC address from ROM: "));
    mac[0] = readRegister(0xFA);
    mac[1] = readRegister(0xFB);
    mac[2] = readRegister(0xFC);
    mac[3] = readRegister(0xFD);
    mac[4] = readRegister(0xFE);
    mac[5] = readRegister(0xFF);
  }
  else
  {
    Serial.print(F("Using static MAC address: "));
    memcpy(mac, static_mac, sizeof(mac));
  }
  char mac_address[18];
  sprintf_P(mac_address, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(mac_address);

  // Set up Ethernet
  if (ENABLE_DHCP)
  {
    Serial.print(F("Getting IP address via DHCP: "));
    Ethernet.begin(mac);
  }
  else
  {
    Serial.print(F("Using static IP address: "));
    Ethernet.begin(mac, static_ip, static_dns);
  }
  Serial.println(Ethernet.localIP());

  // Generate device id
  sprintf_P(g_device_id, PSTR("%02X%02X%02X"), mac[3], mac[4], mac[5]);
  Serial.print(F("Device id: "));
  Serial.println(g_device_id);

  // Generate MQTT client id, unless one is explicitly defined
  if (strlen(mqtt_client_id) == 0)
  {
    sprintf_P(g_mqtt_client_id, PSTR("USM-%s"), g_device_id);  
  }
  else
  {
    memcpy(g_mqtt_client_id, mqtt_client_id, sizeof(g_mqtt_client_id));
  }
  Serial.print(F("MQTT client id: "));
  Serial.println(g_mqtt_client_id);

  // Generate MQTT LWT topic (if required)
  if (ENABLE_MQTT_LWT)
  {
    sprintf_P(g_mqtt_lwt_topic, PSTR("%s/%s"), mqtt_lwt_base_topic, g_mqtt_client_id);  
    Serial.print(F("MQTT LWT topic: "));
    Serial.println(g_mqtt_lwt_topic);
  }
}

/**
  Main processing loop
*/
void loop()
{
  // Check our DHCP lease is still ok
  Ethernet.maintain();

  // Process anything on MQTT and reconnect if necessary
  if (mqtt_client.loop() || mqttConnect())
  {
    // Pat the watchdog since we are connected to MQTT
    patWatchdog();

    // Iterate through each of the MCP23017 input buffers
    uint32_t port_new = 0L;
    for (uint8_t i = 0; i < MCP_MAX_COUNT; i++)
    {
      if (bitRead(g_mcps_found, i) == 0)
        continue;

      uint16_t io_value = mcp23017[i].readGPIOAB();
      usmInput[i].process(i, io_value);
    }
  }
}

/**
  MQTT
*/
boolean mqttConnect()
{
  Serial.print(F("Connecting to MQTT broker..."));

  // Attempt to connect, with a LWT if configured
  boolean success;
  if (ENABLE_MQTT_LWT)
  {
    success = mqtt_client.connect(g_mqtt_client_id, mqtt_username, mqtt_password, g_mqtt_lwt_topic, mqtt_lwt_qos, mqtt_lwt_retain, "0");
  }
  else
  {
    success = mqtt_client.connect(g_mqtt_client_id, mqtt_username, mqtt_password);
  }

  if (success)
  {
    Serial.println(F("success"));
    g_mqtt_backoff = 0;

    // subscribe to our config topic
    char topic[32];
    mqtt_client.subscribe(getConfigTopic(topic));
    
    // Publish LWT so anything listening knows we are alive
    if (ENABLE_MQTT_LWT)
    {
      byte lwt_payload[] = { '1' };
      mqtt_client.publish(g_mqtt_lwt_topic, lwt_payload, 1, mqtt_lwt_retain);
    }
  }
  else
  {
    // Backoff reconnects in 1s increments, until a max of 10s
    if (g_mqtt_backoff < MAX_BACKOFF_SECS) g_mqtt_backoff++;

    Serial.print(F("failed, retry in "));
    Serial.print(g_mqtt_backoff);
    Serial.println(F("s"));

    delay(g_mqtt_backoff * 1000);
  }

  return success;
}

void mqttCallback(char * topic, byte * payload, int length) 
{
  // We only subscribe to the conf topic for this device at;
  //    [<BASETOPIC/]conf/<DEVICEID>/<INDEX>
  // where the message should be of the form;
  //    {"TYPE":"<TYPE>", "INVT":1|0}

  // Tokenise the topic
  char * topicIndex;
  topicIndex = strtok(topic, "/");

  // Junk the first few tokens
  topicIndex = strtok(NULL, "/");
  topicIndex = strtok(NULL, "/");

  if (strlen(mqtt_base_topic) > 0)
  {
    topicIndex = strtok(NULL, "/");
  }

  // Parse the index and work out which MCP/input
  int index = atoi(topicIndex);
  int mcp = (index - 1) / 16;
  int input = (index - 1) % 16;

  // Parse the JSON payload
  StaticJsonDocument<32> json;
  deserializeJson(json, payload, length);

  if (json.containsKey("TYPE"))
  {
    if (strcmp(json["TYPE"], "BUTTON") == 0)
    {
      usmInput[mcp].setType(input, BUTTON);
    }
    else if (strcmp(json["TYPE"], "CONTACT") == 0)
    {
      usmInput[mcp].setType(input, CONTACT);
    }
    else if (strcmp(json["TYPE"], "SWITCH") == 0)
    {
      usmInput[mcp].setType(input, SWITCH);
    }
    else if (strcmp(json["TYPE"], "TOGGLE") == 0)
    {
      usmInput[mcp].setType(input, TOGGLE);
    }
  }

  if (json.containsKey("INVT"))
  {
    usmInput[mcp].setInvert(input, (uint8_t)json["INVT"]);
  }

  if (ENABLE_DEBUG)
  {
    Serial.print(F("[CONF]"));
    Serial.print(F(" INDX:"));
    Serial.print(index);
    Serial.print(F(" TYPE:"));
    Serial.print(getInputType(usmInput[mcp].getType(input)));
    Serial.print(F(" INVT:"));
    Serial.println(usmInput[mcp].getInvert(input));
  }
}

char * getInputType(uint8_t type)
{
  // Determine what type of input we have
  static char inputType[8];
  switch (type)
  {
    case BUTTON:
      sprintf_P(inputType, PSTR("BUTTON"));
      break;
    case CONTACT:
      sprintf_P(inputType, PSTR("CONTACT"));
      break;
    case SWITCH:
      sprintf_P(inputType, PSTR("SWITCH"));
      break;
    case TOGGLE:
      sprintf_P(inputType, PSTR("TOGGLE"));
      break;
    default:
      sprintf_P(inputType, PSTR("ERROR"));
      break;
  }
  return inputType;
}

char * getEventType(uint8_t type, uint8_t state)
{
  // Determine what event we need to publish
  static char eventType[7];
  switch (type)
  {
    case BUTTON:
      switch (state)
      {
        case USM_HOLD_STATE:
          sprintf_P(eventType, PSTR("HOLD"));
          break;
        case 1:
          sprintf_P(eventType, PSTR("SINGLE"));
          break;
        case 2:
          sprintf_P(eventType, PSTR("DOUBLE"));
          break;
        case 3:
          sprintf_P(eventType, PSTR("TRIPLE"));
          break;
        case 4:
          sprintf_P(eventType, PSTR("QUAD"));
          break;
        case 5:
          sprintf_P(eventType, PSTR("PENTA"));
          break;
        default:
          sprintf_P(eventType, PSTR("ERROR"));
          break;
      }
      break;
    case CONTACT:
      switch (state)
      {
        case USM_LOW:
          sprintf_P(eventType, PSTR("CLOSED"));
          break;
        case USM_HIGH:
          sprintf_P(eventType, PSTR("OPEN"));
          break;
        default:
          sprintf_P(eventType, PSTR("ERROR"));
          break;
      }
      break;
    case SWITCH:
      switch (state)
      {
        case USM_LOW:
          sprintf_P(eventType, PSTR("ON"));
          break;
        case USM_HIGH:
          sprintf_P(eventType, PSTR("OFF"));
          break;
        default:
          sprintf_P(eventType, PSTR("ERROR"));
          break;
      }
      break;
    case TOGGLE:
      sprintf_P(eventType, PSTR("TOGGLE"));
      break;
    default:
      sprintf_P(eventType, PSTR("ERROR"));
      break;
  }
  return eventType;
}

char * getConfigTopic(char topic[])
{
  if (strlen(mqtt_base_topic) == 0)
  {
    sprintf_P(topic, PSTR("conf/%s/+"), g_device_id);
  }
  else
  {
    sprintf_P(topic, PSTR("%s/conf/%s/+"), mqtt_base_topic, g_device_id);
  }
  return topic;
}

char * getEventTopic(char topic[], uint8_t index)
{
  if (strlen(mqtt_base_topic) == 0)
  {
    sprintf_P(topic, PSTR("stat/%s/%d"), g_device_id, index);
  }
  else
  {
    sprintf_P(topic, PSTR("%s/stat/%s/%d"), mqtt_base_topic, g_device_id, index);
  }
  return topic;
}

/**
  Button handlers
*/
void usmEvent(uint8_t id, uint8_t input, uint8_t type, uint8_t state)
{
  // Determine the 0-based index of the input
  uint8_t mcp = id;
  uint8_t raw_index = (MCP_PIN_COUNT * mcp) + input;

  // We report the port, channel and index as 1-based numbers
  uint8_t index = raw_index + 1;

  // Create JSON payload
  StaticJsonDocument<64> json;
  json["PORT"] = (raw_index / 4) + 1;
  json["CHAN"] = (input % 4) + 1;
  json["INDX"] = index;
  json["TYPE"] = getInputType(type);
  json["EVNT"] = getEventType(type, state);
  
  char message[80];
  serializeJson(json, message);

  if (ENABLE_DEBUG)
  {
    Serial.print(F("[EVNT] "));
    Serial.println(message);
  }

  // Publish event to MQTT
  char topic[32];
  mqtt_client.publish(getEventTopic(topic, index), message);
}

/**
  Watchdog
*/
void initialiseWatchdog()
{
  if (ENABLE_WATCHDOG)
  {
    Serial.print(F("Watchdog enabled on pin "));
    Serial.println(WATCHDOG_PIN);

    pinMode(WATCHDOG_PIN, OUTPUT);
    digitalWrite(WATCHDOG_PIN, LOW);
  }
  else
  {
    Serial.println(F("Watchdog NOT enabled"));
  }
}

void patWatchdog()
{
  if (ENABLE_WATCHDOG)
  {
    if ((millis() - g_watchdog_last_reset_ms) > WATCHDOG_RESET_MS)
    {
      digitalWrite(WATCHDOG_PIN, HIGH);
      delay(WATCHDOG_PULSE_MS);
      digitalWrite(WATCHDOG_PIN, LOW);

      g_watchdog_last_reset_ms = millis();
    }
  }
}

/**
  I2C
*/
void scanI2CBus()
{
  Serial.println(F("Scanning for devices on the I2C bus..."));

  for (uint8_t i = 0; i < sizeof(I2C_ADDRESS); i++)
  {
    Serial.print(F(" - 0x"));
    Serial.print(I2C_ADDRESS[i], HEX);
    Serial.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(I2C_ADDRESS[i]);
    if (Wire.endTransmission() == 0)
    {
      if (i < MCP_MAX_COUNT) 
      {
        bitWrite(g_mcps_found, i, 1);
        
        // If an MCP23017 was found then initialise and configure the inputs
        mcp23017[i].begin(i);
        for (uint8_t pin = 0; pin < MCP_PIN_COUNT; pin++)
        {
          mcp23017[i].pinMode(pin, INPUT);
          mcp23017[i].pullUp(pin, HIGH);
        }

        // Listen for input events
        usmInput[i].onEvent(usmEvent); 
        
        Serial.println(F("MCP23017"));
      }
      else
      {
        Serial.println(F("ignored"));
      }
    }
    else
    {
      Serial.println(F("empty"));
    }
  }
}

/**
  Required to read the MAC address ROM via I2C
*/
byte readRegister(byte r)
{
  // Register to read
  Wire.beginTransmission(MAC_I2C_ADDRESS);
  Wire.write(r);
  Wire.endTransmission();

  // Read a byte
  Wire.requestFrom(MAC_I2C_ADDRESS, 1);

  // Wait
  while (!Wire.available()) {}

  // Get the value off the bus
  unsigned char v;
  v = Wire.read();
  return v;
}
