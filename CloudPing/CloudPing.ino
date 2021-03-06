#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RH_RF69.h>
#include <Adafruit_GPS.h>
#include "BMP.h"
#include "gps.h"
#include "config.h"

 File logfile;
 File gpsfile;
 File initfile;

#if DEBUG
#define VPRINT(data) Serial.print(data); initfile.print(data);
#define VPRINTLN(data) Serial.println(data); initfile.println(data);
#else
#define VPRINT(data)
#define VPRINTLN(data)
#endif

//Sensoren initialisieren
//Druck & Temperatur
double height;
BMP180 bmp;

//Feinstaubsensor - Code von waveshare.com
float density, voltage;
int   adcvalue;

//GPS Modul
SoftwareSerial gpsSerial(GPS_RX, GPS_TX);
Adafruit_GPS GPS(&gpsSerial);
boolean usingInterrupt = true;

SIGNAL(TIMER0_COMPA_vect) {
  char c = GPS.read();
}

// radio
uint8_t key[] = { AES_KEY };
RH_RF69 rfm69(RADIO_CS, RADIO_INT);

// internal temperature
OneWire oneWire(DS_WIRE_BUS);
DallasTemperature ds_sensor(&oneWire);

long datacounter = 0;

void beep_long() {
  digitalWrite(PIEZO, HIGH);
  delay(2000);
  digitalWrite(PIEZO, LOW);
}

void beep_short() {
  digitalWrite(PIEZO, HIGH);
  delay(500);
  digitalWrite(PIEZO, LOW);
}

void setup() {
  
  Serial.begin(115200);

  VPRINT("CLOUDPING VERSION: ");
  VPRINTLN(VERSION);
  VPRINTLN("Init starting");

  //Piezo init
  VPRINT("Initializing piezo ");
  pinMode(PIEZO, OUTPUT);
  VPRINT("[OK]\nRunning piezo self test ");
  // beep_short();
  delay(1000);
  VPRINTLN("[OK]");
  pinMode(SDS, INPUT);

  // SD init
  VPRINT("Init: SD ");
  if (!SD.begin(SD_PIN)) {
    VPRINTLN("[FAILED]");
    beep_long();
    while (1);
  }
  VPRINTLN("[OK]");

  if (SD.exists("log.txt") && DELETE_OLD) {
    VPRINT("Deleting old log...");
    SD.remove("log.txt");
    VPRINTLN("Done");
  }

  if (SD.exists("init.txt") && DELETE_OLD) {
    VPRINT("Deleting old log...");
    SD.remove("init.txt");
    VPRINTLN("Done");
  }

  initfile = SD.open("setup.txt", FILE_WRITE);

  //BMP180 init
  VPRINT("Initializing BMP180 ");
  if (!bmp.begin()) {
    VPRINTLN("[FAILED]");
    beep_long();
    delay(1000);
    beep_long();
    while (1);
  }
  bmp.calibrate();
  VPRINTLN("[OK]");

  VPRINT("Initializing fine dust sensor ");
  //Feinstaubsensor init
  pinMode(VOUT, INPUT);
  pinMode(ILED, OUTPUT);    //LED als Output
  digitalWrite(ILED, LOW);  //LED standardmäßig auf 0
  VPRINTLN("[OK]");

  // radio init
  VPRINT("Initializing RFM69 ");
  if (!rfm69.init()) {
    VPRINTLN("[FAILED]");
    beep_long();
    delay(1000);
    beep_long();
    delay(1000);
    beep_long();
    while (1);
  }
  rfm69.setFrequency(433.0);
  rfm69.setTxPower(20);
  rfm69.setEncryptionKey(key);
  rfm69.setModemConfig(RH_RF69::GFSK_Rb250Fd250);
  VPRINTLN("[OK]");

  // internal ds
  VPRINT("Initializing Internal DS ");
  ds_sensor.begin();
  VPRINTLN("[OK]");

  VPRINT("Initializing GPS ");
  //GPS init
  GPS.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);
  GPS.sendCommand(PGCMD_ANTENNA);
  usingInterrupt = useInterrupt(true);
  VPRINTLN("[OK]");
  VPRINTLN("GPS fix attempt 1");

  boolean fix = false;
  long timeSinceStart = 0;

  while (!fix) {
    delay(10);
    timeSinceStart += 10;
    int attempt = timeSinceStart / 10 + 1;
    VPRINTLN("Attempt: " + (String)attempt);
    if (GPS.newNMEAreceived()) {
       GPS.parse(GPS.lastNMEA());
    }
    fix = GPS.fix;
    if (timeSinceStart > GPS_TIMEOUT) {
      break;
    }
  }
  if (fix) {
    VPRINTLN("Success");
  } else {
    VPRINTLN("TIMEOUT");
    beep_long();
    delay(1000);
    beep_long();
    delay(1000);
    beep_long();
    delay(1000);
    beep_long(); 
  }
  
  delay(500);
  beep_short();
  
  initfile.close();
}

void loop() {
  
  while(!digitalRead(SDS)){
    VPRINTLN("SDS not active");
    delay(1000);
  }
  
  //Variablen deklarieren
  double temperature, pressure;
  String timestamp;
  int maxHeight = 0;
  

  //BMP180
  bmp.getTemperature(temperature);
  bmp.getPressure(pressure, temperature);
  height = bmp.getHeight(pressure);
  if (height > maxHeight) {
    maxHeight = height;
  } else if (height < maxHeight - 700) {
    digitalWrite(PIEZO, HIGH);
  }

  //Feinstaubsensor
  digitalWrite(ILED, HIGH);
  delayMicroseconds(280);
  adcvalue = analogRead(VOUT);
  digitalWrite(ILED, LOW);
  voltage = (SYS_VOLTAGE / 1023.0) * adcvalue * 11;  //Spannung wird errechnet

  //Vorfiltern - Wenn die gemessene Spannung zu niedrig ist, wird davon ausgegangen, dass kein Feinstaub vorhanden ist
  if (voltage >= NO_DUST_VOLTAGE) {
    voltage -= NO_DUST_VOLTAGE;
    density = voltage * COV_RATIO;
  } else {
    density = 0;
  }

  datacounter++;
  ds_sensor.requestTemperatures();

  //Datenpaket wird erstellt
  String pload = "T:" + (String)temperature + ",P:" + (String)pressure + ",D:" + (String)density + ",Vo:" + (String)voltage + ",DC:" + (String)datacounter + ",IT:" + (String)ds_sensor.getTempCByIndex(0);
  VPRINTLN(pload);
  char payload[pload.length()];
  pload.toCharArray(payload, pload.length());

  logfile = SD.open("log.txt", FILE_WRITE);
  logfile.println(pload);
  logfile.close();
  
  rfm69.send((uint8_t *)payload, sizeof(payload));
  
  String pload_gps = "GPS,";
  
  if (GPS.newNMEAreceived()) {
    VPRINTLN("got nema parsing");
    GPS.parse(GPS.lastNMEA());
    VPRINTLN("parsed");
    pload_gps += "LONG:"+(String)GPS.longitude+",";
    pload_gps += "LAT:" +(String)GPS.longitude+",";
    pload_gps += "SPE:" +(String)GPS.speed+",";
    pload_gps += "ALT:" +(String)GPS.altitude+",";
    pload_gps += "TIM:" +(String)GPS.hour +";"+ (String)GPS.minute +";"+(String)GPS.seconds+",";
  }
  pload_gps += "DC:" +(String)datacounter+",";
  
  char payload_gps[pload_gps.length()];
  pload_gps.toCharArray(payload_gps, pload_gps.length());

  delay(200);

  gpsfile = SD.open("gps.txt", FILE_WRITE);
  gpsfile.println(pload_gps);
  gpsfile.close();
  
  VPRINTLN(pload_gps);
  
  rfm69.send((uint8_t *)payload_gps, sizeof(payload_gps));
  
  delay(200);
}
