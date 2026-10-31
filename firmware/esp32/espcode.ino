/* ===========================================================================
 * Smart Parking - ESP32 Wi-Fi pre-booking server (simple version)
 * Board: "ESP32 Dev Module" (Arduino IDE, esp32 by Espressif core)
 *
 * Wiring to STM32F401 (3.3 V logic, common GND!):
 *   STM32 PA2 (USART2_TX)  ->  ESP32 GPIO25 (RX)
 *   STM32 PA3 (USART2_RX)  <-  ESP32 GPIO26 (TX)
 *   STM32 GND              --  ESP32 GND
 *
 * Flow: user books a free slot on the web page -> ESP32 tells the STM32 which
 * slots are booked ("B,b1,b2,b3") -> STM32 lets a car in only if a slot is booked.
 * STM32 reports slot occupancy ("S,o1,o2,o3"); when a booked slot gets occupied and
 * later becomes empty again, the booking is cleared automatically.
 * =========================================================================== */
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

/* ---------------- Settings ---------------- */
const char *WIFI_SSID = "vivo V70 FE";      // router / phone hotspot (2.4 GHz)
const char *WIFI_PASS = "123456789";
const char *AP_SSID   = "SmartParking";        // fallback hotspot if the router is not found
const char *AP_PASS   = "12345678";            // min 8 characters

#define STM_RX_PIN   25                        // ESP32 RX  <- STM32 PA2
#define STM_TX_PIN   26                        // ESP32 TX  -> STM32 PA3
#define STM_BAUD     115200
#define BOOK_HOLD_MS (30UL * 60UL * 1000UL)    // booking expires if the car never parks

/* ---------------- State ---------------- */
enum BState : uint8_t { B_NONE = 0, B_BOOKED, B_PARKED };
struct Booking { BState st; char plate[12]; uint32_t t; };

Booking  bk[3];
bool     occ[3]        = {false, false, false};
bool     bookingsDirty = true;
uint32_t lastBookTx    = 0;

WebServer server(80);
String    rxLine;

/* ---------------- Web page ---------------- */
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Parking</title>
<style>
 body{font-family:Arial,sans-serif;background:#f1f4f8;margin:0;padding:16px;color:#1b2430}
 .wrap{max-width:480px;margin:auto}
 h1{font-size:22px;margin:4px 0 12px}
 .card{background:#fff;border-radius:12px;padding:14px;margin-bottom:14px;box-shadow:0 1px 4px rgba(0,0,0,.12)}
 .slots{display:flex;gap:8px}
 .slot{flex:1;text-align:center;padding:14px 4px;border-radius:10px;color:#fff;font-weight:bold}
 .free{background:#2e9e5b}.booked{background:#e0a100}.occupied{background:#d64545}
 .slot small{display:block;font-weight:normal;margin-top:4px}
 label{display:block;font-size:13px;margin:8px 0 3px}
 input,select,button{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;border:1px solid #b9c2cc}
 button{background:#1d4ed8;color:#fff;border:0;margin-top:10px}
 button.alt{background:#6b7280}
 .msg{margin-top:8px;font-size:14px;min-height:18px}
</style></head><body><div class="wrap">
<h1>Smart Parking</h1>
<div class="card"><div class="slots" id="slots"></div></div>

<div class="card"><b>Book a slot</b>
 <label>Vehicle number</label><input id="plate" maxlength="11" placeholder="TN38AB1234">
 <label>Slot</label><select id="slot"></select>
 <button onclick="book()">Book slot</button>
 <button class="alt" onclick="cancelB()">Cancel my booking</button>
 <div class="msg" id="msg"></div></div>
</div>
<script>
async function post(path,data){
  try{const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});
      return await r.json();}catch(e){return {ok:false,msg:'Connection error'};}
}
async function refresh(){
  try{
    const s=await (await fetch('/api/status')).json();
    const el=document.getElementById('slots'); el.innerHTML='';
    const sel=document.getElementById('slot'); const keep=sel.value; sel.innerHTML='';
    s.slots.forEach((st,i)=>{
      const d=document.createElement('div'); d.className='slot '+st;
      d.innerHTML='Slot '+(i+1)+'<small>'+st.toUpperCase()+'</small>'; el.appendChild(d);
      if(st==='free'){const o=document.createElement('option'); o.value=i+1; o.text='Slot '+(i+1); sel.appendChild(o);}
    });
    if(!sel.options.length){const o=document.createElement('option'); o.text='No free slot'; o.value=''; sel.appendChild(o);}
    else if(keep) sel.value=keep;
  }catch(e){}
}
async function book(){
  const r=await post('/api/book',{plate:document.getElementById('plate').value,slot:document.getElementById('slot').value});
  document.getElementById('msg').textContent=r.msg||''; refresh();
}
async function cancelB(){
  const r=await post('/api/cancel',{plate:document.getElementById('plate').value});
  document.getElementById('msg').textContent=r.msg||''; refresh();
}
refresh(); setInterval(refresh,2000);
</script></body></html>
)rawliteral";

/* ---------------- Helpers ---------------- */
void sendBookings() {
  char s[16];
  snprintf(s, sizeof(s), "B,%d,%d,%d\n",
           bk[0].st != B_NONE, bk[1].st != B_NONE, bk[2].st != B_NONE);
  Serial2.print(s);
  lastBookTx = millis();
  bookingsDirty = false;
}

void jsonReply(int httpCode, bool ok, const String &msg) {
  String j = "{\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"msg\":\"" + msg + "\"}";
  server.send(httpCode, "application/json", j);
}

const char *slotState(int i) {
  if (occ[i]) return "occupied";
  if (bk[i].st == B_BOOKED) return "booked";
  return "free";
}

String cleanPlate(const String &plate) {
  String clean = "";
  for (unsigned i = 0; i < plate.length() && clean.length() < 11; i++) {
    char c = plate[i];
    if (isalnum((unsigned char)c)) clean += (char)toupper((unsigned char)c);
  }
  return clean;
}

/* ---------------- HTTP handlers ---------------- */
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleStatus() {
  String j = "{\"slots\":[";
  for (int i = 0; i < 3; i++) {
    j += "\"";
    j += slotState(i);
    j += "\"";
    if (i < 2) j += ",";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleBook() {
  int slot = server.arg("slot").toInt();
  String plate = cleanPlate(server.arg("plate"));
  if (slot < 1 || slot > 3)  { jsonReply(400, false, "Choose a slot"); return; }
  if (plate.length() < 4)    { jsonReply(400, false, "Enter a valid vehicle number"); return; }
  int i = slot - 1;
  if (occ[i] || bk[i].st != B_NONE) { jsonReply(409, false, "Slot is not available"); return; }
  for (int k = 0; k < 3; k++)
    if (bk[k].st != B_NONE && plate == String(bk[k].plate)) { jsonReply(409, false, "This vehicle already has a booking"); return; }

  bk[i].st = B_BOOKED;
  strncpy(bk[i].plate, plate.c_str(), sizeof(bk[i].plate) - 1);
  bk[i].plate[sizeof(bk[i].plate) - 1] = 0;
  bk[i].t = millis();
  bookingsDirty = true;
  Serial.printf("Booked slot %d for %s\n", slot, bk[i].plate);
  jsonReply(200, true, "Slot " + String(slot) + " booked. Drive to the entrance gate.");
}

void handleCancel() {
  String plate = cleanPlate(server.arg("plate"));
  for (int i = 0; i < 3; i++) {
    if (bk[i].st == B_BOOKED && plate == String(bk[i].plate)) {
      bk[i].st = B_NONE;
      bookingsDirty = true;
      jsonReply(200, true, "Booking cancelled");
      return;
    }
  }
  jsonReply(404, false, "No booking found for that vehicle number");
}

/* ---------------- STM32 link ---------------- */
void parseLine(const String &l) {
  int a, b, c;
  if (sscanf(l.c_str(), "S,%d,%d,%d", &a, &b, &c) == 3) {
    bool n[3] = {a != 0, b != 0, c != 0};
    for (int i = 0; i < 3; i++) {
      occ[i] = n[i];
      if (bk[i].st == B_BOOKED && occ[i])  { bk[i].st = B_PARKED; bookingsDirty = true; }
      if (bk[i].st == B_PARKED && !occ[i]) { bk[i].st = B_NONE;   bookingsDirty = true; }
    }
  }
}

void pollStm() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      rxLine.trim();
      if (rxLine.length()) parseLine(rxLine);
      rxLine = "";
    } else if (c != '\r') {
      if (rxLine.length() < 40) rxLine += c; else rxLine = "";
    }
  }
}

void expireBookings() {
  uint32_t now = millis();
  for (int i = 0; i < 3; i++)
    if (bk[i].st == B_BOOKED && now - bk[i].t > BOOK_HOLD_MS) { bk[i].st = B_NONE; bookingsDirty = true; }
}

/* ---------------- Setup / loop ---------------- */
void setup() {
  Serial.begin(115200);
  Serial2.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);
  memset(bk, 0, sizeof(bk));

  Serial.println("\nConnecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nConnected. Open: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("\nRouter not found. Hotspot '");
    Serial.print(AP_SSID);
    Serial.print("' started. Open: http://");
    Serial.println(WiFi.softAPIP());
  }
  if (MDNS.begin("parking")) Serial.println("Also reachable at http://parking.local");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/book", HTTP_POST, handleBook);
  server.on("/api/cancel", HTTP_POST, handleCancel);
  server.begin();
}

void loop() {
  server.handleClient();
  pollStm();
  expireBookings();
  if (bookingsDirty || millis() - lastBookTx > 2000) sendBookings();
}