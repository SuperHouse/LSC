/*
 * LSC_Button.h
 * 
 * A push button Arduino library capable of returning the number of
 * consecutive button presses made in quick succession or if the 
 * button was held down for a long time. 
 * 
 * Based on mdPushButton by Michel Deslierres <sigmdel.ca/michel>
 * 
 */

#ifndef LSC_BUTTON_H
#define LSC_BUTTON_H

#include "Arduino.h"

// Hardcoded to save memory - should be configurable really
// All times in milliseconds
#define BUTTON_DEBOUNCE_PRESS_TIME    15      // delay to debounce the make part of the signal
#define BUTTON_DEBOUNCE_RELEASE_TIME  30      // delay to debounce the break part of the signal
#define BUTTON_MULTI_CLICK_TIME       250     // if 0, does not check for multiple button clicks
#define BUTTON_HOLD_TIME              500     // how often a HOLD event is sent while a button is long-pressed

// Assume we are dealing with a 2 byte IO value - i.e. 16 buttons
#define BUTTON_COUNT                  16

// Special state values
#define BUTTON_NO_STATE               0
#define BUTTON_HOLD_STATE             15

// Max number of clicks we support (won't report more even if button clicked more often)
#define BUTTON_MAX_CLICKS             5

// Button states
enum buttonState_t { AWAIT_PRESS, DEBOUNCE_PRESS, AWAIT_RELEASE, DEBOUNCE_RELEASE, AWAIT_MULTI_PRESS };

// Special structure to optimise memory usage for storing current state and click count
union buttonData_t
{
  uint8_t _data;
  struct 
  {
    buttonState_t state  : 4;
    uint8_t clicks : 4;
  } data;
};

// Callback type for onButtonPressed(uint8_t id, uint8_t button, uint8_t state)
//  * `id` is a custom id (user defined, passed to process()) 
//  * `button` is the button number (0 -> BUTTON_COUNT - 1)
//  * `state` is one of;
//    - 1, 2, .. BUTTON_MAX_CLICKS   = number of presses (i.e. multi-click)
//    - BUTTON_HOLD_STATE            = long press (repeats every BUTTON_HOLD_TIME ms)
typedef void (*buttonPressedCallback)(uint8_t, uint8_t, uint8_t);

class LSC_Button 
{
  public:
    LSC_Button(uint8_t active = LOW);
   
    // Process this set of button values and send events via onButtonPressed
    void process(uint8_t id, uint16_t button_value);

    // Set callback function to be called when is button event is detected
    void onButtonPressed(buttonPressedCallback);

  private:
    // Configuration variables
    uint8_t _active; 

    // Button press callback
    buttonPressedCallback _onButtonPressed;

    // State variables    
    // _lastUpdateTime: the last time we processed an update, allows for efficient calculation 
    // of event times instead of having to store a full uint32_t for each button (i.e. 16x)
    uint32_t _lastUpdateTime;
    
    // _eventTime[]: incrementing count of how many milliseconds spent in the current state
    uint16_t _eventTime[BUTTON_COUNT];

    // _buttonState[]: structure to store button state and click count in a single byte
    buttonData_t _buttonState[BUTTON_COUNT];

    // Private methods
    uint8_t * _update(uint16_t button_value);
};

#endif
