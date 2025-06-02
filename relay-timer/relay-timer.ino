/*
 * RetroBox Timer Controller v1.1
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
 * Versione: 1.1
 * Data: 06/02/2025
 *
 * MIT License
 * Copyright (c) 2025 Gabriele Baldassarre
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

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Stringhe da visualizzare sul display
String line_1;
String line_2;

// Stati della macchina
enum SystemMode {
  MODE_PAUSED,
  MODE_SET,
  MODE_RUNNING,
  MODE_FINISHED
};
SystemMode current_mode  = MODE_FINISHED;
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

// Contatore per gestire il timer e le altre funzioni
volatile unsigned long interrupt_counter = 0;
volatile unsigned long second_counter = 0;

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
  lcd.backlight();

  // Configura i pin dei pulsanti come input
  //pinMode(SET_PIN, INPUT);
  //pinMode(PLUS_PIN, INPUT);
  //pinMode(MINUS_PIN, INPUT);
  //pinMode(START_STOP_PIN, INPUT);

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
  if (timerSeconds >= (24*3600)) timerSeconds = 0;

  welcomeScreen();

  // Inizializza Timer1 per chiamare l'interrupt handler ogni 100 millisecondi
  Timer1.initialize(100000);  // 100000 microsecondi = 100 millisecondi
  Timer1.attachInterrupt(timerHandler);
}

void loop() {

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
    if (timerSeconds <= 0) {
      previous_mode = MODE_FINISHED;
      current_mode = MODE_SET;
      return;
    } 
    if (current_mode == MODE_RUNNING) {
      current_mode = MODE_PAUSED;
    } 
    else  {
      if (current_mode != MODE_PAUSED) saveTimeToEEPROM(timerSeconds);
      current_mode = MODE_RUNNING;
    }
  }
} 


// Reset del Timer
// E' disattivato quando il timer e' in stato di "running"
void onSetHold(Button& btn, uint16_t duration){
  if (current_mode != MODE_RUNNING) {
    #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode) ; Serial.println("; Timer Reset!");
    #endif
    timerSeconds = 0;
    current_mode = MODE_FINISHED;
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
  int hours   = timerSeconds / 3600;
  int minutes = (timerSeconds % 3600) / 60;
  int seconds = timerSeconds % 60;
  if (unit == 0) {
      // Modifica le ore
      hours +=value;
      if (hours > MAX_HOURS) hours = 0;
      if (hours < 0) hours = MAX_HOURS;
      timerSeconds = hours * 3600 + minutes * 60 + seconds;
    }
    else if (unit == 1) {
      // Modifica i minuti
      minutes +=value;
      if (minutes > MAX_MIN_SEC) minutes = 0;
      if (minutes < 0) minutes = MAX_MIN_SEC;
      timerSeconds = hours * 3600 + minutes * 60 + seconds;
    }
    else if (unit == 2){
      // Modifica i secondi
      seconds +=value;
      if (seconds > MAX_MIN_SEC) seconds = 0;
      if (seconds < 0) seconds = MAX_MIN_SEC;
      timerSeconds = hours * 3600 + minutes * 60 + seconds;
    }

}

/*************
 * EEPROM
 */
void saveTimeToEEPROM(unsigned long sec) {
  #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode);
    Serial.print("; Saved to EEPROM in position "); Serial.print(EEPROM_ADDRESS);
    Serial.print(": "); Serial.println(timerSeconds);
  #endif
  EEPROM.put(EEPROM_ADDRESS, sec);
}

unsigned long loadTimeFromEEPROM() {
  unsigned long sec;
  EEPROM.get(EEPROM_ADDRESS, sec);
  #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode);
    Serial.print("; Loaded from EEPROM from position "); Serial.print(EEPROM_ADDRESS);
    Serial.print(": "); Serial.println(sec);
  #endif
  return sec;
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
  splitTime(timerSeconds, h, m, s);

  setPayload(true);

  line_1 = "Lights are on...";
  line_2 = "Time: " + formatNumber(h) + ":" + formatNumber(m) + ":" + formatNumber(s);
}

void controllerPaused(){
  int h, m, s;
  splitTime(timerSeconds, h, m, s);

  setPayload(false);
  line_1 = "Paused!         ";
  line_2 = "Time: " + (display_visible ? (formatNumber(h) + ":" + formatNumber(m) + ":" + formatNumber(s)) : ("        "));
}

void controllerFinished(){
  int h, m, s;
  splitTime(timerSeconds, h, m, s);

  setPayload(false);
  line_1 = "Retrobox v1.1   ";
  line_2 = "Time: " + formatNumber(h) + ":" + formatNumber(m) + ":" + formatNumber(s);
}

void controllerSet(){
  int h, m, s;
  splitTime(timerSeconds, h, m, s);

  //setPayload(false);
  line_1 = "Set timer...    ";
  line_2 = "Time: " + (!display_visible && current_time_unit == 0 ? "  " : formatNumber(h)) + ":" + (!display_visible && current_time_unit == 1 ? "  " : formatNumber(m)) + ":" + (!display_visible && current_time_unit == 2 ? "  " : formatNumber(s));

}



/*********************************
 * GESTIONE DELLA TEMPORIZZAZIONE
 */

// Il timer gestisce solo l'aggiornamento del display e lo stato
// del timer
void timerHandler() {
  interrupt_counter++;
  // Azioni da eseguire ogni secondo
  if (interrupt_counter % 10 == 0) {  // 100ms * 10 = 1000ms = 1s
    #ifdef DEBUG
    Serial.print("Mode: "); Serial.print(current_mode);
    Serial.print("; Set: "); Serial.print(set_button.isPressed());
    Serial.print("; Plus: "); Serial.print(plus_button.isPressed());
    Serial.print("; Minus: "); Serial.print(minus_button.isPressed());
    Serial.print("; Start/Stop: "); Serial.print(start_stop_button.isPressed());
    Serial.print("; Payload: "); Serial.print(digitalRead(RELAY_PIN) ? "On" : "Off");
    Serial.print("; Time Left: "); Serial.print(timerSeconds); Serial.print(" s");
    Serial.print("; Elapsed: "); Serial.print(second_counter); Serial.print(" s");
    Serial.print("; IC: "); Serial.println(interrupt_counter);
    #endif

    // Se il timer e' in funzione, decrementa il countdown
    if (current_mode  == MODE_RUNNING) {
      timerSeconds--;
      second_counter++;

      if(timerSeconds <= 0){
        current_mode = MODE_FINISHED;
        timerSeconds = 0;
      }

    }
  }

  // Azioni da eseguire ogni mezzo secondo
  if (interrupt_counter % 5 == 0) {  // 100ms * 5 = 500ms = 0.5s
    // Se siamo in stato "set" oppure "pause" imposta la flag per far lampeggiare parti del display
    // a meno che non ci sia almeno un pulsante premuto
    if ((current_mode == MODE_SET || current_mode == MODE_PAUSED) && !at_least_one_button_pressed) {
      display_visible = !display_visible;
    } else {
      display_visible = true; 
    }
  }

  // Azioni da eseguire ogni 100 msec
  updateDisplay();

  // Resetta il contatore dopo un periodo di tempo per evitare un overflow
  if (interrupt_counter >= 20000) {
    interrupt_counter = 0;
  }
}

/***********************
 * GESTIONE DEL DISPLAY
 */

// Formato numerico (per i trailing zero)
String formatNumber(int val) {
  String out = "";
  if (val < 10) out += "0";
  out += val;
  return out;
}

// Aggiorna semplicemente il display con i messaggi preparati dalle action degli stati
void updateDisplay() {

    lcd.setCursor(0, 0);
    lcd.print(line_1);

    lcd.setCursor(0, 1);
    lcd.print(line_2);
}

void welcomeScreen(){
  int h, m, s;
  splitTime(timerSeconds, h, m, s);

  line_1 = "Retrobox v1.1   ";
  line_2 = "Time: " + formatNumber(h) + ":" + formatNumber(m) + ":" + formatNumber(s);

  updateDisplay();
}
