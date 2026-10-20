#include <EEPROM.h>
#include <AceButton.h>
#include <IRremote.h>
#include <arduino-timer.h>
#include <atmega328_16mhz_ac_phase_control.h>

using namespace ace_button;

// 10 stages (0 to 9) mapped between 500 (Full ON) and 16500 (Low/Dim) ticks
const uint16_t spd[10] = {
  16500, // Stage 0: Min Brightness / Dim (~8250 µs delay)
  14724, // Stage 1
  12946, // Stage 2
  11168, // Stage 3
  9390,  // Stage 4
  7612,  // Stage 5
  5834,  // Stage 6
  4056,  // Stage 7
  2278,  // Stage 8
  500    // Stage 9: Max Brightness (~250 µs delay)
};

auto timer = timer_create_default();

#define RelayPin1 5
#define RelayPin2 6
#define RelayPin3 7
#define RelayPin4 8
#define TriacPin 9

#define SwitchPin1 A0
#define SwitchPin2 A1
#define SwitchPin3 A2
#define SwitchPin4 A3
#define SwitchPin5 A4
#define SwitchPin6 A5

#define ZeroCrossPin 2
#define IR_RECV_PIN 3

#define IR_Button_1    0xF50A7F80
#define IR_Button_2    0xE41B7F80
#define IR_Button_3    0xE01F7F80
#define IR_Button_4    0xF30C7F80
#define IR_Button_5    0xF20D7F80
#define IR_Button_Up   0xF9067F80
#define IR_Button_Dn   0xFA057F80
#define IR_All_Off     0xE11E7F80
#define IR_All_On      0xED127F80

#define EEPROM_RELAY1      0
#define EEPROM_RELAY2      1
#define EEPROM_RELAY3      2
#define EEPROM_RELAY4      3
#define EEPROM_DIMMER      7
#define EEPROM_TRIAC_STATE 8

#define DIMMER_MIN 0
#define DIMMER_MAX 9

String inputBuffer;
uint8_t dimm_value = 0;
bool triacState = false;

// Variables to track button states
bool buttonLastStates[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long buttonPressStartTime[6] = {0, 0, 0, 0, 0, 0};
bool buttonHeldTriggered[6] = {false, false, false, false, false, false};
bool buttonIsDown[6] = {false, false, false, false, false, false};
const unsigned long LONG_PRESS_THRESHOLD = 1200; // 1.2 seconds for long press

ButtonConfig config1;
ButtonConfig config2;
ButtonConfig config3;
ButtonConfig config4;
ButtonConfig config5;
ButtonConfig config6;

AceButton button1(&config1);
AceButton button2(&config2);
AceButton button3(&config3);
AceButton button4(&config4);
AceButton button5(&config5);
AceButton button6(&config6);

void all_Switch_ON();
void all_Switch_OFF();
void sendTasmotaStatus(int relayNum, bool state);
void sendDimmerStatus();
bool sendPeriodicStatus(void*);
void setDimmerLevel(uint8_t val);

void applyDimmer()
{
  if (triacState)
  {
    atmega328_16mhz_ac_phase_control.set_ac_power(spd[dimm_value]);
  }
  else
  {
    atmega328_16mhz_ac_phase_control.set_ac_power(19500); // 19500 ticks = fully OFF
  }
}

void triacOn()
{
  triacState = true;
  uint8_t savedDimmer = EEPROM.read(EEPROM_DIMMER);
  if (savedDimmer > 0 && savedDimmer <= DIMMER_MAX) 
  {
    dimm_value = savedDimmer;
  }
  else 
  {
    dimm_value = DIMMER_MAX; 
  }
  
  applyDimmer();
  EEPROM.update(EEPROM_DIMMER, dimm_value);
  EEPROM.update(EEPROM_TRIAC_STATE, 1);
  sendDimmerStatus();
  sendPeriodicStatus(nullptr);
}

void triacOff()
{
  triacState = false;
  dimm_value = DIMMER_MIN; 
  atmega328_16mhz_ac_phase_control.set_ac_power(19500); // Fully OFF
  EEPROM.update(EEPROM_DIMMER, dimm_value);
  EEPROM.update(EEPROM_TRIAC_STATE, 0);
  sendDimmerStatus();
  sendPeriodicStatus(nullptr);
}

void triacOnOff()
{
  if (triacState) triacOff();
  else triacOn();
}

void dimm_Up()
{
  if (!triacState) 
  { 
    setDimmerLevel(1); 
    return; 
  }
  
  if (dimm_value < DIMMER_MAX) { dimm_value++; }
  applyDimmer();
  EEPROM.update(EEPROM_DIMMER, dimm_value);
  sendDimmerStatus();
  sendPeriodicStatus(nullptr);
}

void dimm_Dn()
{
  if (!triacState) return;
  if (dimm_value > DIMMER_MIN) { dimm_value--; }
  applyDimmer();
  EEPROM.update(EEPROM_DIMMER, dimm_value);
  sendDimmerStatus();
  sendPeriodicStatus(nullptr);
}

void setDimmerLevel(uint8_t val)
{
  if (val > DIMMER_MAX) val = DIMMER_MAX;
  dimm_value = val;
  triacState = (dimm_value > 0);
  applyDimmer();
  EEPROM.update(EEPROM_DIMMER, dimm_value);
  EEPROM.update(EEPROM_TRIAC_STATE, triacState ? 1 : 0);
  sendDimmerStatus();
  sendPeriodicStatus(nullptr);
}

void setRelayState(int relay, bool turnOn)
{
  int pin = 0;
  int eepromAddress = 0;

  switch (relay)
  {
    case 1: pin = RelayPin1; eepromAddress = EEPROM_RELAY1; break;
    case 2: pin = RelayPin2; eepromAddress = EEPROM_RELAY2; break;
    case 3: pin = RelayPin3; eepromAddress = EEPROM_RELAY3; break;
    case 4: pin = RelayPin4; eepromAddress = EEPROM_RELAY4; break;
    default: return;
  }

  digitalWrite(pin, turnOn ? LOW : HIGH);
  EEPROM.update(eepromAddress, turnOn ? HIGH : LOW);
  sendTasmotaStatus(relay, turnOn);
  sendPeriodicStatus(nullptr);
  delay(50);
}

void relayToggle(int relay)
{
  int pin = 0;
  switch (relay)
  {
    case 1: pin = RelayPin1; break;
    case 2: pin = RelayPin2; break;
    case 3: pin = RelayPin3; break;
    case 4: pin = RelayPin4; break;
    default: return;
  }

  bool currentState = (digitalRead(pin) == LOW);
  setRelayState(relay, !currentState);
}

void sendTasmotaStatus(int relayNum, bool state)
{
  Serial.print("{\"POWER");
  Serial.print(relayNum);
  Serial.print("\":\"");
  Serial.print(state ? "ON" : "OFF");
  Serial.println("\"}");
}

void sendDimmerStatus()
{
  Serial.print("{\"Dimmer\":");
  Serial.print(dimm_value);
  Serial.println("}");
}

void eepromState()
{
  uint8_t relay1 = EEPROM.read(EEPROM_RELAY1);
  uint8_t relay2 = EEPROM.read(EEPROM_RELAY2);
  uint8_t relay3 = EEPROM.read(EEPROM_RELAY3);
  uint8_t relay4 = EEPROM.read(EEPROM_RELAY4);

  if (relay1 > 1) relay1 = LOW;
  if (relay2 > 1) relay2 = LOW;
  if (relay3 > 1) relay3 = LOW;
  if (relay4 > 1) relay4 = LOW;

  digitalWrite(RelayPin1, relay1 == HIGH ? LOW : HIGH); delay(50);
  digitalWrite(RelayPin2, relay2 == HIGH ? LOW : HIGH); delay(50);
  digitalWrite(RelayPin3, relay3 == HIGH ? LOW : HIGH); delay(50);
  digitalWrite(RelayPin4, relay4 == HIGH ? LOW : HIGH); delay(50);

  uint8_t storedDimmer = EEPROM.read(EEPROM_DIMMER);
  if (storedDimmer <= DIMMER_MAX) dimm_value = storedDimmer;
  else dimm_value = DIMMER_MAX;

  uint8_t storedTriacState = EEPROM.read(EEPROM_TRIAC_STATE);
  triacState = (storedTriacState == 1 || dimm_value > 0);

  applyDimmer();
}

void ir_remote()
{
  if (IrReceiver.decode())
  {
    if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT))
    {
      uint32_t code = IrReceiver.decodedIRData.decodedRawData;
      switch (code)
      {
        case IR_Button_1: relayToggle(1); break;
        case IR_Button_2: relayToggle(2); break;
        case IR_Button_3: relayToggle(3); break;
        case IR_Button_4: relayToggle(4); break;
        case IR_Button_5: triacOnOff(); break;
        case IR_Button_Up: dimm_Up(); break;
        case IR_Button_Dn: dimm_Dn(); break;
        case IR_All_Off: all_Switch_OFF(); break;
        case IR_All_On: all_Switch_ON(); break;
        default: break;
      }
    }
    IrReceiver.resume();
  }
}

void all_Switch_ON()
{
  setRelayState(1, true);
  setRelayState(2, true);
  setRelayState(3, true);
  setRelayState(4, true);
  setDimmerLevel(DIMMER_MAX);
}

void all_Switch_OFF()
{
  setRelayState(1, false);
  setRelayState(2, false);
  setRelayState(3, false);
  setRelayState(4, false);
  setDimmerLevel(DIMMER_MIN);
}

bool sendPeriodicStatus(void*)
{
  Serial.print("{\"POWER1\":\"");
  Serial.print(digitalRead(RelayPin1) == LOW ? "ON" : "OFF");
  Serial.print("\",\"POWER2\":\"");
  Serial.print(digitalRead(RelayPin2) == LOW ? "ON" : "OFF");
  Serial.print("\",\"POWER3\":\"");
  Serial.print(digitalRead(RelayPin3) == LOW ? "ON" : "OFF");
  Serial.print("\",\"POWER4\":\"");
  Serial.print(digitalRead(RelayPin4) == LOW ? "ON" : "OFF");
  Serial.print("\",\"Dimmer\":");
  Serial.print(dimm_value);
  Serial.println("}");
  return true;
}

void handleSerialControl()
{
  while (Serial.available() > 0)
  {
    char incomingChar = (char)Serial.read();
    if (incomingChar == '\n' || incomingChar == '\r')
    {
      inputBuffer.trim();
      if (inputBuffer.length() > 0)
      {
        String upperInput = inputBuffer;
        upperInput.toUpperCase();

        if (!upperInput.startsWith("QPC") && !upperInput.startsWith("WIF") && !upperInput.startsWith("HDW"))
        {
          bool jsonHandled = false;
          for (int i = 1; i <= 4; i++)
          {
            if (upperInput.indexOf("POWER" + String(i)) != -1)
            {
              if (upperInput.indexOf("ON") != -1) { setRelayState(i, true); jsonHandled = true; break; }
              else if (upperInput.indexOf("OFF") != -1) { setRelayState(i, false); jsonHandled = true; break; }
            }
          }

          if (!jsonHandled && upperInput.indexOf("DIMMER") != -1)
          {
            int index = upperInput.indexOf("DIMMER");
            int colonIndex = upperInput.indexOf(':', index);
            if (colonIndex != -1)
            {
              String sub = upperInput.substring(colonIndex + 1);
              sub.replace("\"", ""); sub.replace("}", ""); sub.trim();
              setDimmerLevel(sub.toInt());
              jsonHandled = true;
            }
          }

          if (!jsonHandled)
          {
            if (upperInput.indexOf("DIMM_UP") != -1 || upperInput.indexOf("IR_UP") != -1) dimm_Up();
            else if (upperInput.indexOf("DIMM_DN") != -1 || upperInput.indexOf("IR_DN") != -1) dimm_Dn();
            else if (upperInput.indexOf("ALL_ON") != -1) all_Switch_ON();
            else if (upperInput.indexOf("ALL_OFF") != -1) all_Switch_OFF();
            else
            {
              if (upperInput == "POWER1 ON" || upperInput == "R1_ON") setRelayState(1, true);
              else if (upperInput == "POWER1 OFF" || upperInput == "R1_OFF") setRelayState(1, false);
              else if (upperInput == "POWER2 ON" || upperInput == "R2_ON") setRelayState(2, true);
              else if (upperInput == "POWER2 OFF" || upperInput == "R2_OFF") setRelayState(2, false);
              else if (upperInput == "POWER3 ON" || upperInput == "R3_ON") setRelayState(3, true);
              else if (upperInput == "POWER3 OFF" || upperInput == "R3_OFF") setRelayState(3, false);
              else if (upperInput == "POWER4 ON" || upperInput == "R4_ON") setRelayState(4, true);
              else if (upperInput == "POWER4 OFF" || upperInput == "R4_OFF") setRelayState(4, false);
              else if (upperInput.startsWith("DIMMER ")) setDimmerLevel(upperInput.substring(7).toInt());
              else if (upperInput == "STATUS") sendPeriodicStatus(nullptr);
            }
          }
        }
      }
      inputBuffer = "";
    }
    else { inputBuffer += incomingChar; }
  }
}

// Handler for wall switches (Switches 1-4 as latching toggle switches, Switches 5-6 as momentary push-buttons)
void handleWallSwitches()
{
  uint8_t pins[6] = {SwitchPin1, SwitchPin2, SwitchPin3, SwitchPin4, SwitchPin5, SwitchPin6};
  unsigned long currentMillis = millis();
  
  for (int i = 0; i < 6; i++)
  {
    bool currentState = digitalRead(pins[i]);
    
    // Detect state change with software debouncing
    if (currentState != buttonLastStates[i])
    {
      delay(15); // Debounce delay
      currentState = digitalRead(pins[i]);
      
      if (currentState != buttonLastStates[i])
      {
        buttonLastStates[i] = currentState;
        
        if (i < 4)
        {
          // Switches 1 to 4: Standard wall latching switches (SPDT toggle)
          // Any state change (flipping the switch) toggles the corresponding relay
          switch (i)
          {
            case 0: relayToggle(1); break;
            case 1: relayToggle(2); break;
            case 2: relayToggle(3); break;
            case 3: relayToggle(4); break;
          }
        }
        else
        {
          // Switches 5 and 6 (SwitchPin5, SwitchPin6): Keep original momentary button behavior
          if (currentState == LOW) // Button Pressed Down
          {
            buttonIsDown[i] = true;
            buttonPressStartTime[i] = currentMillis;
            buttonHeldTriggered[i] = false;
          }
          else // Button Released Up
          {
            if (buttonIsDown[i])
            {
              buttonIsDown[i] = false;
              
              if (!buttonHeldTriggered[i])
              {
                if (i == 4) dimm_Dn();          // SwitchPin5 short click: Step down -1
                else if (i == 5) dimm_Up();     // SwitchPin6 short click: Step up +1
              }
            }
          }
        }
      }
    }
    
    // Check for continuous hold while button is kept pressed down (for SwitchPin5 and SwitchPin6)
    if ((i == 4 || i == 5) && buttonIsDown[i] && !buttonHeldTriggered[i])
    {
      if (currentMillis - buttonPressStartTime[i] >= LONG_PRESS_THRESHOLD)
      {
        buttonHeldTriggered[i] = true; 
        if (i == 4) setDimmerLevel(DIMMER_MIN); // Long hold on Down button jumps straight to 0
        else setDimmerLevel(DIMMER_MAX);      // Long hold on Up button jumps straight to 9
      }
    }
  }
}

void setup()
{
  Serial.begin(115200);
  inputBuffer.reserve(128);

  IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);

  pinMode(RelayPin1, OUTPUT);
  pinMode(RelayPin2, OUTPUT);
  pinMode(RelayPin3, OUTPUT);
  pinMode(RelayPin4, OUTPUT);
  pinMode(TriacPin, OUTPUT);

  pinMode(ZeroCrossPin, INPUT_PULLUP);
  pinMode(SwitchPin1, INPUT_PULLUP);
  pinMode(SwitchPin2, INPUT_PULLUP);
  pinMode(SwitchPin3, INPUT_PULLUP);
  pinMode(SwitchPin4, INPUT_PULLUP);
  pinMode(SwitchPin5, INPUT_PULLUP);
  pinMode(SwitchPin6, INPUT_PULLUP);

  digitalWrite(RelayPin1, HIGH);
  digitalWrite(RelayPin2, HIGH);
  digitalWrite(RelayPin3, HIGH);
  digitalWrite(RelayPin4, HIGH);
  digitalWrite(TriacPin, LOW);

  atmega328_16mhz_ac_phase_control.init();

  for (int i = 0; i < 6; i++)
  {
    uint8_t p = SwitchPin1;
    if(i==1) p = SwitchPin2;
    if(i==2) p = SwitchPin3;
    if(i==3) p = SwitchPin4;
    if(i==4) p = SwitchPin5;
    if(i==5) p = SwitchPin6;
    buttonLastStates[i] = digitalRead(p);
  }

  delay(500);
  eepromState();

  timer.every(10000, sendPeriodicStatus);
}

void loop()
{
  ir_remote();
  handleSerialControl();
  handleWallSwitches(); 
  timer.tick();
}
