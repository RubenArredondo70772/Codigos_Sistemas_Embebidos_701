#include <Arduino.h>

/*
  =====================================================================
  CONTROL DE TEMPERATURA CON ESP32
  CICLO CONTINUO: CALENTAR HASTA MAX -> ENFRIAR HASTA MIN -> REPETIR
  =====================================================================
  Funcionamiento:
   - Un sensor DS18B20 mide la temperatura dentro de la caja.
   - El sistema trabaja en un ciclo sin fin:
       1) CALENTANDO: el foco (rele IN1) esta encendido hasta que la
          temperatura llega a la temperatura MAXIMA (ej. 60 C).
       2) Al llegar al MAX se apaga el foco y se enciende la celda
          Peltier junto con su ventilador (rele IN2).
       3) ENFRIANDO: la Peltier sigue encendida hasta que la temperatura
          baja a la temperatura MINIMA (ej. 30 C).
       4) Al llegar al MIN se apaga la Peltier y se enciende de nuevo
          el foco. Y asi sucesivamente.
   - El ventilador extractor (rele IN3) SOLO se enciende mientras la caja
     se esta ENFRIANDO, para ayudar a sacar el aire caliente. Mientras se
     CALIENTA esta apagado, para no desperdiciar el calor del foco.
   - Nunca estan el foco y la Peltier encendidos al mismo tiempo. Al
     cambiar de uno a otro hay una pausa corta con los dos apagados.
   - Si el sensor deja de responder, se apagan el foco y la Peltier por
     seguridad (para que el foco no siga calentando sin control) y la
     LCD lo indica. El extractor se enciende para sacar el calor que
     quede en la caja.

  Botones:
   - MODO  -> cada pulsacion cambia entre editar el MAX y editar el MIN.
   - SUBIR -> sube 1 grado el valor seleccionado.
   - BAJAR -> baja 1 grado el valor seleccionado.
   Los valores se guardan en memoria y se conservan aunque se vaya la luz.

  LCD 16x2:
     +----------------+
     |>Max:60° Min:30°|   <- limites; la ">" marca cual se esta editando
     |45.3°  50% Calor|   <- temperatura actual, % y estado
     +----------------+
   - El % es la posicion de la temperatura entre el MIN (0%) y el MAX (100%).
     Ejemplo con MIN 30 y MAX 60: 30° = 0%, 45° = 50%, 60° = 100%.
   - Estado: "Calor" (foco encendido) o "Frio" (Peltier encendida).

  LIBRERIAS (instalar desde Programa -> Incluir Libreria -> Administrar):
   - LiquidCrystal I2C   (Frank de Brabander)
   - OneWire             (Paul Stoffregen)
   - DallasTemperature   (Miles Burton)
   - Preferences         (ya viene con el core de ESP32)

  CONEXIONES (en el adaptador: fila S = senal, V = voltaje, G = GND;
  el jumper JUMP del adaptador debe estar en 5V):
   - DS18B20:  DATA -> GPIO 4, VCC -> 3.3V, GND -> GND
               + resistencia de 4.7k entre DATA y 3.3V (obligatoria)
   - Reles:    IN1 -> GPIO 26 (FOCO)
               IN2 -> GPIO 27 (PELTIER + su ventilador)
               IN3 -> GPIO 13 (EXTRACTOR - ahora es obligatorio
                      conectarlo a este rele para poder controlarlo)
   - LCD I2C:  SDA -> GPIO 21, SCL -> GPIO 22, VCC -> 5V
   - Botones (a GND): MODO -> GPIO 32, SUBIR -> GPIO 33, BAJAR -> GPIO 25
  =====================================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

// ---------------------- CONFIGURACION DE PINES ----------------------
#define SENSOR_PIN        4    // DATA del DS18B20

#define RELAY_FOCO        26   // IN1 - foco (CALIENTA)
#define RELAY_PELTIER     27   // IN2 - celda Peltier + su ventilador (ENFRIA)
#define RELAY_EXTRACTOR   13   // IN3 - ventilador extractor (solo al ENFRIAR)

#define BTN_MODO          32
#define BTN_SUBIR         33
#define BTN_BAJAR         25

#define LCD_SDA           21
#define LCD_SCL           22

// La mayoria de modulos de rele son "activos en BAJO" (LOW).
// Si tus reles funcionan al reves, cambia LOW por HIGH.
#define RELAY_ACTIVO   LOW
#define RELAY_INACTIVO (!RELAY_ACTIVO)

// ---------------------- PARAMETROS DE CONTROL --------------------------
const int TEMP_MAX_INICIAL = 60;  // C - valor de fabrica del MAX
const int TEMP_MIN_INICIAL = 30;  // C - valor de fabrica del MIN
const int LIMITE_INFERIOR  = 0;   // C - lo mas bajo que se puede configurar
const int LIMITE_SUPERIOR  = 99;  // C - lo mas alto que se puede configurar
const int PASO_SETPOINT    = 1;   // grados que cambia el MAX/MIN por pulsacion
const int MARGEN_MINIMO    = 5;   // separacion minima entre MAX y MIN (grados)

const unsigned long PAUSA_CAMBIO_MS = 1000;  // foco y Peltier apagados al cambiar
const int MAX_ERRORES_SENSOR = 3;            // lecturas fallidas seguidas antes de parar

// ---------------------- OBJETOS GLOBALES --------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);  // si no se ve nada, prueba 0x3F
OneWire oneWire(SENSOR_PIN);
DallasTemperature sensor(&oneWire);
Preferences prefs;

// ---------------------- ESTADO DEL SISTEMA ------------------------------
enum EstadoCiclo { INICIANDO, CALENTANDO, ENFRIANDO, ERROR_SENSOR };
EstadoCiclo estado = INICIANDO;

int tempMax = TEMP_MAX_INICIAL;
int tempMin = TEMP_MIN_INICIAL;

enum ModoEdicion { EDITAR_MAX, EDITAR_MIN };
ModoEdicion modoEdicion = EDITAR_MAX;

float tempActual    = 0.0;
float porcentaje    = 0.0;
int   erroresSensor = 0;
bool  hayLectura    = false;

unsigned long momentoCambioEstado = 0;

// Lectura del sensor sin bloquear el programa
// (el DS18B20 tarda ~750 ms en medir; mientras tanto los botones siguen funcionando)
unsigned long momentoPeticion = 0;
const unsigned long TIEMPO_CONVERSION_MS = 800;

// Botones (deteccion de flanco + antirrebote)
bool anteriorModo  = HIGH;
bool anteriorSubir = HIGH;
bool anteriorBajar = HIGH;
unsigned long ultimoPulsoModo  = 0;
unsigned long ultimoPulsoSubir = 0;
unsigned long ultimoPulsoBajar = 0;
const unsigned long DEBOUNCE_MS = 200;

unsigned long ultimaActualizacionLCD = 0;
const unsigned long INTERVALO_LCD_MS = 300;

const char GRADO = (char)223;  // simbolo de grados en la LCD

// ---------------------- DECLARACION DE FUNCIONES -----------------------
void leerSensor();
void actualizarCiclo();
void cambiarEstado(EstadoCiclo nuevo);
void aplicarSalidas();
void apagarFocoYPeltier();
void actualizarExtractor();
const char* nombreEstado();
bool botonPresionado(int pin, bool &anterior, unsigned long &ultimoPulso);
void manejarBotones();
void validarLimites();
void guardarConfiguracion();
void actualizarLCD();
void imprimirLinea(int fila, const char* texto);

// =====================================================================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_FOCO, OUTPUT);
  pinMode(RELAY_PELTIER, OUTPUT);
  pinMode(RELAY_EXTRACTOR, OUTPUT);
  apagarFocoYPeltier();
  digitalWrite(RELAY_EXTRACTOR, RELAY_INACTIVO);  // el extractor arranca apagado

  pinMode(BTN_MODO,  INPUT_PULLUP);
  pinMode(BTN_SUBIR, INPUT_PULLUP);
  pinMode(BTN_BAJAR, INPUT_PULLUP);

  // LCD
  delay(500);
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  // Sensor DS18B20 (modo sin espera para no congelar los botones)
  sensor.begin();
  sensor.setResolution(12);           // 0.0625 C de resolucion
  sensor.setWaitForConversion(false);
  if (sensor.getDeviceCount() == 0) {
    Serial.println("AVISO: no se detecto ningun DS18B20. Revisa DATA y la resistencia de 4.7k.");
  }
  sensor.requestTemperatures();
  momentoPeticion = millis();

  // Cargar MAX y MIN guardados (si no hay nada guardado: 60 y 30)
  prefs.begin("configTemp", false);
  tempMax = prefs.getInt("tempMax", TEMP_MAX_INICIAL);
  tempMin = prefs.getInt("tempMin", TEMP_MIN_INICIAL);
  prefs.end();
  validarLimites();

  Serial.println("=== Control de temperatura - ciclo continuo ===");
  Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");

  delay(1000);
  lcd.clear();
}

// =====================================================================
void loop() {
  manejarBotones();

  // Cuando el sensor termina de medir: leer, actualizar el ciclo y pedir otra medicion
  if (millis() - momentoPeticion >= TIEMPO_CONVERSION_MS) {
    leerSensor();
    actualizarCiclo();
    sensor.requestTemperatures();
    momentoPeticion = millis();
  }

  if (millis() - ultimaActualizacionLCD >= INTERVALO_LCD_MS) {
    ultimaActualizacionLCD = millis();
    actualizarLCD();
  }
}

// =====================================================================
// SENSOR DS18B20
void leerSensor() {
  float t = sensor.getTempCByIndex(0);

  // -127 = sensor desconectado. 85 en la primera lectura = valor de arranque del
  // DS18B20 (todavia no ha medido), no es una temperatura real.
  bool lecturaInvalida = (t == DEVICE_DISCONNECTED_C) || (!hayLectura && t == 85.0);

  if (lecturaInvalida) {
    erroresSensor++;
    Serial.println("Error leyendo el sensor DS18B20");
    return;
  }

  erroresSensor = 0;
  hayLectura = true;
  tempActual = t;

  // % = posicion de la temperatura entre MIN (0%) y MAX (100%)
  porcentaje = (tempActual - tempMin) * 100.0 / (tempMax - tempMin);
  porcentaje = constrain(porcentaje, 0.0, 100.0);

  Serial.print("Temperatura: "); Serial.print(tempActual, 2);
  Serial.print(" C | ");         Serial.print(porcentaje, 0);
  Serial.print(" % | Estado: "); Serial.println(nombreEstado());
}

// =====================================================================
// CICLO: CALENTANDO hasta MAX -> ENFRIANDO hasta MIN -> CALENTANDO ...
void actualizarCiclo() {
  // Seguridad: si el sensor falla varias veces seguidas, apagar foco y Peltier
  if (erroresSensor >= MAX_ERRORES_SENSOR) {
    if (estado != ERROR_SENSOR) cambiarEstado(ERROR_SENSOR);
    apagarFocoYPeltier();
    actualizarExtractor();
    return;
  }

  // Todavia no hay ninguna lectura valida: no hacer nada
  if (!hayLectura) return;

  switch (estado) {
    case INICIANDO:
    case ERROR_SENSOR:
      // Primera lectura valida (o el sensor se recupero): decidir por donde empezar
      if (erroresSensor == 0) {
        cambiarEstado(tempActual >= tempMax ? ENFRIANDO : CALENTANDO);
      }
      break;

    case CALENTANDO:
      if (tempActual >= tempMax) cambiarEstado(ENFRIANDO);
      break;

    case ENFRIANDO:
      if (tempActual <= tempMin) cambiarEstado(CALENTANDO);
      break;
  }

  aplicarSalidas();
}

void cambiarEstado(EstadoCiclo nuevo) {
  estado = nuevo;
  momentoCambioEstado = millis();
  apagarFocoYPeltier();   // los dos apagados durante la pausa de cambio
  actualizarExtractor();  // el extractor cambia en el mismo instante
  Serial.print(">>> Cambio de estado: "); Serial.println(nombreEstado());
}

// Extractor: encendido solo al ENFRIAR (y en error del sensor, para sacar
// el calor que quede). Apagado al CALENTAR para no perder el calor del foco.
void actualizarExtractor() {
  bool encender = (estado == ENFRIANDO || estado == ERROR_SENSOR);
  digitalWrite(RELAY_EXTRACTOR, encender ? RELAY_ACTIVO : RELAY_INACTIVO);
}

void aplicarSalidas() {
  actualizarExtractor();

  // Durante la pausa despues de un cambio, foco y Peltier quedan apagados
  if (millis() - momentoCambioEstado < PAUSA_CAMBIO_MS) {
    apagarFocoYPeltier();
    return;
  }

  if (estado == CALENTANDO) {
    digitalWrite(RELAY_PELTIER, RELAY_INACTIVO);  // primero apagar el otro
    digitalWrite(RELAY_FOCO,    RELAY_ACTIVO);
  } else if (estado == ENFRIANDO) {
    digitalWrite(RELAY_FOCO,    RELAY_INACTIVO);
    digitalWrite(RELAY_PELTIER, RELAY_ACTIVO);
  } else {
    apagarFocoYPeltier();
  }
}

void apagarFocoYPeltier() {
  digitalWrite(RELAY_FOCO,    RELAY_INACTIVO);
  digitalWrite(RELAY_PELTIER, RELAY_INACTIVO);
}

const char* nombreEstado() {
  switch (estado) {
    case CALENTANDO:   return "CALENTANDO";
    case ENFRIANDO:    return "ENFRIANDO";
    case ERROR_SENSOR: return "ERROR SENSOR";
    default:           return "INICIANDO";
  }
}

// =====================================================================
// BOTONES
// Devuelve true solo en el instante en que el boton se presiona
bool botonPresionado(int pin, bool &anterior, unsigned long &ultimoPulso) {
  bool lectura = digitalRead(pin);
  bool presionado = false;
  if (lectura == LOW && anterior == HIGH && (millis() - ultimoPulso) > DEBOUNCE_MS) {
    ultimoPulso = millis();
    presionado = true;
  }
  anterior = lectura;
  return presionado;
}

void manejarBotones() {
  // MODO: alterna entre editar MAX y editar MIN
  if (botonPresionado(BTN_MODO, anteriorModo, ultimoPulsoModo)) {
    modoEdicion = (modoEdicion == EDITAR_MAX) ? EDITAR_MIN : EDITAR_MAX;
    Serial.print("Editando: "); Serial.println(modoEdicion == EDITAR_MAX ? "Max" : "Min");
  }

  // SUBIR
  if (botonPresionado(BTN_SUBIR, anteriorSubir, ultimoPulsoSubir)) {
    if (modoEdicion == EDITAR_MAX) {
      tempMax += PASO_SETPOINT;
      if (tempMax > LIMITE_SUPERIOR) tempMax = LIMITE_SUPERIOR;
    } else {
      tempMin += PASO_SETPOINT;
      if (tempMin > tempMax - MARGEN_MINIMO) tempMin = tempMax - MARGEN_MINIMO;
    }
    guardarConfiguracion();
  }

  // BAJAR
  if (botonPresionado(BTN_BAJAR, anteriorBajar, ultimoPulsoBajar)) {
    if (modoEdicion == EDITAR_MAX) {
      tempMax -= PASO_SETPOINT;
      if (tempMax < tempMin + MARGEN_MINIMO) tempMax = tempMin + MARGEN_MINIMO;
    } else {
      tempMin -= PASO_SETPOINT;
      if (tempMin < LIMITE_INFERIOR) tempMin = LIMITE_INFERIOR;
    }
    guardarConfiguracion();
  }
}

// Asegura valores validos (por si en memoria habia algo raro)
void validarLimites() {
  tempMax = constrain(tempMax, LIMITE_INFERIOR + MARGEN_MINIMO, LIMITE_SUPERIOR);
  tempMin = constrain(tempMin, LIMITE_INFERIOR, tempMax - MARGEN_MINIMO);
}

void guardarConfiguracion() {
  prefs.begin("configTemp", false);
  prefs.putInt("tempMax", tempMax);
  prefs.putInt("tempMin", tempMin);
  prefs.end();
  Serial.print("MAX: "); Serial.print(tempMax); Serial.print(" C  MIN: "); Serial.print(tempMin); Serial.println(" C");
}

// =====================================================================
// LCD 16x2
//   Fila 0: ">Max:60° Min:30°"  (la ">" marca el valor que se edita)
//   Fila 1: "45.3°  50% Calor"  (o "Error sensor" si el sensor falla)
void actualizarLCD() {
  char linea[17];

  snprintf(linea, sizeof(linea), "%cMax:%2d%c%cMin:%2d%c",
           modoEdicion == EDITAR_MAX ? '>' : ' ', tempMax, GRADO,
           modoEdicion == EDITAR_MIN ? '>' : ' ', tempMin, GRADO);
  imprimirLinea(0, linea);

  if (estado == ERROR_SENSOR) {
    imprimirLinea(1, "Error sensor");
  } else if (!hayLectura) {
    imprimirLinea(1, "Leyendo sensor..");
  } else {
    const char* textoEstado;
    if (estado == CALENTANDO)     textoEstado = "Calor";
    else if (estado == ENFRIANDO) textoEstado = "Frio";
    else                          textoEstado = "";

    snprintf(linea, sizeof(linea), "%4.1f%c %3d%% %s",
             tempActual, GRADO, (int)(porcentaje + 0.5), textoEstado);
    imprimirLinea(1, linea);
  }
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
