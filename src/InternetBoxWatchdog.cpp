// Hardware: HW-622: ESP8266 Relay

#define ENABLE_ARDUINOOTA
#define ENABLE_UPDATE

#ifdef ENABLE_ARDUINOOTA
# define NO_GLOBAL_ARDUINOOTA
# include <ArduinoOTA.h>
#endif
#include <WiFiManager.h>  // Need to patch to transfer to cpp some duplicated definitions with WebServer.h, make connectWifi public and change HTTP_HEAD to _HTTP_HEAD
#include "Arduino.h"
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
extern "C" {
  #include <ping.h>
}

#define SERIAL_DEBUG false               // Enable / Disable log - activer / désactiver le journal

#define MIN(X,Y)    ((X)<(Y)?(X):(Y))
#define MAX(X,Y)    ((X)>(Y)?(X):(Y))
#define STRINGIFY(s) STRINGIFY2(s)
#define STRINGIFY2(s) #s

#define TAG "InternetBoxWatchdog"
#define VERSION   "1.00"
#define RELAY_SWITCHING_TIME    3000

#define MAX_URL    64
String PingAddress;
int PingPeriod = 3;
enum { ACTIVE = 0, INTERMEDIATE = 1, DEACTIVE = 2 } PingState = ACTIVE;

// Web server port - port du serveur web
#define WEB_SERVER_PORT 80
#define URI_WIFI "/reset"
#define URI_REBOOT "/reboot"
#define URI_OTA "/ota"
#define URI_UPDATE "/update"
#define URI_STATUS "/status"
#define URI_ROOT "/"
#define URI_USAGE "/help"
#define URI_COMMIT "/commit"
#define URI_REARM "/rearm"
#define URI_SWITCH "/switch"
#define URI_CHECK "/check"
#define URI_INFO "/info"

ESP8266WebServer server ( WEB_SERVER_PORT );

#define OTA_TIMER (10*60)
#define REBOOT_TIMER (3)
#define OTA_REBOOT_TIMER (1)

int64_t rebootRequested = 0;
int64_t otaOnTimer = 0;

#define PIN_SENSOR          D1  // GPIO5
#define PIN_RELAY           D2  // GPIO4

static void requestReboot(int timer = REBOOT_TIMER)
{
  if (timer == 0) {
    ESP.restart();
  }
  else {
    rebootRequested = millis() + timer * 1000;    
  }
}

extern "C" void esp_schedule();
extern "C" void esp_yield();

struct ping_result {
    uint errors;
    uint success;
    bool done;
};
static bool check(String address) {
  IPAddress ip;
  if (WiFi.hostByName(address.c_str(), ip, 1000) && ip != INADDR_NONE) {
    static struct ping_result result;
    memset(&result, 0, sizeof(struct ping_result));
    static struct ping_option _options;
    memset(&_options, 0, sizeof(struct ping_option));

    // Repeat count (how many time send a ping message to destination)
    _options.count = 1;
    // Time interval between two ping (seconds??)
    _options.coarse_time = 1;
    // Destination machine
    _options.ip = ip;
    // user data
    _options.reverse = &result;

    // Callbacks
    ping_regist_recv(&_options, [](void* opt, void * resp){
        struct ping_option* ping_opt  = reinterpret_cast<struct ping_option*>(opt);
        struct ping_resp*   ping_resp = reinterpret_cast<struct ping_resp*>(resp);
        struct ping_result* ping_result = reinterpret_cast<struct ping_result*>(ping_opt->reverse);
        if (ping_resp->bytes == 0) {
            ++ping_result->errors;
        }
        else {
            ++ping_result->success;
        }
    });
    ping_regist_sent(&_options, [](void* opt, void *){
        struct ping_option* ping_opt  = reinterpret_cast<struct ping_option*>(opt);
        struct ping_result* ping_result = reinterpret_cast<struct ping_result*>(ping_opt->reverse);
        ping_result->done = true;
    });

    // Let's go!
    ping_start(&_options);
    while (!result.done) delay(100);
    return result.success > 0;
  }
  return false;
}

static void wifi_handler() {
  server.send(200);
  system_restore();
  requestReboot(0);
}

static void reboot_handler() {
  requestReboot();
  server.send(200);
}

#ifdef ENABLE_ARDUINOOTA
ArduinoOTAClass * OTA = NULL;
static void enableOTA(bool enable, bool forceCommit = false)
{
  if (enable) {
    if (OTA == NULL) {
      OTA = new ArduinoOTAClass();
      OTA->setHostname(TAG);
      OTA->setPasswordHash("913f9c49dcb544e2087cee284f4a00b7");   // MD5("device")
      OTA->begin();
      int64_t fr_start = millis();
      otaOnTimer = fr_start + OTA_TIMER * 1000; // 10 minutes
    }
  }
  else {
    if (OTA) {
      if (forceCommit) {
        delete OTA;
        OTA = NULL;
        otaOnTimer = 0;
      }
      else {
        otaOnTimer = 1; // to be disabled on next loop
      }
    }
  }
}
static void ota_handler() {
  if (server.arg("action") == "on") {
    enableOTA(true);
  }
  else if (server.arg("action") == "off") {
    enableOTA(false);
  }
  else if (server.arg("action") == "toggle") {
    enableOTA(OTA == NULL);
  }
  server.send(200);
}
#endif

#ifdef ENABLE_UPDATE
static void update_handler() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.setDebugOutput(true);
    WiFiUDP::stopAll();
    Serial.printf("Update: %s\n", upload.filename.c_str());
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) { //start with max available size
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { //true to set the size to the current progress
      Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
    Serial.setDebugOutput(false);
  }
  yield();
}
#endif

static void usage_handler() {
  static const __FlashStringHelper* info =
    F("<html>"
      "<head>"
        "<title>" TAG " - API</title>"
        "<style>"
        "</style>"
      "</head>"
      "<body>"
        "<h1>" TAG "<span id=\"version\"> (v" VERSION ") - API Usage</span></h1>"
          "<ul>"
            "<li>Reboot: " URI_REBOOT "</li>"
            "<li>Reset Wifi: " URI_WIFI "</li>"
#ifdef ENABLE_ARDUINOOTA
            "<li>OTA on/off/toggle: " URI_OTA "?action=[on|off|toggle]</li>"
#endif
            "<li>Commit: " URI_COMMIT "?address=&lt;address&gt;&period=&lt;period&gt;</li>"
            "<li>Check: " URI_CHECK "?[address=&lt;address&gt;]</li>"
            "<li>Rearm: " URI_REARM "</li>"
            "<li>Switch Relay: " URI_SWITCH "</li>"
            "<li>Status (JSON): " URI_STATUS "</li>"
        "</ul>"
      "</body>"
    "</html>");
  server.send(200, "text/html", info);
}

static void commit_handler() {
  PingAddress = server.arg("address");
  PingPeriod = server.arg("period").toInt();
  EEPROM.begin(sizeof(int) + MAX_URL + 1);
  EEPROM.put(0, PingPeriod);
  int size = MIN(MAX_URL, PingAddress.length());
  for (int i = 0; i < size; ++i)
    EEPROM.write(sizeof(int) + i, PingAddress[i]);
  EEPROM.write(sizeof(int) + size, 0);
  EEPROM.commit();
  EEPROM.end();
  server.send(200);
}

static void check_handler() {
  server.send(check(server.hasArg("address")?server.arg("address"):PingAddress)?200:404);
}

static void rearm_handler() {
    PingState = ACTIVE;
    server.send(200);
}

static void doSwitch() {
    digitalWrite(PIN_RELAY, HIGH);
    delay(RELAY_SWITCHING_TIME);
    digitalWrite(PIN_RELAY, LOW);
}

static void switch_handler() {
    doSwitch();
    server.send(200);
}

static void status_handler() {
  unsigned long currentTimer = millis();
  String info = "{"
                "\"version\":\"" VERSION "\","
                "\"ssid\":\"" + WiFi.SSID() + "\","
                "\"rssi\":" + String(WiFi.RSSI()) + ","
                "\"ip\":\"" + WiFi.localIP().toString() + "\","
                "\"mac\":\"" + WiFi.macAddress() + "\","
                "\"chipId\":\"" + ESP.getChipId() + "\","
#ifdef ENABLE_ARDUINOOTA
                "\"ota\":" + String(OTA ? "true" : "false") + ","
                "\"otaTimer\":" + String(MAX(0, int((otaOnTimer - currentTimer) / 1000))) + ","
#endif
                "\"ping_address\":\"" + PingAddress + "\","
                "\"ping_period\":" + String(PingPeriod) + ","
                "\"ping_state\":" + String((int)PingState) + ","
                "\"reboot\":" + String(rebootRequested > 0 ? "true" : "false") + ","
                "\"rebootTimer\":" + String(MAX(0, int((rebootRequested - currentTimer) / 1000))) + ""
                "}";
  server.send(200, "application/json", info);
}

static void info_handler() {
  static const __FlashStringHelper* info =
    F("<html>"
      "<head>"
        "<title>" TAG "</title>"
        "<script type=\"text/javascript\">"
          "function commit()"
          "{"
            "var xhr = new XMLHttpRequest();"
            "xhr.open(\"GET\", \"" URI_COMMIT "?address=\" + document.getElementById('ping_address').value + \"&period=\" + document.getElementById('ping_period').value, true);"
            "xhr.send(null);"
          "};"
          "function check()"
          "{"
            "var xhr = new XMLHttpRequest();"
            "xhr.open(\"GET\", \"" URI_CHECK "?address=\" + document.getElementById('ping_address').value, true);"
            "xhr.onloadend = function () {"
              "document.getElementById(\"state\").innerHTML = (xhr.status === 200?\"&#10003;\":\"&#10005;\");"
            "};"
            "xhr.send(null);"
          "};"
          "function update()"
          "{"
            "var xhr = new XMLHttpRequest();"
            "xhr.open(\"GET\", \"" URI_STATUS "\", true);"
            "xhr.onload = function (e) {"
              "if (xhr.readyState === 4) {"
                "if (xhr.status === 200) {"
                  "var obj = JSON.parse(xhr.responseText);"
                  "document.getElementById(\"ssid\").innerHTML = obj.ssid;"
                  "document.getElementById(\"rssi\").innerHTML = obj.rssi;"
                  "document.getElementById(\"ip\").innerHTML = obj.ip;"
                  "document.getElementById(\"mac\").innerHTML = obj.mac;"
                  "if (document.getElementById(\"ping_address\").value === \"\") document.getElementById(\"ping_address\").value = obj.ping_address;"
                  "document.getElementById(\"ping_period\").value = obj.ping_period;"
                  "document.getElementById(\"ping_state\").innerHTML = obj.ping_state == 0?\"&#10003;\":(obj.ping_state == 2?\"&#10005;\":\"?\");"
                  "document.getElementById(\"reboot\").innerHTML = obj.rebootTimer>0?\" - \"+obj.rebootTimer:\"\";"
#ifdef ENABLE_ARDUINOOTA
                  "document.getElementById(\"ota\").innerHTML = obj.ota==\"true\"?\" - On (\"+Math.round(obj.otaTimer/60)+\"min)\":\" - Off\";"
#endif
                "}"
              "}"
            "};"
            "xhr.send(null);"
          "};"
          "update();"
          "setInterval(update, 3000);"
          "setInterval(check, 10000);"
        "</script>"
      "</head>"
      "<body>"
        "<h1>" TAG "<span id=\"version\"> (v" VERSION ")</span></h1>"
          "<table style=\"height: 60px;\" width=\"100%\">"
          "<tbody>"
            "<tr>"
              "<td style=\"width: 50%;\">"
                "<span class=\"info\">SSID: </span><span id=\"ssid\"></span>"
                "<br/>"
                "<span class=\"info\">RSSI: </span><span id=\"rssi\"></span>"
                "<br/>"
                "<span class=\"info\">IP: </span><span id=\"ip\"></span>"
                "<br/>"
                "<span class=\"info\">MAC: </span><span id=\"mac\"></span>"
              "</td>"
            "</tr>"
            "<tr>"
              "<td style=\"width: 50%;\">"
                "Status: <span id=\"ping_state\"></span>"
                "<br/>"
                "Ping Address: <input id=\"ping_address\" type=\"text\" maxlength=\"" STRINGIFY(MAX_URL) "\"/>&nbsp;<span id=\"state\"></span>&nbsp;"
                "Count: <input id=\"ping_period\" type=\"number\" min=\"3\" max=\"999\">"
                "&nbsp;<input type=\"button\" onclick=\"commit()\" value=\"Save\"/>"
                "<br/>"
                "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_REBOOT "\');return false;\">Reboot Device</a><span id=\"reboot\"></span>"
                "<br/>"
                "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_WIFI "\');return false;\">Reset Device</a>"
                "<br/>"
#ifdef ENABLE_ARDUINOOTA
                "<a class=\"link\"  href=\"\" onclick=\"invoke(\'" URI_OTA "?action=toggle\');return false;\">Toggle OTA</a><span id=\"ota\"></span>"
                "<br/>"
#endif
#ifdef ENABLE_UPDATE
                "<form id=\"upgradeForm\" method=\"post\" enctype=\"multipart/form-data\" action=\"" URI_UPDATE "\"><span class=\"action\">Upgrade Firmware: </span><input type=\"file\" name=\"fileToUpload\" id=\"upgradeFile\" /><input type=\"submit\" value=\"Upgrade\" id=\"upgradeSubmit\"/></form>"
                "<br/>"
#endif
              "</td>"
            "</tr>"
          "</tbody>"
        "</table>"
        "<a href=\"" URI_USAGE "\">API Usage</a>"
      "</body>"
    "</html>");
  server.send(200, "text/html", info);
}

static void startServer() {
  server.on ( URI_ROOT, info_handler );
  server.on ( URI_INFO, info_handler );
  server.on ( URI_USAGE, usage_handler );
  server.on ( URI_COMMIT, commit_handler );
  server.on ( URI_REARM, rearm_handler );
  server.on ( URI_SWITCH, switch_handler );
  server.on ( URI_CHECK, check_handler );
  server.on ( URI_STATUS, status_handler );
  server.on ( URI_WIFI, wifi_handler );
  server.on ( URI_REBOOT, reboot_handler );
#ifdef ENABLE_ARDUINOOTA
  server.on ( URI_OTA, ota_handler );
#endif
#ifdef ENABLE_UPDATE
  server.on ( URI_UPDATE, HTTP_POST, []() {
        String html = "<html>"
                        "<head>"
                        "<title>" TAG " - OTA</title>" +
                        (!Update.hasError() ? "<meta http-equiv=\"refresh\" content=\"" + String(OTA_REBOOT_TIMER + 1) + "; url=/\">" : "") +
                        "</head>"
                        "<body>Update " + (Update.hasError() ? "failed" : "succeeded") + "</body>"
                    "</html>";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", html);
    requestReboot();
  }, update_handler );
#endif
  server.begin();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(SERIAL_DEBUG);

  // Wi-Fi connection - Connecte le module au réseau Wi-Fi
  // attempt to connect; should it fail, fall back to AP
  WiFiManager().autoConnect(TAG + ESP.getChipId(), "");

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, false);

  EEPROM.begin(sizeof(int) + MAX_URL + 1);
  EEPROM.get(0, PingPeriod);
  PingAddress.clear();
  for (int i = 0; i < MAX_URL + 1; ++i) {
      char val = (char)EEPROM.read(sizeof(int) + i);
      if (val == 0) break;
      PingAddress += val;
  }
  EEPROM.end();

  startServer();
}

void loop() {
  server.handleClient();
  unsigned long currentTime = millis();

  static uint32_t lastChecked = 0;
  if (currentTime > lastChecked + PingPeriod * 1000 ) {
    if (!check(PingAddress)) {
      if (PingState == INTERMEDIATE) {
        doSwitch();
        PingState = DEACTIVE;
      }
      else if (PingState == ACTIVE) {
          PingState = INTERMEDIATE;
      }
    }
    else {
        PingState = ACTIVE;
    }
    lastChecked = currentTime;
  }

  if (rebootRequested != 0 && currentTime > rebootRequested) {
    rebootRequested = 0;
    requestReboot(0);
  }
#ifdef ENABLE_ARDUINOOTA
  if (OTA)  OTA->handle();
  if (otaOnTimer != 0 && currentTime > otaOnTimer) {
    enableOTA(false, true);
  }
#endif

  if (!WiFi.isConnected()) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(15 * 60);
    wm.autoConnect((TAG "-" + String((uint32_t)ESP.getChipId())).c_str(), "");
    if (WiFi.isConnected()) {
      server.begin();
    }
  }
  if (WiFi.isConnected()) {
    server.handleClient();
  }

  delay(100);
}
