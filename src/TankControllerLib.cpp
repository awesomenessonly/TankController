#include "TankControllerLib.h"

#include "Devices/DateTime_TC.h"
#include "Devices/EEPROM_TC.h"
#include "Devices/Ethernet_TC.h"
#include "Devices/Keypad_TC.h"
#include "Devices/LiquidCrystal_TC.h"
#include "Devices/PHControl.h"
#include "Devices/PHProbe.h"
#include "Devices/PID_TC.h"
#include "Devices/PushingBox.h"
#include "Devices/SD_TC.h"
#include "Devices/Serial_TC.h"
#include "Devices/TempProbe_TC.h"
#include "Devices/TemperatureControl.h"
#include "TC_util.h"
#include "UIState/MainMenu.h"
#include "UIState/UIState.h"

const char TANK_CONTROLLER_VERSION[] = "21.06.1";

// ------------ Class Methods ------------
/**
 * static variable to hold singleton
 */
TankControllerLib *TankControllerLib::_instance = nullptr;

/**
 * static function to return singleton
 */
TankControllerLib *TankControllerLib::instance() {
  if (!_instance) {
    _instance = new TankControllerLib;
  }
  return _instance;
}

// ------------ Instance Methods ------------
/**
 * Constructor
 */
TankControllerLib::TankControllerLib() {
  serial("TankControllerLib::TankControllerLib() - version %s", TANK_CONTROLLER_VERSION);
  assert(!_instance);
  // ensure we have instances
  SD_TC::instance();
  EEPROM_TC::instance();
  Keypad_TC::instance();
  LiquidCrystal_TC::instance();
  DateTime_TC::rtc();
  Ethernet_TC::instance();
  TempProbe_TC::instance();
  TemperatureControl::instance();
  PHProbe::instance();
  PHControl::instance();
  state = new MainMenu(this);
  pinMode(LED_BUILTIN, OUTPUT);
}

/**
 * Destructor
 */
TankControllerLib::~TankControllerLib() {
  if (state) {
    delete state;
    state = nullptr;
  }
  if (nextState) {
    delete nextState;
    nextState = nullptr;
  }
}

/**
 * Blink the on-board LED to let us know that loop() is being called
 *
 */
void TankControllerLib::blink() {
  if (millis() / 1000 % 2) {
    digitalWrite(LED_BUILTIN, LOW);  // turn the LED off by making the voltage LOW
  } else {
    digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
  }
}

/**
 * Is the current UIState one that should disable controls?
 * We don't want to turn on the heat/chill if the temperature probe is out of the tank!
 */
bool TankControllerLib::isInCalibration() {
  return state->isInCalibration();
}

/**
 * Private member function called by loop
 * Handles keypresses
 */
void TankControllerLib::handleUI() {
  COUT("TankControllerLib::handleUI() - " << state->name());
  char key = Keypad_TC::instance()->getKey();
  if (key == NO_KEY) {
    if (!lastKeypadTime) {
      // we have already reached an idle state, so don't do other checks
    } else if (isInCalibration()) {
      // we are in calibration, so don't return to main menu
    } else if (nextState) {
      // we already have a next state teed-up, do don't try to return to main menu
    } else if (millis() - lastKeypadTime > IDLE_TIMEOUT) {
      // time since last keypress exceeds the idle timeout, so return to main menu
      setNextState((UIState *)new MainMenu(this));
      lastKeypadTime = 0;  // so we don't do this until another keypress!
    }
  } else {
    serial("Keypad input: %c", key);
    COUT("TankControllerLib::handleUI() - " << state->name() << "::handleKey(" << key << ")");
    state->handleKey(key);
    lastKeypadTime = millis();
  }
  updateState();
  COUT("TankControllerLib::handleUI() - " << state->name() << "::loop()");
  state->loop();
}

/**
 * This is one of two public instance functions.
 * It is called repeatedly while the board is on.
 * (It appears to be called about once every 15 ms.)
 */
void TankControllerLib::loop() {
  blink();                         // blink the on-board LED to show that we are running
  handleUI();                      // look at keypad, update LCD
  updateControls();                // turn CO2 and temperature controls on or off
  writeDataToSD();                 // record current state to data log
  PushingBox::instance()->loop();  // write data to Google Sheets
}

/**
 * This public instance function is called when there is data on the serial port(0).
 */
void TankControllerLib::serialEvent() {
}

/**
 * This public instance function is called when there is data on the serial port(1).
 * This the Atlas EZO pH circuit probe.
 */
void TankControllerLib::serialEvent1() {
  PHProbe::instance()->serialEvent1();
}

/**
 * Set the next state
 */
void TankControllerLib::setNextState(UIState *newState, bool update) {
  COUT("TankControllerLib::setNextState() from " << (nextState ? nextState->name() : "nullptr") << " to "
                                                 << newState->name());
  assert(nextState == nullptr);
  nextState = newState;
  if (update) {
    this->updateState();
  }
}

/**
 * This is one of two public instance functions.
 * Here we do any one-time startup initialization.
 */
void TankControllerLib::setup() {
  serial("TankControllerLib::setup()");
  SD_TC::instance()->printRootDirectory();
}

/**
 * Public member function used to get the current state name.
 * This is primarily used by testing.
 */
const char *TankControllerLib::stateName() {
  return state->name();
}

/**
 * Private member function called by loop to update solonoids
 */
void TankControllerLib::updateControls() {
  // update TemperatureControl
  TemperatureControl::instance()->updateControl(TempProbe_TC::instance()->getRunningAverage());
  // update PHControl
  PHControl::instance()->updateControl(PHProbe::instance()->getPh());
}

/**
 * Private member function called by UIState subclasses
 * Only updates if a new state is available to switch to
 */
void TankControllerLib::updateState() {
  if (nextState) {
    COUT("TankControllerLib::updateState() to " << nextState->name());
    assert(state != nextState);
    delete state;
    state = nextState;
    nextState = nullptr;
    state->start();
  }
}

/**
 * What is the current version?
 */
const char *TankControllerLib::version() {
  serial("TankControllerLib::version() = %s", TANK_CONTROLLER_VERSION);
  return TANK_CONTROLLER_VERSION;
}

/**
 * once per second write the current data to the SD card
 */
void TankControllerLib::writeDataToSD() {
  static uint32_t nextWriteTime = 0;
  char currentTemp[10];
  char currentPh[10];
  if (isInCalibration()) {
    snprintf(currentTemp, sizeof(currentTemp), "C");
    snprintf(currentPh, sizeof(currentPh), "C");
  } else {
    snprintf(currentTemp, sizeof(currentTemp), "%4.2f", (float)TempProbe_TC::instance()->getRunningAverage());
    snprintf(currentPh, sizeof(currentPh), "%5.3f", (float)PHProbe::instance()->getPh());
  }
  static const char header[] = "time,tankid,temp,temp setpoint,pH,pH setpoint,onTime,Kp,Ki,Kd";
  static const char format[] =
      "%02i/%02i/%4i %02i:%02i:%02i, %3i, %s, %4.2f, %s, %5.3f, %4i, %8.1f, %8.1f, %8.1f";
  uint32_t msNow = millis();
  COUT("nextWriteTime: " << nextWriteTime << "; now = " << msNow);
  if (nextWriteTime <= msNow) {
    char buffer[128];
    DateTime_TC dtNow = DateTime_TC::now();
    PID_TC *pPID = PID_TC::instance();
    uint16_t tankId = EEPROM_TC::instance()->getTankID();
    snprintf(buffer, sizeof(buffer), format, (uint16_t)dtNow.month(), (uint16_t)dtNow.day(), (uint16_t)dtNow.year(),
             (uint16_t)dtNow.hour(), (uint16_t)dtNow.minute(), (uint16_t)dtNow.second(), (uint16_t)tankId,
             currentTemp,
             (float)TemperatureControl::instance()->getTargetTemperature(), currentPh,
             (float)PHControl::instance()->getTargetPh(), (uint16_t)(millis() / 1000), (float)pPID->getKp(),
             (float)pPID->getKi(), (float)pPID->getKd());
    SD_TC::instance()->appendData(header, buffer);
    nextWriteTime = msNow / 1000 * 1000 + 1000;  // round up to next second
    COUT(buffer);
  }
}
