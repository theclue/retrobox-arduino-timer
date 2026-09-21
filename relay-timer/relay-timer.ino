/*
 * RetroBox Timer Controller v1.2
 *
 * Questo sketch Arduino implementa un timer digitale con controllo relè, pensato per attivare dispositivi
 * per un intervallo di tempo configurabile (in questo caso lampade UV per Retrobright). L'utente imposta il tempo
 * tramite pulsanti (ore, minuti, secondi), visualizza lo stato su un display LCD 16x2 I2C e gestisce
 * il ciclo con un pulsante START/STOP. Include incremento rapido (tenendo premuti i pulsanti +/-),
 * reset del timer (tenendo premuto il tasto SET per 3s, configurabile), feedback visivo tramite lampeggio del display,
 * debouncing dei pulsanti e debug. Include inoltre il salvataggio dell'ultimo timer impostato in EEPROM,
 * recuperato al prossimo riavvio.
 *
 * Hardware richiesto:
 * - Arduino UNO (o compatibile)
 * - Display LCD 16x2 I2C (indirizzo 0x27)
 * - 4 pulsanti: SET, +, -, START/STOP
 * - 1 modulo relè
 *
 * Autore: Gabriele Baldassarre
 * Licenza: MIT
 * Versione: 1.2
 * Data: 21/09/2026
 * Changelog: fix race condition ISR/loop, ISR minimale, eliminazione stringhe dinamiche,
 * EEPROM con magic byte, watchdog, correzione wrap incremento rapido,
 * correzione overflow int su ore.
 *
 * MIT License
 * Copyright (c) 2026 Gabriele Baldassarre
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <TimerOne.h>
#include <Button.h>
#include <ButtonEventCallback.h>
#include <PushButton.h>
#include <Bounce2.h>
#include <EEPROM.h>
#include <util/atomic.h>
#include <avr/wdt.h>

/*
#define DEBUG
*/

#define ENABLE_BUTTONS

// Pin Set
#define SET_PIN 4
#define PLUS_PIN 2
#define MINUS_PIN 3
#define START_STOP_PIN 5
#define RELAY_PIN 6

#define DEBOUNCE_DELAY 20       // Ritardo del debounce (in msec)
#define RESET_DELAY 3000        // Per quanto tempo deve essere premuto il tasto SET per resettare il Timer (in msec)
#define ACTIVATION_DELAY 500    // Ritardo dell'attivazione della modalità incremento rapido per i puslanti +/-
#define HOLD_INCREMENT 1000     // Ritardo delle pressioni ripetute dei pulsanti in modalita' incremento rapido
#define INCREMENT_AMOUNT 10     // Incremento/Decremento delle unita' temporali in modalita' incremento rapido

#define MAX_HOURS 23
#define MAX_MIN_SEC 59

#define EEPROM_ADDRESS 0
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2
#define MAX_TIMER_SECONDS (24UL*3600UL)
#define TICKS_PER_SECOND 10
#define BLINK_TOGGLE_TICKS 5
#define EEPROM_MAGIC 0xAB12

struct EepromData {
  uint16_t magic;
  unsigned long seconds;
};

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// Buffer da visualizzare sul display
char line_1[LCD_COLS + 1];
char line_2[LCD_COLS + 1];

// Stati della macchina
enum SystemMode {
  MODE_PAUSED,
  MODE_SET,
  MODE_RUNNING,
  MODE_FINISHED
};
volatile SystemMode current_mode  = MODE_FINISHED;
SystemMode previous_mode = current_mode;

// Contasecondi
volatile unsigned long timerSeconds = 0;

#ifdef ENABLE_BUTTONS
// Istanze degli oggetti che gestiranno i pulsanti
PushButton set_button        = PushButton(SET_PIN, 0);
PushButton plus_button       = PushButton(PLUS_PIN, 0);
PushButton minus_button      = PushButton(MINUS_PIN, 0);
PushButton start_stop_button = PushButton(START_STOP_PIN, 0);
#endif

// Tiene conto se almeno un pulsante e' correntemente premuto
// per non far lampeggiare il display
volatile bool at_least_one_button_pressed = false;

// Contatore dei tick pendenti e contatori per gestire il timer nel loop
volatile uint8_t pending_ticks = 0;
uint8_t loop_tick_counter = 0;
unsigned long second_counter = 0;
bool blink_toggle = false;

// Cursore su quale componente del timer e' correntemente selezionato
// per farla lampeggiare
volatile short current_time_unit = 0; // 0=ore, 1=minuti, 2=secondi

// Stato corrente di visibilità del display
// true=visibile, false=non visibile
volatile bool display_visible = true;

void setup() {

  #ifdef DEBUG
  Serial.begin(9600);
  #endif

  lcd.init();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(25000, true);
#endif
  lcd.backlight();

  // I pulsanti chiudono a GND: pressed = LOW, coerente con PushButton(pin, 0).
  pinMode(SET_PIN, INPUT_PULLUP);
  pinMode(PLUS_PIN, INPUT_PULLUP);
  pinMode(MINUS_PIN, INPUT_PULLUP);
  pinMode(START_STOP_PIN, INPUT_PULLUP);

  // Configura il pin del relé come output
  pinMode(RELAY_PIN, OUTPUT);

#ifdef ENABLE_BUTTONS
  // Configurazione iniziale dei pulsanti
  set_button.configureButton(configurePushButton);
  plus_button.configureButton(configurePushButton);
  minus_button.configureButton(configurePushButton);
  start_stop_button.configureButton(configurePushButton);

  // Gestione della pressione dei pulsanti
  set_button.onPress(onButtonPressed);
  plus_button.onPress(onButtonPressed);
  minus_button.onPress(onButtonPressed);
  start_stop_button.onPress(onButtonPressed);

  // Timer Reset
  set_button.onHold(RESET_DELAY, onSetHold);

  // Modalita' inserimento rapido
  plus_button.onHoldRepeat(ACTIVATION_DELAY, HOLD_INCREMENT, onButtonsHoldRepeat);
  minus_button.onHoldRepeat(ACTIVATION_DELAY, HOLD_INCREMENT, onButtonsHoldRepeat);
#endif

  timerSeconds = loadTimeFromEEPROM();
  if (timerSeconds >= MAX_TIMER_SECONDS) timerSeconds = 0;

  welcomeScreen();

  // Inizializza Timer1 per chiamare l'interrupt handler ogni 100 millisecondi
  Timer1.initialize(100000);  // 100000 microsecondi = 100 millisecondi
  Timer1.attachInterrupt(timerHandler);
  wdt_enable(WDTO_2S);
}

void loop() {

  wdt_reset();

  uint8_t ticks_to_process;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    ticks_to_process = pending_ticks;
    pending_ticks = 0;
  }
  bool process_second_tick = false;
  for (uint8_t i = 0; i < ticks_to_process; i++) {
    loop_tick_counter++;
    if (loop_tick_counter >= TICKS_PER_SECOND) loop_tick_counter = 0;
    if (loop_tick_counter % BLINK_TOGGLE_TICKS == 0) blink_toggle = !blink_toggle;
    if (loop_tick_counter == 0) {
      process_second_tick = true;
      if (current_mode == MODE_RUNNING) {
        if (timerSeconds > 0) timerSeconds--;
        if (timerSeconds == 0) current_mode = MODE_FINISHED;
      }
      second_counter++;
    }
  }
  if (process_second_tick) {
#ifdef DEBUG
    unsigned long debug_timer;
    unsigned long debug_second_counter;
    unsigned long debug_tick_counter;
    SystemMode debug_mode;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      debug_timer = timerSeconds;
      debug_second_counter = second_counter;
      debug_tick_counter = loop_tick_counter;
      debug_mode = current_mode;
    }
    Serial.print("Mode: "); Serial.print(debug_mode);
    Serial.print("; Set: "); Serial.print(set_button.isPressed());
    Serial.print("; Plus: "); Serial.print(plus_button.isPressed());
    Serial.print("; Minus: "); Serial.print(minus_button.isPressed());
    Serial.print("; Start/Stop: "); Serial.print(start_stop_button.isPressed());
    Serial.print("; Payload: "); Serial.print(digitalRead(RELAY_PIN) ? "On" : "Off");
    Serial.print("; Time Left: "); Serial.print(debug_timer); Serial.print(" s");
    Serial.print("; Elapsed: "); Serial.print(debug_second_counter); Serial.print(" s");
    Serial.print("; IC: "); Serial.println(debug_tick_counter);
#endif
  }

  #ifdef ENABLE_BUTTONS
  // Lettura dello stato corrente dei pulsanti
  set_button.update();
  plus_button.update();
  minus_button.update();
  start_stop_button.update();

  at_least_one_button_pressed = set_button.isPressed() || plus_button.isPressed() || minus_button.isPressed() || start_stop_button.isPressed();
  #endif

  switch (current_mode) {
    case MODE_SET:
      controllerSet();
      break;
    case MODE_RUNNING:
      controllerRunning();
      break;
    case MODE_PAUSED:
      controllerPaused();
      break;
    case MODE_FINISHED:
      controllerFinished();
      break;
  }

  display_visible = ((current_mode == MODE_SET || current_mode == MODE_PAUSED) && !at_least_one_button_pressed) ? blink_toggle : true;
  updateDisplay();
}

/**********************
 * GESTIONE DEI PULSANTI
 */
#ifdef ENABLE_BUTTONS

// Setup iniziale per determinare l'intervallo di debounce
void configurePushButton(Bounce& bouncedButton){
  #ifdef DEBUG
  Serial.print("Configurazione del pulsante");
  #endif
  bouncedButton.interval(DEBOUNCE_DELAY);
}

// Transizioni di stato dai pulsanti
void onButtonPressed(Button& btn){
  if(btn.is(set_button)){
    // Pulsante 'set' premuto
    if (current_mode == MODE_SET){
      current_time_unit++;
      if (current_time_unit > 2) {
        current_time_unit = 0;
        current_mode = previous_mode;
        unsigned long currentTimer;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
        if (previous_mode == MODE_RUNNING && currentTimer == 0) current_mode = MODE_FINISHED;
        }
    } else {
      previous_mode = current_mode;
      current_time_unit = 0;
      current_mode = MODE_SET;
    }
  } 
  else if (btn.is(plus_button)){
    // Pulsante '+' premuto
    // Non ha effetto se la macchina non e' nello stato "set"
    if(current_mode == MODE_SET){
      alterTimer(current_time_unit, 1);
    }

  } 
  else if (btn.is(minus_button)){
    // Pulsante '-' premuto
    // Non ha effetto se la macchina non e' nello stato "set"
    if(current_mode == MODE_SET){
      alterTimer(current_time_unit, -1);
    }
    
  }
  else if (btn.is(start_stop_button)){
    // Pulsante start/stop premuto
    // Se il timer è a zero forza allo stato di set
    unsigned long currentTimer;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
    if (currentTimer <= 0) {
      previous_mode = MODE_FINISHED;
      current_mode = MODE_SET;
      return;
    } 
    if (current_mode == MODE_RUNNING) {
      current_mode = MODE_PAUSED;
    } 
    else  {
      if (current_mode != MODE_PAUSED) saveTimeToEEPROM(currentTimer);
      current_mode = MODE_RUNNING;
    }
  }
} 


// Reset del Timer
// E' disattivato quando il timer e' in stato di "running"
void onSetHold(Button& btn, uint16_t duration){
  (void)btn;      // firma imposta dalla libreria r89m Button
  (void)duration;
  if (current_mode != MODE_RUNNING) {
    #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode) ; Serial.println("; Timer Reset!");
    #endif
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      timerSeconds = 0;
      current_mode = MODE_FINISHED;
    }
  }
  #ifdef DEBUG
  else {
    Serial.print("Mode: "); Serial.print(current_mode) ; Serial.println("; Timer Reset not allowed while running!");
    }
  #endif
}


// Auto-incrementi di unita' temporali
// I pulsanti '+' e '-' possono essere tenuti premuti per incrementi/decrementi di 10 unità
void onButtonsHoldRepeat(Button& btn, uint16_t duration, uint16_t repeatCount){
  (void)duration;
  (void)repeatCount;
  if (btn.is(plus_button) && current_mode == MODE_SET){
    // Pulsante "plus" premuto
    // Non ha effetto se la macchina non e' nello stato "set"
    alterTimer(current_time_unit, INCREMENT_AMOUNT);

  }
  else if (btn.is(minus_button) && current_mode == MODE_SET){
    // Pulsante "minus" premuto
    // Non ha effetto se la macchina non e' nello stato "set"
    alterTimer(current_time_unit, (-1*INCREMENT_AMOUNT));
  }
}
#endif

// Aggiungi e rimuovi tempo al timer
// unit: 0=ore, 1=minuti, 2=secondi
void alterTimer(short unit, int value){
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    unsigned long currentTimer = timerSeconds;
    int hours   = currentTimer / 3600UL;
    int minutes = (currentTimer % 3600UL) / 60UL;
    int seconds = currentTimer % 60UL;
    if (unit == 0) hours += value;
    else if (unit == 1) minutes += value;
    else if (unit == 2) seconds += value;
    minutes = ((minutes % 60) + 60) % 60;
    seconds = ((seconds % 60) + 60) % 60;
    hours = ((hours % 24) + 24) % 24;
    timerSeconds = (unsigned long)hours * 3600UL + (unsigned long)minutes * 60UL + (unsigned long)seconds;
  }

}

/*************
 * EEPROM
 */
void saveTimeToEEPROM(unsigned long sec) {
  #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode);
    Serial.print("; Saved to EEPROM in position "); Serial.print(EEPROM_ADDRESS);
    Serial.print(": "); Serial.println(sec);
  #endif
  EepromData data = { EEPROM_MAGIC, sec };
  EEPROM.put(EEPROM_ADDRESS, data);
}

unsigned long loadTimeFromEEPROM() {
  EepromData data;
  EEPROM.get(EEPROM_ADDRESS, data);
  #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode);
    Serial.print("; Loaded from EEPROM from position "); Serial.print(EEPROM_ADDRESS);
    Serial.print(": "); Serial.println(data.seconds);
  #endif
  return data.magic == EEPROM_MAGIC ? data.seconds : 0;
}

/**********************
* GESTIONE DEL CARICO
*/

// Commuta il relé
void setPayload(bool status){
   if(status){ 
      if(digitalRead(RELAY_PIN) == LOW) digitalWrite(RELAY_PIN, HIGH);
    } 
    else { 
      if(digitalRead(RELAY_PIN) == HIGH) digitalWrite(RELAY_PIN, LOW);
    }
}

/**********************
 * CONTROLLER
 */

void splitTime(unsigned long seconds, int& h, int& m, int& s) {
  h = seconds / 3600;
  m = (seconds % 3600) / 60;
  s = seconds % 60;
}

void controllerRunning() {
  int h, m, s;
  unsigned long currentTimer;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
  splitTime(currentTimer, h, m, s);

  setPayload(true);

  snprintf(line_1, sizeof(line_1), "%-16s", "Lights are on...");
  snprintf(line_2, sizeof(line_2), "Time: %02d:%02d:%02d  ", h, m, s);
}

void controllerPaused(){
  int h, m, s;
  unsigned long currentTimer;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
  splitTime(currentTimer, h, m, s);

  setPayload(false);
  snprintf(line_1, sizeof(line_1), "%-16s", "Paused!");
  snprintf(line_2, sizeof(line_2), "Time: %s  ", display_visible ? "00:00:00" : "        ");
  if (display_visible) snprintf(line_2, sizeof(line_2), "Time: %02d:%02d:%02d  ", h, m, s);
}

void controllerFinished(){
  int h, m, s;
  unsigned long currentTimer;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
  splitTime(currentTimer, h, m, s);

  setPayload(false);
  snprintf(line_1, sizeof(line_1), "%-16s", "Retrobox v1.2");
  snprintf(line_2, sizeof(line_2), "Time: %02d:%02d:%02d  ", h, m, s);
}

void controllerSet(){
  int h, m, s;
  char h_text[3], m_text[3], s_text[3];
  unsigned long currentTimer;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
  splitTime(currentTimer, h, m, s);

  //setPayload(false);
  snprintf(line_1, sizeof(line_1), "%-16s", "Set timer...");
  snprintf(h_text, sizeof(h_text), !display_visible && current_time_unit == 0 ? "  " : "%02d", h);
  snprintf(m_text, sizeof(m_text), !display_visible && current_time_unit == 1 ? "  " : "%02d", m);
  snprintf(s_text, sizeof(s_text), !display_visible && current_time_unit == 2 ? "  " : "%02d", s);
  snprintf(line_2, sizeof(line_2), "Time: %s:%s:%s  ", h_text, m_text, s_text);

}



/*********************************
 * GESTIONE DELLA TEMPORIZZAZIONE
 */

// La ISR genera solo tick saturati a 255; tutta la logica del timer viene gestita nel loop.
void timerHandler() {
  // Saturazione: se il loop è bloccato, i tick oltre 255 non vengono contati
  // (il watchdog a 2s interviene comunque molto prima)
  if (pending_ticks < 255) pending_ticks++;
}

/***********************
 * GESTIONE DEL DISPLAY
 */

// Aggiorna semplicemente il display con i messaggi preparati dalle action degli stati
void updateDisplay() {

    lcd.setCursor(0, 0);
  lcd.print(line_1);

    lcd.setCursor(0, 1);
    lcd.print(line_2);
}

void welcomeScreen(){
  int h, m, s;
  unsigned long currentTimer;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { currentTimer = timerSeconds; }
  splitTime(currentTimer, h, m, s);

  snprintf(line_1, sizeof(line_1), "%-16s", "Retrobox v1.2");
  snprintf(line_2, sizeof(line_2), "Time: %02d:%02d:%02d  ", h, m, s);

  updateDisplay();
}
