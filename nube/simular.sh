#!/usr/bin/env bash
# ===========================================================================
# Manda tramas al IoT Agent como lo haria el gateway, para probar la
# plataforma completa (y disparar las reglas de decision) sin hardware.
#
# Usa el dispositivo "simulado-01", que provisionamiento.sh registra junto al
# real: los datos simulados caen en otra entidad y no ensucian la del sensor.
#
# Uso:
#   APIKEY=<la-de-secrets.h> bash simular.sh [escenario] [tramas] [periodo_s]
#   APIKEY=... HOST=34.237.180.187 bash simular.sh calor 20 2
#
# Escenarios (pensados para las reglas R1..R4):
#   normal -> T~22, HR~60: todo OK
#   calor  -> T~33, HR~25: VPD>1.6 (R1 CRITICO) y T>32 (R4 ATENCION)
#   hongos -> T~18, HR~96: T-Td<=2 (R2 CRITICO)
#   helada -> T~1,  HR~85: T<=2 (R3 CRITICO)
# Para R5 basta con dejar de enviar (ver SIN_DATOS_MIN del compose).
# ===========================================================================
set -euo pipefail
: "${APIKEY:?Definir APIKEY, la misma del service group}"
HOST="${HOST:-localhost}"
ID="${ID:-simulado-01}"

ESCENARIO="${1:-normal}"
TRAMAS="${2:-10}"
PERIODO="${3:-2}"

case "$ESCENARIO" in
    normal) T=22; H=60 ;;
    calor)  T=33; H=25 ;;
    hongos) T=18; H=96 ;;
    helada) T=1;  H=85 ;;
    *) echo "Escenario desconocido: $ESCENARIO (normal|calor|hongos|helada)"; exit 1 ;;
esac

echo "Escenario $ESCENARIO: $TRAMAS tramas a http://$HOST:5555 con id $ID"
for i in $(seq 1 "$TRAMAS"); do
    # decimas de variacion para que las graficas no sean una linea plana
    CUERPO=$(printf '{"n":%d,"t":%d.%d,"h":%d.%d,"lat":6.245259,"lon":-75.596510,"v":3,"rssi":%d,"snr":%d.%d}' \
             "$i" "$T" "$((RANDOM % 10))" "$H" "$((RANDOM % 10))" \
             "$((-60 - RANDOM % 25))" "$((RANDOM % 10))" "$((RANDOM % 10))")
    CODIGO=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
             "http://$HOST:5555/iot/json?k=$APIKEY&i=$ID" \
             -H "Content-Type: application/json" -d "$CUERPO")
    echo "  $i/$TRAMAS -> HTTP $CODIGO  $CUERPO"
    sleep "$PERIODO"
done
