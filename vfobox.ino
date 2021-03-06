/*

DDS code based on Pete Juliano's (N6QW) super simple DDS code
http://www.jessystems.com/Images/Arduino/AD9850_Signal_Generator.txt

with additional DDS inspiration from Rich, AD7C's project:
http://www.ad7c.com/projects/ad9850-dds-vfo/

Rotary Encoder code based on my ATmega328 port
http://blog.templaro.com/reading-a-rotary-encoder-demo-code-for-atmega328/
of Oleg Mazurov's code written for ATmega644p
https://www.circuitsathome.com/mcu/rotary-encoder-interrupt-service-routine-for-avr-micros

 
 The circuit:
 
 Power
 * nominal 12V supply to amplifier 
 * 5V to Arduino 5V pin
 * GND to Arduino GND pin
 
 Reserve For Serial Debugging
 * Leave digital #0 (RXD) and #1 (TXD) available for serial debugging
  
 Rotary Encoder
 * Rotary Encoder right tab to digital #2
 * Rotary Encoder left tab to digital #3
 * Rotary Encoder middle tab to ground
 * Rotary Switch to digital #4
 
 AD9850
 * CLK to digital #5
 * RST to digital #6
 * DAT to digital #7
 * FQ  to digital #8
 
 Memory Buttons
 * Up to four memory buttons can be added, starting at d9 & d10
   (d11 and d12 not connected in present design)
 
 RF Output Warning
 * Sense rectified RF output on digital #14 (ADC 0) WARN
 * Warning LED on digital #13
 
 I2C bus
 * Built-in SDA on digital #18 (a4)
 * Built-in SCL on digital #19 (a5)
 
 AREF is not connected; we use ATmega328 1.1V internal analog reference
 
*/

// include the library code:
#include "Wire.h"
#include "Adafruit_LiquidCrystal.h"
#include <EEPROM.h>

// set rotary encoder pins
#define ENC_RD	PIND	//encoder port read
#define A_PIN 2  // INT0 vector; digital pin 2
#define B_PIN 3  // INT1 vector; digital pin 3
#define RotEncSwPin 4 // digital pin 17 (a3)

// set up AD9850 pins
#define W_CLK 5        // digital 4
#define RESET 6        // digital 9
#define DATAPIN 7      // digital 8
#define FQ_UD 8        // digital 5

// set up output power monitoring
#define WARN_LED 13   // digital 16
#define RF_V 0        // analog  0
#define THRESHOLD_VOLTAGE 880  //range 0-1100 mV

// set up memory push buttons
#define MEM_BUTTON_START_PIN  9  // digital pin where mem buttons start
#define NUMBER_MEM_BUTTONS    2  // how many mem buttons

#define EE_ADDRESS_START      0

const long bandStart = 100000;  // start of Gnerator at 100 KHZ
const long bandEnd = 40000000; // End of Generator at 40 MHz --signal will be a bit flaky!
const long bandInit = 7150000; // where to initially set the frequency

long freq = 0 ;  // tuning frequency in Hz
long freqDelta = 10000; // how much to change the frequency by, clicking the rotary encoder will change this.

byte spinDirection = 0; // rotary encoder spun ccw (-1), cw (1), or no change (0)

long lastStatus = 0;
boolean statusDisplayed = false;
char statusLine[21];

int threshold = 0; 

// Connect via i2c, default address #1 (nothing jumpered)
Adafruit_LiquidCrystal lcd(0);

// Blank a display line
void clear_line(byte lineNumber) {
  lcd.noBlink();
  lcd.setCursor(0,lineNumber);
  for (int pos=0; pos < 20; pos++){
    lcd.print(" ");
  }
}

// Update status line
void update_status() {
  clear_line(1);
  lcd.setCursor(0,1);
  lcd.print(statusLine);
  statusDisplayed = true;
  lastStatus = millis();
}

//Wipe stale status
void wipeStaleStatus() {
  if (statusDisplayed && millis() - lastStatus > 2000) {
    clear_line(1);
    statusDisplayed = false;
  }
}

//blink the cursor to show selected frequency resolution
void blinkCursor() {
  static long lastBlinkCheck = 0;
  static byte freqCursorPosition = 4;
  
    if (millis() - lastBlinkCheck > 300) {
    freqCursorPosition = 8;
    for (long tempDelta = 10; tempDelta/freqDelta != 1; tempDelta *= 10) {
      freqCursorPosition--;
      if (freqCursorPosition == 2 || freqCursorPosition == 6) {  //hop the punctuation
        freqCursorPosition--;
      }
    }
    lcd.setCursor(freqCursorPosition,0);
    lcd.blink();
    lastBlinkCheck = millis();
  }
}

// Display the frequency...
void display_frequency(long frequency) {  
  int currentCursor;
  byte currentDigit;
  
  lcd.noBlink(); // suppress blinking after printing Mhz
  currentCursor = 0;
  for (long freqDiv = 10000000; freqDiv > 1; freqDiv /=10) {
    currentDigit = (frequency/freqDiv) %10;
    if (currentCursor == 2) {
      lcd.setCursor(currentCursor,0);
      lcd.print(".");
      currentCursor++; 
    } else if (currentCursor == 6) {
      lcd.setCursor(currentCursor,0);
      lcd.print(",");
      currentCursor++;
    } 
    lcd.setCursor(currentCursor,0);
    if (currentCursor == 0 && currentDigit == 0) {
      lcd.print(" ");
    } else {
      lcd.print(currentDigit);
    }
    currentCursor++;
  } 
  lcd.print("0 MHz");
}  

// Generate a positive pulse on 'pin'...
void pulseHigh(int pin) {
  digitalWrite(pin, HIGH); 
  digitalWrite(pin, LOW); 
}

// Calculate and send frequency code to DDS Module...
void send_frequency(long frequency) {
  int32_t sendFreq = double (frequency) * 4294967295/124997797;
  for (int b=0; b<4; b++, sendFreq>>=8) {
    shiftOut(DATAPIN, W_CLK, LSBFIRST, sendFreq & 0xFF);
  } 
  shiftOut(DATAPIN, W_CLK, LSBFIRST, 0x00);  
  pulseHigh(FQ_UD); 
}

// evaluate encoder spins
void evaluateRotary() {
/* encoder routine. Expects encoder with four state changes between detents */
/* and both pins open on detent */

  static uint8_t old_AB = 3;  //lookup table index
  static int8_t encval = 0;   //encoder value  
  static const int8_t enc_states [] PROGMEM = 
  {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};  //encoder lookup table
  /**/
  old_AB <<=2;  //remember previous state
  old_AB |= (( ENC_RD >>2 ) & 0x03 );
  encval += pgm_read_byte(&(enc_states[( old_AB & 0x0f )]));
  
  if( encval > 3 ) {  //four steps forward
    spinDirection = 1;
    encval = 0;
  }
  else if( encval < -3 ) {  //four steps backwards
   spinDirection = 2;
   encval = 0;
  } 
}

//Is there a pending encoder spin to react to? If so, which direction?
void wasRotaryEncoderSpun() {
  // change freq dependent on rotary encoder spin direction
  if(spinDirection != 0) {
    if (spinDirection == 1) {
      freq=constrain(freq-freqDelta,bandStart,bandEnd);
    } else {
      freq=constrain(freq+freqDelta,bandStart,bandEnd);
    }
    spinDirection = 0;
    display_frequency(freq); // push the frequency to LCD display
    send_frequency(freq);  // set the DDS to the new frequency  
  }
}

// Check for the click of the rotary encoder 
void wasRotaryEncoderClicked() {
  static long lastClick = 0;
  
  if (!digitalRead(RotEncSwPin) && millis() - lastClick > 200) { // push button debounce
    // if user clicks rotary encoder, change the size of the increment
    if (freqDelta == 10) {
      freqDelta = 1000000;
    } else {
      freqDelta /= 10;
    }
    lastClick = millis();
  }
}

// Light warning LED for output above a specified level
// set conservatively to warn before distortion from amp
// kicks in. 

void checkOutputLevel() {
  static long lastTrigger = 0;
  
  if(millis() - lastTrigger > 100) {
    if(analogRead(RF_V) > threshold) {
      digitalWrite(WARN_LED, HIGH);
    } else {
      digitalWrite(WARN_LED, LOW);
    }
    lastTrigger = millis();
  }
}

//Handle Memory Button Pushes
void handleMemoryButtons() {

  static boolean mem_armed[NUMBER_MEM_BUTTONS];
  static long last_mem[NUMBER_MEM_BUTTONS];
  
  for (int buttons = 0; buttons < NUMBER_MEM_BUTTONS; buttons++) {   
    if(!mem_armed[buttons] && !digitalRead(MEM_BUTTON_START_PIN+buttons) && millis() - last_mem[buttons] > 50) {
      last_mem[buttons] = millis();
      mem_armed[buttons] = true;
    }
    if(mem_armed[buttons] && digitalRead(MEM_BUTTON_START_PIN+buttons) && millis() - last_mem[buttons] > 50) {  
      //pushed, but for how long?
      lcd.setCursor(0,1);
      if(millis() - last_mem[buttons] > 500) {
      // long push = commit to memory
      EEPROM.put(EE_ADDRESS_START + buttons*sizeof(long),freq);
      strcpy(statusLine,"m");
      statusLine[1] = '0' + buttons; 
      statusLine[2] = 0; // so that next bit gets added on here
      strcat(statusLine," stored");
      update_status();
      } else {
      // short push = qsy to memory frequency
      EEPROM.get(EE_ADDRESS_START + buttons*sizeof(long),freq);
      strcpy(statusLine,"recall m"); 
      statusLine[8] = '0' + buttons; 
      statusLine[9] = 0; 
      update_status();
      send_frequency(freq);     
      display_frequency(freq);
      }
      mem_armed[buttons] = false;
      last_mem[buttons] = millis();
    }
  }
}

void setup() {
  lcd.begin(16,2);
  strcpy(statusLine,"5R8SV SIG GEN");
  update_status();
  
  // Set up DDS
  pinMode(FQ_UD, OUTPUT);
  pinMode(W_CLK, OUTPUT);
  pinMode(DATAPIN, OUTPUT);
  pinMode(RESET, OUTPUT);
  
  
  // Set up Rotary Encoder
  pinMode(A_PIN, INPUT_PULLUP);
  pinMode(B_PIN, INPUT_PULLUP);
  attachInterrupt(0, evaluateRotary, CHANGE);
  attachInterrupt(1, evaluateRotary, CHANGE);
  pinMode(RotEncSwPin, INPUT_PULLUP);
  
  // Set up Output Monitoring
  pinMode(WARN_LED, OUTPUT);
  digitalWrite(WARN_LED, LOW);
  analogReference(INTERNAL);
  threshold = 1023L * THRESHOLD_VOLTAGE / 1100;
  
  // Set up memory buttons
  for (int buttons=0; buttons < NUMBER_MEM_BUTTONS; buttons++) {
    pinMode(MEM_BUTTON_START_PIN + buttons,INPUT_PULLUP);  
  }
    
 // if nothing (reasonable) in first memory, fill mem slots with default starting freq
  EEPROM.get(EE_ADDRESS_START,freq);
  if(freq < bandStart || freq > bandEnd) {
    freq = bandInit;
    for (int buttons = 0; buttons < NUMBER_MEM_BUTTONS; buttons++) {
      EEPROM.put(EE_ADDRESS_START + buttons*sizeof(long),freq);
    }
  }
  
 // start up the DDS... 
  pulseHigh(RESET);
  pulseHigh(W_CLK);
  pulseHigh(FQ_UD); 
  // start the oscillator...
  send_frequency(freq);     
  display_frequency(freq);
}

void loop() {
  
  wasRotaryEncoderSpun();
  wasRotaryEncoderClicked();
  blinkCursor();
  handleMemoryButtons();
  checkOutputLevel();
  wipeStaleStatus();
  
}
