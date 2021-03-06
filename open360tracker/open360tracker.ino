/* Open Source 360° continuous rotation antenna tracker software
 * created by Samuel Brucksch
 *
 * Digital Smooth method from Arduino playground: http://playground.arduino.cc/Main/DigitalSmooth
 * Local GPS uses parser from TinyGPS: http://arduiniana.org/libraries/tinygps/
 * FPV Community thread: http://fpv-community.de/showthread.php?46439-360%B0-Community-Antennentracker-mit-Schleifring-Wer-macht-mit
 */
#include <EEPROM.h>
#include "defines.h"
#include "config.h"
#include <Wire.h>
#include "compass.h"
#include "servos.h"
#include "inttypes.h"
#include "telemetry.h"

#ifdef LCD_DISPLAY
  //download from https://bitbucket.org/fmalpartida/new-liquidcrystal/wiki/Home
  #include <LiquidCrystal_I2C.h>
  LiquidCrystal_I2C lcd(0x27,2,1,0,4,5,6,7);
  char lcd_str[16];
  long lcd_time;
#endif

unsigned long time;
unsigned long calib_timer;

void calcTilt();
void getError();
void calculatePID();

#ifndef MFD
  uint16_t getHeading(geoCoordinate_t *a, geoCoordinate_t *b);
#endif

//PID stuff
long Error[11];
long Accumulator;
long PID;
uint8_t Divider = 15;
int PWMOutput;
long Dk;
#ifdef LOCAL_GPS
  void initGps();
#endif


#ifdef LOCAL_GPS
  #include <SoftwareSerial.h>
  SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
  
  #include <TinyGPS.h>
  TinyGPS gps;
#endif

//distance in meter
uint16_t distance;
// The target position (lat/lon)
geoCoordinate_t targetPosition;
// The tracker position (lat/lon)
geoCoordinate_t trackerPosition;

void setup()
{
  #ifdef LCD_DISPLAY
    lcd.begin(16,2);
    lcd.setBacklightPin(3,POSITIVE);
    lcd.setBacklight(HIGH);
    lcd.home();
    lcd.print(" open360tracker ");
    lcd.setCursor ( 0, 1 );
    lcd.print("   version 0.1  ");
  #endif
  
  hasLat = false;
  hasLon = false;
  hasAlt = false;
  HOME_SET = false;
  SETTING_HOME = false;
  PREVIOUS_STATE = true;
  TRACKING_STARTED = false;
  CURRENT_STATE = true;
  gotNewHeading = false;
  testMode = false;

  Serial.begin(BAUD);
  
  #ifdef DEBUG
    Serial.println("Setup start");
  #endif
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(HOME_BUTTON, INPUT);
  pinMode(CALIB_BUTTON, INPUT);
  
  //LED off
  digitalWrite(LED_PIN, LOW);
  
  //enable internal pullups
  digitalWrite(HOME_BUTTON, HIGH);
  digitalWrite(CALIB_BUTTON, HIGH);
  
  Wire.begin();
  
  // init pan/tilt servos controlled via hardware pwm
  #ifdef DEBUG
    Serial.println("Init Servos");
  #endif
  initServos();
  
  #ifdef DEBUG
    Serial.println("Init Compass");
  #endif
  initCompass();
  
  #ifdef LOCAL_GPS
    #ifdef DEBUG
      Serial.println("Init local GPS");
    #endif
    gpsSerial.begin(GPS_BAUDRATE);
    //let gps serial initialize before we configure it
    delay(20);
    initGps();
  #endif


  
  #ifdef LCD_DISPLAY
    delay(2000);
    
    lcd.clear();
    lcd.setCursor(0,0);
    sprintf(lcd_str, "HDG:%03u AZI:%03u", 0, 0);
    lcd.print(lcd_str);
    lcd.setCursor(0,1);
    sprintf(lcd_str, "A:%05d D:%05u", 0, 0);
    lcd.print(lcd_str);
    lcd_time = millis();
  #endif

  time = millis();
  
  
  #ifdef DEBUG
    Serial.println("Setup finished");
  #endif
}

void loop()
{
  //TODO change to telemetry serial port
  if (Serial.available() > 1)
  {
    uint8_t c = Serial.read();
    
    //TODO remove after debugging
    if (c == 'C'){
        calibrate_compass();
    }   
    encodeTargetData(c);
  }
  
  #ifdef LCD_DISPLAY
  if (millis() > lcd_time){
    lcd.setCursor(0,0);
    sprintf(lcd_str, "HDG:%03u AZI:%03u", trackerPosition.heading/10, targetPosition.heading/10);
    lcd.print(lcd_str);
    lcd.setCursor(0,1);
    sprintf(lcd_str, "A:%05d D:%05u", targetPosition.alt, distance);
    lcd.print(lcd_str);
    lcd_time = millis() + 200;
  }
  #endif

  if (hasAlt){
    targetPosition.alt = getTargetAlt();
    
    
    // mfd has all the data at once, so we do not have to wait for valid lat/lon
    #ifdef MFD
      distance = getDistance();
      targetPosition.heading = getAzimuth() * 10;
      #ifdef DEBUG
        Serial.print("Target alt: ");Serial.print(targetPosition.alt);
        Serial.print(" Target distance: ");Serial.print(distance);
        Serial.print(" Target heading: ");Serial.print(targetPosition.heading/10);
        Serial.print(" Tracker heading: ");Serial.println(trackerPosition.heading/10);
      #endif
    #else
      #ifdef DEBUG
        Serial.print("Target alt: ");Serial.println(targetPosition.alt);
      #endif
    #endif
    
    hasAlt = false;
  }
  
  #ifndef MFD
    //only calculate distance and heading when we have valid telemetry data
    if (hasLat && hasLon){
      targetPosition.lat = getTargetLat();
      targetPosition.lon = getTargetLon();    
      // calculate distance without haversine. We need this for the slope triangle to get the correct pan value
      distance = sqrt( (targetPosition.lat - trackerPosition.lat) * (targetPosition.lat - trackerPosition.lat) 
                        + (targetPosition.lon - trackerPosition.lon) * (targetPosition.lon - trackerPosition.lon) );
      targetPosition.heading = getHeading(&trackerPosition, &targetPosition)*10;
      #ifdef DEBUG
        // TODO correct debug output for lat/lon
        Serial.print("Lat: "); Serial.print(targetPosition.lat); 
        Serial.print(" Lon: "); Serial.print(targetPosition.lon);
        Serial.print(" Distance: "); Serial.print(distance);
        Serial.print(" Heading: "); Serial.print(trackerPosition.heading/10);
        Serial.print(" Target Heading: "); Serial.print(targetPosition.heading/10);
      #endif
      hasLat = false;
      hasLon = false;
    }
  #endif
  
  // TODO we should change this to a 50hz loop here, read the compass and update the servos
  // afterwards. 
  // As a result there will be no overhead in reading compass values and the compass is read
  // when it makes sense, because the servos can only be refreshed at 50hz.
  
  // refresh rate of compass is 75Hz -> 13.333ms to refresh the data
  // we update the heading every 14ms to get as many samples into the smooth array as possible
  if (millis() > time){
    time = millis() + 14;
    trackerPosition.heading = getHeading();
    gotNewHeading = true;
    #ifdef DEBUG
      //Serial.print("Current heading: ");Serial.println(trackerPosition.heading/10);
    #endif
  }
  
  CURRENT_STATE = digitalRead(CALIB_BUTTON);
  if (CURRENT_STATE != PREVIOUS_STATE){
    digitalWrite(LED_PIN, !CURRENT_STATE);
     //pin changed
     if (!CURRENT_STATE && calib_timer == 0){
        calib_timer = millis();
     } else if (CURRENT_STATE && millis() - calib_timer < 4000){
       //button not pressed long enough
       calib_timer = 0; 
     } else if (CURRENT_STATE && millis() - calib_timer > 4000){
       //start calibration routine if button pressed > 4s and released
       //cli();
       calibrate_compass();
       calib_timer = 0; 
       //sei();
     }
     PREVIOUS_STATE = CURRENT_STATE;
  }
  
  #ifndef LOCAL_GPS
    //only needed if no local gps
    if (!digitalRead(HOME_BUTTON)){
      //set home
      #ifndef MFD
        trackerPosition.alt = getTargetAlt();
        trackerPosition.lat = getTargetLat();
        trackerPosition.lon = getTargetLon();
        HOME_SET = true;
      #else
      //MFD protocol: set home must be pressed on driver!
      #endif
      
    }
  #else
    if (gpsSerial.available()){
      uint8_t c = gpsSerial.read();
      #ifdef DEBUG
        Serial.write(c);
      #endif
      if (gps.encode(c)){
        gps.get_position(&trackerPosition.lat,&trackerPosition.lon);
        digitalWrite(LED_PIN, HIGH);
        if(gps.altitude() != TinyGPS::GPS_INVALID_ALTITUDE) {
          trackerPosition.alt = (int16_t)gps.altitude();
        }
      }
      else{
        digitalWrite(LED_PIN, LOW); 
      }
    }
  #endif
  
  #ifdef MFD
    if (SETTING_HOME){
       HOME_SET = true;
       SETTING_HOME = 0;
    }
    
    if (!HOME_SET && testMode && gotNewHeading){
        distance = getDistance();
        targetPosition.alt = getTargetAlt();
        targetPosition.heading = getAzimuth()*10;
        
        getError();
        calculatePID();
        SET_PAN_SERVO_SPEED(PWMOutput);
        calcTilt(); 
        gotNewHeading = false;
    }
  #endif
  
  #if (START_TRACKING_DISTANCE > 0)
    //Only track if tracking process started.
    if(!TRACKING_STARTED) {
      //if plane is START_TRACKING_DISTANCE meter away from tracker start tracking process.
      if (START_TRACKING_DISTANCE <= distance && HOME_SET){
        TRACKING_STARTED = true;
      }
    } else {
  #else
    //Only track if home ist set.
    if (HOME_SET){
  #endif
      digitalWrite(LED_PIN, HIGH);
      // only update pan value if there is new data
      if( gotNewHeading ) {
        getError();       // Get position error
        calculatePID();   // Calculate the PID output from the error
        SET_PAN_SERVO_SPEED(PWMOutput);
        gotNewHeading = false;
        
        calcTilt(); 
      }
    } 
}

//Tilt angle alpha = atan(alt/dist) 
void calcTilt()
{
  uint16_t alpha = 0;
  //prevent division by 0
  if (distance == 0){
    alpha = 90;
  }
  else {
    alpha = toDeg(atan((targetPosition.alt - trackerPosition.alt)/distance));
  }
  //just for current tests, later we will have negative tilt as well
  if (alpha < 0)
    alpha = 0;
  
  SET_TILT_SERVO_SPEED(map(alpha, 0, 90, TILT_0, TILT_90));
}

void getError(void)
{
  // shift error values
  for(byte i = 0; i < 10; i++){
    Error[i+1] = Error[i];
  }

  int16_t delta = targetPosition.heading - trackerPosition.heading;
  if (delta > 1800){
    delta -= 3600; 
  } 
  else if (delta < -1800){
    delta += 3600;
  }  
  // load new error into top array spot  
  Error[0] = delta;
}

void calculatePID(void)
{
  // Calculate the PID  
  PID = Error[0] * P;     // start with proportional gain
  Accumulator += Error[0];  // accumulator is sum of errors
  if(Accumulator>5000)
    Accumulator=5000;
   if(Accumulator<-5000)
    Accumulator=-5000; 
  PID += I * Accumulator; // add integral gain and error accumulation
  Dk=D * (Error[0]-Error[10]);
  PID += Dk; // differential gain comes next
  PID = PID >> Divider; // scale PID down with divider
  // limit the PID to the resolution we have for the PWM variable
  if(PID >= 500)
    PID = 500;
  if(PID <= -500)
    PID = -500;
  if (Error[0] > 10){
    PWMOutput = PAN_0+PID;
  }else if(Error[0] < -10){
    PWMOutput = PAN_0+PID;
  } else {
    PWMOutput = PAN_0;
  }
}

#ifndef MFD
uint16_t getHeading(geoCoordinate_t *a, geoCoordinate_t *b)
{
  // get difference between both points
  int32_t lat = a->lat - b->lat;
  int32_t lon = a->lon - b->lon;
  
  // calculate angle in radians and convert to degrees
  int16_t angle = atan2(lat, lon) * (180 / PI);
  
  // shift from -180/180 to 0/359
  return (uint16_t)(angle < 0 ? angle + 360 : angle);
}
#endif

#ifdef LOCAL_GPS
void initGps(){
  #ifdef MTK
    //set to 1Hz
    gpsSerial.println("$PMTK220,1000*1F");
    //delay for ack/nack as softserial cannot read and write at the same time
    delay(20);

    //only enable GGA sentences
    gpsSerial.print("$PMTK314,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*29");
    //delay for ack/nack as softserial cannot read and write at the same time
    delay(20);
  #endif
}
#endif

