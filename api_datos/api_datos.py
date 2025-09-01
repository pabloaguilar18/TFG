import os
import uuid
from flask import Flask, jsonify, request
from dotenv import load_dotenv
from datetime import datetime
from zoneinfo import ZoneInfo
from influxdb_client import InfluxDBClient
from flask_cors import CORS
import paho.mqtt.client as paho
from paho import mqtt
from paho.mqtt.client import CallbackAPIVersion

load_dotenv()

# InfluxDB Configuration
INFLUX_URL    = os.getenv("INFLUX_URL")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN")
INFLUX_ORG    = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET")

# MQTT Configuration
MQTT_BROKER = os.getenv("MQTT_BROKER")
MQTT_PORT = int(os.getenv("MQTT_PORT", 8883))
MQTT_USERNAME = os.getenv("MQTT_USERNAME")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")

Client    = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
API_Query = Client.query_api()
App = Flask(__name__)
CORS(App)  # Enable CORS for all routes

LOCAL_TZ = ZoneInfo("Europe/Madrid")

# MQTT Client Configuration
Client_ID = f"Flask-API-{uuid.uuid4()}"
mqtt_client = paho.Client(client_id=Client_ID, userdata=None, protocol=paho.MQTTv5, callback_api_version=CallbackAPIVersion.VERSION2)

def setup_mqtt():
    """Initialize MQTT connection"""
    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("MQTT connected successfully for API")
        else:
            print(f"MQTT connection failed with code: {rc}")
    
    def on_disconnect(client, userdata, rc, properties=None):
        print(f"MQTT disconnected with code: {rc}")
    
    def on_publish(client, userdata, mid, properties=None):
        print(f"Message published with mid: {mid}")
        
    def on_log(client, userdata, level, buf):
        print(f"MQTT Log: {buf}")
    
    mqtt_client.on_connect = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_publish = on_publish
    mqtt_client.on_log = on_log
    
    # Configure TLS and authentication
    mqtt_client.tls_set(tls_version=mqtt.client.ssl.PROTOCOL_TLS)
    mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        mqtt_client.loop_start()  # Start the loop in a separate thread
        print("MQTT client initialized and connected")
        return True
    except Exception as e:
        print(f"Error connecting to MQTT: {e}")
        return False

def MakeRangeClause():
    """
    Build the range for Influxdb in the Europe/Madrid zone.
    - If Start (YYYYY-MM-DD) and Stop (YYYYY-MM-DD) are given, use the local range.
    - If ?Days=N is passed, use last N days.
    - By default, last 7 days.
    """
    Start = request.args.get("Start")
    Stop  = request.args.get("Stop")
    Days  = request.args.get("Days")
    # Fixed range
    if Start and Stop:
        Dt_Start = datetime.fromisoformat(Start).replace(tzinfo=LOCAL_TZ, hour=0, minute=0, second=0)
        Dt_Stop  = datetime.fromisoformat(Stop).replace(tzinfo=LOCAL_TZ, hour=23, minute=59, second=59)
        return f'range(start: time(v: "{Dt_Start.isoformat()}"), stop: time(v: "{Dt_Stop.isoformat()}"))'
    # Last N days
    if Days:
        try:
            n = int(Days)
            return f'range(start: -{n}d)'
        except ValueError:
            pass
    # Default 7 días
    return 'range(start: -7d)'

@App.route('/Ping')
def Ping():
    return jsonify({"Status": "All is fine!"})

@App.route('/Data/<User>', methods=['GET'])
def GetUserData(User):
    Device = request.args.get("Device")
    Measure = request.args.get("Measure")
    Filters = [f'r["User"] == "{User}"']
    if Device:
        Filters.append(f'r["Device"] == "{Device}"')
    if Measure:
        Filters.append(f'r._measurement == "{Measure}"')
    Flux = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> {MakeRangeClause()}
      |> filter(fn: (r) => {' and '.join(Filters)})
    '''
    try:
        Tables = API_Query.query(Flux, org=INFLUX_ORG)
        Data = {}
        for Table in Tables:
            for Rec in Table.records:
                Dev = Rec.values["Device"]
                Meas = Rec.get_measurement()
                # Convert timestamp to local zone
                Ts = Rec.get_time().astimezone(LOCAL_TZ).isoformat()
                Value = Rec.get_value()
                Data.setdefault(Dev, {}).setdefault(Meas, []).append({
                    "timestamp": Ts,
                    "Value": Value
                })
        return jsonify({"User": User, "Data": Data})
    except Exception as e:
        return jsonify({"Error": "Error processing query", "Detail": str(e)}), 400

@App.route('/Data/<User>/<Device>', methods=['GET'])
def GetUserDeviceData(User, Device):
    Measure = request.args.get("Measure")
    Filters = [f'r["User"] == "{User}"', f'r["Device"] == "{Device}"']
    if Measure:
        Filters.append(f'r._measurement == "{Measure}"')
    Flux = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> {MakeRangeClause()}
      |> filter(fn: (r) => {' and '.join(Filters)})
    '''
    try:
        Tables = API_Query.query(Flux, org=INFLUX_ORG)
        Data = {}
        for Table in Tables:
            for Rec in Table.records:
                Meas = Rec.get_measurement()
                Ts = Rec.get_time().astimezone(LOCAL_TZ).isoformat()
                Value = Rec.get_value()
                Data.setdefault(Meas, []).append({"timestamp": Ts, "Value": Value})
        return jsonify({"User": User, "Device": Device, "Data": Data})
    except Exception as e:
        return jsonify({"Error": "Error processing query", "Detail": str(e)}), 400

@App.route('/Data/<User>/<Device>/<Measure>', methods=['GET'])
def GetSpecificMeasurement(User, Device, Measure):
    Flux = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> {MakeRangeClause()}
      |> filter(fn: (r) =>
         r["User"] == "{User}" and
         r["Device"] == "{Device}" and
         r._measurement == "{Measure}"
      )
    '''
    try:
        Tables = API_Query.query(Flux, org=INFLUX_ORG)
        Data = []
        for Table in Tables:
            for Rec in Table.records:
                Data.append({
                    "timestamp": Rec.get_time().astimezone(LOCAL_TZ).isoformat(),
                    "Value": Rec.get_value()
                })
        return jsonify({"User": User, "Device": Device, "Measure": Measure, "Data": Data})
    except Exception as e:
        return jsonify({"Error": "Error processing query", "Detail": str(e)}), 400

@App.route('/Data/<User>/hours', methods=['GET'])
def GetUserDataByHours(User):
    """
    Get user data with specific hour range using StartHour and StopHour parameters.
    Format: YYYY-MM-DDTHH:MM (e.g., 2025-06-20T14:30)
    """
    Device = request.args.get("Device")
    Measure = request.args.get("Measure")
    StartHour = request.args.get("StartHour")
    StopHour = request.args.get("StopHour")
    
    # Validate required parameters
    if not StartHour or not StopHour:
        return jsonify({"Error": "StartHour and StopHour parameters are required", 
                       "Format": "YYYY-MM-DDTHH:MM"}), 400
    
    try:
        # Parse the datetime strings and add timezone info
        Dt_Start = datetime.fromisoformat(StartHour).replace(tzinfo=LOCAL_TZ)
        Dt_Stop = datetime.fromisoformat(StopHour).replace(tzinfo=LOCAL_TZ)
        
        # Build the range clause for specific hours
        RangeClause = f'range(start: time(v: "{Dt_Start.isoformat()}"), stop: time(v: "{Dt_Stop.isoformat()}"))'
        
    except ValueError as e:
        return jsonify({"Error": "Invalid datetime format", 
                       "Detail": str(e),
                       "Format": "Use YYYY-MM-DDTHH:MM"}), 400
    
    # Build filters
    Filters = [f'r["User"] == "{User}"']
    if Device:
        Filters.append(f'r["Device"] == "{Device}"')
    if Measure:
        Filters.append(f'r._measurement == "{Measure}"')
    
    # Build Flux query
    Flux = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> {RangeClause}
      |> filter(fn: (r) => {' and '.join(Filters)})
    '''
    
    try:
        Tables = API_Query.query(Flux, org=INFLUX_ORG)
        Data = {}
        for Table in Tables:
            for Rec in Table.records:
                Dev = Rec.values["Device"]
                Meas = Rec.get_measurement()
                # Convert timestamp to local zone
                Ts = Rec.get_time().astimezone(LOCAL_TZ).isoformat()
                Value = Rec.get_value()
                Data.setdefault(Dev, {}).setdefault(Meas, []).append({
                    "timestamp": Ts,
                    "Value": Value
                })
        
        return jsonify({
            "User": User, 
            "TimeRange": f"{StartHour} to {StopHour}",
            "Data": Data
        })
        
    except Exception as e:
        return jsonify({"Error": "Error processing query", "Detail": str(e)}), 400

@App.route('/Users', methods=['GET']) # Return unique list of users
def list_users():
    flux = f'''
    import "influxdata/influxdb/schema"
    schema.tagValues(bucket: "{INFLUX_BUCKET}", tag: "User")
    '''
    result = API_Query.query(org=INFLUX_ORG, query=flux)
    users = sorted({rec.get_value() for table in result for rec in table.records})
    return jsonify(users)

@App.route('/Users/<User>/Devices', methods=['GET']) # Return unique list of devices for a specific user
def list_devices(User):
    flux = f'''
    import "influxdata/influxdb/schema"
    schema.tagValues(bucket: "{INFLUX_BUCKET}"
      , tag: "Device"
      , predicate: (r) => r["User"] == "{User}"
    )
    '''
    result = API_Query.query(org=INFLUX_ORG, query=flux)
    devices = sorted({rec.get_value() for table in result for rec in table.records})
    return jsonify(devices)

@App.route('/SendFrequency', methods=['POST'])
def send_frequency():
    try:
        data = request.get_json()
        user = data.get('user')
        devices = data.get('devices')
        frequency = data.get('frequency')
        
        print(f"Received data: {data}")
        print(f"User: {user}, Devices: {devices}, Frequency: {frequency}")
        
        if not user or not devices or not frequency:
            return jsonify({'error': 'User, devices and frequency are required'}), 400
            
        if not isinstance(frequency, int) or frequency < 1 or frequency > 300:
            return jsonify({'error': 'Frequency must be an integer between 1 and 300'}), 400
            
        if not isinstance(devices, list) or len(devices) == 0:
            return jsonify({'error': 'At least one device must be selected'}), 400
        
        # Check if MQTT is connected
        if not mqtt_client.is_connected():
            return jsonify({'error': 'MQTT client is not connected'}), 500
        
        # Publish to MQTT for each device
        success_count = 0
        failed_devices = []
        
        for device in devices:
            try:
                topic = f"TFG/{user}/{device}/Frecuency"
                result = mqtt_client.publish(topic, str(frequency))
                result.wait_for_publish()
                
                if result.rc == paho.MQTT_ERR_SUCCESS:
                    print(f"Sent frequency {frequency} to topic: {topic}")
                    success_count += 1
                else:
                    print(f"Failed to send to device {device}: {result.rc}")
                    failed_devices.append(device)
                    
            except Exception as e:
                print(f"Error sending to device {device}: {e}")
                failed_devices.append(device)
        
        if success_count == len(devices):
            return jsonify({
                'success': True, 
                'message': f'Frequency {frequency} sent to {len(devices)} devices'
            })
        else:
            return jsonify({
                'success': True,
                'message': f'Frequency sent to {success_count}/{len(devices)} devices',
                'warning': 'Some devices may not have received the message',
                'failed_devices': failed_devices
            })
        
    except Exception as e:
        print(f"Error in send_frequency: {e}")
        return jsonify({'error': str(e)}), 500

@App.route('/mqtt/status', methods=['GET'])
def mqtt_status():
    """Check MQTT connection status"""
    return jsonify({
        'connected': mqtt_client.is_connected(),
        'client_id': Client_ID
    })

if __name__ == '__main__':
    # Initialize MQTT connection
    mqtt_connected = setup_mqtt()
    if not mqtt_connected:
        print("Warning: MQTT connection failed, but starting Flask app anyway")
    
    try:
        App.run(host='0.0.0.0', port=5000)
    finally:
        # Clean up MQTT connection when Flask app stops
        mqtt_client.loop_stop()
        mqtt_client.disconnect()