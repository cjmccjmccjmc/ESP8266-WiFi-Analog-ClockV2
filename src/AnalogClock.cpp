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


// WEMOS D1 Mini pins
#define D0 16                    // can't use D0 to generate interrupts
#define D1  5
#define D2  4
#define D3  0                    // 10K pull-up, boot fails if pulled LOW
#define D4  2                    // 10K pull-up, boot fails if pulled LOW, BUILTIN_LED,
#define D5 14
#define D6 12
#define D7 13
#define D8 15                    // 10K pull-down, boot fails if pulled HIGH

   // Note: to remove need to update schematic to swap over pins.
#define LOCAL_SDA D1                   // output to SDA on the EERam
#define LOCAL_SCL D2                   // output to SCL on the EERam
#define COIL1 D3                 // output to clock's lavet motor coil
#define COIL2 D7                 // output to clock's lavet motor coil
#define REDLED D5                // output to red part of the RGB LED
#define GREENLED D4              // output to green part of the RGB LED
#define BLUELED D8               // output to blue part of the RGB LED
#define SWITCHPIN D6             // input from push button switch

#define DEBOUNCE 50              // 50 milliseconds to debounce the puchbutton switch
#define PULSETIME 30             // 30 millisecond pulse for the lavet motor


#define AP_NAME  "ClockSetupAP"


#define HOUR     0x0000                         // address in EERAM for analogClkHour
#define MINUTE   HOUR+1                         // address in EERAM for analogClkMinute
#define SECOND   HOUR+2                         // address in EERAM for analogClkSecond
#define WEEKDAY  HOUR+3                         // address in EERAM for analogClkWeekday
#define DAY      HOUR+4                         // address in EERAM for analogClkDay
#define MONTH    HOUR+5                         // address in EERAM for analogClkMonth
#define YEAR     HOUR+6                         // address in EERAM for analogClkYear
#define TIMEZONE HOUR+7                         // address in EERAM for timezone
#define CHECK1   HOUR+8                         // address in EERAM for 1st check byte 0xAA
#define CHECK2   HOUR+9                         // address in EERAM for 2nd check byte 0x55

const char* NTPSERVERNAME =  "nz.pool.ntp.org";
 // #define NTPSERVERNAME "0.us.pool.ntp.org"
//#define NTPSERVERNAME "time.nist.gov"
//#define NTPSERVERNAME "time.windows.com"
//#define NTPSERVERNAME "time.google.com"
//#define NTPSERVERNAME "time-a-g.nist.gov"     // NIST, Gaithersburg, Maryland

// EERAM eeRAM(0x50);
I2C_eeprom ee(0x50, I2C_DEVICESIZE_24LC16);

ESP8266WebServer analogClkServer(80);
Ticker pulseTimer,clockTimer,ledTimer;
String lastSyncTime = "";
String lastSyncDate = "";
boolean syncEventTriggered = false;             // true if an NTP time sync event has been triggered
boolean setupComplete = false;
boolean printTime = false;
boolean advanceClock = false;
volatile boolean switchInterruptFlag = false;
// int timeZone=-5;                               // EST
#define timeZone TZ_Pacific_Auckland
byte analogClkHour=0;
byte analogClkMinute=0;
byte analogClkSecond=0;
byte analogClkWeekday=0;
byte analogClkDay=0;
byte analogClkMonth=0;
byte analogClkYear=0;

// ISR functions should be defined with ICACHE_RAM_ATTR attribute to let the compiler know to never remove them from the IRAM. 
// If the ICACHE_RAM_ATTR attribute is missing the firmware will crash at the first call to attachInterrupt() on a ISR routine
// that happens not to be in ram at that moment. This bug will manifest itself only on some platform and some code configurations,
// as it entirely depends of where the compiler chooses to put the ISR functions.
void ICACHE_RAM_ATTR pinInterruptISR();
void handleRoot();
void blueLEDoff();
void greenLEDoff();
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

   // Note: to remove need to update schematic to swap over pins.
   ee.begin(LOCAL_SDA, LOCAL_SCL);
   pinMode(SWITCHPIN,INPUT_PULLUP);
   attachInterrupt(digitalPinToInterrupt(SWITCHPIN),pinInterruptISR,FALLING); // interrupt when puchbutton is pressed   
   pinMode(COIL1,OUTPUT);
   pinMode(COIL2,OUTPUT);
   pinMode(REDLED,OUTPUT);
   pinMode(GREENLED,OUTPUT);
   pinMode(BLUELED,OUTPUT);
   digitalWrite(COIL1,LOW);
   digitalWrite(COIL2,LOW);
   digitalWrite(REDLED,LOW);
   digitalWrite(GREENLED,LOW);
   digitalWrite(BLUELED,LOW);

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
   NTP.setTimeZone(timeZone);

   NTP.onNTPSyncEvent(syncNTPEventFunction);
   if (!NTP.setInterval(10,600)) Serial.println("Problem setting NTP interval.");

   byte waitCount = 500;
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
   analogClkServer.begin();

   //--------------------------------------------------------------------------
   // read analog clock values stored in EERam  
   //--------------------------------------------------------------------------

   if((ee.readByte(CHECK1)==0xAA)&&(ee.readByte(CHECK2)==0x55)&&(!inputAvail)){
      Serial.println("\nReading values from EERAM.");
      analogClkHour = ee.readByte(HOUR);
      Serial.println("\nReading values from EERAM.");
      analogClkMinute = ee.readByte(MINUTE);
      Serial.println("\nReading values from EERAM.");
      analogClkSecond = ee.readByte(SECOND);
      Serial.println("\nReading values from EERAM.");
      analogClkWeekday = ee.readByte(WEEKDAY);
      Serial.println("\nReading values from EERAM.");
      analogClkDay = ee.readByte(DAY);
      Serial.println("\nReading values from EERAM.");
      analogClkMonth = ee.readByte(MONTH);
      Serial.println("\nReading values from EERAM.");
      analogClkYear = ee.readByte(YEAR);      
      Serial.println("\nReading values from EERAM.");
      int ignoretimeZone = ee.readByte(TIMEZONE); 
      Serial.println("\nReading values from EERAM.");
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
         byte seconds = second();  
         if (lastSeconds != seconds) {
            lastSeconds = seconds;
            digitalWrite(BLUELED,HIGH);                            // flash the blue LED once each second while waiting for input to the web page
            ledTimer.once_ms(100,blueLEDoff);
         }
      }
   }
   clockTimer.attach_ms(100,checkClock);                      // start up 100 millisecond clock timer
}

//--------------------------------------------------------------------------
// Main Loop
//--------------------------------------------------------------------------
void loop() {
    static byte lastSeconds = 0; 

    // get any characters from the serial port
    if (Serial.available()) {
      char c = Serial.read();
    }

    // check to see if the pushbutton has been pressed
    if (switchInterruptFlag) {                                 // the pushbutton has been pressed
      switchInterruptFlag = false;
      Serial.println("Pushbutton!");
    }

    // flash the green LED once per second
    byte secs = second();  
    if (lastSeconds != secs) {                                 // when the second changes...
       lastSeconds = secs;                
       digitalWrite(GREENLED,HIGH);                            // turn on the green LED       
       ledTimer.once_ms(100,greenLEDoff);                      // turn off the green LED after 100 milliseconds
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
// Ticker callbacks that turns off the LEDs after 100 milliseconds.
//-------------------------------------------------------------------------
void blueLEDoff(){
   digitalWrite(BLUELED,LOW);                          
}

void greenLEDoff(){
   digitalWrite(GREENLED,LOW);                          
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
   pulseTimer.once_ms(PULSETIME,pulseOff);      // turn off pulse after 30 milliseconds...
}

//--------------------------------------------------------------------------
// Ticker callbacks run every 100 milliseconds that checks if the analog clock's 
// second hand needs to be advanced
//--------------------------------------------------------------------------
void checkClock() {
  time_t analogClkTime = makeTime({analogClkSecond,analogClkMinute,analogClkHour,analogClkWeekday,analogClkDay,analogClkMonth,analogClkYear});
  if (analogClkTime < now()) {                    // if the analog clock is behind the actual time and needs to be advanced...
    advanceClock = true;
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
          "<form action=\"/\" method=\"POST\">"
          "<h1> Analog Clock Setup</h1>"
          "<p>Since the analog clock hands do not provide feedback of their position, you must specify<br>the starting position of the clock hour, minute and second hands. Do not leave any fields blank!</p>"
          "<ol>"
            "<li>Enter the current position of the hour, minute and second hands.</li>"
            "<li>Select your time zone.</li>"
            "<li>Click the \"Submit\" button.</li>"
          "</ol>"
          "<table>"
            "<tr><td><label>Hour   (0-11):</label></td><td><input type=\"number\" min=\"0\" max=\"23\" size=\"3\" name=\"hour\"   value=\"\"></td></tr>"
            "<tr><td><label>Minute (0-59):</label></td><td><input type=\"number\" min=\"0\" max=\"59\" size=\"3\" name=\"minute\" value=\"\"></td></tr>"
            "<tr><td><label>Second (0-59):</label></td><td><input type=\"number\" min=\"0\" max=\"59\" size=\"3\" name=\"second\" value=\"\"></td></tr>"
          "</table><br>"
          "Timezone:<br>"
          "&nbsp;<input type=\"radio\" name=\"timezone\" value=\"E\" checked>&nbsp;Auckland, New Zealand<br>"
          "<input type=\"submit\" value=\"Submit\">"
          "</form>"
        "</body>"
      "</html>");  

          
      if (analogClkServer.hasArg("hour")&&analogClkServer.hasArg("minute")&&analogClkServer.hasArg("second")&&analogClkServer.hasArg("timezone")) {
         String hourValue = analogClkServer.arg("hour");
         analogClkHour = hourValue.toInt();
         String minuteValue = analogClkServer.arg("minute");
         analogClkMinute = minuteValue.toInt();  
         String secondValue = analogClkServer.arg("second");
         analogClkSecond = secondValue.toInt();
         String zoneValue = analogClkServer.arg("timezone");
         /* switch(zoneValue[0]) {
            case 'E': timeZone = -5;  // Eastern Standard Time 5 hours after UTC
                      break;
            case 'C': timeZone = -6;  // Central Standard Time 6 hours after UTC
                      break;        
            case 'M': timeZone = -7;  // Mountain Standard Time 7 hours after UTC
                      break;        
            case 'P': timeZone = -8;  // Pacific Standard Time 8 hours after UTC
                      break;        
            default:  timeZone = -5;  // EST
         }*/
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
         ee.writeByte(TIMEZONE,0);     
         ee.writeByte(CHECK1,0xAA);
         ee.writeByte(CHECK2,0x55);
         setupComplete = true;                               // set flag to indicate that we're done with setup   
      }
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
   int s=uptime%60;
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

//--------------------------------------------------------------------------
// interrupt when the push button switch is pressed
//--------------------------------------------------------------------------
void pinInterruptISR() {
   unsigned long debounce_time = millis()+DEBOUNCE;
   while(millis() < debounce_time);               // wait 50 milliseconds for the switch contacts to stop bouncing
   switchInterruptFlag = true;
}