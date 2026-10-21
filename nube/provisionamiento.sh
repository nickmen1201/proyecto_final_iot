#!/usr/bin/env bash
# ===========================================================================
# Aprovisiona la plataforma: service group y dispositivo en el IoT Agent, y la
# suscripcion de Orion a QuantumLeap. Reconstruido a partir de lo que hay en
# MongoDB de la EC2 (2026-10-21).
#
# SOLO PARA UNA PLATAFORMA NUEVA. En la EC2 actual todo esto ya existe: el
# service group y el dispositivo darian error de duplicado, y la suscripcion
# SI se duplicaria, con lo que cada medida quedaria dos veces en CrateDB.
#
# Uso (en la EC2, con los contenedores arriba):
#   APIKEY=<la-de-include/secrets.h> bash provisionamiento.sh
# ===========================================================================

: "${APIKEY:?Definir APIKEY, la misma que AGENTE_APIKEY en include/secrets.h}"
HOST="${HOST:-localhost}"

SERVICIO="agricultura"
SUBSERVICIO="/"

echo "== Service group =="
curl -s -w '\nHTTP %{http_code}\n' -X POST "http://$HOST:4041/iot/services" \
    -H "Content-Type: application/json" \
    -H "fiware-service: $SERVICIO" -H "fiware-servicepath: $SUBSERVICIO" \
    -d '{
  "services": [{
    "apikey": "'"$APIKEY"'",
    "resource": "/iot/json",
    "entity_type": "SensorAgricola"
  }]
}'

# El dispositivo de la EC2 actual quedo registrado con transport "MQTT", pero
# las medidas llegan por HTTP (MQTT esta desactivado). Aqui va el valor correcto.
echo "== Dispositivo =="
curl -s -w '\nHTTP %{http_code}\n' -X POST "http://$HOST:4041/iot/devices" \
    -H "Content-Type: application/json" \
    -H "fiware-service: $SERVICIO" -H "fiware-servicepath: $SUBSERVICIO" \
    -d '{
  "devices": [{
    "device_id": "461879",
    "entity_name": "urn:ngsi-ld:SensorAgricola:461879",
    "entity_type": "SensorAgricola",
    "apikey": "'"$APIKEY"'",
    "transport": "HTTP",
    "attributes": [
      {"object_id": "n",    "name": "numeroTrama", "type": "Integer"},
      {"object_id": "t",    "name": "temperatura", "type": "Number"},
      {"object_id": "h",    "name": "humedad",     "type": "Number"},
      {"object_id": "lat",  "name": "latitud",     "type": "Number"},
      {"object_id": "lon",  "name": "longitud",    "type": "Number"},
      {"object_id": "rssi", "name": "rssi",        "type": "Integer"},
      {"object_id": "snr",  "name": "snr",         "type": "Number"}
    ]
  }]
}'

echo "== Suscripcion Orion -> QuantumLeap =="
curl -s -w '\nHTTP %{http_code}\n' -X POST "http://$HOST:1026/v2/subscriptions" \
    -H "Content-Type: application/json" \
    -H "fiware-service: $SERVICIO" -H "fiware-servicepath: $SUBSERVICIO" \
    -d '{
  "description": "Historico a QuantumLeap",
  "subject": {
    "entities": [{"idPattern": ".*", "type": "SensorAgricola"}]
  },
  "notification": {
    "http": {"url": "http://quantumleap:8668/v2/notify"},
    "metadata": ["dateCreated", "dateModified"]
  }
}'
