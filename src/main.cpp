/**
 * @file 		  main.cpp
 *
 * Project		: Home automation
 * Author		: Bernd Waldmann
 * Created		: 03-Oct-2024
 * Tabsize		: 4
 * 
 * This Revision: $Id: main.cpp 1671 2024-11-17 17:10:08Z  $
 */

/*
   Copyright (C) 2024 Bernd Waldmann

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. 
   If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/

   SPDX-License-Identifier: MPL-2.0
*/

/*
  Relies on MySensors, Created by Henrik Ekblad <henrik.ekblad@mysensors.org>.
  Note that the MySensors library is under GPL, so 
  - if you want to combine this source code file with the MySensors library 
    and redistribute it, then read the GPL to find out what is allowed.
  - if you want to combine this file with the MySensors library and only 
    use it at home, then read the GPL to find out what is allowed.
  - if you want to take just this source code, learn from it, modify it, and 
    redistribute it, independent from MySensors, then you have to abide by 
    my license rules (MPL 2.0), which imho are less demanding than the GPL.
 */

/**
 * @brief MySensors gateway or repeater, with ESP32 with LAN8720 Ethernet adapter
*/

//----- some configuration options
// these are defined in platformio.ini
// #define USE_ETHERNET    // use Ethernet with LAN8720 rather than WiFi
// #define USE_HSPI        // implies MISO=12 MOSI=13 SCK=14 SS=15
// #define USE_DS18B20     // use temperature sensor

// these can be enabled here
#define USE_SYSLOG      // report to remote syslog server, URL see below
#define USE_NTP         // get time from time server, URL see below
#define USE_HTTP        // enable web UI
//#define USE_ASYNC_WEBSERVER
#define USE_OTA         // enable over-the-air firmware update

//----- uncomment either of these two
//#define OPERATE_AS_GATEWAY
#define OPERATE_AS_REPEATER

//----- the Arduino universe
#include <Arduino.h>
#include <WiFi.h>
#ifdef USE_ETHERNET
 #include <ETH.h>                // LGPLv2.1+ license
#endif
#ifdef USE_SYSLOG
 #include <Syslog.h>             // MIT license, https://github.com/arcao/Syslog
#endif
#ifdef USE_NTP
 #include <NTPClient.h>          // MIT license, https://github.com/arduino-libraries/NTPClient
#endif
#ifdef USE_OTA
 #include <ArduinoOTA.h>         // LGPLv2.1+ license, https://github.com/jandrassy/ArduinoOTA
#endif
#ifdef USE_HTTP 
 #ifdef USE_ASYNC_WEBSERVER
  #include <ESPAsyncWebServer.h>    // LGPLv2.1+ license
 #else
  #include <WebServer.h>        // LGPLv2.1+ license
 #endif
#endif
#ifdef USE_DS18B20
 #include <OneWire.h>
 #include <DS18B20.h>           // MIT license, https://github.com/RobTillaart/DS18B20_RT
#endif

//----- the ESP-IDF universe
#include <rom/rtc.h>            // Apache-2.0 license
#include <esp32/clk.h>
#include <esp_pm.h>

//----- my headers
#include "ansi.h"
#include "Revision.h"   // automatically generated header file with SVN revision
#include "secrets.h"    // WiFi password etc

//=====================================================================
#pragma region configuration

#ifdef USE_ETHERNET
 #define IF_NAME "ETH"
#else
 #define ETH WiFi
 #define IF_NAME "WiFi"
#endif

#if defined(USE_ETHERNET) && !defined(USE_HSPI)
 #define USE_HSPI
#endif

//----- pins for LAN8720 Ethernet module
#define PIN_ETH_PHY_POWER   4      // Pin# of the enable for the ext crystal osc, -1 to disable
#define PIN_ETH_PHY_MDC     23      // Pin# of the I²C SCL
#define PIN_ETH_PHY_MDIO    18      // Pin# of the I²C SDA

//----- pin connected to DS18B20 temperature sensor (optional)
#define PIN_DS18B20         33      

//----- pins for SPI connected to NRF24 module
#ifdef USE_HSPI
 #define MY_RF24_CE_PIN      2
 #define MY_RF24_MISO_PIN    12
 #define MY_RF24_MOSI_PIN    13
 #define MY_RF24_SCK_PIN     14
 #define MY_RF24_CS_PIN      15
#else
 #define MY_RF24_CE_PIN     26
 #define MY_RF24_MISO_PIN    19
 #define MY_RF24_MOSI_PIN    23
 #define MY_RF24_SCK_PIN     18
 #define MY_RF24_CS_PIN      5 
#endif

//----- Syslog
#define SYSLOG_SERVER "log-server"
#define SYSLOG_PORT 514
#define SYSLOG_APPNAME "main"

//----- OTA
#define OTA_PASSWORD "123"
#define OTA_PORT    3232

//----- NTP
#define NTP_SERVER  "fritz.box"

//----- MySensors MQTT (only applies to gateway mode)
#define MY_CONTROLLER_URL_ADDRESS "ha-server"
#define MY_MQTT_PUBLISH_TOPIC_PREFIX "my/E/stat"
#define MY_MQTT_SUBSCRIBE_TOPIC_PREFIX "my/cmnd"

#define VERSION "$Id: main.cpp 1671 2024-11-17 17:10:08Z  $ " __DATE__ " " __TIME__

#ifdef LED_BUILTIN
 #define LED_INIT   pinMode(LED_BUILTIN,OUTPUT);
 #define TURN_LED_ON     digitalWrite(LED_BUILTIN,HIGH)
 #define TURN_LED_OFF    digitalWrite(LED_BUILTIN,LOW)
#else
 #define LED_INIT
 #define TURN_LED_ON
 #define TURN_LED_OFF
#endif

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region Timing

#define SECONDS		* 1000uL
#define MINUTES 	* 60uL SECONDS
#define HOURS 		* 60uL MINUTES
#define DAYS		* 24uL HOURS

/// minimum time between comms statistics reports
const unsigned long MIN_REPORT_INTERVAL = 60 MINUTES;
/// time between temperature measurements
const unsigned long REPORT_TEMPERATURE_INTERVAL = 30 MINUTES;
/// time between keepalive messages
const unsigned long REPORT_HELLO_INTERVAL = 5 MINUTES;

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region MySensors

// when using Ethernet, RF24 module must be connected via SPI(2) default pins, 
// because SPI(3) default pins used by LAN8720
#include <SPI.h>
#ifdef USE_HSPI
 SPIClass hspi(HSPI);   // implies MISO=12 MOSI=13 SCK=14 SS=15
 #define RF24_SPI hspi
#else
 SPIClass vspi(VSPI);   // implies MISO=12 MOSI=13 SCK=14 SS=15
 #define RF24_SPI vspi
#endif

#define MY_RADIO_RF24
// with NRF24-PA-LNA module with external antenna, use this setting
// otherwise, too much interference
#define MY_RF24_PA_LEVEL RF24_PA_LOW

#define MY_RF24_SPI_SPEED 1'000'000u
//#define MY_TRANSPORT_WAIT_READY_MS 3000 

// MySensors operating mode
//#define MY_DEBUG
//#define MY_DEBUG_VERBOSE_RF24
#define MY_INDICATION_HANDLER
#define MY_SPLASH_SCREEN_DISABLED

//----- operate as gateway
#ifdef OPERATE_AS_GATEWAY
 #ifdef USE_ETHERNET
  #define MY_GATEWAY_ESP32_ETHERNET
 #else
  #define MY_GATEWAY_ESP32_WIFI
 #endif
 #define MY_GATEWAY_MQTT_CLIENT
 #define FRIENDLY_PROJECT_NAME "ESP32 MySensors Gateway"
 #define NODE_ID_AS_SENSOR MY_NODE_ID
 #define MY_GATEWAY_MAX_CLIENTS 2
 #define MY_RF24_CHANNEL 99    // test only
#endif

//----- operate as repeater
#ifdef OPERATE_AS_REPEATER
 #define MY_REPEATER_FEATURE
 #define FRIENDLY_PROJECT_NAME "ESP32 MySensors Repeater"
#endif

#include <MySensors.h>

#define SENSOR_ID_ARC		98
#define V_TYPE_ARC			V_VAR5
MyMessage arcMessage = MyMessage(SENSOR_ID_ARC, V_TYPE_ARC);

#define SENSOR_ID_CMND      96

#ifdef USE_DS18B20
 #define SENSOR_ID_TEMP 		41
 MyMessage msgTemperature(SENSOR_ID_TEMP, V_TEMP);
#endif

#define SENSOR_ID_HELLO     95
MyMessage msgHello(SENSOR_ID_HELLO, V_TEXT);

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region Global variables

WiFiUDP udpClient;

#ifdef USE_SYSLOG
 Syslog syslog(udpClient, SYSLOG_SERVER, SYSLOG_PORT, "ESP32", SYSLOG_APPNAME, LOG_USER);
#endif

#ifdef USE_NTP
 NTPClient ntpClient(udpClient,NTP_SERVER);
#endif

#ifdef USE_HTTP 
 #ifdef USE_ASYNC_WEBSERVER
  AsyncWebServer httpServer(80);
 #else
  WebServer httpServer(80);
 #endif
#endif

#ifdef USE_DS18B20
 OneWire ow(PIN_DS18B20);
 DS18B20 ds18b20(&ow);
 bool hasDS18B20 = false;
#endif

/// static buffer for assembling various messages
char msgbuf[256];


/// for counting indication() status notifications
struct RxTxStats_t {
	unsigned nRx, nTx, nGwRx, nGwTx, nErr;
} rxtxStats;

/// nMessages[i] counts messages for node id ì`
unsigned nMessages[256];

struct ArcStats_t {
    unsigned packets;   ///< number of packets sent
    unsigned retries;   ///< number of retries required
    unsigned success;   ///< success rate in percent
} arcStats;

time_t t_last_clear = 0;

const char* reset_reasons[] = {
"0: none",
"1: Vbat power on reset",
"2: unknown",
"3: Software reset digital core",
"4: Legacy watch dog reset digital core",
"5: Deep Sleep reset digital core",
"6: Reset by SLC module, reset digital core",
"7: Timer Group0 Watch dog reset digital core",
"8: Timer Group1 Watch dog reset digital core",
"9: RTC Watch dog Reset digital core",
"10: Instrusion tested to reset CPU",
"11: Time Group reset CPU",
"12: Software reset CPU",
"13: RTC Watch dog Reset CPU",
"14: for APP CPU, reseted by PRO CPU",
"15: Reset when the vdd voltage is not stable",
"16: RTC Watch dog reset digital core and rtc module"
};

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region ARC statistics

/**
 * @brief Collect statistics re Automatic Retries Count (ARC) for RF24.
 * Call this function immediately after each `send()` call.
 * 
 */
void collectArcStatistics()
{
	int rssi = transportHALGetSendingRSSI();	// boils down to (-29 - (8 * (RF24_getObserveTX() & 0xF)))
	int arc = (-(rssi+29))/8;
	arcStats.packets++;         // # of packets sent
	arcStats.retries += arc;    // # of retries required
    arcStats.success = 
        arcStats.packets ? 
            (100uL * arcStats.packets) / (arcStats.packets + arcStats.retries) 
            : 100;
}


/**
 * @brief Reset all statistics counters to zero. Do this every hour or so
 * 
 */
void initStats()
{
	memset( nMessages, 0, sizeof(nMessages));
	memset( &rxtxStats, 0, sizeof(rxtxStats) );
    memset( &arcStats, 0, sizeof arcStats );
    t_last_clear = time(nullptr);
}


/**
 * @brief Send JSON-esque message with error statistics, then reset counters.
 * Error statistics include # of packets sent, # of retries required, success rate
 * Call this once per hour or so
 * 
 * @return const char* pointer to string sent to MySensors, like "{P:100;R:10;S:90}"
 *
 * Success rate:
 * 5 packets 0 retries = 100%
 * 5 packets 5 retries = 50%
 * 5 packets 20 retries = 20%
 */
const char* reportArcStatistics()
{
	//              				    1...5...10...15...20...25 max payload
	//				                    |   |    |    |    |    |
	static char payload[26];	//      {P:65535;R:65535;S:100}
	snprintf(payload, sizeof payload, "{P:%u,R:%u,S:%u}",
        arcStats.packets, arcStats.retries, arcStats.success );

    //memset( &arcStats, 0, sizeof arcStats );
	arcMessage.setSensor(SENSOR_ID_ARC).setType(V_TYPE_ARC);
	send(arcMessage.set(payload));
    collectArcStatistics();
	return payload;
}

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region OTA

#ifdef USE_OTA

void setupOTA()
{
//----- configure Over-The-Air updates
	ArduinoOTA.setPort( OTA_PORT );
	ArduinoOTA.setPassword( OTA_PASSWORD );
    ArduinoOTA.setHostname( ETH.getHostname());
    
	ArduinoOTA.onStart([]() {
		Serial.println("ArduinoOTA start");
	});
	ArduinoOTA.onEnd([]() {
		Serial.println("\nArduinoOTA end");
	});
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
	});
	ArduinoOTA.onError([](ota_error_t error) {
		Serial.printf("Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR) {
			Serial.println("Auth Failed");
		} else if (error == OTA_BEGIN_ERROR) {
			Serial.println("Begin Failed");
		} else if (error == OTA_CONNECT_ERROR) {
			Serial.println("Connect Failed");
		} else if (error == OTA_RECEIVE_ERROR) {
			Serial.println("Receive Failed");
		} else if (error == OTA_END_ERROR) {
			Serial.println("End Failed");
		}
	});
	ArduinoOTA.begin();
}

#endif // USE_OTA

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region Webserver

#ifdef USE_HTTP 

#ifdef OPERATE_AS_REPEATER
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>%TITLE%</title>
  <style>
    body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; line-height: 1.05; }
    table { border-collapse: collapse; }
    td { text-align: right; border: 1px solid #777777; padding: 4px; }
    button { margin: 5px; padding:10px; min-height:20px; min-width: 80px; float:left; }
    .mph { color: #606060; font-size:smaller; }
  </style>
</head>
<body>
  <h2>%TITLE%</h2>
  <p>
    IP:<b>%IPADDR%</b>&ensp;
    Name:<b>%HOSTNAME%</b>&ensp;
    Node:<b>%NODEID%</b>&ensp;
    Parent:<b>%PARENT%</b>&ensp;
    Power:<b>%POWER%</b>
  </p>  
  <p>
    ARC <b>%SUCCESS%</b>%% success,&ensp;<b>%PACKETS%</b> packets,&ensp;<b>%RETRIES%</b> retries.&emsp;
  </p>
  <p>
    Node: rx:<b>%NRX%</b>&emsp;tx:<b>%NTX%</b>&emsp;err:<b>%NERR%</b>&emsp;
    Time: %NOW%
  </p>
  <p>%TABLE%</p>
  <p>
   <form action="/clear"><button type="submit">Clear</button></form>
   <form action="/reboot"><button type="submit">Restart</button></form>
  </p>
</body>
</html>
)rawliteral";
#endif // OPERATE_AS_REPEATER

#ifdef OPERATE_AS_GATEWAY
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>%TITLE%</title>
  <style>
    body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }
    table { border-collapse: collapse; }
    td { text-align: right; border: 1px solid #777777; padding: 4px; }
    button { margin: 5px; padding:10px; min-height:20px; min-width: 80px; float:left; }
    .mph { color: #606060; font-size:smaller; }
  </style>
</head>
<body>
  <h2>%TITLE%</h2>
  <p>IP:<b>%IPADDR%</b>&emsp;Name:<b>%HOSTNAME%</b></p>  
  <p>
    Node: rx:<b>%NRX%</b>&emsp;tx:<b>%NTX%</b>&emsp;err:<b>%NERR%</b><br/>
    Gateway: rx:<b>%NGWRX%</b>&emsp;tx:<b>%NGWTX%</b>
  </p>
  <p>%TABLE%</p>
  <form action="/clear"><button type="submit">Clear</button></form>
  <form action="/reboot"><button type="submit">Restart</button></form>
  <p>%NOW%</p>
</body>
</html>
)rawliteral";
#endif // OPERATE_AS_GATEWAY

/**
 * @brief Convert unsigned int to string
 * 
 * @param u 
 * @return String 
 */
String utos( unsigned u )
{
    static char buf[10];
    utoa(u,buf,10);
    return String(buf);
}


/**
 * @brief Generate one HTML table row, #of messages received from nodes (y)..(y+9)
 * 
 * @param y 
 * @return String 
 */
String make_table_row(unsigned y, time_t nSecsElapsed)
{
    unsigned x, totalMsgs, MsgsPerHour;

    String s = "<tr><th>" + utos(y) + ":</td>";
    for (x=0; x<10; x++) {
        totalMsgs = nMessages[y + x];
        if (nSecsElapsed)
            MsgsPerHour = (totalMsgs * 3600uL) / nSecsElapsed;
        s += "<td>";
        if (totalMsgs > 0) {
            s += "<b>" + utos(totalMsgs) + "</b>";
            if (nSecsElapsed)
                s += "&ensp;<span class='mph'>" + utos(MsgsPerHour) + "/h</span>";
        }
        s += "</td>";
    }
    s += "</tr>\n";
    return s;
}


/**
 * @brief Generate HTML table with statistics (# of messages received per node)
 * 
 * @return String 
 */
String make_table() 
{
    String s;
    unsigned x,y;
    time_t nSecsElapsed = time(nullptr) - t_last_clear;

    s = "<table><tr><th> </th>";
    for (x=0; x<10; x++) s += "<th>&ensp;+" + utos(x) + "</th>";
    s += "</tr>\n";
    s += make_table_row(0,nSecsElapsed);
    s += make_table_row(20,nSecsElapsed);
    for (y=100; y<200; y+=10) {
        s += make_table_row(y,nSecsElapsed);
    }
    s += "</table>";
    return s;
}


/**
 * @brief Poor man's templating engine: replace keywords with content
 * 
 * @param var       the keyword that was enclosed in %...%
 * @return String   the replacement
 */
String processor(const String& var)
{
    if (var.length()==0) return "%";

    //----- static device information
    if (var=="IPADDR") return ETH.localIP().toString();
    if (var=="HOSTNAME") return ETH.getHostname();
    if (var=="NODEID") return String(MY_NODE_ID);
    if (var=="VERSION") return String(VERSION);
    if (var=="PARENT") return String(transportGetParentNodeId());
    //----- configuration
    if (var=="POWER") return String(MY_RF24_PA_LEVEL);

    //-----indication-based counts
    if (var=="NRX") return String(rxtxStats.nRx);
    if (var=="NTX") return String(rxtxStats.nTx);
    if (var=="NGWRX") return String(rxtxStats.nGwRx);
    if (var=="NGWTX") return String(rxtxStats.nGwTx);
    if (var=="NERR") return String(rxtxStats.nErr);
    //----- ARC statistics
    if (var=="PACKETS") return String(arcStats.packets);
    if (var=="RETRIES") return String(arcStats.retries);
    if (var=="SUCCESS") return String(arcStats.success);

    //----- general information
    if (var=="TITLE") return FRIENDLY_PROJECT_NAME ;
    if (var=="NOW") {
#ifdef USE_NTP
    	time_t epoch = ntpClient.getEpochTime();
#else
        time_t epoch = time(nullptr);
#endif
        strftime(msgbuf, sizeof msgbuf, "%d.%m.%Y %H:%M:%S", localtime(&epoch));
        return String(msgbuf);
    }
    //----- the biggie: table of messages vs node id
    if (var=="TABLE") return make_table();
    return String();
}


#define CHAR_BEGIN_VAR '%'
#define CHAR_END_VAR '%'

/**
 * @brief Poor man's templating engine: find all keywords 
 * 
 * @param tpl      the HTML with embedded keywords enclosed in %...%
 * @return String  final HTML
 */
static String process( const String& tpl )
{
    //Serial.printf("From:\n%s\n",tpl.c_str());
    String res = "";
    int p1,p2;

    p1 = tpl.indexOf(CHAR_BEGIN_VAR);
    p2 = 0;
    while (p1 != -1) {
        res += tpl.substring(p2,p1);
        p2 = tpl.indexOf(CHAR_END_VAR,p1+1);
        if (p2 != -1) {
            res += processor( tpl.substring(p1+1,p2) );
        }
        p1 = tpl.indexOf(CHAR_BEGIN_VAR,p2+1);
        p2++;
    }
    res += tpl.substring(p2);
    //Serial.printf("To:\n%s\n",res.c_str());
    return res;
}


 #ifdef USE_ASYNC_WEBSERVER
  void setupHTTPServer()
  {
    // Route for root / web page
    httpServer.on( "/", HTTP_GET, [](AsyncWebServerRequest *request) {
        log_i("HTTP '/'");
        request->send_P(200, "text/html", index_html, processor);
    });
    httpServer.on("/clear", HTTP_GET, [] (AsyncWebServerRequest *request) {
        log_i("HTTP '/clear'");
        initStats();
        request->redirect("/");
    });
    httpServer.on("/reboot", HTTP_GET, [] (AsyncWebServerRequest *request) {
        log_i("HTTP '/reboot'");
        request->redirect("/");
        ESP.restart();
    });
    httpServer.onNotFound( [] (AsyncWebServerRequest *request) {
        log_e("HTTP not found");
        request->send(404, "text/plain", "not found");
    });
    // Start server
    httpServer.begin();
  }
 #else // USE_ASYNC_WEBSERVER
  void setupHTTPServer()
  {
    // Route for root / web page
    httpServer.on( "/", HTTP_GET, []() {
        log_i("HTTP '/'");
        httpServer.send(200, "text/html", process(index_html));
    });
    httpServer.on("/clear", HTTP_GET, [] () {
        log_i("HTTP '/clear'");
        initStats();
        httpServer.sendHeader("Location", "/",true);  
        httpServer.send(302, "text/plain", "");
    });
    httpServer.on("/reboot", HTTP_GET, [] () {
        log_i("HTTP '/reboot'");
        httpServer.sendHeader("Location", "/",true);  
        httpServer.send(302, "text/plain", "");
        ESP.restart();
    });
    httpServer.onNotFound( [] () {
        log_e("HTTP not found");
        httpServer.send(404, "text/plain", "not found");
    });
    // Start server
    httpServer.begin();
  }
 #endif // USE_ASYNC_WEBSERVER
#endif // USE_HTTP

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region Network event handler

void WiFiEvent(WiFiEvent_t event) 
{
	switch (event) { 
	case ARDUINO_EVENT_WIFI_STA_START:
	case ARDUINO_EVENT_ETH_START:
        char buf[14];
        uint8_t mac[6];
        ETH.macAddress(mac);
        snprintf(buf, sizeof buf, "ESP32-%02hX%02hX%02hX", mac[3], mac[4], mac[5]);
		ETH.setHostname(buf);
		Serial.printf(
            "... " IF_NAME " started, set hostname to '" ANSI_BOLD "%s" ANSI_RESET "'\n"
            ,
            buf);
		break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
	case ARDUINO_EVENT_ETH_CONNECTED:
		Serial.printf(
            "... " IF_NAME " " ANSI_BRIGHT_GREEN "Connected" ANSI_RESET
#ifdef USE_ETHERNET
            " %s, %hu Mbps\n"
            ,
            ETH.fullDuplex() ? "FULL_DUPLEX" : "",
            ETH.linkSpeed()
#endif
            );
		break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
	case ARDUINO_EVENT_ETH_GOT_IP:
		Serial.printf("... " IF_NAME " "
            "MAC: " ANSI_BOLD "%s" ANSI_RESET ", "
            "IPv4: " ANSI_BOLD "%s" ANSI_RESET "\n"
            , 
            ETH.macAddress().c_str(), 
            ETH.localIP().toString().c_str()
            );
		break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
	case ARDUINO_EVENT_ETH_DISCONNECTED:
		Serial.println("... " IF_NAME " " ANSI_BRIGHT_RED "Disconnected" ANSI_RESET);
		break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
	case ARDUINO_EVENT_ETH_STOP:
		Serial.println("... " IF_NAME " Stopped");
		break;
	default:
		break;
	}
}

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region MySensors standard functions


/**
 * @brief Send information about sketch and sensors
 * (Standard MySensors function to be implemented in application)
 * 
 */
void presentation()
{
	static char rev[] = "$Rev: 1671 $";
	char* p = strchr(rev+6,'$');
	if (p) *p=0;

	// Present locally attached sensors here
	sendSketchInfo( "MyGwESP32-ETH", rev+6 );
    collectArcStatistics();
	//              				    1...5...10...15...20...25 max payload
	//				                    |   |    |    |    |    |
	present(SENSOR_ID_ARC, S_CUSTOM, F("ARC stats (JSON)") );
	collectArcStatistics();    
    delay(10);
    present(SENSOR_ID_CMND, S_INFO, F("Commands"));
	collectArcStatistics();    
    delay(10);
#ifdef USE_DS18B20
	present( SENSOR_ID_TEMP, S_TEMP, F("Temperature [°C]" ));
    collectArcStatistics();
#endif
}


/**
 * @brief React to various events reported by MySensors
 * (Standard MySensors function to be implemented in application)
 * 
 * @param ind 
 */
void indication( const indication_t ind )
{
	switch (ind) {
		case INDICATION_TX:		rxtxStats.nTx++; break;
		case INDICATION_RX: 	rxtxStats.nRx++; break;
		case INDICATION_GW_TX:	rxtxStats.nGwTx++; break;
		case INDICATION_GW_RX: 	rxtxStats.nGwRx++; break;
		case INDICATION_ERR_TX:	rxtxStats.nErr++; break;
		default: 				break;
	}
}


/**
 * @brief Callback when message is received
 * (Standard MySensors function to be implemented in application).
 * 
 * @param message 
 */
void receive(const MyMessage &message)
{
	char buf[32];

#ifdef OPERATE_AS_GATEWAY
	nMessages[ message.getSender() ]++;
#endif

	// We only expect one type of message from controller. But we better check anyway.
	if (message.isAck()) {
		//Serial.println("Ack from gateway");
		return;
	}

    Serial.printf("Msg Type:%d Sensor:%d Payload:%s\n",
		message.type,message.sensor,message.getString());

	if (										// MQTT: my/cmnd/25/96/1/0/47   text
		message.sensor == SENSOR_ID_CMND && 
		message.type == V_TEXT 
		) {
		// parse command
        Serial.printf("Execute command '%s'\n",message.getString());

	} else {
        Serial.println("unknown message\r\n");
    }
}


#ifdef OPERATE_AS_REPEATER
/**
 * @brief Let user code peek at incoming message _before_ it is forwarded to parent.
 * Defined in my modified MySensors library, as a "weak" function, i.e. the library 
 * will call this if it is defined in user code, or else quietly ignore it.
 * 
 * @param message 
 */
 void previewMessage(const MyMessage &message) 
 {
	nMessages[ message.getSender() ]++;
 }


/**
 * @brief Let user code peek at message _after_ it has been forwarded to parent.
 * Defined in my modified MySensors library, as a "weak" function, i.e. the library 
 * will call this if it is defined in user code, or else quietly ignore it.
 * 
 * @param message 
 */
 void afterTransportMessage(const MyMessage &message)
 {
    collectArcStatistics();
 }

#endif

//---------------------------------------------------------------------
#pragma endregion
//=====================================================================
#pragma region Local sensors
#ifdef USE_DS18B20

/**
 * @brief Initialize DS18B20 temperature sensor
 * 
 * @return true if sensor found and initialized
 * @return false if no sensor found
 */
bool initTemperature()
{
    bool res = ds18b20.begin();
    if (res) {
        hasDS18B20 = true;
        uint8_t addr[8];
        ds18b20.getAddress(addr);
        Serial.printf( 
            "Found DS18B20 at %02hX:%02hX:%02hX:%02hX:%02hX:%02hX:%02hX:%02hX\n",
            addr[0],addr[0],addr[2],addr[2],addr[3],addr[4],addr[5],addr[6],addr[7]
        );
        ds18b20.requestTemperatures();
    } else {
        Serial.println("No DS18B20 found");
    }
    return res;
}


void reportTemperature()
{
    if (hasDS18B20) {
        float t = ds18b20.getTempC();
        if (int(t) == 85) return;
        ds18b20.requestTemperatures();
        send(msgTemperature.set(t,1));
        collectArcStatistics();
        Serial.printf("Temperature " ANSI_BOLD "%.1f" ANSI_RESET "°C\n",t);
    }
}

#endif // #ifdef USE_DS18B20
//---------------------------------------------------------------------
#pragma endregion

/**
 * @brief Early hardware initialization, called by MySensors library 
 * well before `setup()`. 
 * (Standard MySensors function to be implemented in application)
 */
void preHwInit(void)
{
	Serial.begin(115200,SERIAL_8N1);
    delay(3000);
	Serial.setDebugOutput(true);
	Serial.println(">>>>> begin preHwInit");

	WiFi.onEvent(WiFiEvent);
    LED_INIT;
    TURN_LED_ON;

#ifdef USE_ETHERNET
    WiFi.mode(WIFI_OFF);
 #if ESP_ARDUINO_VERSION_MAJOR==3
    bool res = ETH.begin(
                    ETH_PHY_LAN8720,
                    1, //ESP_ETH_PHY_ADDR_AUTO,
                    PIN_ETH_PHY_MDC,
                    PIN_ETH_PHY_MDIO,
                    PIN_ETH_PHY_POWER,
                    ETH_CLOCK_GPIO0_IN
                );
 #else // ESP_ARDUINO_VERSION_MAJOR==3
    bool res = ETH.begin( 
                    1, //ESP_ETH_PHY_ADDR_AUTO,
                    PIN_ETH_PHY_POWER,
                    PIN_ETH_PHY_MDC,
                    PIN_ETH_PHY_MDIO,
                    ETH_PHY_LAN8720,
                    ETH_CLOCK_GPIO0_IN
                ); 
 #endif // ESP_ARDUINO_VERSION_MAJOR==3
#else // USE_ETHERNET
 #ifdef OPERATE_AS_REPEATER
    WiFi.mode(WIFI_STA);
    WiFi.begin(MY_WIFI_SSID,MY_WIFI_PASSWORD);
 #endif // OPERATE_AS_REPEATER
#endif // USE_ETHERNET
    delay(3000);
    TURN_LED_OFF;
	Serial.println(">>>>> end preHwInit");
    Serial.flush();
}


void setup() 
{
    int rtc_reset_reason = rtc_get_reset_reason(0);    

//----- Telnet

	Serial.println("---------- begin setup()");
    Serial.println(FRIENDLY_PROJECT_NAME " " SVN_REV);
    Serial.println(VERSION);
    Serial.printf("Reset reason %s\n",reset_reasons[rtc_reset_reason]);

//----- report environment

    Serial.printf( ANSI_BOLD "%s" ANSI_RESET, 
        ESP.getChipModel() );
    Serial.printf(" at " ANSI_BOLD "%d" ANSI_RESET " MHz",
        (int)ESP.getCpuFreqMHz());
    Serial.printf(" (APB:%d)",
        getApbFrequency()/1000000 );
    Serial.printf("  Flash:" ANSI_BOLD "%d" ANSI_RESET "K", 
        (int)(ESP.getFlashChipSize() / 1024));
    Serial.printf("  Heap:" ANSI_BOLD "%d" ANSI_RESET, 
        ESP.getFreeHeap() );
    Serial.printf("  Core:" ANSI_BOLD "%s" ANSI_RESET, 
        ESP.getSdkVersion() );//esp_get_idf_version() );
    Serial.println();

    String sConfig = "Config: ";
#ifdef OPERATE_AS_REPEATER
    sConfig += "repeater, ";
#endif
#ifdef OPERATE_AS_GATEWAY
    sConfig += "gateway, ";
#endif
#ifdef USE_ETHERNET
    sConfig += "Ethernet, ";
#else
    sConfig += "WiFi, ";
#endif
#ifdef USE_HSPI
    sConfig += "HSPI, ";
#else
    sConfig += "VSPI, ";
#endif
#ifdef MY_SEPARATE_PROCESS_TASK
    sConfig += "2 tasks, ";
#else
    sConfig += "1 task, ";
#endif
#ifdef USE_ASYNC_WEBSERVER
    sConfig += "async webserver, ";
#else
    sConfig += "normal webserver, ";
#endif

    Serial.println( sConfig );

//----- locally attached sensors

    initStats();

//----- Ethernet

    const char* hostname = ETH.getHostname();
    snprintf(msgbuf,sizeof(msgbuf), "MAC:%s  IP:%s  hostname:%s",
        ETH.macAddress().c_str(),
        ETH.localIP().toString().c_str(),
        hostname ? hostname : "(unknown)"
        );
    String sNetwork(msgbuf);

//----- NTP

    ntpClient.begin();
    ntpClient.forceUpdate();
    time_t now = ntpClient.getEpochTime();
    char snow[50];
    strftime(snow,sizeof(snow),"%F %T",localtime(&now));
    log_i("initialized NTP, current time %s", snow);

//----- Syslog

#ifdef USE_SYSLOG
    syslog.logMask(LOG_UPTO(LOG_INFO));
    syslog.deviceHostname(ETH.getHostname());
    syslog.logf(LOG_NOTICE,"Starting %s, reset reason '%s'", 
        FRIENDLY_PROJECT_NAME, reset_reasons[rtc_reset_reason] );
	syslog.log(LOG_NOTICE, VERSION );
	syslog.log(LOG_NOTICE, sNetwork );
    syslog.log( LOG_NOTICE, sConfig );
    log_i("initialized Syslog");
#endif

//----- Webserver

#ifdef USE_HTTP
    setupHTTPServer();
    log_i("initialized HTTP server");
#endif

//----- OTA

#ifdef USE_OTA
    setupOTA();
    log_i("initialized OTA");
#endif

//----- Temperature sensor

#ifdef USE_DS18B20
    if (initTemperature()) {
        reportTemperature();
    }
#endif

//----- done

    const char* arc = reportArcStatistics();
    log_i("ARC: %s",arc);
#ifdef USE_SYSLOG
    syslog.logf(LOG_NOTICE,"ARC: %s", arc );
#endif

	Serial.println("---------- end setup()");
    Serial.flush();

/*
    esp_pm_config_esp32_t cfg;
    int err = esp_pm_get_configuration(&cfg);
    if (err==ESP_OK)
        Serial.printf("ESP32 PM: max=%d min=%d light sleep %s\n",
            cfg.max_freq_mhz,
            cfg.min_freq_mhz,
            cfg.light_sleep_enable ? "ON" : "OFF"
        );
*/
}


void loop() 
{
    unsigned long t_now = millis();
    
#if defined( USE_HTTP) && !defined(USE_ASYNC_WEBSERVER)
    httpServer.handleClient();
#endif

#ifdef USE_OTA
    ArduinoOTA.handle();
#endif

#ifdef USE_NTP
    ntpClient.update();    
#endif

#ifdef USE_DS18B20
    // report module temperature
	static unsigned long t_lastTemperatureReport=0;
	if ((unsigned long)(t_now - t_lastTemperatureReport) > REPORT_TEMPERATURE_INTERVAL) {
		t_lastTemperatureReport=t_now;
        reportTemperature();
    }
#endif

#ifdef OPERATE_AS_REPEATER
	static unsigned long t_lastHelloReport=0;
	if ((unsigned long)(t_now - t_lastHelloReport) > REPORT_HELLO_INTERVAL) {
		t_lastHelloReport=t_now;
        send(msgHello.set((uint32_t)t_now));
        collectArcStatistics();
        Serial.println(reportArcStatistics());
    }
#endif

    // every now and then, report ARC statistics ("pseudo-RSSI")
	static unsigned long t_lastReport=0;
	if ((unsigned long)(t_now - t_lastReport) > MIN_REPORT_INTERVAL) {
		t_lastReport=t_now;
        wait(1);
        const char* arc = reportArcStatistics();
        log_i("ARC: %s",arc);
        //initStats();
	}

#ifdef LED_BUILTIN
    // blink LED
    static unsigned t_last_LED=0;
	if ((unsigned long)(t_now - t_last_LED) > 50uL) {
		t_last_LED=t_now;
        unsigned t = t_now & 0x3FF; // count up to ~1000ms
        if (t < 50) TURN_LED_ON; else TURN_LED_OFF;
    }
#endif
}
