/*
  ESP32 กริ่งเรียกจากครัวไปหน้าร้าน (ตัวในครัว)
  โค้ดประกอบคลิป K iTech Review / Kham iTech Lab

  อาหารเสร็จ กดปุ่ม → ส่งสัญญาณไปตัวหน้าร้านผ่าน ESP-NOW (ไม่ต้องใช้เราเตอร์)
  ไฟ LED ในครัวบอกสถานะ:
    กระพริบเร็ว   = กำลังส่ง
    กระพริบช้า    = หน้าร้านได้รับแล้ว กำลังดัง รอคนกดรับ
    ติดค้าง 3 วิ   = หน้าร้านกดรับทราบแล้ว
    กระพริบถี่ 3 ครั้ง แล้วดับ = ส่งไม่ถึง (ตัวหน้าร้านปิดอยู่ หรือไกลเกินไป)

  ต่อวงจร:
    ปุ่มกด     : ขาหนึ่ง -> GPIO 13, อีกขา -> GND (ใช้ pull-up ในบอร์ด)
    LED สถานะ  : GPIO 2 (LED บนบอร์ด DevKit) หรือต่อ LED + ตัวต้านทาน 220Ω เพิ่มเองก็ได้
    ไฟเลี้ยง    : อะแดปเตอร์มือถือ 5V เสียบช่อง USB

  รองรับ ESP32 Arduino core 2.x และ 3.x
*/
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- ตั้งค่า (ต้องตรงกับตัวหน้าร้าน) ----------
const uint32_t GROUP_ID = 0x4B495443;   // เปลี่ยนเป็นเลขของร้านคุณ กันชนกับเครื่องอื่นแถวนั้น
const uint8_t  CHANNEL  = 1;            // ช่องสัญญาณ 1–13 ทั้งสองตัวต้องตรงกัน
const uint8_t  STATION  = 1;            // เลขจุดเรียก ถ้ามีหลายปุ่มให้ใส่ 1, 2, 3 ...

const int BUTTON_PIN = 13;
const int LED_PIN    = 2;

// ---------- ข้อความที่ส่งหากัน ----------
enum MsgType : uint8_t { MSG_CALL = 1, MSG_GOT = 2, MSG_ACK = 3 };
struct __attribute__((packed)) Msg {
  uint32_t group;
  uint8_t  type;
  uint8_t  station;
  uint16_t seq;
};

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum State { IDLE, CALLING, WAITING, DONE, FAILED };
State state = IDLE;
uint16_t seq = 0;
unsigned long stateAt = 0, lastSend = 0;

volatile bool gotMsg = false;
volatile Msg inbox;

void sendMsg(MsgType type) {
  Msg m = {GROUP_ID, type, STATION, seq};
  esp_now_send(BROADCAST, (const uint8_t *)&m, sizeof(m));
}

void handleData(const uint8_t *data, int len) {
  if (len != sizeof(Msg)) return;
  Msg m;
  memcpy(&m, data, sizeof(m));
  if (m.group != GROUP_ID || m.station != STATION) return;
  inbox.group = m.group; inbox.type = m.type; inbox.station = m.station; inbox.seq = m.seq;
  gotMsg = true;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) { handleData(data, len); }
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) { handleData(data, len); }
#endif

void setState(State s) { state = s; stateAt = millis(); }

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW เริ่มไม่ได้");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(100); }
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  seq = esp_random() & 0xFFFF;   // ไม่ให้เลขซ้ำกับรอบก่อนรีบูต
  Serial.println("ตัวในครัวพร้อม");
}

bool buttonPressed() {
  static bool last = HIGH;
  static unsigned long changedAt = 0;
  bool now = digitalRead(BUTTON_PIN);
  bool fired = false;
  if (now != last && millis() - changedAt > 40) {   // กันปุ่มเด้ง
    changedAt = millis();
    if (now == LOW) fired = true;
    last = now;
  }
  return fired;
}

void loop() {
  unsigned long now = millis();

  if (buttonPressed()) {
    seq++;
    Serial.printf("เรียก! (รอบ %u)\n", seq);
    setState(CALLING);
    lastSend = 0;
  }

  if (gotMsg) {
    gotMsg = false;
    if (inbox.seq == seq) {
      if (inbox.type == MSG_GOT && state == CALLING) { Serial.println("หน้าร้านได้รับแล้ว"); setState(WAITING); }
      if (inbox.type == MSG_ACK && (state == CALLING || state == WAITING)) { Serial.println("หน้าร้านกดรับแล้ว"); setState(DONE); }
    }
  }

  switch (state) {
    case IDLE:
      digitalWrite(LED_PIN, LOW);
      break;
    case CALLING:   // ส่งซ้ำทุก 300 ms จนกว่าหน้าร้านตอบ (ไม่เกิน 6 วิ)
      if (now - lastSend > 300) { sendMsg(MSG_CALL); lastSend = now; }
      digitalWrite(LED_PIN, (now / 100) % 2);
      if (now - stateAt > 6000) { Serial.println("ส่งไม่ถึงหน้าร้าน"); setState(FAILED); }
      break;
    case WAITING:   // หน้าร้านดังอยู่ รอคนกดรับ (ส่งย้ำทุก 2 วิ เผื่อหน้าร้านรีบูต)
      if (now - lastSend > 2000) { sendMsg(MSG_CALL); lastSend = now; }
      digitalWrite(LED_PIN, (now / 500) % 2);
      if (now - stateAt > 10UL * 60 * 1000) setState(IDLE);   // ไม่มีใครรับใน 10 นาที เลิกรอ
      break;
    case DONE:
      digitalWrite(LED_PIN, HIGH);
      if (now - stateAt > 3000) setState(IDLE);
      break;
    case FAILED:
      digitalWrite(LED_PIN, (now - stateAt) < 1500 && ((now / 80) % 2));
      if (now - stateAt > 1500) setState(IDLE);
      break;
  }
  delay(5);
}
