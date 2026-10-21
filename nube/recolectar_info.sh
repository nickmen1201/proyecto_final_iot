#!/usr/bin/env bash
# ===========================================================================
# Recolecta la configuracion y el estado de la plataforma FIWARE de la EC2 para
# documentarla en nube/README.md. Solo lee: no crea, borra ni modifica nada.
#
# Uso (en la EC2):
#   bash recolectar_info.sh [carpeta-del-compose] > info.txt 2>&1
#
# La carpeta por defecto es ~/fiware. Las apikeys se reemplazan por
# <REDACTADO>, pero revisar info.txt antes de compartirlo por si el compose
# tiene otras claves.
# ===========================================================================

DIR="${1:-$HOME/fiware}"
cd "$DIR" || { echo "No existe la carpeta $DIR"; exit 1; }

seccion() { printf '\n===== %s =====\n' "$1"; }

# Tapa la apikey en JSON ("apikey":"..."), en variables (APIKEY=...) y en URLs
# del agente (?k=...).
redactar() {
    sed -E -e 's/("?[A-Za-z_]*apikey"?[[:space:]]*[:=][[:space:]]*"?)[^",&[:space:]]+/\1<REDACTADO>/Ig' \
           -e 's/([?&]k=)[^&[:space:]"]+/\1<REDACTADO>/g'
}

seccion "Fecha e IP publica"
date -u
curl -s --max-time 5 https://checkip.amazonaws.com

seccion "Versiones de Docker"
docker version --format 'Docker {{.Server.Version}}'
docker compose version
systemctl is-enabled docker

seccion "Archivos de la carpeta $DIR"
ls -la

seccion "docker compose config (compose + .env ya resueltos)"
docker compose config | redactar

seccion "Politica de reinicio de cada contenedor"
docker inspect -f '{{.Name}}  restart={{.HostConfig.RestartPolicy.Name}}' $(docker compose ps -q)

seccion "Volumenes y redes"
docker volume ls
docker network ls

seccion "Orion: /version"
curl -s --max-time 5 localhost:1026/version

seccion "IoT Agent: /iot/about"
curl -s --max-time 5 localhost:4041/iot/about

# Los service groups, dispositivos, suscripciones y entidades se leen directo
# de Mongo para no tener que adivinar los encabezados fiware-service.
seccion "MongoDB: bases, service groups, dispositivos, suscripciones, entidades"
docker compose exec -T mongo-db mongosh --quiet --eval '
db.adminCommand({ listDatabases: 1 }).databases.forEach(function (d) {
    var base = db.getSiblingDB(d.name);
    var colecciones = base.getCollectionNames();
    print("\n--- base " + d.name + ": " + colecciones.join(", "));
    ["groups", "devices", "csubs"].forEach(function (c) {
        if (colecciones.indexOf(c) >= 0) {
            print("[" + c + "]");
            base.getCollection(c).find().forEach(function (x) { print(EJSON.stringify(x)); });
        }
    });
    if (colecciones.indexOf("entities") >= 0) {
        print("[entities] total: " + base.entities.countDocuments());
        base.entities.find().limit(3).forEach(function (x) { print(EJSON.stringify(x)); });
    }
});
' | redactar

seccion "CrateDB: tablas de QuantumLeap, columnas y ultimas filas"
python3 -I - <<'EOF'
import json, urllib.request

def sql(stmt):
    req = urllib.request.Request("http://localhost:4200/_sql",
                                 data=json.dumps({"stmt": stmt}).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)

try:
    print("Version:", sql("SELECT version['number'] FROM sys.nodes")["rows"])
    tablas = sql("SELECT table_schema, table_name FROM information_schema.tables "
                 "WHERE table_schema LIKE 'mt%' ORDER BY 1, 2")["rows"]
    if not tablas:
        print("No hay tablas mt*: QuantumLeap aun no ha guardado nada.")
    for esquema, tabla in tablas:
        nombre = '"%s"."%s"' % (esquema, tabla)
        cols = sql("SELECT column_name, data_type FROM information_schema.columns "
                   "WHERE table_schema = '%s' AND table_name = '%s' ORDER BY ordinal_position"
                   % (esquema, tabla))["rows"]
        total = sql("SELECT count(*) FROM %s" % nombre)["rows"][0][0]
        print("\n--- %s  (%s filas)" % (nombre, total))
        print("columnas:", ", ".join("%s:%s" % (c, t) for c, t in cols))
        ultimas = sql("SELECT * FROM %s ORDER BY time_index DESC LIMIT 3" % nombre)
        print(ultimas["cols"])
        for fila in ultimas["rows"]:
            print(fila)
except Exception as e:
    print("Error consultando CrateDB:", e)
EOF

seccion "Ultimos logs (iot-agent, orion, quantumleap)"
docker compose logs --tail=40 --no-color iot-agent orion quantumleap | redactar
