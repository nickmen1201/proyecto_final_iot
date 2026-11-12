"""Limpiar: lee el crudo nuevo de CrateDB, aplica las reglas de limpieza y lo
guarda en Postgres (medidas_limpias). Corre en un bucle cada PERIODO_S.

Es incremental e idempotente: la marca de agua evita releer lo ya procesado y
el ON CONFLICT evita duplicados si se reinicia a mitad de un lote.
"""
import os
import time

import comun

PERIODO_S = int(os.getenv("PERIODO_S", "60"))

INSERTAR = """
    INSERT INTO medidas_limpias
      (entity_id, time_index, n, temperatura, humedad, latitud, longitud,
       rssi, snr, validez)
    VALUES (%(entity_id)s, to_timestamp(%(time_index)s / 1000.0), %(n)s,
            %(temperatura)s, %(humedad)s, %(latitud)s, %(longitud)s,
            %(rssi)s, %(snr)s, %(validez)s)
    ON CONFLICT DO NOTHING
"""


def ciclo(pg, ultima_t):
    with pg.cursor() as cur:
        cur.execute("SELECT ultimo_ts FROM marca_agua WHERE servicio = 'limpiar'")
        fila = cur.fetchone()
        desde_ms = int(fila[0].timestamp() * 1000) if fila else 0

    crudas = comun.leer_crudas(desde_ms)
    if not crudas:
        return 0

    limpias = [x for m in crudas if (x := comun.limpiar_medida(m, ultima_t))]
    with pg.cursor() as cur:
        cur.executemany(INSERTAR, limpias)
        cur.execute(
            """INSERT INTO marca_agua VALUES ('limpiar', to_timestamp(%s / 1000.0))
               ON CONFLICT (servicio) DO UPDATE SET ultimo_ts = EXCLUDED.ultimo_ts""",
            (crudas[-1]["time_index"],),
        )
    print(f"limpiar: {len(crudas)} crudas -> {len(limpias)} limpias", flush=True)
    return len(crudas)


def main():
    pg = comun.pg_conn()
    # Semilla del control de saltos: la ultima T aceptada de cada equipo.
    with pg.cursor() as cur:
        cur.execute("""SELECT DISTINCT ON (entity_id) entity_id, temperatura,
                              (extract(epoch FROM time_index) * 1000)::bigint
                       FROM medidas_limpias WHERE temperatura IS NOT NULL
                       ORDER BY entity_id, time_index DESC""")
        ultima_t = {eid: (t, ts) for eid, t, ts in cur.fetchall()}

    while True:
        try:
            ciclo(pg, ultima_t)
        except Exception as e:
            # Lo normal al principio: la tabla de CrateDB aun no existe
            # porque no ha llegado la primera medida.
            print(f"limpiar: {e}", flush=True)
        time.sleep(PERIODO_S)


if __name__ == "__main__":
    main()
