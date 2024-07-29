#include <Arduino.h>

#include <ESP8266WiFi.h>

#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <AsyncTimer.h>

#include <LittleFS.h>
#include <FS.h>

FS *filesystem = &LittleFS;

const int http_server_port = 80;

const char* wifi_ssid = "wifi_ssid";
const char* wifi_pass = "wifi_pass";

const char* http_admin_username = "admin";
const char* http_admin_password = "admin";

const int autoLockDelay = 5000;
const int debounceDelay = 100;

const String passwords[] = {
    "password1",
    "password2",
    "password3",
};

AsyncTimer timer;

class Logger {
    private:
    File logFile;
    public:
    Logger() {
        if(!filesystem->begin()) {
            this->log("Unable to mount LittleFS. Logging to Serial.");
        }
    }

    void log(String msg) {
        String logLine = "[" + (String)millis() + "] " + msg + "\n"; 
        Serial.println(logLine);

        logFile = filesystem->open("/log.txt", "a");
        if(logFile)
            logFile.write(logLine.c_str());
        logFile.close();
    }
    String getLog() {
        String result;
        logFile = filesystem->open("/log.txt", "r");
        if(!logFile)
            return result;
        
        while (logFile.available()) {
            result += logFile.readString();
        }
        logFile.close();

        return result;
    }
};


class HardwareLock {
    public:
    int lockpin, revlockpin;

    HardwareLock(int p1 = D3, int p2 = D4) {
        lockpin     = p1;
        revlockpin  = p2;

        pinMode(this->lockpin,    OUTPUT);
        pinMode(this->revlockpin, OUTPUT);
    }
    
    void lock() {
        digitalWrite(this->revlockpin, HIGH);
        digitalWrite(this->lockpin, LOW);
    }

    void unlock() {
        digitalWrite(this->lockpin, HIGH);
        digitalWrite(this->revlockpin, LOW);
    }
    bool getState() {
        return !digitalRead(this->lockpin);
    }
};

class APICalls {
    private:
    HardwareLock hwlock;
    Logger logger;

    public:
    bool checkPassword(AsyncWebServerRequest *request) {
        int success = 0;
        String password;
        for(size_t i=0; i<request->headers(); i++) {
            if(request->getHeader(i)->name() == "pass") {
                password = request->getHeader(i)->value();
                break;
            }
        }
        for(String pass : passwords) {
            if(pass == password) {
                success = 1;
                break;
            }
        }
        return success;
    }

    bool autolock(AsyncWebServerRequest *request) {
        String autolockValue;
        for(size_t i=0; i<request->headers(); i++) {
            if(request->getHeader(i)->name() == "autolock") {
                autolockValue = request->getHeader(i)->value();
                break;
            }
        }
        return autolockValue == "true";
    }

    void lock(AsyncWebServerRequest *request) {
        if(!checkPassword(request)) {
            logger.log("LOCK FAIL: " + request->client()->remoteIP().toString());
            request->send(403, "text/plain", "fail");
            return;
        }

        this->hwlock.lock();
        logger.log("LOCK SUCCESS: " + request->client()->remoteIP().toString());
        request->send(200, "text/plain", "success");
    }

    void unlock(AsyncWebServerRequest *request) {
        
        if(!checkPassword(request)) {
            logger.log("UNLOCK FAIL: " + request->client()->remoteIP().toString());
            request->send(403, "text/plain", "fail");
            return;
        }
        this->hwlock.unlock();
        logger.log("UNLOCK SUCCESS: " + request->client()->remoteIP().toString());
        request->send(200, "text/plain", "success");
        
        if(autolock(request)) {
            timer.setTimeout([this]() {
                if(this->hwlock.getState() == false) {
                    this->hwlock.lock();
                    logger.log(" \\_ AUTO LOCKED");
                }
            }, autoLockDelay);
        }
    }

    void state(AsyncWebServerRequest *request) {
        logger.log("STATE REQUEST: " + request->client()->remoteIP().toString());
        if(this->hwlock.getState())
            request->send(200, "text/plain", "locked");
        else
            request->send(200, "text/plain", "unlocked");
    }
};

class Button {
    private:
    int buttonPin = D2;
    Logger logger;
    HardwareLock hwlock;
    bool btnState = false;
    bool lastBtnState = false;
    unsigned long lastDebounceTime = 0; 

    public:

    Button(int p1 = D2) {
        buttonPin = p1;
        pinMode(buttonPin, INPUT_PULLUP);
    }
    bool isClicked() {
        bool reading = !digitalRead(buttonPin);
        if (reading != lastBtnState) {
            lastDebounceTime = millis();
        }
        if ((millis() - lastDebounceTime) > debounceDelay) {
            if (reading != btnState) {
                btnState = reading;
                if (btnState) {
                    return true;
                }
            }
        }
        lastBtnState = reading;
        return false;
    }
    void lock() {
        this->hwlock.lock();
        logger.log("LOCK SUCCESS: LOCAL");
    }

    void unlock() {
        logger.log("UNLOCK SUCCESS: LOCAL");
        hwlock.unlock();
        timer.setTimeout([this]{
            this->hwlock.lock();
            logger.log("LOCK SUCCESS: LOCAL");
            logger.log(" \\_ AUTO LOCKED");
        }, autoLockDelay);
    }
};

AsyncWebServer server(http_server_port);

HardwareLock rootHwlock;
APICalls api;
Button button(D1);

void setup() {
    Serial.begin(115200);
    rootHwlock.lock();

    Serial.printf("Connecting to %s...\n", wifi_ssid); 
    WiFi.begin(wifi_ssid, wifi_pass);

    while(!WiFi.isConnected()) {
        delay(250);
        Serial.print('.');
    }
    Serial.printf("\nConnected to %s. IP: %s\n", wifi_ssid, WiFi.localIP().toString().c_str());

    server.on("/api/lock", HTTP_GET, [](AsyncWebServerRequest *request){
        api.lock(request);
    });
    server.on("/api/unlock", HTTP_GET, [](AsyncWebServerRequest *request){
        api.unlock(request);
    });
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request){
        api.state(request);
    });
    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(!request->authenticate(http_admin_username, http_admin_password))
            return request->requestAuthentication();
        Logger logger;

        String response = "<html><body style=\"background: #D1D1D1;\"><h1 style=\"text-align: center;text-shadow: 1px 1px 5px #515151;\">Log File</h1><pre>";
        response += logger.getLog();
        response += "</pre></body></html>";
        request->send(200, "text/html", response);
    });

    server.begin();

}

void loop() {
    if(button.isClicked()) {
        button.unlock();
    }
    timer.handle();
}
