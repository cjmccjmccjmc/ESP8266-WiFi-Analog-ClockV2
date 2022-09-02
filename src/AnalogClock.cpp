//--------------------------------------------------------------------------
// Analog Clock
// Pulses the Lavet clock motor to keep the clock in sync with local time.
// Stores the clock's hour hand, minute hand and second hand positions in EERAM.
//--------------------------------------------------------------------------

#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <ESPNtpClient.h>
#include <Ticker.h>
#include <ESP8266WebServer.h>
#include "I2C_eeprom.h"
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include "FS.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static const uint8_t COIL1 = D3; // output to clock's lavet motor coil
static const uint8_t COIL2 = D7; // output to clock's lavet motor coil


const unsigned long PULSETIME = 35;             // 30 millisecond pulse for the lavet motor, less can be better

// Prevent the clock advancing too fast and overloading the gears, the following values are how fast to advance in once hour:
// 200 5 hours
// 400 2.5hr
// 500 2 hrs
const unsigned long MIN_PULSE_GAP_MS = 400;  


const char* AP_NAME = "ClockSetupAP";
const char* MDNS_HOSTNAME = "myclock";
const int MAX_DNS_ATTEMPTS = 10;


const char* CONFIG_URL = "/config";
const char* CLOCK_URL = "/clock";
const char* CONFIG_API = "/api/config";
const char* TZ_JSON = "/timezone.json";
const char* APP_JS = "/app.js";

const uint16_t HOUR = 0x0000;                         // address in EERAM for analogClkHour
const uint16_t MINUTE = HOUR+1;                         // address in EERAM for analogClkMinute
const uint16_t SECOND = HOUR+2;                         // address in EERAM for analogClkSecond
const uint16_t WEEKDAY = HOUR+3;                         // address in EERAM for analogClkWeekday
const uint16_t DAY = HOUR+4;                         // address in EERAM for analogClkDay
const uint16_t MONTH = HOUR+5;                         // address in EERAM for analogClkMonth
const uint16_t YEAR = HOUR+6;                         // address in EERAM for analogClkYear
const uint16_t CHECK1 = HOUR+7;                         // address in EERAM for 1st check byte 0xAA
const uint16_t CHECK2 = HOUR+8;                         // address in EERAM for 2nd check byte 0x55

const uint16_t LAST_EERAM_POS = CHECK2;

const char* NTPSERVERNAME = "time.google.com";
// const char* NTPSERVERNAME = "0.us.pool.ntp.org";
// const char* NTPSERVERNAME = "time.nist.gov";
// const char* NTPSERVERNAME = "time.windows.com";
// const char* NTPSERVERNAME = "time-a-g.nist.gov";     // NIST, Gaithersburg, Maryland


const char* DEFAULT_TIMEZONE = "UTC0";
const char* CONFIG_ZONE = "zonevalue";
const char* CONFIG_REGION = "region";
const char* CONFIG_CITY = "city";


const static size_t JSON_CAPACITY = 1024;
const static size_t MAX_CONFIG_FILE_SIZE = JSON_CAPACITY;
StaticJsonDocument<JSON_CAPACITY> doc;

const char *CONFIG_FILENAME = "/config.json";

// Set the time for alignment sync with NTP server, since this is a wallclock, kept it within half a second. 
const float SYNC_ACCURACY_SECONDS = 0.5;
const long SYNC_ACCURACY_US = SYNC_ACCURACY_SECONDS * 1000000;
const int THRESHOLD_HOUR_BUMP = 12;
String timezone = "";

// EERAM eeRAM(0x50);
I2C_eeprom ee(0x50, I2C_DEVICESIZE_24LC16);

ESP8266WebServer analogClkServer(80);
Ticker pulseTimer,clockTimer;
String lastSyncTime = "";
String lastSyncDate = "";
boolean syncEventTriggered = false;             // true if an NTP time sync event has been triggered
boolean setupComplete = false;
boolean printTime = false;
boolean advanceClock = false;
byte analogClkHour=0;
byte analogClkMinute=0;
byte analogClkSecond=0;
byte analogClkWeekday=0;
byte analogClkDay=0;
byte analogClkMonth=0;
byte analogClkYear=0;
unsigned long lastPulseTime = 0;

void handleRoot();
void handleSave();
void pulseOff();
void pulseCoil();
void checkClock();
void updateClock();

// Forward declarations
void syncNTPEventFunction(NTPEvent_t);
String getUpTime();

//--------------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------------
void setup() {


   //--------------------------------------------------------------------------
   // configure hardware...
   //--------------------------------------------------------------------------

   ee.begin();
   pinMode(COIL1,OUTPUT);
   pinMode(COIL2,OUTPUT);

   digitalWrite(COIL1,LOW);
   digitalWrite(COIL2,LOW);

   //--------------------------------------------------------------------------      
   // print the banner... 
   //--------------------------------------------------------------------------
   Serial.begin(115200);  
   unsigned long waitTime = millis()+500;
   while(millis() < waitTime)yield();                          // wait one half second for the serial port
   Serial.println("\n\nAnalog Clock");
   Serial.printf("Sketch size: %u\n",ESP.getSketchSize());
   Serial.printf("Free size: %u\n",ESP.getFreeSketchSpace());
   Serial.print(ESP.getResetReason());
   Serial.println(" Reset");

  // Setup the File System.
  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    delay(5);
    ESP.reset();
  }


  // Wifi setup.
  WiFiManager wifiManager; 
  while ( !wifiManager.autoConnect(AP_NAME) ) {
    ; // keep looping until conenction made.
  }

   //--------------------------------------------------------------------------
   // prompt for web configuration...
   //--------------------------------------------------------------------------
   while (Serial.available()) Serial.read();                  // empty the serial input buffer
   Serial.println("Press any key for web configuration.");
   byte count = 10;
   byte lastSec = second();                      
   boolean inputAvail = false;   
   while (count && !inputAvail) {
      byte thisSec = second();
      if (lastSec != thisSec) {
         lastSec = thisSec;
         Serial.printf("Waiting for %u seconds\n\r",count--);         
      }
      inputAvail = Serial.available();
   }
   byte c = Serial.read();
   //--------------------------------------------------------------------------
   // start the web server
   //--------------------------------------------------------------------------

   analogClkServer.on("/", handleRoot);
   analogClkServer.serveStatic(CONFIG_URL, LittleFS, "/config.html");
   analogClkServer.serveStatic(CLOCK_URL, LittleFS, "/main.html");
   analogClkServer.serveStatic(TZ_JSON, LittleFS, "/tz.json");
   analogClkServer.serveStatic(APP_JS, LittleFS, "/app.js");

   analogClkServer.serveStatic(CONFIG_API, LittleFS, CONFIG_FILENAME);
   analogClkServer.on(CONFIG_API,HTTP_PUT, handleSave);

   analogClkServer.begin();

  // Publish mDNS
  int mdnsAttempt = 0;
  while (!MDNS.begin(MDNS_HOSTNAME, WiFi.localIP()) && mdnsAttempt <= MAX_DNS_ATTEMPTS) {
    Serial.println(F("Error setting up MDNS responder!"));
    delay(1000);
    mdnsAttempt++;
  }
  MDNS.addService("http", "tcp", 80);


  //--------------------------------------------------------------------------
  // connect to the NTP server, do this first so year, month, day gets setup.
  //--------------------------------------------------------------------------
   NTP.setMinSyncAccuracy(SYNC_ACCURACY_US);
   NTP.onNTPSyncEvent(syncNTPEventFunction);
   if (!NTP.setInterval(10,600)) Serial.println("Problem setting NTP interval.");
   NTP.begin(NTPSERVERNAME,true);                        // start the NTP client

   int waitCount = 500;
   Serial.print("Waiting for sync with NTP server");   
   while (timeStatus() != timeSet) {                           // wait until the the time is set and synced
      waitTime = millis()+500;
      while(millis() < waitTime)yield();                       // wait one half second       
      Serial.print(".");                                       // print a "." every half second
      --waitCount;
      if (waitCount==0) ESP.restart();                         // if time is not set and synced after 50 seconds, restart the ESP8266
   }


  //--------------------------------------------------------------------------
  // read analog clock values stored in EERam  
  //--------------------------------------------------------------------------

  if((ee.readByte(CHECK1)==0xAA)&&(ee.readByte(CHECK2)==0x55)&&(!inputAvail)){
    analogClkHour = ee.readByte(HOUR);
    analogClkMinute = ee.readByte(MINUTE);
    analogClkSecond = ee.readByte(SECOND);
    analogClkWeekday = ee.readByte(WEEKDAY);
    analogClkDay = ee.readByte(DAY);
    analogClkMonth = ee.readByte(MONTH);
    analogClkYear = ee.readByte(YEAR);      

    Serial.println("\nReading values from Config.");
    File configFile = LittleFS.open(CONFIG_FILENAME, "r");
    bool failedToReadConfig = false;

    if (!configFile) {
      Serial.println("Failed to open config file");
      failedToReadConfig = true;
    } else {

      size_t size = configFile.size();
      if (size > MAX_CONFIG_FILE_SIZE) {
        Serial.println("Config file size is too large");
        failedToReadConfig = true;
      } else {

        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        StaticJsonDocument<MAX_CONFIG_FILE_SIZE> json;
        DeserializationError error = deserializeJson(json, buf.get());
        if (error) {
          Serial.println("Failed to parse config file");
          failedToReadConfig = true;
        } else {
          // Can read data

          timezone = json[CONFIG_ZONE].as<String>().c_str();
          failedToReadConfig = false;
        }
      }
    }
    

    if (failedToReadConfig) {
      Serial.println("Using Default values for timezone");
      timezone = DEFAULT_TIMEZONE;
    }

    // Set timezone now, so takes into account either the set config or loaded config. 
    NTP.setTimeZone(timezone.c_str());

    setupComplete = true;      
  }
   //--------------------------------------------------------------------------   
   // get values from setup web page...   
   //--------------------------------------------------------------------------
   else {
      for (byte i=0;i<=LAST_EERAM_POS;i++) {
         ee.writeByte(HOUR+i,0);                                // clear eeram     
      }

      if ( mdnsAttempt <= MAX_DNS_ATTEMPTS) {
        Serial.printf("\nBrowse to http://%s.local (http://%s) to set up the analog clock.\n\r",MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
      } else {
        Serial.printf("\nBrowse to http://%s to set up the analog clock.\n\r",WiFi.localIP().toString().c_str());
      }

      while(!setupComplete) {
        analogClkServer.handleClient();
        MDNS.update();
      }

   }  


  Serial.printf("%02d/%02d/%04d %02d:%02d:%02d  %02d/%02d/%04d %02d:%02d:%02d\n\r",
  day(), month(), year(), hour(),minute(),second(), 
  analogClkDay, analogClkMonth, analogClkYear, analogClkHour,analogClkMinute,analogClkSecond);                      


  // Check if need to bump the internal clock hour value foward 12 hours. 
  if ( abs(analogClkHour - hour()) >= THRESHOLD_HOUR_BUMP ) {
    analogClkHour = (analogClkHour + 12) % 24; 
    Serial.println("Bumping internal position of hour hand forward half a day");
  } 


   clockTimer.attach_ms(100,checkClock);                      // start up 100 millisecond clock timer
}

//--------------------------------------------------------------------------
// Main Loop
//--------------------------------------------------------------------------
void loop() {

    // get any characters from the serial port
    if (Serial.available()) {
      char c = Serial.read();
    }

    // If clock advance has been triggered
    // Note: the has been moved out of the Ticker as EERAM update is runs too long for use in a ticker callback.
    if ( advanceClock ) {
      updateClock();
      advanceClock = false;
    }

    // print analog clock and actual time if values have changed  
    if (printTime) {                                           // when the analog clock is updated...
       printTime = false;
       Serial.printf("%02d/%02d/%04d %02d:%02d:%02d  %02d/%02d/%04d %02d:%02d:%02d\n\r",
       day(), month(), year(), hour(),minute(),second(), 
       analogClkDay, analogClkMonth, analogClkYear, analogClkHour,analogClkMinute,analogClkSecond);                      
    }

    // handle requests from the web server  
    analogClkServer.handleClient();                                 // handle requests from status web page

    // Handle mDNS lookups
    MDNS.update();


}


//------------------------------------------------------------------------
// Ticker callback that turns off the pulse to the analog clock Lavet motor
// after 30 milliseconds.
//-------------------------------------------------------------------------
void pulseOff() {
   digitalWrite(COIL1,LOW);
   digitalWrite(COIL2,LOW);
}

//--------------------------------------------------------------------------
// pulse the clock's Lavet motor to advance the second hand.
// The Lavet motor requires polarized control pulses. If the control pulses are inverted,
// the clock appears to run one second behind. To remedy the problem, invert the polarity
// of the control pulses. This is easily done by exchanging the wires connecting the Lavet motor.
//--------------------------------------------------------------------------
void pulseCoil() {
   if ((analogClkSecond%2)==0){                 // positive motor pulse on even seconds
      digitalWrite(COIL1,HIGH);
      digitalWrite(COIL2,LOW);
   }
   else {                                       // negative motor pulse on odd seconds
      digitalWrite(COIL1,LOW);
      digitalWrite(COIL2,HIGH);
   }
   lastPulseTime = millis();
   pulseTimer.once_ms(PULSETIME,pulseOff);      // turn off pulse after 30 milliseconds...
}

//--------------------------------------------------------------------------
// Ticker callbacks run every 100 milliseconds that checks if the analog clock's 
// second hand needs to be advanced
//--------------------------------------------------------------------------
void checkClock() {
  time_t analogClkTime = makeTime({analogClkSecond,analogClkMinute,analogClkHour,analogClkWeekday,analogClkDay,analogClkMonth,analogClkYear});
  if (analogClkTime < now()) {                    // if the analog clock is behind the actual time and needs to be advanced...

    // Only tigger an advance if less than minimum gap to prevent too fast advance.
    if ( lastPulseTime + MIN_PULSE_GAP_MS < millis() ) {
      advanceClock = true;
    }
  }
}

void updateClock() {
  pulseCoil();                                 // pulse the motor to advance the analog clock's second hand
  if (++analogClkSecond==60){                  // since the clock motor has been pulsed, increase the seconds count
      analogClkSecond=0;                        // at 60 seconds, reset analog clock's seconds count back to zero
      if (++analogClkMinute==60) {
          analogClkMinute=0;                    // at 60 minutes, reset analog clock's minutes count back to zero
          if (++analogClkHour==24) {
              analogClkHour=0;                  // at 24 hours, reset analog clock's hours count back to zero
              analogClkWeekday=weekday();       // update values
              analogClkDay=day();
              analogClkMonth=month();
              analogClkYear=year()-1970;   
              
              ee.writeByte(WEEKDAY,analogClkWeekday);// save the updated values in eeRAM
              ee.writeByte(DAY,analogClkDay);
              ee.writeByte(MONTH,analogClkMonth); 
              ee.writeByte(YEAR,analogClkYear); 
          }
      }
  }
  ee.writeByte(HOUR,analogClkHour);             // save the new values in eeRAM
  ee.writeByte(MINUTE,analogClkMinute);
  ee.writeByte(SECOND,analogClkSecond); 
  printTime = true;                             // set flag to update display
  advanceClock = false;                         // set flag that have advanced clock
}


// -------------------------------------------------------------------------
// Redirects Main page
// -------------------------------------------------------------------------

void handleRoot() {

  const char * destUrl = CLOCK_URL;
  if (! setupComplete) {
    destUrl =  CONFIG_URL;
  }

  analogClkServer.sendHeader("Location", String(destUrl), true);
  analogClkServer.send ( 302, "text/plain", "");          
}




//--------------------------------------------------------------------------
// Handles save requests.
//--------------------------------------------------------------------------
void handleSave() {

  Serial.println("Entered handle save");

  if ( setupComplete ) {
    // Return error to prevent if step. 
    analogClkServer.send ( 403, "text/plain", "Setup already complete");
    return;
  }

  if (analogClkServer.hasArg("plain") == false){ 
    analogClkServer.send(500, "text/plain", "Body not received");
    return; 
  }
  const String body = analogClkServer.arg("plain");
  const char* b = body.c_str();

  DeserializationError error = deserializeJson(doc, b);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    analogClkServer.send(500, "text/plain", "deserializeJson() failed");
    return;
  }


  JsonObject hands = doc["clockhands"];
  analogClkHour = hands["hour"];
  analogClkMinute = hands["minute"];  
  analogClkSecond = hands["second"];

  JsonObject timeZ = doc["timezone"];

  String zoneValue = timeZ["string"];
  String region = timeZ["region"];
  String city = timeZ["city"];

  // Get the now time.
  analogClkWeekday=weekday();
  analogClkDay=day();
  analogClkMonth=month();
  analogClkYear=year()-1970; 

  // save the updated values in EERam...     
  ee.writeByte(HOUR,analogClkHour); 
  ee.writeByte(MINUTE,analogClkMinute);
  ee.writeByte(SECOND,analogClkSecond);      
  ee.writeByte(WEEKDAY,analogClkWeekday);
  ee.writeByte(DAY,analogClkDay);
  ee.writeByte(MONTH,analogClkMonth); 
  ee.writeByte(YEAR,analogClkYear); 
  ee.writeByte(CHECK1,0xAA);
  ee.writeByte(CHECK2,0x55);

  // Save config values to local disk
  doc.clear();
  doc[CONFIG_ZONE] = zoneValue;
  doc[CONFIG_REGION] = region;
  doc[CONFIG_CITY] = city;

  File configFile = LittleFS.open(CONFIG_FILENAME, "w");
  if (!configFile) {
    analogClkServer.send(500, "text/plain", "Failed to open config file for writing");
    return;
  }
  serializeJson(doc, configFile);
  configFile.close();


  // Tidy up before leaving
  doc.clear();
  setupComplete = true;                               // set flag to indicate that we're done with setup   
  analogClkServer.send ( 200, "text/plain", "");
}

//--------------------------------------------------------------------------
// Returns uptime as a formatted String: Days, Hours, Minutes, Seconds
//--------------------------------------------------------------------------
String getUpTime() {
   long uptime=millis()/1000;
   int d=uptime/86400;
   int h=(uptime%86400)/3600;
   int m=(uptime%3600)/60;
   String daysStr="";
   if (d>0) (d==1) ? daysStr="1 day," : daysStr=String(d)+" days,";
   String hoursStr="";
   if (h>0) (h==1) ? hoursStr="1 hour," : hoursStr=String(h)+" hours,";
   String minutesStr="";
   if (m>0) (m==1) ? minutesStr="1 minute" : minutesStr=String(m)+" minutes";
   return daysStr+" "+hoursStr+" "+minutesStr;
}

//--------------------------------------------------------------------------
// NTP event handler
//--------------------------------------------------------------------------
void syncNTPEventFunction(NTPEvent_t e){

  if ( e.event == timeSyncd) {
    lastSyncTime = NTP.getTimeStr(NTP.getLastNTPSync());
    lastSyncDate = NTP.getDateStr(NTP.getLastNTPSync());
    Serial.print("Got NTP time: ");
    Serial.print(lastSyncTime+"  ");
    Serial.println(lastSyncDate);
    {
      time_t tempTimeT = NTP.getLastNTPSync();
      tm* tempTimeLocal = localtime(&tempTimeT);
      // Localtime() seems to return the year as year since 1900 instead of 1970 as asumed by setTime.
      setTime(tempTimeLocal->tm_hour, tempTimeLocal->tm_min, tempTimeLocal->tm_sec, tempTimeLocal->tm_mday, tempTimeLocal->tm_mon, tempTimeLocal->tm_year % 100);
    }
  } else {
    // Log non-sync events
      Serial.println(NTP.ntpEvent2str(e));
  }

}

