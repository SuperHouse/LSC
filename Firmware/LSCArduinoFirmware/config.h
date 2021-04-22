/* ----------------- General config -------------------------------- */
/* Debugging */
#define DEBUG_BUTTONS               true

/* Ethernet */
#define ENABLE_DHCP                 true    // true/false
#define ENABLE_MAC_ADDRESS_ROM      true    // true/false
#define MAC_I2C_ADDRESS             0x50    // Microchip 24AA125E48 I2C ROM address

/* MQTT */
const char* mqtt_broker           = "192.168.1.185"; // IP address of your MQTT broker
const char* mqtt_username         = "YOUR USERNAME"; // Your MQTT username
const char* mqtt_password         = "YOUR PASSWORD"; // Your MQTT password

/* Serial */
#define     SERIAL_BAUD_RATE    115200               // Speed for USB serial console

/* ----------------- Hardware-specific config ---------------------- */
/* Switch inputs */
#define     DEBOUNCE_TIME          400      // Milliseconds

/* OLED */
#define     SCREEN_WIDTH           128      // OLED display width (pixels)
#define     SCREEN_HEIGHT           64      // OLED display height (pixels)
#define     OLED_RESET              -1      // Reset pin (or -1 if sharing Arduino reset pin)
