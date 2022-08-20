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

#include "Generated_Timezones.hpp"

static const uint8_t COIL1 = D3; // output to clock's lavet motor coil
static const uint8_t COIL2 = D7; // output to clock's lavet motor coil


const unsigned long PULSETIME = 35;             // 30 millisecond pulse for the lavet motor, less can be better

// Prevent the clock advancing too fast and overloading the gears, the following values are how fast to advance in once hour:
// 200 5 hours
// 400 2.5hr
// 500 2 hrs
const unsigned long MIN_PULSE_GAP_MS = 400;  


const char* AP_NAME = "ClockSetupAP";


const uint16_t HOUR = 0x0000;                         // address in EERAM for analogClkHour
const uint16_t MINUTE = HOUR+1;                         // address in EERAM for analogClkMinute
const uint16_t SECOND = HOUR+2;                         // address in EERAM for analogClkSecond
const uint16_t WEEKDAY = HOUR+3;                         // address in EERAM for analogClkWeekday
const uint16_t DAY = HOUR+4;                         // address in EERAM for analogClkDay
const uint16_t MONTH = HOUR+5;                         // address in EERAM for analogClkMonth
const uint16_t YEAR = HOUR+6;                         // address in EERAM for analogClkYear
const uint16_t TIMEZONE = HOUR+7;                         // address in EERAM for timezone
const uint16_t CHECK1 = HOUR+8;                         // address in EERAM for 1st check byte 0xAA
const uint16_t CHECK2 = HOUR+9;                         // address in EERAM for 2nd check byte 0x55

const char* NTPSERVERNAME = "time.google.com";
// const char* NTPSERVERNAME = "0.us.pool.ntp.org";
// const char* NTPSERVERNAME = "time.nist.gov";
// const char* NTPSERVERNAME = "time.windows.com";
// const char* NTPSERVERNAME = "time-a-g.nist.gov";     // NIST, Gaithersburg, Maryland

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
   
  // Wifi setup.
  WiFiManager wifiManager; 
  while ( !wifiManager.autoConnect(AP_NAME) ) {
    ; // keep looping until conenction made.
  }

  //--------------------------------------------------------------------------
   // connect to the NTP server...
   //--------------------------------------------------------------------------
   NTP.begin(NTPSERVERNAME,true);                        // start the NTP client

   NTP.onNTPSyncEvent(syncNTPEventFunction);
   if (!NTP.setInterval(10,600)) Serial.println("Problem setting NTP interval.");

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
   analogClkServer.on("/",handleRoot);
   analogClkServer.on("/clock",handleRoot);
   analogClkServer.on("/save",handleSave);
   analogClkServer.begin();

   //--------------------------------------------------------------------------
   // read analog clock values stored in EERam  
   //--------------------------------------------------------------------------

   if((ee.readByte(CHECK1)==0xAA)&&(ee.readByte(CHECK2)==0x55)&&(!inputAvail)){
      Serial.println("\nReading values from EERAM.");
      analogClkHour = ee.readByte(HOUR);
      analogClkMinute = ee.readByte(MINUTE);
      analogClkSecond = ee.readByte(SECOND);
      analogClkWeekday = ee.readByte(WEEKDAY);
      analogClkDay = ee.readByte(DAY);
      analogClkMonth = ee.readByte(MONTH);
      analogClkYear = ee.readByte(YEAR);      
      byte timezonePosition = ee.readByte(TIMEZONE); 
      NTP.setTimeZone(GENERATED_TZ_LOOKUP[timezonePosition]);
      setupComplete = true;      
   }
   //--------------------------------------------------------------------------   
   // get values from setup web page...   
   //--------------------------------------------------------------------------
   else {
      for (byte i=0;i<10;i++) {
         ee.writeByte(HOUR+i,0);                                // clear eeram     
      }
      Serial.printf("\nBrowse to %s to set up the analog clock.\n\r",WiFi.localIP().toString().c_str());
    
      byte lastSeconds = second();
      while(!setupComplete) {
         analogClkServer.handleClient();
      }
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
       Serial.printf("%02d:%02d:%02d  %02d:%02d:%02d\n\r",hour(),minute(),second(),analogClkHour,analogClkMinute,analogClkSecond);                      
    }

    // handle requests from the web server  
    analogClkServer.handleClient();                                 // handle requests from status web page


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

//--------------------------------------------------------------------------
// Handles requests from the setup server client.
//--------------------------------------------------------------------------
void handleRoot() {
   if (setupComplete) {
      char timeStr[12];
      sprintf(timeStr,"%02d:%02d:%02d", analogClkHour,analogClkMinute,analogClkSecond);
      analogClkServer.send(200, "text/html",
      "<!DOCTYPE HTML>"
      "<html>"
        "<head>"
          "<META HTTP-EQUIV=\"refresh\" CONTENT=\"1\">"
          "<meta content=\"text/html; charset=utf-8\">"
          "<title> ESP8266 Analog Clock </title>"
        "</head>"
        "<body style=\"background-color:lightgrey;\">"
          "<h1>Analog Clock&nbsp;&nbsp;"+String(timeStr)+"</h1>"
          "<p>Uptime: "+getUpTime()+"</p>"
          "<p>Last NTP sync at "+lastSyncTime+" on "+lastSyncDate+"</p>"
        "</body>"
      "</html>");
   }
   else {
      analogClkServer.send(200, "text/html",
      "<!DOCTYPE HTML>"
      "<html>"
        "<head>"
          "<meta content=\"text/html; charset=utf-8\">"
          "<title>Analog Clock Setup</title>"
        "</head>"
        "<body>"
          "<form action=\"/save\" method=\"POST\">"
          "<h1> Analog Clock Setup</h1>"
          "<p>Since the analog clock hands do not provide feedback of their position, you must specify<br>the starting position of the clock hour, minute and second hands. Do not leave any fields blank!</p>"
          "<ol>"
            "<li>Enter the current position of the hour, minute and second hands.</li>"
            "<li>Select your time zone.</li>"
            "<li>Click the \"Submit\" button.</li>"
          "</ol>"
          "<script>"
          "var tzLook =" + String(GENERATED_TZ_JSON) + ";"
          "</script>"
          "<table>"
          "<tr>"
          "<td>"
          "<label>"
          "Hour   (0-11):</label>"
          "</td>"
          "<td>"
          "<input type=\"number\" min=\"0\" max=\"23\" size=\"3\" name=\"hour\"   value=\"\">"
          "</td>"
          "</tr>"
          "<tr>"
          "<td>"
          "<label>"
          "Minute (0-59):</label>"
          "</td>"
          "<td>"
          "<input type=\"number\" min=\"0\" max=\"59\" size=\"3\" name=\"minute\" value=\"\">"
          "</td>"
          "</tr>"
          "<tr>"
          "<td>"
          "<label>"
          "Second (0-59):</label>"
          "</td>"
          "<td>"
          "<input type=\"number\" min=\"0\" max=\"59\" size=\"3\" name=\"second\" value=\"\">"
          "</td>"
          "</tr>"
          "<tr>"
          "<td>"
          "<label>"
          "Timezone:"
          "</label>"
          "</td>"
          "<td>"
          "<select onchange=\"onAreaChange()\" name=\"area\" size=\"11\" id=\"area\">"
          "  <option value=\"noarea\">No Area</option>"
          "</select>"
          "</td>"
          "<td>"
          "<select name=\"city\" id=\"city\" size=\"11\">"
          "  <option value=\"nocity\">----</option>"
          "</select>"
          "</td>"
          "</tr>"
          "</table>"
          "<input type=\"submit\" value=\"Submit\">"
          "</form>"
          "<script>"
          "function removeOptions(selectElement) {"
          "   var i, L = selectElement.options.length - 1;"
          "   for(i = L; i >= 0; i--) {"
          "      selectElement.remove(i);"
          "   }"
          "}"
          "function setSelectToValues(selRef, lst) {"
          "    removeOptions(selRef);"
          "    for (const val in lst) {"
          "	var el = document.createElement(\"option\");"
          "	el.textContent = val;"
          "	if ( typeof(lst[val]) == \"number\" ) {"
          "	    el.value = lst[val];"
          "	} else {"
          "	    el.value = val;"
          "	}"
          "	selRef.appendChild(el);"
          "    }"
          "    selRef.selectedIndex = \"0\""
          "}"
          "function onAreaChange() {   "
          "    setSelectToValues(city, tzLook[area.value])"
          "}"
          "setSelectToValues(area, tzLook);"
          "browserTz = Intl.DateTimeFormat().resolvedOptions().timeZone.split(\"/\");"
          "area.value = browserTz[0];"
          "onAreaChange();"
          "city.value = tzLook[area.value][browserTz[1]];"
          "</script>"
        "</body>"
      "</html>");  
  }
}


//--------------------------------------------------------------------------
// Handles save requests.
//--------------------------------------------------------------------------
void handleSave() {
  analogClkServer.send(200, "text/html",
  "<!DOCTYPE HTML>"
  "<html>"
    "<head>"
      "<META HTTP-EQUIV=\"refresh\" CONTENT=\"5; URL=\'/clock\'\">"
      "<meta content=\"text/html; charset=utf-8\">"
      "<title> ESP8266 Analog Clock - Saving...</title>"
    "</head>"
    "<body style=\"background-color:lightgrey;\">"
      "<h1>Saving Analog Clock...</h1>"
      "Redirecting after save."
    "</body>"
  "</html>");
          
  if (analogClkServer.hasArg("hour")&&analogClkServer.hasArg("minute")&&analogClkServer.hasArg("second")&&analogClkServer.hasArg("city")) {
         String hourValue = analogClkServer.arg("hour");
         analogClkHour = hourValue.toInt();
         String minuteValue = analogClkServer.arg("minute");
         analogClkMinute = minuteValue.toInt();  
         String secondValue = analogClkServer.arg("second");
         analogClkSecond = secondValue.toInt();
         String zoneValue = analogClkServer.arg("city");
         int timezonePosition = zoneValue.toInt();
         NTP.setTimeZone(GENERATED_TZ_LOOKUP[timezonePosition]);

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
         ee.writeByte(TIMEZONE,timezonePosition);
         ee.writeByte(CHECK1,0xAA);
         ee.writeByte(CHECK2,0x55);
         setupComplete = true;                               // set flag to indicate that we're done with setup   
      }
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

  switch(e.event) { 
    case noResponse:
      Serial.println("NTP server no response");
      break;
    
    case invalidAddress:
      Serial.println("NTP server can't find server");
      break;

    case invalidPort:
      Serial.println("NTP port already used");
      break;

    case requestSent:
      Serial.println("NTP request sent, waiting for response");
      break;

    case partlySync:
      Serial.println("Successful sync but offset was over threshold");
      break;

    case syncNotNeeded:
      Serial.println("Successful sync but offset was under minimum threshold");
      break;

    case errorSending:
      Serial.println("An error happened while sending the request");
      break;

    case responseError:
      Serial.println("Wrong response received");
      break;

    case syncError:
      Serial.println("Error adjusting time");
      break;

    case accuracyError:
      Serial.println("NTP server time is not accurate enough");
      break;

   case timeSyncd:
      lastSyncTime = NTP.getTimeStr(NTP.getLastNTPSync());
      lastSyncDate = NTP.getDateStr(NTP.getLastNTPSync());
      Serial.print("Got NTP time: ");
      Serial.print(lastSyncTime+"  ");
      Serial.println(lastSyncDate);

      setTime(NTP.getLastNTPSync());
      break;

    default:
      Serial.print("Unknown NTP sync process event: ");
      Serial.print(e.event);
      Serial.println();

  }

}

