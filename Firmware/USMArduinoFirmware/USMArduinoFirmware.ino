/**
  Universal State Monitor
  
  Monitor buttons, switches, or contacts and report associated events.

  Uses MCP23017 I2C I/O buffers to detect digital signals being pulled to 
  GND and publishes event reports to an MQTT broker.

  The configuration of each individual input can be set by publishing
  an MQTT message to one of these topics;

    [BASETOPIC/]conf/<CLIENTID>/<INDEX>/type
    [BASETOPIC/]conf/<CLIENTID>/<INDEX>/invt
    
  where;

    BASETOPIC   Optional base topic prepended to device topics
    CLIENTID    Client id of device, defaults to USM-<MAC ADDRESS>
    INDEX       Index of the input to configure (1-96)
    
  The message should be;

    /type       One of BUTTON, CONTACT, ROTARY, SWITCH or TOGGLE
    /invt       Either 0 or 1 (to invert event)
    
  A null or empty message will reset the input to;

    /type       SWITCH
    /invt       0 (non-inverted)
    
  A retained message will ensure the USM auto-configures on startup.
  
  The event report is to a topic of the form;

    [BASETOPIC/]stat/<CLIENTID>/<INDEX>

  where;
  
    BASETOPIC   Optional base topic prepended to device topics
    CLIENTID    Client id of device, defaults to USM-<MAC ADDRESS>
    INDEX       Index of the input causing the event (1-96)

  The message is a JSON payload of the form; 

    {"PORT":24, "CHANNEL":2, "INDEX":94, "TYPE":"SWITCH", "EVENT":"ON"}

  where EVENT can be one of (depending on type);

    BUTTON      SINGLE, DOUBLE, TRIPLE, QUAD, PENTA, or HOLD
    CONTACT     OPEN or CLOSED
    ROTARY      UP or DOWN
    SWITCH      ON or OFF
    TOGGLE      TOGGLE

  Compile options:
    Arduino Uno or Arduino Mega 2560
    If using an Uno (or clone) suggest setting MCP_MAX_COUNT = 6 as any
    more than that starts to cause "Low memory" warnings in the compiler

  External dependencies. Install using the Arduino library manager:
      "Adafruit_MCP23017"
      "PubSubClient" by Nick O'Leary
      "SSD1306Ascii"

  Bundled dependencies. No need to install separately:
      "USM_Input" by ben.jones12@gmail.com, based on mdButton library
      "USM_Oled" by moinmoin-sh

  Based on the Light Switch Controller hardware found here:
    www.superhouse.tv/lightswitch

  Bugs/Features:
   - See GitHub issues list.

  Written by Ben & Moin on behalf of Jon Oxer for www.superhouse.tv
    https://github.com/sumnerboy12/
    https://github.com/moinmoin-sh/
    https://github.com/superhouse/

  Copyright 2019-2021 SuperHouse Automation Pty Ltd
*/

/*--------------------------- Version ------------------------------------*/
#define VERSION "4.7"

/*--------------------------- Configuration ------------------------------*/
// Should be no user configuration in this file, everything should be in;
#include "config.h"

/*--------------------------- Libraries ----------------------------------*/
#include <Wire.h>                     // For I2C
#include <Ethernet.h>                 // For networking
#include <PubSubClient.h>             // For MQTT
#include "Adafruit_MCP23017.h"        // For MCP23017 I/O buffers
#include "USM_Input.h"                // For input handling (embedded)
#include "USM_Oled.h"                 // For OLED runtime displays

/*--------------------------- Constants ----------------------------------*/
// Each MCP23017 has 16 inputs
#define MCP_PIN_COUNT       16

// List of I2C addresses we might be interested in 
//  - 0x20-0x27 are the possible 8x MCP23017 chips
byte MCP_I2C_ADDRESS[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27 };

/*--------------------------- Global Variables ---------------------------*/
// Each bit corresponds to an MCP found on the IC2 bus
uint8_t g_mcps_found = 0;

// Was an OLED found on the I2C bus
byte g_oled_found = 0;

// If no mqtt_client_id set in config.h defaults to "USM-<MAC ADDRESS>"
char g_mqtt_client_id[16];

// LWT published to <mqtt_lwt_base_topic>/<mqtt_client_id>
char g_mqtt_lwt_topic[32];

// When reconnecting to MQTT broker backoff in increasing intervals
uint8_t g_mqtt_backoff = 0;

// Last time we attempted to reconnect to MQTT
uint32_t g_mqtt_last_reconnect_ms = 0L;

// Last time the watchdog was reset
uint32_t g_watchdog_last_reset_ms = 0L;

// store MCP portAB values -> needed for port animation
uint16_t g_mcp_io_values[MCP_MAX_COUNT];

// for timeout (clear) of bottom line input event display
uint32_t g_last_event_display = 0L;

// for timeout (dim) of OLED
uint32_t g_last_oled_trigger = 0L;

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

// OLED
SSD1306AsciiWire oled;

/*--------------------------- Program ------------------------------------*/
/**
  Setup
*/
void setup()
{
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

  // Start the I2C bus
  Wire.begin();

  // Scan the I2C bus for any MCP23017s and initialise them
  scanI2CBus();

  // set I2C clock to 400 kHz for faster scan rate  
  // needs to be called after scanI2CBus() to take effect
  Wire.setClock(I2C_CLOCK_SPEED);

  // Display the firmware version and initialise the port display
  if (g_oled_found)
  {
    USM_Oled_draw_logo(VERSION);
    USM_Oled_draw_ports(g_mcps_found);
  }

  // Determine MAC address
  byte mac[6];
  if (ENABLE_MAC_ADDRESS_ROM)
  {
    Serial.print(F("Getting MAC address from ROM: "));
    mac[0] = readRegister(MAC_I2C_ADDRESS, 0xFA);
    mac[1] = readRegister(MAC_I2C_ADDRESS, 0xFB);
    mac[2] = readRegister(MAC_I2C_ADDRESS, 0xFC);
    mac[3] = readRegister(MAC_I2C_ADDRESS, 0xFD);
    mac[4] = readRegister(MAC_I2C_ADDRESS, 0xFE);
    mac[5] = readRegister(MAC_I2C_ADDRESS, 0xFF);
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

  // Display IP and MAC addresses
  if (g_oled_found)
  {
    oled.setCursor(25, 2);
    oled.print(Ethernet.localIP()); 
    oled.setCursor(25, 3);
    oled.print(mac_address); 
  }

  // Generate MQTT client id, unless one is explicitly defined
  if (mqtt_client_id == NULL)
  {
    sprintf_P(g_mqtt_client_id, PSTR("USM-%02X%02X%02X"), mac[3], mac[4], mac[5]);  
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

  // Check our MQTT broker connection is still ok
  mqttMaintain();

  // Iterate through each of the MCP23017 input buffers
  uint32_t port_changed = 0L;
  for (uint8_t mcp = 0; mcp < MCP_MAX_COUNT; mcp++)
  {
    if (bitRead(g_mcps_found, mcp) == 0)
      continue;

    // Read the values for all 16 inputs on this MCP
    uint16_t io_value = mcp23017[mcp].readGPIOAB();

    // Compare with last stored value
    uint16_t tmp = io_value ^ g_mcp_io_values[mcp];
    for (uint8_t j = 0; j < 4; j++)
    {
      if ((tmp >> (j * 4)) & 0x000F)
      {
        bitSet(port_changed, (mcp * 4) + j);
      }
    }

    // Need to store so we can detect changes for port animation
    g_mcp_io_values[mcp] = io_value;

    // Check for any input events
    usmInput[mcp].process(mcp, io_value);
  }
  
  // Update OLED port animation 
  updateOled(port_changed);

  // Pat the watchdog once our processing loop completes
  patWatchdog();
}

/**
  MQTT
*/
void mqttMaintain()
{
  if (mqtt_client.loop())
  {
    // Currently connected so ensure we are ready to reconnect if it drops
    g_mqtt_backoff = 0;
    g_mqtt_last_reconnect_ms = millis();    
  }
  else
  {
    // Calculate the backoff interval and check if we need to try again
    uint32_t mqtt_backoff_ms = (uint32_t)g_mqtt_backoff * MQTT_BACKOFF_SECS * 1000;
    if ((millis() - g_mqtt_last_reconnect_ms) > mqtt_backoff_ms)
    {
      Serial.print(F("Connecting to MQTT broker..."));

      if (mqttConnect()) 
      {
        Serial.println(F("success"));
      }
      else
      {
        // Reconnection failed, so backoff
        if (g_mqtt_backoff < MQTT_MAX_BACKOFF_COUNT) { g_mqtt_backoff++; }
        g_mqtt_last_reconnect_ms = millis();

        Serial.print(F("failed, retry in "));
        Serial.print(g_mqtt_backoff * MQTT_BACKOFF_SECS);
        Serial.println(F("s"));
      }
    }
  }
}

boolean mqttConnect()
{
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
    // Subscribe to our config topics
    char topic[42];
    mqtt_client.subscribe(getConfigTopic(topic));
    
    // Publish LWT so anything listening knows we are alive
    if (ENABLE_MQTT_LWT)
    {
      byte lwt_payload[] = { '1' };
      mqtt_client.publish(g_mqtt_lwt_topic, lwt_payload, 1, mqtt_lwt_retain);
    }
  }

  return success;
}

void mqttCallback(char * topic, byte * payload, int length) 
{
  // We support config updates published to the following topics;
  //    [<BASETOPIC/]conf/<CLIENTID>/<INDEX>/type
  //    [<BASETOPIC/]conf/<CLIENTID>/<INDEX>/invt
  // where the message should be;
  //    /type     One of BUTTON, CONTACT, ROTARY, SWITCH or TOGGLE
  //    /invt     Either 0 or 1
  // and a null or empty message will default to;
  //    /type     SWITCH
  //    /invt     0

  // Tokenise the topic
  char * topicIndex;
  topicIndex = strtok(topic, "/");

  // Junk the first few tokens
  topicIndex = strtok(NULL, "/");
  topicIndex = strtok(NULL, "/");

  if (mqtt_base_topic != NULL)
  {
    topicIndex = strtok(NULL, "/");
  }

  // Parse the index and work out which MCP/input
  int index = atoi(topicIndex);
  int mcp = (index - 1) / 16;
  int input = (index - 1) % 16;

  if (ENABLE_DEBUG)
  {
    Serial.print(F("[CONF]"));
    Serial.print(F(" INDX:"));
    Serial.print(index);
  }

  // Check the index is valid
  if ((index < 1) || (index > (MCP_MAX_COUNT * 16)))
  {
    if (ENABLE_DEBUG) { Serial.println(F(" INVALID INDEX")); }
    return;
  }

  // Parse the config type
  topicIndex = strtok(NULL, "/");
  char * configType = topicIndex;

  // Parse the message and update the config
  if (strncmp(configType, "type", 4) == 0)
  {
    if (ENABLE_DEBUG) { Serial.print(F(" TYPE:")); }

    if (length == 0 || strncmp((char*)payload, "SWITCH", length) == 0)
    {
      usmInput[mcp].setType(input, SWITCH);
      if (ENABLE_DEBUG) { Serial.println(F("SWITCH")); }
    }
    else if (strncmp((char*)payload, "BUTTON", length) == 0)
    {
      usmInput[mcp].setType(input, BUTTON);
      if (ENABLE_DEBUG) { Serial.println(F("BUTTON")); }
    }
    else if (strncmp((char*)payload, "CONTACT", length) == 0)
    {
      usmInput[mcp].setType(input, CONTACT);
      if (ENABLE_DEBUG) { Serial.println(F("CONTACT")); }
    }
    else if (strncmp((char*)payload, "ROTARY", length) == 0)
    {
      usmInput[mcp].setType(input, ROTARY);
      if (ENABLE_DEBUG) { Serial.println(F("ROTARY")); }
    }
    else if (strncmp((char*)payload, "TOGGLE", length) == 0)
    {
      usmInput[mcp].setType(input, TOGGLE);
      if (ENABLE_DEBUG) { Serial.println(F("TOGGLE")); }
    }
    else
    {
      if (ENABLE_DEBUG) { Serial.println(F("ERROR")); }
    }
  }
  else if (strncmp(configType, "invt", 4) == 0)
  {
    if (ENABLE_DEBUG) { Serial.print(F(" INVT:")); }
    
    if (length == 0 || strncmp((char*)payload, "0", length) == 0)
    {
      usmInput[mcp].setInvert(input, 0);
      if (ENABLE_DEBUG) { Serial.println(0); }
    }
    else if (strncmp((char*)payload, "1", length) == 0)
    {
      usmInput[mcp].setInvert(input, 1);
      if (ENABLE_DEBUG) { Serial.println(1); }
    }
    else
    {
      if (ENABLE_DEBUG) { Serial.println(F("ERROR")); }
    }
  }
  else
  {
    if (ENABLE_DEBUG) { Serial.print(F(" INVALID:")); }
    if (ENABLE_DEBUG) { Serial.println(configType); }
  }
}

void getInputType(char inputType[], uint8_t type)
{
  // Determine what type of input we have
  sprintf_P(inputType, PSTR("ERROR"));
  switch (type)
  {
    case BUTTON:
      sprintf_P(inputType, PSTR("BUTTON"));
      break;
    case CONTACT:
      sprintf_P(inputType, PSTR("CONTACT"));
      break;
    case ROTARY:
      sprintf_P(inputType, PSTR("ROTARY"));
      break;
    case SWITCH:
      sprintf_P(inputType, PSTR("SWITCH"));
      break;
    case TOGGLE:
      sprintf_P(inputType, PSTR("TOGGLE"));
      break;
  }
}

void getEventType(char eventType[], uint8_t type, uint8_t state)
{
  // Determine what event we need to publish
  sprintf_P(eventType, PSTR("ERROR"));
  switch (type)
  {
    case BUTTON:
      switch (state)
      {
        case USM_HOLD_EVENT:
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
      }
      break;
    case ROTARY:
      switch (state)
      {
        case USM_LOW:
          sprintf_P(eventType, PSTR("UP"));
          break;
        case USM_HIGH:
          sprintf_P(eventType, PSTR("DOWN"));
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
      }
      break;
    case TOGGLE:
      sprintf_P(eventType, PSTR("TOGGLE"));
      break;
  }
}

char * getConfigTopic(char topic[])
{
  if (mqtt_base_topic == NULL)
  {
    sprintf_P(topic, PSTR("conf/%s/+/+"), g_mqtt_client_id);
  }
  else
  {
    sprintf_P(topic, PSTR("%s/conf/%s/+/+"), mqtt_base_topic, g_mqtt_client_id);
  }
  return topic;
}

char * getEventTopic(char topic[], uint8_t index)
{
  if (mqtt_base_topic == NULL)
  {
    sprintf_P(topic, PSTR("stat/%s/%d"), g_mqtt_client_id, index);
  }
  else
  {
    sprintf_P(topic, PSTR("%s/stat/%s/%d"), mqtt_base_topic, g_mqtt_client_id, index);
  }
  return topic;
}

/**
  OLED
*/
void updateOled(uint32_t port_changed)
{
  if (g_oled_found)
  {
    // Check if port animation update required
    if (port_changed)
    {
      oled.ssd1306WriteCmd(SSD1306_DISPLAYON);
      oled.setContrast(OLED_CONTRAST_ON);
      g_last_oled_trigger = millis(); 
 
      USM_Oled_animate(port_changed, g_mcp_io_values);
    }

    // Clear event display if timed out
    if (g_last_event_display)
    {
      if ((millis() - g_last_event_display) > OLED_EVENT_MS)
      {
        oled.setCursor(0, 7);
        oled.clearToEOL();
        
        g_last_event_display = 0L;
      }
    }

    // Dim OLED if timed out
    if (g_last_oled_trigger)
    {
      if ((millis() - g_last_oled_trigger) > OLED_ON_MS)
      {
        // Turn OLED OFF if OLED_CONTRAST_DIM is set to 0
        if (OLED_CONTRAST_DIM == 0)
        {
          oled.ssd1306WriteCmd(SSD1306_DISPLAYOFF);
        }
        else
        { 
          oled.setContrast(OLED_CONTRAST_DIM);
        }
        
        g_last_oled_trigger = 0L;
      }
    }
  } 
}

/**
  Button handlers
*/
void usmEvent(uint8_t id, uint8_t input, uint8_t type, uint8_t state)
{
  // Determine the port, channel, and index (all 1-based)
  uint8_t mcp = id;
  uint8_t raw_index = (MCP_PIN_COUNT * mcp) + input;
  uint8_t port = (raw_index / 4) + 1;
  uint8_t channel = (input % 4) + 1;
  uint8_t index = raw_index + 1;
  
  char inputType[8];
  getInputType(inputType, type);
  char eventType[7];
  getEventType(eventType, type, state);

  if (ENABLE_DEBUG)
  {
    Serial.print(F("[EVNT]"));
    Serial.print(F(" INDX:"));
    Serial.print(index);
    Serial.print(F(" TYPE:"));
    Serial.print(inputType);
    Serial.print(F(" EVNT:"));
    Serial.println(eventType);
  }

  char message[66];
  if (g_oled_found)
  {
    // Show last input event on buttom line
    oled.setCursor(0, 7);
    oled.setInvertMode(true);
    sprintf_P(message, PSTR("IDX:%2d %s %s   "), index, inputType, eventType);
    oled.print(message); 
    oled.setInvertMode(false);    
    g_last_event_display = millis(); 
  }

  if (mqtt_client.connected())
  {
    // Build JSON payload for this event
    sprintf_P(message, PSTR("{\"PORT\":%d,\"CHAN\":%d,\"INDX\":%d,\"TYPE\":\"%s\",\"EVNT\":\"%s\"}"), port, channel, index, inputType, eventType);
  
    // Publish event to MQTT
    char topic[42];
    mqtt_client.publish(getEventTopic(topic, index), message);
  }
  else
  {
    Serial.println("FAILOVER!!!");    
  }
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

  // Scan for MCP's
  uint8_t mcp = 0;
  for (uint8_t addr = 0; addr < sizeof(MCP_I2C_ADDRESS); addr++)
  {
    Serial.print(F(" - 0x"));
    Serial.print(MCP_I2C_ADDRESS[addr], HEX);
    Serial.print(F("..."));

    // Check if there is anything responding on this address
    Wire.beginTransmission(MCP_I2C_ADDRESS[addr]);
    if (Wire.endTransmission() == 0)
    {
      if (mcp < MCP_MAX_COUNT) 
      {
        bitWrite(g_mcps_found, mcp, 1);
        
        // If an MCP23017 was found then initialise and configure the inputs
        mcp23017[mcp].begin(MCP_I2C_ADDRESS[addr] & 0x07);
        for (uint8_t pin = 0; pin < MCP_PIN_COUNT; pin++)
        {
          mcp23017[mcp].pinMode(pin, INPUT);
          if (MCP_INTERNAL_PULLUPS) { mcp23017[mcp].pullUp(pin, HIGH); }

          // NOTE: above code works for v1.3.0 of the MCP23017 library
          //       but in v2.0.0+ need to replace with;
          //mcp23017[mcp].pinMode(pin, MCP_INTERNAL_PULLUPS ? INPUT_PULLUP : INPUT);
        }

        // Listen for input events
        usmInput[mcp].onEvent(usmEvent);

        Serial.print(F("MCP23017"));
        if (MCP_INTERNAL_PULLUPS) { Serial.print(F(" (internal pullups)")); }
        Serial.println();
      }
      else
      {
        // MCP found but we don't have enough space to handle it (see MCP_MAX_COUNT)
        Serial.println(F("ignored"));
      }

      // Increment our MCP index
      mcp++;
    }
    else
    {
      // No MCP found at this address
      Serial.println(F("empty"));
    }
  }
  
  // Scan for OLED   
  Serial.print(F(" - 0x"));
  Serial.print(OLED_I2C_ADDRESS, HEX);
  Serial.print(F("..."));

  // Check if OLED is anything responding
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  if (Wire.endTransmission() == 0)
  {
     g_oled_found = 1;
    
    // If OLED was found then initialise
    #ifdef OLED_TYPE_SSD1306
      Serial.print(F("SSD1306 "));
      #if OLED_RESET_PIN >= 0
        oled.begin(&Adafruit128x64, OLED_I2C_ADDRESS, OLED_RESET_PIN);
      #else
        oled.begin(&Adafruit128x64, OLED_I2C_ADDRESS);
      #endif
    #endif
    #ifdef OLED_TYPE_SH1106
      Serial.print(F("SH1106 "));
      #if OLED_RESET_PIN >= 0
        oled.begin(&SH1106_128x64, OLED_I2C_ADDRESS, OLED_RESET_PIN);
      #else
        oled.begin(&SH1106_128x64, OLED_I2C_ADDRESS);
      #endif
    #endif
    
    oled.clear();
    oled.setFont(Adafruit5x7);  
    oled.println(F("Initialising..."));
    
    Serial.println(F("OLED"));
  }
  else
  {
    Serial.println(F("empty"));
  }
}

// Read 1 byte from I2C device register
uint8_t readRegister(int adr, uint8_t reg) 
{
  Wire.beginTransmission(adr);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(adr, 1);
  while (!Wire.available()) {}
  return Wire.read();
}

// Write 1 byte to I2C device register
void writeRegister(int adr, uint8_t reg, uint8_t data) 
{
  Wire.beginTransmission(adr);
  Wire.write(reg);
  Wire.write(data); 
  Wire.endTransmission();
} 
