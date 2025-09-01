#include <esp_wifi.h>
#include <esp_now.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>

#define EEPROM_SIZE 100
#define ADDR_COORDINATOR_MAC 0
#define ADDR_STATUS 24
#define ADDR_DEVICE 40
#define ADDR_WIFI_CHANNEL 80
#define ADDR_FRECUENCY 90

#define BLUE_LED_PIN 21
#define CONFIG_PIN 23
#define RX_PIN 16
#define TX_PIN 17

#define NUM_BLINKS 5
#define AUTOPAIRING_HALTEN_TIME 3000
#define CHANNEL_SCAN_TIMEOUT 3000
#define NO_CONFIGURATION_HALTEN 8000

uint8_t Broadcast_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t Coordinator_MAC[6];
uint8_t WiFi_Channel = 1;
uint8_t Current_Scan_Channel = 1;
String Device = "";
bool Is_Configured;
unsigned long Last_Sensor_Reading = 0;
unsigned long Last_Channel_Scan;
int Sensor_Reading_Interval = 5; // In seconds
AsyncWebServer Web_Server(80);
HardwareSerial Serial_Port(2);
PZEM004Tv30 Pzem(Serial_Port, RX_PIN, TX_PIN);

enum Message_Type : uint8_t { PAIRING = 0, DATA = 1, CHANNEL_CHANGE = 2, FRECUENCY = 3 };
struct Pairing_Struct { uint8_t Message_Type; uint8_t ID;  uint8_t WiFi_Channel; char Device[40]; };
struct Message_Struct{ uint8_t Message_Type; float Voltage; float Current; float Power; char Device[40]; };

void ChannelChange();

void BlinkLed(int Num_Times, int Led_Pin){
    for(int i = 0; i < Num_Times; i++){
        digitalWrite(Led_Pin, HIGH);
        delay(250);
        digitalWrite(Led_Pin, LOW);
        delay(250);
    }
}

void AddFriend(const uint8_t * MAC, bool Save_MAC){ // Function to add a new friend, to communicate with him
    if(Save_MAC){
        for(int j = 0; j < 6; j++){
            EEPROM.write(j + ADDR_COORDINATOR_MAC, MAC[j]);
            EEPROM.commit();
        }
    }

    esp_now_peer_info_t Friend;
    memset(&Friend, 0, sizeof(Friend));
    memcpy(Friend.peer_addr, MAC, 6);
    Friend.channel = WiFi_Channel;
    Friend.encrypt = false;
    esp_now_add_peer(&Friend);
}

void MessageReceived(const uint8_t *MAC, const uint8_t *Data_On_the_Way, int Length){
    Pairing_Struct Pairing_Data;
    memset(&Pairing_Data, 0, sizeof(Pairing_Data));
    memcpy(&Pairing_Data, Data_On_the_Way, sizeof(Pairing_Data));
    switch(Pairing_Data.Message_Type){
        case PAIRING:
            if(Pairing_Data.ID == 0){ // If the id isn't 0, it doesn't come from the coordinator
                Is_Configured = true;
                BlinkLed(NUM_BLINKS, BLUE_LED_PIN);
                WiFi_Channel = Current_Scan_Channel;
                Current_Scan_Channel = 14; // I don't have to search for more channels anymore, I make the loop end
                memcpy(Coordinator_MAC, MAC, 6);
            }
            break;
        case CHANNEL_CHANGE:
            if((Pairing_Data.ID == 0) && (Pairing_Data.WiFi_Channel != WiFi_Channel)){
                WiFi_Channel = Pairing_Data.WiFi_Channel;
                ChannelChange();
            }
            break;
        case FRECUENCY:
            if(Pairing_Data.ID == 0){
                if (String(Pairing_Data.Device) == Device) {
                    Sensor_Reading_Interval = Pairing_Data.WiFi_Channel; // Using WiFi_Channel to store the reading interval
                    EEPROM.writeInt(ADDR_FRECUENCY, Sensor_Reading_Interval);
                    EEPROM.commit();
                }
            }
            break;
    }
}

void ChannelChange(){
    esp_now_deinit();
    delay(500);
    ESP_ERROR_CHECK(esp_wifi_set_channel(WiFi_Channel,  WIFI_SECOND_CHAN_NONE));
    if(esp_now_init() != ESP_OK)
        ESP.restart();
    esp_now_register_recv_cb(MessageReceived);
    AddFriend(Coordinator_MAC, false);
    EEPROM.write(ADDR_WIFI_CHANNEL, WiFi_Channel);
    EEPROM.commit();
}

void ReadEEPROM(){
    for(int i = 0; i < 6; i++)
        Coordinator_MAC[i] = EEPROM.read(i + ADDR_COORDINATOR_MAC );
    AddFriend(Coordinator_MAC, false); // I add the coordinator as a friend so that I can communicate with him

    Device = EEPROM.readString(ADDR_DEVICE);
    WiFi_Channel = EEPROM.read(ADDR_WIFI_CHANNEL);
    Sensor_Reading_Interval = EEPROM.readInt(ADDR_FRECUENCY);

    if((WiFi_Channel < 1) || (WiFi_Channel > 13)) // Validate WiFi channel
        WiFi_Channel = 1; // Default channel
}

bool CompareMACs(){ // Function to compare the Broadcast MAC with the Coordinator MAC
    return (memcmp(Broadcast_MAC, Coordinator_MAC, 6) == 0);
}

void ScanChannelsForPairing(){
    unsigned long Current_Time = 0;
    Last_Channel_Scan = 0;
    Is_Configured = false;

    Pairing_Struct Pairing_Data;
    Pairing_Data.Message_Type = PAIRING;
    Pairing_Data.ID = 1;
    esp_now_del_peer(Broadcast_MAC);

    while((Current_Scan_Channel <= 13)){
        Current_Time = millis();
        if(Current_Time - Last_Channel_Scan >= CHANNEL_SCAN_TIMEOUT){            
            esp_now_deinit(); // I de-initialise ESP-NOW to change channel
            delay(500);
            esp_err_t channel_result = esp_wifi_set_channel(Current_Scan_Channel, WIFI_SECOND_CHAN_NONE);
            if(channel_result != ESP_OK){
                Current_Scan_Channel++;
                Last_Channel_Scan = Current_Time;
                continue;
            }

            if(esp_now_init() != ESP_OK){
                Current_Scan_Channel++;
                Last_Channel_Scan = Current_Time;
                continue;
            }

            esp_now_register_recv_cb(MessageReceived);
            
            WiFi_Channel = Current_Scan_Channel; // I update the wifi channel to try to communicate from it
            AddFriend(Broadcast_MAC, false);

            esp_now_send(Broadcast_MAC, (uint8_t *) &Pairing_Data, sizeof(Pairing_Data));
            delay(200);

            Current_Scan_Channel++;
            Last_Channel_Scan = Current_Time;
        }
        delay(300); // Wait a while before switching to the next channel
    }
    if(Is_Configured == false){ // If I try with all channels and it still does not configure, I put it in factory mode.
        AddFriend(Broadcast_MAC, true);
        EEPROM.writeString(ADDR_STATUS, "NO_CONFIGURED");
        EEPROM.writeInt(ADDR_FRECUENCY, 5);
        EEPROM.commit();
        ESP.restart();
    }
    else{
        Serial.println("Device configured successfully!");
        EEPROM.write(ADDR_WIFI_CHANNEL, WiFi_Channel); // I save the channel in the EEPROM
        esp_now_del_peer(Coordinator_MAC);
        AddFriend(Coordinator_MAC, true); // I add the address of the device that sent me the message, saving the MAC in the EEPROM
        EEPROM.commit();
        ESP.restart(); // I restart the device to apply the changes
    }
}

void IfButtonPressed(bool Is_Configuring){ // Function to check if the button is pressed
    if((digitalRead(CONFIG_PIN) == LOW) && (Is_Configuring == true)){ // If I'm pressing the button, I start the counter
        unsigned long Elapsed_Pairing_Time = 0;
        while(digitalRead(CONFIG_PIN) == LOW){
            Elapsed_Pairing_Time += 100;
            if(Elapsed_Pairing_Time == AUTOPAIRING_HALTEN_TIME)
                BlinkLed(1, BLUE_LED_PIN); // Blink the LED once
            else if(Elapsed_Pairing_Time == NO_CONFIGURATION_HALTEN)
                BlinkLed(1, BLUE_LED_PIN); // Blink the LED once
            delay(100);
        }
        if(Elapsed_Pairing_Time >= AUTOPAIRING_HALTEN_TIME){ // If I have pressed the button for more than 3 seconds, I start the pairing process
            ScanChannelsForPairing(); // I scan the channels to find the coordinator
            esp_now_deinit(); // If I found the coordinator, I de-initialise ESP-NOW to change the channel and add the coordinator as a friend
            delay(500);
            esp_err_t channel_result = esp_wifi_set_channel(WiFi_Channel, WIFI_SECOND_CHAN_NONE);
            if(esp_now_init() != ESP_OK)
                ESP.restart();
            esp_now_register_recv_cb(MessageReceived);
            AddFriend(Coordinator_MAC, false);
            Current_Scan_Channel = 1;
            Last_Channel_Scan = 0;
        }
        else if(Elapsed_Pairing_Time >= NO_CONFIGURATION_HALTEN){ // If I have pressed the button for more than 8 seconds, I reset the device to factory settings
            EEPROM.writeString(ADDR_STATUS, "CONFIGURED");
            EEPROM.commit();
            ESP.restart();
        }
    }
}

void ReadAndSendSensorData(){
    unsigned long Current_Time = millis();
    
    if(Current_Time - Last_Sensor_Reading >= Sensor_Reading_Interval * 1000){ // Check if it's time to read the sensor data
        Last_Sensor_Reading = Current_Time;

        float Voltage = Pzem.voltage();
        float Current = Pzem.current();
        float Power = Pzem.power();

        if(!isnan(Voltage)){
            Message_Struct Data;
            Data.Message_Type = DATA;
            Data.Voltage = Voltage;
            Data.Current = Current;
            Data.Power = Power;
            strncpy(Data.Device, Device.c_str(), sizeof(Data.Device) - 1);
            Data.Device[sizeof(Data.Device) - 1] = '\0';
            Serial.print("\n");
            esp_err_t result = esp_now_send(Coordinator_MAC, (uint8_t *) &Data, sizeof(Data));  // Corregido: enviar al coordinador
            if(result == ESP_OK) 
                Serial.println("Datos enviados exitosamente\n");
            else
                Serial.println("Error enviando datos: " + String(result));
            delay(200);
        }
    }
}

String DeleteBlankSpaces(String Input){ // Remove blanks
    for(int i = 0; i < Input.length(); i++){
        if(Input.charAt(i) == ' '){
            Input.remove(i, 1);
            i--;
        }
    }
    return Input;
}

void startConfigAP(){ 
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("TFG KPROJECT", "Kproject2024+");
    delay(200);
    IPAddress IP(192,168,68,1), Gateway(192,168,68,1), Netmask(255,255,255,0);
    WiFi.softAPConfig(IP, Gateway, Netmask);

    Web_Server.on("/config", HTTP_POST, [](AsyncWebServerRequest* request){}, NULL, 
    [](AsyncWebServerRequest* request, uint8_t *Data, size_t Len, size_t Index, size_t Total){
        String JSON_Received = "";
        for(size_t i = 0; i < Len; i++){
            JSON_Received += (char)Data[i];
        }
        
        DynamicJsonDocument Deserialization(512);
        DeserializationError err = deserializeJson(Deserialization, JSON_Received);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        request->send(200, "application/json", "{\"message\":\"Saved configuration. Restarting...\"}");
        Device = DeleteBlankSpaces(Deserialization["Device_Name"].as<String>());
        
        EEPROM.writeString(ADDR_DEVICE, Device); // I save the new data in the EEPROM
        EEPROM.writeString(ADDR_STATUS, "CONFIGURED");
        EEPROM.write(ADDR_WIFI_CHANNEL, WiFi_Channel);
        EEPROM.commit();

        delay(1000);
        ESP.restart();
    });

    Web_Server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
        req->send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html lang="es">
    <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Configurar nuevo dispositivo</title>
    <style>
    body {
        font-family: Arial, sans-serif;
        background: #f0f2f5;
        display: flex;
        align-items: center;
        justify-content: center;
        min-height: 100vh;
        margin: 0;
        padding: 20px;
        box-sizing: border-box;
    }
    .card {
        background: #fff;
        border-radius: 12px;
        box-shadow: 0 4px 12px rgba(0,0,0,0.1);
        padding: 2rem;
        width: 100%;
        max-width: 400px;
        text-align: center;
    }
    .input-group {
        margin-bottom: 1.5rem;
        text-align: left;
    }
    label {
        display: block;
        font-size: 0.9rem;
        margin-bottom: 0.5rem;
        color: #555;
        font-weight: bold;
    }
    input {
        width: 100%;
        padding: 0.8rem;
        border: 1px solid #ccc;
        border-radius: 6px;
        font-size: 1rem;
        box-sizing: border-box;
        background-color: #fff;
    }
    input:focus {
        outline: none;
        border-color: #007BFF;
        box-shadow: 0 0 0 2px rgba(0,123,255,0.25);
    }
    button {
        width: 100%;
        padding: 1rem;
        background: #007BFF;
        color: #fff;
        font-size: 1rem;
        border: none;
        border-radius: 6px;
        cursor: pointer;
        transition: background 0.3s ease;
        font-weight: bold;
    }
    button:disabled {
        background: #aac6f5;
        cursor: not-allowed;
    }
    button:hover:not(:disabled) {
        background: #0056b3;
    }
    .popup {
        position: fixed;
        top: 0;
        left: 0;
        width: 100%;
        height: 100%;
        background: rgba(0,0,0,0.5);
        display: none;
        align-items: center;
        justify-content: center;
        z-index: 1000;
    }
    .popup-content {
        background: #fff;
        padding: 2rem;
        border-radius: 12px;
        box-shadow: 0 8px 24px rgba(0,0,0,0.2);
        max-width: 350px;
        text-align: center;
        margin: 20px;
    }
    .popup-content h3 {
        margin-top: 0;
        color: #28a745;
        font-size: 1.3rem;
    }
    .popup-content p {
        color: #555;
        line-height: 1.5;
    }
    .spinner {
        border: 3px solid #f3f3f3;
        border-top: 3px solid #28a745;
        border-radius: 50%;
        width: 30px;
        height: 30px;
        animation: spin 1s linear infinite;
        margin: 1rem auto;
    }
    @keyframes spin {
        0% { transform: rotate(0deg); }
        100% { transform: rotate(360deg); }
    }
    </style>
    </head>
    <body>
    <div class="card">
        <h1>Configurar dispositivo</h1>
        <form id="configForm">
            <div class="input-group">
                <label for="Device_Name">Nombre del dispositivo</label>
                <input id="Device_Name" name="Device_Name" type="text" required maxlength="30" placeholder="Lugar donde instalará el dispositivo">
            </div>
            <button type="submit" id="submitBtn" disabled>Enviar configuración</button>
        </form>
    </div>

    <div class="popup" id="successPopup">
        <div class="popup-content">
            <h3>✅ ¡Dispositivo configurado!</h3>
            <p><strong>El dispositivo se está reiniciando</strong></p>
            <p>Ya puede cerrar esta ventana.</p>
        </div>
    </div>

    <script>
        const form = document.getElementById('configForm');
        const deviceInput = document.getElementById('Device_Name');
        const submitBtn = document.getElementById('submitBtn');
        const successPopup = document.getElementById('successPopup');

        deviceInput.addEventListener('input', function() {
            const isValid = this.value.trim().length > 0 && this.value.length <= 30;
            submitBtn.disabled = !isValid;
        });

        form.addEventListener('submit', async function(e) {
            e.preventDefault();
            
            const deviceName = deviceInput.value.trim();
            
            if (!deviceName) {
                alert('Por favor, ingresa un nombre para el dispositivo');
                return;
            }

            submitBtn.disabled = true;
            submitBtn.textContent = 'Configurando...';

            try {
                const response = await fetch('/config', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        Device_Name: deviceName
                    })
                });

                if (response.ok) {
                    successPopup.style.display = 'flex';
                } else {
                    throw new Error('Error del servidor');
                }

            } catch (error) {
                alert('Error al configurar el dispositivo. Inténtalo de nuevo.');
                submitBtn.disabled = false;
                submitBtn.textContent = 'Configurar Dispositivo';
            }
        });

        deviceInput.focus();
    </script>
    </body>
    </html>
        )rawliteral");
    });

    Web_Server.begin();
}

void setup(){
    Serial.begin(115200);
    pinMode(BLUE_LED_PIN, OUTPUT);
    digitalWrite(BLUE_LED_PIN, LOW); // Turn off the blue LED at startup
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Set maximum transmission power
    WiFi.disconnect(); // Disconnect from any previous WiFi network
    EEPROM.begin(EEPROM_SIZE);
    pinMode(CONFIG_PIN, INPUT_PULLUP);
    randomSeed(esp_random()); // Seed to improve randomness

    String Initial_Configuration = EEPROM.readString(ADDR_STATUS); // Read the initial configuration status from EEPROM
    if(strcmp(Initial_Configuration.c_str(), "CONFIGURED")){ // If the device is not configured
        if(!strcmp(Initial_Configuration.c_str(), "NO_CONFIGURED")){
            startConfigAP(); // Start the configuration access point
            digitalWrite(BLUE_LED_PIN, HIGH);
            while(1){
                IfButtonPressed(false);
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
        ReadEEPROM();
     
        ESP_ERROR_CHECK(esp_wifi_set_channel(WiFi_Channel,  WIFI_SECOND_CHAN_NONE));
        if(esp_now_init() != ESP_OK)
            ESP.restart();

        esp_now_register_recv_cb(MessageReceived);
        AddFriend(Coordinator_MAC, false);
        
        IfButtonPressed(true); // Check if the button is pressed to start pairing

        if(CompareMACs() == true){ // If the Broadcast MAC is equal to the Coordinator MAC, it means that the device is not configured
            delay(1000);
            ESP.restart();
        }
    }
}

void loop(){
    IfButtonPressed(true);

    ReadAndSendSensorData();

    delay(100);
}