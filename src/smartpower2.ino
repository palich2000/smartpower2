#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <Hash.h>
#include <FS.h>
#include <Wire.h>
#include <SimpleTimer.h>
#include <mcp4652.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266httpUpdate.h>

#define CONSOLE_PORT 23

WiFiServer logServer(CONSOLE_PORT);
WiFiClient logClient;

#define USE_SERIAL Serial

#define MAX_LCD_SSID_LENGTH	12
#define MAX_LCD_IP_LENGTH	14

#define WiFi_connect_timeout	20

#define ON	0
#define OFF	1
#define HOME    0
#define SETTINGS    1

#define POWER		D6
#define POWERLED	D1
#define BTN_ONOFF	D7
#define I2C_SDA		D2
#define I2C_SCL		D5

#define SET_DEFAULT_VOLTAGE	'v'
#define SET_VOLTAGE		'w'
#define SAVE_NETWORKS		'n'
#define CMD_ONOFF		'o'
#define SET_AUTORUN		'a'
#define PAGE_STATE		'p'
#define DATA_PVI		'd'
#define MEASUREWATTHOUR		'm'
#define FW_VERSION		'f'

#define FWversion	1.21

uint8_t onoff = OFF;
unsigned char measureWh;
float setVoltage;
unsigned char connectedWeb;
unsigned char connectedLCD;
unsigned char timerId;
unsigned char timerId2;
unsigned char autorun;

unsigned char D4state;
unsigned char D1state;


float volt;
float ampere;
float watt;
double watth;

#define MAX_SRV_CLIENTS 1

ESP8266WebServer webServer;
WebSocketsServer webSocket = WebSocketsServer(81);

IPAddress config_ip = IPAddress(192, 168, 4, 1);
IPAddress current_ip;
char config_ssid[20];
char current_ssid[20];
char password[20];


SimpleTimer timer;
SimpleTimer timer2;
// lcd slave address are 0x27 or 0x3f
LiquidCrystal_I2C lcd(0x27, 16, 2);

struct client_sp2 {
    uint8_t connected;
    uint8_t page;
};

struct client_sp2 client_sp2[5];

void setup() {
    USE_SERIAL.begin(115200);
    USE_SERIAL.setDebugOutput(true);
    pinMode(POWERLED, OUTPUT);
    USE_SERIAL.println(__FUNCTION__);

    Wire.begin(I2C_SDA, I2C_SCL);
    mcp4652_init();
    ina231_configure();

    pinMode(BTN_ONOFF, INPUT);
    attachInterrupt(digitalPinToInterrupt(BTN_ONOFF), pinChanged, CHANGE);

    for(uint8_t t = 4; t > 0; t--) {
        USE_SERIAL.printf("[SETUP] BOOT WAIT %d...\n\r", t);
        USE_SERIAL.flush();
        delay(1000);
    }

    USE_SERIAL.println("===========================");
    SPIFFS.begin();
    {
        Dir dir = SPIFFS.openDir("/");
        while (dir.next()) {
            String fileName = dir.fileName();
            size_t fileSize = dir.fileSize();
            USE_SERIAL.printf("FS File: %s, size: %s\n\r",
                              fileName.c_str(), formatBytes(fileSize).c_str());
        }
    }
    FSInfo info;
    SPIFFS.info(info);
    USE_SERIAL.printf("SPIFS: size: %d used: %d\n\r", info.totalBytes, info.usedBytes);

    initSmartPower();

    lcd_status();

    network_init();
    webserver_init();

    timerId = timer.setInterval(1000, handler);

    // Log
    logServer.begin();
    logServer.setNoDelay(true);
}

void loop() {
    webServer.handleClient();
    webSocket.loop();
    timer.run();


    if (logServer.hasClient()) {
        // A connection attempt is being made
        USE_SERIAL.printf("A connection attempt is being made.\n");

        if (!logClient) {
            // This is the first client to connect
            logClient = logServer.available();

            USE_SERIAL.printf("This is the first client to connect.\n");
        } else {
            if (!logClient.connected()) {
                // A previous client has disconnected.
                //  Connect the new client.
                logClient.stop();
                logClient = logServer.available();

                USE_SERIAL.printf("A previous client has disconnected.\n");
            } else {
                // A client connection is already in use.
                // Drop the new connection attempt.
                WiFiClient tempClient = logServer.available();
                tempClient.stop();

                USE_SERIAL.printf("A client connection is already in use.\n");
            }
        }
    }


}

void network_init() {
    USE_SERIAL.println(__FUNCTION__);
    int ap_found = 0;
    strcpy(current_ssid, config_ssid);


    WiFi.mode(WIFI_STA);
    USE_SERIAL.println("Scanning wifi....");
    int n = WiFi.scanNetworks();
    USE_SERIAL.println("scan done");

    if (n == 0) {
        USE_SERIAL.println("no networks found");
    } else {
        USE_SERIAL.print(n);
        USE_SERIAL.println(" networks found");
        for (int i = 0; i < n; ++i) {
            USE_SERIAL.print(i + 1);
            USE_SERIAL.print(": ");
            USE_SERIAL.print(WiFi.SSID(i));
            USE_SERIAL.print(" (");
            USE_SERIAL.print(WiFi.RSSI(i));
            USE_SERIAL.print(")");
            USE_SERIAL.print((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*");
            if (String(current_ssid) == WiFi.SSID(i)) {
                USE_SERIAL.print(" FOUND ");
                ap_found = 1;
            }
            USE_SERIAL.println();
        }
    }
    WiFi.scanDelete();
    WiFi.disconnect(true);

    USE_SERIAL.print("AP:");
    USE_SERIAL.print(current_ssid);
    USE_SERIAL.print(" PASSWORD:");
    USE_SERIAL.println(password);

    delay(100);

    if (ap_found) {
        USE_SERIAL.println("Try start wifi client mode");
        WiFi.mode(WIFI_STA);
        WiFi.begin(current_ssid, password);
        int c = 0;
        while ((WiFi.status() != WL_CONNECTED) && (c < WiFi_connect_timeout)) {
            delay(1000);
            Serial.print("|");
            c++;
        }
        if (c < WiFi_connect_timeout) {
            Serial.println("");
            Serial.println("WiFi connected");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());
            current_ip = WiFi.localIP();
        } else {
            Serial.println("Unable to connect.");
            Serial.println("Disconnecting.");
            WiFi.disconnect(true);
        }
    }

    if ((!ap_found) || ((ap_found) && (WiFi.status() != WL_CONNECTED))) {
        if (ap_found) {
            strncat(current_ssid, String(ESP.getChipId(), HEX).c_str(), (sizeof(current_ssid) - strlen(current_ssid)) - 1);
            current_ssid[sizeof(current_ssid) - 1] = 0;
        }
        current_ip = config_ip;

        IPAddress gateway(current_ip[0], current_ip[1], current_ip[2], current_ip[3]);
        IPAddress subnet(255, 255, 255, 0);

        USE_SERIAL.printf("Starting WiFi AP mode: %s\n", current_ssid);
        USE_SERIAL.printf("    %s %s %s\n", current_ip.toString().c_str(), gateway.toString().c_str(), subnet.toString().c_str());

        WiFi.mode(WIFI_AP);

        WiFi.softAPConfig(current_ip, gateway, subnet);
        WiFi.softAP(current_ssid, password);

        USE_SERIAL.printf("WiFi interface IP address: %s\n", WiFi.softAPIP().toString().c_str());
    }
    USE_SERIAL.printf("WiFi mode: %s\n", String(WiFi.getMode()).c_str());
}

void webserver_init(void) {
    USE_SERIAL.println(__FUNCTION__);

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    webServer =  ESP8266WebServer(current_ip, 80);
    webServer.on("/list", HTTP_GET, handleFileList);
    webServer.on("/format", HTTP_GET, handleFormat);
    webServer.on("/upload", HTTP_POST, []() {
        webServer.send(200);
    }, handleFileUpload);

    webServer.onNotFound([]() {
        if(!handleFileRead(webServer.uri()))
            webServer.send(404, "text/plain", "FileNotFound");
    });
    webServer.begin();

    USE_SERIAL.println("HTTP webServer started");
}

//format bytes
String formatBytes(size_t bytes) {
    if (bytes < 1024) {
        return String(bytes) + "B";
    } else if(bytes < (1024 * 1024)) {
        return String(bytes / 1024.0) + "KB";
    } else if(bytes < (1024 * 1024 * 1024)) {
        return String(bytes / 1024.0 / 1024.0) + "MB";
    } else {
        return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
    }
}

String getContentType(String filename) {
    if(webServer.hasArg("download")) return "application/octet-stream";
    else if(filename.endsWith(".htm")) return "text/html";
    else if(filename.endsWith(".html")) return "text/html";
    else if(filename.endsWith(".js")) return "application/javascript";
    else if(filename.endsWith(".css")) return "text/css";
    else if(filename.endsWith(".gif")) return "image/gif";
    else if(filename.endsWith(".ico")) return "image/x-icon";
    return "text/plain";
}

bool handleFileRead(String path) {
    if (path.endsWith("/")) {
        path += "index.html";
    }
    USE_SERIAL.println("handleFileRead: " + path);
    String contentType = getContentType(path);
    String pathWithGz = path + ".gz";
    if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
        if (SPIFFS.exists(pathWithGz))
            path += ".gz";
        File file = SPIFFS.open(path, "r");
        size_t sent = webServer.streamFile(file, contentType);
        file.close();
        return true;
    }
    Serial.println("False!");
    return false;
}

void handleFormat() {
    Serial.println("SPIFFS.format");
    SPIFFS.format();
    Serial.println("SPIFFS.format Done");
    webServer.send(200, "text/plain", "OK");
}

void handleFileList() {
    if(!webServer.hasArg("dir")) {
        webServer.send(500, "text/plain", "BAD ARGS");
        return;
    }
    String path = webServer.arg("dir");
    Serial.println("handleFileList: " + path);
    Dir dir = SPIFFS.openDir(path);
    path = String();

    String output = "[";
    while(dir.next()) {
        File entry = dir.openFile("r");
        if (output != "[") output += ',';
        bool isDir = false;
        output += "{\"type\":\"";
        output += (isDir) ? "dir" : "file";
        output += "\",\"name\":\"";
        output += String(entry.name()).substring(1);
        output += "\"}";
        entry.close();
    }

    output += "]";
    webServer.send(200, "text/json", output);
}

void handleClientData(uint8_t num, String data) {
    Serial.println(data);
    switch (data.charAt(0)) {
    case PAGE_STATE:
        client_sp2[num].page = data.charAt(1) - 48;
        sendStatus(num, data.charAt(1) - 48);
        break;
    case CMD_ONOFF:
        onoff = data.substring(1).toInt();
        if (onoff) {
            volt = watt = ampere = watth = 0;
        }
        digitalWrite(POWER, onoff);
        digitalWrite(POWERLED, LOW);
        send_data_to_clients(String(CMD_ONOFF) + onoff, HOME, num);
        break;
    case SET_VOLTAGE:
        setVoltage = data.substring(1).toFloat();
        mcp4652_write(WRITE_WIPER0, quadraticRegression(setVoltage));
        send_data_to_clients(String(SET_VOLTAGE) + setVoltage, HOME, num);
        break;
    case SET_DEFAULT_VOLTAGE: {
        File f = SPIFFS.open("/txt/settings.txt", "r+");
        f.seek(0, SeekSet);
        f.findUntil("voltage", "\n\r");
        f.seek(1, SeekCur);
        f.print(setVoltage);
        f.close();
        break;
    }
    case SET_AUTORUN: {
        if (autorun == data.substring(1).toInt()) {
            break;
        }
        autorun = data.substring(1).toInt();

        File f = SPIFFS.open("/txt/settings.txt", "r+");
        f.seek(0, SeekSet);
        f.findUntil("autorun", "\n\r");
        f.seek(1, SeekCur);
        f.print(autorun);
        f.close();

        send_data_to_clients(String(SET_AUTORUN) + autorun, SETTINGS, num);
        break;
    }
    case SAVE_NETWORKS: {
        String _ssid = data.substring(1, data.indexOf(','));
        data.remove(1, data.indexOf(','));
        String _ipaddr = data.substring(1, data.indexOf(','));
        data.remove(1, data.indexOf(','));
        String _password = data.substring(1, data.indexOf(','));

        if ((String(config_ssid) != _ssid) || (config_ip.toString() != _ipaddr) || (String(password) != _password)) {
            saveNetworkConfig(_ssid, _ipaddr, _password);
            readNetworkConfig();
            for (int i = 0; i < 5; i++) {
                if (client_sp2[i].connected && client_sp2[i].page && (num != i))
                    sendStatus(i, 1);
            }
        }
        break;
    }
    case MEASUREWATTHOUR:
        measureWh = data.substring(1).toInt();
        send_data_to_clients(String(MEASUREWATTHOUR) + measureWh, HOME, num);
        break;
    }
}

void fs_init(void) {
    String ssidTemp = "ssid=\"SmartPower2_" + String(ESP.getChipId(), HEX) + "\"";

    File f = SPIFFS.open("/js/settings.js", "w");
    f.println(ssidTemp.c_str());
    f.println("ipaddr=\"192.168.4.1\"");
    f.println("passwd=\"12345678\"");
    f.flush();
    f.close();
    readNetworkConfig();

    File f2 = SPIFFS.open("/txt/settings.txt", "w");
    f2.println("autorun=0");
    f2.println("voltage=5.00");
    f2.println("firstboot=0");
    f2.flush();
    f2.close();
    readHWSettings();
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {
    switch(type) {
    case WStype_DISCONNECTED :
        USE_SERIAL.printf("[%u] Disconnected!\n\r", num);
        client_sp2[num].connected = 0;
        connectedWeb = 0;
        break;
    case WStype_CONNECTED : {
        IPAddress ip = webSocket.remoteIP(num);
        USE_SERIAL.printf("[%u] WS connected from %s url: %s\n\r",
                          num, ip.toString().c_str(), payload);
        client_sp2[num].connected = 1;
        connectedWeb = 1;
        break;
    }
    case WStype_TEXT : {
        handleClientData(num, String((char *)&payload[0]));
        break;
    }
    case WStype_BIN :
        USE_SERIAL.printf("[%u] get binary lenght: %u\n\r", num, lenght);
        hexdump(payload, lenght);
        break;
    }
}

void readHWSettings(void) {
    USE_SERIAL.println(__FUNCTION__);
    File f = SPIFFS.open("/txt/settings.txt", "r");

    f.seek(0, SeekSet);
    f.findUntil("autorun", "\n\r");
    f.seek(1, SeekCur);
    autorun = f.readStringUntil('\n').toInt();

    f.findUntil("voltage", "\n\r");
    f.seek(1, SeekCur);
    setVoltage = f.readStringUntil('\n').toFloat();

    f.close();
}

void saveNetworkConfig(String _ssid, String _ipaddr, String _password) {
    USE_SERIAL.println(__FUNCTION__);
    File f = SPIFFS.open("/js/settings.js", "w");
    f.println("ssid=\"" + _ssid + "\"");
    f.println("ipaddr=\"" + _ipaddr + "\"");
    f.println("passwd=\"" + _password + "\"");
    f.flush();
    f.close();
}

void readNetworkConfig(void) {
    unsigned int ipaddr[4];

    USE_SERIAL.println(__FUNCTION__);
    File f = SPIFFS.open("/js/settings.js", "r");

    f.findUntil("ssid", "\n\r");

    f.seek(2, SeekCur);
    f.readStringUntil('"').toCharArray(config_ssid, 20);

    f.findUntil("ipaddr", "\n\r");
    f.seek(2, SeekCur);
    ipaddr[0] = f.readStringUntil('.').toInt();
    ipaddr[1] = f.readStringUntil('.').toInt();
    ipaddr[2] = f.readStringUntil('.').toInt();
    ipaddr[3] = f.readStringUntil('"').toInt();
    config_ip = IPAddress(ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3]);

    f.findUntil("passwd", "\n\r");
    f.seek(2, SeekCur);
    f.readStringUntil('"').toCharArray(password, 20);
    f.close();
}

bool isFirstBoot() {
    USE_SERIAL.println(__FUNCTION__);
    File f = SPIFFS.open("/txt/settings.txt", "r");

    f.seek(0, SeekSet);
    f.findUntil("firstboot", "\n\r");
    f.seek(1, SeekCur);
    int value = f.readStringUntil('\n').toInt();

    bool result;
    if (value) {
        result = true;
    } else {
        result = false;
    }

    f.close();

    return result;
}

void initSmartPower(void) {
    USE_SERIAL.println(__FUNCTION__);

    if (isFirstBoot()) {
        fs_init();
    } else {
        readHWSettings();
        readNetworkConfig();
    }

    onoff = !autorun;


    mcp4652_write(WRITE_WIPER0, quadraticRegression(setVoltage));
    pinMode(POWER, OUTPUT);
    digitalWrite(POWER, onoff);

    pinMode(D4, OUTPUT);
    digitalWrite(D4, HIGH);
}

// lcd slave address are 0x27 or 0x3f
unsigned char lcdSlaveAddr;
int lcd_available(void) {
    Wire.beginTransmission(0x27);
    if (!Wire.endTransmission()) {
        lcdSlaveAddr = 0x27;
        return lcdSlaveAddr;
    } else {
        Wire.beginTransmission(0x3f);
        if (!Wire.endTransmission()) {
            lcdSlaveAddr = 0x3f;
            return lcdSlaveAddr;
        }
    }

    lcdSlaveAddr = 0;

    return 0;
}

void lcd_status(void) {
    if (lcd_available() > 0) {
        if (!connectedLCD) {
            lcd = LiquidCrystal_I2C(lcdSlaveAddr, 16, 2);
            lcd.init();
            lcd.backlight();
            connectedLCD = 1;
        }
    } else {
        connectedLCD = 0;
    }
}

void printPower_LCD(void) {
    double rwatth;
    lcd.setCursor(0, 0);
    lcd.print(volt, 3);
    lcd.print(" V ");
    lcd.print(ampere, 3);
    lcd.print(" A  ");

    lcd.setCursor(0, 1);
    if (watt < 10) {
        lcd.print(watt, 3);
    } else {
        lcd.print(watt, 2);
    }
    lcd.print(" W ");

    rwatth = watth / 3600;
    if (rwatth < 10) {
        lcd.print(rwatth, 3);
        lcd.print(" ");
    } else if (rwatth < 100) {
        lcd.print(rwatth, 2);
        lcd.print(" ");
    } else if (rwatth < 1000) {
        lcd.print(rwatth, 1);
        lcd.print(" ");
    } else {
        lcd.print(rwatth / 1000, 0);
        lcd.print(" K");
    }
    lcd.print("Wh     ");
}

uint8_t cnt_ssid;
int8_t  cnt_ip;
int8_t  cursor_ssid;
int8_t  cursor_ip;

void printInfo_LCD(void) {
    String str;
    int i;
    cnt_ssid = String(current_ssid).length();
    lcd.setCursor(0, 0);
    if ((WiFi.getMode() & WIFI_AP) != 0) {
        lcd.print("  AP:");
    } else {
        lcd.print(" CLI:");
    }
    str = String(current_ssid);
    if (cnt_ssid < MAX_LCD_SSID_LENGTH) {
        lcd.print(str);
        for (i = 0; i < MAX_LCD_SSID_LENGTH; i++) {
            lcd.print(" ");
        }
    } else {
        if ((cursor_ssid - 5) == cnt_ssid) {
            cursor_ssid = 0;
        }
        lcd.print(str.substring(cursor_ssid++));
        if ((cnt_ssid - cursor_ssid) < MAX_LCD_SSID_LENGTH - 5) {
            if (cnt_ssid < cursor_ssid) {
                for (i = 0; i < 6 - (cursor_ssid - cnt_ssid); i++)
                    lcd.print(" ");
            } else {
                lcd.print("     ");
            }
            lcd.print(str);
        } else if ((cnt_ssid - cursor_ssid) < MAX_LCD_SSID_LENGTH) {
            for (i = 0; i < MAX_LCD_SSID_LENGTH - (cnt_ssid - cursor_ssid); i++)
                lcd.print(" ");
        }
    }

    lcd.setCursor(0, 1);
    lcd.print("IP:");
    cnt_ip = current_ip.toString().length();
    if (cnt_ip < MAX_LCD_IP_LENGTH) {
        lcd.print(current_ip.toString());
        for (i = 0; i < MAX_LCD_IP_LENGTH - cnt_ip; i++)
            lcd.print(" ");
    } else {
        if ((cursor_ip - 5) == cnt_ip) {
            cursor_ip = 0;
        }
        lcd.print(current_ip.toString().substring(cursor_ip++));
        if ((cnt_ip - cursor_ip) < MAX_LCD_IP_LENGTH - 5) {
            if (cnt_ip < cursor_ip) {
                for (i = 0; i < 6 - (cursor_ip - cnt_ip); i++)
                    lcd.print(" ");
            } else {
                lcd.print("     ");
            }
            lcd.print(current_ip.toString());
        } else if((cnt_ip - cursor_ip) < MAX_LCD_IP_LENGTH) {
            for (i = 0; i < MAX_LCD_IP_LENGTH - (cnt_ip - cursor_ip); i++)
                lcd.print(" ");
        }
    }
}

void readPower(void) {
    volt = ina231_read_voltage();
    watt = ina231_read_power();
    ampere = ina231_read_current();
}

void wifi_connection_status(void) {
    if (WiFi.softAPgetStationNum() > 0) {
        if (connectedWeb) {
            D4state = !D4state;
        } else {
            D4state = LOW;
        }
    } else {
        D4state = HIGH;
    }
    digitalWrite(D4, D4state);
}

double a = 0.0000006562;
double b = 0.0022084236;
float c = 4.08;
int quadraticRegression(double volt) {
    double d;
    double root;
    d = b * b - a * (c - volt);
    root = (-b + sqrt(d)) / a;
    if (root < 0) {
        root = 0;
    } else if (root > 255) {
        root = 255;
    }
    return root;
}

unsigned long btnPress;
unsigned long btnRelese = 1;
unsigned long currtime;
unsigned char btnChanged;
unsigned char resetCnt;
unsigned char swlock;
void pinChanged() {
    if ((millis() - currtime) > 30) {
        swlock = 0;
    }

    if (!swlock) {
        if (!digitalRead(BTN_ONOFF) && (btnPress == 0)) {
            swlock = 1;
            currtime = millis();
            btnPress = 1;
            btnRelese = 0;
        }
    }

    if (!swlock) {
        if (digitalRead(BTN_ONOFF) && (btnRelese == 0)) {
            swlock = 1;
            currtime = millis();
            btnRelese = 1;
            btnPress = 0;
            btnChanged = 1;
            onoff = !onoff;
            watth = 0;
            digitalWrite(POWER, onoff);
            digitalWrite(POWERLED, LOW);
        }
    }
}

void readSystemReset() {
    if (!digitalRead(BTN_ONOFF) && (btnPress == 1)) {
        if (resetCnt++ > 5) {
            USE_SERIAL.println("System Reset!!");
            fs_init();
            resetCnt = 0;
        }
    } else {
        resetCnt = 0;
    }
}

void sendStatus(uint8_t num, uint8_t page) {
    if (!page) {
        webSocket.sendTXT(num, String(CMD_ONOFF) + onoff);
        webSocket.sendTXT(num, String(SET_VOLTAGE) + setVoltage);
        webSocket.sendTXT(num, String(MEASUREWATTHOUR) + measureWh);
        webSocket.sendTXT(num, String(FW_VERSION) + FWversion);
    } else if (page) {
        webSocket.sendTXT(num, String(SET_AUTORUN) + autorun);
        webSocket.sendTXT(num, String(SAVE_NETWORKS) + String(current_ssid) + "," +
                          config_ip.toString() + "," + String(password));
    }
}

void send_data_to_clients(String str, uint8_t page) {
    for (int i = 0; i < 5; i++) {
        if (client_sp2[i].connected && (client_sp2[i].page == page))
            webSocket.sendTXT(i, str);
    }
}

void send_data_to_clients(String str, uint8_t page, uint8_t num) {
    for (int i = 0; i < 5; i++) {
        if (client_sp2[i].connected && (client_sp2[i].page == page) && (num != i))
            webSocket.sendTXT(i, str);
    }
}

void handler(void) {
    if (onoff == ON) {
        digitalWrite(POWERLED, D1state = !D1state);
        if (connectedLCD || connectedWeb || logClient) {
            readPower();
        }
    }

    if (connectedLCD) {
        if (onoff == ON)
            printPower_LCD();
        else if (onoff == OFF)
            printInfo_LCD();
    }

    if (btnChanged) {
        if (onoff == OFF) {
            watt = volt = ampere = watth = 0;
        }
        if (connectedWeb) {
            send_data_to_clients(String(CMD_ONOFF) + onoff, HOME);
            measureWh = !onoff;
        }
        btnChanged = 0;
    }

    if (connectedWeb) {
        if (onoff == ON) {
            String data = String(DATA_PVI);
            data += String(watt, 3) + "," + String(volt, 3) + "," + String(ampere, 3);
            if (measureWh) {
                watth += watt;
            }
            data += "," + String(watth / 3600, 3);
            send_data_to_clients(data, HOME);
        }
    }

    if (logClient && logClient.connected()) {
        // Report the log info
        String data = String(volt, 3) + "," + String(ampere, 3) + "," +
                      String(watt, 3) + "," + String(watth / 3600, 3) + "\r\n";

        logClient.write(data.c_str());

        while(logClient.available()) {
            logClient.read();
        }
    }

    lcd_status();
    wifi_connection_status();
    readSystemReset();
}

File fsUploadFile;

void handleFileUpload() {

    HTTPUpload& upload = webServer.upload();

    if(upload.status == UPLOAD_FILE_START) {
        int i, ac = webServer.args();
        for (i = 0; i < ac; i++) {
            Serial.printf("%d %s %s \r\n", i, webServer.argName(i).c_str(), webServer.arg(i).c_str());
        }

        String filename = upload.filename;

        if (webServer.hasArg("path")) {
            String path = webServer.arg("path");
            if (!path.endsWith("/")) path += "/";
            filename = path + filename;
        } else {
            if(!filename.startsWith("/")) filename = "/" + filename;
        }

        Serial.print("handleFileUpload Name: ");
        Serial.println(filename);

        fsUploadFile = SPIFFS.open(filename, "w");
        filename = String();
    } else if(upload.status == UPLOAD_FILE_WRITE) {
        if(fsUploadFile)
            fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
    } else if(upload.status == UPLOAD_FILE_END) {
        if(fsUploadFile) {                                    // If the file was successfully created
            fsUploadFile.close();                               // Close the file again
            Serial.print("handleFileUpload Size: ");
            Serial.println(upload.totalSize);
            webServer.sendHeader("Location", "/success.html");     // Redirect the client to the success page
            webServer.send(303);
        } else {
            webServer.send(500, "text/plain", "500: couldn't create file");
        }
    }
}
