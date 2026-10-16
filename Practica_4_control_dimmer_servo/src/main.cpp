#include <Arduino.h>

#include <ESP32Servo.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------------- PINES ----------------------
#define PIN_SERVO      18
#define BTN_SUBIR      32
#define BTN_BAJAR      33
#define LCD_SDA        21
#define LCD_SCL        22

// ---------------------- AJUSTES ----------------------
const int  ANGULO_MIN       = 0;      // angulo con el foco al MINIMO 
const int  ANGULO_MAX       = 180;    // angulo con el foco al MAXIMO 
const bool INVERTIR_SENTIDO = false;  // true si SUBIR baja la intensidad
const int  PASO_GRADOS      = 3;      // grados que se mueve por cada pulsacion

// Pulsos del MG995 (en microsegundos) para 0 y 180 grados
const int PULSO_MIN_US = 500;
const int PULSO_MAX_US = 2400;

// Repeticion automatica al mantener presionado un boton
const unsigned long RETARDO_REPETICION_MS  = 400;  // espera antes de repetir
const unsigned long INTERVALO_REPETICION_MS = 60;  // velocidad de repeticion
const unsigned long DEBOUNCE_MS = 30;

// ---------------------- OBJETOS Y VARIABLES ----------------------
Servo servo;
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 16, 2);  

const char GRADO = (char)223;  // simbolo de grados en la LCD

int angulo = ANGULO_MIN;   // angulo actual del servo
bool hayCambioSinGuardar = false;
unsigned long ultimoCambio = 0;
const unsigned long GUARDAR_DESPUES_MS = 2000;  // guarda 2 s despues del ultimo cambio

struct Boton {
  int pin;
  bool presionado;
  unsigned long momentoPresion;
  unsigned long ultimaRepeticion;
  unsigned long ultimoCambioLectura;
  bool lecturaAnterior;
};

Boton botonSubir = { BTN_SUBIR, false, 0, 0, 0, HIGH };
Boton botonBajar = { BTN_BAJAR, false, 0, 0, 0, HIGH };

// ---------------------- DECLARACION DE FUNCIONES ----------------------
bool debeMover(Boton &b);
void moverIntensidad(int direccion);
void aplicarAngulo();
int  porcentajeIntensidad();
void actualizarLCD();
void imprimirLinea(int fila, const char* texto);

// =====================================================================
void setup() {
  Serial.begin(115200);

  pinMode(BTN_SUBIR, INPUT_PULLUP);
  pinMode(BTN_BAJAR, INPUT_PULLUP);

  // LCD: pequena espera para que arranque, e I2C a velocidad estandar
  delay(500);
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  imprimirLinea(0, "Dimmer + servo");
  imprimirLinea(1, "Iniciando...");

  // Recuperar el ultimo angulo guardado (asi el servo no salta al encender)
  prefs.begin("dimmer", false);
  angulo = prefs.getInt("angulo", ANGULO_MIN);
  prefs.end();
  angulo = constrain(angulo, ANGULO_MIN, ANGULO_MAX);

  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);  // los servos usan senal de 50 Hz
  servo.attach(PIN_SERVO, PULSO_MIN_US, PULSO_MAX_US);
  aplicarAngulo();

  delay(800);
  actualizarLCD();

  Serial.println("=== Dimmer con servomotor ===");
  Serial.print("Angulo inicial: "); Serial.print(angulo);
  Serial.print(" | Intensidad: ");  Serial.print(porcentajeIntensidad());
  Serial.println(" %");
}

// =====================================================================
void loop() {
  if (debeMover(botonSubir)) moverIntensidad(+1);
  if (debeMover(botonBajar)) moverIntensidad(-1);

  // Guardar la posicion en memoria cuando el usuario deja de mover
  // (no se guarda en cada paso para no desgastar la memoria flash)
  if (hayCambioSinGuardar && millis() - ultimoCambio > GUARDAR_DESPUES_MS) {
    prefs.begin("dimmer", false);
    prefs.putInt("angulo", angulo);
    prefs.end();
    hayCambioSinGuardar = false;
    Serial.println("Posicion guardada");
  }
}

// =====================================================================
// Devuelve true cuando el boton debe producir un paso:
//  - una vez al presionarlo
//  - y luego repetidamente mientras se mantiene presionado
bool debeMover(Boton &b) {
  bool lectura = digitalRead(b.pin);
  unsigned long ahora = millis();

  // Antirrebote: ignorar cambios muy rapidos
  if (lectura != b.lecturaAnterior) {
    b.ultimoCambioLectura = ahora;
    b.lecturaAnterior = lectura;
  }
  if (ahora - b.ultimoCambioLectura < DEBOUNCE_MS) return false;

  bool presionadoAhora = (lectura == LOW);

  if (presionadoAhora && !b.presionado) {        // se acaba de presionar
    b.presionado = true;
    b.momentoPresion = ahora;
    b.ultimaRepeticion = ahora;
    return true;
  }
  if (!presionadoAhora) {                        // se solto
    b.presionado = false;
    return false;
  }
  // Se mantiene presionado: repetir despues del retardo inicial
  if (ahora - b.momentoPresion > RETARDO_REPETICION_MS &&
      ahora - b.ultimaRepeticion > INTERVALO_REPETICION_MS) {
    b.ultimaRepeticion = ahora;
    return true;
  }
  return false;
}

// direccion: +1 = mas intensidad, -1 = menos intensidad
void moverIntensidad(int direccion) {
  if (INVERTIR_SENTIDO) direccion = -direccion;

  int nuevo = constrain(angulo + direccion * PASO_GRADOS, ANGULO_MIN, ANGULO_MAX);
  if (nuevo == angulo) return;  // ya esta en el limite

  angulo = nuevo;
  aplicarAngulo();
  actualizarLCD();
  hayCambioSinGuardar = true;
  ultimoCambio = millis();

  Serial.print("Angulo: ");        Serial.print(angulo);
  Serial.print(" | Intensidad: "); Serial.print(porcentajeIntensidad());
  Serial.println(" %");
}

void aplicarAngulo() {
  servo.write(angulo);
}

// % de intensidad segun la posicion dentro del recorrido calibrado
int porcentajeIntensidad() {
  int p = map(angulo, ANGULO_MIN, ANGULO_MAX, 0, 100);
  return INVERTIR_SENTIDO ? 100 - p : p;
}

// =====================================================================
// LCD 16x2
//   Fila 0: "Intensidad:  75%"
//   Fila 1: "Servo:     135° "
void actualizarLCD() {
  char linea[17];
  snprintf(linea, sizeof(linea), "Intensidad: %3d%%", porcentajeIntensidad());
  imprimirLinea(0, linea);
  snprintf(linea, sizeof(linea), "Servo:     %3d%c", angulo, GRADO);
  imprimirLinea(1, linea);
}

// Escribe un texto en la fila indicada y rellena con espacios hasta 16
// caracteres, para borrar restos de textos anteriores sin parpadeo
void imprimirLinea(int fila, const char* texto) {
  lcd.setCursor(0, fila);
  int n = 0;
  while (texto[n] != '\0' && n < 16) {
    lcd.print(texto[n]);
    n++;
  }
  while (n < 16) {
    lcd.print(' ');
    n++;
  }
}
