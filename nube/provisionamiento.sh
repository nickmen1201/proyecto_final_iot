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

# Dos dispositivos con el mismo mapeo: el sensor real y "simulado-01", que
# usa simular.sh para probar la plataforma y las reglas sin hardware. Cada
# uno cae en su propia entidad de Orion.
#
# El dispositivo de la EC2 anterior quedo registrado con transport "MQTT",
# pero las medidas llegan por HTTP (MQTT esta desactivado). Aqui va el valor
# correcto. Los campos v/l/s son la mascara de validez del emisor y los
# sensores de luz y de suelo (reservados: se mapean desde ya para no tener
# que reaprovisionar cuando el firmware los mande).
dispositivo() {
    cat <<JSON
    {
      "device_id": "$1",
      "entity_name": "urn:ngsi-ld:SensorAgricola:$1",
      "entity_type": "SensorAgricola",
      "apikey": "$APIKEY",
      "transport": "HTTP",
      "attributes": [
        {"object_id": "n",    "name": "numeroTrama",  "type": "Integer"},
        {"object_id": "t",    "name": "temperatura",  "type": "Number"},
        {"object_id": "h",    "name": "humedad",      "type": "Number"},
        {"object_id": "lat",  "name": "latitud",      "type": "Number"},
        {"object_id": "lon",  "name": "longitud",     "type": "Number"},
        {"object_id": "rssi", "name": "rssi",         "type": "Integer"},
        {"object_id": "snr",  "name": "snr",          "type": "Number"},
        {"object_id": "v",    "name": "validez",      "type": "Integer"},
        {"object_id": "l",    "name": "luz",          "type": "Number"},
        {"object_id": "s",    "name": "humedadSuelo", "type": "Number"}
      ]
    }
JSON
}

echo "== Dispositivos =="
curl -s -w '\nHTTP %{http_code}\n' -X POST "http://$HOST:4041/iot/devices" \
    -H "Content-Type: application/json" \
    -H "fiware-service: $SERVICIO" -H "fiware-servicepath: $SUBSERVICIO" \
    -d '{"devices": ['"$(dispositivo 461879)"','"$(dispositivo simulado-01)"']}'

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
