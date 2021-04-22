/**
  Light Switch Controller to MQTT

  Uses up to 6 MCP23017 I2C I/O buffers to detect light switch presses,
  and report them to MQTT.

  The report is to a topic of the form:

    tele/ABC123/BUTTONS

  where the "ABC123" is a unique ID derived from the MAC address of the
  device. The message is the numeric ID of the button that was pressed.
  Buttons are numbered from 1 to 96.

  Compile options:
    Arduino Uno or Arduino Mega 2560

  External dependencies. Install using the Arduino library manager:
      "PubSubClient" by Nick O'Leary

  Bundled dependencies. No need to install separately:
      "Adafruit SH1106" by wonho-maker, forked from Adafruit SSD1306 library

  More information:
    www.superhouse.tv/lightswitch

  Bugs:
   - Device ID is not being set correctly.
   - LCD doesn't work.
   - MQTT stops publishing after a while.

  To do:
   - Debouncing.
   - Multi-press, long press.
   - Front panel button inputs.
   - Save button state as 1 byte per port, instead of 1 byte per pin.

  Written by Jonathan Oxer for www.superhouse.tv
    https://github.com/superhouse/

  Copyright 2019-2021 SuperHouse Automation Pty Ltd
*/

#define VERSION "2.1"
/*--------------------------- Configuration ------------------------------*/
// Configuration should be done in the included file:
#include "config.h"

/*--------------------------- Libraries ----------------------------------*/
//#include <SPI.h>
#include <Wire.h>                     // For I2C
#include <Adafruit_GFX.h>             // For OLED
#include <Adafruit_SSD1306.h>         // For OLED
#include <Ethernet.h>                 // For networking
#include <PubSubClient.h>             // For MQTT
#include "Adafruit_MCP23017.h"        // For MCP23017 I/O buffers

/*--------------------------- Global Variables ---------------------------*/
// Ethernet
static byte mac[]  = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };  // Set if no MAC ROM
static byte ip[]   = { 192, 168, 1, 35 }; // Use if DHCP disabled

// MQTT
char g_mqtt_message_buffer[50];     // Longest message we send is currently only 2 bytes long
char g_command_topic[50];           // MQTT topic for receiving commands
char g_button_topic[50];            // MQTT topic for reporting button presses
char g_client_buffer[20];

// Inputs
uint8_t button_status[8][16];
uint32_t g_last_input_time = 0;     // Used for debouncing

// General
//uint32_t g_device_id;               // Unique ID for device

/*--------------------------- Function Signatures ------------------------*/
void mqttCallback(char* topic, byte* payload, int length);
byte readRegister(byte r);

/*--------------------------- Instantiate Global Objects -----------------*/
// I/O buffers
Adafruit_MCP23017 g_input_buffers[6];
//Adafruit_MCP23017 mcp7;

// Ethernet client
EthernetClient ethClient;

// MQTT client
PubSubClient mqtt_client(mqtt_broker, 1883, mqttCallback, ethClient);

// OLED
Adafruit_SSD1306 OLED(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

/*--------------------------- Program ------------------------------------*/
/**
  Setup
*/
void setup()
{
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println();
  Serial.println(F("     SuperHouse.TV"));
  Serial.print(F("Light Switch Controller, v"));
  Serial.println(VERSION);

  // Set up display
  OLED.begin(0x3C);
  OLED.clearDisplay();
  OLED.setTextWrap(false);
  OLED.setTextSize(1);
  OLED.setTextColor(WHITE);
  OLED.setCursor(0, 0);
  OLED.println("www.superhouse.tv");
  //OLED.println(" Particulate Matter");
  //OLED.print(" Sensor v"); OLED.println(VERSION);
  //OLED.print  (" Device id: ");
  //OLED.println(g_device_id, HEX);
  OLED.display();

  if ( ENABLE_MAC_ADDRESS_ROM == true )
  {
    Serial.print(F("Getting MAC address from ROM: "));
    mac[0] = readRegister(0xFA);
    mac[1] = readRegister(0xFB);
    mac[2] = readRegister(0xFC);
    mac[3] = readRegister(0xFD);
    mac[4] = readRegister(0xFE);
    mac[5] = readRegister(0xFF);
  } else {
    Serial.print(F("Using static MAC address: "));
  }
  // Print the IP address
  char tmpBuf[17];
  sprintf(tmpBuf, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(tmpBuf);

  // Set up the Ethernet library to talk to the Wiznet board
  if (true == ENABLE_DHCP)
  {
    Ethernet.begin(mac);      // Use DHCP
  } else {
    Ethernet.begin(mac, ip);  // Use static address defined above
  }

  // Print IP address:
  Serial.print(F("My IP: http://"));
  for (byte thisByte = 0; thisByte < 4; thisByte++) {
    // print the value of each byte of the IP address:
    Serial.print(Ethernet.localIP()[thisByte], DEC);
    if (thisByte < 3)
    {
      Serial.print(".");
    }
  }
  Serial.println();

  Serial.println("Id:");
  char device_id_buffer[17];
  sprintf(device_id_buffer, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  Serial.println(device_id_buffer);
  //g_device_id = device_id_buffer;
  //Serial.println(g_device_id);

  // Set up the MQTT topics. By inserting the unique ID,
  // the result is of the form: "stat/D9616F/BUTTONS" etc
  sprintf(g_command_topic, "cmnd/%02X%02X%02X/COMMAND", mac[3], mac[4], mac[5]);  // For receiving commands
  sprintf(g_button_topic,  "stat/%02X%02X%02X/BUTTONS", mac[3], mac[4], mac[5]);  // Button presses

  // Report the MQTT topics to the serial console
  Serial.println("MQTT topics:");
  Serial.println(g_command_topic);  // For receiving messages
  Serial.println(g_button_topic);   // From PMS

  // Connect to MQTT server and announce we're alive
  String clientString = "Arduino-" + String(Ethernet.localIP());
  clientString.toCharArray(g_client_buffer, clientString.length() + 1);
  if (mqtt_client.connect(g_client_buffer, mqtt_username, mqtt_password))
  {
    Serial.println(F("MQTT connected"));
    String messageString = clientString + ": Starting up";
    messageString.toCharArray(g_mqtt_message_buffer, messageString.length() + 1);
    mqtt_client.publish("events", g_mqtt_message_buffer);
    //mqtt_client.subscribe("test/2");
  }

  for (uint8_t i = 0; i < 6; i++)
  {
    g_input_buffers[i].begin(i);
    for (uint8_t j = 0; j < 16; j++)
    {
      g_input_buffers[i].pinMode(j, INPUT);
      g_input_buffers[i].pullUp(j, HIGH);
    }
  }

  /*
    mcp7.begin(7);  // Address 7 (A2=1, A1=1, A0=1)
    for (int i = 0; i < 16; i++) {
      mcp7.pinMode(i, INPUT);
      mcp7.pullUp(i, HIGH);  // turn on a 100K pullup internally
    }
  */
}


/**

*/
void loop()
{
  uint16_t bit_masks[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

  // Iterate through each of the MCP23017 input buffers
  for (uint8_t input_buffer_number = 0; input_buffer_number < 6; input_buffer_number++)
  {
    uint16_t port_value = g_input_buffers[input_buffer_number].readGPIOAB();
    for (uint8_t input_pin_number = 0; input_pin_number < 16; input_pin_number++)
    {
      if ((port_value & bit_masks[input_pin_number]) == 0)
      {
        // Bit value is 0 (low) so button is pressed
        if (button_status[input_buffer_number][input_pin_number] == 1) // Only act if the value was previously 1
        {
          if (millis() > g_last_input_time + DEBOUNCE_TIME)
          {
            g_last_input_time = millis();
            uint16_t button_number = (16 * input_buffer_number) + input_pin_number;
            button_number++; // Offset by 1 so buttons are number 1-96 instead of 0-95

#if true == DEBUG_BUTTONS
            Serial.print("Press detected. Chip:");
            Serial.print(input_buffer_number);
            Serial.print(" Bit:");
            Serial.print(input_pin_number);
            Serial.print(" Button:");
            Serial.println(button_number);
#endif

            sprintf(g_mqtt_message_buffer, "%01i", button_number);
            mqtt_client.publish(g_button_topic, g_mqtt_message_buffer);
          }
        }
        button_status[input_buffer_number][input_pin_number] = 0;
      } else {
        // Bit value is 1 (high) so button is not pressed
        button_status[input_buffer_number][input_pin_number] = 1;
      }
    }
  }


  /* This is the front panel. It probably shouldn't report to MQTT. */
  /*
    port_bank = 7;
    for (int i = 0; i < 16; i++)
    {
    port_value = mcp7.digitalRead(i);
    if (port_value != button_status[port_bank][i])
    {
      if (0 == port_value)
      {
        Serial.print("Press: ");
        Serial.print(port_bank);
        Serial.print("-");
        Serial.println(i);
        sprintf(g_mqtt_message_buffer, "%02i%02i", port_bank, i);
        mqtt_client.publish(g_button_topic, g_mqtt_message_buffer);
      }
      button_status[port_bank][i] = port_value;
    }
    }
  */
}

/**
  MQTT callback
*/
void mqttCallback (char* topic, byte * payload, int length)
{
  Serial.print(F("Received: "));
  for (int index = 0;  index < length;  index ++) {
    Serial.print(payload[index]);
  }
  Serial.println();
}

/**
  Required to read the MAC address ROM via I2C
*/
byte readRegister(byte r)
{
  unsigned char v;
  Wire.beginTransmission(MAC_I2C_ADDRESS);
  Wire.write(r);  // Register to read
  Wire.endTransmission();

  Wire.requestFrom(MAC_I2C_ADDRESS, 1); // Read a byte
  while (!Wire.available())
  {
    // Wait
  }
  v = Wire.read();
  return v;
}
