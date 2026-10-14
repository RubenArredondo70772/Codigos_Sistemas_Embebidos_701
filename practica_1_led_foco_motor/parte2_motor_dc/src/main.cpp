#include <Arduino.h>

#define MOTOR 2
#define SW1 21
#define SW2 19
#define ENCIENDE_MOTOR digitalWrite(MOTOR,HIGH)
#define APAGA_MOTOR digitalWrite(MOTOR,LOW)
#define PRESSED(x) digitalRead(x)==0

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR, OUTPUT);
  pinMode(SW1, INPUT_PULLUP);
  pinMode(SW2, INPUT_PULLUP);
}

void loop() {
  if (PRESSED(SW1)) {
    ENCIENDE_MOTOR;
    Serial.println("Motor encendido");
    delay(100);
  }
  if (PRESSED(SW2)) {
    APAGA_MOTOR;
    Serial.println("Motor apagado");
    delay(100);
  }
}