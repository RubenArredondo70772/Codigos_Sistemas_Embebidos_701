#include <Arduino.h>

#define FOCO 2
#define SW1 21
#define SW2 19
#define ENCIENDE_FOCO digitalWrite(FOCO,HIGH)  // HIGH activa el módulo de relé (foco encendido)
#define APAGA_FOCO digitalWrite(FOCO,LOW)      // LOW desactiva el relé (foco apagado)
#define PRESSED(x) digitalRead(x)==0

void setup() {
  Serial.begin(115200);
  pinMode(FOCO, OUTPUT);
  pinMode(SW1, INPUT_PULLUP);
  pinMode(SW2, INPUT_PULLUP);
  APAGA_FOCO;
}

void loop() {
  if (PRESSED(SW1)) {
    ENCIENDE_FOCO;
    Serial.println("Foco encendido");
    delay(100);
  }
  if (PRESSED(SW2)) {
    APAGA_FOCO;
    Serial.println("Foco apagado");
    delay(100);
  }
}