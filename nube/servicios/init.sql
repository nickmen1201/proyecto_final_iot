-- Esquema de la capa curada y de decisiones. Lo ejecuta el contenedor de
-- Postgres solo la primera vez que arranca con el volumen vacio
-- (docker-entrypoint-initdb.d). Si se cambia, hay que recrear el volumen.

-- Salida del servicio Limpiar: una fila por medida valida de CrateDB.
CREATE TABLE medidas_limpias (
    entity_id   text        NOT NULL,
    time_index  timestamptz NOT NULL,
    n           bigint,                 -- numero de trama del emisor
    temperatura real,                   -- C, NULL si el dato no es confiable
    humedad     real,                   -- %
    latitud     double precision,
    longitud    double precision,
    rssi        real,                   -- calidad del enlace LoRa
    snr         real,
    validez     integer,                -- mascara que mando el emisor
    PRIMARY KEY (entity_id, time_index)
);

-- Analitica, nivel descriptivo: agregados por hora.
CREATE TABLE resumen_horario (
    entity_id text        NOT NULL,
    hora      timestamptz NOT NULL,
    t_min     real,
    t_max     real,
    t_avg     real,
    h_avg     real,
    vpd_avg   real,                     -- kPa
    tramas    integer,                  -- recibidas en la hora
    perdidas  integer,                  -- huecos en la secuencia n
    rssi_avg  real,
    snr_avg   real,
    PRIMARY KEY (entity_id, hora)
);

-- Analitica, nivel diagnostico/predictivo: indicadores del momento.
CREATE TABLE indicadores (
    entity_id   text        NOT NULL,
    momento     timestamptz NOT NULL,
    temperatura real,                   -- promedio de los ultimos 10 min
    humedad     real,
    vpd         real,                   -- kPa, promedio de 30 min
    punto_rocio real,                   -- C
    delta_rocio real,                   -- T - Td: <= 2 es riesgo de condensacion
    tendencia_t real,                   -- pendiente de T en C/hora (60 min)
    t_pred_1h   real,                   -- T estimada dentro de 1 hora
    gdd_hoy     real,                   -- grados-dia acumulados del dia (base 10)
    perdida_pct real,                   -- fraccion de tramas perdidas, ultima hora
    PRIMARY KEY (entity_id, momento)
);

-- Analitica, nivel prescriptivo: una fila cada vez que una regla CAMBIA de
-- nivel. ultima_evaluacion avanza mientras el nivel se mantiene.
CREATE TABLE decisiones (
    id                bigserial PRIMARY KEY,
    entity_id         text        NOT NULL,
    regla             text        NOT NULL,  -- R1..R5
    nombre            text        NOT NULL,
    nivel             text        NOT NULL,  -- OK | ATENCION | CRITICO | SIN_DATOS
    accion            text        NOT NULL,  -- recomendacion para el agricultor
    evidencia         jsonb,                 -- valores que justifican la decision
    desde             timestamptz NOT NULL,
    ultima_evaluacion timestamptz NOT NULL
);
CREATE INDEX decisiones_ultima ON decisiones (entity_id, regla, desde DESC);

-- Hasta donde proceso cada servicio, para trabajar incremental.
CREATE TABLE marca_agua (
    servicio  text PRIMARY KEY,
    ultimo_ts timestamptz NOT NULL
);
