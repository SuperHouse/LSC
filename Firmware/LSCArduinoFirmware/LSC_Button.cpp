/*
 * LSC_Button.cpp
 * 
 * A push button Arduino library capable of returning the number of
 * consecutive button presses made in quick succession or if the 
 * button was held down for a long time. 
 *
 * Based on mdPushButton by Michel Deslierres <sigmdel.ca/michel>
 * 
 */

#include "Arduino.h"
#include "LSC_Button.h"

// If active = LOW (the default) then an input is pulled low on a button press
// If active = HIGH then an input is pulled high on a button press
LSC_Button::LSC_Button(uint8_t active) 
{
  // What is the button pressed (active) state?
  _active = active;

  // Initialise our state variables
  _lastUpdateTime = 0;
  for (uint8_t i = 0; i < BUTTON_COUNT; i++)
  {
    _buttonState[i].data.state = AWAIT_PRESS;
    _buttonState[i].data.clicks = 0;
    _eventTime[i] = 0;
  }
}

void LSC_Button::onButtonPressed(buttonPressedCallback callback)
{ 
  _onButtonPressed = callback; 
}

void LSC_Button::process(uint8_t id, uint16_t button_value) 
{
  // Process each input to see what, if any, events have occured
  uint8_t * state = _update(button_value);

  // Check if we have a callback to handle the press events
  if (_onButtonPressed) 
  {
    for (uint8_t i = 0; i < BUTTON_COUNT; i++)
    {
      // Only interested in buttons with events to report
      if (state[i] != BUTTON_NO_STATE) {
        _onButtonPressed(id, i, state[i]);
      }
    }
  }
}  

uint8_t * LSC_Button::_update(uint16_t button_value) 
{
  static uint8_t state[BUTTON_COUNT] = {};

  // Work out how long since our last update so we can increment the event times for each button
  uint16_t delta = millis() - _lastUpdateTime;
  _lastUpdateTime = millis();
  
  // Process each button (this is not doing any I/O)
  for (uint8_t i = 0; i < BUTTON_COUNT; i++)
  {
    // Default to a no state - i.e. no event
    state[i] = BUTTON_NO_STATE;

    // Increment the event time for this button
    _eventTime[i] = _eventTime[i] + delta;
    
    // AWAIT_PRESS
    if (_buttonState[i].data.state == AWAIT_PRESS) 
    {
      _buttonState[i].data.clicks = 0;
      if (bitRead(button_value, i) == _active) 
      {
        _buttonState[i].data.state = DEBOUNCE_PRESS;
        _eventTime[i] = 0;
      }
    } 
    // DEBOUNCE_PRESS
    else if (_buttonState[i].data.state == DEBOUNCE_PRESS) 
    {
      if (_eventTime[i] > BUTTON_DEBOUNCE_PRESS_TIME) 
      {
        _buttonState[i].data.state = AWAIT_RELEASE;
        _eventTime[i] = 0;
      }  
    } 
    // AWAIT_RELEASE
    else if (_buttonState[i].data.state == AWAIT_RELEASE) 
    {
      if (bitRead(button_value, i) != _active) 
      {
        _buttonState[i].data.state = DEBOUNCE_RELEASE;
        _eventTime[i] = 0;
      }
      else
      {
        if (_eventTime[i] > BUTTON_HOLD_TIME) 
        {
          _buttonState[i].data.clicks = BUTTON_HOLD_STATE;
          _eventTime[i] = 0;
          state[i] = BUTTON_HOLD_STATE;
        }
      }
    }
    // DEBOUNCE_RELEASE
    else if (_buttonState[i].data.state == DEBOUNCE_RELEASE) 
    {
      if (_eventTime[i] > BUTTON_DEBOUNCE_RELEASE_TIME) 
      {
        if (_buttonState[i].data.clicks == BUTTON_HOLD_STATE) 
        {
          _buttonState[i].data.state = AWAIT_PRESS;
        } 
        else 
        {
          _buttonState[i].data.clicks = min(BUTTON_MAX_CLICKS, _buttonState[i].data.clicks + 1);
          _buttonState[i].data.state = AWAIT_MULTI_PRESS;
          _eventTime[i] = 0; 
        }
      }  
    } 
    // AWAIT_MULTI_PRESS
    else if (_buttonState[i].data.state == AWAIT_MULTI_PRESS) 
    { 
      if (bitRead(button_value, i) == _active) 
      {
        _buttonState[i].data.state = DEBOUNCE_PRESS;
        _eventTime[i] = 0;
      } 
      else if (_eventTime[i] > BUTTON_MULTI_CLICK_TIME) 
      {
        _buttonState[i].data.state = AWAIT_PRESS;
        state[i] = _buttonState[i].data.clicks;
      } 
    }
  }

  return state;  
}    
