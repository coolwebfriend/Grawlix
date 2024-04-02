/*===========================================================================*/
/*             Grawlix: Vintage Speech Synthesizer for Eurorack              */
/*                by Anthony D'Angelo • http://heythats.cool                 */
/*===========================================================================*/

/* TODO:
* - Add encoder-based memory navigation
*/

#include "DaisyDuino.h" // Version 1.6.0
using namespace daisy;

// Create Daisy hardware and control objects
DaisyHardware patch;
static int num_channels;
static float sample_rate;
static float update_rate;
AnalogControl x, y, voct, freq, rdm;
Switch pSw1, pSw2, aSw1, aSw2, button1;
GateIn gate;
Encoder encoder;

// Pin                    | In/Out  | Device, Pin | Purpose
const int vAr = D2;       // In from   SC-01, 8      (A/R, Acknowledge)
const int vStb = D3;      // Out to    SC-01, 7      (STB, Strobe)
const int vClk = D1;      // Out to    SC-01, 15     (MCX, External Clock)
const int srData = D6;    // Out to    74HC595, 14   (DS, Serial In)
const int srLatch = D4;   // Out to    74HC595, 12   (STCP Shift Clock In)
const int srClk = D5;     // Out to    74HC595, 11   (SHCP Storage Clock In)

// Set this to true if you want to log messages.
bool debug = false;

// Prints a message to the serial monitor if debug == true.
void debugPrint(String message) 
{
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


class Grawlix 
{
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
  int updateMode(int mode, bool a, bool b) 
  {
    if (!a && !b) { mode = 1; };
    if (a && !b) { mode = 2; };
    if (!a && b) { mode = 3; };
    debugPrint("Advance Mode Updated.");
  }

  /*
  * Contains the input phonemes to be synthesized.
  */
  class Phrase 
  {
  public:
    static const uint maxLength = 32;
    String symbols[maxLength];
    int inflections[maxLength];
    int length = 32;

    //  Populate phrase with data from a memory slot.
    void setFromMemory() 
    {
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
    int setFromGrid(float x, float y) 
    {
      length = 1;
      int xIndex = map(x, 0, 10^16-1, 0, 7);
      int yIndex = map(y, 0, 10^16-1, 0, 4);
      symbols[0] = validSymbols[vowelGrid[xIndex][yIndex]];
      debugPrint("Grid coords " + String(xIndex) + ", " + 
                  String(yIndex) + " = " + symbols[0]);
    }

    // Populate phrase with random values.
    void setFromRandom() 
    {
      randomSeed(rdm.Value());
      static int minLen = 4;
      static int maxLen = 10;
      length = random(minLen, maxLen);

      for (int i = 0; i < length; i++) 
      {
        randomSeed(rdm.Value());
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
  class Phoneme 
  {
  public:
    int symbolIndex = 3;      // 0 ~ 64
    int inflectionIndex = 0;  // 0 ~ 3
    byte payload = 0;         // 8 bits to send to SC-01.
    Phoneme(String symbol, int inflection) 
    {
      for (int i = 0; i < 64; i++) {
        symbolIndex = (inflection == symbol[i]) ? i : 3;
      }
      inflectionIndex = (inflection >= 0 && inflection <= 3) ? inflection : 0;
      payload = inflectionIndex << 6;
      payload = payload | symbolIndex;
      debugPrint(
                "Phoneme" + symbol + ", "
                + String(inflectionIndex) + " = "
                + String(payload)
                );
    }
  };

  // Handles I/O from SC-01 and related circuitry.
  class Votrax {
  public:
    bool ar;

    // Return true when AR pin goes high.
    bool arUpdate() 
    {
      digitalRead(vAr) == HIGH ? true : false;
      debugPrint("Ar pulse received.");
    }

    // Trigger SC-01 input register latch.
    void strobe() 
    {
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
    void shiftUpdate(byte data) 
    {
      shiftOut(srData, srClk, MSBFIRST, data);
      digitalWrite(srLatch, HIGH);
      digitalWrite(srLatch, LOW);
      debugPrint("Shift register loaded, ready to strobe.");
    }
  };

  Votrax sc01;

  //  Wait until it's ok to advance to next phoneme
  void advListen() 
  {
    volatile bool advance;
    updateMode(advanceMode, aSw1.Pressed(), aSw2.Pressed());
    switch (advanceMode) 
    {
      case 1: // Auto Mode
        debugPrint("Advance Mode: Auto Mode");
        advance = sc01.arUpdate();
        break;
      case 2: // Button Mode
        debugPrint("Advance Mode: Button Mode");
        advance = sc01.arUpdate() && button1.Pressed();
        break;
      case 3: // Gate Mode
        debugPrint("Advance Mode: Gate Mode");
        advance = sc01.arUpdate() && gate.State();
        break;
    }
    debugPrint("Waiting to Advance...");
    while (!advance) {}
  }

  // Outputs a given phrase.
  void say(Phrase phrase) 
  {
    debugPrint("Synthesizing phonemes...");
    for (int i = 0; i < phrase.length; i++) 
    {
      updateMode(advanceMode, aSw1.Pressed(), aSw2.Pressed());
      Phoneme p(phrase.symbols[i], phrase.inflections[i]);
      debugPrint("Inflection: " + String(p.inflectionIndex));
      sc01.shiftUpdate(p.payload);
      advListen();
      sc01.strobe();
    }
  }


  //Main loop.
  void main() 
  {
    updateMode(phonemeMode, pSw1.Pressed(), pSw2.Pressed());
    switch (phonemeMode) 
    {
      case 1:  // Memory Mode
        debugPrint("Phoneme Mode: Memory Mode");
        phrase.setFromMemory();
        break;
      case 2:  // XY Mode
        debugPrint("Phoneme Mode: Grid Mode");
        phrase.setFromGrid(x.Value(), y.Value());
        break;
      case 3:  // Random Mode
        debugPrint("Phoneme Mode: Random Mode");
        phrase.setFromRandom();
        break;
    };
    say(phrase);
    while (!button1.Pressed()) {}
  }
};

Grawlix grawl;

void AudioCallback(float **in, float **out, size_t size)
{
  float amp = 1;
  for (size_t i = 0; i < size; i++)
  {
    out[0][i] = in[0][i] * amp;
  }
}

/*
* Hardware timer PMW. Code generated from STM32CubeIDE.
*/
class HardwarePWM
{
  public:
  float freq_knob;
  float voct_multiplier; 
  uint32_t kHz                      = 10^3;
  uint32_t MHz                      = 10^6;
  uint32_t prescaler                = 0;
  uint32_t system_clock_rate        = System::GetPClk2Freq();
  uint32_t default_freq             = 720 * kHz;
  uint32_t default_counter_period   = (system_clock_rate / default_freq)-1;
  uint32_t default_pw               = 0.5;
  uint32_t default_pw_period        = default_counter_period * default_pw;
  uint32_t target_freq              = default_freq;
  uint32_t target_pw                = 0.5;

  /*
  * Configured for  TIM3, Channel 1
  * STM32 Pin:      PB4
  * Seed pin:       A4
  * Patch_SM pin:   D1
  */
  TIM_HandleTypeDef htim3;

  void Init()
  {
    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};
    TIM_OC_InitTypeDef sConfigOC = {0};
    htim3.Instance = TIM3;
    htim3.Init.Prescaler = 0;
    htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3.Init.Period = default_counter_period;
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
    {
      Error_Handler();
    }
    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
    {
      Error_Handler();
    }
    if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
    {
      Error_Handler();
    }
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
    {
      Error_Handler();
    }
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = default_pw_period;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
    {
      Error_Handler();
    }
  }

  uint32_t freqToTicks(uint32_t freq)
  {
    uint32_t ticks = (system_clock_rate / freq) - 1;
    return ticks;
  }

  void setFreq(uint16_t freq_knob, uint16_t voct_val)
  {
    const float multipliers[37] = {
      0.500, 0.530, 0.561, 0.595, 0.630, 0.667, 
      0.707, 0.749, 0.794, 0.841, 0.891, 0.944,
      1.000, 1.059, 1.122, 1.189, 1.260, 1.335, 
      1.414, 1.498, 1.587, 1.682, 1.782, 1.888,
      2.000, 2.119, 2.245, 2.378, 2.520, 2.669, 
      2.828, 2.996, 3.175, 3.363, 3.563, 3.775,
      4.000
    };
    float knob = map(freq_knob,0,10^16-1,0.5,2);
    float voct_mult = multipliers[map(voct_val,0,10^16-1,0,37)];
    target_freq = (720 * MHz) * freq_knob * voct_val;
    uint32_t target_ticks        = freqToTicks(target_freq);
    uint32_t updated_pw_period   = ( target_ticks * target_pw );

    htim3.Instance->ARR     = target_ticks;
    htim3.Instance->CCR1    = updated_pw_period;
  }

  void setPulseWidth(uint32_t pw)
  {
    target_pw = pw;
    uint32_t target_pw_period = (freqToTicks(target_freq) * target_pw)-1;
    htim3.Instance->CCR1 = target_pw_period;
  }
};

HardwarePWM sc01Clock;

void setup() 
{
  Serial.begin(9600);
  // Initialize Daisy hardware.
  update_rate = 1000;
  patch = DAISY.init(DAISY_PATCH_SM, AUDIO_SR_48K);
  num_channels = patch.num_channels;
  sample_rate = DAISY.get_samplerate();
  debugPrint("G R A W L I X  by heythats.cool");

  // Initialize Daisy control objects.
  x.Init(PIN_PATCH_SM_CV_4,update_rate);
  y.Init(PIN_PATCH_SM_CV_3,update_rate);
  voct.InitBipolarCv(PIN_PATCH_SM_CV_2, update_rate);
  freq.Init(PIN_PATCH_SM_CV_1, update_rate);
  rdm.Init(PIN_PATCH_SM_CV_5,update_rate);
  //Initialize Daisy Switches
  pSw1.Init(update_rate, false, PIN_PATCH_SM_A2, INPUT);
  pSw2.Init(update_rate, false, PIN_PATCH_SM_A3, INPUT);
  aSw1.Init(update_rate, false, PIN_PATCH_SM_A8, INPUT);
  aSw2.Init(update_rate, false, PIN_PATCH_SM_A9, INPUT);
  button1.Init(update_rate, false, PIN_PATCH_SM_B9, INPUT);
  //Initialize Daisy GateIn
  gate.Init(PIN_PATCH_SM_GATE_IN_1, INPUT, false);
  //Initialize Daisy Encoder
  encoder.Init(update_rate, PIN_PATCH_SM_D9, PIN_PATCH_SM_D8, 
               PIN_PATCH_SM_D7, INPUT, INPUT, INPUT);

  // Initialize Votrax data i/o pins.
  pinMode(vAr, INPUT);
  int outPins[6] = { vStb, vClk, srData, srLatch, srClk };
  for (int i = 0; i < 6; i++) { pinMode(i, OUTPUT); }

  // Initialize hardware pwm
  sc01Clock.Init();
  sc01Clock.setFreq(freq.Value(), voct.Value());

  DAISY.begin(AudioCallback);
}

void loop() {
  grawl.main();
}
