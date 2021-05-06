
#include "Arduino.h"
#include "LSC_Oled.h"

#define SHOW_BUTTON                   // comment this line if button animation on port level only


extern SSD1306AsciiWire oled;

/**
  Calculate free RAM at runtime
*/
int getFreeRam() 
{
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}


/** 
  draw log and header on OLED
 */
void LSC_Oled_draw_logo(char * firmware_version)
{ 
  oled.clear();
  // displaying logo
  oled.setFont(blockfont8x8);  
  oled.setCursor(0, 0);
  oled.write(7); 
  oled.write(8); 
  oled.setCursor(0, 1);
  oled.write(9); 
  oled.write(10); 
  oled.setFont(Adafruit5x7);   
  
  oled.setCursor(25, 0);
  oled.println(F("SuperHouse.TV"));
  oled.setCursor(25, 1);
  oled.print(F("LSC v"));
  oled.print(firmware_version);
  oled.setCursor(0, 7);
  oled.print(F(" Free RAM: ")); 
  oled.println(getFreeRam());
}


/** 
  draw port outline on OLED
 */
 void LSC_Oled_draw_ports (uint8_t mcps_found)
{
  oled.setFont(blockfont10x8);  
  for (uint8_t i = 0; i < 12; i++)
  {
    uint8_t c = (bitRead(mcps_found, i>>1)) ? FRAME_SOLID : FRAME_DASHED;
    uint8_t x = i*10 + i/4*4;
    oled.setCursor(x, 4);
    oled.write(c); 
    oled.setCursor(x, 5);
    oled.write(c); 
  }  
  oled.setFont(Adafruit5x7);   
} 


/**
  animation of key_press in ports view
  | 1 | 3 | 5 | 7 |     
  +---+---+---+---+
  | 2 | 4 | 6 | 8 | 
*/
void LSC_Oled_animate (uint32_t port_changed, uint32_t port_new, uint16_t g_mcp_io_values[])
{
  uint8_t i, x, y, c, port_val;
  uint16_t  io_value;
  
  oled.setFont(blockfont10x8);  
  for (i = 0; i < 24; i++)
  {
    if (bitRead(port_changed, i))
    {
      
#ifndef SHOW_BUTTON   // usse port view or button view
      c = (bitRead (port_new, i)) ? SOLID : FRAME_SOLID ;
#else
      // determin the button of the port
      io_value = ~g_mcp_io_values[i>>2];
      port_val = (io_value >> (i & 0x03) * 4) & 0x0f;
      switch (port_val){
        case 0: c = FRAME_SOLID; break;
        case 1: c = BUT_TOP_LEFT; break;
        case 2: c = BUT_BOTTOM_LEFT; break;
        case 4: c = BUT_TOP_RIGHT; break;
        case 8: c = BUT_BOTTOM_RIGHT; break;
        default: c = BUT_MULTI; break;        // more than 1 button detected
      }
#endif      
      x = (i>>1)*10 + (i>>3)*4;   //  i/2*10(port dist) + i/8*4(port group dist)
      y = 4 + (i & 0x01);
      oled.setCursor(x, y);     
      oled.write(c); 
    }
  }
  oled.setFont(Adafruit5x7);  
}
