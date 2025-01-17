/*
 Talks to SMA Sunny Boy, Model SB 8000US-12
    - Gets AC power (only)
    - Sends the result to a custom LED bargraph

  Based on code found at https://github.com/stuartpittaway/nanodesmapvmonitor
 */

//Need to change SoftwareSerial/NewSoftSerial.h file to set buffer to 128 bytes or you will get buffer overruns!
//Find the line below and change
//#define _NewSS_MAX_RX_BUFF 128 // RX buffer size

#include "Arduino.h"
#include "ESP32_SMA.h"

#include <ESP32Time.h>
#include "time.h"
#include "bluetooth.h"
#include "SMANetArduino.h"
#include "LocalUtil.h"
#include "mainstate.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Update.h>
#include "site_details.h"

#include "EspMQTTClient.h"

#include <logging.hpp>
#include <ets-appender.hpp>
#include <udp-appender.hpp>


// A time in Unix Epoch that is "now" - used to check that the NTP server has
// sync'd the time correctly before updating the inverter
#define AFTER_NOW  1630152740 // 2319321600

using namespace esp32m;

EspMQTTClient client = EspMQTTClient(
      SSID,
      PASSWORD,
      MQTT_SERVER,
      MQTT_USER, // Can be omitted if not needed
      MQTT_PASS, // Can be omitted if not needed
      HOST);

ESP32Time ESP32rtc;     // Time structure. Holds what time the ESP32 thinks it is.
ESP32Time nextMidnight; // Create a time structure to hold the answer to "What time (in time_t seconds) is the upcoming midnight?"

#ifdef SLEEP_ROUTINE
ESP32Time wakeupTime;
ESP32Time bedTime;
#endif
//#define WAKEUP_TIME 22,30 
//#define BED_TIME 05,45

#ifdef SYSLOG_HOST
#ifdef SYSLOG_PORT
  UDPAppender udpappender(SYSLOG_HOST, SYSLOG_PORT);
#else
  UDPAppender udpappender(SYSLOG_HOST, 514);
#endif
#endif


//BST Start and end dates - this needs moving into some sort of PROGMEM array for the years or calculated based on the BST logic see
//http://www.time.org.uk/bstgmtcodepage1.aspx
//static time_t SummerStart=1435888800;  //Sunday, 31 March 02:00:00 GMT
//static time_t SummerEnd=1425952800;  //Sunday, 27 October 02:00:00 GMT





bool blinklaststate;
void blinkLed()
{
  digitalWrite(LED_BUILTIN, blinklaststate);
  blinklaststate = !blinklaststate;
}

void blinkLedOff()
{
  if (blinklaststate)
  {
    blinkLed();
  }
}


static uint64_t currentvalue = 0;
static unsigned int valuetype = 0;
static unsigned long value = 0;
static uint64_t value64 = 0;
static long lastRanTime = 0;
static long nowTime = 0;
static unsigned long spotpowerac = 0;
static unsigned long spotpowerdc = 0;
static float spotvoltdc=0;
static float spotampdc=0;

// "datetime" stores the number of seconds since the epoch (normally 01/01/1970), AS RETRIEVED
//     from the SMA. The value is updated when data is read from the SMA, like when
//     getInstantACPower() is called.
//static unsigned long datetime=0;   // stores the number of seconds since the epoch (normally 01/01/1970)
time_t datetime = 0;

const unsigned long seventy_years = 2208988800UL;

 prog_uchar PROGMEM smanet2packetx80x00x02x00[] = {0x80, 0x00, 0x02, 0x00};
 size_t sizeof_smanet2packetx80x00x02x00 = sizeof(smanet2packetx80x00x02x00);
 prog_uchar PROGMEM smanet2packet2[] = {0x80, 0x0E, 0x01, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
 size_t sizeof_smanet2packet2 = sizeof(smanet2packet2);
 //prog_uchar PROGMEM SMANET2header[] = {0xFF, 0x03, 0x60, 0x65};
 //prog_uchar PROGMEM InverterCodeArray[] = {0x5c, 0xaf, 0xf0, 0x1d, 0x50, 0x00}; // Fake address on the SMA NETWORK
 //prog_uchar PROGMEM fourzeros[] = {0, 0, 0, 0};
 prog_uchar PROGMEM smanet2packet6[] = {0x54, 0x00, 0x22, 0x26, 0x00, 0xFF, 0x22, 0x26, 0x00};
 size_t sizeof_smanet2packet6 = sizeof(smanet2packet6);
 prog_uchar PROGMEM smanet2packet99[] = {0x00, 0x04, 0x70, 0x00};
 size_t sizeof_smanet2packet99 = sizeof(smanet2packet99);
 prog_uchar PROGMEM smanet2packet0x01000000[] = {0x01, 0x00, 0x00, 0x00};
 size_t sizeof_smanet2packet0x01000000 = sizeof(smanet2packet0x01000000);

//Password needs to be 12 bytes long, with zeros as trailing bytes (Assume SMA INVERTER PIN code is 0000)
const unsigned char SMAInverterPasscode[] = {SMA_INVERTER_USER_PASSCODE};



void onConnectionEstablished()
{
  client.publish(MQTT_BASE_TOPIC "LWT", "online", true);
  log_w("WiFi and MQTT connected");
  log_w("v1 Build 2w d (%s) t (%s) "  ,__DATE__ , __TIME__) ;

#ifdef PUBLISH_HASS_TOPICS
  // client.publish(MQTT_BASE_TOPIC "LWT", "online", true);
  client.publish("homeassistant/sensor/" HOST "/signal/config", "{\"name\": \"" FRIENDLY_NAME " Signal Strength\", \"state_topic\": \"" MQTT_BASE_TOPIC "signal\", \"unique_id\": \"" HOST "-signal\", \"unit_of_measurement\": \"dB\", \"device\": {\"identifiers\": [\"" HOST "-device\"], \"name\": \"" FRIENDLY_NAME "\"}}", true);
  client.publish("homeassistant/sensor/" HOST "/generation_today/config", "{\"name\": \"" FRIENDLY_NAME " Power Generation Today\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "generation_today\", \"unique_id\": \"" HOST "-generation_today\", \"unit_of_measurement\": \"Wh\", \"state_class\": \"total_increasing\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);
  client.publish("homeassistant/sensor/" HOST "/generation_total/config", "{\"name\": \"" FRIENDLY_NAME " Power Generation Total\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "generation_total\", \"unique_id\": \"" HOST "-generation_total\", \"unit_of_measurement\": \"Wh\", \"state_class\": \"total_increasing\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);
  client.publish("homeassistant/sensor/" HOST "/instant_ac/config", "{\"name\": \"" FRIENDLY_NAME " Instantinous AC Power\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "instant_ac\", \"unique_id\": \"" HOST "-instant_ac\", \"unit_of_measurement\": \"W\", \"state_class\": \"measurement\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);
  client.publish("homeassistant/sensor/" HOST "/instant_dc/config", "{\"name\": \"" FRIENDLY_NAME " Instantinous DC Power\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "instant_dc\", \"unique_id\": \"" HOST "-instant_dc\", \"unit_of_measurement\": \"W\", \"state_class\": \"measurement\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);
  client.publish("homeassistant/sensor/" HOST "/instant_vdc/config", "{\"name\": \"" FRIENDLY_NAME " Instantinous DC Voltage\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "instant_vdc\", \"unique_id\": \"" HOST "-instant_vdc\", \"unit_of_measurement\": \"V\", \"state_class\": \"measurement\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);
  client.publish("homeassistant/sensor/" HOST "/instant_adc/config", "{\"name\": \"" FRIENDLY_NAME " Instantinous DC Ampere\", \"device_class\": \"energy\", \"state_topic\": \"" MQTT_BASE_TOPIC "instant_adc\", \"unique_id\": \"" HOST "-instant_adc\", \"unit_of_measurement\": \"A\", \"state_class\": \"measurement\", \"device\": {\"identifiers\": [\"" HOST "-device\"]} }", true);

#endif // PUBLISH_HASS_TOPICS
}

void setup()
{
  Serial.begin(115200);                      //Serial port for debugging output

  Logging::setLevel(esp32m::Info);
  Logging::addAppender(&ETSAppender::instance());
#ifdef SYSLOG_HOST
  udpappender.setMode(UDPAppender::Format::Syslog);
  Logging::addAppender(&udpappender);
#endif
  Serial.println("added appenders");

  // Get the MAC address in reverse order (not sure why, but do it here to make setup easier)
  unsigned char smaSetAddress[6] = {SMA_ADDRESS};
  for (int i = 0; i < 6; i++)
    smaBTInverterAddressArray[i] = smaSetAddress[5 - i];

  pinMode(LED_BUILTIN, OUTPUT);

  ESP32rtc.setTime(30, 24, 15, 17, 1, 2021); // 17th Jan 2021 15:24:30  // Need this to be accurate. Since connecting to the internet anyway, use NTP.

  client.setMaxPacketSize(512); // must be big enough to send home assistant config
  client.enableMQTTPersistence();
  client.enableDebuggingMessages();                                     // Enable debugging messages sent to serial output
  client.enableHTTPWebUpdater("/update");                               // Enable the web updater. User and password default to values of MQTTUsername and MQTTPassword. These can be overrited with enableHTTPWebUpdater("user", "password").
  client.enableLastWillMessage(MQTT_BASE_TOPIC "LWT", "offline", true); // You can activate the retain flag by setting the third parameter to true

  log_w("Connected to %s" , SSID);
  log_w("IP address: %s" , WiFi.localIP().toString().c_str());

  // Always set time to GMT timezone
  configTime(timeZoneOffset, 0, NTP_SERVER);
  log_w("2W Connected to %s" , SSID);
  
  Logging::hookUartLogger();
  // setupOTAServer();


  log_d("hello world!");
}

void everySecond()
{
  blinkLedOff();
}

void every5Minutes()
{
  log_d("every5Minutes()");
  if (client.isConnected())
  {
    client.publish(MQTT_BASE_TOPIC "signal", String(WiFi.RSSI()));
  }
}

void everyHour()
{

}

void everyDay()
{

}


unsigned long sleepuntil = 0;
void dodelay(unsigned long duration)
{
  sleepuntil = millis() + duration;
}

//uint8_t mainstate = MAINSTATE_INIT;
MainState mainstate = MAINSTATE_INIT;
uint8_t innerstate = 0;
unsigned long nextSecond = 0;
unsigned long next5Minute = 0;
unsigned long nextHour = 0;
unsigned long nextDay = 0;
int thisminute = -1;
int checkbtminute = -1;

void loop()
{
  struct tm timeinfo;
  client.loop();
  // server.handleClient();
  
  if (millis() >= nextSecond)
  {
    nextSecond = millis() + 1000;
    everySecond();
  }

  if (millis() >= next5Minute)
  {
    next5Minute = millis() + 1000 * 60 * 5;
    every5Minutes();
  }

  if (millis() >= nextHour)
  {
    nextHour = millis() + 1000 * 3600;
    everyHour();
  }

  if (millis() >= nextDay)
  {
    nextDay = millis() + 1000 * 3600 * 24 ;
    everyDay();
  }

  // "delay" the main BT loop
  if (millis() < sleepuntil)
    return;

  // if in the main loop, only run at the top of the minute
  if (mainstate >= MAINSTATE_GET_INSTANT_AC_POWER)
  {
    getLocalTime(&timeinfo);
    if (timeinfo.tm_min == thisminute)
      return;

    
    // Check we are connected to BT, if not, restart process
    if (!BTCheckConnected())
    {
      mainstate = MAINSTATE_INIT;
    }
  }

  // Wait for initial NTP sync before setting up inverter
  if (mainstate == MAINSTATE_INIT && ESP32rtc.getEpoch() < AFTER_NOW)
  { configTime(timeZoneOffset, 0, NTP_SERVER); // just added configtime multiple times to make this work. Not a nice workaround...

    log_d("NTP not yet sync'd, sleeping");
    log_w("NTP not yes sync'd, sleeping 10s");
    dodelay(10000);
    configTime(timeZoneOffset, 0, NTP_SERVER);

  }

  // Only bother to do anything if we are connected to WiFi and MQTT
  if (!client.isConnected())
    return;

  // Run the "main" BT loop
  blinkLed();

  log_d("Loop mainstate: %s", mainstate.toString().c_str());

  switch (mainstate)
  {
  case MAINSTATE_INIT:
    if (BTStart())
    {
      mainstate++;
      innerstate = 0;
    }
    else
    {
      dodelay(1000);
    }
    break;

  case MAINSTATE_INIT_SMA_CONNECTION:
    if (initialiseSMAConnection())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_LOGON_SMA_INVERTER:
    if (logonSMAInverter())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_GET_DAILY_YIELD_3 : //3
    // Doing this to set datetime
    if (getDailyYield())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_CHECK_SET_INVERTER_TIME: // 4:
    if (checkIfNeedToSetInverterTime())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

    // --------- regular loop -----------------

  case MAINSTATE_GET_INSTANT_AC_POWER:// 5:
    if (getInstantACPower())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_GET_INSTANT_DC_POWER: //6:
    if (getInstantDCPower())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_GET_DAILY_YIELD_7:// 7:
    if (getDailyYield())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  case MAINSTATE_TOTAL_POWER_GENERATION: //8:
    if (getTotalPowerGeneration())
    {
      mainstate++;
      innerstate = 0;
    }
    break;

  default:
    mainstate = MAINSTATE_GET_INSTANT_AC_POWER;
    innerstate = 0;
    thisminute = timeinfo.tm_min;
  }
  return;

  //getInverterName();
  //HistoricData();
  lastRanTime = millis() - 4000;
  log_i("While...");

  while (1)
  {

    //HowMuchMemory();

    //Populate datetime and spotpowerac variables with latest values
    //getInstantDCPower();
    nowTime = millis();
    if (nowTime > (lastRanTime + 4000))
    {
      lastRanTime = nowTime;
      Serial.print("^");
      getInstantACPower();
    }

    //digitalClockDisplay(now());

    //The inverter always runs in UTC time (and so does this program!), and ignores summer time, so fix that here...
    //add 1 hour to readings if its summer
    // DRH Temp removal of: if ((datetime>=SummerStart) && (datetime<=SummerEnd)) datetime+=60*60;

#ifdef SLEEP_ROUTINE

 //wakeupTime;
 //bedTime;
#endif



#ifdef allowsleep
    if ((ESP32rtc.getEpoch() > (datetime + 3600)) && (spotpowerac == 0))
    {
      //Inverter output hasnt changed for an hour, so put Nanode to sleep till the morning
      //debugMsgln("Bed time");

      //sleeping=true;

      //Get midnight on the day of last solar generation
      // First, create a time structure to hold the answer to "At what time (in time_t seconds) is the upcoming midnight?"
      tmElements_t tm;
      tm.Year = year(datetime) - 1970;
      tm.Month = month(datetime);
      tm.Day = day(datetime);
      tm.Hour = 23;
      tm.Hour = hour(datetime);
      tm.Minute = 59;
      tm.Second = 59;
      time_t midnight = makeTime(tm);

      //Move to midnight
      //debugMsg("Midnight ");digitalClockDisplay( midnight );debugMsgln("");

      if (ESP32rtc.getEpoch() < midnight)
      {
        //Time to calculate SLEEP time, we only do this if its BEFORE midnight
        //on the day of solar generation, otherwise we might wake up and go back to sleep immediately!

        //Workout what time sunrise is and sleep till then...
        //longitude, latitude (london uk=-0.126236,51.500152)
        unsigned int minutespastmidnight = ComputeSun(mylongitude, mylatitude, datetime, true);

        //We want to wake up at least 15 minutes before sunrise, just in case...
        checktime = midnight + minutespastmidnight - 15 * 60;
      }
    }
#endif

    //digitalClockDisplay( checktime );

    //Delay for approx. 4 seconds between instant AC power readings

  } // end of while(1)
} // end of loop()



//-------------------------------------------------------------------------------------------
bool checkIfNeedToSetInverterTime()
{
  //digitalClockDisplay(now());Serial.println("");
  //digitalClockDisplay(datetime);Serial.println("");
  log_i("checkIfNeedToSetInverterTime()");

  unsigned long timediff;

  timediff = abs((long) (datetime - ESP32rtc.getEpoch()));
  log_i("Time diff: %lu, datetime: %lu, epoch: %lu " , timediff, datetime, ESP32rtc.getEpoch());

  if (timediff > 60)
  {
    //If inverter clock is out by more than 1 minute, set it to the time from NTP, saves filling up the
    //inverters event log with hundred of "change time" lines...
    setInverterTime(); //Set inverter time to now()
  }

  return true;
}

prog_uchar PROGMEM smanet2settime[] = {
    0x8c, 0x0a, 0x02, 0x00, 0xf0, 0x00, 0x6d, 0x23, 0x00, 0x00, 0x6d, 0x23, 0x00, 0x00, 0x6d, 0x23, 0x00};

void setInverterTime()
{
  //Sets inverter time for those SMA inverters which don't have a realtime clock (Tripower 8000 models for instance)
  log_i("setInverterTime(%i) " , innerstate);

  //Payload...

  //** 8C 0A 02 00 F0 00 6D 23 00 00 6D 23 00 00 6D 23 00
  //   9F AE 99 4F   ==Thu, 26 Apr 2012 20:22:55 GMT  (now)
  //   9F AE 99 4F   ==Thu, 26 Apr 2012 20:22:55 GMT  (now)
  //   9F AE 99 4F   ==Thu, 26 Apr 2012 20:22:55 GMT  (now)
  //   01 00         ==Timezone +1 hour for BST ?
  //   00 00
  //   A1 A5 99 4F   ==Thu, 26 Apr 2012 19:44:33 GMT  (strange date!)
  //   01 00 00 00
  //   F3 D9         ==Checksum
  //   7E            ==Trailer

  //Set time to Feb

  //2A 20 63 00 5F 00 B1 00 0B FF B5 01
  //7E 5A 00 24 A3 0B 50 DD 09 00 FF FF FF FF FF FF 01 00
  //7E FF 03 60 65 10 A0 FF FF FF FF FF FF 00 00 78 00 6E 21 96 37 00 00 00 00 00 00 01
  //** 8D 0A 02 00 F0 00 6D 23 00 00 6D 23 00 00 6D 23 00
  //14 02 2B 4F ==Thu, 02 Feb 2012 21:37:24 GMT
  //14 02 2B 4F ==Thu, 02 Feb 2012 21:37:24 GMT
  //14 02 2B 4F  ==Thu, 02 Feb 2012 21:37:24 GMT
  //00 00        ==No time zone/BST not applicable for Feb..
  //00 00
  //AD B1 99 4F  ==Thu, 26 Apr 2012 20:35:57 GMT
  //01 00 00 00
  //F6 87        ==Checksum
  //7E

  //2A 20 63 00 5F 00 B1 00 0B FF B5 01
  //7E 5A 00 24 A3 0B 50 DD 09 00 FF FF FF FF FF FF 01 00
  //7E FF 03 60 65 10 A0 FF FF FF FF FF FF 00 00 78 00 6E 21 96 37 00 00 00 00 00 00 1C
  //** 8D 0A 02 00 F0 00 6D 23 00 00 6D 23 00 00 6D 23 00
  //F5 B3 99 4F
  //F5 B3 99 4F
  //F5 B3 99 4F 01 00 00 00 28 B3 99 4F 01 00 00 00
  //F3 C7 7E

  //2B 20 63 00 5F 00 DD 00 0B FF B5 01
  //7E 5A 00 24 A3 0B 50 DD 09 00 FF FF FF FF FF FF 01 00
  //7E FF 03 60 65 10 A0 FF FF FF FF FF FF 00 00 78 00 6E 21 96 37 00 00 00 00 00 00 08
  //** 80 0A 02 00 F0 00 6D 23 00 00 6D 23 00 00 6D 23 00
  //64 76 99 4F ==Thu, 26 Apr 2012 16:23:00 GMT
  //64 76 99 4F ==Thu, 26 Apr 2012 16:23:00 GMT
  //64 76 99 4F  ==Thu, 26 Apr 2012 16:23:00 GMT
  //58 4D   ==19800 seconds = 5.5 hours
  //00 00
  //62 B5 99 4F
  //01 00 00 00
  //C3 27 7E

  
  time_t currenttime = ESP32rtc.getEpoch(); // Returns the ESP32 RTC in number of seconds since the epoch (normally 01/01/1970)
  //digitalClockDisplay(currenttime);
  writePacketHeader(level1packet);
  writeSMANET2PlusPacket(level1packet, 0x09, 0x00, packet_send_counter, 0, 0, 0);
  writeSMANET2ArrayFromProgmem(level1packet, smanet2settime, sizeof(smanet2settime));
  writeSMANET2Long(level1packet, currenttime);
  writeSMANET2Long(level1packet, currenttime);
  writeSMANET2Long(level1packet, currenttime);
  // writeSMANET2Long(level1packet, timeZoneOffset);
  writeSMANET2uint(level1packet, timeZoneOffset);
  writeSMANET2uint(level1packet, 0);
  writeSMANET2Long(level1packet, currenttime); //No idea what this is for...
  writeSMANET2ArrayFromProgmem(level1packet, smanet2packet0x01000000, sizeof(smanet2packet0x01000000));
  writeSMANET2PlusPacketTrailer(level1packet);
  writePacketLength(level1packet);
  sendPacket(level1packet);
  packet_send_counter++;
  //debugMsgln(" done");
}

prog_uchar PROGMEM smanet2totalyieldWh[] = {
    0x54, 0x00, 0x01, 0x26, 0x00, 0xFF, 0x01, 0x26, 0x00};


size_t xArraySize(void);

bool initialiseSMAConnection()
{
  log_i("initialiseSMAConnection(%i) " , innerstate);


  unsigned char netid;
  switch (innerstate)
  {
  case 0:
    //Wait for announcement/broadcast message from PV inverter
    if (getPacket(0x0002))
      innerstate++;
    break;

  case 1:
    //Extract data from the 0002 packet
    netid = level1packet[4];

    

    // Now create a response and send it.
    for (int i = 0; i < sizeof(level1packet); i++)
      level1packet[i] = 0x00;

    writePacketHeader(level1packet, 0x02, 0x00, smaBTInverterAddressArray);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packet99, sizeof_smanet2packet99 );
    writeSMANET2SingleByte(level1packet, netid);
    writeSMANET2ArrayFromProgmem(level1packet, fourzeros, sizeof_fourzeros );
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packet0x01000000, sizeof(smanet2packet0x01000000));
    writePacketLength(level1packet);
    sendPacket(level1packet);
    innerstate++;
    break;

  case 2:
    // The SMA inverter will respond with a packet carrying the command '0x000A'.
    // It will return with cmdcode set to 0x000A.
    if (getPacket(0x000a))
      innerstate++;
    break;

  case 3:
    // The SMA inverter will now send two packets, one carrying the '0x000C' command, then the '0x0005' command.
    // Sit in the following loop until you get one of these two packets.
    cmdcode = readLevel1PacketFromBluetoothStream(0);
    if ((cmdcode == 0x000c) || (cmdcode == 0x0005))
      innerstate++;
    break;

  case 4:
    // If the most recent packet was command code = 0x0005 skip this next line, otherwise, wait for 0x0005 packet.
    // Since the first SMA packet after a 0x000A packet will be a 0x000C packet, you'll probably sit here waiting at least once.
    if (cmdcode == 0x0005)
    {
      innerstate++;
    }
    else
    {
      if (getPacket(0x0005))
        innerstate++;
    }
    break;

  case 5:
    //First SMANET2 packet
    writePacketHeader(level1packet, sixff);
    writeSMANET2PlusPacket(level1packet, 0x09, 0xa0, packet_send_counter, 0, 0, 0);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packetx80x00x02x00, sizeof_smanet2packetx80x00x02x00);
    writeSMANET2SingleByte(level1packet, 0x00);
    writeSMANET2ArrayFromProgmem(level1packet, fourzeros, sizeof_fourzeros);
    writeSMANET2ArrayFromProgmem(level1packet, fourzeros, sizeof_fourzeros);
    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);
    sendPacket(level1packet);

    if (getPacket(0x0001) && validateChecksum())
    {
      innerstate++;
      packet_send_counter++;
    }
    break;

  case 6:
    //Second SMANET2 packet
    writePacketHeader(level1packet, sixff);
    writeSMANET2PlusPacket(level1packet, 0x08, 0xa0, packet_send_counter, 0x00, 0x03, 0x03);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packet2, sizeof_smanet2packet2);

    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);
    sendPacket(level1packet);
    packet_send_counter++;

    innerstate++;
    break;

  default:
    return true;
  }

  return false;
}

prog_uchar PROGMEM smanet2packet_logon[] = {
    0x80, 0x0C, 0x04, 0xFD, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00, 0xaa, 0xaa, 0xbb, 0xbb};

bool logonSMAInverter()
{
  log_i("logonSMAInverter(%i)" , innerstate);

  //Third SMANET2 packet
  switch (innerstate)
  {
  case 0:
    writePacketHeader(level1packet, sixff);
    writeSMANET2PlusPacket(level1packet, 0x0e, 0xa0, packet_send_counter, 0x00, 0x01, 0x01);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packet_logon, sizeof(smanet2packet_logon));
    writeSMANET2ArrayFromProgmem(level1packet, fourzeros, sizeof_fourzeros);

    //INVERTER PASSWORD
    for (int passcodeloop = 0; passcodeloop < sizeof(SMAInverterPasscode); passcodeloop++)
    {
      unsigned char v = pgm_read_byte(SMAInverterPasscode + passcodeloop);
      writeSMANET2SingleByte(level1packet, (v + 0x88) % 0xff);
    }

    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);
    sendPacket(level1packet);

    innerstate++;
    break;

  case 1:
    if (getPacket(0x0001) && validateChecksum())
    {
      innerstate++;
      packet_send_counter++;
    }
    break;

  default:
    return true;
  }

  return false;
}

bool getDailyYield()
{
  //We expect a multi packet reply to this question...
  //We ask the inverter for its DAILY yield (generation)
  //once this is returned we can extract the current date/time from the inverter and set our internal clock
  log_d("getDailyYield(%i)" , innerstate);

  switch (innerstate)
  {
  case 0:
    writePacketHeader(level1packet);
    //writePacketHeader(level1packet,0x01,0x00,smaBTInverterAddressArray);
    writeSMANET2PlusPacket(level1packet, 0x09, 0xa0, packet_send_counter, 0, 0, 0);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packetx80x00x02x00, sizeof(smanet2packetx80x00x02x00));
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packet6, sizeof(smanet2packet6));
    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);

    sendPacket(level1packet);

    innerstate++;
    break;

  case 1:
    if (getPacket(0x0001))
    {
      if (validateChecksum())
      {
        packet_send_counter++;
        innerstate++;
      }
      else
        innerstate = 0;
    }
    break;

  case 2:
    //Returns packet looking like this...
    //    7E FF 03 60 65 0D 90 5C AF F0 1D 50 00 00 A0 83
    //    00 1E 6C 5D 7E 00 00 00 00 00 00 03
    //    80 01 02 00
    //    54 01 00 00 00 01 00 00 00 01
    //    22 26  //command code 0x2622 daily yield
    //    00     //unknown
    //    D6 A6 99 4F  //Unix time stamp (backwards!) = 1335469782 = Thu, 26 Apr 2012 19:49:42 GMT
    //    D9 26 00     //Daily generation 9.945 kwh
    //    00
    //    00 00 00 00
    //    18 61    //checksum
    //    7E       //packet trailer

    // Does this packet contain the British Summer time flag?
    //dumpPacket('Y');

    valuetype = level1packet[40 + 1 + 1] + level1packet[40 + 2 + 1] * 256;

    //Serial.println(valuetype,HEX);
    //Make sure this is the right message type
    if (valuetype == 0x2622)
    {
      memcpy(&value64, &level1packet[40 + 8 + 1], 8);
      //0x2622=Day Yield Wh
      // memcpy(&datetime, &level1packet[40 + 4 + 1], 4);
      datetime = LocalUtil::get_long(level1packet + 40 + 1 + 4);
      // debugMsg("Current Time (epoch): ");
      // debugMsgLn(String(datetime));

      //setTime(datetime);
      currentvalue = value64;
      client.publish(MQTT_BASE_TOPIC "generation_today", LocalUtil::uint64ToString(currentvalue), true);
      log_i("Day Yield: %f" , (double)value64 / 1000);

    }
    innerstate++;
    break;

  default:
    return true;
  }

  return false;
}

prog_uchar PROGMEM smanet2acspotvalues[] = {
    0x51, 0x00, 0x3f, 0x26, 0x00, 0xFF, 0x3f, 0x26, 0x00, 0x0e};

bool getInstantACPower()
{
  log_d("getInstantACPower(%i)" , innerstate);

  int32_t thisvalue;
  //Get spot value for instant AC wattage
  // debugMsg("getInstantACPower stage: ");
  // debugMsgLn(String(innerstate));

  switch (innerstate)
  {
  case 0:
    writePacketHeader(level1packet); //writePacketHeader(pcktBuf, 0x01, addr_unknown);
    //writePacketHeader(level1packet,0x01,0x00,smaBTInverterAddressArray);
    writeSMANET2PlusPacket(level1packet, 0x09, 0xA1, packet_send_counter, 0, 0, 0); // writePacket(pcktBuf, 0x09, 0xA0, 0, device->SUSyID, device->Serial);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packetx80x00x02x00, sizeof(smanet2packetx80x00x02x00));
    writeSMANET2ArrayFromProgmem(level1packet, smanet2acspotvalues, sizeof(smanet2acspotvalues));
    writeSMANET2PlusPacketTrailer(level1packet); //writePacketTrailer(pcktBuf);
    writePacketLength(level1packet); //writePacketLength(pcktBuf);

    sendPacket(level1packet);
    innerstate++;
    break;

  case 1:
    if (waitForMultiPacket(0x0001))
    {
      if (validateChecksum())
      {
        packet_send_counter++;
        innerstate++;
      }
      else
        innerstate = 0;
    }
    break;

  case 2:
    //value will contain instant/spot AC power generation along with date/time of reading...
    // memcpy(&datetime, &level1packet[40 + 1 + 4], 4);
    datetime = LocalUtil::get_long(level1packet + 40 + 1 + 4);
    thisvalue = LocalUtil::get_long(level1packet + 40 + 1 + 8);
    // memcpy(&thisvalue, &level1packet[40 + 1 + 8], 4);
    
    currentvalue = thisvalue;
    client.publish(MQTT_BASE_TOPIC "instant_ac", LocalUtil::uint64ToString(currentvalue), true);

    log_i("AC Pwr= %li " , thisvalue);

    spotpowerac = thisvalue;

    //displaySpotValues(28);
    innerstate++;
    break;

  default:
    return true;
  }

  return false;
}

bool getTotalPowerGeneration()
{

  //Gets the total kWh the SMA inverter has generated in its lifetime...
  // debugMsg("getTotalPowerGeneration stage: ");
  // debugMsgLn(String(innerstate));
  log_d("getTotalPowerGeneration(%i)" , innerstate);

  switch (innerstate)
  {
  case 0:
    writePacketHeader(level1packet);
    writeSMANET2PlusPacket(level1packet, 0x09, 0xa0, packet_send_counter, 0, 0, 0);
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packetx80x00x02x00, sizeof(smanet2packetx80x00x02x00));
    writeSMANET2ArrayFromProgmem(level1packet, smanet2totalyieldWh, sizeof(smanet2totalyieldWh));
    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);

    sendPacket(level1packet);
    innerstate++;
    break;

  case 1:
    if (waitForMultiPacket(0x0001))
    {
      if (validateChecksum())
      {
        packet_send_counter++;
        innerstate++;
      }
      else
        innerstate = 0;
    }
    break;

  case 2:
    //displaySpotValues(16);
    memcpy(&datetime, &level1packet[40 + 1 + 4], 4);
    memcpy(&value64, &level1packet[40 + 1 + 8], 8);
    //digitalClockDisplay(datetime);
    currentvalue = value64;
    log_i("Total Power: %f ", (double)value64 / 1000);
    client.publish(MQTT_BASE_TOPIC "generation_total", LocalUtil::uint64ToString(currentvalue), true);
    innerstate++;
    break;

  default:
    return true;
  }

  return false;
}

//Returns volts + amps
//prog_uchar PROGMEM smanet2packetdcpower[]={  0x83, 0x00, 0x02, 0x80, 0x53, 0x00, 0x00, 0x45, 0x00, 0xFF, 0xFF, 0x45, 0x00 };
// Just DC Power (watts)
prog_uchar PROGMEM smanet2packetdcpower[] = {
    0x83, 0x00, 0x02, 0x80, 0x53, 0x00, 0x00, 0x25, 0x00, 0xFF, 0xFF, 0x25, 0x00};
bool getInstantDCPower()
{
  // 2W - This appears broken...
  return true;
  log_i("getInstantDCPower(%i)", innerstate);
  //DC
  //We expect a multi packet reply to this question...

  switch (innerstate)
  {
  case 0:

    writePacketHeader(level1packet);
    //writePacketHeader(level1packet,0x01,0x00,smaBTInverterAddressArray);
    writeSMANET2PlusPacket(level1packet, 0x09, 0xE0, packet_send_counter, 0, 0, 0);
    // writeSMANET2ArrayFromProgmem(level1packet, smanet2packetx80x00x02x00, sizeof(smanet2packetx80x00x02x00));
    writeSMANET2ArrayFromProgmem(level1packet, smanet2packetdcpower, sizeof(smanet2packetdcpower));
    writeSMANET2PlusPacketTrailer(level1packet);
    writePacketLength(level1packet);

    sendPacket(level1packet);
    innerstate++;
    break;

  case 1:
    if (waitForMultiPacket(0x0001))
    {
      if (validateChecksum())
      {
        packet_send_counter++;
        innerstate++;
      }
      else
        innerstate = 0;
    }
    break;

  case 2:
    //displaySpotValues(28);
    for (int i = 40 + 1; i < packetposition - 3; i += 28)
    {
      valuetype = level1packet[i + 1] + level1packet[i + 2] * 256;
      memcpy(&value, &level1packet[i + 8], 4);

      //valuetype
      log_i("getInstantDCPower(): valuetype: %li value: %lu ", valuetype, value);

      //0x451f=DC Voltage  /100
      //0x4521=DC Current  /1000
      //0x251e=DC Power /1
      if (valuetype==0x451f) spotvoltdc=(float)value/(float)100.0;

      if (valuetype==0x4521) spotampdc=(float)value/(float)1000.0;

      if (valuetype == 0x251e) {
        if (value > MAX_SPOTDC) {
          spotpowerdc = spotvoltdc * spotampdc; 
        } else {
          spotpowerdc = value;
        }  
      } 

      memcpy(&datetime, &level1packet[i + 4], 4);
    }

    //spotpowerdc=volts*amps;

    client.publish(MQTT_BASE_TOPIC "instant_dc", LocalUtil::uint64ToString(spotpowerdc), true);
    client.publish(MQTT_BASE_TOPIC "instant_vdc", LocalUtil::uint64ToString(spotvoltdc), true);
    client.publish(MQTT_BASE_TOPIC "instant_adc", LocalUtil::uint64ToString(spotampdc), true);

    log_i("DC Pwr=%lu Volt=%f Amp=%f " , spotpowerdc, spotvoltdc, spotampdc);

    innerstate++;
    break;

  default:
    return true;
  }

  return false;
}

/*
//Inverter name
 prog_uchar PROGMEM smanet2packetinvertername[]={   0x80, 0x00, 0x02, 0x00, 0x58, 0x00, 0x1e, 0x82, 0x00, 0xFF, 0x1e, 0x82, 0x00};  
 
 void getInverterName() {
 
 do {
 //INVERTERNAME
 debugMsgln("InvName"));
 writePacketHeader(level1packet,sixff);
 //writePacketHeader(level1packet,0x01,0x00,sixff);
 writeSMANET2PlusPacket(level1packet,0x09, 0xa0, packet_send_counter, 0, 0, 0);
 writeSMANET2ArrayFromProgmem(level1packet,smanet2packetinvertername);
 writeSMANET2PlusPacketTrailer(level1packet);
 writePacketLength(level1packet);
 sendPacket(level1packet);
 
 waitForMultiPacket(0x0001);
 } 
 while (!validateChecksum());
 packet_send_counter++;
 
 valuetype = level1packet[40+1+1]+level1packet[40+2+1]*256;
 
 if (valuetype==0x821e) {
 memcpy(invertername,&level1packet[48+1],14);
 Serial.println(invertername);
 memcpy(&datetime,&level1packet[40+4+1],4);  //Returns date/time unit switched PV off for today (or current time if its still on)
 }
 }
 
 void HistoricData() {
 
 time_t currenttime=now();
 digitalClockDisplay(currenttime);
 
 debugMsgln("Historic data...."));
 tmElements_t tm;
 if( year(currenttime) > 99)
 tm.Year = year(currenttime)- 1970;
 else
 tm.Year = year(currenttime)+ 30;  
 
 tm.Month = month(currenttime);
 tm.Day = day(currenttime);
 tm.Hour = 10;      //Start each day at 5am (might need to change this if you're lucky enough to live somewhere hot and sunny!!
 tm.Minute = 0;
 tm.Second = 0;
 time_t startTime=makeTime(tm);  //Midnight
 
 
 //Read historic data for today (SMA inverter saves 5 minute averaged data)
 //We read 30 minutes at a time to save RAM on Arduino
 
 //for (int hourloop=1;hourloop<24*2;hourloop++)
 while (startTime < now()) 
 {
 //HowMuchMemory();
 
 time_t endTime=startTime+(25*60);  //25 minutes on
 
 //digitalClockDisplay(startTime);
 //digitalClockDisplay(endTime);
 //debugMsgln(" ");
 
 do {
 writePacketHeader(level1packet);
 //writePacketHeader(level1packet,0x01,0x00,smaBTInverterAddressArray);
 writeSMANET2PlusPacket(level1packet,0x09, 0xE0, packet_send_counter, 0, 0, 0);
 
 writeSMANET2SingleByte(level1packet,0x80);
 writeSMANET2SingleByte(level1packet,0x00);
 writeSMANET2SingleByte(level1packet,0x02);
 writeSMANET2SingleByte(level1packet,0x00);
 writeSMANET2SingleByte(level1packet,0x70);
 // convert from an unsigned long int to a 4-byte array
 writeSMANET2Long(level1packet,startTime);
 writeSMANET2Long(level1packet,endTime);
 writeSMANET2PlusPacketTrailer(level1packet);
 writePacketLength(level1packet);
 sendPacket(level1packet);
 
 waitForMultiPacket(0x0001);
 }
 while (!validateChecksum());
 //debugMsg("packetlength=");    Serial.println(packetlength);
 
 packet_send_counter++;
 
 //Loop through values
 for(int x=40+1;x<(packetposition-3);x+=12){
 memcpy(&value,&level1packet[x+4],4);
 
 if (value > currentvalue) {
 memcpy(&datetime,&level1packet[x],4);
 digitalClockDisplay(datetime);
 debugMsg("=");
 Serial.println(value);
 currentvalue=value;         
 
 //uploadValueToSolarStats(currentvalue,datetime);          
 }
 }
 
 startTime=endTime+(5*60);
 delay(750);  //Slow down the requests to the SMA inverter
 }
 }
 */
