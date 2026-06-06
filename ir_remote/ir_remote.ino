#include <Arduino.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRtext.h>
#include <IRutils.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ========== PIN ==========
#define IR_RECV_PIN  15
#define IR_SEND_PIN  4

// ========== PARAMETER ==========
const uint16_t kCaptureBufferSize   = 1024;
const uint8_t  kTimeout             = 50;
const uint16_t kMinUnknownSize      = 12;
const uint8_t  kTolerancePercentage = kTolerance;

// ========== OBJEK ==========
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kTimeout, true);
IRac   ac(IR_SEND_PIN);
Preferences prefs;
decode_results results;

WebServer server(80);
DNSServer  dns;

// ========== STATE ==========
stdAc::state_t acState;
stdAc::state_t acPrevState;
bool prevStateValid = false;

uint8_t  baseState[kStateSizeMax];
uint16_t stateLen   = 0;
bool     scanMode   = false;
String   protoName  = "UNKNOWN";

// WiFi
String cfgSSID, cfgPass, cfgAPI;
bool   apMode = false;

// ========================================
// WIFI CONFIG
// ========================================
void loadWifiConfig() {
  prefs.begin("wifi", true);
  cfgSSID = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgAPI  = prefs.getString("api",  "");
  prefs.end();
}

void saveWifiConfig(String ssid, String pass, String api) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("api",  api);
  prefs.end();
}

// ========================================
// PORTAL HTML
// ========================================
void sendPage(String body) {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Control AC</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font:15px sans-serif;background:#f0f4f8;min-height:100vh;"
    "display:flex;justify-content:center;padding:20px}"
    ".c{background:#fff;border-radius:12px;padding:24px;width:100%;"
    "max-width:380px;box-shadow:0 2px 12px rgba(0,0,0,.12);align-self:flex-start}"
    "h2{color:#2d3748;margin-bottom:20px;font-size:1.15em;text-align:center}"
    "label{display:block;color:#4a5568;font-size:.82em;font-weight:600;margin-bottom:4px}"
    "input{width:100%;padding:9px 10px;border:1px solid #cbd5e0;border-radius:8px;"
    "font-size:.95em;margin-bottom:13px}"
    "input:focus{border-color:#4299e1;outline:none}"
    "button{width:100%;padding:11px;background:#4299e1;color:#fff;border:none;"
    "border-radius:8px;font-size:.95em;cursor:pointer;font-weight:600}"
    "button:active{background:#2b6cb0}"
    ".ok{margin-top:14px;padding:10px;border-radius:8px;background:#c6f6d5;"
    "color:#276749;text-align:center;font-size:.88em}"
    ".err{margin-top:14px;padding:10px;border-radius:8px;background:#fed7d7;"
    "color:#c53030;text-align:center;font-size:.88em}"
    "</style></head><body><div class='c'>"
    "<h2>&#127777; Control AC Setup</h2>" +
    body +
    "</div></body></html>";
  server.send(200, "text/html", html);
}

String formHTML() {
  return
    "<form method='POST' action='/save'>"
    "<label>WiFi SSID</label>"
    "<input name='ssid' placeholder='Nama WiFi' required>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='Password WiFi'>"
    "<label>API Key</label>"
    "<input name='api' placeholder='API Key (opsional)'>"
    "<button type='submit'>Simpan &amp; Sambung</button>"
    "</form>";
}

void handleRoot() { sendPage(formHTML()); }

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String api  = server.arg("api");

  if (ssid.isEmpty()) {
    sendPage(formHTML() + "<div class='err'>SSID tidak boleh kosong!</div>");
    return;
  }

  saveWifiConfig(ssid, pass, api);
  sendPage("<div class='ok'>Tersimpan!<br>ESP32 menyambung ke WiFi &amp; restart...</div>");
  delay(2000);
  ESP.restart();
}

// ========================================
// WIFI CONNECT / AP MODE
// ========================================
void startConfigPortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ControlAC-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/",        handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();

  Serial.println("================================");
  Serial.println("  Mode: WiFi Setup Portal");
  Serial.println("  AP  : ControlAC-Setup");
  Serial.print  ("  IP  : "); Serial.println(WiFi.softAPIP());
  Serial.println("  Buka: http://192.168.4.1");
  Serial.println("================================");
}

bool connectWifi() {
  if (cfgSSID.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
  Serial.print("Konek ke " + cfgSSID);
  for (uint8_t i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\n✗ Gagal konek WiFi → buka portal");
  return false;
}

// ========================================
// SAVE & LOAD AC
// ========================================
void saveToFlash() {
  prefs.begin("ac", false);
  prefs.putString("proto",   protoName);
  prefs.putInt("protocol",   (int)acState.protocol);
  prefs.putInt("model",      acState.model);
  prefs.putBool("power",     acState.power);
  prefs.putFloat("temp",     acState.degrees);
  prefs.putBool("celsius",   acState.celsius);
  prefs.putInt("mode",       (int)acState.mode);
  prefs.putInt("fan",        (int)acState.fanspeed);
  prefs.putInt("swingv",     (int)acState.swingv);
  prefs.putInt("swingh",     (int)acState.swingh);
  prefs.putBool("turbo",     acState.turbo);
  prefs.putBool("econo",     acState.econo);
  prefs.putBool("filter",    acState.filter);
  prefs.putUInt("statelen",  stateLen);
  if (stateLen > 0) prefs.putBytes("base", baseState, stateLen);
  prefs.end();
}

void loadFromFlash() {
  prefs.begin("ac", true);
  protoName            = prefs.getString("proto", "UNKNOWN");
  acState.protocol     = (decode_type_t)prefs.getInt("protocol", (int)decode_type_t::UNKNOWN);
  acState.model        = prefs.getInt("model", -1);
  acState.power        = prefs.getBool("power", false);
  acState.degrees      = prefs.getFloat("temp", 24);
  acState.celsius      = prefs.getBool("celsius", true);
  acState.mode         = (stdAc::opmode_t)prefs.getInt("mode", (int)stdAc::opmode_t::kCool);
  acState.fanspeed     = (stdAc::fanspeed_t)prefs.getInt("fan", (int)stdAc::fanspeed_t::kAuto);
  acState.swingv       = (stdAc::swingv_t)prefs.getInt("swingv", (int)stdAc::swingv_t::kOff);
  acState.swingh       = (stdAc::swingh_t)prefs.getInt("swingh", (int)stdAc::swingh_t::kOff);
  acState.turbo        = prefs.getBool("turbo", false);
  acState.econo        = prefs.getBool("econo", false);
  acState.filter       = prefs.getBool("filter", false);
  acState.light        = false;
  acState.clean        = false;
  acState.beep         = false;
  acState.sleep        = -1;
  acState.clock        = -1;
  stateLen             = prefs.getUInt("statelen", 0);
  if (stateLen > 0) prefs.getBytes("base", baseState, stateLen);
  prefs.end();
}

// ========================================
// KIRIM SINYAL
// ========================================
void sendSignal() {
  if (acState.protocol == decode_type_t::UNKNOWN || stateLen == 0) {
    Serial.println("❌ Belum scan! Ketik 'scan'"); return;
  }
  if (!ac.isProtocolSupported(acState.protocol)) {
    Serial.println("❌ Protocol tidak didukung: " + protoName); return;
  }
  Serial.print("Mengirim ["); Serial.print(protoName);
  Serial.print("] P="); Serial.print(acState.power ? "ON" : "OFF");
  Serial.print(" T="); Serial.print(acState.degrees);
  Serial.print(" M="); Serial.println(IRac::opmodeToString(acState.mode));

  stdAc::state_t* prev = prevStateValid ? &acPrevState : nullptr;
  ac.sendAc(acState, prev);
  acPrevState    = acState;
  prevStateValid = true;
  Serial.println("✓ Terkirim!");
  saveToFlash();
  printStatus();
}

// ========================================
// PRINT STATUS
// ========================================
void printStatus() {
  Serial.println("========== STATUS AC ==========");
  Serial.print("Protocol : "); Serial.println(protoName);
  Serial.print("Model    : "); Serial.println(acState.model);
  Serial.print("Power    : "); Serial.println(acState.power ? "ON" : "OFF");
  Serial.print("Suhu     : "); Serial.print(acState.degrees); Serial.println(" C");
  Serial.print("Mode     : "); Serial.println(IRac::opmodeToString(acState.mode));
  Serial.print("Fan      : "); Serial.println(IRac::fanspeedToString(acState.fanspeed));
  Serial.print("Swing V  : "); Serial.println(IRac::swingvToString(acState.swingv));
  Serial.print("Swing H  : "); Serial.println(IRac::swinghToString(acState.swingh));
  Serial.print("Turbo    : "); Serial.println(acState.turbo ? "ON" : "OFF");
  Serial.print("Econo    : "); Serial.println(acState.econo ? "ON" : "OFF");
  Serial.print("WiFi     : "); Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : (apMode ? "Portal" : "Offline"));
  Serial.print("API Key  : "); Serial.println(cfgAPI.isEmpty() ? "-" : cfgAPI.substring(0, 4) + "****");
  Serial.print("Data     : "); Serial.println(stateLen > 0 ? "✓ Siap" : "✗ Belum scan");
  Serial.println("================================");
}

// ========================================
// PRINT HELP
// ========================================
void printHelp() {
  Serial.println("=========== COMMAND ============");
  Serial.println("scan             → scan remote");
  Serial.println("power on/off     → nyala/mati");
  Serial.println("temp 16-30       → set suhu");
  Serial.println("mode cool/heat/dry/fan/auto");
  Serial.println("fan auto/min/low/med/high/max");
  Serial.println("swingv on/off    → swing vertikal");
  Serial.println("swingh on/off    → swing horizontal");
  Serial.println("turbo on/off     → turbo mode");
  Serial.println("econo on/off     → econo mode");
  Serial.println("send             → kirim ulang");
  Serial.println("status           → lihat status");
  Serial.println("reset            → hapus data AC");
  Serial.println("reset wifi       → hapus WiFi & restart portal");
  Serial.println("reset api        → hapus API key");
  Serial.println("help             → menu ini");
  Serial.println("================================");
}

// ========================================
// PROSES SCAN
// ========================================
void processScanResult() {
  if (results.repeat)     { Serial.println("⚠ Repeat, coba lagi..."); return; }
  if (results.rawlen < 10){ Serial.println("⚠ Sinyal pendek...");    return; }

  decode_type_t proto = results.decode_type;

  uint32_t now = millis();
  Serial.printf("\nTimestamp : %06u.%03u\n", now / 1000, now % 1000);
  Serial.println("Library   : v" _IRREMOTEESP8266_VERSION_STR "\n");
  if (results.overflow) Serial.println("⚠ Buffer penuh!");
  Serial.print(resultToHumanReadableBasic(&results));
  String desc = IRAcUtils::resultAcToString(&results);
  if (desc.length()) Serial.println("Mesg Desc.: " + desc);
  Serial.println(resultToSourceCode(&results));

  if (!hasACState(proto)) {
    Serial.println("⚠ Bukan AC protocol! Coba lagi..."); return;
  }
  if (!ac.isProtocolSupported(proto)) {
    Serial.println("⚠ Protocol tidak didukung IRac: " + typeToString(proto, false)); return;
  }

  stateLen = results.bits / 8;
  memcpy(baseState, results.state, stateLen);
  protoName = typeToString(proto, false);

  if (IRAcUtils::decodeToState(&results, &acState, nullptr)) {
    Serial.println("✓ State decoded!");
  } else {
    acState.protocol = proto;
    acState.model    = -1;
  }

  acState.light = false; acState.clean = false;
  acState.beep  = false; acState.sleep = -1; acState.clock = -1;
  prevStateValid = false;
  scanMode       = false;
  saveToFlash();

  Serial.println("✓ Scan selesai! Protocol: " + protoName);
  printHelp();
  printStatus();
}

// ========================================
// PROSES COMMAND
// ========================================
void processCommand(String cmd) {
  cmd.trim();
  String c = cmd;
  c.toLowerCase();

  if (c == "scan") {
    scanMode = true;
    Serial.println("================================");
    Serial.println("Mode SCAN aktif!");
    Serial.println("Tekan sembarang tombol remote AC");
    Serial.println("================================");

  } else if (c == "power on")  { acState.power = true;  sendSignal();
  } else if (c == "power off") { acState.power = false; sendSignal();

  } else if (c.startsWith("temp ")) {
    float t = c.substring(5).toFloat();
    if (t >= 16 && t <= 30) { acState.degrees = t; sendSignal(); }
    else Serial.println("Suhu harus 16-30!");

  } else if (c == "mode cool") { acState.mode = stdAc::opmode_t::kCool; sendSignal();
  } else if (c == "mode heat") { acState.mode = stdAc::opmode_t::kHeat; sendSignal();
  } else if (c == "mode dry")  { acState.mode = stdAc::opmode_t::kDry;  sendSignal();
  } else if (c == "mode fan")  { acState.mode = stdAc::opmode_t::kFan;  sendSignal();
  } else if (c == "mode auto") { acState.mode = stdAc::opmode_t::kAuto; sendSignal();

  } else if (c == "fan auto")  { acState.fanspeed = stdAc::fanspeed_t::kAuto;   sendSignal();
  } else if (c == "fan min")   { acState.fanspeed = stdAc::fanspeed_t::kMin;    sendSignal();
  } else if (c == "fan low")   { acState.fanspeed = stdAc::fanspeed_t::kLow;    sendSignal();
  } else if (c == "fan med")   { acState.fanspeed = stdAc::fanspeed_t::kMedium; sendSignal();
  } else if (c == "fan high")  { acState.fanspeed = stdAc::fanspeed_t::kHigh;   sendSignal();
  } else if (c == "fan max")   { acState.fanspeed = stdAc::fanspeed_t::kMax;    sendSignal();

  } else if (c == "swingv on")  { acState.swingv = stdAc::swingv_t::kAuto; sendSignal();
  } else if (c == "swingv off") { acState.swingv = stdAc::swingv_t::kOff;  sendSignal();
  } else if (c == "swingh on")  { acState.swingh = stdAc::swingh_t::kAuto; sendSignal();
  } else if (c == "swingh off") { acState.swingh = stdAc::swingh_t::kOff;  sendSignal();

  } else if (c == "turbo on")  { acState.turbo = true;  sendSignal();
  } else if (c == "turbo off") { acState.turbo = false; sendSignal();
  } else if (c == "econo on")  { acState.econo = true;  sendSignal();
  } else if (c == "econo off") { acState.econo = false; sendSignal();

  } else if (c == "send")   { sendSignal();
  } else if (c == "status") { printStatus();
  } else if (c == "help")   { printHelp();

  } else if (c == "reset") {
    prefs.begin("ac", false); prefs.clear(); prefs.end();
    acState          = stdAc::state_t();
    acState.protocol = decode_type_t::UNKNOWN;
    acState.model    = -1;
    acState.degrees  = 24;
    acState.celsius  = true;
    acState.mode     = stdAc::opmode_t::kCool;
    acState.fanspeed = stdAc::fanspeed_t::kAuto;
    acState.swingv   = stdAc::swingv_t::kOff;
    acState.swingh   = stdAc::swingh_t::kOff;
    acState.sleep    = -1;
    acState.clock    = -1;
    protoName        = "UNKNOWN";
    stateLen         = 0;
    prevStateValid   = false;
    Serial.println("✓ Reset AC! Ketik 'scan'");

  } else if (c == "reset wifi") {
    prefs.begin("wifi", false); prefs.clear(); prefs.end();
    Serial.println("✓ WiFi dihapus! Restart ke portal...");
    delay(1000);
    ESP.restart();

  } else if (c == "reset api") {
    cfgAPI = "";
    prefs.begin("wifi", false);
    prefs.putString("api", "");
    prefs.end();
    Serial.println("✓ API Key dihapus!");

  } else {
    Serial.println("Command tidak dikenal. Ketik 'help'");
  }
}

// ========================================
// SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(50);

  irrecv.setUnknownThreshold(kMinUnknownSize);
  irrecv.setTolerance(kTolerancePercentage);
  irrecv.enableIRIn();

  // Init default AC state
  acState.protocol = decode_type_t::UNKNOWN;
  acState.model    = -1;
  acState.degrees  = 24;
  acState.celsius  = true;
  acState.mode     = stdAc::opmode_t::kCool;
  acState.fanspeed = stdAc::fanspeed_t::kAuto;
  acState.swingv   = stdAc::swingv_t::kOff;
  acState.swingh   = stdAc::swingh_t::kOff;
  acState.turbo    = false;
  acState.econo    = false;
  acState.light    = false;
  acState.filter   = false;
  acState.clean    = false;
  acState.beep     = false;
  acState.sleep    = -1;
  acState.clock    = -1;

  loadFromFlash();
  loadWifiConfig();

  // WiFi: coba konek, gagal → portal
  if (!connectWifi()) {
    startConfigPortal();
  }

  Serial.println("================================");
  Serial.println("  ESP32 AC Universal");
  Serial.println("================================");

  if (stateLen == 0) {
    Serial.println("⚠ Belum ada data AC!");
    Serial.println("Ketik 'scan' → tekan tombol remote");
  } else {
    Serial.print("✓ Protocol : "); Serial.println(protoName);
  }

  printHelp();
  printStatus();
}

// ========================================
// LOOP
// ========================================
void loop() {
  // Handle portal jika AP mode
  if (apMode) {
    dns.processNextRequest();
    server.handleClient();
  }

  // IR receiver
  if (irrecv.decode(&results)) {
    if (scanMode) {
      processScanResult();
    } else {
      if (!results.repeat && hasACState(results.decode_type)) {
        if (results.decode_type == acState.protocol) {
          Serial.println("\n>>> Remote asli, sync state...");
          stdAc::state_t s;
          if (IRAcUtils::decodeToState(&results, &s, nullptr)) {
            acState.power    = s.power;
            acState.degrees  = s.degrees;
            acState.mode     = s.mode;
            acState.fanspeed = s.fanspeed;
            acState.swingv   = s.swingv;
            acState.swingh   = s.swingh;
            Serial.println(IRAcUtils::resultAcToString(&results));
          }
        }
      }
    }
    irrecv.resume();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    processCommand(cmd);
  }
}
