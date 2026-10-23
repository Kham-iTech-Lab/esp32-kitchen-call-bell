/*
  ESP32 กริ่งเรียกจากครัวไปหน้าร้าน (ตัวหน้าร้าน)
  โค้ดประกอบคลิป K iTech Review / Kham iTech Lab

  รับสัญญาณจากตัวในครัวผ่าน ESP-NOW → บัซเซอร์ดัง + ไฟกระพริบ
  กดปุ่ม "รับทราบ" → เงียบ แล้วส่งกลับไปบอกครัวว่ามีคนเห็นแล้ว
  ถ้ายังไม่มีใครกดรับ จะดังเตือนซ้ำทุก 15 วินาที

  ต่อวงจร:
    บัซเซอร์แอคทีฟ 5V : ขา + -> GPIO 25, ขา - -> GND
                          (ถ้าอยากดังกว่านี้ ต่อผ่านทรานซิสเตอร์ หรือใช้รีเลย์สั่งกริ่ง)
    LED                : GPIO 26 -> ตัวต้านทาน 220Ω -> LED -> GND
    ปุ่มรับทราบ          : ขาหนึ่ง -> GPIO 27, อีกขา -> GND
    ไฟเลี้ยง             : อะแดปเตอร์มือถือ 5V เสียบช่อง USB

  รองรับ ESP32 Arduino core 2.x และ 3.x
*/
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------- ตั้งค่า (ต้องตรงกับตัวในครัว) ----------
const uint32_t GROUP_ID = 0x4B495443;
const uint8_t  CHANNEL  = 1;

const int BUZZER_PIN = 25;
const int LED_PIN    = 26;
const int ACK_PIN    = 27;
const bool BUZZER_ACTIVE_HIGH = true;    // บัซเซอร์บางโมดูลดังเมื่อขาเป็น LOW ให้เปลี่ยนเป็น false

const unsigned long REMIND_EVERY = 15000;   // ดังเตือนซ้ำถ้ายังไม่มีคนรับ (ms)

// ---------- ข้อความที่ส่งหากัน ----------
enum MsgType : uint8_t { MSG_CALL = 1, MSG_GOT = 2, MSG_ACK = 3 };
struct __attribute__((packed)) Msg {
  uint32_t group;
  uint8_t  type;
  uint8_t  station;
  uint16_t seq;
};

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// เก็บได้หลายจุดเรียก (ถ้าครัวมีหลายปุ่ม)
const int MAX_STATIONS = 8;
struct Call { bool active; uint16_t seq; bool seen; };
Call calls[MAX_STATIONS + 1];

// คิวข้อความจาก callback (callback ทำงานคนละ task กับ loop)
const int QLEN = 8;
volatile Msg q[QLEN];
volatile uint8_t qHead = 0, qTail = 0;

unsigned long alertAt = 0;   // เวลาที่เริ่มดังรอบล่าสุด
bool ringing = false;

void sendMsg(MsgType type, uint8_t station, uint16_t seq) {
  Msg m = {GROUP_ID, type, station, seq};
  esp_now_send(BROADCAST, (const uint8_t *)&m, sizeof(m));
}

void handleData(const uint8_t *data, int len) {
  if (len != sizeof(Msg)) return;
  Msg m;
  memcpy(&m, data, sizeof(m));
  if (m.group != GROUP_ID || m.type != MSG_CALL) return;
  uint8_t next = (qHead + 1) % QLEN;
  if (next == qTail) return;   // คิวเต็ม ข้ามไป (ครัวจะส่งซ้ำเอง)
  q[qHead].group = m.group; q[qHead].type = m.type; q[qHead].station = m.station; q[qHead].seq = m.seq;
  qHead = next;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) { handleData(data, len); }
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) { handleData(data, len); }
#endif

void buzzer(bool on) { digitalWrite(BUZZER_PIN, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW); }

void startAlert() { ringing = true; alertAt = millis(); }

bool anyActive() {
  for (int i = 1; i <= MAX_STATIONS; i++) if (calls[i].active) return true;
  return false;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(ACK_PIN, INPUT_PULLUP);
  buzzer(false);

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
  Serial.println("ตัวหน้าร้านพร้อม");
}

bool ackPressed() {
  static bool last = HIGH;
  static unsigned long changedAt = 0;
  bool now = digitalRead(ACK_PIN);
  bool fired = false;
  if (now != last && millis() - changedAt > 40) {
    changedAt = millis();
    if (now == LOW) fired = true;
    last = now;
  }
  return fired;
}

void loop() {
  unsigned long now = millis();

  // ข้อความเรียกจากครัว
  while (qTail != qHead) {
    uint8_t st = q[qTail].station;
    uint16_t sq = q[qTail].seq;
    qTail = (qTail + 1) % QLEN;
    if (st < 1 || st > MAX_STATIONS) continue;
    sendMsg(MSG_GOT, st, sq);                 // ตอบทุกครั้ง ครัวจะได้หยุดส่งซ้ำ
    Call &c = calls[st];
    if (c.seen && c.seq == sq) {              // รอบนี้กดรับไปแล้ว: ย้ำบอกครัวอีกที
      sendMsg(MSG_ACK, st, sq);
    } else if (!c.active || c.seq != sq) {    // เรียกใหม่
      c.active = true; c.seq = sq; c.seen = false;
      Serial.printf("ครัวเรียก! จุด %u\n", st);
      startAlert();
    }
  }

  // กดรับทราบ: รับทุกจุดที่ค้างอยู่
  if (ackPressed() && anyActive()) {
    for (int i = 1; i <= MAX_STATIONS; i++) {
      if (!calls[i].active) continue;
      for (int k = 0; k < 3; k++) { sendMsg(MSG_ACK, i, calls[i].seq); delay(20); }
      calls[i].active = false; calls[i].seen = true;
    }
    ringing = false;
    buzzer(false);
    Serial.println("รับทราบแล้ว");
  }

  // เสียงเตือน: ปี๊บ 3 ครั้ง แล้วรอ ดังซ้ำทุก REMIND_EVERY จนกว่าจะมีคนรับ
  if (anyActive() && !ringing && now - alertAt > REMIND_EVERY) startAlert();
  if (ringing) {
    unsigned long t = now - alertAt;
    buzzer(t < 1500 && (t % 500) < 250);
    if (t >= 1500) { ringing = false; buzzer(false); }
  }
  digitalWrite(LED_PIN, anyActive() ? (now / 300) % 2 : LOW);
  delay(5);
}
