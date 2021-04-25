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
   - LCD doesn't work.

  To do:
   - Configurable MCP count.
   - Debouncing.
   - Multi-press, long press.
   - Front panel button inputs.
   - Save button state as 1 byte per port, instead of 1 byte per pin.

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
#include <Wire.h>                     // For I2C
#include <Adafruit_GFX.h>             // For OLED
#include <Adafruit_SSD1306.h>         // For OLED
#include <Ethernet.h>                 // For networking
#include <PubSubClient.h>             // For MQTT
#include "Adafruit_MCP23017.h"        // For MCP23017 I/O buffers

/*--------------------------- Constants ----------------------------------*/
#define BUTTON_PRESSED    0
#define BUTTON_RELEASED   1

const uint16_t BIT_MASK[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

/*--------------------------- Global Variables ---------------------------*/
// MQTT
char g_mqtt_client_id[16];          // MQTT client id
char g_mqtt_command_topic[32];      // MQTT topic for receiving commands
char g_mqtt_button_topic[32];       // MQTT topic for reporting button events
char g_mqtt_message_buffer[32];     // MQTT message buffer

// Inputs
uint8_t g_button_status[6][16];
uint32_t g_last_input_time = 0;     // Used for debouncing

// Watchdog
long watchdogLastResetTime = 0;

/*--------------------------- Function Signatures ------------------------*/
void mqttCallback(char* topic, byte* payload, int length);
byte readRegister(byte r);

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_MCP23017 mcp23017s[6];

// Ethernet client
EthernetClient ethernet;

// MQTT client
PubSubClient mqtt_client(mqtt_broker, mqtt_port, mqttCallback, ethernet);

// OLED
Adafruit_SSD1306 OLED(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

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
  
  // Set up display
  if (ENABLE_OLED) 
  {
    OLED.begin(0x3C);
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
    memcpy(mac, static_mac, 6);
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
  Serial.print("MQTT client id: ");
  Serial.println(g_mqtt_client_id);
  Serial.print("MQTT command topic: ");
  Serial.println(g_mqtt_command_topic);
  Serial.print("MQTT status topic: ");
  Serial.println(g_mqtt_button_topic);

  // Initialise I/O chips
  Serial.print("Initialising MCP23017 I/O chips (x6)...");
  for (uint8_t mcp = 0; mcp < 6; mcp++) 
  {
    mcp23017s[mcp].begin(mcp);
    for (uint8_t pin = 0; pin < 16; pin++) 
    {
      mcp23017s[mcp].pinMode(pin, INPUT);
      mcp23017s[mcp].pullUp(pin, HIGH);
    }
  }
  Serial.println("done");
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
    for (uint8_t mcp = 0; mcp < 6; mcp++) 
    {
      // Read the full set of pins in one hit, then check each      
      uint16_t io_value = mcp23017s[mcp].readGPIOAB();
      for (uint8_t pin = 0; pin < 16; pin++) 
      {
        // If the value is HIGH the button is NOT pressed
        if ((io_value & BIT_MASK[pin]) == BIT_MASK[pin]) 
        {
          buttonReleased(mcp, pin);
        } 
        else 
        {
          buttonPressed(mcp, pin);
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
  Serial.print("Connecting to MQTT broker...");

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
    Serial.println("success");
    
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
    Serial.println("failed");
  }
  
  return success; 
}

/**
  Button handlers
 */
void buttonPressed(uint8_t mcp, uint8_t pin) 
{
  if (g_button_status[mcp][pin] != BUTTON_PRESSED) 
  {
    // Only act if the value has changed
    if (millis() > g_last_input_time + DEBOUNCE_TIME) 
    {
      // Reset our debounce timer
      g_last_input_time = millis();
      
      // Determine the button number (1-based)
      uint16_t button = (16 * mcp) + pin + 1;

#if DEBUG_BUTTONS
      Serial.print("Press detected. Chip:");
      Serial.print(mcp);
      Serial.print(" Bit:");
      Serial.print(pin);
      Serial.print(" Button:");
      Serial.println(button);
#endif

      // Publish event to MQTT
      sprintf(g_mqtt_message_buffer, "%01i", button);
      mqtt_client.publish(g_mqtt_button_topic, g_mqtt_message_buffer);
    }
  }
  
  // Update the button status
  g_button_status[mcp][pin] = BUTTON_PRESSED;
}

void buttonReleased(uint8_t mcp, uint8_t pin) 
{
  // Update the button status
  g_button_status[mcp][pin] = BUTTON_RELEASED;
}

/**
  Watchdog
 */
void initialiseWatchdog() 
{
  if (ENABLE_WATCHDOG) 
  {
    Serial.print("Watchdog enabled on pin ");
    Serial.println(WATCHDOG_PIN);
    
    pinMode(WATCHDOG_PIN, OUTPUT);
    digitalWrite(WATCHDOG_PIN, LOW);
  } 
  else 
  {
    Serial.println("Watchdog NOT enabled");
  }
}

void patWatchdog() 
{
  if (ENABLE_WATCHDOG) 
  {
    if ((millis() - watchdogLastResetTime) > WATCHDOG_RESET_INTERVAL) 
    {
      // Pulse the watchdog to reset it
      digitalWrite(WATCHDOG_PIN, HIGH);
      delay(WATCHDOG_PULSE_LENGTH);
      digitalWrite(WATCHDOG_PIN, LOW);

      // Reset our internal timer
      watchdogLastResetTime = millis();
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
