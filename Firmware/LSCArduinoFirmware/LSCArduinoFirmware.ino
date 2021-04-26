/**
  Light Switch Controller to MQTT

  Uses up to 6 MCP23017 I2C I/O buffers to detect light switch presses,
  and report them to MQTT.

  The report is to a topic of the form:

    stat/ABC123/BUTTONS

  where the "ABC123" is a unique ID derived from the MAC address of the
  device. The message is the numeric ID of the button that was pressed.
  Buttons are numbered from 1 to 96.

  Compile options:
    Arduino Uno or Arduino Mega 2560

  External dependencies. Install using the Arduino library manager:
      "Adafruit_GFX"
      "Adafruit_SSD1306"
      "Adafruit_MCP23017"
      "PubSubClient" by Nick O'Leary

  Bundled dependencies. No need to install separately:
      "Adafruit SH1106" by wonho-maker, forked from Adafruit SSD1306 library

  More information:
    www.superhouse.tv/lightswitch

  Bugs:
   - See GitHub issues list.

  To do:
   - Configurable MCP count.
   - Debouncing.
   - Multi-press, long press.
   - Front panel button inputs.

  Written by Jonathan Oxer for www.superhouse.tv
    https://github.com/superhouse/

  Copyright 2019-2021 SuperHouse Automation Pty Ltd
*/

/*--------------------------- Version ------------------------------------*/
#define VERSION "2.2"

/*--------------------------- Configuration ------------------------------*/
// Should be no user configuration in this file, everything should be in;
#include "config.h"

/*--------------------------- Libraries ----------------------------------*/
#include <Wire.h>                       // For I2C
#include <Adafruit_GFX.h>               // For OLED
#include "Adafruit_SH1106.h"            // For OLED
#include <Ethernet.h>                   // For networking
#include <PubSubClient.h>               // For MQTT
#include "Adafruit_MCP23017.h"          // For MCP23017 I/O buffers

/*--------------------------- Constants ----------------------------------*/
#define BUTTON_PRESSED    0
#define BUTTON_RELEASED   1

// A single I2C bus can support up to 8 MCP23017 chips (16 inputs each)
#define MAX_MCP_COUNT     8
#define MCP_PIN_COUNT     16

// List of all I2C addresses we might be interested in
// 0x20-0x27 are the 8x MCP23017 chips, 0x3C is the OLED button inputs
const byte I2C_ADDRESS[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x3C };

/*--------------------------- Global Variables ---------------------------*/
// MQTT
char g_mqtt_client_id[16];              // MQTT client id
char g_mqtt_command_topic[32];          // MQTT topic for receiving commands
char g_mqtt_button_topic[32];           // MQTT topic for reporting button events
char g_mqtt_message_buffer[32];         // MQTT message buffer

// Inputs
byte g_mcp_address[MAX_MCP_COUNT]     = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint32_t g_last_input_time = 0;         // Used for debouncing
uint16_t g_button_status[MAX_MCP_COUNT];

// OLED
byte g_oled_address = 0;                // Scan I2C bus for OLED

// Watchdog
uint32_t g_watchdog_last_reset = 0;

/*--------------------------- Function Signatures ------------------------*/
void mqttCallback(char* topic, byte* payload, int length);
byte readRegister(byte r);

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_MCP23017 mcp23017s[MAX_MCP_COUNT];

// Ethernet client
EthernetClient ethernet;

// MQTT client
PubSubClient mqtt_client(mqtt_broker, mqtt_port, mqttCallback, ethernet);

// OLED
Adafruit_SH1106 OLED(OLED_RESET);

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
  Serial.println(F("     SuperHouse.TV"));
  Serial.print(F("Light Switch Controller, v"));
  Serial.println(VERSION);
  Serial.println();

  // Set up watchdog
  initialiseWatchdog();

  // Scan the I2C bus to determine what hardware we have
  scanI2CBus();
  
  // Set up display if found
  if (g_oled_address != 0)
  {
    OLED.begin(SH1106_SWITCHCAPVCC, g_oled_address);
    OLED.clearDisplay();
    OLED.setTextWrap(false);
    OLED.setTextSize(1);
    OLED.setTextColor(WHITE);
    OLED.setCursor(0, 0);
    OLED.println("www.superhouse.tv");
    OLED.println(" Light Switch Controller");
    OLED.print(" Sensor v"); OLED.println(VERSION);
    //OLED.print  (" Device id: ");
    //OLED.println(g_device_id, HEX);
    OLED.display();
  }

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
  char mac_address[17];
  sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
  Serial.print(F("Device id: "));
  char device_id[17];
  sprintf(device_id, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  Serial.println(device_id);

  // Generate MQTT client id
  sprintf(g_mqtt_client_id, "Arduino-%s", device_id);

  // Generate MQTT topics using the device ID
  if (strlen(mqtt_base_topic) == 0)
  {
    sprintf(g_mqtt_command_topic, "cmnd/%s/COMMAND", device_id);
    sprintf(g_mqtt_button_topic,  "stat/%s/BUTTONS", device_id);
  }
  else
  {
    sprintf(g_mqtt_command_topic, "%s/cmnd/%s/COMMAND", mqtt_base_topic, device_id);
    sprintf(g_mqtt_button_topic,  "%s/stat/%s/BUTTONS", mqtt_base_topic, device_id);
  }

  // Report MQTT details to the serial console
  Serial.print(F("MQTT client id: "));
  Serial.println(g_mqtt_client_id);
  Serial.print(F("MQTT command topic: "));
  Serial.println(g_mqtt_command_topic);
  Serial.print(F("MQTT status topic: "));
  Serial.println(g_mqtt_button_topic);

  // Initialise I/O chips
  Serial.print(F("Initialising MCP23017 chips..."));
  for (uint8_t mcp = 0; mcp < MAX_MCP_COUNT; mcp++)
  {
    if (g_mcp_address[mcp] == 0) 
      continue;

    mcp23017s[mcp].begin(mcp);
    for (uint8_t pin = 0; pin < MCP_PIN_COUNT; pin++)
    {
      mcp23017s[mcp].pinMode(pin, INPUT);
      mcp23017s[mcp].pullUp(pin, HIGH);
    }
  }
  Serial.println(F("done"));
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
    for (uint8_t mcp = 0; mcp < MAX_MCP_COUNT; mcp++)
    {
      if (g_mcp_address[mcp] == 0) 
        continue;

      // Read the full set of pins in one hit, then check each
      uint16_t io_value = mcp23017s[mcp].readGPIOAB();
      for (uint8_t pin = 0; pin < MCP_PIN_COUNT; pin++)
      {
        // If the value is HIGH the button is NOT pressed
        if (bitRead(io_value, pin) == BUTTON_PRESSED)
        {
          buttonPressed(mcp, pin);
        }
        else
        {
          buttonReleased(mcp, pin);
        }
      }
    }
  }
}

/**
  MQTT
*/
void mqttCallback(char* topic, byte * payload, int length)
{
  Serial.print(F("Received: "));
  for (int index = 0;  index < length;  index ++)
  {
    Serial.print(payload[index]);
  }
  Serial.println();
}

boolean mqttConnect()
{
  Serial.print(F("Connecting to MQTT broker..."));

  boolean success;
  if (ENABLE_MQTT_LWT)
  {
    success = mqtt_client.connect(g_mqtt_client_id, mqtt_username, mqtt_password, mqtt_lwt_topic, mqtt_lwt_qos, mqtt_lwt_retain, "0");
  }
  else
  {
    success = mqtt_client.connect(g_mqtt_client_id, mqtt_username, mqtt_password);
  }

  if (success)
  {
    Serial.println(F("success"));

    // Subscribe to our command topic
    mqtt_client.subscribe(g_mqtt_command_topic);

    // Publish LWT so anything listening knows we are alive
    if (ENABLE_MQTT_LWT)
    {
      byte lwt_payload[] = { '1' };
      mqtt_client.publish(mqtt_lwt_topic, lwt_payload, 1, mqtt_lwt_retain);
    }

    // Publish a message on the events topic to indicate startup
    if (ENABLE_MQTT_EVENTS)
    {
      sprintf(g_mqtt_message_buffer, "%s is starting up", g_mqtt_client_id);
      mqtt_client.publish(mqtt_events_topic, g_mqtt_message_buffer);
    }
  }
  else
  {
    Serial.println(F("failed, wait 5s before trying again"));
    delay(5000);
  }

  return success;
}

/**
  Button handlers
*/
void buttonPressed(uint8_t mcp, uint8_t pin)
{
  if (bitRead(g_button_status[mcp], pin) != BUTTON_PRESSED)
  {
    // Only act if the value has changed
    if (millis() > g_last_input_time + DEBOUNCE_TIME)
    {
      // Reset our debounce timer
      g_last_input_time = millis();

      // Determine the button number (1-based)
      uint16_t button = (MCP_PIN_COUNT * mcp) + pin + 1;

#if DEBUG_BUTTONS
      Serial.print(F("Press detected. Chip:"));
      Serial.print(mcp);
      Serial.print(F(" Bit:"));
      Serial.print(pin);
      Serial.print(F(" Button:"));
      Serial.println(button);
#endif

      // Publish event to MQTT
      sprintf(g_mqtt_message_buffer, "%01i", button);
      mqtt_client.publish(g_mqtt_button_topic, g_mqtt_message_buffer);
    }
  }

  // Update the button status
  g_button_status[mcp] = bitWrite(g_button_status[mcp], pin, BUTTON_PRESSED);
}

void buttonReleased(uint8_t mcp, uint8_t pin)
{
  // Update the button status
  g_button_status[mcp] = bitWrite(g_button_status[mcp], pin, BUTTON_RELEASED);
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
    if ((millis() - g_watchdog_last_reset) > WATCHDOG_RESET_INTERVAL)
    {
      // Pulse the watchdog to reset it
      digitalWrite(WATCHDOG_PIN, HIGH);
      delay(WATCHDOG_PULSE_LENGTH);
      digitalWrite(WATCHDOG_PIN, LOW);

      // Reset our internal timer
      g_watchdog_last_reset = millis();
    }
  }
}

/**
  I2C scanner
 */
void scanI2CBus()
{
  // Detect devices on the I2C bus to work out what hardware we have
  Serial.println(F("Detecting devices on the I2C bus..."));

  for (uint8_t i = 0; i < sizeof(I2C_ADDRESS); i++)
  {
    byte i2cAddress = I2C_ADDRESS[i];
    
    // ignore if nothing found at this address
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0)
      continue;

    // the last address is the OLED, all others are MCP23017
    if (i == (sizeof(I2C_ADDRESS) - 1))
    {
      g_oled_address = i2cAddress;
      Serial.print(F(" - OLED at "));
    }
    else
    {
      g_mcp_address[i] = i2cAddress;
      Serial.print(F(" - MCP23017 at "));
    }

    Serial.print(F("0x"));
    Serial.println(i2cAddress, HEX);
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
