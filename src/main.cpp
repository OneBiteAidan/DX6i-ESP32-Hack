/*
Author: Aidan Boucher
Date: 06/14/2026
*/

#include <Arduino.h>
#include <BleGamepad.h>
#include <TFT_eSPI.h> // Pins configured in User_Setup.h, not here

// Define 4 analog pins (Strictly on ADC1 to avoid Bluetooth Conflicts)
#define LEFT_JOYSTICK_VERT  36 // Left Joystick Up/Down
#define LEFT_JOYSTICK_HORI  39 // Left Joystick Left/Right
#define RIGHT_JOYSTICK_VERT 34 // Right Joystick Up/Down
#define RIGHT_JOYSTICK_HORI 35 // Right Joystick Left/Right

// Define 8 digital pins for controller switches/buttons
const int digitalPins[8] = { 17, 18, 25, 26, 27, 32, 19, 16 };
bool lastButtonStates[8] = {HIGH}; // Default to high

// Define 3 digital pins for rotary encoder assembly
#define ENCODER_PIN_A 22
#define ENCODER_PIN_B 23
#define ENCODER_BTN   4

// HID button indicies for encoder (appended after the 8 physical buttons)
#define BTN_SCROLL_UP 9
#define BTN_SCROLL_DOWN 10
#define BTN_ENCODER_CLICK 11

bool lastEncoderBtnState = HIGH;

// Quadrature decode state
volatile uint8_t encoderState = 0;
volatile int8_t encoderDelta = 0; // +1, -1, or 0 per loop check

// Standard quadrature lookup table for a 2-bit gray code encoder
// Index: (oldA oldB newA newB) -> -1, 0, or +1
const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1,  -1, 0
};

// Software trim state. One trim offset per axis, in HID units
int trimX = 0;
int trimY = 0;
int trimZ = 0;
int trimRz = 0;
const int TRIM_STEP = 256;
const int TRIM_MAX = 4096;

// Create BLE gamepad instance w/ custom broadcast identifiers
BleGamepad bleGamepad("Spektrum DX6i BLE", "Custom RC", 100);

// Create TFT display instance
TFT_eSPI tft = TFT_eSPI();

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 2

// Reads encoder pins and updates encoderState/encoderDelta using the gray code table.
// Call this frequently (every loop interation) for reliable decoding.
void pollEncoder()
{
  uint8_t a = digitalRead(ENCODER_PIN_A);
  uint8_t b = digitalRead(ENCODER_PIN_B);
  uint8_t newState = (a << 1) | b;

  uint8_t tableIndex = (encoderState << 2) | newState;
  int8_t result = encoderTable[tableIndex];

  if (result != 0) {
    encoderDelta += result;
  }

  encoderState = newState;
}

void setup() {
  Serial.begin(115200); // For debugging diagnostics via USB
  Serial.println("Initializing DX6i Bluetooth Mod...");

  // Configure digital switch pins
  for (int i = 0; i < 8; i++) {
    // Uses internal chip resistors so switches don't float when open
    pinMode(digitalPins[i], INPUT_PULLUP);
    lastButtonStates[i] = digitalRead(digitalPins[i]);
  }

  // Configure encoder pins + integrated click button
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(ENCODER_BTN, INPUT_PULLUP);
  lastEncoderBtnState = digitalRead(ENCODER_BTN);

  // Prime initial encoder state so the first pollEncoder() call has a valid baseline
  encoderState = (digitalRead(ENCODER_PIN_A) << 1) | digitalRead(ENCODER_PIN_B);

  // Configure BLE parameters
  BleGamepadConfiguration config;
  config.setAutoReport(false); // Manually push packets for zero latency
  config.setButtonCount(8);
  config.setHatSwitchCount(0); // Disable D-pad hat switches

  // Enable 4 analog axes (X, Y, Z, Rz)
  config.setWhichAxes(true, true, true, false, false, true, false, false);
  
  // Start bluetooth servicer
  bleGamepad.begin(&config);
  Serial.println("Bluetooth Broadcasting: 'Spektrum DX6i BLE' is ready to pair!");

  // Start the TFT display
  tft.init();
  // Set the TFT display rotation in landscape mode
  tft.setRotation(3);

  // Clear the screen before writing to it
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  // Set the X and Y coordinates for center of display
  int centerX = SCREEN_WIDTH / 2;
  int centerY = SCREEN_HEIGHT / 2;

  tft.drawCentreString("Hello, world!", centerX, 30, FONT_SIZE);
}

void loop() {
  // Always poll the encoder, even if not connected, so sate doesn't drift/skip

  // Only process data if a device is connected over Bluetooth
  if (bleGamepad.isConnected()) {
    bool stateChanged = false;

    // Process digital switches
    for (int i = 0; i < 8; i++) {
      bool currentState = digitalRead(digitalPins[i]);

      // Look for any switch transitions
      if (currentState != lastButtonStates[i]) {
        lastButtonStates[i] = currentState;
        stateChanged = true;

        // "LOW" means closed to ground (Switch flipped ON)
        if (currentState == LOW) {
          bleGamepad.press(i + 1); // HID buttons are 1-indexed
        } else {
          bleGamepad.release(i + 1);
        }
      }
    }

    // Process encoder click (separate from rotation)
    bool currentEncoderBtnState = digitalRead(ENCODER_BTN);
    if (currentEncoderBtnState != lastEncoderBtnState) {
      lastEncoderBtnState = currentEncoderBtnState;
      stateChanged = true;

      if (currentEncoderBtnState == LOW) {
        bleGamepad.press(BTN_ENCODER_CLICK);
      } else {
        bleGamepad.release(BTN_ENCODER_CLICK);
      }
    }

    // Process encoder rotation as discrete scroll pulses
    // Each detent fires a brief press+release on the relevant scroll button
    if (encoderDelta > 0) {
      bleGamepad.press(BTN_SCROLL_UP);
      bleGamepad.sendReport();
      delay(5);
      bleGamepad.release(BTN_SCROLL_UP);
      encoderDelta--;
      stateChanged = true;
    } else if (encoderDelta < 0) {
      bleGamepad.press(BTN_SCROLL_DOWN);
      bleGamepad.sendReport();
      delay(5);
      bleGamepad.release(BTN_SCROLL_DOWN);
      encoderDelta++;
      stateChanged = true;
    }

    // Process gimbal potentiometers
    // Reach raw 12-bit ESP32 ADC values (0 to 4095)
    int rawLeftHori = analogRead(LEFT_JOYSTICK_HORI);
    int rawLeftVert = analogRead(LEFT_JOYSTICK_VERT);
    int rawRightHori = analogRead(RIGHT_JOYSTICK_HORI);
    int rawRightVert = analogRead(RIGHT_JOYSTICK_VERT);

    // Convert 0-4095 scale to standard HID gamepad scale (0 to 32767)
    int posX = map(rawLeftHori, 0, 4095, 0, 32767);
    int posY = map(rawLeftVert, 0, 4095, 0, 32767);
    int posZ = map(rawRightHori, 0, 4095, 0, 32767);
    int posRz = map(rawRightVert, 0, 4095, 0, 32767);

    // Map calculated pot variables directory to individual axes
    bleGamepad.setX(posX);
    bleGamepad.setY(posY);
    bleGamepad.setZ(posZ);
    bleGamepad.setRZ(posRz);

    // Push the entire snapshot frame out at once
    bleGamepad.sendReport();

    delay(10);
  }
}