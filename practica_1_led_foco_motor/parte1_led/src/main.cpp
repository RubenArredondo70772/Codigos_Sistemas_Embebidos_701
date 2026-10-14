#include <Arduino.h>

#define LED 2
#define SW1 21
#define SW2 19
#define PRENDE_LED digitalWrite(LED,HIGH)
#define APAGA_LED digitalWrite(LED,LOW)
#define PRESSED(x) digitalRead(x)==0 

void setup() {

  Serial.begin(115200);
  Serial.println("Hello, ESP32!");

  pinMode(LED, OUTPUT);
  pinMode(SW1, INPUT_PULLUP);
  pinMode(SW2, INPUT_PULLUP);

}

void loop() {

  if(PRESSED(SW1)){
      PRENDE_LED;
      Serial.println("Boton 1 pulsado");
      delay(100);
  }

 if(PRESSED(SW2)){
      APAGA_LED;
      Serial.println("Boton 2 pulsado");
      delay(100);
  }
}