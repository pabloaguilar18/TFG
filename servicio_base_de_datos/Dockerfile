FROM python:3.12

ENV PYTHONUNBUFFERED=1

WORKDIR /app

COPY . .

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    rm -rf /var/lib/apt/lists/* && pip install --no-cache-dir -r requirements.txt

CMD ["python", "api_base_de_datos.py"]