/*
   Arduino Sketch for a Sparkfun ESP8266 Thing Dev board
   to read the ambient temperature and humidity from a DHT22 chip
   and send that temperature and humidity to Weather Underground.

   Copyright (c) 2020 Bradford Needham
   { @bneedhamia , https://www.needhamia.com }

   Licensed under GPL V2
   a copy of which should have been supplied with this file.


   RHT03 (DHT22) parts and datasheets are available
   from Sparkfun at https://www.sparkfun.com/products/10167
   and Adafruit at https://www.adafruit.com/product/385
*/
//345678901234567890123456789312345678941234567895123456789612345678971234567898

/*
   Flags to change the program behavior:

   TEST_URL_ENCODING = uncomment this line to only
     test the urlencodedcat() function.
     Used only during development of urlencodedcat().
     Comment this line out for normal Sketch behavior.
*/
//#define TEST_URL_ENCODING true

/*
    NOTE: because the ESP8266 WiFi operates in the background,
     setup() and loop() must not take a long time.
     In particular, never call any delay() greater than
     a few hundred milliseconds.
*/
#include <ESP8266WiFi.h> // Defines WiFi, the WiFi controller object
#include <WiFiClientSecure.h>
#include <EEPROM.h>   // NOTE: ESP8266 EEPROM library differs from Arduino's.
#include <dht.h> // DHT22 library, modified to be portable.
// Library source is at https://github.com/bneedhamia/DHT

/*
   Web Site specific constants.
     host = the hostname of the server to connect to.
     port = the HTTPS port to connect to (443 is the default).
     sslFingerprint = the SHA1 fingerprint of the SSL Certificate
       of the web site we will be connecting to.
     httpProtocol = the protocol to use. Some sites prefer HTTP/1.1.
       This Sketch uses HTTP/1.0 to avoid getting a response that is
       Transfer-encoding: chunked
       which is hard to parse.
     url = the url, less https:// and the server name.
     httpAgent = a string to identify our client software.
       This Sketch uses the name of the Github Repository of this project.
       Replace this with whatever you like (no spaces).

     To Find the Fingerprint of a site:
     - Browse to an https: page on the desired server.
       E.g., https://weatherstation.wunderground.com
       (you may get a 404 error; that's ok).
     - Copy the certificate from that page. There are instructions
       on how to do this in various browsers. Search for
       "read the certificate from a website in Chrome" (without quotes)
       or whatever web client you prefer.
     - Save the certificate in Base-64 encoded X.509
     - In git bash (or in a linux terminal window), type
       openssl x509 -noout -fingerprint -sha1 -inform pem -in certificate-file.cer
       Where "certificate-file.cer" is the filename of the certificate you saved.
     - in the response, copy the colon-separated set of numbers.
     - paste them in the sslFingerprint initializer below.
     - replace each ":" character in sslFingerprint with a space.
     Note: when the site's certificate expires and is updated, you will need to
     update the code here with the new SSL certificate signature.

     The Weather Underground Personal Weather Station upload protocol
     is specified at https://support.weather.com/s/article/PWS-Upload-Protocol
*/
const char *host = "weatherstation.wunderground.com";
const int port = 443;
const char *sslFingerprint
  = "50 22 08 9D DC 3C 2D FC 21 D4 22 30 07 8B 2E 68 63 47 20 02";
// Certificate expires August 25, 2021, but it may be replaced earlier.
const char *httpProtocol = "HTTP/1.0";
const char *url = "/weatherstation/updateweatherstation.php";
const char *httpAgent = "MAX31820WeatherStation";

/*
   Pins:

   DHT22 pin 1 --> power input, connected to Arduino 5V
     Pin 1 is the left pin as the open grid faces you and the pins point down.
   DHT22 pin 2 --> signal output, to a level converter
     which converts the 5V signal to 3.3V,
     then connects to the Arduino (connect to DHT22_DATA)
   DHT22 pin3 & 4 --> Arduino GND. Both 3 and 4 are grounded
     because some DHT22's expect pin 3 to be grounded, while others
     expect pin 4 to be grounded.
   Normally, you'd wire a 4.7K ohm resistor
     between DHT22 pin 2 and DHT22 pin 1 (5V), but you don't here
     because the level shifter contains an internal pull up resistor.

   PIN_LED_L = LOW to light the on-board LED.
     The _L suffix indicates that the pin is Active-Low.
     Active-Low because that's how the ESP8266 Thing Dev board
     on-board LED is wired.
     When the LED is steady-on, we're waiting to connect to the WiFi.
     When the LED is blinking, we've encountered a non-recoverable error.
     When the LED is off, things are working correctly.

   Note about the ESP8266 Thing Dev board I/O pins:
   several pins are unsuitable as data pins:
     0 is connected to an on-board 10K ohm pull-up resistor.
     2 is connected to an on-board 10K ohm pull-up resistor.
     5 is connected to an on-board LED and 220 ohm resistor to Vcc.
     15 is connected to an on-board 10K pull-down resistor.
     
   Pin 16 does work with the DHT22 library, so it can be used as a data pin.
*/
const int PIN_LED_L = 5;
const int DHT22_DATA = 4;

/*
   Time (milliseconds) per attempt to read and report the temperatures.
   Set this to about 5 minutes (1000L * 60L * 5L) for normal operation,
   so you don't overwhelm the server.
*/
const long MS_PER_TEMPERATURE_REQUEST = 1000L * 60L * 5L;

/*
   The EEPROM layout, starting at START_ADDRESS, is:
   String[EEPROM_WIFI_SSID_INDEX] = WiFi SSID. A null-terminated string.
     NOTE: The ESP8266 cannot connect to 5 GHz WiFi; only to 2.5 GHz WiFi.
   String[EEPROM_WIFI_PASS_INDEX] = WiFi Password. A null-terminated string
   String[EEPROM_STATION_ID_INDEX] =
     the HTTPS POST Weather Underground station ID parameter to send.
   String[EEPROM_STATION_KEY_INDEX] =
     the HTTPS POST Weather Underground station key parameter to send.
   EEPROM_END_MARK

   To create a Station ID and Key for your particular weather station,
   visit https://www.wunderground.com/member/devices/new

   To write these values, use the Sketch write_eeprom_strings.
   See https://github.com/bneedhamia/write_eeprom_strings
   Modify that Sketch, replacing the initialization of STRING_NAME[] with
   the following lines:

  const char *STRING_NAME[] = {
  "SSID",
  "Password",
  "stationId",
  "stationKey",
  0
  };

*/
const int START_ADDRESS = 0;      // The first EEPROM address to read from.
const byte EEPROM_END_MARK = 255; // marks the end of the data we wrote to EEPROM
const int EEPROM_MAX_STRING_LENGTH = 120; // max string length in EEPROM
const int EEPROM_WIFI_SSID_INDEX = 0;
const int EEPROM_WIFI_PASS_INDEX = 1;
const int EEPROM_STATION_ID_INDEX = 2;
const int EEPROM_STATION_KEY_INDEX = 3;

/*
   The states of the state machine that loop() runs.

   state = the current state of the machine. A STATE_* value.
   stateBegunMs = if needed, the time (millis()) we entered this state.

   STATE_ERROR = an unrecoverable error has occurred. We stop.
   STATE_WAITING_FOR_WIFI = we're waiting for the WiFi to connect
     with the Access Point.
   STATE_TO_READ_TEMPERATURE = We're ready to read the temperature and humidity.
     stateBegunMs = time (millis()) we entered this state.
   STATE_WAITING_FOR_NEXT_READ = we're waiting to request temperature again.
*/

const uint8_t STATE_ERROR = 0;
const uint8_t STATE_WAITING_FOR_WIFI = 1;
const uint8_t STATE_TO_READ_TEMPERATURE = 2;
const uint8_t STATE_WAITING_FOR_NEXT_READ = 3;
uint8_t state;
unsigned long stateBegunMs = 0L;

/*
   dht22 manages I/O with the DHT22 temperature/Humidity sensor.
   It caches temperature(C) and Humidity(%).
*/
dht dht22;

/*
   dewPointC = the calculated dew point, in degrees C.
 */
float dewPointC;

/*
   WiFi access point parameters, read from EEPROM.

   wifiSsid = SSID of the local WiFi Access Point to connect to.
   wifiPassword = Password of the Access Point.
*/
char *wifiSsid;
char *wifiPassword;

/*
   HTTPS POST private parameters, read from EEPROM.
   stationId = the POST 'ID' parameter.
   stationKey = the POST 'PASSWORD' parameter.
*/
char *stationId;
char *stationKey;


// Called once automatically on ESP8266 Reset.
void setup() {
  Serial.begin(9600);
  delay(100); // give the Serial port time to power up.
  Serial.println(F("Reset."));

#if defined(TEST_URL_ENCODING)
  testUrlencodedcat();
  state = STATE_ERROR;
  return;
#endif // defined(TEST_URL_ENCODING)

  // Set up our pins.

  pinMode(PIN_LED_L, OUTPUT);
  digitalWrite(PIN_LED_L, HIGH); // ESP8266 Thing Dev LED is Active Low.

  // The DHT pin is managed by the DHT22 library.

  // Read the wifi parameters from EEPROM, if they're there.
  wifiSsid = readEEPROMString(START_ADDRESS, EEPROM_WIFI_SSID_INDEX);
  wifiPassword = readEEPROMString(START_ADDRESS, EEPROM_WIFI_PASS_INDEX);
  if (wifiSsid == 0 || wifiPassword == 0) {
    Serial.println(F("EEPROM not initialized."));

    state = STATE_ERROR;
    return;
  }

  // Read our Weather Underground station credentials from EEPROM
  stationId = readEEPROMString(START_ADDRESS, EEPROM_STATION_ID_INDEX);
  stationKey = readEEPROMString(START_ADDRESS, EEPROM_STATION_KEY_INDEX);
  if (stationId == 0 || stationKey == 0) {
    Serial.println(F("EEPROM not initialized."));

    state = STATE_ERROR;
    return;
  }

  // Finally, do the one-time WiFi setup and request connection
  WiFi.mode(WIFI_STA);    // Station (Client), not soft AP or dual mode.
  WiFi.setAutoConnect(false); // don't connect until we call .begin()
  WiFi.setAutoReconnect(true); // if the connection drops, reconnect.

  Serial.print(F("Connecting to "));
  Serial.println(wifiSsid);
  WiFi.begin(wifiSsid, wifiPassword);

  state = STATE_WAITING_FOR_WIFI;
  stateBegunMs = millis();
}

// Called repeatedly, automatically.
void loop() {
  unsigned long nowMs; // The current time (millis())
  int wifiStatus;  // connection state of the WiFi.

  nowMs = millis();

  switch (state) {
    case STATE_ERROR:
      // Blink the led.
      if (nowMs % 1000 < 500) { // will be slightly long at millis() rollover.
        digitalWrite(PIN_LED_L, LOW);
      } else {
        digitalWrite(PIN_LED_L, HIGH);
      }
      break;

    case STATE_WAITING_FOR_WIFI:
      // Waiting for either WiFi connection or connection failure.

      wifiStatus = WiFi.status();
      if (wifiStatus == WL_CONNECT_FAILED) {
        // bad password. Fail.
        Serial.print(F("Password incorrect for "));
        Serial.println(wifiSsid);
        state = STATE_ERROR;
        stateBegunMs = millis();

      } else if (wifiStatus == WL_NO_SSID_AVAIL) {
        // Show we're waiting.
        digitalWrite(PIN_LED_L, LOW);

        // Continue to wait for the WiFi AP to become available.
        // This state is our house-power-fail recovery state.

      } else if (wifiStatus == WL_CONNECTED) {
        // We've connected to WiFi. Start reading temperatures.
        Serial.print(F("Connected, IP address: "));
        Serial.println(WiFi.localIP());

        // Indicate we're working fine.
        digitalWrite(PIN_LED_L, HIGH);

        // We're ready to read the temperature and humidity.
        state = STATE_TO_READ_TEMPERATURE;
        stateBegunMs = millis();
      }
      break;


    case STATE_TO_READ_TEMPERATURE:
      // The temperature is ready. Read and print it.
      int result;
      
      result = dht22.read22(DHT22_DATA);
      if (result != DHTLIB_OK) {
        if (result == DHTLIB_ERROR_TIMEOUT) {
          Serial.println("Timeout waiting for DHT22");
        } else if (result == DHTLIB_ERROR_CHECKSUM) {
          Serial.println("Garbled result from DHT22");
        } else {
          Serial.println("Unknown failure from DHT22");
        }
        
        // The read failed. Try again later.
        Serial.println("DHT fail.");
        state = STATE_WAITING_FOR_NEXT_READ;
        stateBegunMs = millis();
        return;
      }

      // According to https://iridl.ldeo.columbia.edu/dochelp/QA/Basic/dewpoint.html
      // we can estimate the Dew Point (in degrees C) from the temperature (C)
      // and Relative Humidity (%).
      dewPointC = (float) (dht22.temperature - (100.0 - dht22.humidity)/5.0);

      // Log the temperature and humidity on the serial port.
      
      Serial.print(dht22.temperature, 1);
      Serial.print("C;  ");
      Serial.print(dht22.humidity, 1);
      Serial.print("%;  ");
      Serial.print(dewPointC, 1);
      Serial.print("C dew point");
      Serial.println();

      // Report our data to the web server.
      if (doHttpsPost()) {
        digitalWrite(PIN_LED_L, HIGH); // success: turn off the LED.
      } else {
        digitalWrite(PIN_LED_L, LOW); // recoverable failure: turn on the LED.
      }

      // Wait until it's time to request the temperature again.
      state = STATE_WAITING_FOR_NEXT_READ;
      stateBegunMs = millis();

      break;

    case STATE_WAITING_FOR_NEXT_READ:
      if ((long) (nowMs - stateBegunMs) < MS_PER_TEMPERATURE_REQUEST) {
        return; // wait more.
      }

      // Next read the temperature and humidity.
      state = STATE_TO_READ_TEMPERATURE;
      stateBegunMs = millis();
      break;

    default:
      Serial.print("unknown state: ");
      Serial.println(state);
      state = STATE_ERROR;
      break;
  }
}

/*
    Uploads the temperature and humidity - that's all our weather station knows.
    Uses:
      stationId, stationKey
      dht fields (temperature and humidity)
      and the server information (host, post, etc.)

    Returns true if successful, false otherwise.
*/
boolean doHttpsPost() {
  char content[1024];  // Buffer for the content of the POST request.
  char *pContent;  // a pointer into content[]
  char ch;

  // client = manager of the HTTPS connection to the web site.
  WiFiClientSecure client;


  /*
     Build the content of the POST. That is, the parameters.
     UrlEncode the fields that can contain arbitrary text.
  */

  strcpy(content, "");

  strcat(content, "ID=");
  urlencodedcat(content, stationId);

  strcat(content, "&");
  strcat(content, "PASSWORD=");
  urlencodedcat(content, stationKey);

  strcat(content, "&");
  strcat(content, "action=updateraw"); // says "I'm posting the weather"

  strcat(content, "&");
  strcat(content, "dateutc=now"); // says when the data was collected

  // Convert the temperatures from Celsius to Fahrenheit
  float temperatureF = dht22.temperature * (9.0 / 5.0) + 32.0;
  float dewPointF = dewPointC * (9.0 / 5.0) + 32.0;

  strcat(content, "&");
  strcat(content, "tempf=");
  floatcat(content, temperatureF);

  strcat(content, "&");
  strcat(content, "humidity=");
  floatcat(content, dht22.humidity);

  strcat(content, "&");
  strcat(content, "dewptf=");
  floatcat(content, dewPointF);


  // Perform the Post

  client.setFingerprint(sslFingerprint);
  if (!client.connect(host, port)) {
    Serial.print("Failed to connect to ");
    Serial.print(host);
    Serial.print(" port ");
    Serial.println(port);
    Serial.print(" using SSL fingerprint ");
    Serial.print(sslFingerprint);
    Serial.println();
    return false;
  }
  Serial.print("Connected to ");
  Serial.println(host);

  client.print("POST ");
  client.print(url);
  client.print(" ");
  client.println(httpProtocol);

  client.print("Host: ");
  client.println(host);

  client.print("User-Agent: ");
  client.println(httpAgent);

  client.println("Connection: close");

  client.println("Content-Type: application/x-www-form-urlencoded");

  client.print("Content-Length: ");
  client.println(strlen(content));

  client.println();  // blank line indicates the end of the HTTP headers.

  // send the content: the POST parameters.
  client.print(content);

  Serial.println("Reading response:");
  while (client.connected()) {
    if (client.available()) {
      ch = client.read();
      Serial.print(ch);
    }
    delay(1); // to yield the processor for a moment.
  }

  Serial.println();
  Serial.println("connection closed.");

  client.stop();

  return true;
}

/*
   Append (concatenate) the given floating-point number
   as a string to the given buffer.

   buffer = a space to concatenate into. It must contain a null-terminated
    set of characters.
   f = the floating point number to convert into a set of characters.

*/
void floatcat(char *buffer, float f) {
  char *p;
  long l;

  p = buffer + strlen(buffer);

  // Convert the sign.
  if (f < 0.0) {
    *p++ = '-';
    f = -f;
  }

  // Convert the integer part.
  l = (long) f;
  f -= (float) l;
  itoa(l, p, 10);
  p += strlen(p);

  // Convert the first 2 digits of the fraction.
  *p++ = '.';
  f *= 100.0;
  itoa((long) f, p, 10);
}

/*
   Append (concatenate) the given string to the given buffer,
   UrlEncoding the string as it is appended.

   buffer = a space to concatenate into. It must contain a null-terminated
    set of characters, and be large enough to store the result.
   str = points to the null-terminated string to UrlEncode and append.

*/
void urlencodedcat(char *buffer, char *str) {
  char *pBuf; // points to the next empty space in buffer[]
  char *pIn;  // points to a character in str[]
  char ch;  // the current input character to UrlEncode.
  char hexDigit[] = "0123456789ABCDEF";

  pBuf = buffer + strlen(buffer);

  for (pIn = str; *pIn; ++pIn) {
    ch = *pIn;
    if (isAlphaNumeric(ch)) {
      // Alphabetic and numeric characters are passed as-is.
      *pBuf++ = ch;
      continue;
    }

    /*
       All other characters are encoded as a % followed by
       two hexidecimal characters. E.g., = is encoded as %3D.
       Reference: https://developer.mozilla.org/en-US/docs/Glossary/percent-encoding
    */

    *pBuf++ = '%';
    *pBuf++ = hexDigit[((int) ch >> 4) & 0xF];
    *pBuf++ = hexDigit[((int) ch) & 0xF];
  }

  *pBuf = (char) 0; // null terminate the output.
}

#if defined(TEST_URL_ENCODING)
/*
   A simple unit test of the Url Encoding function.
*/
void testUrlencodedcat() {
  static char input[255 + 1]; // 255 = 1..255, +1 for terminating null
  static char output[255 * 3 + 1]; // 256 * 3 for each encoded char, +1 for terminating null

  Serial.println(F("Output should be URL Encoded 0x01 through 0xff"));

  output[0] = (char) 0;
  char *pIn = input;
  for (int i = 1; i < 256; ++i) {
    *pIn++ = (char) i;
  }
  *pIn = (char) 0;

  urlencodedcat(output, input);

  Serial.println(output);

}
#endif // defined(TEST_URL_ENCODING)

/********************************
   From https://github.com/bneedhamia/write_eeprom_strings example
*/
/*
   Reads a string from EEPROM.  Copy this code into your program that reads EEPROM.

   baseAddress = EEPROM address of the first byte in EEPROM to read from.
   stringNumber = index of the string to retrieve (string 0, string 1, etc.)

   Assumes EEPROM contains a list of null-terminated strings,
   terminated by EEPROM_END_MARK.

   Returns:
   A pointer to a dynamically-allocated string read from EEPROM,
   or null if no such string was found.
*/
char *readEEPROMString(int baseAddress, int stringNumber) {
  int start;   // EEPROM address of the first byte of the string to return.
  int length;  // length (bytes) of the string to return, less the terminating null.
  char ch;
  int nextAddress;  // next address to read from EEPROM.
  char *result;     // points to the dynamically-allocated result to return.
  int i;


#if defined(ESP8266)
  EEPROM.begin(512);
#endif

  nextAddress = START_ADDRESS;
  for (i = 0; i < stringNumber; ++i) {

    // If the first byte is an end mark, we've run out of strings too early.
    ch = (char) EEPROM.read(nextAddress++);
    if (ch == (char) EEPROM_END_MARK) {
#if defined(ESP8266)
      EEPROM.end();
#endif
      return (char *) 0;  // not enough strings are in EEPROM.
    }

    // Read through the string's terminating null (0).
    int length = 0;
    while (ch != '\0' && length < EEPROM_MAX_STRING_LENGTH - 1) {
      ++length;
      ch = EEPROM.read(nextAddress++);
    }
  }

  // We're now at the start of what should be our string.
  start = nextAddress;

  // If the first byte is an end mark, we've run out of strings too early.
  ch = (char) EEPROM.read(nextAddress++);
  if (ch == (char) EEPROM_END_MARK) {
#if defined(ESP8266)
    EEPROM.end();
#endif
    return (char *) 0;  // not enough strings are in EEPROM.
  }

  // Count to the end of this string.
  length = 0;
  while (ch != '\0' && length < EEPROM_MAX_STRING_LENGTH - 1) {
    ++length;
    ch = EEPROM.read(nextAddress++);
  }

  // Allocate space for the string, then copy it.
  result = new char[length + 1];
  nextAddress = start;
  for (i = 0; i < length; ++i) {
    result[i] = (char) EEPROM.read(nextAddress++);
  }
  result[i] = '\0';

  return result;
}
