"""API de consulta para el Front. Lee Postgres (capa curada y decisiones),
Orion (estado actual) y el IoT Agent (dispositivos registrados): las lineas
de retorno del tablero. Nunca expone esos servicios: el Front solo habla con
esta API, de solo lectura, a traves de Caddy.
"""
from typing import Annotated

import requests
from fastapi import FastAPI, HTTPException, Query

import comun

app = FastAPI(title="API agricultura", docs_url=None, redoc_url=None)

# La entidad viaja a la URL de Orion: solo se aceptan caracteres de un id
# NGSI normal, para que no pueda colarse otra ruta.
Entidad = Annotated[str | None, Query(pattern=r"^[\w:.\-]+$", max_length=128)]
Horas = Annotated[int, Query(ge=1, le=24 * 14)]


def consultar(sql, args=()):
    """-> lista de dicts."""
    with comun.pg_conn() as pg, pg.cursor() as cur:
        cur.execute(sql, args)
        columnas = [c.name for c in cur.description]
        return [dict(zip(columnas, fila)) for fila in cur.fetchall()]


def entidad_defecto(entidad):
    """La entidad pedida o, si no se indica, la del dato mas reciente."""
    if entidad:
        return entidad
    filas = consultar(
        "SELECT entity_id FROM medidas_limpias ORDER BY time_index DESC LIMIT 1")
    if not filas:
        raise HTTPException(503, "Todavia no hay medidas limpias")
    return filas[0]["entity_id"]


@app.get("/api/salud")
def salud():
    def probar(funcion):
        try:
            funcion()
            return "ok"
        except Exception as e:
            print(f"api: salud: {e}", flush=True)    # el detalle, al log
            return "error"

    return {
        "postgres": probar(lambda: consultar("SELECT 1")),
        "cratedb": probar(lambda: comun.crate_sql("SELECT 1")),
        "orion": probar(lambda: requests.get(
            f"{comun.ORION_URL}/version", timeout=5).raise_for_status()),
        "agente": probar(lambda: requests.get(
            f"{comun.AGENTE_URL}/iot/about", timeout=5).raise_for_status()),
    }


@app.get("/api/dispositivos")
def dispositivos():
    """Dispositivos registrados en el IoT Agent: el front los ofrece para
    elegir que equipo mirar."""
    r = requests.get(f"{comun.AGENTE_URL}/iot/devices",
                     headers=comun.CABECERAS_FIWARE, timeout=5)
    if not r.ok:
        raise HTTPException(502, "El IoT Agent no respondio")
    return [{"id": d["device_id"], "entidad": d["entity_name"]}
            for d in r.json().get("devices", [])]


@app.get("/api/actual")
def actual(entidad: Entidad = None):
    """Estado actual segun Orion (ultimo valor de cada atributo) mas la ultima
    posicion GPS valida segun Postgres (Orion puede no tener fix aun)."""
    eid = entidad_defecto(entidad)
    r = requests.get(f"{comun.ORION_URL}/v2/entities/{eid}",
                     params={"options": "keyValues"},
                     headers=comun.CABECERAS_FIWARE, timeout=5)
    posicion = consultar(
        """SELECT latitud, longitud, time_index FROM medidas_limpias
           WHERE entity_id = %s AND latitud IS NOT NULL
           ORDER BY time_index DESC LIMIT 1""", (eid,))
    return {"entidad": eid, "orion": r.json() if r.ok else None,
            "posicion": posicion[0] if posicion else None}


@app.get("/api/decisiones")
def decisiones(entidad: Entidad = None):
    """Estado vigente de cada regla: el semaforo."""
    return consultar(
        """SELECT DISTINCT ON (regla) regla, nombre, nivel, accion, evidencia,
                  desde, ultima_evaluacion
           FROM decisiones WHERE entity_id = %s
           ORDER BY regla, desde DESC""", (entidad_defecto(entidad),))


@app.get("/api/decisiones/historial")
def historial(entidad: Entidad = None,
              limite: Annotated[int, Query(ge=1, le=500)] = 50):
    return consultar(
        """SELECT regla, nombre, nivel, accion, evidencia, desde, ultima_evaluacion
           FROM decisiones WHERE entity_id = %s
           ORDER BY desde DESC LIMIT %s""", (entidad_defecto(entidad), limite))


@app.get("/api/resumen")
def resumen(entidad: Entidad = None, horas: Horas = 24):
    return consultar(
        """SELECT * FROM resumen_horario
           WHERE entity_id = %s AND hora > now() - make_interval(hours => %s)
           ORDER BY hora""", (entidad_defecto(entidad), horas))


@app.get("/api/indicadores")
def indicadores(entidad: Entidad = None, horas: Horas = 24):
    return consultar(
        """SELECT * FROM indicadores
           WHERE entity_id = %s AND momento > now() - make_interval(hours => %s)
           ORDER BY momento""", (entidad_defecto(entidad), horas))


@app.get("/api/medidas")
def medidas(entidad: Entidad = None, horas: Horas = 24,
            paso_min: Annotated[int, Query(ge=1, le=60)] = 5):
    """Series de T y HR para las graficas, promediadas en cubetas de paso_min
    para no mandar miles de puntos al navegador."""
    return consultar(
        """SELECT date_bin(make_interval(mins => %s), time_index,
                           timestamptz '2001-01-01') AS momento,
                  avg(temperatura) AS temperatura, avg(humedad) AS humedad,
                  avg(rssi) AS rssi, count(*) AS tramas
           FROM medidas_limpias
           WHERE entity_id = %s AND time_index > now() - make_interval(hours => %s)
           GROUP BY 1 ORDER BY 1""",
        (paso_min, entidad_defecto(entidad), horas))
