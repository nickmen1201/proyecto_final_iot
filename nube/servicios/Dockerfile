# Imagen unica para limpiar, analitica y api: mismos datos, mismas
# dependencias. Cada servicio del compose la arranca con su propio command.
FROM python:3.15.2-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY *.py .

CMD ["python", "limpiar.py"]
