/**
  Light Switch Controller to MQTT

  Uses MCP23017 I2C I/O buffers to detect light switch presses,
  and report them to MQTT.

  The report is to a topic of the form:

    stat/ABC123/BUTTON<1-96> 

  where the "ABC123" is an ID derived from the MAC address of the
  device and each button has a unique topic. 
  
  The message is a JSON payload of the form; 

    {"PORT":24, "SWITCH":1, "BUTTON":93, "ACTION":"SINGLE"}

  where ACTION can be one of;

    SINGLE, DOUBLE, TRIPLE, QUAD, PENTA, or HOLD
    
  Compile options:
    Arduino Uno or Arduino Mega 2560

  External dependencies. Install using the Arduino library manager:
      "Adafruit_MCP23017"
      "PubSubClient" by Nick O'Leary

  Bundled dependencies. No need to install separately:
      "Adafruit SH1106" by wonho-maker, forked from Adafruit SSD1306 library
      "LSC_Button" by ben.jones12@gmail.com, forked from mdButton library

  More information:
    www.superhouse.tv/lightswitch

  Bugs/Features:
   - See GitHub issues list.

  Written by Jonathan Oxer for www.superhouse.tv
    https://github.com/superhouse/

  Copyright 2019-2021 SuperHouse Automation Pty Ltd
*/

/*--------------------------- Version ------------------------------------*/
#define VERSION "3.0"

/*--------------------------- Configuration ------------------------------*/
// Should be no user configuration in this file, everything should be in;
#include "config.h"

/*--------------------------- Libraries ----------------------------------*/
#include <Wire.h>                     // For I2C
#include <Ethernet.h>                 // For networking
#include <PubSubClient.h>             // For MQTT
#include "Adafruit_MCP23017.h"        // For MCP23017 I/O buffers
#include "LSC_Button.h"               // For button click handling

/*--------------------------- Constants ----------------------------------*/
// Each MCP23017 has 16 inputs
#define MCP_PIN_COUNT     16

// List of I2C addresses we might be interested in 
//  - 0x20-0x27 are the possible 8x MCP23017 chips 
const byte MCP_I2C_ADDRESS[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25 };

/*--------------------------- Global Variables ---------------------------*/
// Each bit corresponds to an MCP found on the IC2 bus
uint8_t g_mcps_found = 0;

// Unique device id (last 3 HEX pairs of MAC address)
char g_device_id[7];

// If no mqtt_client_id set in config.h defaults to "LSC-<g_device_id>"
char g_mqtt_client_id[16];

// LWT published to <mqtt_lwt_base_topic>/<mqtt_client_id>
char g_mqtt_lwt_topic[32];

// Buffer used for MQTT payloads
char g_mqtt_message_buffer[48];

// When reconnecting to MQTT broker backoff in 5s increments
uint8_t g_mqtt_backoff = 0;

// Last time the watchdog was reset
uint32_t g_watchdog_last_reset_ms = 0;

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_MCP23017 mcp23017[sizeof(MCP_I2C_ADDRESS)];

// Button handlers
LSC_Button button[sizeof(MCP_I2C_ADDRESS)];

// Ethernet client
EthernetClient ethernet;

// MQTT client
PubSubClient mqtt_client(mqtt_broker, mqtt_port, ethernet);

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
  Serial.println(F("Light Switch Controller"));
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
    sprintf_P(g_mqtt_client_id, PSTR("LSC-%s"), g_device_id);  
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
    for (uint8_t i = 0; i < sizeof(MCP_I2C_ADDRESS); i++)
    {
      if (bitRead(g_mcps_found, i) == 0)
        continue;

      uint16_t io_value = mcp23017[i].readGPIOAB();
      button[i].process(i, io_value);
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

    // Publish LWT so anything listening knows we are alive
    if (ENABLE_MQTT_LWT)
    {
      byte lwt_payload[] = { '1' };
      mqtt_client.publish(g_mqtt_lwt_topic, lwt_payload, 1, mqtt_lwt_retain);
    }

    // Publish a message on the events topic to indicate startup
    if (ENABLE_MQTT_EVENTS)
    {
      sprintf_P(g_mqtt_message_buffer, PSTR("%s online"), g_mqtt_client_id);
      mqtt_client.publish(mqtt_events_topic, g_mqtt_message_buffer);
    }
  }
  else
  {
    // Backoff reconnects in 5s increments, until a max of 30s
    uint8_t backoffSecs = g_mqtt_backoff * 5;
    if (g_mqtt_backoff < 6) g_mqtt_backoff++;

    Serial.print(F("failed, retry in "));
    Serial.print(backoffSecs);
    Serial.println(F("s"));

    delay(backoffSecs * 1000);
  }

  return success;
}

char * getMqttButtonTopic(uint8_t button)
{
  static char topic[32];
  if (strlen(mqtt_base_topic) == 0)
  {
    sprintf_P(topic, PSTR("stat/%s/BUTTON%d"), g_device_id, button);
  }
  else
  {
    sprintf_P(topic, PSTR("%s/stat/%s/BUTTON%d"), mqtt_base_topic, g_device_id, button);
  }
  return topic;
}

char * getMqttButtonAction(uint8_t state)
{
  // Determine what action we need to publish
  static char action[7];
  switch (state)
  {
    case BUTTON_HOLD_STATE:
      sprintf_P(action, PSTR("HOLD"));
      break;
    case 1:
      sprintf_P(action, PSTR("SINGLE"));
      break;
    case 2:
      sprintf_P(action, PSTR("DOUBLE"));
      break;
    case 3:
      sprintf_P(action, PSTR("TRIPLE"));
      break;
    case 4:
      sprintf_P(action, PSTR("QUAD"));
      break;
    case 5:
      sprintf_P(action, PSTR("PENTA"));
      break;
    default:
      sprintf_P(action, PSTR("ERROR"));
      break;
  }
  return action;
}

/**
  Button handlers
*/
void buttonPressed(uint8_t id, uint8_t button, uint8_t state)
{
  // Determine the port, switch, and button numbers (1-based)
  uint8_t mcp = id;
  uint8_t raw_button = (MCP_PIN_COUNT * mcp) + button;
  uint8_t port = (raw_button / 4) + 1;
  uint8_t port_switch = (button % 4) + 1;
  uint8_t mqtt_button = raw_button + 1;

  if (DEBUG_BUTTONS)
  {
    Serial.print(F("Press detected. PORT:"));
    Serial.print(port);
    Serial.print(F(" SWITCH:"));
    Serial.print(port_switch);
    Serial.print(F(" BUTTON:"));
    Serial.print(mqtt_button);
    Serial.print(F(" STATE:"));
    Serial.print(state);
    Serial.print(F(" ACTION:"));
    Serial.println(getMqttButtonAction(state));
  }

  // Publish event to MQTT
  sprintf_P(g_mqtt_message_buffer, PSTR("{\"PORT\":%d, \"SWITCH\":%d, \"BUTTON\":%d, \"ACTION\":\"%s\"}"), port, port_switch, mqtt_button, getMqttButtonAction(state));
  mqtt_client.publish(getMqttButtonTopic(mqtt_button), g_mqtt_message_buffer);
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
  Serial.println(F("Scanning for MCP23017 I/O chips on the I2C bus..."));

  for (uint8_t i = 0; i < sizeof(MCP_I2C_ADDRESS); i++)
  {
    Serial.print(F(" - 0x"));
    Serial.print(MCP_I2C_ADDRESS[i], HEX);
    Serial.print(F("..."));

    // Set the bit indicating we found an MCP at this address
    Wire.beginTransmission(MCP_I2C_ADDRESS[i]);    
    bitWrite(g_mcps_found, i, Wire.endTransmission() == 0);

    // If a chip was found then initialise and configure the inputs
    if (bitRead(g_mcps_found, i))
    {  
      mcp23017[i].begin(i);
      for (uint8_t pin = 0; pin < MCP_PIN_COUNT; pin++)
      {
        mcp23017[i].pinMode(pin, INPUT);
        mcp23017[i].pullUp(pin, HIGH);
      }
  
      button[i].onButtonPressed(buttonPressed); 
      Serial.println(F("ready"));
    }
    else
    {
      Serial.println(F("missing"));
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
