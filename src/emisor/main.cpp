// ===========================================================================
// EMISOR - T-Beam v1.2 + HDC1080 + GPS NEO-6M
// Mide temperatura y humedad del aire, les agrega la posicion GPS y lo manda
// por LoRa al gateway, que lo reenvia por WiFi a la nube.
// ===========================================================================
#include "radio_config.h"
#include <ClosedCube_HDC1080.h>
#include <TinyGPSPlus.h>

#define HDC1080_ADDR                0x40

// ---------------------------------------------------------------------------
// IDENTIFICADOR DEL EQUIPO
// Es el primer campo de cada trama. El gateway lo usa para saber de quien es.
// ---------------------------------------------------------------------------
#ifndef DEVICE_ID
#define DEVICE_ID                   "461879"
#endif

// Formato de la trama (JSON en una sola linea):
//
//   {"id":"461879","n":12,"t":22.41,"h":61.30,"lat":6.245259,"lon":-75.596510}
//
//   n   -> numero de trama. Permite detectar en la nube tramas perdidas.
//   t   -> temperatura del aire en C (promedio de las muestras del ciclo)
//   h   -> humedad relativa del aire en %
//   lat/lon -> posicion GPS
//
// Un dato que no es valido en este ciclo (sensor ausente o GPS sin fix) viaja
// como null, no como cero: un 0.0 de relleno se confundiria con una medida real
// (0 C es una temperatura posible y 0,0 es un punto real del mapa). Filtrar los
// null es trabajo del servicio de limpieza en la nube.

int counter = 0;

TinyGPSPlus gps;
ClosedCube_HDC1080 sensor;
// setupBoards() ya abre el UART del GPS (Serial1, 9600 baudios, pines de la placa).
HardwareSerial &comunicacion_gps = SerialGPS;

bool sensor_ok = false;         // el HDC1080 responde en el bus?
int fallos_seguidos = 0;        // lecturas invalidas consecutivas

// ---------------------------------------------------------------------------
// MAQUINA DE ESTADOS
//   0 Setup     -> cabecera del ciclo (el arranque del hardware va en setup())
//   1 Chirp     -> toma NUM_MUESTRAS lecturas crudas del HDC1080
//   2 Prunning  -> descarta las invalidas y promedia las que quedan
//   3 GPS       -> latitud y longitud
//   4 Bundling  -> arma la trama JSON (no la envia)
//   5 Send      -> transmite por LoRa y refresca el OLED
//   6 Sleep     -> espera troceada: ningun trozo pasa de 900 ms
// El ciclo es 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 0. Cada pasada de loop()
// ejecuta un solo estado, asi que la maquina nunca se queda bloqueada mas de lo
// que dura el estado en curso.
// ---------------------------------------------------------------------------
enum Estado : uint8_t {
    ESTADO_SETUP    = 0,
    ESTADO_CHIRP    = 1,
    ESTADO_PRUNNING = 2,
    ESTADO_GPS      = 3,
    ESTADO_BUNDLING = 4,
    ESTADO_SEND     = 5,
    ESTADO_SLEEP    = 6
};

static Estado estado = ESTADO_SETUP;

// El periodo se mide desde el arranque del ciclo (estado 0), no desde el final
// del envio: asi cada trama sale cada PERIODO_CICLO_MS exactos.
#define PERIODO_CICLO_MS            10000UL
#define SLEEP_TROZO_MS              900UL

static unsigned long sleep_restante  = 0;
static unsigned long ciclo_inicio_ms = 0;

// Datos que recogen PRUNNING y GPS y que empaqueta BUNDLING.
static double dato_temperatura = 0.0;
static double dato_humedad     = 0.0;
static double dato_latitud     = 0.0;
static double dato_longitud    = 0.0;
static bool   dato_temp_valido = false;
static bool   dato_gps_valido  = false;

// Muestreo del estado CHIRP: se guardan las lecturas crudas y el promedio se
// calcula en PRUNNING, para que cada etapa haga una sola cosa.
#define NUM_MUESTRAS                3
#define MUESTRA_GAP_MS              200UL

// Rangos fisicos del HDC1080. Las lecturas fallidas del bus ya llegan como NAN
// (ver leerCrudoHdc), esto es una segunda barrera contra valores absurdos.
#define TEMP_MIN                    -40.0
#define TEMP_MAX                    125.0
#define HUM_MIN                     0.0
#define HUM_MAX                     100.0

static double  muestras_temp[NUM_MUESTRAS];
static double  muestras_hum[NUM_MUESTRAS];
static uint8_t muestra_idx = 0;         // lecturas tomadas en este ciclo

// La trama vive entre BUNDLING (que la arma) y SEND (que la transmite).
static char payload[LORA_MAX_PAYLOAD + 1];
static bool payload_listo = false;      // false si no cupo: no se transmite

// El NEO-6M manda posicion cada segundo; una de mas de 3 s ya no es actual.
#define GPS_EDAD_MAX_MS             3000UL


static void smartDelay(unsigned long ms)
{
    unsigned long start = millis();
    do {
        while (comunicacion_gps.available())
            gps.encode(comunicacion_gps.read());
    } while (millis() - start < ms);
}

// La libreria ClosedCube no comprueba nada: begin() "funciona" aunque el sensor
// no este conectado. Preguntamos al bus directamente.
static bool sondearSensor()
{
    Wire.beginTransmission(HDC1080_ADDR);
    return Wire.endTransmission() == 0;
}

// Un timeout de I2C puede dejar el bus colgado para siempre. Reiniciarlo es la
// unica forma de recuperarlo sin resetear la placa.
static void reiniciarBusI2C()
{
    Serial.println("I2C: reiniciando bus...");
    Wire.end();
    delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    delay(10);
    sensor_ok = sondearSensor();
    if (sensor_ok) {
        sensor.begin(HDC1080_ADDR);
    }
    Serial.printf("I2C: bus reiniciado, HDC1080 %s\n", sensor_ok ? "OK" : "SIN RESPUESTA");
}

// La libreria ClosedCube tampoco avisa cuando una lectura falla: si el sensor no
// contesta, Wire.read() devuelve -1, la libreria lo arma como 0xFFFF y eso da
// 124.997 C / 99.998 %, que caen DENTRO del rango fisico y pasarian el filtro.
// Por eso el registro se lee aqui comprobando cada paso del bus.
#define HDC1080_REG_TEMPERATURA     0x00
#define HDC1080_REG_HUMEDAD         0x01
#define HDC1080_CONVERSION_MS       9       // 14 bits tarda ~6.5 ms (datasheet)

static bool leerCrudoHdc(uint8_t registro, uint16_t &crudo)
{
    // Escribir el puntero dispara la conversion.
    Wire.beginTransmission(HDC1080_ADDR);
    Wire.write(registro);
    if (Wire.endTransmission() != 0) {
        return false;
    }
    delay(HDC1080_CONVERSION_MS);
    if (Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2) != 2) {
        return false;
    }
    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    crudo = ((uint16_t)msb << 8) | lsb;
    // Los 2 bits bajos estan reservados y el sensor siempre los manda en 0:
    // si vienen en 1, el dato no salio del sensor (bus flotando en alto).
    return (crudo & 0x03) == 0;
}

// Una lectura fallida vuelve como NAN, que enRango() rechaza siempre.
static double leerTemperatura()
{
    uint16_t crudo;
    return leerCrudoHdc(HDC1080_REG_TEMPERATURA, crudo)
           ? (crudo / 65536.0) * 165.0 - 40.0
           : NAN;
}

static double leerHumedad()
{
    uint16_t crudo;
    return leerCrudoHdc(HDC1080_REG_HUMEDAD, crudo)
           ? (crudo / 65536.0) * 100.0
           : NAN;
}

// Cualquier comparacion con NAN da false, asi que NAN nunca esta en rango.
static bool enRango(double v, double minimo, double maximo)
{
    return v > minimo && v < maximo;
}

// ---------------------------------------------------------------------------
// ESTADO 0: SETUP
// ---------------------------------------------------------------------------
static void estadoSetup()
{
    ciclo_inicio_ms = millis();
    Serial.printf("\n======== Ciclo %d ========\n", counter);
    estado = ESTADO_CHIRP;
}

// ---------------------------------------------------------------------------
// ESTADO 1: CHIRP
// Toma NUM_MUESTRAS lecturas del HDC1080 y las guarda crudas. Se toma UNA
// muestra por pasada de loop(), para no bloquear la maquina todo el muestreo.
// Aqui no se filtra nada: decidir que lectura sirve es trabajo de PRUNNING.
// ---------------------------------------------------------------------------
static void estadoChirp()
{
    if (muestra_idx == 0) {
        Serial.println("---- CHIRP: tomando muestras ----");
    }

    if (!sensor_ok) {
        Serial.println("sensor ausente, no hay muestras que tomar");
        // Se intenta recuperar el bus una vez por ciclo: si el sensor se
        // reconecta, el siguiente ciclo ya mide.
        reiniciarBusI2C();
        muestra_idx = 0;
        estado = ESTADO_PRUNNING;
        return;
    }

    muestras_temp[muestra_idx] = leerTemperatura();
    muestras_hum[muestra_idx]  = leerHumedad();
    Serial.printf("Muestra %d: T=%.2f  H=%.2f\n", muestra_idx + 1,
                  muestras_temp[muestra_idx], muestras_hum[muestra_idx]);
    muestra_idx++;

    if (muestra_idx >= NUM_MUESTRAS) {
        estado = ESTADO_PRUNNING;
    } else {
        smartDelay(MUESTRA_GAP_MS);     // separar las muestras en el tiempo
    }
}

// ---------------------------------------------------------------------------
// ESTADO 2: PRUNNING
// Descarta las muestras fuera de rango y promedia las que sobreviven. Si no
// sobrevive ninguna, el dato queda invalido y la trama saldra con null.
// ---------------------------------------------------------------------------
static void estadoPrunning()
{
    uint8_t validas = 0;
    double suma_t = 0.0;
    double suma_h = 0.0;

    for (uint8_t i = 0; i < muestra_idx; i++) {
        if (enRango(muestras_temp[i], TEMP_MIN, TEMP_MAX) &&
            enRango(muestras_hum[i], HUM_MIN, HUM_MAX)) {
            suma_t += muestras_temp[i];
            suma_h += muestras_hum[i];
            validas++;
        } else {
            Serial.printf("---- PRUNNING: descartada muestra %d (T=%.2f H=%.2f)\n",
                          i + 1, muestras_temp[i], muestras_hum[i]);
        }
    }

    // Varias lecturas basura seguidas suelen ser el bus colgado.
    if (sensor_ok && validas == 0) {
        if (++fallos_seguidos >= 3) {
            reiniciarBusI2C();
            fallos_seguidos = 0;
        }
    } else {
        fallos_seguidos = 0;
    }

    dato_temp_valido = (validas > 0);
    if (dato_temp_valido) {
        dato_temperatura = suma_t / validas;
        dato_humedad     = suma_h / validas;
        Serial.printf("---- PRUNNING: T=%.2f  H=%.2f (%d de %d muestras)\n",
                      dato_temperatura, dato_humedad, validas, muestra_idx);
    } else {
        Serial.println("---- PRUNNING: ninguna muestra valida, se enviara null");
    }

    muestra_idx = 0;
    estado = ESTADO_GPS;
}

// ---------------------------------------------------------------------------
// ESTADO 3: GPS
// No bloquea esperando el fix: el GPS se va leyendo en cada smartDelay del
// ciclo, aqui solo se mira lo que haya hasta ahora.
// ---------------------------------------------------------------------------
static void estadoGps()
{
    // TinyGPSPlus nunca invalida la posicion: si se pierde el fix, isValid()
    // sigue en true con la ultima posicion conocida. Se exige ademas que sea
    // reciente; si no, se manda null como cuando no hay fix.
    dato_gps_valido = gps.location.isValid() &&
                      gps.location.age() < GPS_EDAD_MAX_MS;

    if (dato_gps_valido) {
        dato_latitud  = gps.location.lat();
        dato_longitud = gps.location.lng();
        Serial.printf("---- GPS: lat=%.6f lon=%.6f (%u satelites)\n",
                      dato_latitud, dato_longitud, gps.satellites.value());
    } else if (gps.location.isValid()) {
        Serial.printf("---- GPS: fix perdido hace %lu ms (%u satelites), se enviara null\n",
                      (unsigned long)gps.location.age(), gps.satellites.value());
    } else {
        Serial.printf("---- GPS: sin fix todavia (%u satelites), se enviara null\n",
                      gps.satellites.value());
    }

    estado = ESTADO_BUNDLING;
}

// ---------------------------------------------------------------------------
// ESTADO 4: BUNDLING
// Arma el JSON con lo que dejaron PRUNNING y GPS. Solo lo arma: transmitirlo es
// cosa de SEND.
// ---------------------------------------------------------------------------
static void estadoBundling()
{
    char campo_t[16]   = "null";
    char campo_h[16]   = "null";
    char campo_lat[20] = "null";
    char campo_lon[20] = "null";

    if (dato_temp_valido) {
        snprintf(campo_t, sizeof(campo_t), "%.2f", dato_temperatura);
        snprintf(campo_h, sizeof(campo_h), "%.2f", dato_humedad);
    }
    if (dato_gps_valido) {
        snprintf(campo_lat, sizeof(campo_lat), "%.6f", dato_latitud);
        snprintf(campo_lon, sizeof(campo_lon), "%.6f", dato_longitud);
    }

    int total = snprintf(payload, sizeof(payload),
                         "{\"id\":\"%s\",\"n\":%d,\"t\":%s,\"h\":%s,\"lat\":%s,\"lon\":%s}",
                         DEVICE_ID, counter, campo_t, campo_h, campo_lat, campo_lon);

    Serial.printf("---- BUNDLING: %d bytes ----\n", total);
    Serial.println(payload);

    // Una trama truncada es JSON roto: el gateway la descartaria igual, asi que
    // ni se gasta aire en ella.
    payload_listo = (total > 0 && total <= LORA_MAX_PAYLOAD);
    if (!payload_listo) {
        Serial.printf("---- ERROR: %d bytes superan los %d de LoRa, la trama no se envia\n",
                      total, LORA_MAX_PAYLOAD);
    }

    estado = ESTADO_SEND;
}

// ---------------------------------------------------------------------------
// ESTADO 5: SEND
// Transmite la trama, refresca el OLED y cierra el ciclo. endPacket() devuelve
// 1 si la radio acepto el paquete: no confirma que el gateway lo recibiera.
// ---------------------------------------------------------------------------
static void estadoSend()
{
    bool enviado = false;
    if (payload_listo) {
        LoRa.beginPacket();
        LoRa.print(payload);
        enviado = (LoRa.endPacket() == 1);
    }

    Serial.println(enviado ? "---- SEND: trama enviada por LoRa ----"
                           : "---- SEND: FALLO la transmision LoRa ----");

    if (u8g2) {
        char buf[40];
        u8g2->clearBuffer();
        u8g2->drawStr(0, 12, enviado ? "LoRa TX: OK" : "LoRa TX: FALLO");
        snprintf(buf, sizeof(buf), "Trama: %d", counter);
        u8g2->drawStr(0, 26, buf);
        if (dato_temp_valido) {
            snprintf(buf, sizeof(buf), "T:%.1fC  H:%.0f%%", dato_temperatura, dato_humedad);
        } else {
            snprintf(buf, sizeof(buf), "Sensor: sin dato");
        }
        u8g2->drawStr(0, 40, buf);
        snprintf(buf, sizeof(buf), "GPS: %s", dato_gps_valido ? "fix" : "sin fix");
        u8g2->drawStr(0, 54, buf);
        u8g2->sendBuffer();
    }
    counter++;

    unsigned long transcurrido = millis() - ciclo_inicio_ms;
    sleep_restante = (transcurrido < PERIODO_CICLO_MS)
                     ? (PERIODO_CICLO_MS - transcurrido)
                     : 0;

    estado = ESTADO_SLEEP;
}

// ---------------------------------------------------------------------------
// ESTADO 6: SLEEP
// Cada pasada gasta como mucho 900 ms; el GPS se sigue leyendo mientras tanto.
// ---------------------------------------------------------------------------
static void estadoSleep()
{
    unsigned long trozo = (sleep_restante > SLEEP_TROZO_MS) ? SLEEP_TROZO_MS
                                                            : sleep_restante;
    smartDelay(trozo);
    sleep_restante -= trozo;

    if (sleep_restante == 0) {
        estado = ESTADO_SETUP;
    }
}

void setup()
{
    setupBoards();

    // setupBoards() levanta un punto de acceso WiFi abierto llamado "T-Beam".
    // El emisor solo usa LoRa: se apaga el WiFi (menos consumo, nada abierto).
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    smartDelay(1500);

    sensor.begin(HDC1080_ADDR);
    sensor_ok = sondearSensor();
    Serial.printf("HDC1080 @ 0x%02X: %s\n", HDC1080_ADDR, sensor_ok ? "OK" : "NO RESPONDE");

    Serial.println("Emisor LoRa");
    if (!iniciarRadio()) {
        Serial.println("No arranco la radio LoRa!");
        while (1);
    }
    mostrarConfigRadio();

    estado = ESTADO_SETUP;
}

void loop()
{
    switch (estado) {
    case ESTADO_SETUP:    estadoSetup();    break;
    case ESTADO_CHIRP:    estadoChirp();    break;
    case ESTADO_PRUNNING: estadoPrunning(); break;
    case ESTADO_GPS:      estadoGps();      break;
    case ESTADO_BUNDLING: estadoBundling(); break;
    case ESTADO_SEND:     estadoSend();     break;
    case ESTADO_SLEEP:    estadoSleep();    break;
    default:              estado = ESTADO_SETUP; break;
    }
}
