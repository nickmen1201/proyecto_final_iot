// ===========================================================================
// GATEWAY - segunda placa LoRa con WiFi
// Escucha las tramas LoRa del emisor y las reenvia por HTTP POST al agente que
// corre en la EC2. Le agrega a cada trama la calidad de la senal con la que
// llego (rssi, snr), que es util para la analitica del enlace.
//
//   emisor --LoRa--> gateway --WiFi/HTTP--> IoT Agent JSON (EC2:5555) --> Orion
//
// El IoT Agent JSON de FIWARE espera el id del dispositivo en la URL, no en el
// cuerpo:
//
//   LoRa:  {"id":"461879","n":12,"t":22.41,"h":61.30,"lat":...,"lon":...}
//   HTTP:  POST /iot/json?k=<apikey>&i=461879
//          {"n":12,"t":22.41,"h":61.30,"lat":...,"lon":...,"rssi":-87,"snr":9.5}
//
// Ademas "id" es un nombre reservado en NGSI: si viajara en el cuerpo, Orion
// rechazaria la actualizacion entera.
//
// Si el WiFi o la nube no estan disponibles, las tramas se guardan en una cola
// y se reintentan, en vez de perderse.
// ===========================================================================
#include "radio_config.h"
#include <HTTPClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Falta include/secrets.h: copiar include/secrets.example.h y rellenarlo"
#endif

// Solo se reenvian tramas que empiecen asi. Con sync word propio y CRC ya casi
// no entra ruido, pero esto evita mandar a la nube algo que no es nuestro.
#define PREFIJO_TRAMA               "{\"id\":\""

// ---------------------------------------------------------------------------
// MODO DE PRUEBA
// Con 1, el gateway fabrica una trama sintetica cada PRUEBA_PERIODO_MS y la
// manda al agente como si hubiera llegado por LoRa. Sirve para probar la nube
// con una sola placa, sin el emisor. Las tramas LoRa reales se siguen
// recibiendo. Tambien se activa sin tocar el codigo con -DMODO_PRUEBA=1.
//
// Usa el mismo id que el emisor para caer en la misma entidad de Orion; los
// datos sinteticos se reconocen por rssi/snr en null. Al terminar la prueba,
// volver a 0 (o borrar esos registros en la nube).
// ---------------------------------------------------------------------------
#ifndef MODO_PRUEBA
#define MODO_PRUEBA                 0
#endif

#if MODO_PRUEBA
#define PRUEBA_ID                   "461879"
#define PRUEBA_PERIODO_MS           10000UL
#define PRUEBA_LAT                  6.245259
#define PRUEBA_LON                  -75.596510
static unsigned long enviadas_prueba = 0;
#endif

// ---------------------------------------------------------------------------
// COLA DE REENVIO
// Buffer circular: si se llena, se descarta la trama mas vieja. Con una trama
// cada 10 s, 20 huecos aguantan unos 3 minutos sin WiFi.
// ---------------------------------------------------------------------------
#define COLA_TAM                    20
#define TRAMA_MAX                   (LORA_MAX_PAYLOAD + 48)   // + rssi/snr
#define ID_MAX                      24

struct Medida {
    char id[ID_MAX];            // va en la URL (i=...)
    char cuerpo[TRAMA_MAX];     // va en el cuerpo del POST
};

static Medida  cola[COLA_TAM];
static uint8_t cola_inicio = 0;     // la mas vieja
static uint8_t cola_cuenta = 0;

#define REINTENTO_MS                5000UL
#define HTTP_TIMEOUT_MS             4000
#define WIFI_REINTENTO_MS           10000UL

static unsigned long ultimo_intento_ms   = 0;
static unsigned long ultimo_wifi_ms      = 0;

// Estado para el OLED
static unsigned long recibidas   = 0;
static unsigned long reenviadas  = 0;
static int           ultimo_http = 0;
static int           ultimo_rssi = 0;
static bool          oled_sucio  = true;

// Mientras secrets.h tenga el SSID de la plantilla, el gateway solo escucha
// LoRa: asi se puede probar el enlace sin nube y sin un log lleno de
// reintentos de WiFi.
static const bool wifi_configurado = strcmp(WIFI_SSID, "nombre-de-la-red") != 0;

// Para detectar tramas perdidas con el campo n del emisor.
static long ultimo_n = -1;

// Cada cuanto se imprime que la radio sigue escuchando, para distinguir "no
// llega nada" de "el gateway se colgo".
#define LATIDO_MS                   10000UL
static unsigned long ultimo_latido_ms = 0;

// Reserva el siguiente hueco de la cola y lo devuelve para rellenarlo.
static Medida *encolar()
{
    if (cola_cuenta == COLA_TAM) {
        Serial.println("COLA: llena, se descarta la trama mas vieja");
        cola_inicio = (cola_inicio + 1) % COLA_TAM;
        cola_cuenta--;
    }
    uint8_t fin = (cola_inicio + cola_cuenta) % COLA_TAM;
    cola_cuenta++;
    return &cola[fin];
}

static void desencolar()
{
    cola_inicio = (cola_inicio + 1) % COLA_TAM;
    cola_cuenta--;
}

static void conectarWiFi()
{
    Serial.printf("WiFi: conectando a \"%s\"...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ultimo_wifi_ms = millis();
}

// El id viaja en la URL sin codificar: solo se aceptan caracteres que no
// necesitan escaparse, para que un id raro no pueda meter "&k=..." en la URL.
static bool idValido(const char *id, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_') {
            return false;
        }
    }
    return true;
}

// Valida una trama con formato LoRa, separa el id del resto, le agrega la
// calidad de la senal y la mete en la cola. "calidad" son los campos
// rssi/snr ya formateados, p. ej.  "rssi":-87,"snr":9.5
static void encolarTrama(const char *trama, int n, const char *calidad)
{
    const size_t prefijo = strlen(PREFIJO_TRAMA);
    if (n < 2 || strncmp(trama, PREFIJO_TRAMA, prefijo) != 0 || trama[n - 1] != '}') {
        Serial.println("  -> no es una trama nuestra, se ignora");
        return;
    }

    // trama = {"id":"461879",<resto>}
    //                ^id     ^cierre
    const char *id     = trama + prefijo;
    const char *cierre = strchr(id, '"');
    size_t id_len = cierre ? (size_t)(cierre - id) : 0;
    if (id_len == 0 || id_len >= ID_MAX || cierre[1] != ',' || !idValido(id, id_len)) {
        Serial.println("  -> trama sin id valido, se ignora");
        return;
    }
    const char *resto = cierre + 2;
    int resto_len = (int)((trama + n - 1) - resto);   // sin la llave de cierre
    if (resto_len <= 0) {
        Serial.println("  -> trama sin medidas, se ignora");
        return;
    }

    // El id va a la URL; el cuerpo es el resto con rssi/snr al final.
    Medida m;
    memcpy(m.id, id, id_len);
    m.id[id_len] = '\0';
    snprintf(m.cuerpo, sizeof(m.cuerpo), "{%.*s,%s}", resto_len, resto, calidad);
    Serial.printf("  -> para el agente: i=%s  %s\n", m.id, m.cuerpo);

    // Sin WiFi configurado no hay a donde mandarla: encolarla solo llenaria la
    // cola y el log de "COLA: llena".
    if (!wifi_configurado) {
        return;
    }
    *encolar() = m;
}

// Compara el campo n con el de la trama anterior para ver si se perdio alguna
// en el aire. Si n baja, el emisor se reinicio (su contador vuelve a 0).
static void revisarSecuencia(const char *trama)
{
    const char *campo = strstr(trama, "\"n\":");
    if (!campo) {
        return;
    }
    long n = atol(campo + 4);
    if (ultimo_n >= 0) {
        if (n == ultimo_n + 1) {
            Serial.println("  -> secuencia OK");
        } else if (n > ultimo_n + 1) {
            Serial.printf("  -> PERDIDAS %ld trama(s) entre n=%ld y n=%ld\n",
                          n - ultimo_n - 1, ultimo_n, n);
        } else {
            Serial.printf("  -> n bajo de %ld a %ld: el emisor se reinicio\n", ultimo_n, n);
        }
    }
    ultimo_n = n;
}

// Lee un paquete LoRa si lo hay y lo pasa a encolarTrama() con su rssi/snr.
// parsePacket() se llama en cada pasada de loop(): asi es como la libreria
// espera que se use. Limitacion conocida: mientras un POST bloquea, la radio
// puede dejar de escuchar y perder una trama.
static void recibirLoRa()
{
    int tam = LoRa.parsePacket();
    if (tam == 0) {
        return;
    }

    char trama[LORA_MAX_PAYLOAD + 1];
    int n = 0;
    while (LoRa.available() && n < LORA_MAX_PAYLOAD) {
        trama[n++] = (char)LoRa.read();
    }
    trama[n] = '\0';

    int   rssi = LoRa.packetRssi();
    float snr  = LoRa.packetSnr();
    recibidas++;
    ultimo_rssi = rssi;
    oled_sucio = true;

    Serial.printf("\nLoRa RX #%lu (%d bytes, RSSI %d dBm, SNR %.1f dB): %s\n",
                  recibidas, n, rssi, snr, trama);
    revisarSecuencia(trama);

    char calidad[32];
    snprintf(calidad, sizeof(calidad), "\"rssi\":%d,\"snr\":%.1f", rssi, snr);
    encolarTrama(trama, n, calidad);
}

// Sin pantalla, este latido es lo que confirma que el gateway sigue vivo y
// escuchando aunque no llegue nada.
static void latido()
{
    if (millis() - ultimo_latido_ms < LATIDO_MS) {
        return;
    }
    ultimo_latido_ms = millis();
    Serial.printf("[escuchando LoRa... recibidas: %lu, ultimo RSSI: %d]\n",
                  recibidas, ultimo_rssi);
}

#if MODO_PRUEBA
// Arma una trama con el mismo formato que la del emisor y la pasa por el mismo
// camino que una recibida por LoRa (validacion incluida). Los valores varian un
// poco de una trama a otra para que en las graficas se vea que llegan nuevas.
// rssi/snr van como null: no hubo radio, y un 0 pareceria una medida real.
static void generarTramaPrueba()
{
    static unsigned long ultima_ms = 0;
    if (enviadas_prueba != 0 && millis() - ultima_ms < PRUEBA_PERIODO_MS) {
        return;
    }
    ultima_ms = millis();

    char trama[LORA_MAX_PAYLOAD + 1];
    int n = snprintf(trama, sizeof(trama),
                     "{\"id\":\"%s\",\"n\":%lu,\"t\":%.2f,\"h\":%.2f,\"lat\":%.6f,\"lon\":%.6f}",
                     PRUEBA_ID, enviadas_prueba,
                     22.0 + (enviadas_prueba % 10) * 0.1,
                     60.0 + (enviadas_prueba % 7) * 0.5,
                     PRUEBA_LAT, PRUEBA_LON);
    Serial.printf("PRUEBA: trama sintetica %s\n", trama);
    encolarTrama(trama, n, "\"rssi\":null,\"snr\":null");
    enviadas_prueba++;
    oled_sucio = true;
}
#endif

// Intenta mandar la trama mas vieja de la cola. Devuelve true si la nube la
// acepto (respuesta 2xx).
static bool reenviarUna()
{
    const Medida &m = cola[cola_inicio];
    String url = String("http://") + AGENTE_HOST + ":" + AGENTE_PUERTO +
                 "/iot/json?k=" + AGENTE_APIKEY + "&i=" + m.id;

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int codigo = http.POST((uint8_t *)m.cuerpo, strlen(m.cuerpo));
    ultimo_http = codigo;
    oled_sucio = true;

    if (codigo > 0) {
        Serial.printf("HTTP POST %s %s -> %d\n", url.c_str(), m.cuerpo, codigo);
    } else {
        Serial.printf("HTTP POST %s -> error: %s\n", url.c_str(),
                      HTTPClient::errorToString(codigo).c_str());
    }
    http.end();

    if (codigo >= 200 && codigo < 300) {
        return true;
    }
    if (codigo >= 400 && codigo < 500) {
        // La nube rechazo el contenido: reintentarla daria el mismo error y
        // bloquearia la cola. Se descarta y queda en el log.
        Serial.printf("  -> rechazada (%d), se descarta\n", codigo);
        desencolar();
    }
    return false;
}

static void vaciarCola()
{
    if (cola_cuenta == 0 || WiFi.status() != WL_CONNECTED) {
        return;
    }
    // Tras un fallo de red se espera REINTENTO_MS antes de volver a probar.
    if (ultimo_intento_ms != 0 && millis() - ultimo_intento_ms < REINTENTO_MS) {
        return;
    }

    if (reenviarUna()) {
        desencolar();
        reenviadas++;
        ultimo_intento_ms = 0;          // la siguiente puede salir ya
    } else {
        ultimo_intento_ms = millis();
    }
}

static void refrescarOled()
{
    if (!u8g2 || !oled_sucio) {
        return;
    }
    oled_sucio = false;

    char buf[40];
    u8g2->clearBuffer();
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "WiFi: %s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(buf, sizeof(buf), "WiFi: desconectado");
    }
    u8g2->drawStr(0, 12, buf);
#if MODO_PRUEBA
    // En modo prueba se cambia esta linea para que no se olvide activado.
    snprintf(buf, sizeof(buf), "PRUEBA: %lu  RX: %lu", enviadas_prueba, recibidas);
#else
    snprintf(buf, sizeof(buf), "RX: %lu  RSSI: %d", recibidas, ultimo_rssi);
#endif
    u8g2->drawStr(0, 26, buf);
    snprintf(buf, sizeof(buf), "Nube: %lu  HTTP: %d", reenviadas, ultimo_http);
    u8g2->drawStr(0, 40, buf);
    snprintf(buf, sizeof(buf), "En cola: %u", cola_cuenta);
    u8g2->drawStr(0, 54, buf);
    u8g2->sendBuffer();
}

void setup()
{
    setupBoards();

    // setupBoards() levanta un punto de acceso abierto llamado "T-Beam". Aqui
    // la placa tiene que ser cliente de la red WiFi, no punto de acceso.
    WiFi.softAPdisconnect(true);
    if (wifi_configurado) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        conectarWiFi();
    } else {
        WiFi.mode(WIFI_OFF);
        Serial.println("WiFi: secrets.h tiene la red de ejemplo, el gateway solo "
                       "escucha LoRa (no reenvia a la nube)");
    }

    Serial.println("Gateway LoRa -> HTTP");
    if (!iniciarRadio()) {
        Serial.println("No arranco la radio LoRa!");
        while (1);
    }
    mostrarConfigRadio();
    Serial.printf("Destino: http://%s:%d/iot/json?k=%s&i=<id>\n",
                  AGENTE_HOST, AGENTE_PUERTO, AGENTE_APIKEY);

#if MODO_PRUEBA
    Serial.printf("*** MODO PRUEBA: trama sintetica cada %lu ms con id %s ***\n",
                  PRUEBA_PERIODO_MS, PRUEBA_ID);
#endif
}

void loop()
{
    static wl_status_t wifi_previo = WL_IDLE_STATUS;

    recibirLoRa();
    latido();
#if MODO_PRUEBA
    generarTramaPrueba();
#endif

    if (!wifi_configurado) {
        return;
    }

    wl_status_t wifi = WiFi.status();
    if (wifi != wifi_previo) {
        if (wifi == WL_CONNECTED) {
            Serial.printf("WiFi: conectado, IP %s\n", WiFi.localIP().toString().c_str());
        } else if (wifi_previo == WL_CONNECTED) {
            Serial.println("WiFi: conexion perdida");
        }
        wifi_previo = wifi;
        oled_sucio = true;
    }
    if (wifi != WL_CONNECTED && millis() - ultimo_wifi_ms > WIFI_REINTENTO_MS) {
        // El codigo ayuda a diagnosticar: 1 = SSID no encontrado (nombre mal o
        // red solo de 5 GHz), 4 = fallo de conexion, 0/6 = no termino de
        // conectar (con clave mala el ESP32 suele quedarse en 6).
        Serial.printf("WiFi: sin conexion (estado %d), reintentando\n", wifi);
        WiFi.disconnect();
        conectarWiFi();
    }

    vaciarCola();
    refrescarOled();
}
