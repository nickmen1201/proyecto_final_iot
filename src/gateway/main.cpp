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

// Lee un paquete LoRa si lo hay, separa el id del resto, le agrega rssi/snr y
// lo mete en la cola.
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

    Serial.printf("LoRa RX (%d bytes, RSSI %d, SNR %.1f): %s\n", n, rssi, snr, trama);

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
    if (id_len == 0 || id_len >= ID_MAX || cierre[1] != ',') {
        Serial.println("  -> trama sin id valido, se ignora");
        return;
    }
    const char *resto = cierre + 2;
    int resto_len = (int)((trama + n - 1) - resto);   // sin la llave de cierre

    // El id va a la URL; el cuerpo es el resto con rssi/snr al final.
    Medida *m = encolar();
    memcpy(m->id, id, id_len);
    m->id[id_len] = '\0';
    snprintf(m->cuerpo, sizeof(m->cuerpo), "{%.*s,\"rssi\":%d,\"snr\":%.1f}",
             resto_len, resto, rssi, snr);
}

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
    snprintf(buf, sizeof(buf), "RX: %lu  RSSI: %d", recibidas, ultimo_rssi);
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
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    conectarWiFi();

    Serial.println("Gateway LoRa -> HTTP");
    if (!iniciarRadio()) {
        Serial.println("No arranco la radio LoRa!");
        while (1);
    }
    mostrarConfigRadio();
    Serial.printf("Destino: http://%s:%d/iot/json?k=%s&i=<id>\n",
                  AGENTE_HOST, AGENTE_PUERTO, AGENTE_APIKEY);
}

void loop()
{
    static wl_status_t wifi_previo = WL_IDLE_STATUS;

    recibirLoRa();

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
        WiFi.disconnect();
        conectarWiFi();
    }

    vaciarCola();
    refrescarOled();
}
