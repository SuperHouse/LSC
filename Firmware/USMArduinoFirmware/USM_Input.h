/*
 * USM_Input.h
 * 
 * An Arduino library capable of detecting input events and reporing
 * consecutive button presses made in quick succession or if the 
 * button was held down for a long time. 
 * 
 */

#ifndef USM_INPUT_H
#define USM_INPUT_H

#include "Arduino.h"

// Hardcoded to save memory - should be configurable really
// All times in milliseconds
#define USM_DEBOUNCE_LOW_TIME      15      // delay to debounce the make part of the signal
#define USM_DEBOUNCE_HIGH_TIME     30      // delay to debounce the break part of the signal
#define USM_MULTI_CLICK_TIME       250     // if 0, does not check for multiple button clicks
#define USM_HOLD_TIME              500     // how often a HOLD event is sent while a button is long-pressed

// Assume we are dealing with a 2 byte IO value - i.e. 16 buttons
#define USM_INPUT_COUNT            16

// Special state values
#define USM_NO_STATE               0
#define USM_HOLD_STATE             15
#define USM_LOW                    14
#define USM_HIGH                   13

// Max number of clicks we support (won't report more even if button clicked more often)
#define USM_MAX_CLICKS             5

// Input types
enum usmType_t { BUTTON, CONTACT, SWITCH, TOGGLE };

// Input states
enum usmState_t { IS_HIGH, DEBOUNCE_LOW, IS_LOW, DEBOUNCE_HIGH, AWAIT_MULTI };

// Special structure to optimise memory usage for storing current state and click count
union usmData_t
{
  uint8_t _data;
  struct 
  {
    usmState_t state : 4;
    uint8_t clicks : 4;
  } data;
};

// Callback type for onEvent(uint8_t id, uint8_t button, uint8_t state)
//  * `id` is a custom id (user defined, passed to process()) 
//  * `input` is the input number (0 -> USM_INPUT_COUNT - 1)
//  * `type` is one of BUTTON, CONTACT, SWITCH or TOGGLE
//  * `state` is one of;
//    - 1, 2, .. USM_MAX_CLICKS   = number of presses (i.e. multi-click)
//    - USM_HOLD_STATE            = long press (repeats every BUTTON_HOLD_TIME ms)
typedef void (*eventCallback)(uint8_t, uint8_t, uint8_t, uint8_t);

class USM_Input
{
  public:
    USM_Input();

    // Get/Set the input type
    uint8_t getType(uint8_t input);
    void setType(uint8_t input, uint8_t type);

    // Process this set of button values and send events via onButtonPressed
    void process(uint8_t id, uint16_t button_value);

    // Set callback function to be called when is button event is detected
    void onEvent(eventCallback);

  private:
    // Configuration variables
    uint32_t _usmType;

    // Input event callback
    eventCallback _onEvent;

    // State variables    
    // _lastUpdateTime: the last time we processed an update, allows for efficient calculation 
    // of event times instead of having to store a full uint32_t for each input (i.e. 16x)
    uint32_t _lastUpdateTime;
    
    // _eventTime[]: incrementing count of how many milliseconds spent in the current state
    uint16_t _eventTime[USM_INPUT_COUNT];

    // _usmState[]: structure to store state and click count in a single byte
    usmData_t _usmState[USM_INPUT_COUNT];

    // Private methods
//    uint8_t * _update(uint16_t value);
    void _update(uint8_t state[], uint16_t value);
};

#endif
