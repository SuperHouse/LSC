/*
 * USM_Input.cpp
 * 
 * An Arduino library capable of detecting input events and reporting
 * consecutive button presses made in quick succession or if the 
 * button was held down for a long time. 
 *
 */

#include "Arduino.h"
#include "USM_Input.h"

USM_Input::USM_Input() 
{
  // Initialise our state variables
  _lastUpdateTime = 0;
  for (uint8_t i = 0; i < USM_INPUT_COUNT; i++)
  {
    // Default all inputs to non-inverted switches
    setType(i, SWITCH);
    setInvert(i, 0);

    // Assume all inputs are in-active - i.e. HIGH
    _usmState[i].data.state = IS_HIGH;
    _usmState[i].data.clicks = 0;
    
    _eventTime[i] = 0;
  }
}

uint8_t USM_Input::getType(uint8_t input)
{
  uint8_t index = input / 2;
  uint8_t bits = (input % 2) * 4;
  
  // shifts the desired 4 bits to the right most position then masks the 4 LSB
  return (_usmType[index] >> bits) & 0x0F;
}

void USM_Input::setType(uint8_t input, uint8_t type)
{
  uint8_t index = input / 2;
  uint8_t bits = (input % 2) * 4;
  
  // sets a mask with the 4 bits we want to change to 0  
  uint8_t mask = ~(0x0F << bits);
  // '& mask' clears, then '| (..)' sets the desired type at desired location 
  _usmType[index] = (_usmType[index] & mask) | (type << bits);

  // reset the state for this input ready for processing again
  _usmState[input].data.state = IS_HIGH;
}

uint8_t USM_Input::getInvert(uint8_t input)
{
  // shifts the desired 1 bit to the right most position then masks the LSB
  return (_usmInvert >> input) & 0x01;
}

void USM_Input::setInvert(uint8_t input, uint8_t invert)
{
  // sets a mask with the 1 bit we want to change to 0  
  uint16_t mask = ~(0x01 << input);
  // '& mask' clears, then '| (..)' sets the desired type at desired location 
  _usmInvert = (_usmInvert & mask) | ((uint16_t)invert << input);
}

void USM_Input::onEvent(eventCallback callback)
{ 
  _onEvent = callback; 
}

void USM_Input::process(uint8_t id, uint16_t value) 
{
  // Process each input to see what, if any, events have occured
  uint8_t event[USM_INPUT_COUNT];
  _update(event, value);

  // Check if we have a callback to handle the press events
  if (_onEvent) 
  {
    for (uint8_t i = 0; i < USM_INPUT_COUNT; i++)
    {
      // Only interested in inputs with events to report
      if (event[i] != USM_NO_EVENT) 
      {
        _onEvent(id, i, getType(i), event[i]);
      }
    }
  }
}  

uint8_t USM_Input::_getValue(uint16_t value, uint8_t input)
{
  return (bitRead(value, input) ^ getInvert(input));
}

uint16_t USM_Input::_getDebounceLowTime(uint8_t type)
{
  switch (type)
  {
    case BUTTON:
      return USM_BTN_DEBOUNCE_LOW_MS;
    case ROTARY:
      return USM_ROT_DEBOUNCE_LOW_MS;
    default:
      return USM_OTH_DEBOUNCE_LOW_MS;
  }
}

uint16_t USM_Input::_getDebounceHighTime(uint8_t type)
{
  switch (type)
  {
    case BUTTON:
      return USM_BTN_DEBOUNCE_HIGH_MS;
    case ROTARY:
      return USM_ROT_DEBOUNCE_HIGH_MS;
    default:
      return USM_OTH_DEBOUNCE_HIGH_MS;
  }
}

void USM_Input::_update(uint8_t event[], uint16_t value) 
{
  // Work out how long since our last update so we can increment the event times for each button
  uint16_t delta = millis() - _lastUpdateTime;
  _lastUpdateTime = millis();

  // Read rotary encoder values in pairs (gaps allowed)
  uint8_t rotaryReady = 0;
  uint8_t rotaryValue1;
  uint8_t rotaryValue2;
  
  // Process each button (this is not doing any I/O)
  for (uint8_t i = 0; i < USM_INPUT_COUNT; i++)
  {
    // Default to no state - i.e. no event
    event[i] = USM_NO_EVENT;

    // Increment the event time for this button
    _eventTime[i] = _eventTime[i] + delta;

    // Get the configured type of this input
    uint8_t type = getType(i);

    if (type == ROTARY)
    {
      if (rotaryReady == 0)
      {
        rotaryValue1 = _getValue(value, i);
        rotaryReady = 1;
      }
      else
      {
        rotaryValue2 = _getValue(value, i);
        rotaryReady = 0;
        
        // Get the encoder (gray) state, now we have values for both inputs
        unsigned char rotaryState = rotaryValue2 << 1 | rotaryValue1;

        // Check if this event generates an output (before updating state below)
        event[i] = usmRotaryEvent[_usmState[i].data.state][rotaryState];

        // Update the state from our state table
        _usmState[i].data.state = usmRotaryState[_usmState[i].data.state][rotaryState];
      }
    }
    else
    {
      // IS_HIGH
      if (_usmState[i].data.state == IS_HIGH) 
      {
        _usmState[i].data.clicks = 0;
        if (_getValue(value, i) == LOW) 
        {
          _usmState[i].data.state = DEBOUNCE_LOW;
          _eventTime[i] = 0;
        }
      } 
      // DEBOUNCE_LOW
      else if (_usmState[i].data.state == DEBOUNCE_LOW) 
      {
        if (_getValue(value, i) == HIGH)
        {
          // if input bounces before our debounce timer expires then must be a glitch so reset
          _usmState[i].data.state = IS_HIGH;
          _eventTime[i] = 0;
        }
        else if (_eventTime[i] > _getDebounceLowTime(type)) 
        {
          _usmState[i].data.state = IS_LOW;
          _eventTime[i] = 0;
  
          // for CONTACT, SWITCH or TOGGLE inputs send an event since we have transitioned
          if (type != BUTTON)
          {
            event[i] = USM_LOW;
          }
        }  
      } 
      // IS_LOW
      else if (_usmState[i].data.state == IS_LOW) 
      {
        if (_getValue(value, i) == HIGH) 
        {
          _usmState[i].data.state = DEBOUNCE_HIGH;
          _eventTime[i] = 0;
        }
        else
        {
          if (type == BUTTON && _eventTime[i] > USM_BTN_HOLD_MS) 
          {
            _usmState[i].data.clicks = USM_HOLD_EVENT;
            _eventTime[i] = 0;
            event[i] = USM_HOLD_EVENT;
          }
        }
      }
      // DEBOUNCE_HIGH
      else if (_usmState[i].data.state == DEBOUNCE_HIGH) 
      {
        if (_getValue(value, i) == LOW)
        {
          // if input bounces before our debounce timer expires then must be a glitch so reset
          _usmState[i].data.state = IS_LOW;
          _eventTime[i] = 0;
        }
        else if (_eventTime[i] > _getDebounceHighTime(type)) 
        {
          // for CONTACT, SWITCH or TOGGLE inputs send an event since we have transitioned
          // otherwise check if we have been holding or increment the click count
          if (type != BUTTON)
          {
            _usmState[i].data.state = IS_HIGH;
            _eventTime[i] = 0;
            event[i] = USM_HIGH;
          }
          else
          {
            if (_usmState[i].data.clicks == USM_HOLD_EVENT) 
            {
              _usmState[i].data.state = IS_HIGH;
            } 
            else 
            {
              _usmState[i].data.clicks = min(USM_BTN_MAX_CLICKS, _usmState[i].data.clicks + 1);
              _usmState[i].data.state = AWAIT_MULTI;
              _eventTime[i] = 0; 
            }
          }
        }  
      } 
      // AWAIT_MULTI (can only be here for BUTTON inputs)
      else if (_usmState[i].data.state == AWAIT_MULTI) 
      { 
        if (_getValue(value, i) == LOW) 
        {
          _usmState[i].data.state = DEBOUNCE_LOW;
          _eventTime[i] = 0;
        } 
        else if (_eventTime[i] > USM_BTN_MULTI_CLICK_MS) 
        {
          _usmState[i].data.state = IS_HIGH;
          event[i] = _usmState[i].data.clicks;
        } 
      }
    }
  }
}    
