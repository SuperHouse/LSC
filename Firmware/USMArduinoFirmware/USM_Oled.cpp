
#include "Arduino.h"
#include "USM_Oled.h"



extern SSD1306AsciiWire oled;

// holds bit map for 1 pattern , 8 bits high, PATTERN_WIDTH wide
uint8_t bit_map[PATTERN_WIDTH];

// combines (bitwise or) single pattern to complete one (multiple inputs per port)
void USM_update_bit_map (uint8_t pattern)
{
  for (uint8_t i = 0; i < PATTERN_WIDTH; i++)
  {
    bit_map[i] = bit_map[i] | pgm_read_byte(&pattern_table[pattern][i]); 
  }
}

// fills bit map with desired pattern
void USM_set_bit_map (uint8_t pattern)
{
  memcpy_P(bit_map, pattern_table[pattern], PATTERN_WIDTH);
}

// writes bit map to OLED at x/y 
void USM_write_bit_map (uint8_t x, uint8_t y)
{
  uint8_t i;
  oled.setCursor(x, y);
  for (i = 0; i < PATTERN_WIDTH-1; i++)
  {
    oled.ssd1306WriteRamBuf(bit_map[i]);
  }
  oled.ssd1306WriteRam(bit_map[i]); // writes buffer to display
}

// writes a pattern to OLED at x/y
void USM_write_pattern (uint8_t pattern, uint8_t x, uint8_t y)
{
  USM_set_bit_map (pattern);
  USM_write_bit_map(x, y);
}


/** 
  draw logo and header on OLED
 */
void USM_Oled_draw_logo(char * firmware_version)
{ 
  oled.clear();
  // display logo
  USM_write_pattern (6, 0, 0);
  USM_write_pattern (7, 8, 0);
  USM_write_pattern (8, 0, 1);
  USM_write_pattern (9, 8, 1);
  
  oled.setCursor(25, 0);
  oled.println(F("SuperHouse.TV"));
  oled.setCursor(25, 1);
  oled.print(F("USM v"));
  oled.print(firmware_version);
  oled.setCursor(0, 7);
}

/** 
  draw port outline on OLED
 */
 void USM_Oled_draw_ports (uint8_t mcps_found)
{
  for (uint8_t i = 0; i < 12; i++)
  {
    uint8_t c = (bitRead(mcps_found, i>>1)) ? FRAME_SOLID : FRAME_DASHED;
    uint8_t x = i*PATTERN_WIDTH + i/4*4;
    USM_write_pattern (c, x, 4);
    USM_write_pattern (c, x, 5);
  }  
} 


/**
  animation of input state in ports view
  Ports:    | 1 | 3 | 5 | 7 |     Index:      | 1 : 3 | 9 : 11|
            +---+---+---+---+....             |.......|.......|
            | 2 | 4 | 6 | 8 |                 | 2 : 4 | 10: 12|
                                              +-------+-------+......
                                              | 5 : 7 | 13: 15|
                                              |.......|.......|
                                              | 6 : 8 | 14: 16|                                             
*/
void USM_Oled_animate (uint32_t port_changed, uint16_t g_mcp_io_values[])
{
  uint8_t i, x, y, c, port_val;
  uint16_t  io_value;

  for (i = 0; i < 24; i++)
  {
    if (bitRead(port_changed, i))
    {
      // determin the input levels of the port
      USM_set_bit_map(FRAME_SOLID);
      io_value = ~g_mcp_io_values[i>>2];
      port_val = (io_value >> (i & 0x03) * 4) & 0x0f;
      if (port_val & 0x01) USM_update_bit_map(INP_TOP_LEFT);
      if (port_val & 0x02) USM_update_bit_map(INP_BOTTOM_LEFT);
      if (port_val & 0x04) USM_update_bit_map(INP_TOP_RIGHT);
      if (port_val & 0x08) USM_update_bit_map(INP_BOTTOM_RIGHT);
      x = (i>>1)*PATTERN_WIDTH + (i>>3)*4;   //  i/2*PATTERN_WIDTH (port dist) + i/8*4 (port group dist)
      y = 4 + (i & 0x01);
      USM_write_bit_map(x, y);
    }
  } 
}
