#include "config.h"
#ifdef FRSKY_X
#include "telemetry.h"
#include "frsky_common.h"

void processFrskyPacket(uint8_t *packet);
void parseTelemHubByte(uint8_t c);
int32_t gpsToLong(int8_t neg, uint16_t bp, uint16_t ap);

#define START_STOP         0x7e
#define BYTESTUFF          0x7d
#define STUFF_MASK         0x20

// FrSky PRIM IDs (1 byte)
#define DATA_FRAME         0x10

// FrSky old DATA IDs (1 byte)
#define GPS_ALT_BP_ID      0x01
#define GPS_ALT_AP_ID      0x09
#define BARO_ALT_BP_ID     0x10
#define BARO_ALT_AP_ID     0x21
#define GPS_LONG_BP_ID     0x12
#define GPS_LONG_AP_ID     0x1A
#define GPS_LAT_BP_ID      0x13
#define GPS_LAT_AP_ID      0x1B
#define GPS_LONG_EW_ID     0x22
#define GPS_LAT_NS_ID      0x23
#define FRSKY_LAST_ID      0x3F

// FrSky new DATA IDs (2 bytes)
#define RSSI_ID            0xf101
#define ADC1_ID            0xf102
#define ADC2_ID            0xf103
#define BATT_ID            0xf104
#define SWR_ID             0xf105
#define ALT_FIRST_ID       0x0100
#define ALT_LAST_ID        0x010f

#define TELEMETRY_INIT    0
#define TELEMETRY_OK      1
#define TELEMETRY_KO      2

#define FRSKY_RX_PACKET_SIZE 9

uint8_t frskyRxBuffer[FRSKY_RX_PACKET_SIZE];
uint8_t telemetryState = TELEMETRY_INIT;

//alt in m
int16_t alt;

int8_t latsign;
int8_t lonsign;

//lat lon in decimal degree dd.ddddd
uint16_t lat_bp;
uint16_t lon_bp;
uint16_t lat_ap;
uint16_t lon_ap;

void processHubPacket(uint8_t id, uint16_t value)
{
  if (id > FRSKY_LAST_ID)
    return;

  switch (id) {
    case BARO_ALT_AP_ID:
      #ifdef VARIO
        alt = (int32_t)100 * value;
      #endif
      break;
    case GPS_ALT_AP_ID:
      #ifndef VARIO
        alt = value;
      #endif;
      break;
    case GPS_LONG_BP_ID:
      //if we have values we should have a sat fix here
      if (value){
        hasFix = true;
      } else {
        hasFix = false; 
      }
      lon_bp = value;
      break;
    case GPS_LONG_AP_ID:
      lon_ap = value;
      break;
    case GPS_LAT_BP_ID:
      //if we have values we should have a sat fix here
      if (value){
        hasFix = true;
      } else {
        hasFix = false; 
      }
      lat_bp = value;
      break;
    case GPS_LAT_AP_ID:
      lat_ap = value;
      break;
    case GPS_LONG_EW_ID:
      if (value == 'E'){
        lonsign = 1;
      }else {
        lonsign = -1;
      }
      hasLon = true;
      break;
    case GPS_LAT_NS_ID:
       if (value == 'N'){
        latsign = 1;
      }else {
        latsign = -1;
      }
      hasLat = true;
      break;
  }
}

bool checkSportPacket(uint8_t *packet)
{
  short crc = 0;
  for (int i=1; i<FRSKY_RX_PACKET_SIZE; i++) {
    crc += packet[i]; //0-1FF
    crc += crc >> 8; //0-100
    crc &= 0x00ff;
    crc += crc >> 8; //0-0FF
    crc &= 0x00ff;
  }
  return (crc == 0x00ff);
}

#define SPORT_DATA_U8(packet)   (packet[4])
#define SPORT_DATA_S32(packet)  (*((int32_t *)(packet+4)))
#define SPORT_DATA_U32(packet)  (*((uint32_t *)(packet+4)))
#define HUB_DATA_U16(packet)    (*((uint16_t *)(packet+4)))

void processSportPacket(uint8_t *packet)
{
  /* uint8_t  dataId = packet[0]; */
  uint8_t  prim   = packet[1];
  uint16_t appId  = *((uint16_t *)(packet+2));

  if (!checkSportPacket(packet))
    return;

  switch (prim)
  {
    case DATA_FRAME:

      if (appId == RSSI_ID) {
        //rssi = SPORT_DATA_U8(packet);
      }
      if (appId == SWR_ID) {
        //swr = SPORT_DATA_U8(packet);
      }
      else if ((appId >> 8) == 0) {
        // The old FrSky IDs
        uint8_t  id = (uint8_t)appId;
        uint16_t value = HUB_DATA_U16(packet);
        processHubPacket(id, value);
      }
      else if (appId == ALT_FIRST_ID) {
        #ifdef VARIO
          alt = SPORT_DATA_S32(packet);
        #endif
      }
      else if (appId == ALT_FIRST_ID) {
        #ifndef VARIO
          alt = SPORT_DATA_S32(packet);
        #endif
      }
      break;
  }
}

// Receive buffer state machine state enum
enum FrSkyDataState {
  STATE_DATA_IDLE,
  STATE_DATA_IN_FRAME,
  STATE_DATA_XOR,
};


int32_t getTargetLat(){
  return gpsToLong(latsign, lat_bp, lat_ap);
}

int32_t getTargetLon(){
  return gpsToLong(lonsign, lon_bp, lon_ap);
}

int16_t getTargetAlt(){
  return alt;
}

void encodeTargetData(uint8_t data){
  static uint8_t numPktBytes = 0;
  static uint8_t dataState = STATE_DATA_IDLE;

  if (data == START_STOP) {
    dataState = STATE_DATA_IN_FRAME;
    numPktBytes = 0;
  }
  else {
    switch (dataState) {
      case STATE_DATA_XOR:
        frskyRxBuffer[numPktBytes++] = data ^ STUFF_MASK;
        dataState = STATE_DATA_IN_FRAME;
        break;

      case STATE_DATA_IN_FRAME:
        if (data == BYTESTUFF)
          dataState = STATE_DATA_XOR; // XOR next byte
        else
          frskyRxBuffer[numPktBytes++] = data;
        break;
    }
  }

  if (numPktBytes == FRSKY_RX_PACKET_SIZE) {
    processSportPacket(frskyRxBuffer);
    dataState = STATE_DATA_IDLE;
  } 
}
#endif

