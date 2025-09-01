#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>

static const char *MQTT_Server = "fe5308abbd84497a99f9a52639b36f3f.s1.eu.hivemq.cloud";
static const int MQTT_Port = 8883;
static const char *MQTT_User = "mqtt_user";
static const char *MQTT_Password = "Kproject2024+";

#define EEPROM_SIZE 240
#define ADDR_STATUS 0
#define ADDR_USER_NAME 20
#define ADDR_WIFI_SSID 60
#define ADDR_WIFI_PASSWORD 130
#define ADDR_WIFI_CHANNEL 200
#define ADDR_FRECUENCY 210
#define ADDR_DEVICE 220

#define PIN_BLUE_LED 18
#define PIN_GREEN_LED 19
#define PIN_RED_LED 21
#define PIN_CONFIG 23
#define RX_PIN 16
#define TX_PIN 17

#define NUM_BLINKS 3
#define TIME_TO_PAIRING 2000 //In ms
#define MIN_TIME_AUTOPAIRING_HALTEN 3000
#define MAX_TIME_AUTOPAIRING_HALTEN 5000
#define INIT_TIME_CONFIGURATION_HALTEN 5000
#define NO_CONFIGURATION_HALTEN_TIME 10000
#define MQTT_CHECK_INTERVAL 1800000 // 30 min
#define WIFI_CHECK_INTERVAL 900000 // 15 min
#define CHANNEL_CHECK_INTERVAL 3600000 // 1 hour
#define MAX_MQTT_RECONNECT_ATTEMPTS 3
#define MAX_WIFI_RECONNECT_ATTEMPTS 5

AsyncWebServer Web_Server(80);
WiFiClientSecure Secure_Client;
PubSubClient MQTT_Client(Secure_Client);
String SSID_WiFi = "";
String WiFi_Password = "";
String User_Name = "";
String Device = "";
bool Is_Pairing = false;
bool New_Pairing_Event = false;
uint8_t WiFi_Channel = 1;
unsigned long Initial_Pairing_Time = 0;
unsigned long Elapsed_Pairing_Time = 0;
unsigned long Time_Start_Config = 0;
bool Turn_On_WiFi_Led = false;
std::vector<String> Available_SSIDs;
bool Turn_On_Configuration_Leds = true;
bool Configuration_Status = false;
bool Connection_Success = false;
unsigned long Last_Sensor_Reading = 0;
unsigned long Last_MQTT_Check = 0;
unsigned long Last_WiFi_Check = 0;
unsigned long Last_Channel_Check = 0;
int MQTT_Reconnect_Attempts = 0;
int WiFi_Reconnect_Attempts = 0;
int Sensor_Reading_Interval = 5; // In seconds
bool Force_Channel_Recalculation = false;
HardwareSerial Serial_Port(2);
PZEM004Tv30 Pzem(Serial_Port, RX_PIN, TX_PIN);

enum Message_Type : uint8_t { PAIRING = 0, DATA = 1, CHANNEL_CHANGE = 2, FRECUENCY = 3 };
struct Pairing_Struct { uint8_t Message_Type; uint8_t ID;  uint8_t WiFi_Channel; char Device[40]; };struct Message_Struct{ uint8_t Message_Type; float Voltage; float Current; float Power; char Device[40];};

void ScanAvailableNetworks(){
    Available_SSIDs.clear();
    int n = WiFi.scanNetworks();
    for(int i=0; i<n; i++){
        Available_SSIDs.push_back(WiFi.SSID(i));
    }
    WiFi.scanDelete();
}

uint8_t CalculateWiFiChannel(const char *SSID_WiFi){
    int Num = WiFi.scanNetworks();
   
    for(int i = 0; i < Num; i++){
        if(WiFi.SSID(i) == String(SSID_WiFi)){
            uint8_t Channel = WiFi.channel(i);
            WiFi.scanDelete();
            return Channel;
        }
        yield();
    }
    WiFi.scanDelete();

    return 1; // Default channel if not found
}

void PostOnMQTT(String Topic, String Device, float Data_Value){
    String Publicar = "TFG/" + User_Name + "/" + Device + Topic;
    MQTT_Client.publish(Publicar.c_str(), String(Data_Value, 2).c_str(), false);
}

void AddFriend(const uint8_t *MAC){
    esp_now_del_peer(MAC);
    esp_now_peer_info_t Friend;
    memset(&Friend, 0, sizeof(esp_now_peer_info_t));
    memcpy(Friend.peer_addr, MAC, 6);
    Friend.channel = WiFi_Channel;
    Friend.encrypt = false;
    esp_now_add_peer(&Friend);
}

void SendByESPNOW(const uint8_t *MAC, const uint8_t *Data_On_the_Way, uint8_t Message_Type, size_t Data_Size){
    AddFriend(MAC);
    Pairing_Struct Pairing_Data;
    memcpy(&Pairing_Data, Data_On_the_Way, sizeof(Pairing_Data));
    Serial.print("\n"); // Necessary for ESPNOW to work properly
    esp_now_send(MAC, (uint8_t *) &Pairing_Data, Data_Size);
    esp_now_del_peer(MAC);
}

void MessageReceived(const uint8_t *MAC, const uint8_t *Data_On_the_Way, int Length){
    Serial.println("\nHE RECIBIDO UN MENSAJE DE: " + String(MAC[0], HEX) + ":" + String(MAC[1], HEX) + ":" + String(MAC[2], HEX) + ":" + String(MAC[3], HEX) + ":" + String(MAC[4], HEX) + ":" + String(MAC[5], HEX));
    Pairing_Struct Pairing_Data;
    memcpy(&Pairing_Data, Data_On_the_Way, sizeof(Pairing_Data));
    switch(Pairing_Data.Message_Type){ // I verify the type of message received
        case PAIRING:
            if((Is_Pairing == 1) && (Pairing_Data.ID == 1)){ // If the pairing is active and the ID is 1 (not coordinator)
                New_Pairing_Event = true;
                Pairing_Data.Message_Type = PAIRING;
                Pairing_Data.ID = 0;
                Pairing_Data.WiFi_Channel = WiFi_Channel;
                SendByESPNOW(MAC, (uint8_t *) &Pairing_Data, PAIRING, sizeof(Pairing_Data));
            }
            break;
        case DATA:
            Message_Struct Measurement_Data;
            memcpy(&Measurement_Data, Data_On_the_Way, sizeof(Measurement_Data));
            String Device_Name = String(Measurement_Data.Device);
            PostOnMQTT("/Voltage", Device_Name, Measurement_Data.Voltage);
            PostOnMQTT("/Current", Device_Name, Measurement_Data.Current);
            PostOnMQTT("/Power", Device_Name, Measurement_Data.Power);
            break;
    }
}

void BlinkLed(int Num_Times, int Led_Pin, int Time){
    if(Led_Pin != PIN_BLUE_LED){
        digitalWrite(Led_Pin, HIGH);
        delay(Time);
        Turn_On_WiFi_Led = false;
    }

    for(int i = 0; i < Num_Times; i++){
        digitalWrite(Led_Pin, HIGH);
        delay(250);
        digitalWrite(Led_Pin, LOW);
        delay(250);
    }
}

void TurnOn2Leds(int Led_Pin_1, int Led_Pin_2){
    digitalWrite(Led_Pin_1, HIGH);
    digitalWrite(Led_Pin_2, HIGH);
    delay(500);
    digitalWrite(Led_Pin_1, LOW);
    digitalWrite(Led_Pin_2, LOW);
    delay(500);
}

void WiFiConfiguration(const char * SSID_WiFi, const char *WiFi_Password){
    Turn_On_Configuration_Leds = false;
    WiFi.begin(SSID_WiFi, WiFi_Password);
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 15000){
        delay(500);
        yield();
    }

    Configuration_Status = true;
    
    if(WiFi.status() != WL_CONNECTED){
        BlinkLed(NUM_BLINKS, PIN_RED_LED, 3000);
        delay(1000);
        Turn_On_Configuration_Leds = true;
        return;
    } 

    Connection_Success = true;
    
    if(Turn_On_WiFi_Led)
        BlinkLed(NUM_BLINKS, PIN_GREEN_LED, 3000);
    
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    delay(200);
}

void MQTTConfiguration(){
    int Retry_Count = 0;
    int Max_Retries = 5;

    if(WiFi.status() != WL_CONNECTED)
        ESP.restart();

	while(!MQTT_Client.connected() && Retry_Count < Max_Retries){
        String ID_Cliente = "TFG" + String(random(0xFFFF), HEX); // I generate a random client ID for MQTT

		if(MQTT_Client.connect(ID_Cliente.c_str(), MQTT_User, MQTT_Password)){
			Serial.println("¡CONECTADO A MQTT!");
            MQTT_Client.subscribe(("TFG/" + User_Name + "/#").c_str()); // I subscribe to the topic for receiving messages
        }
		else{
            Retry_Count++;
            if(Retry_Count >= Max_Retries){
                BlinkLed(10, PIN_RED_LED, 10000); // If it fails to connect to MQTT, blink the red LED
                delay(1000);
                ESP.restart();
            }
            delay(2000);
		}
	}
}

void CallBackMQTT(char *Topic, byte *Message_Payload, unsigned int Size){
    String Topic_String = String(Topic);
    if(Topic_String.endsWith("/Autopairing")){
        Is_Pairing = true;
        Initial_Pairing_Time = millis();
    }
    if (Topic_String.endsWith("/Frecuency")) {
        int lastSlash = Topic_String.lastIndexOf('/');
        int secondLastSlash = Topic_String.lastIndexOf('/', lastSlash - 1);
        String Device_From_Topic = Topic_String.substring(secondLastSlash + 1, lastSlash);

        String Payload_Str;
        for (int i = 0; i < Size; i++) {
            Payload_Str += (char)Message_Payload[i];
        }
        int New_Frecuency = Payload_Str.toInt();

        if (Device_From_Topic == Device) {
            Sensor_Reading_Interval = New_Frecuency;
            EEPROM.writeInt(ADDR_FRECUENCY, Sensor_Reading_Interval);
            EEPROM.commit();
        } else {
            Pairing_Struct Message;
            Message.Message_Type = FRECUENCY;
            Message.ID = 0;
            Message.WiFi_Channel = New_Frecuency;
            strncpy(Message.Device, Device_From_Topic.c_str(), sizeof(Message.Device));
            Message.Device[sizeof(Message.Device) - 1] = '\0';

            const uint8_t Broadcast_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            SendByESPNOW(Broadcast_MAC, (uint8_t *)&Message, FRECUENCY, sizeof(Message));
        }
    }
}

bool SaveConfiguration(String User, String SSID_WiFi, String WiFi_Password, String Device){
    EEPROM.writeString(ADDR_STATUS, "CONFIGURED");
    EEPROM.writeString(ADDR_USER_NAME, User);
    EEPROM.writeString(ADDR_WIFI_SSID, SSID_WiFi);
    EEPROM.writeString(ADDR_WIFI_PASSWORD, WiFi_Password);
    EEPROM.writeString(ADDR_DEVICE, Device);
    
    if(EEPROM.commit())
        return true;
    else
        return false;
}

bool ValidateEEPROMData(){
    User_Name = EEPROM.readString(ADDR_USER_NAME);
    SSID_WiFi = EEPROM.readString(ADDR_WIFI_SSID);
    WiFi_Password = EEPROM.readString(ADDR_WIFI_PASSWORD);
    WiFi_Channel = EEPROM.read(ADDR_WIFI_CHANNEL);
    Device = EEPROM.readString(ADDR_DEVICE);
    Sensor_Reading_Interval = EEPROM.readInt(ADDR_FRECUENCY);
    
    if((User_Name.length() == 0) || (User_Name.length() > 30) || (SSID_WiFi.length() == 0) || (SSID_WiFi.length() > 32) || 
    (WiFi_Password.length() == 0) || (WiFi_Password.length() > 64) || (Device.length() == 0) || (Device.length() > 30) || 
    (WiFi_Channel < 1) || (WiFi_Channel > 13) || (Sensor_Reading_Interval < 1) || (Sensor_Reading_Interval > 300)){
        for(int i = 0; i < 5; i++){
            BlinkLed(1, PIN_RED_LED, 0);
            BlinkLed(1, PIN_BLUE_LED, 0);
        }
        return false;
    }
    return true;
}

void IfButtonPressed(bool In_The_Loop){
    if(digitalRead(PIN_CONFIG) == LOW){
        unsigned long Time = 0;
        while(digitalRead(PIN_CONFIG) == LOW){
            Time += 100;
            if((Time == MIN_TIME_AUTOPAIRING_HALTEN) && (In_The_Loop))
                BlinkLed(1, PIN_BLUE_LED, 0);
            else if(Time == INIT_TIME_CONFIGURATION_HALTEN)
                TurnOn2Leds(PIN_RED_LED, PIN_GREEN_LED);
            else if(Time == NO_CONFIGURATION_HALTEN_TIME)
                TurnOn2Leds(PIN_BLUE_LED, PIN_RED_LED);
            delay(100);
        }
        if(((Time >= MIN_TIME_AUTOPAIRING_HALTEN ) && (Time < MAX_TIME_AUTOPAIRING_HALTEN)) && (In_The_Loop)){
            Initial_Pairing_Time = millis();
            Is_Pairing = true;
        }
        else if((Time >= INIT_TIME_CONFIGURATION_HALTEN) && (Time < NO_CONFIGURATION_HALTEN_TIME)){
            EEPROM.writeString(0, "NO_CONFIGURED");
            EEPROM.writeInt(ADDR_FRECUENCY, 5);
            EEPROM.commit();
            TurnOn2Leds(PIN_RED_LED, PIN_GREEN_LED);
            ESP.restart();
        }
        else if(Time >= NO_CONFIGURATION_HALTEN_TIME){
            EEPROM.writeString(0, "CONFIGURED");
            EEPROM.commit();
            TurnOn2Leds(PIN_BLUE_LED, PIN_RED_LED);
            ESP.restart();
        }
    }
}

void ReadAndSendSensorData(){
    unsigned long current_time = millis();
    
    if(current_time - Last_Sensor_Reading >= Sensor_Reading_Interval * 1000){
        Last_Sensor_Reading = current_time;

        float Voltage = Pzem.voltage();
        float Current = Pzem.current();
        float Power = Pzem.power();
        
        if((MQTT_Client.connected()) && (!isnan(Voltage))){
            PostOnMQTT("/Current", Device, Current);
            PostOnMQTT("/Voltage", Device, Voltage);
            PostOnMQTT("/Power", Device, Power);
        }
    }
}

bool IsWiFiChannelChanged(){
    uint8_t New_WiFi_Channel = CalculateWiFiChannel(SSID_WiFi.c_str());

    if(New_WiFi_Channel < 1 || New_WiFi_Channel > 13)
        New_WiFi_Channel = WiFi_Channel;
    
    if(WiFi_Channel != New_WiFi_Channel){
        Serial.println("EL CANAL WIFI HA CAMBIADO DE " + String(WiFi_Channel) + " A " + String(New_WiFi_Channel));
        
        ESP_ERROR_CHECK(esp_wifi_set_channel(New_WiFi_Channel,  WIFI_SECOND_CHAN_NONE));
        if(esp_now_init() != ESP_OK)
            esp_restart();
        uint8_t Broadcast_Addr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        Pairing_Struct Pairing_Data;
        Pairing_Data.Message_Type = PAIRING;
        Pairing_Data.WiFi_Channel = New_WiFi_Channel;
        Pairing_Data.ID = 0;
        SendByESPNOW(Broadcast_Addr, (uint8_t *) &Pairing_Data, CHANNEL_CHANGE, sizeof(Pairing_Data));
        delay(1000);
        WiFi_Channel = New_WiFi_Channel;
        EEPROM.write(ADDR_WIFI_CHANNEL, WiFi_Channel);
        EEPROM.commit();
        return true;
    }

    WiFi.disconnect();
    return false;
}

String DeleteBlankSpaces(String Input){
    for(int i = 0; i < Input.length(); i++){
        if(Input.charAt(i) == ' '){
            Input.remove(i, 1);
            i--;
        }
    }
    return Input;
}

void startConfigAP() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("TFG KPROJECT", "Kproject2024+");
    delay(200);
    IPAddress IP(192,168,68,1), Gateway(192,168,68,1), Netmask(255,255,255,0);
    WiFi.softAPConfig(IP, Gateway, Netmask);

    Web_Server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req){
        String json = "{";
        json += "\"Complete_Configuration\":" + String(Configuration_Status ? "true" : "false") + ",";
        json += "\"WiFi_Connected\":" + String(Connection_Success ? "true" : "false") + ",";
        json += "\"tiempo_transcurrido\":" + String((millis() - Time_Start_Config) / 1000);
        json += "}";
        req->send(200, "application/json", json);
    });

    Web_Server.on("/restart", HTTP_POST, [](AsyncWebServerRequest* req){
        if(Connection_Success){
            req->send(200, "application/json", "{\"message\":\"Reiniciando dispositivo...\"}");
            delay(200);
            ESP.restart();
        } else {
            req->send(400, "application/json", "{\"error\":\"No se puede reiniciar, configuración no exitosa\"}");
        }
    });

    Web_Server.on("/config", HTTP_POST, [](AsyncWebServerRequest* request){}, NULL, 
    [](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
        String JSON_Recibido = "";
        for(size_t i = 0; i < len; i++){
            JSON_Recibido += (char)data[i];
        }
        
        DynamicJsonDocument Deserializacion(1024);
        DeserializationError err = deserializeJson(Deserializacion, JSON_Recibido);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"JSON inválido\"}");
            return;
        }

        request->send(200, "application/json", "{\"message\":\"Configuración recibida, procesando...\"}");
        
        Configuration_Status = false;
        Connection_Success = false;
        SSID_WiFi = Deserializacion["SSID_WiFi"].as<String>();
        WiFi_Password = Deserializacion["Pass_WiFi"].as<String>();
        User_Name = DeleteBlankSpaces(Deserializacion["User_Name"].as<String>());
        Device = DeleteBlankSpaces(Deserializacion["Device"].as<String>());
        Turn_On_WiFi_Led = true;
        WiFiConfiguration(SSID_WiFi.c_str(), WiFi_Password.c_str());

        if (Connection_Success)
            Connection_Success = SaveConfiguration(User_Name, SSID_WiFi, WiFi_Password, Device);
    });

    Web_Server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
        req->send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html lang="es">
    <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Configurar Dispositivo</title>
    <style>
    body {
        font-family:Arial,sans-serif;
        background:#f0f2f5;
        display:flex;align-items:center;justify-content:center;
        min-height:100vh;margin:0;padding:20px;box-sizing:border-box;
    }
    .card {
        background:#fff;border-radius:12px;
        box-shadow:0 4px 12px rgba(0,0,0,0.1);
        padding:2rem;width:100%;max-width:400px;text-align:center;
    }
    .input-group {
        margin-bottom:1rem;
        text-align:left;
        position:relative;
    }
    label {
        display:block;
        font-size:0.9rem;
        margin-bottom:0.3rem;
        color:#555;
    }
    input, select {
        width:100%;padding:0.6rem;
        border:1px solid #ccc;border-radius:6px;
        font-size:1rem;box-sizing:border-box;
        background-color:#fff;
    }
    #Pass_WiFi {
        padding-right:2.5rem;
    }
    #SSID_WiFi {
        padding-right:2.5rem;
        appearance:none;
        -webkit-appearance:none;
        -moz-appearance:none;
        background-image: url("data:image/svg+xml;charset=UTF-8,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23666' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3e%3cpolyline points='6,9 12,15 18,9'%3e%3c/polyline%3e%3c/svg%3e");
        background-repeat:no-repeat;
        background-position:right 0.75rem center;
        background-size:16px;
    }
    .toggle-pass {
        position:absolute;
        top:calc(50% + 0.65rem);
        right:0.75rem;
        width:1.2em;height:1.2em;
        transform:translateY(-50%);
        display:flex;align-items:center;justify-content:center;
        cursor:pointer;user-select:none;
        z-index:2;
    }
    .toggle-pass svg {
        width:16px;height:16px;
        fill:none;
        stroke:#666;
        stroke-width:2;
        stroke-linecap:round;
        stroke-linejoin:round;
    }
    small.warning {
        display:none;color:#dc3545;font-size:0.8rem;
    }
    button {
        width:100%;padding:0.8rem;
        background:#007BFF;color:#fff;
        font-size:1rem;border:none;border-radius:6px;
        cursor:pointer;transition:background 0.3s ease;
    }
    button:disabled {
        background:#aac6f5;cursor:not-allowed;
    }
    button:hover:not(:disabled) {
        background:#0056b3;
    }
    button.success {
        background:#28a745;
    }
    button.success:hover {
        background:#218838;
    }
    button.danger {
        background:#dc3545;
    }
    button.danger:hover {
        background:#c82333;
    }
    .msg {
        margin-top:1rem;font-size:0.9rem;
        padding:0.5rem;border-radius:6px;
        display:none;
        white-space: pre-line;
    }
    .msg.success {
        background:#d4edda;color:#155724;border:1px solid #c3e6cb;
        display:block;
    }
    .msg.error {
        background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;
        display:block;
    }
    .msg.info {
        background:#d1ecf1;color:#0c5460;border:1px solid #bee5eb;
        display:block;
    }
    .popup {
        position:fixed;
        top:0;left:0;width:100%;height:100%;
        background:rgba(0,0,0,0.5);
        display:flex;align-items:center;justify-content:center;
        z-index:1000;
    }
    .popup-content {
        background:#fff;
        padding:2rem;
        border-radius:12px;
        box-shadow:0 8px 24px rgba(0,0,0,0.2);
        max-width:400px;
        text-align:center;
        margin:20px;
    }
    .popup-content h3 {
        margin-top:0;
        color:#dc3545;
    }
    .popup-content button {
        margin-top:1rem;
        padding:0.5rem 1rem;
        background:#dc3545;
        color:#fff;
        border:none;
        border-radius:6px;
        cursor:pointer;
    }
    .popup-content button:hover {
        background:#c82333;
    }
    </style>
    </head>
    <body>
    <div class="card">
        <h1>Configurar dispositivo</h1>
        <form id="cfg">
        <div class="input-group">
            <label for="User_Name">Nombre de Usuario</label>
            <input id="User_Name" name="User_Name" type="text" required placeholder="Introducir nombre sin espacios">
            <small id="Name_Warning" class="warning">Máximo 30 caracteres</small>
        </div>
        <div class="input-group">
            <label for="Device_Name">Nombre del dispositivo</label>
            <input id="Device_Name" name="Device_Name" type="text" required maxlength="30" placeholder="Lugar donde instalará el dispositivo">
            <small id="Name_Warning" class="warning">Máximo 30 caracteres</small>
        </div>
        <div class="input-group">
            <label for="SSID_WiFi">SSID WiFi</label>
            <select id="SSID_WiFi" name="SSID_WiFi" required>
            <option value="">— Elige una red —</option>
            </select>
        </div>
        <div class="input-group">
            <label for="Pass_WiFi">Contraseña WiFi</label>
            <input id="Pass_WiFi" name="Pass_WiFi" type="password" required disabled>
            <span id="togglePass" class="toggle-pass">
            <svg id="iconEye" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" style="display:block;">
                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
                <circle cx="12" cy="12" r="3"/>
            </svg>
            <svg id="iconEyeSlash" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" style="display:none;">
                <path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1 2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/>
                <line x1="1" y1="1" x2="23" y2="23"/>
            </svg>
            </span>
        </div>
        <button type="submit" id="submitBtn" disabled>Enviar configuración</button>
        <button type="button" id="restartBtn" class="success" style="display:none;">Reiniciar Dispositivo</button>
        <button type="button" id="retryBtn" class="danger" style="display:none;">Reintentar</button>
        
        <div class="msg" id="statusMsg"></div>
        </form>

        <!-- Popup para errores de WiFi -->
        <div class="popup" id="errorPopup" style="display:none;">
            <div class="popup-content">
                <h3>⚠️ Error de Conexión WiFi ⚠️</h3>
                <p>No se pudo conectar al WiFi.</p>
                <p>Verifique la contraseña.</p>
                <button onclick="ClosePopUp()">Entendido</button>
            </div>
        </div>

        <!-- Popup para configuración exitosa -->
        <div class="popup" id="successPopup" style="display:none;">
            <div class="popup-content">
                <h3 style="color:#28a745;">✅ ¡Dispositivo reiniciado! ✅</h3>
                <p><strong>Ya puede cerrar esta pestaña.</strong></p>
            </div>
        </div>
    </div>

    <script>
        const form = document.getElementById('cfg'),
            nameInput = document.getElementById('User_Name'),
            deviceInput = document.getElementById('Device_Name'),
            nameWarn = document.getElementById('Name_Warning'),
            selSSID = document.getElementById('SSID_WiFi'),
            passInput = document.getElementById('Pass_WiFi'),
            toggle = document.getElementById('togglePass'),
            eye = document.getElementById('iconEye'),
            eyeSlash = document.getElementById('iconEyeSlash'),
            submitBtn = document.getElementById('submitBtn'),
            restartBtn = document.getElementById('restartBtn'),
            retryBtn = document.getElementById('retryBtn'),
            msg = document.getElementById('statusMsg');
            errorPopup = document.getElementById('errorPopup'),
            successPopup = document.getElementById('successPopup');

        let Checking_Status = false;

        async function ScanAvailableNetworks() {
            try {
                const res = await fetch('/scan');
                const ssids = await res.json();
                selSSID.innerHTML = '<option value="">— Elige una red —</option>';
                ssids.forEach(s => {
                    const o = document.createElement('option');
                    o.value = s; o.textContent = s;
                    selSSID.append(o);
                });
            } catch(e) { console.error(e); }
        }
        
        // Función para verificar estado
        async function StatusVerification() {
            try {
                const res = await fetch('/status');
                const data = await res.json();
                return data;
            } catch {
                return null;
            }
        }

        // Función para mostrar mensaje
        function ShowMessage(texto, tipo) {
            if (texto === "" || !texto) {
                // Si no hay texto, ocultar completamente el mensaje
                msg.className = 'msg';
                msg.textContent = '';
            } else {
                msg.className = 'msg ' + tipo;
                msg.textContent = texto;
            }
        }

        // Función para mostrar popup de error
        function ShowErrorPopup() {
            errorPopup.style.display = 'flex';
        }

        // Función para cerrar popup de error
        function ClosePopUp() {
            errorPopup.style.display = 'none';
        }

        // Función para mostrar popup de éxito
        function ShowSuccessPopup() {
            successPopup.style.display = 'flex';
        }

        // Función para cerrar popup de éxito
        function CloseSuccessPopUp() {
            successPopup.style.display = 'none';
        }

        // Event listeners básicos
        nameInput.addEventListener('input', ()=>{
            nameWarn.style.display = nameInput.value.length > 30 ? 'block' : 'none';
            Validate();
        });

        deviceInput.addEventListener('input', ()=>{
            nameWarn.style.display = deviceInput.value.length > 30 ? 'block' : 'none';
            Validate();
        });

        selSSID.addEventListener('change', ()=>{
            passInput.disabled = (selSSID.value === '');
            Validate();
        });

        toggle.addEventListener('click', ()=>{
            if (passInput.type === 'password') {
                passInput.type = 'text';
                eye.style.display = 'none';
                eyeSlash.style.display = 'block';
            } else {
                passInput.type = 'password';
                eye.style.display = 'block';
                eyeSlash.style.display = 'none';
            }
        });

        function Validate() {
            submitBtn.disabled = !(
                nameInput.value &&
                selSSID.value &&
                passInput.value &&
                !passInput.disabled &&
                deviceInput.value
            );
        }

        // Función para reiniciar
        restartBtn.addEventListener('click', async () => {
            ShowMessage('', 'info');
            restartBtn.disabled = true;
            
            try {
                await fetch('/restart', { method: 'POST' });
                restartBtn.style.display = 'none';
                ShowSuccessPopup();
            } catch(e) {
                restartBtn.style.display = 'none';
                ShowSuccessPopup();
            }
        });

        // Función para reintentar configuración
        retryBtn.addEventListener('click', () => {
            // Resetear estado de los botones
            submitBtn.style.display = 'block';
            submitBtn.disabled = false;
            submitBtn.textContent = 'Configurar dispositivo';
            retryBtn.style.display = 'none';
            restartBtn.style.display = 'none';
            
            // Limpiar Message
            ShowMessage("", null);
            
            // Enfocar el campo de contraseña para que el User pueda editarla
            passInput.focus();
            passInput.select();

            setTimeout(() => {
                form.dispatchEvent(new Event('submit', { cancelable: true }));
            }, 100); 
        });

        form.addEventListener('input', Validate);

        // Envío de formulario simplificado
        form.addEventListener('submit', async e => {
            e.preventDefault(); 
            
            submitBtn.disabled = true;
            submitBtn.textContent = 'Enviando...';
            restartBtn.style.display = 'none';
            retryBtn.style.display = 'none';
            
            const body = {
                User_Name: nameInput.value,
                SSID_WiFi: selSSID.value,
                Pass_WiFi: passInput.value,
                Device: deviceInput.value
            };

            try {
                // Enviar configuración
                const res = await fetch('/config', {
                    method:'POST',
                    headers:{'Content-Type':'application/json'},
                    body:JSON.stringify(body)
                });

                if (res.ok) {
                    ShowMessage('Verificando conexión WiFi \n\nAtento a la luz del dispositivo:\n• Luz roja: Contraseña WiFi incorrecta \n• Luz verde: WiFi configurado correctamente', 'info');
                    submitBtn.textContent = 'Procesando...';
                    Checking_Status = true;
                    
                    // Verificar estado en tiempo real
                    let Attempts = 0;
                    const Max_Attempts = 35; // 18 segundos máximo
                    
                    while (Checking_Status && Attempts < Max_Attempts) {
                        const Status = await StatusVerification();
                        
                        if (Status) {
                            ShowMessage("", 'info');
                            
                            if (Status.Complete_Configuration) {
                                Checking_Status = false;
                                
                                if (Status.WiFi_Connected) {
                                    ShowMessage('¡Configuración del dispositivo completada! \n\nReinicie el dispositivo para que se apliquen los cambios.', 'success');
                                    submitBtn.style.display = 'none';
                                    restartBtn.style.display = 'block';
                                } else {
                                    ShowErrorPopup();
                                    submitBtn.style.display = 'none';
                                    retryBtn.style.display = 'block';
                                }
                                break;
                            }
                        }
                        
                        Attempts++;
                        await new Promise(resolve => setTimeout(resolve, 500)); // Verificar cada 500ms
                    }
                    
                    if (Checking_Status) {
                        // Timeout
                        Checking_Status = false;
                        ShowMessage("", 'info');
                        submitBtn.style.display = 'none';
                        retryBtn.style.display = 'block';
                    }
                    
                } else {
                    throw new Error('Error del servidor');
                }
                
            } catch (error) {
                Checking_Status = false;
                ShowMessage('Error de comunicación. Inténtalo de nuevo.', 'error');
                submitBtn.textContent = 'Configurar dispositivo';
                submitBtn.disabled = false;
                retryBtn.style.display = 'block';
            }
        });

        // Cargar redes al inicio
        ScanAvailableNetworks();

        window.ClosePopUp = ClosePopUp;
        window.CloseSuccessPopUp = CloseSuccessPopUp;
    </script>
    </body>
    </html>
        )rawliteral");
    });

    Web_Server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* req){
        String json = "[";
        for (size_t i = 0; i < Available_SSIDs.size(); i++) {
            json += "\"" + Available_SSIDs[i] + "\"";
            if (i + 1 < Available_SSIDs.size()) json += ",";
        }
        json += "]";
        req->send(200, "application/json", json);
    });

    ScanAvailableNetworks();
    Web_Server.begin();
}

void CheckConnections(){
    unsigned long current_time = millis();
    
    if(current_time - Last_WiFi_Check >= WIFI_CHECK_INTERVAL){
        Last_WiFi_Check = current_time;
        
        if(WiFi.status() != WL_CONNECTED){
            WiFi_Reconnect_Attempts++;
            
            if(WiFi_Reconnect_Attempts <= MAX_WIFI_RECONNECT_ATTEMPTS){
                WiFi.reconnect();
                
                unsigned long reconnect_start = millis();
                while((WiFi.status() != WL_CONNECTED) && (millis() - reconnect_start < 15000)){
                    delay(500);
                    yield();
                }
                if(WiFi.status() == WL_CONNECTED)
                    WiFi_Reconnect_Attempts = 0;
            }
            else{
                Force_Channel_Recalculation = true;
                WiFi_Reconnect_Attempts = 0;
            }
        }
        else
            WiFi_Reconnect_Attempts = 0; // Reset if WiFi is connected
    }
    
    if((WiFi.status() == WL_CONNECTED) && (current_time - Last_MQTT_Check >= MQTT_CHECK_INTERVAL)){
        Last_MQTT_Check = current_time;
        
        if(!MQTT_Client.connected()){
            MQTT_Reconnect_Attempts++;
            
            if(MQTT_Reconnect_Attempts <= MAX_MQTT_RECONNECT_ATTEMPTS){
                MQTTConfiguration();
                
                if(MQTT_Client.connected())
                    MQTT_Reconnect_Attempts = 0;
            }
            else{
                delay(1000);
                ESP.restart();
            }
        }
        else
            MQTT_Reconnect_Attempts = 0; // Reset if MQTT is connected
    }
    
    if((Force_Channel_Recalculation) || (current_time - Last_Channel_Check >= CHANNEL_CHECK_INTERVAL)){
        Last_Channel_Check = current_time;
        
        if(Force_Channel_Recalculation)
            Force_Channel_Recalculation = false;
        
        CalculateWiFiChannel(SSID_WiFi.c_str());
    }
}

void setup(){
    Serial.begin(115200);
    Serial.flush();
    pinMode(PIN_RED_LED, OUTPUT);
    pinMode(PIN_GREEN_LED, OUTPUT);
    pinMode(PIN_BLUE_LED, OUTPUT);
    digitalWrite(PIN_BLUE_LED, LOW);
    digitalWrite(PIN_RED_LED, LOW);
    digitalWrite(PIN_GREEN_LED, LOW);
    pinMode(PIN_CONFIG, INPUT_PULLUP);
    EEPROM.begin(EEPROM_SIZE);
    randomSeed(analogRead(0)); // It's used to generate random numbers
    esp_task_wdt_init(120, false); // Initialize the watchdog timer with a timeout of 120 seconds

    String Initial_Configuration = EEPROM.readString(ADDR_STATUS);

    IfButtonPressed(false);

    if(strcmp(Initial_Configuration.c_str(), "CONFIGURED")){ // I check if it has never been configured
        if(!strcmp(Initial_Configuration.c_str(), "NO_CONFIGURED")){
            startConfigAP(); // Start the configuration access point
            while(1){
                IfButtonPressed(false);
                if(Turn_On_Configuration_Leds)
                    TurnOn2Leds(PIN_RED_LED, PIN_GREEN_LED);
                else
                    yield();
                
                delay(100);
            }
        }
        else if(strcmp(Initial_Configuration.c_str(), "NO_CONFIGURED")){
            EEPROM.writeString(ADDR_STATUS, "NO_CONFIGURED");
            EEPROM.writeInt(ADDR_FRECUENCY, 5);
            EEPROM.commit();
            ESP.restart();
        }
    }
    else if(!strcmp(Initial_Configuration.c_str(), "CONFIGURED")){
        WiFi.softAPdisconnect(true); // Deactivate the access point if it was active
        
        bool Status_EEPROM_Data = ValidateEEPROMData();
        if(!Status_EEPROM_Data){
            EEPROM.writeString(ADDR_STATUS, "NO_CONFIGURED");
            EEPROM.commit();
            ESP.restart();
        }
        
        WiFi.mode(WIFI_STA);
        delay(200);
        //esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

        if(IsWiFiChannelChanged() == false){ // If the WiFi channel has not changed, I set the channel from EEPROM
            ESP_ERROR_CHECK(esp_wifi_set_channel(WiFi_Channel,  WIFI_SECOND_CHAN_NONE));
            if(esp_now_init() != ESP_OK)
                esp_restart();
        }
        esp_wifi_set_ps(WIFI_PS_NONE);

        esp_now_register_recv_cb(MessageReceived);

        uint8_t Broadcast_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        AddFriend(Broadcast_MAC);

        WiFiConfiguration(SSID_WiFi.c_str(), WiFi_Password.c_str());

        Secure_Client.setInsecure();
        MQTT_Client.setServer(MQTT_Server, MQTT_Port);
        MQTT_Client.setCallback(CallBackMQTT);
        delay(500);
    }
}

void loop(){
    IfButtonPressed(true);

    CheckConnections();

    if(Is_Pairing){
        digitalWrite(PIN_BLUE_LED, HIGH);
        Elapsed_Pairing_Time = millis() - Initial_Pairing_Time;
        if(Elapsed_Pairing_Time >= (TIME_TO_PAIRING * 60)){ // If elapsed pairing time exceeds the limit    
            digitalWrite(PIN_BLUE_LED, LOW);
            Is_Pairing = false;
        }
    }

    if(New_Pairing_Event){
        BlinkLed(4, PIN_BLUE_LED, 0);
        digitalWrite(PIN_BLUE_LED, HIGH);
        New_Pairing_Event = false;
    }

    if(!MQTT_Client.connected())
        MQTTConfiguration();
    else
	    MQTT_Client.loop();

    ReadAndSendSensorData();

    delay(10);
}