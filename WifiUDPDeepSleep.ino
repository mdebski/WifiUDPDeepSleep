#include <ESP8266WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFiUdp.h>
#include "constants.h"
#include "config.h"

#ifdef DISABLE_SERIAL
#define Serial fakeSerial
#endif

OneWire oneWire(SENSOR_GPIO_NUM);
DallasTemperature sensors(&oneWire);

WiFiUDP udp;

class FakeSerial {
public:
  void begin(int baud) {};
  void setTimeout(int timeout) {};
  void println(char * str) {};
  void printf(char * s, ...) {};
  operator bool() const { return true; }
} fakeSerial;

typedef struct {
 char protocol_magic[4];
 uint32 id;
 uint8 data_count;
} __attribute__((packed)) ReplyPacket;

typedef struct {
 char protocol_magic[4];
 uint32 id;
 uint32 measurements_timedelta_ms;
 uint8 data_count;
} __attribute__((packed)) PacketHeader;

typedef struct {
 uint16 temperature_cC;
 uint16 voltage_1_1024_V;
 uint16 time_awake_ms;
} __attribute__((packed)) Measurement;

typedef struct {
 uint32 magic;
 uint32 cnt;
 uint32 rand;
 uint16 last_awake_time_ms;
 PacketHeader hdr;
 Measurement data[MAX_DATA_COUNT];
} __attribute__((packed)) RtcStore;

static_assert(sizeof(RtcStore) <= 512, "RtcStore too big");

RtcStore rtc_mem;

bool waitForWifi(int wifi_start_time) {
  while (WiFi.status() != WL_CONNECTED &&
      millis() - wifi_start_time < MAX_WIFI_CONNECT_TIME_MS) {
    if (WiFi.status() == WL_CONNECT_FAILED) {
      Serial.println("Failed to connect to WiFi.");
      break;
    }
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi timeout.");
    return false;
  } else {
    return true;
  }
}

void zeroMeasurements() {
  rtc_mem.hdr.data_count = 0;
  rtc_mem.hdr.measurements_timedelta_ms = CYCLE_TIME_MS;
  int a = rtc_mem.rand + (rtc_mem.rand >> 8) + (rtc_mem.rand >> 16) + (rtc_mem.rand >> 24);
  rtc_mem.hdr.id = rtc_mem.cnt + ((a & 0xff) << 24);
  memset(rtc_mem.data, 0x0, sizeof(rtc_mem.data));
}

void read_or_init_rtc() {
  ESP.rtcUserMemoryRead(0, (uint32*) &rtc_mem, sizeof(rtc_mem));
  if(rtc_mem.magic != RTC_MAGIC) {
    Serial.println("Initializing rtc mem");
    rtc_mem.magic = RTC_MAGIC;
    // do not initialize rtc_mem.rand.
    rtc_mem.cnt = 0;
    rtc_mem.last_awake_time_ms = 0;
    memcpy(rtc_mem.hdr.protocol_magic, PROTOCOL_MAGIC, sizeof(PROTOCOL_MAGIC));
    zeroMeasurements();
  }
}

bool sendMeasurements() {
    if(!udp.begin(LOCAL_PORT)) return false;
    Serial.printf("Sending: %d -> %s:%d, id: %d %x, measurements: %d\n", LOCAL_PORT,
      REMOTE_IP.toString().c_str(), REMOTE_PORT, rtc_mem.hdr.id, rtc_mem.hdr.id, rtc_mem.hdr.data_count);
    if(!udp.beginPacket(REMOTE_IP, REMOTE_PORT)) return false;
    udp.write((char*) &rtc_mem.hdr, sizeof(PacketHeader) + sizeof(Measurement)*rtc_mem.hdr.data_count );
    if(!udp.endPacket()) return false;
    return true;
}

void makeMeasurement() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  int rtc_time = system_get_rtc_time();
  uint16 voltage = ESP.getVcc();
  Serial.printf("Data_count: %d\n", rtc_mem.hdr.data_count);
  Serial.printf("Temperature: %.1f *C\n", temp);
  Serial.printf("rtc_time: %d\n", rtc_time);
  Serial.printf("voltage: %d\n", voltage);
  Serial.printf("last_awake_time: %d\n", rtc_mem.last_awake_time_ms);

  if(rtc_mem.hdr.data_count < MAX_DATA_COUNT) {
    rtc_mem.data[rtc_mem.hdr.data_count].temperature_cC = (uint16) (temp*100);
    rtc_mem.data[rtc_mem.hdr.data_count].voltage_1_1024_V = voltage;
    rtc_mem.data[rtc_mem.hdr.data_count].time_awake_ms = rtc_mem.last_awake_time_ms;
    rtc_mem.hdr.data_count += 1;
  }
}

void setLastAwakeTime(int awake_time_ms) {
  if(rtc_mem.hdr.data_count > 0) {
    rtc_mem.data[rtc_mem.hdr.data_count - 1].time_awake_ms = awake_time_ms;
  }
}

bool sendThisCycle(int cnt) {
  // send in second cycle so that we know quickly if it works.
  return cnt == 1 || (cnt % SEND_EVERY == SEND_EVERY - 1);
}

ADC_MODE(ADC_VCC);

int awake_time_start_ms;
int sent_tries;

void clean_up_and_sleep() {
  rtc_mem.cnt += 1;
  rtc_mem.last_awake_time_ms = millis() - awake_time_start_ms;
  ESP.rtcUserMemoryWrite(0, (uint32*) &rtc_mem, sizeof(rtc_mem));
  int sleepTime = max(CYCLE_TIME_MS - rtc_mem.last_awake_time_ms, 1) * MS_TO_US;
  Serial.printf("Sleep time: %d\n", sleepTime);
  if(sendThisCycle(rtc_mem.cnt)) {
    ESP.deepSleep(sleepTime, WAKE_RF_DEFAULT);
  } else {
    ESP.deepSleep(sleepTime, WAKE_RF_DISABLED);
  }
  delay(1000);
}

int loop_cnt;

void setup() {
  awake_time_start_ms = millis();
  loop_cnt = 0;
  int wifi_start_time;

  Serial.begin(76800);
  Serial.setTimeout(2000);
  while(!Serial) { }

  read_or_init_rtc();

  Serial.printf("Woken up: %d, wifi_cycle: %d, start_time: %d\n", rtc_mem.cnt, sendThisCycle(rtc_mem.cnt), awake_time_start_ms);

  if(sendThisCycle(rtc_mem.cnt)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.config(LOCAL_IP, GATEWAY_IP, SUBNET);
    wifi_start_time = millis();
  }

  makeMeasurement();

  if(sendThisCycle(rtc_mem.cnt)) {
    if(waitForWifi(wifi_start_time)) {
      Serial.printf("WiFi connected in %d ms, my addr: %s\n", millis() - wifi_start_time, WiFi.localIP().toString().c_str());
      sent_tries = 0;
      // Do not sleep, let loop() handle udp send & recv.
    }
  } else {
    clean_up_and_sleep();
  }
}

char packetBuf[50];

void loop() {
 bool got_ack = false;
 bool broken = false;
 ReplyPacket * pkt;
 int packetSize = udp.parsePacket();
 if(packetSize) {
   Serial.printf("Received %d bytes from %s, port %d\n", packetSize, udp.remoteIP().toString().c_str(), udp.remotePort());
   int len = udp.read(packetBuf, sizeof(packetBuf));
   if(len == sizeof(ReplyPacket)) {
     pkt = (ReplyPacket*) packetBuf;
     Serial.printf("Expected: %c%c%c%c, %x, %d\n", PROTOCOL_MAGIC[0], PROTOCOL_MAGIC[1], PROTOCOL_MAGIC[2], PROTOCOL_MAGIC[3], rtc_mem.hdr.id, rtc_mem.hdr.data_count);
     Serial.printf("Got: %c%c%c%c, %x, %d\n", pkt->protocol_magic[0], pkt->protocol_magic[1], pkt->protocol_magic[2], pkt->protocol_magic[3], pkt->id, pkt->data_count);
     if(
       memcmp(pkt->protocol_magic, PROTOCOL_MAGIC, sizeof(PROTOCOL_MAGIC)) == 0 &&
       pkt->id == rtc_mem.hdr.id &&
       pkt->data_count == rtc_mem.hdr.data_count
     ) {
       Serial.printf("Got valid ack in loop %d\n", loop_cnt);
       got_ack = true;
     } else {
       Serial.printf("Invalid ack\n");
     }
   } else {
     Serial.printf("Invalid response length");
   }
 }
 if(loop_cnt % RETRY_SEND_EVERY_N_LOOPS == 0) {
   Serial.printf("Sending in loop %d [try %d]\n", loop_cnt, sent_tries);
   sent_tries += 1;
   if(!sendMeasurements()) {
     Serial.println("UDP send error");
     broken = true;
   }
 }
 if(got_ack || broken || millis() - awake_time_start_ms > MAX_AWAKE_TIME_MS) {
   Serial.printf("Ending loop.\n");
   zeroMeasurements();
   clean_up_and_sleep();
 }
 loop_cnt++;
 delay(RECV_LOOP_DELAY_MS);
}
