#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "HX711.h"
#include <RotaryEncoder.h> 
#include <AccelStepper.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

// ==========================================
// 📌 PIN DEFINITIONS
// ==========================================
#define LOADCELL_DOUT_PIN 4 
#define LOADCELL_SCK_PIN 5 
#define ENCODER_SW 6  
#define ENCODER_CLK 12
#define ENCODER_DT 13
#define OLED1_PWR 7 
#define OLED1_SDA 8
#define OLED1_SCL 9 
#define OLED2_SDA 10
#define OLED2_SCL 11
#define IN1 15
#define IN2 16
#define IN3 17
#define IN4 18

// ==========================================
// 🔵 BLUETOOTH UUIDs (Standard BLE UUIDs)
// ==========================================
// Using standard 128-bit UUIDs for compatibility
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charCommandUUID("beb5483e-36e1-4688-b7f5-ea07361b26a8");
static BLEUUID charWeightUUID("a3c87500-8ed3-4bdf-9035-1f3da3229bb3");

// BLE Server pointers
BLEServer *pServer = NULL;
BLECharacteristic *pCommandCharacteristic = NULL;
BLECharacteristic *pWeightCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Command parsing variables
String receivedCommand = "";
bool commandReceived = false;
int webTargetWhole = 10;
int webTargetDecimal = 0;
bool webDispenseRequested = false;

// ==========================================
// 🔵 BLUETOOTH CALLBACKS
// ==========================================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Bluetooth device connected");
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("Bluetooth device disconnected");
      // Restart advertising to allow reconnection
      BLEDevice::startAdvertising();
    }
};

class MyCommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      if (value.length() > 0) {
        Serial.print("Received command: ");
        Serial.println(value.c_str());
        
        // Parse JSON command
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, value.c_str());
        
        if (!error) {
          const char* action = doc["action"];
          if (action && strcmp(action, "dispense") == 0) {
            // Extract target weight from flavors (use sweet as example, or sum all)
            float totalFlavor = 0;
            if (doc.containsKey("flavors")) {
              totalFlavor += doc["flavors"]["spicy"] | 0;
              totalFlavor += doc["flavors"]["salty"] | 0;
              totalFlavor += doc["flavors"]["sweet"] | 0;
              totalFlavor += doc["flavors"]["umami"] | 0;
            }
            
            // Map flavor total (0-400) to weight (0-40g)
            int targetWeight = (int)(totalFlavor / 10.0);
            if (targetWeight < 1) targetWeight = 10; // Default
            
            webTargetWhole = targetWeight;
            webTargetDecimal = 0;
            webDispenseRequested = true;
            commandReceived = true;
            
            Serial.print("Parsed dispense command - Target: ");
            Serial.print(targetWeight);
            Serial.println("g");
          }
        } else {
          Serial.print("JSON parse error: ");
          Serial.println(error.c_str());
        }
      }
    }
};

// ==========================================
// 📦 OBJECTS & CONSTRUCTORS (THE SWAP)
// ==========================================
// Screen 1: Software I2C (Fewer pixels, handles SW bit-banging easily)
U8G2_SSD1306_128X32_UNIVISION_F_SW_I2C screen1(U8G2_R0, /* clock=*/ OLED1_SCL, /* data=*/ OLED1_SDA, /* reset=*/ U8X8_PIN_NONE);

// Screen 2: Primary Hardware I2C (Max speed for animations & smooth numbers)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C screen2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

HX711 scale;
RotaryEncoder encoder(ENCODER_CLK, ENCODER_DT, RotaryEncoder::LatchMode::TWO03);
AccelStepper stepper(AccelStepper::FULL4WIRE, IN1, IN3, IN2, IN4);

// ==========================================
// ⚙️ GLOBAL SYSTEM VARIABLES
// ==========================================
float calibration_factor = 2280.0; 
float currentWeight = 0.0;
bool isOunces = false;

// Screen 2 Variables
int targetWhole = 10;
int targetDecimal = 0;
float getTargetWeight() { return targetWhole + (targetDecimal * 0.1); }

// ==========================================
// 🧠 STATE MACHINES
// ==========================================
enum ActiveScreen { SCREEN_1_SETTINGS, SCREEN_2_DISPENSE };
ActiveScreen activeScreen = SCREEN_2_DISPENSE; 

enum S1_State { S1_MENU, S1_ZEROING, S1_ZERO_SUCCESS, S1_CALIBRATING };
S1_State s1State = S1_MENU;
int s1MenuIndex = 0; 

enum S2_State { S2_HOVER, S2_EDIT_WHOLE, S2_EDIT_DEC, S2_CONFIRM, S2_DISPENSING, S2_RETRACTING, S2_DONE };
S2_State s2State = S2_HOVER;
int s2HoverIndex = 0; 
int s2ConfirmIndex = 0; 

// ==========================================
// ⚡ BUTTON TIMING & INTERRUPTS
// ==========================================
long oldEncoderPos = 0;
unsigned long buttonPressTime = 0;
bool buttonHeldTriggered = false;
bool isButtonPressed = false;
unsigned long stateTimer = 0; 

void IRAM_ATTR checkPosition() { encoder.tick(); }

// ==========================================
// 🚀 SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  
  // Initialize Bluetooth
  BLEDevice::init("FlavorStation-S3");
  
  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  // Create the BLE Service
  BLEService *pService = pServer->createService(serviceUUID);
  
  // Create Command Characteristic (for receiving commands from web app)
  pCommandCharacteristic = pService->createCharacteristic(
                      charCommandUUID,
                      BLECharacteristic::PROPERTY_WRITE | 
                      BLECharacteristic::PROPERTY_WRITE_NR
                    );
  pCommandCharacteristic->setCallbacks(new MyCommandCallbacks());
  
  // Create Weight Characteristic (for sending weight to web app)
  pWeightCharacteristic = pService->createCharacteristic(
                      charWeightUUID,
                      BLECharacteristic::PROPERTY_READ | 
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pWeightCharacteristic->addDescriptor(new BLE2902());
  
  // Start the service
  pService->start();
  
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(serviceUUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("Bluetooth ready. Waiting for connection...");

  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT), checkPosition, CHANGE);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  stepper.setMaxSpeed(500.0);    // Drastically higher top speed limit
  stepper.setAcceleration(300.0);

  // Power cycle Screen 1
  pinMode(OLED1_PWR, OUTPUT);
  digitalWrite(OLED1_PWR, LOW); 
  delay(200); 
  digitalWrite(OLED1_PWR, HIGH); 
  delay(100);

  // Map HARDWARE I2C entirely to Screen 2
  Wire.begin(OLED2_SDA, OLED2_SCL);
  Wire.setClock(400000); // 400kHz Max Speed

  // Start screens
  screen1.begin();
  // screen2.setI2CAddress(0x3D * 2); // Uncomment if Screen 2 is black!
  screen2.begin();

  screen1.clearBuffer(); screen1.setFont(u8g2_font_ncenB08_tr); screen1.drawStr(5, 20, "Booting Scale..."); screen1.sendBuffer();
  screen2.clearBuffer(); screen2.setFont(u8g2_font_ncenB10_tr); screen2.drawStr(10, 35, "System Starting..."); screen2.sendBuffer();
  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor); 
  scale.tare(15); 
}

// ==========================================
// 🔄 MAIN LOOP
// ==========================================
void loop() {
  // Handle Bluetooth disconnection/reconnection events
  if (deviceConnected != oldDeviceConnected) {
    if (deviceConnected) {
      Serial.println("Client connected");
    } else {
      Serial.println("Client disconnected");
    }
    oldDeviceConnected = deviceConnected;
  }
  
  // Send weight update via BLE if connected (throttled to avoid flooding)
  static unsigned long lastWeightUpdate = 0;
  if (deviceConnected && pWeightCharacteristic && millis() - lastWeightUpdate > 500) {
    char weightStr[20];
    sprintf(weightStr, "{\"w\":%.1f}", currentWeight);
    pWeightCharacteristic->setValue(weightStr);
    pWeightCharacteristic->notify();
    lastWeightUpdate = millis();
  }

  // 1. FAST TASKS: These must run thousands of times per second
  handleButton();
  handleEncoder();
  readScale();
  
  // Check for web dispense command and update target if needed
  if (webDispenseRequested && s2State == S2_HOVER) {
    targetWhole = webTargetWhole;
    targetDecimal = webTargetDecimal;
    s2State = S2_DISPENSING; // Go directly to dispensing
    webDispenseRequested = false;
    Serial.println("Web command: Starting dispensing immediately");
  }
  
  runMotorLogic();

  // 2. SLOW TASKS: Cap the screen refresh rate to fix the motor CPU choke!
  static unsigned long lastScreenUpdate = 0;
  
  // Add the RETRACTING state to the speed limits so it doesn't choke the CPU
  int refreshRate = (s2State == S2_DISPENSING || s2State == S2_RETRACTING) ? 500 : 100; 

  if (millis() - lastScreenUpdate > refreshRate) {
    
    // Pause Screen 1 during BOTH dispensing and retracting
    if (s2State != S2_DISPENSING && s2State != S2_RETRACTING) {
      drawScreen1(); 
    }
    drawScreen2();
    lastScreenUpdate = millis();
  } // <-- Closes the 'if' statement
} // <-- CRITICAL: Closes the 'loop' function. If this is missing, the next function will crash!

// ==========================================
// 🎛️ INPUT HANDLING
// ==========================================
void handleButton() {
  bool currentButtonState = (digitalRead(ENCODER_SW) == LOW);
  
  if (currentButtonState && !isButtonPressed) {
    isButtonPressed = true;
    buttonPressTime = millis();
    buttonHeldTriggered = false;
  } 
  else if (currentButtonState && isButtonPressed) {
    // LONG PRESS (2 Seconds) -> Switch Screens ONLY
    if (millis() - buttonPressTime > 2000 && !buttonHeldTriggered) {
      buttonHeldTriggered = true;
      activeScreen = (activeScreen == SCREEN_1_SETTINGS) ? SCREEN_2_DISPENSE : SCREEN_1_SETTINGS;
      s1State = S1_MENU; 
      if(s2State != S2_DISPENSING) s2State = S2_HOVER; 
    }
  } 
  else if (!currentButtonState && isButtonPressed) {
    isButtonPressed = false;
    // SHORT CLICK -> Enter Menus / Edit Modes / Confirm
    if (!buttonHeldTriggered && millis() - buttonPressTime > 50) {
      executeShortClick();
    }
  }
}

void executeShortClick() {
  if (activeScreen == SCREEN_1_SETTINGS) {
    if (s1State == S1_MENU) {
      if (s1MenuIndex == 0) { 
        s1State = S1_ZEROING; drawScreen1(); scale.tare(10); s1State = S1_ZERO_SUCCESS; stateTimer = millis();
      } else if (s1MenuIndex == 1) { s1State = S1_CALIBRATING; } 
      else if (s1MenuIndex == 2) { isOunces = !isOunces; }
    } else if (s1State == S1_CALIBRATING) {
      s1State = S1_MENU; 
    }
  } 
  else if (activeScreen == SCREEN_2_DISPENSE) {
    if (s2State == S2_HOVER) {
      if (s2HoverIndex == 0) s2State = S2_EDIT_WHOLE;
      else if (s2HoverIndex == 1) s2State = S2_EDIT_DEC;
      else if (s2HoverIndex == 2) s2State = S2_CONFIRM; 
    } 
    else if (s2State == S2_EDIT_WHOLE || s2State == S2_EDIT_DEC) {
      s2State = S2_HOVER; // Save and go back to hover
    }
    else if (s2State == S2_CONFIRM) {
      if (s2ConfirmIndex == 1) {
        s2State = S2_DISPENSING;
      }
      else s2State = S2_HOVER; 
    }
  }
}

void handleEncoder() {
  long newPos = encoder.getPosition();
  if (newPos != oldEncoderPos) {
    int dir = (int)encoder.getDirection(); 
    oldEncoderPos = newPos;

    if (activeScreen == SCREEN_1_SETTINGS) {
      if (s1State == S1_MENU) {
        s1MenuIndex += dir;
        if (s1MenuIndex < 0) s1MenuIndex = 0;
        if (s1MenuIndex > 2) s1MenuIndex = 2;
      } else if (s1State == S1_CALIBRATING) {
        calibration_factor += (dir * 10.0);
        scale.set_scale(calibration_factor);
      }
    } 
    else if (activeScreen == SCREEN_2_DISPENSE) {
      if (s2State == S2_HOVER) {
        s2HoverIndex += dir;
        if (s2HoverIndex < 0) s2HoverIndex = 0;
        if (s2HoverIndex > 2) s2HoverIndex = 2;
      } else if (s2State == S2_EDIT_WHOLE) {
        targetWhole += dir;
        if (targetWhole < 0) targetWhole = 0;
        if (targetWhole > 99) targetWhole = 99;
      } else if (s2State == S2_EDIT_DEC) {
        targetDecimal += dir;
        if (targetDecimal < 0) targetDecimal = 0;
        if (targetDecimal > 9) targetDecimal = 9;
      } else if (s2State == S2_CONFIRM) {
        s2ConfirmIndex += dir;
        if (s2ConfirmIndex < 0) s2ConfirmIndex = 0;
        if (s2ConfirmIndex > 1) s2ConfirmIndex = 1;
      }
    }
  }
}

// ==========================================
// ⚖️ SENSOR & MOTOR LOGIC
// ==========================================
void readScale() {
  if (scale.is_ready()) {
    float rawWeight = scale.get_units(1); 
    if (rawWeight > -0.3 && rawWeight < 0.3) rawWeight = 0.0;
    currentWeight = isOunces ? (rawWeight * 0.035274) : rawWeight;
  }
}

void runMotorLogic() {
  if (s2State == S2_DISPENSING) {
    if (currentWeight < getTargetWeight()) {
      stepper.setSpeed(500); 
      stepper.runSpeed();
    } else {
      // 🔄 Target reached! Move to retraction instead of finishing.
      s2State = S2_RETRACTING; 
      stateTimer = millis();   // Start the timer for the reverse motion
    }
  } 
  else if (s2State == S2_RETRACTING) {
    // ⏱️ Run backward for 800 milliseconds (0.8 seconds)
    if (millis() - stateTimer < 800) { 
      stepper.setSpeed(-500); // Negative speed spins the motor backward!
      stepper.runSpeed();
    } else {
      // 🛑 Time is up! Turn off the motor and show DONE.
      s2State = S2_DONE;
      stateTimer = millis();
      digitalWrite(IN1, LOW); digitalWrite(IN2, LOW); digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    }
  }
}

// ==========================================
// 📺 UI RENDERING
// ==========================================
void drawScreen1() {
  screen1.clearBuffer();
  if (activeScreen == SCREEN_1_SETTINGS) screen1.drawFrame(0, 0, 128, 32);

  screen1.setFont(u8g2_font_ncenB08_tr);

  if (s1State == S1_MENU) {
    if (s1MenuIndex == 0) screen1.drawStr(10, 12, "> 1. Zero Scale"); else screen1.drawStr(10, 12, "  1. Zero Scale");
    if (s1MenuIndex == 1) screen1.drawStr(10, 22, "> 2. Calibrate"); else screen1.drawStr(10, 22, "  2. Calibrate");
    
    char unitStr[20]; sprintf(unitStr, "  3. Units: %s", isOunces ? "oz" : "g");
    if (s1MenuIndex == 2) unitStr[0] = '>'; 
    screen1.drawStr(10, 32, unitStr);
  } 
  else if (s1State == S1_ZEROING) screen1.drawStr(20, 22, "ZEROING...");
  else if (s1State == S1_ZERO_SUCCESS) {
    screen1.drawStr(10, 20, "[ SUCCESS ]");
    if (millis() - stateTimer > 1500) s1State = S1_MENU; 
  }
  else if (s1State == S1_CALIBRATING) {
    screen1.setCursor(10, 15); screen1.print("Factor: "); screen1.print(calibration_factor, 0);
    screen1.setCursor(10, 28); screen1.print("Live: "); screen1.print(currentWeight, 1);
  }
  screen1.sendBuffer();
}

void drawScreen2() {
  screen2.clearBuffer();
  if (activeScreen == SCREEN_2_DISPENSE) screen2.drawFrame(0, 0, 128, 64);

  if (s2State == S2_HOVER || s2State == S2_EDIT_WHOLE || s2State == S2_EDIT_DEC) {
    screen2.setFont(u8g2_font_ncenB10_tr);
    screen2.drawStr(10, 20, "Target:");

    char wholeStr[5], decStr[5];
    sprintf(wholeStr, "%02d", targetWhole);
    sprintf(decStr, ".%d", targetDecimal); 

    // 1. Draw the Target Numbers
    screen2.setFont(u8g2_font_ncenB14_tr);
    screen2.drawStr(70, 22, wholeStr);
    screen2.drawStr(92, 22, decStr);

    // 2. Draw the Unit (g or oz) right next to the numbers
    screen2.setFont(u8g2_font_ncenB10_tr); 
    screen2.drawStr(114, 22, isOunces ? "oz" : "g"); 

    // 3. EDIT UI: Solid box around the number being edited
    if (s2State == S2_EDIT_WHOLE) screen2.drawFrame(68, 5, 25, 22);
    if (s2State == S2_EDIT_DEC) screen2.drawFrame(93, 5, 18, 22);
    
    // HOVER UI: Lines under the numbers
    if (s2State == S2_HOVER) {
      if (s2HoverIndex == 0) screen2.drawLine(70, 24, 90, 24); 
      if (s2HoverIndex == 1) screen2.drawLine(95, 24, 110, 24); 
    }

    screen2.setFont(u8g2_font_ncenB10_tr);
    if (s2HoverIndex == 2 && s2State == S2_HOVER) {
      screen2.drawBox(14, 40, 100, 18);
      screen2.setDrawColor(0); screen2.drawStr(22, 54, "DISPENSE"); screen2.setDrawColor(1); 
    } else {
      screen2.drawFrame(14, 40, 100, 18); screen2.drawStr(22, 54, "DISPENSE");
    }
  } 
  else if (s2State == S2_CONFIRM) {
    screen2.setFont(u8g2_font_ncenB08_tr); screen2.setCursor(15, 20);
    screen2.print("Dispense "); screen2.print(getTargetWeight(), 1); screen2.print(" ?");
    if (s2ConfirmIndex == 0) screen2.drawStr(30, 45, "> NO      YES");
    else screen2.drawStr(30, 45, "  NO    > YES");
  }
  else if (s2State == S2_DISPENSING || s2State == S2_RETRACTING) {
    screen2.setFont(u8g2_font_ncenB10_tr); 
    if (s2State == S2_DISPENSING) screen2.drawStr(15, 20, "DISPENSING...");
    else screen2.drawStr(15, 20, "RETRACTING..."); // Show reverse status
    screen2.setFont(u8g2_font_ncenB18_tr); screen2.setCursor(20, 50); screen2.print(currentWeight, 1);
    screen2.setFont(u8g2_font_ncenB10_tr); screen2.print(isOunces ? " oz" : " g");
  }
  else if (s2State == S2_DONE) {
    screen2.setFont(u8g2_font_ncenB14_tr); screen2.drawStr(15, 35, "FINISHED!");
    if (millis() - stateTimer > 2500) s2State = S2_HOVER; 
  }
  screen2.sendBuffer();
}
