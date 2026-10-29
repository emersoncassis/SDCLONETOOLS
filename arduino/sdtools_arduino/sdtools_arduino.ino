/*
 * SD Clone Tools - ponte Arduino/ESP32 para cartão SD
 *
 * Fala direto com o cartão SD pelo barramento SPI, sem passar pelo leitor do computador.
 * Assim o app consegue ler os registros internos do cartão (CID e CSD), ver se ele está
 * bloqueado por senha, testar a gravação e, se o bit de proteção temporária
 * (TMP_WRITE_PROTECT) estiver ligado, tentar desligá-lo.
 *
 * Placas testadas no código: Arduino Uno, Nano, Mega e ESP32.
 * Ligação (módulo micro SD com regulador/conversor de nível, como os módulos azuis comuns):
 *   Uno/Nano: CS=D10  MOSI=D11  MISO=D12  SCK=D13  VCC=5V  GND=GND
 *   Mega:     CS=D53  MOSI=D51  MISO=D50  SCK=D52  VCC=5V  GND=GND
 *   ESP32:    CS=GPIO5 MOSI=GPIO23 MISO=GPIO19 SCK=GPIO18 VCC=3V3 GND=GND
 *
 * Protocolo serial (115200 baud, uma linha por comando, toda resposta termina com "END"):
 *   SDT?    -> SDTOOLS 1 <placa>
 *   INFO    -> INIT OK V2 HC | INIT ERR n ; CID <hex> ; CSD <hex> ; STATUS <r1> <r2>
 *   WTEST   -> grava 1 bloco no meio do cartão com o conteúdo invertido, lê de volta e devolve o original
 *   CLRTMP  -> desliga o bit TMP_WRITE_PROTECT do CSD (nunca mexe no bit permanente)
 */
#include <SPI.h>

#ifndef SD_CS
#if defined(ESP32)
#define SD_CS 5
#elif defined(ARDUINO_AVR_MEGA2560)
#define SD_CS 53
#else
#define SD_CS 10
#endif
#endif

#if defined(ESP32)
#define BOARD_NAME "ESP32"
#elif defined(ARDUINO_AVR_MEGA2560)
#define BOARD_NAME "MEGA"
#elif defined(ARDUINO_AVR_NANO)
#define BOARD_NAME "NANO"
#else
#define BOARD_NAME "ARDUINO"
#endif

static bool g_hc = false, g_v2 = false, g_tx = false;
static uint8_t g_blk[512];

static uint8_t xfer(uint8_t b) { return SPI.transfer(b); }
static void csLow() { digitalWrite(SD_CS, LOW); }
static void csHigh() {
  digitalWrite(SD_CS, HIGH);
  xfer(0xFF);
}
static void beginTx(uint32_t hz) {
  if (g_tx) SPI.endTransaction();
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  g_tx = true;
}

static uint8_t crc7(const uint8_t *d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int j = 0; j < 8; j++) {
      crc <<= 1;
      if ((b ^ crc) & 0x80) crc ^= 0x09;
      b <<= 1;
    }
  }
  return ((crc & 0x7F) << 1) | 1;
}

static uint16_t crc16(const uint8_t *d, int n) {
  uint16_t crc = 0;
  for (int i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int j = 0; j < 8; j++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

static bool waitReady(uint16_t ms) {
  uint32_t t = millis();
  while (xfer(0xFF) != 0xFF)
    if (millis() - t > ms) return false;
  return true;
}

static uint8_t cmd(uint8_t c, uint32_t arg) {
  if (c & 0x80) { /* ACMD: precisa de CMD55 antes */
    c &= 0x7F;
    uint8_t r = cmd(55, 0);
    if (r > 1) return r;
  }
  csHigh();
  csLow();
  waitReady(500);
  uint8_t f[5] = {(uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg};
  for (int i = 0; i < 5; i++) xfer(f[i]);
  xfer(crc7(f, 5));
  uint8_t r = 0xFF;
  for (int i = 0; i < 10; i++) {
    r = xfer(0xFF);
    if (!(r & 0x80)) break;
  }
  return r;
}

static bool waitToken(uint8_t tok, uint16_t ms) {
  uint32_t t = millis();
  uint8_t b;
  while ((b = xfer(0xFF)) == 0xFF)
    if (millis() - t > ms) return false;
  return b == tok;
}

/* inicializa o cartão; devolve 0 ou um código de erro */
static int sdInit() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin();
  beginTx(250000);
  for (int i = 0; i < 10; i++) xfer(0xFF);
  uint32_t t = millis();
  uint8_t r;
  while ((r = cmd(0, 0)) != 1) {
    if (millis() - t > 1500) { csHigh(); return 1; } /* cartão não responde */
  }
  r = cmd(8, 0x1AA);
  g_v2 = false;
  if (r == 1) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = xfer(0xFF);
    if ((b[2] & 0x0F) != 1 || b[3] != 0xAA) { csHigh(); return 2; } /* tensão não aceita */
    g_v2 = true;
  }
  t = millis();
  do {
    r = cmd(0x80 | 41, g_v2 ? 0x40000000UL : 0);
  } while (r != 0 && millis() - t < 2500);
  if (r != 0) { csHigh(); return 3; } /* não saiu do estado ocioso */
  g_hc = false;
  if (g_v2 && cmd(58, 0) == 0) {
    uint8_t o[4];
    for (int i = 0; i < 4; i++) o[i] = xfer(0xFF);
    g_hc = (o[0] & 0x40) != 0;
  }
  if (!g_hc) cmd(16, 512);
  csHigh();
  beginTx(4000000);
  return 0;
}

static bool readReg(uint8_t c, uint8_t *out) {
  if (cmd(c, 0) != 0 || !waitToken(0xFE, 300)) { csHigh(); return false; }
  for (int i = 0; i < 16; i++) out[i] = xfer(0xFF);
  xfer(0xFF);
  xfer(0xFF);
  csHigh();
  return true;
}

static bool status(uint8_t *r1, uint8_t *r2) {
  *r1 = cmd(13, 0);
  *r2 = xfer(0xFF);
  csHigh();
  return *r1 != 0xFF;
}

static bool readBlock(uint32_t blk, uint8_t *buf) {
  if (cmd(17, g_hc ? blk : blk * 512UL) != 0 || !waitToken(0xFE, 500)) { csHigh(); return false; }
  for (int i = 0; i < 512; i++) buf[i] = xfer(0xFF);
  xfer(0xFF);
  xfer(0xFF);
  csHigh();
  return true;
}

/* devolve 0 aceito; senão: 0x100|R1 (comando recusado) ou a resposta de dados do cartão */
static int writeBlock(uint32_t blk, const uint8_t *buf) {
  uint8_t r = cmd(24, g_hc ? blk : blk * 512UL);
  if (r != 0) { csHigh(); return 0x100 | r; }
  xfer(0xFF);
  xfer(0xFE);
  for (int i = 0; i < 512; i++) xfer(buf[i]);
  uint16_t c = crc16(buf, 512);
  xfer(c >> 8);
  xfer(c & 0xFF);
  uint8_t resp = xfer(0xFF) & 0x1F;
  waitReady(1000);
  csHigh();
  return resp == 0x05 ? 0 : resp;
}

static void printHex(const char *tag, const uint8_t *b, int n) {
  Serial.print(tag);
  Serial.print(' ');
  for (int i = 0; i < n; i++) {
    if (b[i] < 16) Serial.print('0');
    Serial.print(b[i], HEX);
  }
  Serial.println();
}

static uint32_t blocksFromCsd(const uint8_t *c) {
  if ((c[0] >> 6) == 1) {
    uint32_t cs = ((uint32_t)(c[7] & 0x3F) << 16) | ((uint32_t)c[8] << 8) | c[9];
    return (cs + 1) * 1024UL;
  }
  uint32_t rbl = c[5] & 0x0F;
  uint32_t cs = ((uint32_t)(c[6] & 0x03) << 10) | ((uint32_t)c[7] << 2) | (c[8] >> 6);
  uint32_t mult = ((c[9] & 0x03) << 1) | (c[10] >> 7);
  return ((cs + 1) << (mult + 2)) << (rbl > 9 ? rbl - 9 : 0);
}

static void doInfo() {
  int e = sdInit();
  if (e) { Serial.print("INIT ERR "); Serial.println(e); return; }
  Serial.print("INIT OK ");
  Serial.print(g_v2 ? "V2" : "V1");
  Serial.println(g_hc ? " HC" : " SC");
  uint8_t r[16];
  if (readReg(10, r)) printHex("CID", r, 16);
  else Serial.println("CID ERR");
  if (readReg(9, r)) printHex("CSD", r, 16);
  else Serial.println("CSD ERR");
  uint8_t s1, s2;
  if (status(&s1, &s2)) {
    uint8_t s[2] = {s1, s2};
    printHex("STATUS", s, 2);
  }
}

static void doWtest() {
  int e = sdInit();
  if (e) { Serial.print("INIT ERR "); Serial.println(e); return; }
  uint8_t c[16];
  if (!readReg(9, c)) { Serial.println("WTEST READERR"); return; }
  uint32_t blk = blocksFromCsd(c) / 2;
  if (!readBlock(blk, g_blk)) { Serial.println("WTEST READERR"); return; }
  for (int i = 0; i < 512; i++) g_blk[i] = ~g_blk[i];
  int w = writeBlock(blk, g_blk);
  uint8_t s1 = 0, s2 = 0;
  status(&s1, &s2);
  if (w) {
    Serial.print("WTEST REJECT ");
    Serial.print(w, HEX);
    Serial.print(' ');
    Serial.println(s2, HEX);
    return;
  }
  /* lê de volta: g_blk tem o padrão gravado; o original é o inverso */
  static uint8_t rb[512];
  bool rok = readBlock(blk, rb);
  int res = 0; /* 0 ok, 1 ignorada, 2 corrompida */
  if (!rok) res = 2;
  else {
    bool same = true, orig = true;
    for (int i = 0; i < 512; i++) {
      if (rb[i] != g_blk[i]) same = false;
      if (rb[i] != (uint8_t)~g_blk[i]) orig = false;
    }
    res = same ? 0 : (orig ? 1 : 2);
  }
  Serial.println(res == 0 ? "WTEST OK" : (res == 1 ? "WTEST IGNORED" : "WTEST CORRUPT"));
  for (int i = 0; i < 512; i++) g_blk[i] = ~g_blk[i]; /* conteúdo original */
  Serial.println(writeBlock(blk, g_blk) == 0 ? "RESTORE OK" : "RESTORE ERR");
}

static void doClrTmp() {
  int e = sdInit();
  if (e) { Serial.print("INIT ERR "); Serial.println(e); return; }
  uint8_t c[16];
  if (!readReg(9, c)) { Serial.println("CLRTMP ERR 1"); return; }
  if (c[14] & 0x20) { Serial.println("CLRTMP PERM"); return; }   /* proteção permanente: não mexe */
  if (!(c[14] & 0x10)) { Serial.println("CLRTMP NOTSET"); return; }
  c[14] &= ~0x10;
  c[15] = crc7(c, 15);
  if (cmd(27, 0) != 0) { csHigh(); Serial.println("CLRTMP ERR 2"); return; }
  xfer(0xFF);
  xfer(0xFE);
  for (int i = 0; i < 16; i++) xfer(c[i]);
  uint16_t k = crc16(c, 16);
  xfer(k >> 8);
  xfer(k & 0xFF);
  uint8_t resp = xfer(0xFF) & 0x1F;
  waitReady(1000);
  csHigh();
  if (resp != 0x05) { Serial.print("CLRTMP ERR 3 "); Serial.println(resp, HEX); return; }
  uint8_t n[16];
  if (readReg(9, n)) printHex("CSD", n, 16);
  Serial.println((readReg(9, n) && !(n[14] & 0x10)) ? "CLRTMP OK" : "CLRTMP ERR 4");
}

void setup() {
  Serial.begin(115200);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
}

void loop() {
  static char line[24];
  static uint8_t n = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (n < sizeof line - 1) line[n++] = ch;
      continue;
    }
    line[n] = 0;
    n = 0;
    if (!strcmp(line, "SDT?")) { Serial.print("SDTOOLS 1 "); Serial.println(BOARD_NAME); }
    else if (!strcmp(line, "INFO")) doInfo();
    else if (!strcmp(line, "WTEST")) doWtest();
    else if (!strcmp(line, "CLRTMP")) doClrTmp();
    else Serial.println("ERR UNKNOWN");
    Serial.println("END");
  }
}
