"""API de consulta para el Front. Lee Postgres (capa curada y decisiones) y
Orion (estado actual, la linea de retorno del tablero). Nunca expone las
bases: el Front solo habla con esta API a traves de Caddy.
"""
import requests
from fastapi import FastAPI, HTTPException

import comun

app = FastAPI(title="API agricultura", docs_url=None, redoc_url=None)


def consultar(sql, args=()):
    """-> lista de dicts."""
    with comun.pg_conn() as pg, pg.cursor() as cur:
        cur.execute(sql, args)
        columnas = [c.name for c in cur.description]
        return [dict(zip(columnas, fila)) for fila in cur.fetchall()]


def entidad_defecto(entidad=None):
    """La entidad pedida o, por defecto, la del dato mas reciente."""
    if entidad:
        return entidad
    filas = consultar(
        "SELECT entity_id FROM medidas_limpias ORDER BY time_index DESC LIMIT 1")
    if not filas:
        raise HTTPException(503, "Todavia no hay medidas limpias")
    return filas[0]["entity_id"]


@app.get("/api/salud")
def salud():
    estado = {}
    try:
        consultar("SELECT 1")
        estado["postgres"] = "ok"
    except Exception as e:
        estado["postgres"] = str(e)
    try:
        comun.crate_sql("SELECT 1")
        estado["cratedb"] = "ok"
    except Exception as e:
        estado["cratedb"] = str(e)
    try:
        requests.get(f"{comun.ORION_URL}/version", timeout=5).raise_for_status()
        estado["orion"] = "ok"
    except Exception as e:
        estado["orion"] = str(e)
    return estado


@app.get("/api/actual")
def actual(entidad: str | None = None):
    """Estado actual segun Orion (ultimo valor de cada atributo) mas la ultima
    posicion GPS conocida segun Postgres (Orion puede no tener fix aun)."""
    eid = entidad_defecto(entidad)
    r = requests.get(f"{comun.ORION_URL}/v2/entities/{eid}",
                     params={"options": "keyValues"},
                     headers=comun.CABECERAS_FIWARE, timeout=5)
    orion = r.json() if r.ok else {"error": f"Orion respondio {r.status_code}"}
    posicion = consultar(
        """SELECT latitud, longitud, time_index FROM medidas_limpias
           WHERE entity_id = %s AND latitud IS NOT NULL
           ORDER BY time_index DESC LIMIT 1""", (eid,))
    return {"entidad": eid, "orion": orion,
            "posicion": posicion[0] if posicion else None}


@app.get("/api/decisiones")
def decisiones(entidad: str | None = None):
    """Estado vigente de cada regla: el semaforo."""
    eid = entidad_defecto(entidad)
    return consultar(
        """SELECT DISTINCT ON (regla) regla, nombre, nivel, accion, evidencia,
                  desde, ultima_evaluacion
           FROM decisiones WHERE entity_id = %s
           ORDER BY regla, desde DESC""", (eid,))


@app.get("/api/decisiones/historial")
def historial(entidad: str | None = None, limite: int = 50):
    eid = entidad_defecto(entidad)
    return consultar(
        """SELECT regla, nombre, nivel, accion, evidencia, desde, ultima_evaluacion
           FROM decisiones WHERE entity_id = %s
           ORDER BY desde DESC LIMIT %s""", (eid, min(limite, 500)))


@app.get("/api/resumen")
def resumen(entidad: str | None = None, horas: int = 24):
    eid = entidad_defecto(entidad)
    return consultar(
        """SELECT * FROM resumen_horario
           WHERE entity_id = %s AND hora > now() - make_interval(hours => %s)
           ORDER BY hora""", (eid, min(horas, 24 * 14)))


@app.get("/api/indicadores")
def indicadores(entidad: str | None = None, horas: int = 24):
    eid = entidad_defecto(entidad)
    return consultar(
        """SELECT * FROM indicadores
           WHERE entity_id = %s AND momento > now() - make_interval(hours => %s)
           ORDER BY momento""", (eid, min(horas, 24 * 14)))


@app.get("/api/medidas")
def medidas(entidad: str | None = None, horas: int = 24, paso_min: int = 5):
    """Series de T y HR para las graficas, promediadas en cubetas de paso_min
    para no mandar miles de puntos al navegador."""
    eid = entidad_defecto(entidad)
    return consultar(
        """SELECT date_bin(make_interval(mins => %s), time_index,
                           timestamptz '2001-01-01') AS momento,
                  avg(temperatura) AS temperatura, avg(humedad) AS humedad,
                  avg(rssi) AS rssi, count(*) AS tramas
           FROM medidas_limpias
           WHERE entity_id = %s AND time_index > now() - make_interval(hours => %s)
           GROUP BY 1 ORDER BY 1""",
        (max(1, paso_min), eid, min(horas, 24 * 14)))
