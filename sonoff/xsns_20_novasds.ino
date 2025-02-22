/*
  xsns_20_novasds.ino - Nova SDS011/SDS021 particle concentration sensor support for Sonoff-Tasmota

  Copyright (C) 2019  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_NOVA_SDS
/*********************************************************************************************\
 * Nova Fitness SDS011 (and possibly SDS021) particle concentration sensor
 * For background information see http://aqicn.org/sensor/sds011/
 * For protocol specification see
 *   https://cdn.sparkfun.com/assets/parts/1/2/2/7/5/Laser_Dust_Sensor_Control_Protocol_V1.3.pdf
 *
 * Hardware Serial will be selected if GPIO3 = [SDS0X01]
\*********************************************************************************************/

#define XSNS_20             20

#include <TasmotaSerial.h>

#ifndef STARTING_OFFSET
#define STARTING_OFFSET               30      // Turn on NovaSDS XX-seconds before tele_period is reached
#endif
#if STARTING_OFFSET < 10
#error "Please set STARTING_OFFSET >= 10"
#endif
#ifndef NOVA_SDS_RECDATA_TIMEOUT
#define NOVA_SDS_RECDATA_TIMEOUT      150     // NodaSDS query data timeout in ms
#endif
#ifndef NOVA_SDS_DEVICE_ID
#define NOVA_SDS_DEVICE_ID            0xFFFF  // NodaSDS all sensor response
#endif

TasmotaSerial *NovaSdsSerial;

uint8_t novasds_type = 1;
uint8_t novasds_valid = 0;
uint8_t cont_mode = 1;

struct sds011data {
  uint16_t pm100;
  uint16_t pm25;
} novasds_data;
uint16_t pm100_sum;
uint16_t pm25_sum;

// NovaSDS commands
#define NOVA_SDS_REPORTING_MODE       2       // Cmnd "data reporting mode"
#define NOVA_SDS_QUERY_DATA           4       // Cmnd "Query data"
#define NOVA_SDS_SET_DEVICE_ID        5       // Cmnd "Set Device ID"
#define NOVA_SDS_SLEEP_AND_WORK       6       // Cmnd "sleep and work mode"
#define NOVA_SDS_WORKING_PERIOD       8       // Cmnd "working period"
#define NOVA_SDS_CHECK_FIRMWARE_VER   7       // Cmnd "Check firmware version"
  #define NOVA_SDS_QUERY_MODE           0       // Subcmnd "query mode"
  #define NOVA_SDS_SET_MODE             1       // Subcmnd "set mode"
  #define NOVA_SDS_REPORT_ACTIVE        0       // Subcmnd "report active mode" - Sensor received query data command to report a measurement data
  #define NOVA_SDS_REPORT_QUERY         1       // Subcmnd "report query mode" - Sensor automatically reports a measurement data in a work period
  #define NOVA_SDS_SLEEP                0       // Subcmnd "sleep mode"
  #define NOVA_SDS_WORK                 1       // Subcmnd "work mode"


bool NovaSdsCommand(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint16_t sensorid, uint8_t *buffer)
{
  uint8_t novasds_cmnd[19] = {0xAA, 0xB4, byte1, byte2, byte3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)(sensorid & 0xFF), (uint8_t)((sensorid>>8) & 0xFF), 0x00, 0xAB};

  // calc crc
  for (uint32_t i = 2; i < 17; i++) {
    novasds_cmnd[17] += novasds_cmnd[i];
  }

//  char hex_char[60];
//  AddLog_P2(LOG_LEVEL_DEBUG, PSTR("SDS: Send %s"), ToHex_P((unsigned char*)novasds_cmnd, 19, hex_char, sizeof(hex_char), ' '));

  // send cmnd
  NovaSdsSerial->write(novasds_cmnd, sizeof(novasds_cmnd));
  NovaSdsSerial->flush();

  // wait for any response
  unsigned long cmndtime = millis();
  while ( (TimePassedSince(cmndtime) < NOVA_SDS_RECDATA_TIMEOUT) && ( ! NovaSdsSerial->available() ) );
  if ( ! NovaSdsSerial->available() ) {
    // timeout
    return false;
  }
  uint8_t recbuf[10];
  memset(recbuf, 0, sizeof(recbuf));
  // sync to 0xAA header
  while ( (TimePassedSince(cmndtime) < NOVA_SDS_RECDATA_TIMEOUT) && ( NovaSdsSerial->available() > 0) && (0xAA != (recbuf[0] = NovaSdsSerial->read())) );
  if ( 0xAA != recbuf[0] ) {
    // no head found
    return false;
  }

  // read rest (9 of 10 bytes) of message
  NovaSdsSerial->readBytes(&recbuf[1], 9);
  AddLogBuffer(LOG_LEVEL_DEBUG_MORE, recbuf, sizeof(recbuf));

  if ( nullptr != buffer ) {
    // return data to buffer
    memcpy(buffer, recbuf, sizeof(recbuf));
  }

  // checksum & tail check
  if ((0xAB != recbuf[9] ) || (recbuf[8] != ((recbuf[2] + recbuf[3] + recbuf[4] + recbuf[5] + recbuf[6] + recbuf[7]) & 0xFF))) {
    AddLog_P(LOG_LEVEL_DEBUG, PSTR("SDS: " D_CHECKSUM_FAILURE));
    return false;
  }

  return true;
}

void NovaSdsSetWorkPeriod(void)
{
    // set sensor working period to default
    NovaSdsCommand(NOVA_SDS_WORKING_PERIOD, NOVA_SDS_SET_MODE, 0, NOVA_SDS_DEVICE_ID, nullptr);
    // set sensor report on query
    NovaSdsCommand(NOVA_SDS_REPORTING_MODE, NOVA_SDS_SET_MODE, NOVA_SDS_REPORT_QUERY,   NOVA_SDS_DEVICE_ID, nullptr);
}

bool NovaSdsReadData(void)
{
  uint8_t d[10];
  if ( ! NovaSdsCommand(NOVA_SDS_QUERY_DATA, 0, 0, NOVA_SDS_DEVICE_ID, d) ) {
    return false;
  }
  novasds_data.pm25 = (d[2] + 256 * d[3]);
  novasds_data.pm100 = (d[4] + 256 * d[5]);

  return true;
}

/*********************************************************************************************/

void NovaSdsSecond(void)                 // Every second
{
  if (!novasds_valid)
  { //communication problem, reinit
    NovaSdsSetWorkPeriod();
    novasds_valid=1;
  }
  if((Settings.tele_period - STARTING_OFFSET <= 0))
  {
    if(!cont_mode)
    { //switched to continuous mode
      cont_mode = 1;
      NovaSdsCommand(NOVA_SDS_SLEEP_AND_WORK, NOVA_SDS_SET_MODE, NOVA_SDS_WORK, NOVA_SDS_DEVICE_ID, nullptr);
    }
  }
  else
    cont_mode = 0;

  if(tele_period == Settings.tele_period - STARTING_OFFSET && !cont_mode)
  { //lets start fan and laser
    NovaSdsCommand(NOVA_SDS_SLEEP_AND_WORK, NOVA_SDS_SET_MODE, NOVA_SDS_WORK, NOVA_SDS_DEVICE_ID, nullptr);  
  }
  if(tele_period >= Settings.tele_period-5 && tele_period <= Settings.tele_period-2)
  { //we are doing 4 measurements here
    if(!(NovaSdsReadData())) novasds_valid=0;
    pm100_sum += novasds_data.pm100;
    pm25_sum  += novasds_data.pm25;
  }
  if(tele_period == Settings.tele_period-1)
  { //calculate the average of 4 measuremens
    novasds_data.pm100 = pm100_sum >> 2;
    novasds_data.pm25  = pm25_sum >> 2;
    if(!cont_mode)
      NovaSdsCommand(NOVA_SDS_SLEEP_AND_WORK, NOVA_SDS_SET_MODE, NOVA_SDS_SLEEP, NOVA_SDS_DEVICE_ID, nullptr);  //stop fan and laser
    pm100_sum = pm25_sum = 0;
  }
}

/*********************************************************************************************\
 * Command Sensor20
 *
 * 1 .. 255 - Set working period in minutes
\*********************************************************************************************/

bool NovaSdsCommandSensor(void)
{
  if ((XdrvMailbox.payload > 0) && (XdrvMailbox.payload < 256)) {
    Settings.novasds_period = XdrvMailbox.payload;
    NovaSdsSetWorkPeriod();
  }
  Response_P(S_JSON_SENSOR_INDEX_NVALUE, XSNS_20, Settings.novasds_period);

  return true;
}

void NovaSdsInit(void)
{
  novasds_type = 0;
  if (pin[GPIO_SDS0X1_RX] < 99 && pin[GPIO_SDS0X1_TX] < 99) {
    NovaSdsSerial = new TasmotaSerial(pin[GPIO_SDS0X1_RX], pin[GPIO_SDS0X1_TX], 1);
    if (NovaSdsSerial->begin(9600)) {
      if (NovaSdsSerial->hardwareSerial()) {
        ClaimSerial();
      }
      novasds_type = 1;
      NovaSdsSetWorkPeriod();
    }
  }
}

#ifdef USE_WEBSERVER
const char HTTP_SDS0X1_SNS[] PROGMEM =
  "{s}SDS0X1 " D_ENVIRONMENTAL_CONCENTRATION " 2.5 " D_UNIT_MICROMETER "{m}%s " D_UNIT_MICROGRAM_PER_CUBIC_METER "{e}"
  "{s}SDS0X1 " D_ENVIRONMENTAL_CONCENTRATION " 10 " D_UNIT_MICROMETER "{m}%s " D_UNIT_MICROGRAM_PER_CUBIC_METER "{e}";      // {s} = <tr><th>, {m} = </th><td>, {e} = </td></tr>
#endif  // USE_WEBSERVER

void NovaSdsShow(bool json)
{
  if (novasds_valid) {
    float pm10f = (float)(novasds_data.pm100) / 10.0f;
    float pm2_5f = (float)(novasds_data.pm25) / 10.0f;
    char pm10[33];
    dtostrfd(pm10f, 1, pm10);
    char pm2_5[33];
    dtostrfd(pm2_5f, 1, pm2_5);
    if (json) {
      ResponseAppend_P(PSTR(",\"SDS0X1\":{\"PM2.5\":%s,\"PM10\":%s}"), pm2_5, pm10);
#ifdef USE_DOMOTICZ
      if (0 == tele_period) {
        DomoticzSensor(DZ_VOLTAGE, pm2_5);  // PM2.5
        DomoticzSensor(DZ_CURRENT, pm10);   // PM10
      }
#endif  // USE_DOMOTICZ
#ifdef USE_WEBSERVER
    } else {
      WSContentSend_PD(HTTP_SDS0X1_SNS, pm2_5, pm10);
#endif  // USE_WEBSERVER
    }
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xsns20(uint8_t function)
{
  bool result = false;

  if (novasds_type) {
    switch (function) {
      case FUNC_INIT:
        NovaSdsInit();
        break;
      case FUNC_EVERY_SECOND:
        NovaSdsSecond();
        break;
      case FUNC_COMMAND_SENSOR:
        if (XSNS_20 == XdrvMailbox.index) {
          result = NovaSdsCommandSensor();
        }
        break;
      case FUNC_JSON_APPEND:
        NovaSdsShow(1);
        break;
#ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:
        NovaSdsShow(0);
        break;
#endif  // USE_WEBSERVER
    }
  }
  return result;
}

#endif  // USE_NOVA_SDS
