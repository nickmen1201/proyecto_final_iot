"""Acceso a datos y limpieza, compartidos por limpiar, analitica y api.

La limpieza vive aqui para que Limpiar y Analitica apliquen EXACTAMENTE las
mismas reglas: Analitica lee el crudo de CrateDB (como en el tablero) y no
debe decidir sobre datos que Limpiar descartaria.
"""
import os

import psycopg
import requests

CRATE_URL = os.getenv("CRATE_URL", "http://crate-db:4200/_sql")
TABLA_CRATE = os.getenv("TABLA_CRATE", "mtagricultura.etsensoragricola")
ORION_URL = os.getenv("ORION_URL", "http://orion:1026")
CABECERAS_FIWARE = {"fiware-service": "agricultura", "fiware-servicepath": "/"}

_PG = dict(
    host=os.getenv("PG_HOST", "postgres"),
    dbname=os.getenv("PG_DB", "agricultura"),
    user=os.getenv("PG_USER", "agro"),
    password=os.environ["PG_PASSWORD"],   # sin clave no hay nada que hacer
)

# Mismos limites fisicos que usa el emisor (src/emisor/main.cpp).
TEMP_MIN, TEMP_MAX = -40.0, 125.0
SALTO_T_MAX = 5.0       # mas de 5 C entre tramas seguidas es imposible...
SALTO_VENTANA_MS = 60000  # ...pero solo si las separan menos de 60 s: tras un
                          # hueco largo (gateway caido) la T pudo cambiar de
                          # verdad y comparar contra un valor viejo anularia
                          # medidas buenas sin salida posible.

VALIDEZ_AIRE = 1    # bits del campo "v" del emisor
VALIDEZ_GPS = 2


def crate_sql(stmt, args=()):
    """Consulta CrateDB por su API HTTP. Devuelve (columnas, filas)."""
    r = requests.post(CRATE_URL, json={"stmt": stmt, "args": list(args)}, timeout=15)
    r.raise_for_status()
    datos = r.json()
    return datos["cols"], datos["rows"]


def pg_conn():
    return psycopg.connect(**_PG, autocommit=True)


def leer_crudas(desde_ms, limite=5000):
    """Medidas de CrateDB posteriores a desde_ms (epoch ms), como dicts.

    SELECT * a proposito: QuantumLeap crea las columnas (validez, latitud...)
    solo cuando llega el primer valor no nulo, asi que pedirlas por nombre
    fallaria mientras no existan.
    """
    cols, filas = crate_sql(
        f"SELECT * FROM {TABLA_CRATE} WHERE time_index > ? "
        f"ORDER BY time_index LIMIT {limite}",
        [int(desde_ms)],
    )
    return [dict(zip(cols, fila)) for fila in filas]


def limpiar_medida(m, ultima_t):
    """Aplica las reglas de limpieza a una medida cruda de CrateDB.

    Devuelve el dict curado, o None si la fila se descarta entera.
    ultima_t: dict entity_id -> (temperatura, time_index ms) de la ultima
    medida aceptada, para el control de saltos.
    Reglas, en orden:
      1. descartar tramas sinteticas del MODO_PRUEBA (rssi y snr en null)
      2. mascara de validez del emisor: bit apagado = valor repetido por
         Orion, no una medida nueva -> NULL
      3. rangos fisicos
      4. salto imposible de temperatura frente a la ultima aceptada
    """
    if m.get("rssi") is None and m.get("snr") is None:
        return None

    eid = m["entity_id"]
    t, h = m.get("temperatura"), m.get("humedad")
    lat, lon = m.get("latitud"), m.get("longitud")
    v = m.get("validez")

    if v is not None:
        if not int(v) & VALIDEZ_AIRE:
            t = h = None
        if not int(v) & VALIDEZ_GPS:
            lat = lon = None

    if t is not None and not (TEMP_MIN < t < TEMP_MAX):
        t = h = None            # mismo sensor: si T es basura, HR tambien
    if h is not None and not (0.0 <= h <= 100.0):
        h = None

    previa = ultima_t.get(eid)
    if (t is not None and previa is not None
            and m["time_index"] - previa[1] <= SALTO_VENTANA_MS
            and abs(t - previa[0]) > SALTO_T_MAX):
        t = h = None
    if t is not None:
        ultima_t[eid] = (t, m["time_index"])

    return {
        "entity_id": eid,
        "time_index": m["time_index"],      # epoch ms
        "n": m.get("numerotrama"),
        "temperatura": t,
        "humedad": h,
        "latitud": lat,
        "longitud": lon,
        "rssi": m.get("rssi"),
        "snr": m.get("snr"),
        "validez": v,
    }


def contar_perdidas(enes):
    """Tramas perdidas segun los huecos del contador n. Un n que baja es un
    reinicio del emisor y no cuenta como perdida."""
    enes = sorted(x for x in enes if x is not None)
    return sum(max(0, b - a - 1) for a, b in zip(enes, enes[1:]))
