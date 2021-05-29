
/*
 * USM_Input.cpp
 * 
 * An Arduino library capable of detecting input events and reporing
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
    // Default all inputs to buttons (non-inverted)
    setType(i, BUTTON);
    setInvert(i, 0);

    // Assume all inputs are in-active - i.e. HIGH
    _usmState[i].data.state = IS_HIGH;
    _usmState[i].data.clicks = 0;
    
    _eventTime[i] = 0;
  }
}

uint8_t USM_Input::getType(uint8_t input)
{
  // shifts the desired 2 bits to the right most position then masks the 2 LSB
  return (_usmType >> (input * 2)) & 0x0003L;
}

void USM_Input::setType(uint8_t input, uint8_t type)
{
  // sets a mask with the 2 bits we want to change to 0  
  uint32_t mask = ~(0x03L << (input * 2));
  // '& mask' clears, then '| (..)' sets the desired type at desired location 
  _usmType = (_usmType & mask) | ((uint32_t)type << (input * 2));
}

uint8_t USM_Input::getInvert(uint8_t input)
{
  // shifts the desired 1 bit to the right most position then masks the LSB
  return (_usmInvert >> input) & 0x0001;
}

void USM_Input::setInvert(uint8_t input, uint8_t invert)
{
  // sets a mask with the 1 bit we want to change to 0  
  uint16_t mask = ~(0x0001 << input);
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
  uint8_t state[USM_INPUT_COUNT];
  _update(state, value);

  // Check if we have a callback to handle the press events
  if (_onEvent) 
  {
    for (uint8_t i = 0; i < USM_INPUT_COUNT; i++)
    {
      // Only interested in inputs with events to report
      if (state[i] != USM_NO_STATE) 
      {
        _onEvent(id, i, getType(i), state[i]);
      }
    }
  }
}  

uint8_t USM_Input::_getValue(uint16_t value, uint8_t input)
{
  uint8_t bit = bitRead(value, input);
  if (getInvert(input)) { bit = !bit; }
  return bit;
}

void USM_Input::_update(uint8_t state[], uint16_t value) 
{
  // Work out how long since our last update so we can increment the event times for each button
  uint16_t delta = millis() - _lastUpdateTime;
  _lastUpdateTime = millis();
  
  // Process each button (this is not doing any I/O)
  for (uint8_t i = 0; i < USM_INPUT_COUNT; i++)
  {
    // Default to a no state - i.e. no event
    state[i] = USM_NO_STATE;

    // Increment the event time for this button
    _eventTime[i] = _eventTime[i] + delta;

    // Get the configured type of this input
    uint8_t type = getType(i);
    
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
      if (_eventTime[i] > USM_DEBOUNCE_LOW_TIME) 
      {
        _usmState[i].data.state = IS_LOW;
        _eventTime[i] = 0;

        // for CONTACT, SWITCH or TOGGLE inputs send an event since we have transitioned
        if (type != BUTTON)
        {
          state[i] = USM_LOW;
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
        if (type == BUTTON && _eventTime[i] > USM_HOLD_TIME) 
        {
          _usmState[i].data.clicks = USM_HOLD_STATE;
          _eventTime[i] = 0;
          state[i] = USM_HOLD_STATE;
        }
      }
    }
    // DEBOUNCE_HIGH
    else if (_usmState[i].data.state == DEBOUNCE_HIGH) 
    {
      if (_eventTime[i] > USM_DEBOUNCE_HIGH_TIME) 
      {
        // for CONTACT, SWITCH or TOGGLE inputs send an event since we have transitioned
        // otherwise check if we have been holding or increment the click count
        if (type != BUTTON)
        {
          _usmState[i].data.state = IS_HIGH;
          _eventTime[i] = 0;
          state[i] = USM_HIGH;
        }
        else
        {
          if (_usmState[i].data.clicks == USM_HOLD_STATE) 
          {
            _usmState[i].data.state = IS_HIGH;
          } 
          else 
          {
            _usmState[i].data.clicks = min(USM_MAX_CLICKS, _usmState[i].data.clicks + 1);
            _usmState[i].data.state = AWAIT_MULTI;
            _eventTime[i] = 0; 
          }
        }
      }  
    } 
    // AWAIT_MULTI
    else if (_usmState[i].data.state == AWAIT_MULTI) 
    { 
      if (_getValue(value, i) == LOW) 
      {
        _usmState[i].data.state = DEBOUNCE_LOW;
        _eventTime[i] = 0;
      } 
      else if (_eventTime[i] > USM_MULTI_CLICK_TIME) 
      {
        _usmState[i].data.state = IS_HIGH;
        state[i] = _usmState[i].data.clicks;
      } 
    }
  }

  return state;  
}    
