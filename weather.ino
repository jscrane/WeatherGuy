#include <EtherCard.h>
#include <TinyXML.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#include <petit_fatfs.h>
#include <MemoryFree.h>

#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>

#define TFT_RST  -1
#define TFT_RS   5
#define TFT_LED  6
#define TFT_CS   7
#define SD_CS    8
#define ETHER_CS 10

byte Ethernet::buffer[567];  // 567 is minimum buffer size to avoid losing data
static uint32_t next_fetch, bright_on;

char website[] PROGMEM = "weather.yahooapis.com";

byte xmlbuf[90];
TinyXML xml;

Adafruit_ST7735 tft(TFT_CS, TFT_RS, TFT_RST);

#define IN_LOCATION 1
#define IN_UNITS 2
#define IN_ATMOS 4
#define IN_FORECAST 8
#define IN_WIND 16
#define IN_CONDITION 32
#define DISPLAY_UPDATE 64
#define READING_RESPONSE 128
byte status;

char temp_unit, pres_unit[3], speed_unit[5], condition_code[3], condition_text[32], city[16];
byte wind_speed, atmos_humidity;
int8_t atmos_rising, condition_temp, wind_chill;
uint16_t wind_direction, atmos_pressure;

struct forecast {
  int8_t low, high;
  char code[3], day[4], text[16], date[7];
} forecasts[5];
struct forecast *fcast = forecasts;
  
//#define DEBUG
#ifdef DEBUG
Print &out = Serial;
#else
Print &out = tft;
#endif

static int centre_text(const char *s, int x, int size)
{
  return x - (strlen(s)*size*6) / 2;
}

static int right(int n, int x, int size)
{
  return x - n*size*6;
}

static int val_len(int b)
{
  if (b >= 1000) return 4;
  if (b >= 100) return 3;
  if (b >= 10) return 2;
  return 1;
}

// from Adafruit's spitftbitmap ST7735 example
void bmp_draw(byte *buf, int bufsiz, char *filename, uint8_t x, uint8_t y) {
  int      bmpWidth, bmpHeight;   // W+H in pixels
  uint8_t  bmpDepth;              // Bit depth (currently must be 24)
  uint32_t bmpImageoffset;        // Start of image data in file
  uint32_t rowSize;               // Not always = bmpWidth; may have padding
  uint8_t  buffidx = bufsiz;      // Current position in buffer
  boolean  flip    = true;        // BMP is stored bottom-to-top
  int      w, h, row, col;
  uint8_t  r, g, b;
  uint32_t pos = 0;

  if((x >= tft.width()) || (y >= tft.height())) return;

#ifdef DEBUG
  uint32_t startTime = millis();
#endif

  int res = PFFS.open_file(filename);
  if (res != FR_OK) {
    out.print(F("file.open!"));
    out.print(' ');
    out.print(filename);
    out.print(' ');
    out.println(res);
    return;
  }

  // Parse BMP header
  uint32_t currPos = 0;
  if (read16(currPos) != 0x4D42) {
    out.println(F("Unknown BMP signature"));
    return;
  }
#ifdef DEBUG  
  out.print(F("File size: "));
  out.println(read32(currPos));
#else
  (void)read32(currPos);
#endif  
  (void)read32(currPos); // Read & ignore creator bytes
  bmpImageoffset = read32(currPos); // Start of image data
#ifdef DEBUG
  out.print(F("Image Offset: ")); 
  out.println(bmpImageoffset, DEC);
#endif
  // Read DIB header
  uint32_t header_size = read32(currPos);
#ifdef DEBUG
  out.print(F("Header size: ")); 
  out.println(header_size);
#endif
  bmpWidth  = read32(currPos);
  bmpHeight = read32(currPos);
  if (read16(currPos) != 1) {
    out.println(F("# planes -- must be '1'"));
    return;
  }
  bmpDepth = read16(currPos); // bits per pixel
#ifdef DEBUG
  out.print(F("Bit Depth: ")); 
  out.println(bmpDepth);
#endif
  if((bmpDepth != 24) || (read32(currPos) != 0)) {
    // 0 = uncompressed
    out.println(F("BMP format not recognized."));
    return;
  }
  
#ifdef DEBUG
  out.print(F("Image size: "));
  out.print(bmpWidth);
  out.print('x');
  out.println(bmpHeight);
#endif
  
  // BMP rows are padded (if needed) to 4-byte boundary
  rowSize = (bmpWidth * 3 + 3) & ~3;
  
  // If bmpHeight is negative, image is in top-down order.
  // This is not canon but has been observed in the wild.
  if (bmpHeight < 0) {
    bmpHeight = -bmpHeight;
    flip      = false;
  }
  
  // Crop area to be loaded
  w = bmpWidth;
  h = bmpHeight;
  if ((x+w-1) >= tft.width())  w = tft.width()  - x;
  if ((y+h-1) >= tft.height()) h = tft.height() - y;
  
  // Set TFT address window to clipped image bounds
  tft.setAddrWindow(x, y, x+w-1, y+h-1);
  
  for (row=0; row<h; row++) { // For each scanline...
  
    // Seek to start of scan line.  It might seem labor-
    // intensive to be doing this on every line, but this
    // method covers a lot of gritty details like cropping
    // and scanline padding.  Also, the seek only takes
    // place if the file position actually needs to change
    // (avoids a lot of cluster math in SD library).
    if (flip) // Bitmap is stored bottom-to-top order (normal BMP)
      pos = bmpImageoffset + (bmpHeight - 1 - row) * rowSize;
    else     // Bitmap is stored top-to-bottom
      pos = bmpImageoffset + row * rowSize;
      
    if (currPos != pos) { // Need seek?
      PFFS.lseek_file(pos);
      currPos = pos;
      buffidx = bufsiz; // Force buffer reload
    }
  
    for (col=0; col<w; col++) { // For each pixel...
      // Time to read more pixel data?
      if (buffidx >= bufsiz) { // Indeed
        int nread;
        PFFS.read_file((char *)buf, bufsiz, &nread);
        currPos += nread;
        buffidx = 0; // Set index to beginning
      }
  
      // Convert pixel from BMP to TFT format, push to display
      b = buf[buffidx++];
      g = buf[buffidx++];
      r = buf[buffidx++];
      tft.pushColor(tft.Color565(r,g,b));
    } // end pixel
  }
#ifdef DEBUG
  out.print(F("Loaded in "));
  out.print(millis() - startTime);
  out.println(F(" ms"));
#endif
}

// These read 16- and 32-bit types from the SD card file.
// BMP data is stored little-endian, Arduino is little-endian too.
// May need to reverse subscript order if porting elsewhere.
static uint16_t read16(uint32_t &p) {
  uint16_t result;
  int n;
  PFFS.read_file((char *)&result, sizeof(result), &n);
  p += n;
  return result;
}

static uint32_t read32(uint32_t &p) {
  uint32_t result;
  int n;
  PFFS.read_file((char *)&result, sizeof(result), &n);
  p += n;
  return result;
}

static const __FlashStringHelper *cardinal_direction(short deg)
{
  if (deg < 12 || deg >= 349)
    return F("N");
  if (deg >= 12 && deg < 34)
    return F("NNE");
  if (deg >= 34 && deg < 57)
    return F("NE");
  if (deg >= 57 && deg < 79)
    return F("ENE");
  if (deg >= 79 && deg < 102)
    return F("E");
  if (deg >= 102 && deg < 124)
    return F("ESE");
  if (deg >= 124 && deg < 147)
    return F("SE");
  if (deg >= 147 && deg < 169)
    return F("SSE");
  if (deg >= 169 && deg < 192)
    return F("S");
  if (deg >= 192 && deg < 214)
    return F("SSW");
  if (deg >= 214 && deg < 237)
    return F("SW");
  if (deg >= 237 && deg < 259)
    return F("WSW");
  if (deg >= 259 && deg < 282)
    return F("W");
  if (deg >= 282 && deg < 304)
    return F("WNW");
  if (deg >= 304 && deg < 327)
    return F("NW");
//  if (deg >= 327 && deg < 349)
  return F("NNW");
}

static void display_current() {
  tft.fillScreen(ST7735_WHITE);
  tft.setTextColor(ST7735_BLACK);

  tft.setTextSize(2);
  tft.setCursor(1, 1);
  tft.print(wind_speed);
  tft.setTextSize(1);
  tft.print(speed_unit);  
  tft.setCursor(1, 17);
  tft.print(cardinal_direction(wind_direction));
  
  tft.setTextSize(2);
  tft.setCursor(1, tft.height()-16);
  tft.print(condition_temp);
  tft.setTextSize(1);
  tft.print(temp_unit);
  if (condition_temp != wind_chill) {
    tft.setCursor(1, tft.height()-24);
    tft.print(wind_chill);
  }

  tft.setTextSize(2);
  tft.setCursor(right(val_len(atmos_humidity), tft.width(), 2)-6, tft.height()-16);
  tft.print(atmos_humidity);
  tft.setTextSize(1);
  tft.setCursor(tft.width()-6, tft.height()-16);  // ???
  tft.print('%');

  tft.setTextSize(2);
  tft.setCursor(right(val_len(atmos_pressure), tft.width(), 2)-6*strlen(pres_unit), 1);
  tft.print(atmos_pressure);
  tft.setTextSize(1);
  tft.print(pres_unit);
  if (atmos_rising == 1) {
    tft.setCursor(right(6, tft.width(), 1), 17);
    tft.print(F("rising"));
  } else if (atmos_rising == -1) {
    tft.setCursor(right(7, tft.width(), 1), 17);
    tft.print(F("falling"));
  }

  tft.setCursor(centre_text(city, 80, 1), 30);
  tft.print(city);
  bmp_draw(xmlbuf, sizeof(xmlbuf), condition_code, 54, 38);  
  tft.setCursor(centre_text(condition_text, 80, 1), 90);
  tft.print(condition_text);

  // http://www.iquilezles.org/www/articles/sincos/sincos.htm
  int rad = 50, cx = 80, cy = 64;
  const float a = 0.999847695, b = 0.017452406;
  // wind dir is azimuthal angle with N at 0
  float s = 1.0, c = 0.0;
  for (uint16_t i = 0; i < wind_direction; i++) {
    const float ns = a*s + b*c;
    const float nc = a*c - b*s;
    c = nc;
    s = ns;
  }
  // wind dir rotates clockwise so compensate
  float ex = cx-rad*c, ey = cy-rad*s;
  tft.fillCircle(ex, ey, 3, ST7735_BLACK);
  tft.drawLine(ex, ey, (3*ex + cx)/4, (3*ey + cy)/4, ST7735_BLACK);
}

static void display_forecast(struct forecast *f)
{
  tft.fillScreen(ST7735_WHITE);
  
  tft.setTextSize(2);
  tft.setCursor(1, tft.height()-16);
  tft.print(f->low);
  tft.setTextSize(1);
  tft.print(temp_unit);
  tft.setCursor(1, tft.height()-24);
  tft.print(F("low"));

  tft.setTextSize(2);
  tft.setCursor(right(val_len(f->high), tft.width(), 2)-6, tft.height()-16);
  tft.print(f->high);
  tft.setTextSize(1);
  tft.setCursor(tft.width()-6, tft.height()-16);  // ???
  tft.print(temp_unit);
  tft.setCursor(right(4, tft.width(), 1), tft.height()-24);
  tft.print(F("high"));
  
  tft.setTextSize(2);
  tft.setCursor(centre_text(f->day, 80, 2), 14);
  tft.print(f->day);
  tft.setTextSize(1);
  tft.setCursor(centre_text(f->date, 80, 1), 30);
  tft.println(f->date);
  bmp_draw(xmlbuf, sizeof(xmlbuf), f->code, 54, 38);  
  tft.setCursor(centre_text(f->text, 80, 1), 90);
  tft.print(f->text);  
}

static void set_status(int bit, boolean cond)
{
  if (cond)
    status |= bit;
  else
    status &= ~bit;
}

static void read_str(const char *from, uint16_t fromlen, char *to, uint16_t tolen)
{
  char buf[20];
  if (fromlen >= sizeof(buf))
    fromlen = sizeof(buf) - 1;
  if (fromlen > tolen)
    fromlen = tolen - 1;
  memcpy(buf, from, fromlen);
  buf[fromlen] = 0;
  if (strcmp(to, buf) != 0) {
    strncpy(to, buf, tolen); 
    set_status(DISPLAY_UPDATE, true);
  }
}

static int read_int(const char *from, int curr)
{
  int val = atoi(from);
  if (val != curr)
    set_status(DISPLAY_UPDATE, true);
  return val;
}

static boolean strequals(const char *first, PGM_P second)
{
  return strcmp_P(first, second) == 0;
}

static boolean strcontains(const char *first, PGM_P second)
{
    return strstr_P(first, second) != 0;
}

void xml_callback(uint8_t statusflags, char *tagName, uint16_t tagNameLen, char *data, uint16_t dlen) {
  if (statusflags & STATUS_START_TAG) {
    if (tagNameLen) {
      set_status(IN_LOCATION, strcontains(tagName, PSTR(":location")));
      set_status(IN_UNITS, strcontains(tagName, PSTR(":units")));
      set_status(IN_ATMOS, strcontains(tagName, PSTR(":atmos")));
      set_status(IN_WIND, strcontains(tagName, PSTR(":wind")));
      set_status(IN_CONDITION, strcontains(tagName, PSTR(":condition")));
      set_status(IN_FORECAST, strcontains(tagName, PSTR(":forecast")));
    }
  } else if (statusflags & STATUS_END_TAG) {
    if (strequals(tagName, PSTR("/rss"))) {
      set_status(DISPLAY_UPDATE, true);
      fcast = forecasts;
    } else if (status & IN_FORECAST)
      fcast++;
  } else if (statusflags & STATUS_ATTR_TEXT) {
    if (status & IN_LOCATION) {
      if (strequals(tagName, PSTR("city")))
        read_str(data, dlen, city, sizeof(city));    
    } else if (status & IN_UNITS) {
      if (strequals(tagName, PSTR("temperature")))
        temp_unit = *data;
      else if (strequals(tagName, PSTR("pressure")))
        read_str(data, dlen, pres_unit, sizeof(pres_unit));
      else if (strequals(tagName, PSTR("speed")))
        read_str(data, dlen, speed_unit, sizeof(speed_unit));
    } else if (status & IN_WIND) {
      if (strequals(tagName, PSTR("chill")))
        wind_chill = read_int(data, wind_chill);
      else if (strequals(tagName, PSTR("direction")))
        wind_direction = read_int(data, wind_direction);
      else if (strequals(tagName, PSTR("speed")))
        wind_speed = read_int(data, wind_speed);
    } else if (status & IN_ATMOS) {
      if (strequals(tagName, PSTR("humidity")))
        atmos_humidity = read_int(data, atmos_humidity);
      else if (strequals(tagName, PSTR("pressure")))
        atmos_pressure = read_int(data, atmos_pressure);
      else if (strequals(tagName, PSTR("rising")))
        atmos_rising = read_int(data, atmos_rising);
    } else if (status & IN_CONDITION) {
      if (strequals(tagName, PSTR("code")))
        read_str(data, dlen, condition_code, sizeof(condition_code));
      if (strequals(tagName, PSTR("text")))
        read_str(data, dlen, condition_text, sizeof(condition_text));
      else if (strequals(tagName, PSTR("temp")))
        condition_temp = read_int(data, condition_temp);
    } else if (status & IN_FORECAST) {
      if (strequals(tagName, PSTR("day")))
        read_str(data, dlen, fcast->day, sizeof(fcast->day));
      else if (strequals(tagName, PSTR("low")))
        fcast->low = read_int(data, fcast->low);
      else if (strequals(tagName, PSTR("high")))
        fcast->high = read_int(data, fcast->high);
      else if (strequals(tagName, PSTR("code")))
        read_str(data, dlen, fcast->code, sizeof(fcast->code));
      else if (strequals(tagName, PSTR("text")))
        read_str(data, dlen, fcast->text, sizeof(fcast->text));
      else if (strequals(tagName, PSTR("date")))
        read_str(data, dlen, fcast->date, sizeof(fcast->date));
    }
  } else if (statusflags & STATUS_ERROR) {
#ifdef DEBUG
    out.print(F("\nTAG:"));
    out.print(tagName);
    out.print(F(" :"));
    out.println(data);
#endif
    bool rsp = (status & READING_RESPONSE);
    status = 0;
    set_status(DISPLAY_UPDATE, true);
    set_status(READING_RESPONSE, rsp);
  }
}

static byte rx()
{
  SPDR = 0xFF;
  loop_until_bit_is_set(SPSR, SPIF);
  return SPDR;
}

static void tx(byte d)
{
  SPDR = d;
  loop_until_bit_is_set(SPSR, SPIF);
}

uint16_t update_interval = 20*60;
char city_code[7];
char units[2];
byte bright, dim, fade;

static void halt() {
  out.println(F("halt"));
  for (;;);
}

void setup () {
#ifdef DEBUG
  Serial.begin(57600);
#endif

  tft.initR(INITR_REDTAB);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(0,0);
  analogWrite(TFT_LED, 0);
  
  out.println(freeMemory());

  // initialise the SD card and read the config file
  int res = PFFS.begin(SD_CS, rx, tx);
  if (res != FR_OK) {
    out.print(F("PFFS!"));
    out.println(res);
    halt();
  }
  strcpy_P((char *)xmlbuf, PSTR("config"));
  res = PFFS.open_file((char *)xmlbuf);
  if (res != FR_OK) {
    out.print(F("config!"));
    out.println(res);
    halt();
  }
  int nread;
  res = PFFS.read_file((char *)xmlbuf, sizeof(xmlbuf), &nread);
  if (res != FR_OK) {
    out.print(F("read!"));
    out.println(res);
    halt();
  }
  const char *delim = " \n";
  char *p = strtok((char *)xmlbuf, delim);
  update_interval = atoi(p);
  p = strtok(0, delim);
  bright = atoi(p);
  p = strtok(0, delim);
  dim = atoi(p);  
  p = strtok(0, delim);
  strcpy(city_code, p);
  p = strtok(0, delim);
  strcpy(units, p);
  
  // ethernet interface mac address, must be unique on the LAN
  byte mac[] = { 0x74, 0x69, 0x69, 0x2d, 0x30, 0x31 };
  if (ether.begin(sizeof Ethernet::buffer, mac, ETHER_CS) == 0) {
    out.println(F("Ethernet!"));
    halt();
  }

  if (!ether.dhcpSetup()) {
    out.println(F("DHCP!"));
    halt();
  }

  // FIXME
  ether.hisip[0] = 188;
  ether.hisip[1] = 125;
  ether.hisip[2] = 73;
  ether.hisip[3] = 190;
  /*
  if (!ether.dnsLookup(website)) {
    out.println(F("DNS!"));
    halt();
  }
  */

  xml.init(xmlbuf, sizeof(xmlbuf), xml_callback);
  
  analogWrite(TFT_LED, dim);
  fade = dim;
}

static void net_callback(byte status, word off, word len) {
#ifdef DEBUG
  out.println(off);
  out.println(len);
#endif
  if (status == 1) {
    set_status(READING_RESPONSE, len == 512);
    char *rs = (char *)Ethernet::buffer+off;  
    while (len-- > 0) {
#ifdef DEBUG
      out.print(*rs);
#endif
      xml.processChar(*rs++);
    }
  }
}

#define TWO_MINS 120000L

void loop() {
  ether.packetLoop(ether.packetReceive());

  uint32_t now = millis();
  if (now > next_fetch) {
    next_fetch = now + update_interval * 1000L;
    ether.persistTcpConnection(true);
    strcpy_P((char *)xmlbuf, PSTR("?w="));
    strcat((char *)xmlbuf, city_code);
    strcat_P((char *)xmlbuf, PSTR("&u="));
    strcat((char *)xmlbuf, units);
    ether.browseUrl(PSTR("/forecastrss"), (char *)xmlbuf, website, PSTR("Accept: text/xml\r\n"), net_callback);
    set_status(READING_RESPONSE, true);
  }  else if (!(status & READING_RESPONSE)) {
    if (status & DISPLAY_UPDATE) {  
      display_current();
      set_status(DISPLAY_UPDATE, false);
    } else if ((now % 10000) == 0 && fade != dim) {
      uint32_t t = (now / 10000) % 6;
      if (t == 0)
        display_current();
      else
        display_forecast(forecasts+t-1);
    }
  }
  tft.fillRect(tft.width()/2-2, 0, 4, 4, (status & READING_RESPONSE)? ST7735_RED: ST7735_GREEN);

  if (fade == dim && analogRead(A5) == 1023) {
    bright_on = now;
    fade = bright;
    analogWrite(TFT_LED, fade);
  } else if (now > bright_on + TWO_MINS && fade < dim) {
    analogWrite(TFT_LED, fade++);
    if (fade == dim)
      set_status(DISPLAY_UPDATE, (now / 10000) % 6 != 0);
    else
      delay(25);
  }
}

