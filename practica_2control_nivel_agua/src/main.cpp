#include <Arduino.h>

/*
  =====================================================================
  CONTROL DE NIVEL DE AGUA CON ESP32  (bote de 17 x 11 cm)
  CICLO CONTINUO: LLENAR HASTA MAX -> VACIAR HASTA MIN -> REPETIR
  =====================================================================
  Funcionamiento:
   - Un sensor ultrasonico (HC-SR04) mide la distancia entre el sensor
     (montado arriba del bote) y la superficie del agua. Con eso se
     calcula el nivel en % y en litros.
   - El sistema trabaja en un ciclo sin fin:
       1) LLENANDO: la bomba de llenado (IN2) esta encendida hasta que
          el nivel llega al porcentaje MAXIMO (ej. 80%).
       2) Al llegar al MAX se apaga la bomba de llenado y se enciende la
          bomba de extraccion (IN1).
       3) VACIANDO: la bomba de extraccion sigue encendida hasta que el
          nivel baja al porcentaje MINIMO (ej. 20%).
       4) Al llegar al MIN se apaga la extraccion y se enciende de nuevo
          el llenado. Y asi sucesivamente.
   - Nunca estan las 2 bombas encendidas al mismo tiempo. Al cambiar de
     una a otra hay una pausa corta con las dos apagadas.
   - Si el sensor deja de responder, se apagan las 2 bombas por
     seguridad (para que no se desborde el bote) y la LCD lo indica.

  Botones:
   - MODO  -> cada pulsacion cambia entre editar el MAX y editar el MIN.
   - SUBIR -> sube 1% el valor del modo seleccionado.
   - BAJAR -> baja 1% el valor del modo seleccionado.
   Los valores se guardan en memoria y se conservan aunque se vaya la luz.

  LCD 16x2:
     +----------------+
     |Max:80% Min:20% |   <- limites configurados
     |[Max]  75% 0.86L|   <- modo seleccionado, % actual y litros
     +----------------+

  MEDIDAS DEL BOTE (cilindro de pie):
   - Sensor ubicado a 16.5 cm del fondo
   - 100% = agua a 12 cm del fondo  -> distancia sensor-agua = 4.5 cm
   - 0%   = agua en el fondo        -> distancia sensor-agua = 16.5 cm
   - 1% = 0.12 cm de agua; volumen al 100% = ~1.14 L

  LIBRERIAS:
   - LiquidCrystal_I2C (Frank de Brabander)
   - Preferences (ya viene con el core de ESP32)

  CONEXIONES (en el adaptador: fila S = senal, V = voltaje, G = GND;
  el jumper JUMP del adaptador debe estar en 5V):
   - HC-SR04:  TRIG -> GPIO 5,  ECHO -> GPIO 18 (con divisor 1k/2k)
   - Reles:    IN1 -> GPIO 26 (EXTRACCION), IN2 -> GPIO 27 (LLENADO)
   - LCD I2C:  SDA -> GPIO 21,  SCL -> GPIO 22, VCC -> 5V
   - Botones (a GND): MODO -> GPIO 32, SUBIR -> GPIO 33, BAJAR -> GPIO 25
  =====================================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ---------------------- CONFIGURACION DE PINES ----------------------
#define TRIG_PIN      5
#define ECHO_PIN      18

#define RELAY_EXTRAE  26   // IN1 - bomba que SACA agua
#define RELAY_LLENA   27   // IN2 - bomba que METE agua

#define BTN_MODO      32
#define BTN_SUBIR     33
#define BTN_BAJAR     25

#define LCD_SDA       21
#define LCD_SCL       22

// La mayoria de modulos de rele son "activos en BAJO" (LOW).
// Si tus reles funcionan al reves, cambia LOW por HIGH.
#define RELAY_ACTIVO   LOW
#define RELAY_INACTIVO (!RELAY_ACTIVO)

// ---------------------- MEDIDAS DEL BOTE ------------------------------
const float ALTURA_SENSOR_CM     = 16.5;  // altura del sensor desde el fondo
const float ALTURA_AGUA_LLENO_CM = 12.0;  // altura del agua que es 100%
const float ALTURA_AGUA_VACIO_CM = 0.0;   // altura del agua que es 0%
const float DIAMETRO_BOTE_CM     = 11.0;

const float DIST_TANQUE_LLENO_CM  = ALTURA_SENSOR_CM - ALTURA_AGUA_LLENO_CM;  // 4.5 cm
const float DIST_TANQUE_VACIO_CM  = ALTURA_SENSOR_CM - ALTURA_AGUA_VACIO_CM;  // 16.5 cm
const float RADIO_BOTE_CM         = DIAMETRO_BOTE_CM / 2.0;
const float AREA_BASE_CM2         = PI * RADIO_BOTE_CM * RADIO_BOTE_CM;
const float TANQUE_VOLUMEN_LITROS = AREA_BASE_CM2 * (ALTURA_AGUA_LLENO_CM - ALTURA_AGUA_VACIO_CM) / 1000.0;  // ~1.14 L

// ---------------------- PARAMETROS DE CONTROL --------------------------
const int PASO_SETPOINT   = 1;    // cuanto cambia el MAX/MIN por pulsacion (%)
const int MARGEN_MINIMO   = 10;   // separacion minima entre MAX y MIN (%)
const unsigned long PAUSA_CAMBIO_BOMBA_MS = 1000;  // ambas apagadas al cambiar de bomba
const int MAX_ERRORES_SENSOR = 4; // lecturas fallidas seguidas antes de parar (~2 s)

// ---------------------- OBJETOS GLOBALES --------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);  // si no se ve nada, prueba 0x3F
Preferences prefs;

// ---------------------- ESTADO DEL SISTEMA ------------------------------
enum EstadoCiclo { INICIANDO, LLENANDO, VACIANDO, ERROR_SENSOR };
EstadoCiclo estado = INICIANDO;

int nivelMax = 80;   // % - se carga de memoria en setup()
int nivelMin = 20;   // % - se carga de memoria en setup()

enum ModoEdicion { EDITAR_MAX, EDITAR_MIN };
ModoEdicion modoEdicion = EDITAR_MAX;

float nivelActualPorc   = 0.0;
float nivelActualLitros = 0.0;
int   erroresSensor     = 0;

unsigned long momentoCambioEstado = 0;

// Botones (deteccion de flanco + antirrebote)
bool anteriorModo  = HIGH;
bool anteriorSubir = HIGH;
bool anteriorBajar = HIGH;
unsigned long ultimoPulsoModo  = 0;
unsigned long ultimoPulsoSubir = 0;
unsigned long ultimoPulsoBajar = 0;
const unsigned long DEBOUNCE_MS = 200;

unsigned long ultimaLecturaSensor = 0;
const unsigned long INTERVALO_SENSOR_MS = 500;

unsigned long ultimaActualizacionLCD = 0;
const unsigned long INTERVALO_LCD_MS = 300;

// ---------------------- DECLARACION DE FUNCIONES -----------------------
float medirDistanciaCM();
void actualizarNivelAgua();
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
void actualizarCiclo();
void cambiarEstado(EstadoCiclo nuevo);
void aplicarBombas();
void apagarBombas();
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

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(RELAY_EXTRAE, OUTPUT);
  pinMode(RELAY_LLENA, OUTPUT);
  apagarBombas();

  pinMode(BTN_MODO,  INPUT_PULLUP);
  pinMode(BTN_SUBIR, INPUT_PULLUP);
  pinMode(BTN_BAJAR, INPUT_PULLUP);

  // LCD: pequena espera para que arranque, e I2C a velocidad estandar
  delay(500);
  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.setClock(100000);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  // Cargar MAX y MIN guardados (si no hay nada guardado: 80 y 20)
  prefs.begin("config", false);
  nivelMax = prefs.getInt("nivelMax", 80);
  nivelMin = prefs.getInt("nivelMin", 20);
  prefs.end();
  validarLimites();

  Serial.println("=== Control de nivel - ciclo continuo ===");
  Serial.print("Distancia al 100%: "); Serial.print(DIST_TANQUE_LLENO_CM); Serial.println(" cm");
  Serial.print("Distancia al 0%:   "); Serial.print(DIST_TANQUE_VACIO_CM); Serial.println(" cm");
  Serial.print("Volumen al 100%:   "); Serial.print(TANQUE_VOLUMEN_LITROS, 3); Serial.println(" L");
  Serial.print("MAX: "); Serial.print(nivelMax); Serial.print("%  MIN: "); Serial.print(nivelMin); Serial.println("%");

  delay(1000);
  lcd.clear();
}

// =====================================================================
void loop() {
  manejarBotones();

  if (millis() - ultimaLecturaSensor >= INTERVALO_SENSOR_MS) {
    ultimaLecturaSensor = millis();
    actualizarNivelAgua();
    actualizarCiclo();
  }

  if (millis() - ultimaActualizacionLCD >= INTERVALO_LCD_MS) {
    ultimaActualizacionLCD = millis();
    actualizarLCD();
  }
}

// =====================================================================
// SENSOR: mide 5 veces y devuelve la mediana (filtra lecturas erroneas)
float medirDistanciaCM() {
  const int NUM_MUESTRAS = 5;
  float muestras[NUM_MUESTRAS];
  int validas = 0;

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duracion = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duracion > 0) {
      muestras[validas] = duracion * 0.0343 / 2.0;
      validas++;
    }
    delay(10);
  }

  if (validas == 0) return -1;

  for (int i = 0; i < validas - 1; i++) {
    for (int j = 0; j < validas - i - 1; j++) {
      if (muestras[j] > muestras[j + 1]) {
        float tmp = muestras[j];
        muestras[j] = muestras[j + 1];
        muestras[j + 1] = tmp;
      }
    }
  }
  return muestras[validas / 2];
}

void actualizarNivelAgua() {
  float distancia = medirDistanciaCM();

  if (distancia < 0) {
    erroresSensor++;
    Serial.println("Error leyendo el sensor ultrasonico");
    return;
  }
  erroresSensor = 0;

  float porcentaje = mapFloat(distancia, DIST_TANQUE_LLENO_CM, DIST_TANQUE_VACIO_CM, 100.0, 0.0);
  nivelActualPorc   = constrain(porcentaje, 0.0, 100.0);
  nivelActualLitros = (nivelActualPorc / 100.0) * TANQUE_VOLUMEN_LITROS;

  Serial.print("Distancia: ");   Serial.print(distancia, 1);
  Serial.print(" cm | Nivel: "); Serial.print(nivelActualPorc, 1);
  Serial.print(" % | ");         Serial.print(nivelActualLitros, 2);
  Serial.print(" L | Estado: "); Serial.println(nombreEstado());
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// =====================================================================
// CICLO: LLENANDO hasta MAX -> VACIANDO hasta MIN -> LLENANDO ...
void actualizarCiclo() {
  // Seguridad: si el sensor falla varias veces seguidas, parar todo
  if (erroresSensor >= MAX_ERRORES_SENSOR) {
    if (estado != ERROR_SENSOR) cambiarEstado(ERROR_SENSOR);
    apagarBombas();
    return;
  }

  switch (estado) {
    case INICIANDO:
    case ERROR_SENSOR:
      // Primera lectura valida (o el sensor se recupero): decidir por donde empezar
      if (erroresSensor == 0) {
        cambiarEstado(nivelActualPorc >= nivelMax ? VACIANDO : LLENANDO);
      }
      break;

    case LLENANDO:
      if (nivelActualPorc >= nivelMax) cambiarEstado(VACIANDO);
      break;

    case VACIANDO:
      if (nivelActualPorc <= nivelMin) cambiarEstado(LLENANDO);
      break;
  }

  aplicarBombas();
}

void cambiarEstado(EstadoCiclo nuevo) {
  estado = nuevo;
  momentoCambioEstado = millis();
  apagarBombas();   // las dos apagadas durante la pausa de cambio
  Serial.print(">>> Cambio de estado: "); Serial.println(nombreEstado());
}

void aplicarBombas() {
  // Durante la pausa despues de un cambio, ambas bombas quedan apagadas
  if (millis() - momentoCambioEstado < PAUSA_CAMBIO_BOMBA_MS) {
    apagarBombas();
    return;
  }

  if (estado == LLENANDO) {
    digitalWrite(RELAY_EXTRAE, RELAY_INACTIVO);   // primero apagar la otra
    digitalWrite(RELAY_LLENA,  RELAY_ACTIVO);
  } else if (estado == VACIANDO) {
    digitalWrite(RELAY_LLENA,  RELAY_INACTIVO);
    digitalWrite(RELAY_EXTRAE, RELAY_ACTIVO);
  } else {
    apagarBombas();
  }
}

void apagarBombas() {
  digitalWrite(RELAY_EXTRAE, RELAY_INACTIVO);
  digitalWrite(RELAY_LLENA,  RELAY_INACTIVO);
}

const char* nombreEstado() {
  switch (estado) {
    case LLENANDO:     return "LLENANDO";
    case VACIANDO:     return "VACIANDO";
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
    Serial.print("Modo: "); Serial.println(modoEdicion == EDITAR_MAX ? "Max" : "Min");
  }

  // SUBIR
  if (botonPresionado(BTN_SUBIR, anteriorSubir, ultimoPulsoSubir)) {
    if (modoEdicion == EDITAR_MAX) {
      nivelMax += PASO_SETPOINT;
      if (nivelMax > 100) nivelMax = 100;
    } else {
      nivelMin += PASO_SETPOINT;
      if (nivelMin > nivelMax - MARGEN_MINIMO) nivelMin = nivelMax - MARGEN_MINIMO;
    }
    guardarConfiguracion();
  }

  // BAJAR
  if (botonPresionado(BTN_BAJAR, anteriorBajar, ultimoPulsoBajar)) {
    if (modoEdicion == EDITAR_MAX) {
      nivelMax -= PASO_SETPOINT;
      if (nivelMax < nivelMin + MARGEN_MINIMO) nivelMax = nivelMin + MARGEN_MINIMO;
    } else {
      nivelMin -= PASO_SETPOINT;
      if (nivelMin < 0) nivelMin = 0;
    }
    guardarConfiguracion();
  }
}

// Asegura valores validos (por si en memoria habia algo raro)
void validarLimites() {
  nivelMax = constrain(nivelMax, MARGEN_MINIMO, 100);
  nivelMin = constrain(nivelMin, 0, nivelMax - MARGEN_MINIMO);
}

void guardarConfiguracion() {
  prefs.begin("config", false);
  prefs.putInt("nivelMax", nivelMax);
  prefs.putInt("nivelMin", nivelMin);
  prefs.end();
  Serial.print("MAX: "); Serial.print(nivelMax); Serial.print("%  MIN: "); Serial.print(nivelMin); Serial.println("%");
}

// =====================================================================
// LCD 16x2
//   Fila 0: "Max:80% Min:20% "
//   Fila 1: "[Max]  75% 0.86L" (o "Error sensor" si el sensor falla)
void actualizarLCD() {
  char linea[17];

  snprintf(linea, sizeof(linea), "Max:%d%% Min:%d%%", nivelMax, nivelMin);
  imprimirLinea(0, linea);

  if (estado == ERROR_SENSOR) {
    imprimirLinea(1, "Error sensor");
  } else {
    // [Max] o [Min] = modo seleccionado, luego % actual y litros actuales
    // El % se alinea a 3 cifras para que el texto no "brinque" al cambiar
    snprintf(linea, sizeof(linea), "[%s] %3d%% %.2fL",
             modoEdicion == EDITAR_MAX ? "Max" : "Min",
             (int)(nivelActualPorc + 0.5),
             nivelActualLitros);
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
