#pragma once

// Copiar este archivo como include/secrets.h y rellenarlo.
// secrets.h esta en .gitignore: la clave del WiFi no se sube al repositorio.

#define WIFI_SSID       "nombre-de-la-red"
#define WIFI_PASSWORD   "clave-de-la-red"

// Elastic IP de la EC2 donde corre el docker-compose.
#define AGENTE_HOST     "0.0.0.0"

// Puerto sur (southbound) del IoT Agent JSON: IOTA_HTTP_PORT en el compose.
#define AGENTE_PUERTO   5555

// apikey del service group con que se provisiono el agente (puerto 4041).
#define AGENTE_APIKEY   "cambiar-apikey"
