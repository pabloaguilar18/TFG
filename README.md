# Proyecto TFG: Plataforma IoT con MQTT, InfluxDB, API REST y Aplicación Web

Este proyecto implementa una solución completa para la recogida, almacenamiento y visualización de datos IoT:

- 📡 **Recolección de datos vía MQTT**
- 💾 **Almacenamiento en InfluxDB**
- 🌐 **Exposición de API REST con Flask**
- 🖥️ **Visualización mediante una aplicación web estática en Nginx**
- 🐳 **Orquestación de todos los servicios con Docker Compose**

---

## 📁 Estructura del proyecto

```
├── api_base_de_datos/ ← Servicio MQTT → InfluxDB
│   ├── api_base_de_datos.py
│   ├── Dockerfile
│   └── requirements.txt
├── api_datos/ ← API REST con Flask
│   ├── api_datos.py
│   ├── Dockerfile
│   └── requirements.txt
├── aplicacion_web/ ← Aplicación web estática (HTML/JS)
│   ├── dashboard.html
│   ├── Dockerfile
│   └── nginx.conf
├── .env
├── docker-compose.yml
└── README.md
```

---

## 🛠 Pre-requisitos

* [Docker](https://docs.docker.com/get-docker/) instalado.
* [Docker Compose](https://docs.docker.com/compose/install/) instalado.
* Conexión a Internet para descargar las imágenes.

---

## ⚙️ Variables de entorno

Copiar y ajustar el fichero de ejemplo `.env` en la raíz:

```dotenv
# Conexión MQTT (Ejemplo):
MQTT_BROKER=tu_mqtt_broker
MQTT_PORT=8883
MQTT_USERNAME=usuario_mqtt
MQTT_PASSWORD=contraseña_mqtt
MQTT_TOPIC=/TFG/#

# Conexión InfluxDB (Ejemplo):
INFLUX_URL=http://influxdb:8086
INFLUX_TOKEN=RjH5k0hd!4QO
INFLUX_ORG=TFG_Pablo
INFLUX_BUCKET=TFG_bucket

```

---

## 🚀 Despliegue con Docker Compose

En la raíz del proyecto ejecutar:

```bash
docker-compose up -d --build
```

Esto levantará los siguientes servicios:

|       Servicio        |                 Descripción                 |  Puerto  |
| --------------------- | ------------------------------------------- | -------- |
| **influxdb**          | Base de datos de series temporales          |   8086   |
| **api_base_de_datos** | Cliente MQTT que vuelca datos a InfluxDB    |    —     |
| **aplicacion_web**    | Interfaz web estática vía Nginx             |    80    |
| **api_datos**         | API REST con Flask                          |   5000   |

Volúmenes persisten datos de InfluxDB y Grafana:

```yaml
volumes:
  influxdb-data:
```

---

## 🔧 Descripción de componentes

### 1. Servicio MQTT → InfluxDB

* Se conecta al broker definido en las variables de entorno.
* Se suscribe al tópico `MQTT_TOPIC`.
* Almacena en InfluxDB:
  * Measurement = nombre de la medida
  * Tags: `User`, `Device`
  * Field: `Value`

### 2. API REST (Flask)

Base URL: http://localhost:5000 (o /api/ si accedes desde la web)

Endpoints principales:

* **GET** `/Ping` → Comprueba conexión.

* **GET** `/Data/<User>`

  * Parámetros opcionales: `Device`, `Measure`, `Start`, `Stop`, `Days`.
  * Devuelve datos agrupados por dispositivo y medida.

* **GET** `/Data/<User>/<Device>`

  * Parámetro opcional: `Measure`.

* **GET** `/Data/<User>/<Device>/<Measure>` → Devuelve todos los datos de esa medida

* **GET** `/Data/<User>/hours` → Rangos con `StartHour` y `StopHour` (formato `YYYY-MM-DDTHH:MM`).

La lógica de rango (`range`) se adapta automáticamente al huso `Europe/Madrid`.

* **GET** `/Users` → Devuelve lista única de usuarios

* **GET** `/Users/<User>/Devices` → Devuelve una lista única de dispositivos de ese usuario

* **POST** `/SendFrequency`
Content-Type: application/json

{
  "user": "Pablo",
  "devices": ["Sensor1", "Sensor2"],
  "frequency": 60
}

* Publica en los tópicos: TFG/<Usuario>/<Dispositivo>/Frecuency

* **GET** `/mqtt/status` → Verifica si la API está conectada al broker MQTT

---

## 📊 Aplicación Web

  * URL: `http://localhost`
  * Servida por nginx desde el contenedor aplicacion_web
  * El archivo principal es dashboard.html
  * Usa nginx.conf para redirigir automáticamente /api/ hacia la API Flask

---

## 📝 Uso y ejemplos

### Simulación de envío MQTT (ejemplo con `mosquitto_pub`)

```bash
mosquitto_pub -h <broker> -p 8883 -u usuario_mqtt -P contraseña_mqtt \
  -t "/TFG/Pablo/Sensor1/Temperatura" -m "22.5"
```

### Consulta API
```bash
# Datos últimos 7 días para usuario 'Pablo'
curl "http://localhost:5000/Data/Pablo?Days=7"

# Datos de un dispositivo concreto
curl "http://localhost:5000/Data/Pablo/Sensor1?Measure=Temperatura&Days=1"
```

### Envío de frecuencia a dispositivos
```bash
curl -X POST http://localhost/api/SendFrequency \
  -H "Content-Type: application/json" \
  -d '{"user":"Pablo","devices":["Sensor1"],"frequency":30}'
```

---