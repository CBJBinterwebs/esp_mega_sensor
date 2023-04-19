/**
 * Espressif Device Mega-Sensor
 * 
 * For Monitoring Environmental Conditions and Air Quality
 * 
 *Author: C Beck, 2021.  Updated 2023.
 */

//WIFI packages
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASS";
ESP8266WebServer server(80);  //define server to port 80

////////////////
//neopixelbus for air quality LED
#include <NeoPixelBus.h>
const uint16_t PixelCount = 1;  //1 LED
const uint8_t PixelPin = 16;    //make sure to set this to the correct pin for your board and setup
#define colorSaturation 128
// three element pixels, in different order and speeds
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(PixelCount, PixelPin);
//NeoPixelBus<NeoRgbFeature, Neo400KbpsMethod> strip(PixelCount, PixelPin);
// For Esp8266, the Pin is omitted and it uses GPIO3 due to DMA hardware use.
// There are other Esp8266 alternative methods that provide more pin options, but also have
// other side effects.
// for details see wiki linked here https://github.com/Makuna/NeoPixelBus/wiki/ESP8266-NeoMethods
RgbColor red(colorSaturation, 0, 0);
RgbColor orange(255, 128, 0);
RgbColor green(0, colorSaturation, 0);
RgbColor blue(0, 0, colorSaturation);
RgbColor white(colorSaturation);
RgbColor black(0);

HslColor hslRed(red);
HslColor hslOrange(orange);
HslColor hslGreen(green);
HslColor hslBlue(blue);
HslColor hslWhite(white);
HslColor hslBlack(black);

////////////////
/*
DHT11 
temp and humidity sensor setup
*/
#include "dht.h"
const int dht_pin = 14;  //set your DHT pin here
dht DHT;

////////////////
// BMP180 temp, humid, alt. sensor setup
// 5V ------ VCC
// GND ----- GND
// SDA ----- SDA
// SCL ----- SCL
#include <Arduino.h>
#include <Wire.h>
#include <BMP180I2C.h>
#define I2C_ADDRESS 0x77  //set i2c address of your BMP180
//create an BMP180 object using the i2c interface
BMP180I2C bmp180(I2C_ADDRESS);

////////////////
//SGP30 air quality sensor setup
#include "Adafruit_SGP30.h"
Adafruit_SGP30 sgp;
/* return absolute humidity [mg/m^3] with approximation formula
* @param temperature [°C]
* @param humidity [%RH]
*/
uint32_t getAbsoluteHumidity(float temperature, float humidity) {
  //approximation formula from Sensirion SGP30 Driver Integration chapter 3.15
  const float absoluteHumidity = 216.7f * ((humidity / 100.0f) * 6.112f * exp((17.62f * temperature) / (243.12f + temperature)) / (273.15f + temperature));  // [g/m^3]
  const uint32_t absoluteHumidityScaled = static_cast<uint32_t>(1000.0f * absoluteHumidity);                                                                 // [mg/m^3]
  return absoluteHumidityScaled;
}

////////////////
//SSD1306 oled screen setup, can also be used without screen with some altering of the code
#include <ACROBOTIC_SSD1306.h>

////////////////
//variables
int counter = 0;
float DHTtemp, DHThumid, BMPtemp, BMPpress, LEDstatus, SGPvoc, SGPco2, SGPh2, SGPeth, gasTemp, gasHumid, AVGtemp, AVGtempf, millibars = 0.0;
String errorMsg = "None";
String ipString = "";
boolean ledRed;

void setup() {
  //general setups
  Serial.begin(9600);  //for debugging over serial monitor
  //wait for serial connection to open (only necessary on some boards)
  while (!Serial);
  Wire.begin();
  delay(500);

  //wifi setups
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("...");

  //ssd1306 oled screen startup
  oled.init();  // Initialze SSD1306 OLED display
  oled.setFont(font5x7);
  oled.clearDisplay();   // Clear screen
  oled.setTextXY(0, 0);  // Set cursor position, start of line 0
  oled.putString("Environment");
  oled.setTextXY(1, 0);  // Set cursor position, start of line 1
  oled.putString("Sensor");
  oled.setTextXY(2, 0);  // Set cursor position, start of line 2
  oled.putString("Startup!");

  //wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    oled.setTextXY(3, 0);
    oled.putString("...");
    Serial.print(".");
    oled.setTextXY(3, 0);
    oled.putString("WiFi Connecting");
    delay(500);
  }
  //print wifi info
  Serial.println("...");
  Serial.print("Connected to ");
  oled.clearDisplay();
  oled.setTextXY(0, 0);
  oled.putString("Wifi Connected!");
  Serial.println(ssid);
  ipString = WiFi.localIP().toString();
  Serial.print("IP address: ");
  Serial.println(ipString);
  oled.setTextXY(1, 0);
  oled.putString("IP Address: ");
  oled.setTextXY(2, 0);
  oled.putString(ipString);
  delay(1000);
  oled.clearDisplay();

  //server mdns
  if (MDNS.begin("nurserysensors")) {
    Serial.println("MDNS responder started");
  }

  //declare routes
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);

  //start server
  server.begin();
  Serial.println("HTTP server started");

  //start neopixel, reset neopixel LED to 'off' state
  strip.Begin();
  strip.Show();

  //bmp180 pressure/temp init and setup
  //begin() initializes the interface, checks the sensor ID and reads the calibration parameters.
  if (!bmp180.begin()) {
    Serial.println("begin() failed. Check your BMP180 Interface and I2C address.");
    errorMsg = "err: check BMP180 wiring and address!";
    oled.setTextXY(3, 0);
    oled.putString(errorMsg);
    while (1);  //do nothing until an event is triggered
  }
  //reset sensor to default parameters.
  bmp180.resetToDefaults();
  //enable ultra high resolution mode for pressure measurements
  bmp180.setSamplingMode(BMP180MI::MODE_UHR);

  //sgp30 temp/pressure init and setup
  Serial.println("SGP30 test");
  oled.setTextXY(4, 0);
  oled.putString("SGP30 test...");
  if (!sgp.begin()) {
    Serial.println("Sensor not found :(");
    errorMsg = "SGP30 sensor not found!";
    oled.setTextXY(4, 0);
    oled.putString("err: SGP30 sensor not found!");
    while (1);  //do nothing until an event is triggered
  }
  Serial.print("Found SGP30 serial #");
  Serial.print(sgp.serialnumber[0], HEX);
  Serial.print(sgp.serialnumber[1], HEX);
  Serial.println(sgp.serialnumber[2], HEX);
  //if you have a baseline measurement from before you can assign it to start, to 'self-calibrate'
  //sgp.setIAQBaseline(0x8E68, 0x8F41);  //varies from sensor to sensor

  //test neopixelbus LED
  Serial.println("LED On...");
  oled.setTextXY(5, 0);
  oled.putString("LED test: ON");
  // set the colors,
  // if they don't match in order, you need to use NeoGrbFeature feature
  strip.SetPixelColor(0, white);
  // the following line demonstrates rgbw color support
  // if the NeoPixels are rgbw types the following line will compile
  // if the NeoPixels are anything else, the following line will give an error
  //strip.SetPixelColor(3, RgbwColor(colorSaturation));
  strip.Show();
  delay(2500);
  Serial.println("LED Off ...");
  oled.setTextXY(5, 0);
  oled.putString("LED test: OFF");
  // turn off the pixels
  strip.SetPixelColor(0, black);
  strip.Show();
  oled.clearDisplay();
}

void loop() {
  server.handleClient();
  MDNS.update();

  //read from dht temp/humid
  DHT.read11(dht_pin);
  DHThumid = DHT.humidity;
  Serial.print("DHT humidity = ");
  Serial.print(DHThumid);
  Serial.print("%  ");
  DHTtemp = DHT.temperature;
  Serial.print("DHT temperature = ");
  Serial.print(DHTtemp);
  Serial.println("C  ");
  delay(1000);  //need to wait 2 seconds before accessing the sensor again, 1 sec. delay here plus others downline

  //bmp180 pressure/temp
  //start a temperature measurement
  if (!bmp180.measureTemperature()) {
    Serial.println("could not start temperature measurement, is a measurement already running?");
    errorMsg = "could not get BMP180 temp!";
    BMPtemp = 0.0;
    return;
  }
  //wait for the measurement to finish. proceed as soon as hasValue() returned true.
  do {
    delay(100);
  } while (!bmp180.hasValue());
  BMPtemp = bmp180.getTemperature();
  Serial.print("BMP Temperature: ");
  Serial.print(BMPtemp);
  Serial.println(" degC");
  //start a pressure measurement. pressure measurements depend on temperature measurement, you should only start a pressure
  //measurement immediately after a temperature measurement.
  if (!bmp180.measurePressure()) {
    Serial.println("could not start pressure measurement, is a measurement already running?");
    errorMsg = "could not get BMP180 pressure!";
    BMPpress = 0.0;
    return;
  }
  //wait for the measurement to finish. proceed as soon as hasValue() returned true.
  do {
    delay(100);
  } while (!bmp180.hasValue());
  BMPpress = bmp180.getPressure();
  Serial.print("Pressure: ");
  Serial.print(BMPpress);
  Serial.println(" Pa");
  delay(500);
  //sgp30 gas sensor
  // If you have a temperature / humidity sensor, you can set the absolute humidity to enable the humditiy compensation for the air quality signals
  AVGtemp = tempAvg(DHTtemp, BMPtemp);
  //float humidity = DHThumid; // [%RH]
  sgp.setHumidity(getAbsoluteHumidity(AVGtemp, DHThumid));

  if (!sgp.IAQmeasure()) {
    Serial.println("Measurement failed");
    errorMsg = "SGP reading failed!";
    SGPvoc = 0.0;
    SGPco2 = 0.0;
    SGPh2 = 0.0;
    SGPeth = 0.0;
    return;
  }

  SGPvoc = sgp.TVOC;
  SGPco2 = sgp.eCO2;
  Serial.print("TVOC ");
  Serial.print(SGPvoc);
  Serial.print(" ppb\t");
  Serial.print("eCO2 ");
  Serial.print(SGPco2);
  Serial.println(" ppm");

  if (!sgp.IAQmeasureRaw()) {
    Serial.println("Raw Measurement failed");
    errorMsg = "SGP reading failed!";
    SGPvoc = 0.0;
    SGPco2 = 0.0;
    SGPh2 = 0.0;
    SGPeth = 0.0;
    return;
  }

  SGPh2 = sgp.rawH2;
  SGPeth = sgp.rawEthanol;
  Serial.print("Raw H2 ");
  Serial.print(SGPh2);
  Serial.print(" \t");
  Serial.print("Raw Ethanol ");
  Serial.print(SGPeth);
  Serial.println("");
  delay(50);
  counter++;

  if (counter == 30) {
    counter = 0;
    uint16_t TVOC_base, eCO2_base;
    if (!sgp.getIAQBaseline(&eCO2_base, &TVOC_base)) {
      Serial.println("Failed to get baseline readings");
      return;
    }
    Serial.print("****Baseline values: eCO2: 0x");
    Serial.print(eCO2_base, HEX);
    Serial.print(" & TVOC: 0x");
    Serial.println(TVOC_base, HEX);
  }

  AVGtempf = convertTemp(AVGtemp);
  buildScreen();
  delay(500);
}

//TODO: debug the screen of unnecessary characters
void buildScreen() {
  oled.setTextXY(0, 0);
  oled.putString("SensorIP:");  //9 chars
  oled.setTextXY(0, 9);
  oled.putString(ipString);
  oled.setTextXY(1, 0);
  oled.putString("Temperature: ");  //13 chars
  oled.setTextXY(1, 13);
  oled.putString(String(AVGtempf));
  oled.setTextXY(2, 0);
  oled.putString("Humidity: ");  //10 chars
  oled.setTextXY(2, 10);
  oled.putString(String(DHThumid));
  oled.setTextXY(3, 0);
  oled.putString("Pressure: ");  //10 chars
  oled.setTextXY(3, 10);
  oled.putString(String(convertPress(BMPpress)));
  oled.setTextXY(4, 0);
  oled.putString("VOC: ");  //5 chars
  oled.setTextXY(4, 5);
  oled.putString(String(SGPvoc));  //13 chars
  oled.setTextXY(5, 0);
  oled.putString("CO2: ");  //5 chars
  oled.setTextXY(5, 5);
  oled.putString(String(SGPco2));  //13 chars
  oled.setTextXY(6, 0);
  oled.putString("Err: ");  //5 chars
  oled.setTextXY(6, 5);
  oled.putString(errorMsg);

  //set led color based on air quality
  setLedColor();
}

//average the temp of the two sensors
float tempAvg(float temp1, float temp2) {
  float newTemp = ((temp1 + temp2) / 2);
  return newTemp;
}

//convert temp to farenheit
float convertTemp(float temp) {
  float convertedTemp = ((temp * (1.8)) + (32.0));
  return convertedTemp;
}

//convert pascals to millibars
float convertPress(float pressure) {
  millibars = (pressure / 100);
  return millibars;
}

//logic to set LED color
void setLedColor() {
  unsigned long previousMillis = 0;  //will store last time LED was updated
  const long interval = 1500;        //interval at which to blink (milliseconds)
  //no data from sensor
  if ((SGPvoc == 0.0) || (SGPco2 == 0.0)) {
    strip.SetPixelColor(0, white);
    strip.Show();
  }
  //good
  if ((SGPvoc < 220.01) && (SGPco2 < 700.01)) {
    strip.SetPixelColor(0, green);
    strip.Show();
  }
  //ok
  if (((SGPvoc >= 220.01) && (SGPvoc < 660.01)) || ((SGPco2 >= 700.01) && (SGPco2 < 1000.01))) {
    strip.SetPixelColor(0, orange);
    strip.Show();
  }
  //bad
  if (((SGPvoc >= 660.01) && (SGPvoc < 2200.01)) || ((SGPco2 >= 1000.01) && (SGPco2 < 1500.01))) {
    strip.SetPixelColor(0, red);
    strip.Show();
  }
  //hazardous, blink the light
  while ((SGPvoc >= 2200.01) || (SGPco2 >= 1500.01)) {
    strip.SetPixelColor(0, black);
    strip.Show();
    ledRed = false;
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      //save the last time you blinked the LED
      previousMillis = currentMillis;
      if (ledRed = false) {
        strip.SetPixelColor(0, red);
        strip.Show();
        ledRed = true;
      } else {
        strip.SetPixelColor(0, black);
        strip.Show();
        ledRed = false;
      }
    }
  }
}

//build html
String prepareHtmlPage() {
  String htmlPage;
  htmlPage.reserve(1024);  //prevent ram fragmentation
  htmlPage = F("<!DOCTYPE HTML>"
               "<html>"
               "<head>"
               "<meta charset=\"utf-8\" />"
               "<meta http-equiv=\"x-ua-compatible\" content=\"ie=edge\">"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />"
               "<title>Nursery Sensors</title>"
               "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/foundation-sites@6.6.3/dist/css/foundation.min.css\" integrity=\"sha256-ogmFxjqiTMnZhxCqVmcqTvjfe1Y/ec4WaRj/aQPvn+I=\" crossorigin=\"anonymous\">"
               "<style>"
               ".pricing-table {background-color: #fefefe;border: solid 1px #cacaca;width: 100%;text-align: center;list-style-type: none;}"
               ".pricing-table li {border-bottom: dotted 1px #cacaca;padding: 0.875rem 1.125rem;}"
               ".pricing-table li:last-child {border-bottom: 0;}"
               ".pricing-table .title {background-color: #0a0a0a;color: #fefefe;border-bottom: 0;}"
               ".pricing-table .price {background-color: #e6e6e6;font-size: 2rem;border-bottom: 0;}"
               ".pricing-table .description {color: #8a8a8a;font-size: 80%;}"
               ".pricing-table :last-child {margin-bottom: 0;}"
               "</style>"
               "</head>"
               "<div class=\"grid-container fluid\">"
               "<div class=\"grid-x grid-margin-x\">"
               "<div class=\"cell small-12 medium-6 large-4\">"
               "<ul class=\"pricing-table\">"
               "<li class=\"title\">DHT Temperature</li>"
               "<li class=\"price\">");
  htmlPage += String(convertTemp(DHTtemp));
  htmlPage += F(" &#x2109;</li>"
                //ignore
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">DHT Humidity</li>"
                "<li class=\"price\">");
  htmlPage += String(DHThumid);
  htmlPage += F(" %</li>"
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">BMP Temperature</li>"
                "<li class=\"price\">");
  htmlPage += String(convertTemp(BMPtemp));
  htmlPage += F(" &#x2109;</li>"
                "</ul>"
                "</div>"
                "</div>"
                "<div class=\"grid-x grid-margin-x\">"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">BMP Pressure</li>"
                "<li class=\"price\">");
  htmlPage += String(convertPress(BMPpress));
  htmlPage += F(" millibars</li>"
                //ignore
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">SGP VOC</li>"
                "<li class=\"price\">");
  htmlPage += String(SGPvoc);
  htmlPage += F(" ppb</li>"
                "<li class=\"description\">0-220 is GOOD</li>"
                "<li class=\"description\">220-660 is MODERATE</li>"
                "<li class=\"description\">660-2200 is UNHEALTHY</li>"
                "<li class=\"description\">2200+ is HAZARDOUS</li>"
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">SGP cO2</li>"
                "<li class=\"price\">");
  htmlPage += String(SGPco2);
  htmlPage += F(" ppm</li>"
                "<li class=\"description\"><600 is EXCELLENT</li>"
                "<li class=\"description\">600-800 is GOOD</li>"
                "<li class=\"description\">800-1000 is MODERATE</li>"
                "<li class=\"description\">1000-1500 is VENTILATION RECOMMENDED</li>"
                "<li class=\"description\">1500-2100 is BAD! VENTILATION REQUIRED</li>"
                "</ul>"
                "</div>"
                "</div>"
                "<div class=\"grid-x grid-margin-x\">"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">SGP H2</li>"
                "<li class=\"price\">");
  htmlPage += String(SGPh2);
  htmlPage += F(" ppm</li>"
                //ignore
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">SGP Ethanol</li>"
                "<li class=\"price\">");
  htmlPage += String(SGPeth);
  htmlPage += F(" ppm</li>"
                "</ul>"
                "</div>"
                "<div class=\"cell small-12 medium-6 large-4\">"
                "<ul class=\"pricing-table\">"
                "<li class=\"title\">Errors</li>"
                "<li class=\"price\">");
  htmlPage += errorMsg;
  htmlPage += F("</li>"
                "</ul>"
                "</div>"
                "</div>"
                "</div>");
  htmlPage += F("</html>"
                "\r\n");
  return htmlPage;
}

//functions after http calls
void handleRoot() {
  server.send(200, "text/html", prepareHtmlPage());
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}
