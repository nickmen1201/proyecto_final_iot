"""Analitica: lee el crudo de CrateDB (como en el tablero), lo limpia con las
mismas reglas que Limpiar y produce las tres capas de la toma de decisiones:

  1. Descriptiva:   resumen_horario  (que paso)
  2. Predictiva:    indicadores      (VPD, punto de rocio, tendencia, GDD)
  3. Prescriptiva:  decisiones       (reglas R1..R5 con nivel y accion)

Las decisiones usan ventanas (no lecturas sueltas) e histeresis (para salir de
un nivel hay que cruzar el umbral con margen), y solo se inserta una fila
cuando una regla CAMBIA de nivel: el historial queda corto y legible.
"""
import json
import math
import os
import time
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

import comun

PERIODO_S = int(os.getenv("PERIODO_S", "60"))
# Minutos sin datos para declarar el sistema caido (R5). Configurable para
# poder demostrar la regla sin esperar.
SIN_DATOS_MIN = float(os.getenv("SIN_DATOS_MIN", "5"))
VENTANA_H = 48          # horas de crudo que se recalculan en cada pasada
TZ_LOCAL = ZoneInfo("America/Bogota")   # para el corte diario de los GDD

# Umbrales del cultivo de referencia: tomate. Cambiar de cultivo es cambiar
# estos numeros, no el codigo.
UMBRALES = {
    "vpd_atencion": 1.2,    # kPa: programar riego
    "vpd_critico": 1.6,     # kPa: estres hidrico, regar ya
    "vpd_hongos": 0.4,      # kPa: aire casi saturado
    "vpd_margen": 0.1,
    "rocio_delta": 2.0,     # C entre T y punto de rocio: condensacion
    "rocio_margen": 0.5,
    "hr_hongos": 90.0,      # % de HR sostenida
    "horas_hongos": 3,      # horas con HR alta (de las ultimas 6) para alertar
    "helada_t": 2.0,        # C
    "helada_margen": 0.5,
    "calor_atencion": 32.0, # C
    "calor_critico": 35.0,  # C
    "calor_margen": 1.0,
    "gdd_base": 10.0,       # C, base de grados-dia del tomate
    "perdida_max": 0.20,    # fraccion de tramas perdidas tolerable
    "rssi_min": -115.0,     # dBm: cerca del piso de sensibilidad del SX1276
}


# ---------------------------------------------------------------------------
# Formulas agronomicas (FAO-56 / Tetens). Solo necesitan T y HR.
# ---------------------------------------------------------------------------
def presion_saturacion(t):
    """kPa a la temperatura t (C)."""
    return 0.6108 * math.exp(17.27 * t / (t + 237.3))


def vpd(t, h):
    """Deficit de presion de vapor en kPa: cuanta agua 'pide' el aire."""
    return presion_saturacion(t) * (1.0 - h / 100.0)


def punto_rocio(t, h):
    """C. Si T baja hasta aqui (o la hoja esta asi de fria), condensa."""
    if h <= 0:
        return None
    a = math.log(h / 100.0) + 17.27 * t / (t + 237.3)
    return 237.3 * a / (17.27 - a)


def promedio(valores):
    valores = [v for v in valores if v is not None]
    return sum(valores) / len(valores) if valores else None


def pendiente(puntos):
    """Pendiente de la recta de minimos cuadrados sobre (segundos, valor).

    Exige al menos media hora entre el primer y el ultimo punto: extrapolar
    una hora a partir de un par de minutos de datos (p. ej. recien arrancado
    el sistema) amplifica el ruido y dispara falsas alarmas de helada.
    """
    if len(puntos) < 5 or puntos[-1][0] - puntos[0][0] < 1800:
        return None
    n = len(puntos)
    sx = sum(p[0] for p in puntos)
    sy = sum(p[1] for p in puntos)
    sxx = sum(p[0] ** 2 for p in puntos)
    sxy = sum(p[0] * p[1] for p in puntos)
    d = n * sxx - sx * sx
    return (n * sxy - sx * sy) / d if d else None


# ---------------------------------------------------------------------------
# Capa descriptiva: agregados por hora
# ---------------------------------------------------------------------------
def resumen_por_hora(filas):
    """filas limpias de UNA entidad -> lista de dicts por hora."""
    horas = {}
    for f in filas:
        horas.setdefault(f["time_index"] // 3600000 * 3600000, []).append(f)
    resumen = []
    for hora_ms, grupo in sorted(horas.items()):
        temps = [f["temperatura"] for f in grupo if f["temperatura"] is not None]
        resumen.append({
            "hora": hora_ms,
            "t_min": min(temps) if temps else None,
            "t_max": max(temps) if temps else None,
            "t_avg": promedio(temps),
            "h_avg": promedio([f["humedad"] for f in grupo]),
            "vpd_avg": promedio([vpd(f["temperatura"], f["humedad"])
                                 for f in grupo
                                 if f["temperatura"] is not None
                                 and f["humedad"] is not None]),
            "tramas": len(grupo),
            "perdidas": comun.contar_perdidas([f["n"] for f in grupo]),
            "rssi_avg": promedio([f["rssi"] for f in grupo]),
            "snr_avg": promedio([f["snr"] for f in grupo]),
        })
    return resumen


# ---------------------------------------------------------------------------
# Capa predictiva: indicadores del momento
# ---------------------------------------------------------------------------
def calcular_indicadores(filas, ahora_ms):
    """filas limpias de UNA entidad (48 h, puede ser vacia) -> indicadores."""
    def ventana(minutos):
        corte = ahora_ms - minutos * 60000
        return [f for f in filas
                if f["time_index"] > corte and f["temperatura"] is not None]

    v10, v30, v60 = ventana(10), ventana(30), ventana(60)
    t_act = promedio([f["temperatura"] for f in v10])
    h_act = promedio([f["humedad"] for f in v10])
    rocio = punto_rocio(t_act, h_act) if t_act is not None and h_act is not None else None

    tend = pendiente([((f["time_index"] - ahora_ms) / 1000.0, f["temperatura"])
                      for f in v60])
    tend_h = tend * 3600.0 if tend is not None else None     # C por hora

    hoy = datetime.now(TZ_LOCAL).date()
    temps_hoy = [f["temperatura"] for f in filas
                 if f["temperatura"] is not None
                 and datetime.fromtimestamp(f["time_index"] / 1000.0,
                                            TZ_LOCAL).date() == hoy]
    gdd = (max(0.0, (min(temps_hoy) + max(temps_hoy)) / 2 - UMBRALES["gdd_base"])
           if temps_hoy else None)

    hora = [f for f in filas if f["time_index"] > ahora_ms - 3600000]
    perdidas = comun.contar_perdidas([f["n"] for f in hora])
    total = len(hora) + perdidas

    return {
        # minutos desde la ultima trama recibida (aunque el sensor fallara)
        "edad_min": (ahora_ms - filas[-1]["time_index"]) / 60000.0 if filas else None,
        "temperatura": t_act,                               # promedio de 10 min
        "humedad": h_act,
        "t_30min": promedio([f["temperatura"] for f in v30]),
        "vpd": promedio([vpd(f["temperatura"], f["humedad"])
                         for f in v30 if f["humedad"] is not None]),
        "punto_rocio": rocio,
        "delta_rocio": t_act - rocio if rocio is not None else None,
        "tendencia_t": tend_h,
        "t_pred_1h": t_act + tend_h if t_act is not None and tend_h is not None else None,
        "gdd_hoy": gdd,
        "perdida_pct": perdidas / total if total else None,
        "rssi_avg": promedio([f["rssi"] for f in hora]),
    }


# ---------------------------------------------------------------------------
# Capa prescriptiva: reglas de decision
# ---------------------------------------------------------------------------
def clasificar(valor, atencion, critico, previo, margen):
    """Nivel por umbrales crecientes (mayor = peor), con histeresis: quien ya
    esta en un nivel necesita bajar 'margen' por debajo del umbral para salir."""
    if valor is None:
        return "SIN_DATOS"
    lim_c = critico - (margen if previo == "CRITICO" else 0)
    lim_a = atencion - (margen if previo in ("ATENCION", "CRITICO") else 0)
    if valor >= lim_c:
        return "CRITICO"
    if valor >= lim_a:
        return "ATENCION"
    return "OK"


def evaluar_reglas(ind, resumen, previos, ahora_ms, u=UMBRALES):
    """-> lista de (regla, nombre, nivel, accion, evidencia)."""
    # R5 primero: si no hay datos frescos, las demas reglas no pueden decir
    # "todo bien": dicen SIN_DATOS.
    edad_min = ind["edad_min"]
    if edad_min is None or edad_min > SIN_DATOS_MIN:
        ev = {"minutos_sin_datos": None if edad_min is None else round(edad_min, 1)}
        reglas = [(r, nombre, "SIN_DATOS", "Sin datos recientes: no se puede evaluar.", ev)
                  for r, nombre in [("R1", "Estrés hídrico"), ("R2", "Riesgo de hongos"),
                                    ("R3", "Helada"), ("R4", "Estrés térmico")]]
        reglas.append(("R5", "Salud del sistema", "CRITICO",
                       "Revisar el emisor o el gateway: no llegan medidas.", ev))
        return reglas

    reglas = []
    sin_aire = "Sin lecturas de aire válidas."

    # R1: estres hidrico por VPD alto (promedio de 30 min)
    nivel = clasificar(ind["vpd"], u["vpd_atencion"], u["vpd_critico"],
                       previos.get("R1"), u["vpd_margen"])
    accion = {"CRITICO": "Regar ahora; sombrear si persiste.",
              "ATENCION": "Programar riego en las próximas horas.",
              "OK": "Demanda hídrica normal.",
              "SIN_DATOS": sin_aire}[nivel]
    reglas.append(("R1", "Estrés hídrico", nivel, accion,
                   {"vpd_30min_kpa": ind["vpd"]}))

    # R2: hongos por condensacion (T-Td), HR alta sostenida o VPD muy bajo
    horas_hr_alta = sum(1 for r in resumen
                        if r["hora"] > ahora_ms - 6 * 3600000
                        and r["h_avg"] is not None and r["h_avg"] >= u["hr_hongos"])
    delta_lim = u["rocio_delta"] + (u["rocio_margen"]
                                    if previos.get("R2") == "CRITICO" else 0)
    if ind["delta_rocio"] is None:
        nivel, accion = "SIN_DATOS", sin_aire
    elif ind["delta_rocio"] <= delta_lim or horas_hr_alta >= u["horas_hongos"]:
        nivel = "CRITICO"
        accion = "Ventilar y suspender el riego por aspersión; considerar fungicida preventivo."
    elif ind["vpd"] is not None and ind["vpd"] < u["vpd_hongos"]:
        nivel, accion = "ATENCION", "Ventilar; no regar por ahora."
    else:
        nivel, accion = "OK", "Sin condiciones de condensación."
    reglas.append(("R2", "Riesgo de hongos", nivel, accion,
                   {"delta_rocio_c": ind["delta_rocio"], "horas_hr_alta": horas_hr_alta,
                    "vpd_30min_kpa": ind["vpd"]}))

    # R3: helada actual (CRITICO) o prevista para dentro de 1 hora (ATENCION)
    t, pred = ind["temperatura"], ind["t_pred_1h"]
    helada_lim = u["helada_t"] + (u["helada_margen"]
                                  if previos.get("R3") == "CRITICO" else 0)
    if t is None:
        nivel, accion = "SIN_DATOS", sin_aire
    elif t <= helada_lim:
        nivel, accion = "CRITICO", "Activar protección antiheladas (cubrir o riego)."
    elif pred is not None and pred <= u["helada_t"]:
        nivel, accion = "ATENCION", "Preparar protección: helada probable en ~1 hora."
    else:
        nivel, accion = "OK", "Sin riesgo de helada."
    reglas.append(("R3", "Helada", nivel, accion,
                   {"t_10min_c": t, "t_pred_1h_c": pred,
                    "tendencia_c_por_h": ind["tendencia_t"]}))

    # R4: estres termico por calor (promedio de 30 min)
    nivel = clasificar(ind["t_30min"], u["calor_atencion"], u["calor_critico"],
                       previos.get("R4"), u["calor_margen"])
    accion = {"CRITICO": "Sombrear o nebulizar; reforzar el riego.",
              "ATENCION": "Vigilar; preparar sombreo.",
              "OK": "Temperatura en zona segura.",
              "SIN_DATOS": sin_aire}[nivel]
    reglas.append(("R4", "Estrés térmico", nivel, accion,
                   {"t_30min_c": ind["t_30min"]}))

    # R5: llegan tramas; falta ver si el sensor mide y si el enlace aguanta
    ev = {"perdida_pct": ind["perdida_pct"], "rssi_avg": ind["rssi_avg"]}
    if t is None:
        nivel = "ATENCION"
        accion = "Revisar el sensor de aire del emisor: llegan tramas pero sin lecturas válidas."
    elif ((ind["perdida_pct"] or 0) > u["perdida_max"]
          or (ind["rssi_avg"] is not None and ind["rssi_avg"] < u["rssi_min"])):
        nivel = "ATENCION"
        accion = "Revisar la antena o la ubicación del gateway: el enlace pierde tramas."
    else:
        nivel, accion = "OK", "Enlace y equipos funcionando."
    reglas.append(("R5", "Salud del sistema", nivel, accion, ev))
    return reglas


# ---------------------------------------------------------------------------
# Persistencia
# ---------------------------------------------------------------------------
def guardar_resumen(cur, eid, resumen):
    for r in resumen:
        cur.execute("""
            INSERT INTO resumen_horario (entity_id, hora, t_min, t_max, t_avg,
                h_avg, vpd_avg, tramas, perdidas, rssi_avg, snr_avg)
            VALUES (%s, to_timestamp(%s / 1000.0), %s, %s, %s, %s, %s, %s, %s, %s, %s)
            ON CONFLICT (entity_id, hora) DO UPDATE SET
                t_min = EXCLUDED.t_min, t_max = EXCLUDED.t_max,
                t_avg = EXCLUDED.t_avg, h_avg = EXCLUDED.h_avg,
                vpd_avg = EXCLUDED.vpd_avg, tramas = EXCLUDED.tramas,
                perdidas = EXCLUDED.perdidas, rssi_avg = EXCLUDED.rssi_avg,
                snr_avg = EXCLUDED.snr_avg""",
            (eid, r["hora"], r["t_min"], r["t_max"], r["t_avg"], r["h_avg"],
             r["vpd_avg"], r["tramas"], r["perdidas"], r["rssi_avg"], r["snr_avg"]))


def guardar_indicadores(cur, eid, ind, ahora):
    cur.execute("""
        INSERT INTO indicadores (entity_id, momento, temperatura, humedad, vpd,
            punto_rocio, delta_rocio, tendencia_t, t_pred_1h, gdd_hoy, perdida_pct)
        VALUES (%s, date_trunc('minute', %s::timestamptz), %s, %s, %s, %s, %s, %s, %s, %s, %s)
        ON CONFLICT DO NOTHING""",
        (eid, ahora, ind["temperatura"], ind["humedad"], ind["vpd"],
         ind["punto_rocio"], ind["delta_rocio"], ind["tendencia_t"],
         ind["t_pred_1h"], ind["gdd_hoy"], ind["perdida_pct"]))


def guardar_decisiones(cur, eid, reglas, ahora):
    for regla, nombre, nivel, accion, evidencia in reglas:
        ev = json.dumps({k: (round(v, 3) if isinstance(v, float) else v)
                         for k, v in evidencia.items()})
        cur.execute("""SELECT id, nivel FROM decisiones
                       WHERE entity_id = %s AND regla = %s
                       ORDER BY desde DESC LIMIT 1""", (eid, regla))
        previa = cur.fetchone()
        if previa and previa[1] == nivel:
            cur.execute("""UPDATE decisiones SET ultima_evaluacion = %s,
                           accion = %s, evidencia = %s WHERE id = %s""",
                        (ahora, accion, ev, previa[0]))
        else:
            cur.execute("""INSERT INTO decisiones (entity_id, regla, nombre,
                           nivel, accion, evidencia, desde, ultima_evaluacion)
                           VALUES (%s, %s, %s, %s, %s, %s, %s, %s)""",
                        (eid, regla, nombre, nivel, accion, ev, ahora, ahora))
            print(f"analitica: {eid} {regla} -> {nivel} ({accion})", flush=True)


def ciclo(pg):
    ahora = datetime.now(timezone.utc)
    ahora_ms = int(ahora.timestamp() * 1000)
    # recientes=True: si la ventana no cupiera en el limite, se conservan las
    # filas nuevas (perder las ultimas pareceria un "sin datos" falso).
    crudas = comun.leer_crudas(ahora_ms - VENTANA_H * 3600000, ahora_ms,
                               limite=100000, recientes=True)

    ultima_t, por_entidad = {}, {}
    for m in crudas:
        f = comun.limpiar_medida(m, ultima_t)
        if f:
            por_entidad.setdefault(f["entity_id"], []).append(f)

    with pg.cursor() as cur:
        # Un equipo que ya tuvo decisiones y desaparecio de la ventana se sigue
        # evaluando (sin filas -> SIN_DATOS): si no, su ultimo "OK" quedaria
        # congelado para siempre.
        cur.execute("SELECT DISTINCT entity_id FROM decisiones")
        for (eid,) in cur.fetchall():
            por_entidad.setdefault(eid, [])

        for eid, filas in por_entidad.items():
            resumen = resumen_por_hora(filas)
            ind = calcular_indicadores(filas, ahora_ms)

            cur.execute("""SELECT DISTINCT ON (regla) regla, nivel FROM decisiones
                           WHERE entity_id = %s ORDER BY regla, desde DESC""", (eid,))
            previos = dict(cur.fetchall())

            guardar_resumen(cur, eid, resumen)
            if filas:
                guardar_indicadores(cur, eid, ind, ahora)
            guardar_decisiones(cur, eid,
                               evaluar_reglas(ind, resumen, previos, ahora_ms), ahora)


def main():
    while True:
        # Conexion nueva en cada pasada: si Postgres se reinicia, el servicio
        # se recupera solo en el siguiente ciclo.
        try:
            with comun.pg_conn() as pg:
                ciclo(pg)
        except Exception as e:
            print(f"analitica: {e}", flush=True)
        time.sleep(PERIODO_S)


if __name__ == "__main__":
    main()
