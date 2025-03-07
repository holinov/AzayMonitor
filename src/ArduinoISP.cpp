#include <Arduino.h>
#include <SPI.h>

// ArduinoISP
// Copyright (c) 2008-2011 Randall Bohn
// If you require a license, see
// https://www.gnu.org/licenses/gpl-2.0.txt

#define PROG_FLICKER true

// Configure SPI clock (in Hz).
// E.g. for an ATtiny @ 128 kHz: the datasheet states that both the high and low
// SPI clock pulse must be > 2 CPU cycles, so take 3 cycles i.e. divide target
// f_cpu by 6:
#define SPI_CLOCK 		(125000)  // Slower speed = 125kHz

#define RESET     53  // Use pin 53 for reset
#define LED_HB    9
#define LED_ERR   8
#define LED_PMODE 7

#define BAUDRATE	19200
#define HWVER 2
#define SWMAJ 1
#define SWMIN 18

// STK Definitions
#define STK_OK      0x10
#define STK_FAILED  0x11
#define STK_UNKNOWN 0x12
#define STK_INSYNC  0x14
#define STK_NOSYNC  0x15
#define CRC_EOP     0x20 //EOF packet

#define BUFF_SIZE 256
uint8_t buff[BUFF_SIZE];

#define beget16(addr) (*addr * 256 + *(addr+1) )

// Global variables
int error = 0;
int pmode = 0;
unsigned int here;  // Address for reading/writing, set by 'U' command

// Parameters structure
struct param_t {
  uint8_t devicecode;
  uint8_t revision;
  uint8_t progtype;
  uint8_t parmode;
  uint8_t polling;
  uint8_t selftimed;
  uint8_t lockbytes;
  uint8_t fusebytes;
  uint8_t flashpoll;
  uint16_t eeprompoll;
  uint16_t pagesize;
  uint16_t eepromsize;
  uint32_t flashsize;
} param;

// Forward declarations
void pulse(int pin, int times);
void prog_lamp(int state);
uint8_t write_flash_pages(int length);
void flash(uint8_t hilo, unsigned int addr, uint8_t data);
void commit(unsigned int addr);

uint8_t getch() {
  while (!Serial.available());
  return Serial.read();
}

void fill(int n) {
  for (int x = 0; x < n; x++) {
    buff[x] = getch();
  }
}

void pulse(int pin, int times) {
  do {
    digitalWrite(pin, HIGH);
    delay(30);
    digitalWrite(pin, LOW);
    delay(30);
  }
  while (--times);
}

void prog_lamp(int state) {
  digitalWrite(LED_PMODE, state);
}

uint8_t spi_transaction(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  SPI.transfer(a);
  SPI.transfer(b);
  SPI.transfer(c);
  return SPI.transfer(d);
}

void empty_reply() {
  if (getch() == CRC_EOP) {
    Serial.write(STK_INSYNC);
    Serial.write(STK_OK);
  } else {
    error++;
    Serial.write(STK_NOSYNC);
  }
}

void breply(uint8_t b) {
  if (getch() == CRC_EOP) {
    Serial.write(STK_INSYNC);
    Serial.write(b);
    Serial.write(STK_OK);
  } else {
    error++;
    Serial.write(STK_NOSYNC);
  }
}

void get_version(uint8_t c) {
  switch(c) {
    case 0x80:  breply(HWVER); break;
    case 0x81:  breply(SWMAJ); break;
    case 0x82:  breply(SWMIN); break;
    case 0x93:  breply('S'); break;
    default:    breply(0);
  }
}

void set_parameters() {
  // call this after reading parameter packet into buff[]
  empty_reply();
}

void reset_target(bool reset) {
  digitalWrite(RESET, reset ? LOW : HIGH);
  delay(50);  // make sure it has time to take effect
}

void start_pmode() {
  // Reset sequence - first complete reset cycle
  digitalWrite(RESET, HIGH);
  pinMode(RESET, OUTPUT);
  delay(100);
  digitalWrite(RESET, LOW);
  delay(100);

  // Initialize SPI before releasing reset
  SPI.begin();
  SPI.beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));

  // Clear any data on SPI bus
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);

  delay(50);
  digitalWrite(RESET, HIGH);  // Release reset to start programming
  delay(300);  // Give target time to stabilize

  pmode = 1;
  prog_lamp(HIGH);

  // Double-check if we can communicate by reading SIGNATURE
  uint8_t check = spi_transaction(0x30, 0x00, 0x00, 0x00);
  if (check == 0xFF) {
    // If we got 0xFF, target might not be ready
    digitalWrite(LED_ERR, HIGH);  // Signal error
  } else {
    digitalWrite(LED_ERR, LOW);   // No error detected
  }
}

void end_pmode() {
  SPI.end();
  digitalWrite(RESET, HIGH);
  pinMode(RESET, INPUT);
  pmode = 0;
  prog_lamp(LOW);
}

void universal() {
  uint8_t ch;
  fill(4);
  ch = spi_transaction(buff[0], buff[1], buff[2], buff[3]);
  breply(ch);
}

void flash(uint8_t hilo, unsigned int addr, uint8_t data) {
  spi_transaction(0x40+8*hilo, addr>>8 & 0xFF, addr & 0xFF, data);
}

void commit(unsigned int addr) {
  spi_transaction(0x4C, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

unsigned int current_page() {
  return here & 0xFFFFFFE0;  // 32 bytes per page for ATtiny2313
}

void write_flash(int length) {
  fill(length);
  if (CRC_EOP == getch()) {
    Serial.write(STK_INSYNC);
    Serial.write(write_flash_pages(length));
  } else {
    error++;
    Serial.write(STK_NOSYNC);
  }
}

uint8_t write_flash_pages(int length) {
  int x = 0;
  unsigned int page = current_page();
  while (x < length) {
    if (page != current_page()) {
      commit(page);
      page = current_page();
    }
    flash(LOW, here, buff[x++]);
    flash(HIGH, here, buff[x++]);
    here++;
  }
  commit(page);
  return STK_OK;
}

uint8_t flash_read(uint8_t hilo, unsigned int addr) {
  return spi_transaction(0x20 + hilo * 8, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

void flash_read_page(int length) {
  for (int x = 0; x < length; x += 2) {
    uint8_t low = flash_read(LOW, here);
    Serial.write(low);
    uint8_t high = flash_read(HIGH, here);
    Serial.write(high);
    here++;
  }
}

void read_page() {
  char result = (char)STK_FAILED;
  int length = 256 * getch();
  length += getch();
  char memtype = getch();
  if (CRC_EOP != getch()) {
    error++;
    Serial.write(STK_NOSYNC);
    return;
  }
  Serial.write(STK_INSYNC);
  if (memtype == 'F') flash_read_page(length);
  Serial.write(STK_OK);
}

void read_signature() {
  if (CRC_EOP != getch()) {
    error++;
    Serial.write(STK_NOSYNC);
    return;
  }
  Serial.write(STK_INSYNC);
  uint8_t high = spi_transaction(0x30, 0x00, 0x00, 0x00);
  Serial.write(high);
  uint8_t middle = spi_transaction(0x30, 0x00, 0x01, 0x00);
  Serial.write(middle);
  uint8_t low = spi_transaction(0x30, 0x00, 0x02, 0x00);
  Serial.write(low);
  Serial.write(STK_OK);
}

void program_page() {
  char result = (char) STK_FAILED;
  unsigned int length = 256 * getch();
  length += getch();
  char memtype = getch();
  if (memtype == 'F') {
    write_flash(length);
    return;
  }
  if (memtype == 'E') {
    result = (char)STK_OK;
    if (CRC_EOP == getch()) {
      Serial.write(STK_INSYNC);
      Serial.write(result);
    } else {
      error++;
      Serial.write(STK_NOSYNC);
    }
    return;
  }
  Serial.write(STK_FAILED);
  return;
}

void setup() {
  // Start with everything off/reset
  pinMode(LED_PMODE, OUTPUT);
  digitalWrite(LED_PMODE, LOW);
  pinMode(LED_ERR, OUTPUT);
  digitalWrite(LED_ERR, LOW);
  pinMode(LED_HB, OUTPUT);
  digitalWrite(LED_HB, LOW);

  // Initialize serial and wait for port to open
  Serial.begin(BAUDRATE);
  delay(1000);  // Give serial port time to stabilize

  // Visual indicator we're alive
  pulse(LED_PMODE, 2);
  pulse(LED_ERR, 2);
  pulse(LED_HB, 2);

  // Initialize SPI pins
  pinMode(MISO, INPUT);
  pinMode(MOSI, OUTPUT);
  pinMode(SCK, OUTPUT);

  // Reset target
  pinMode(RESET, OUTPUT);
  digitalWrite(RESET, HIGH);
  delay(100);

  Serial.print("Arduino ISP version ");
  Serial.println(HWVER, DEC);
  Serial.print("Software version ");
  Serial.print(SWMAJ, DEC);
  Serial.print(".");
  Serial.println(SWMIN, DEC);
  Serial.flush();

  // Initialize SPI
  SPI.begin();
  SPI.beginTransaction(SPISettings(SPI_CLOCK, MSBFIRST, SPI_MODE0));
}

void loop() {
  // Is there incoming data to process?
  if (Serial.available()) {
    uint8_t ch = getch();
    digitalWrite(LED_HB, HIGH);  // Show activity

    // Process command
    switch (ch) {
      case '0': // signon
        error = 0;
        empty_reply();
        break;
      case '1':
        if (getch() == CRC_EOP) {
          Serial.write(STK_INSYNC);
          Serial.write(HWVER);
          Serial.write(SWMAJ);
          Serial.write(SWMIN);
          Serial.write(STK_OK);
        } else {
          error++;
          Serial.write(STK_NOSYNC);
        }
        break;
      case 'A':
        get_version(getch());
        break;
      case 'B':
        fill(20);
        set_parameters();
        break;
      case 'E': // extended parameters - ignore for now
        fill(5);
        empty_reply();
        break;
      case 'P':
        if (!pmode) start_pmode();
        empty_reply();
        break;
      case 'U': // set address (word)
        here = getch();
        here += 256 * getch();
        empty_reply();
        break;
      case 0x60: //STK_PROG_FLASH
        getch(); // low addr
        getch(); // high addr
        empty_reply();
        break;
      case 0x61: //STK_PROG_DATA
        getch(); // data
        empty_reply();
        break;
      case 0x64: //STK_PROG_PAGE
        program_page();
        break;
      case 0x74: //STK_READ_PAGE
        read_page();
        break;
      case 'V': //0x56
        universal();
        break;
      case 'Q': //0x51
        error = 0;
        end_pmode();
        empty_reply();
        break;
      case 0x75: //STK_READ_SIGN
        read_signature();
        break;
      default:
        error++;
        if (CRC_EOP == getch()) {
          Serial.write(STK_UNKNOWN);
        } else {
          Serial.write(STK_NOSYNC);
        }
    }

    digitalWrite(LED_HB, LOW);   // Command processed
  }

  // Heartbeat LED - blink in different patterns depending on status
  static unsigned long lastHeartbeat = 0;
  if ((millis() - lastHeartbeat) > 1000) {
    if (pmode) {
      // In programming mode - fast blink
      digitalWrite(LED_HB, !digitalRead(LED_HB));
      lastHeartbeat = millis();
    } else {
      // Idle - slower blink
      digitalWrite(LED_HB, HIGH);
      delay(20);
      digitalWrite(LED_HB, LOW);
      lastHeartbeat = millis();
    }
  }
}