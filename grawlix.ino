/*===========================================================================*/
/*             Grawlix: Vintage Speech Synthesizer for Eurorack              */
/*                by Anthony D'Angelo • http://heythats.cool                 */
/*===========================================================================*/

/* TODO:
* - Resolve Pin type issue in HardwarePWM class
* - Add encoder-based memory navigation
*/

#include "src/DaisyDuino/src/DaisyDuino.h"
using namespace daisy;

// Pin                    | In/Out  | Device, Pin | Purpose
const int cv1 = 22;       // In from   J1, sleeve    (X CV)
const int cv2 = 23;       // In from   J2, sleeve    (Y CV)
const int cv3 = 24;       // In from   J3, sleeve    (1v/Oct)
const int cv4 = 25;       // In from   J4, sleeve    (Freq)
const int rdm = 26;       // In from   n/a           (Noise Source)
const int gate1 = 27;     // In from   J5, sleeve    (Gate)
const int button1 = 28;   // In from   B1, 3         (Button)
const int sw1 = 29;       // In from   SW1, 1        (Phoneme Mode)
const int sw2 = 30;       // In from   SW1, 3        (Phoneme Mode)
const int sw3 = 31;       // In from   SW2, 1        (Advance Mode)
const int sw4 = 32;       // In from   SW2, 3        (Advance Mode)
const int enc1 = 33;      // In fron   ENC1, 1       (Memory Navigate)
const int enc2 = 34;      // In from   ENC1, 2       (Memory Navigate)
const int enc3 = 35;      // In from   ENC1, SW      (Memory Navigate)
const int vAr = 10;       // In from   SC-01, 8      (A/R, Acknowledge)
const int vStb = 11;      // Out to    SC-01, 7      (STB, Strobe)
const int vClk = 12;      // Out to    SC-01, 15     (MCX, External Clock)
const int srData = 13;    // Out to    74HC595, 14   (DS, Serial In)
const int srLatch = 14;   // Out to    74HC595, 12   (STCP Shift Clock In)
const int srClk = 15;     // Out to    74HC595, 11   (SHCP Storage Clock In)
const int audioIn = 16;   // In from   SC-01, 22     (AO = Audio Out)
const int audioOut = 18;  // Out to    J5, sleeve    (Audio Out)

// Create Daisy hardware and control objects
DaisyHardware hw;
static int num_channels;
static float sample_rate;
AnalogControl x, y, voct, user;
Switch pSw1, pSw2, aSw1, aSw2, button;
GateIn gate;
Encoder encoder;

// Set this to true if you want to log messages.
bool debug = false;

// Prints a message to the serial monitor if debug == true.
void debugPrint(String message) {
  bool* db = &debug;
  if (*db == true) {
    Serial.println(message);
  }
}

// Valid Votrax phoneme symbols per SC-01 documentation
const String validSymbols[64] = {
  "EH3", "EH2", "EH1", "PA0", "DT", "A2", "A1", "ZH",
  "AH2", "I3", "I2", "I1", "M", "N", "B", "V",
  "CH", "SH", "Z", "AW1", "NG", "AH1", "OO1", "OO",
  "L", "K", "J", "H", "G", "F", "D", "S",
  "A", "AY", "Y1", "UH3", "AH", "P", "O", "I",
  "U", "Y", "T", "R", "E", "W", "AE", "AE1",
  "AW2", "UH2", "UH1", "UH", "O2", "O1", "IU", "U1",
  "THV", "TH", "ER", "EH", "E1", "AW", "PA1", "STOP"
};

// Phoneme Codes in XY arrangement per SC-01 documentation.
const int vowelGrid[8][5] = {
  { 44, 59, 46, 51, 22 },
  { 60, 2, 47, 50, 43 },
  { 41, 1, 36, 49, 58 },
  { 34, 0, 21, 35, 24 },
  { 39, 32, 8, 38, 54 },
  { 11, 6, 61, 53, 40 },
  { 10, 5, 19, 52, 55 },
  { 9, 33, 48, 23, 45 }
};

class Grawlix {
public:
  /*
    * Determines the source of the synthesized phonemes.
    * 1 = Memory Mode:     Pick from one of 8 saved phrases. (TODO)
    * 2 = Vowel Grid Mode: Continuous formant synthesis using X/Y grid.
    * 3 = Random Mode:     Generate phrases of random length and content.
    */
  volatile int phonemeMode;

  /*
    * Determines when the next phoneme is triggered.
    * 1 = Auto Mode:   advance when SC-01 sends an "Acknowledge" bit.
    * 2 = Button Mode: advance on button press
    * 3 = Gate Mode:   advance on +5v Gate input. 
    */
  volatile int advanceMode;

  //Currently selected memory slot.
  volatile int memoryIndex = 0;

  //Updates a given mode based on the state of two switch values.
  int updateMode(int mode, bool a, bool b) {
    if (!a && !b) { mode = 1; };
    if (a && !b) { mode = 2; };
    if (!a && b) { mode = 3; };
    debugPrint("Advance Mode Updated.");
  }

  /*
    * Contains the input phonemes to be synthesized.
    */
  class Phrase {
  public:
    static const uint maxLength = 32;
    String symbols[maxLength];
    int inflections[maxLength];
    int length = 32;

    //  Populate phrase with data from a memory slot.
    void setFromMemory() {
      int slot = 1;
      // int mem = memorySlot;
      // length = mem.length;
      // for (int i = 0; i < length; i++)
      // {
      //   symbols[i] = mem.symbols[i];
      //   inflections[i] = mem.symbols[i];
      // }
      debugPrint("Loaded new phrase from memory slot #slot");
    }

    // Populate phrase of length 1 with symbol from the Vowel Grid
    int setFromGrid(float x, float y) {
      length = 1;
      int xIndex = map(x, -1, 1, 0, 7);
      int yIndex = map(y, -1, 1, 0, 4);
      symbols[0] = validSymbols[vowelGrid[xIndex][yIndex]];
      debugPrint("Grid coords #xIndex , #yIndex = " + symbols[0]);
    }

    // Populate phrase with random values.
    void setFromRandom() {
      randomSeed(analogRead(rdm));
      static int minLen = 4;
      static int maxLen = 10;
      length = random(minLen, maxLen);

      for (int i = 0; i < length; i++) {
        randomSeed(analogRead(rdm));
        int index = random(0, 63);
        symbols[i] = validSymbols[index];
        inflections[i] = random(0, 3);
      }
      debugPrint("Generated new random phrase of length " + String(length));
    }
  };

  Phrase phrase;

  /**
    * Consists of a symbol and an inflection given to the constructor,
    which first validates the given string and integer as valid inputs,
    then encodes a payload byte to send to the SC-01. See Votrax SC-01 
    documentation for more details on what the chip expects. 
    */
  class Phoneme {
  public:
    int symbolIndex = 3;      // 0 ~ 64
    int inflectionIndex = 0;  // 0 ~ 3
    byte payload = 0;         // 8 bits to send to SC-01.
    Phoneme(String symbol, int inflection) {
      for (int i = 0; i < 64; i++) {
        symbolIndex = (inflection == symbol[i]) ? i : 3;
      }
      inflectionIndex = (inflection >= 0 && inflection <= 3) ? inflection : 0;
      payload = inflectionIndex << 6;
      payload = payload | symbolIndex;
      debugPrint(
        "Phoneme" + symbol + ", "
        + String(inflectionIndex) + " = "
        + String(payload));
    }
  };

  // Handles I/O from SC-01 and related circuitry.
  class Votrax {
  public:
    bool ar;

    // Return true when AR pin goes high.
    bool arUpdate() {
      digitalRead(vAr) == HIGH ? true : false;
      debugPrint("Ar pulse received.");
    }

    // Trigger SC-01 input register latch.
    void strobe() {
      static int strobeTime = 150;
      digitalWrite(vStb, HIGH);
      // @todo How can this be acheived:
      //  - without a delay()
      //  - based on divs of the SC-01 ext clock
      delay(strobeTime);
      digitalWrite(vStb, LOW);
      debugPrint("SC-01 Input register updated");
    }

    // Write a byte to the shift register feeding the SC-01
    void shiftUpdate(byte data) {
      shiftOut(srData, srClk, MSBFIRST, data);
      digitalWrite(srLatch, HIGH);
      digitalWrite(srLatch, LOW);
    }
  };

  Votrax sc01;

  //  Wait until it's ok to advance to next phoneme
  void advListen() {
    volatile bool advance;
    updateMode(advanceMode, aSw1.Pressed(), aSw2.Pressed());
    switch (advanceMode) {
      case 1:
        debugPrint("Advance Mode: Auto Mode");
        advance = sc01.arUpdate();
      case 2:
        debugPrint("Advance Mode: Button Mode");
        advance = sc01.arUpdate() && button.Pressed();
      case 3:
        debugPrint("Advance Mode: Gate Mode");
        advance = sc01.arUpdate() && gate.State();
    }
    debugPrint("Waiting to Advance...");
    while (!advance) {}
  }

  // Outputs a given phrase.
  void say(Phrase phrase) {
    debugPrint("Synthesizing phonemes...");
    for (int i = 0; i < phrase.length; i++) {
      updateMode(advanceMode, aSw1.Pressed(), aSw2.Pressed());
      Phoneme p(phrase.symbols[i], phrase.inflections[i]);
      debugPrint("Inflection: " + String(p.inflectionIndex));
      sc01.shiftUpdate(p.payload);
      advListen();
      sc01.strobe();
    }
  }

  //Main loop.
  void main() {
    updateMode(phonemeMode, pSw1.Pressed(), pSw2.Pressed());
    switch (phonemeMode) {
      case 1:  // Memory Mode
        debugPrint("Phoneme Mode: Memory Mode");
        phrase.setFromMemory();
      case 2:  // XY Mode
        debugPrint("Phoneme Mode: Grid Mode");
        phrase.setFromGrid(x.Process(), y.Process());
      case 3:  // Random Mode
        debugPrint("Phoneme Mode: Random Mode");
        phrase.setFromRandom();
    };
    say(phrase);
    while (!button.Pressed()) {}
  }
};

/*
* Generates a square wave whose frequency is set based on
* timer/counter divisions from the CPU clock, and routes 
* the output to a given pin. 
*/
class HardwarePWM {
  public:
    uint32_t prescaler          = 1;
    uint32_t system_clock_rate  = System::GetPClk2Freq() / prescaler;
    uint32_t system_tick_period = 1/system_clock_rate;
    uint32_t default_period     = 556; //* Center freq of SC-01
    TimerHandle timer;
    TimChannel pwm;

    void Init()
    {
      // Configure timer
      TimerHandle::Config tim_cfg;
      tim_cfg.periph = TimerHandle::Config::Peripheral::TIM_3;
      tim_cfg.dir = TimerHandle::Config::CounterDir::UP;
      timer.Init(tim_cfg);

      // Initialize timer
      timer.SetPrescaler(prescaler - 1); /**< ps=0 is divide by 1 and so on.*/
      timer.SetPeriod(default_period);
      timer.Start();
      
      // Configure hardware pwm
      TimChannel::Config chn_cfg;
      chn_cfg.tim = &timer;
      chn_cfg.chn = TimChannel::Config::Channel::ONE;
      chn_cfg.mode = TimChannel::Config::Mode::PWM_GENERATION;

      // Initialize hardware pwm
      pwm.Init(chn_cfg);
      pwm.SetPwm(default_period/2); //*<50% duty cycle>
      pwm.Start();
    }

    //* Set hardware pwm frequency and duty cycle. */
    void setFreq(uint32_t target_freq_hz, uint32_t duty_cycle)
    {
      uint32_t target_freq_period = 1/target_freq_hz;
      uint32_t target_freq_ticks = target_freq_period / system_tick_period;
      pwm.SetPwm(target_freq_ticks/2); //* 50% Duty Cycle
      timer.SetPeriod(target_freq_ticks);
    }
};

HardwarePWM sc01Clock;

Grawlix g;

void setup() {
  // Initialize Daisy hardware.
  hw = DAISY.init(DAISY_SEED, AUDIO_SR_48K);
  num_channels = hw.num_channels;
  sample_rate = DAISY.get_samplerate();
  debugPrint("G R A W L I X  by heythats.cool");
  int updateRate = 1000;

  // Initialize Daisy control objects.
  gate.Init(gate1, INPUT, false);
  encoder.Init(updateRate, enc1, enc2, enc3, INPUT, INPUT, INPUT);

  AnalogControl controls[4] = { x, y, voct, user };
  int cvPins[4] = { cv1, cv2, cv3, cv4 };
  for (int i = 0; i < 4; i++) { controls[i].InitBipolarCv(cvPins[i], updateRate); }

  Switch switches[5] = { pSw1, pSw2, pSw1, pSw2, button };
  int switchPins[5] = { sw1, sw2, sw3, sw4, button1 };
  for (int i = 0; i < 5; i++) { switches[i].Init(1000, false, switchPins[i], INPUT); }

  // Initialize Votrax data i/o pins.
  pinMode(vAr, INPUT);
  int outPins[6] = { vStb, vClk, srData, srLatch, srClk };
  for (int i = 0; i < 6; i++) { pinMode(i, OUTPUT); }

  // Initialize hardware pwm
  sc01Clock.Init();
}

void loop() {
  g.main();
}
