import uuid
import paho.mqtt.client as paho
from paho import mqtt
from paho.mqtt.client import CallbackAPIVersion
import os
from dotenv import load_dotenv
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

load_dotenv()

Client_ID = f"Client-{uuid.uuid4()}"

MQTT_BROKER = os.getenv("MQTT_BROKER")
MQTT_PORT = int(os.getenv("MQTT_PORT", 8883))
MQTT_USERNAME = os.getenv("MQTT_USERNAME")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")
MQTT_TOPIC = os.getenv("MQTT_TOPIC")
INFLUX_URL = os.getenv("INFLUX_URL")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN")
INFLUX_ORG = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET")

Influxdb = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
Write_API = Influxdb.write_api(write_options=SYNCHRONOUS)

def OnConnect(Client, Userdata, Flags, RC, Properties=None):
    if RC == 0:
        print("Successful MQTT connection")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"MQTT connection failure, code: {RC}")
        Error_Codes = {
            1: "Incorrect protocol version",
            2: "Invalid client identifier", 
            3: "Server not available",
            4: "Incorrect user or password",
            5: "Unauthorised"
        }
        print(f"Reason: {Error_Codes.get(RC, 'Unknown error')}")

def OnSubscribe(Client, Userdata, Mid, Granted_QOS, Properties=None):
    print(f"Successfully subscribed to the topic '{MQTT_TOPIC}'")

def OnMessage(Client, Userdata, Message):
    # Topic has the form: /TFG/User/Device/Measurement
    Topic = Message.topic
    Payload = Message.payload.decode(errors='ignore')
    Parts = Topic.strip('/').split('/')
    if len(Parts) != 4:
        return
    _, User, Device, Measure = Parts
    try:
        Value = float(Payload)
    except ValueError:
        return

    point = (
        Point(Measure)
        .tag("User", User)
        .tag("Device", Device)
        .field("Value", Value)
    )
    Write_API.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    print(f"Save: {User}/{Device}/{Measure} = {Value}")

client = paho.Client(client_id=Client_ID, userdata=None, protocol=paho.MQTTv5, callback_api_version=CallbackAPIVersion.VERSION2)
client.on_connect = OnConnect
client.tls_set(tls_version=mqtt.client.ssl.PROTOCOL_TLS)
client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

try:
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.on_subscribe = OnSubscribe
    client.on_message = OnMessage
    client.loop_forever()
except KeyboardInterrupt:
    client.disconnect()
except Exception as e:
    print(f"Error: {e}")
    import traceback
    traceback.print_exc()