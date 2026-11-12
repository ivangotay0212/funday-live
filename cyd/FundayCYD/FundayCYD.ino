// Funday Live for the Cheap Yellow Display (ESP32-2432S028R, 320x240 ILI9341 + XPT2046 touch)
//
// A pocket version of index.html. Same data (Sleeper league + ESPN public scoreboard), same rules
// for "has the ball / red zone / defense on the field", same lineup-health checks.
//
// Screens (tap anywhere for the next one, hold 1.5 s to pick a different team):
//   0 Scorebug   your team vs opponent, win bar, who is still to play, lineup alert strip
//   1 My lineup  starters with points; gold edge = has the ball, red = red zone, blue = on D
//   2 Opponent   same for the other side
//   3 NFL games  every game this week; green edge = your starter is in it, red = theirs
// A scores ticker runs along the bottom. Touchdowns and red zone trips involving either
// lineup take over the whole screen for 6 seconds (tap to dismiss).
//
// Libraries: TFT_eSPI (with the User_Setup.h in this folder's parent), ArduinoJson 7,
// XPT2046_Touchscreen. Board: "ESP32 Dev Module", Partition scheme: "Huge APP".
//
// Networking runs in its own task on core 0, drawing and touch on core 1, so the ticker keeps
// moving while a slow download is in progress. The two sides share one snapshot behind a mutex.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <map>
#include <algorithm>
#include "config.h"

// ---------------------------------------------------------------- pins (2432S028R)
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
#define LED_R      4
#define LED_G      16
#define LED_B      17
#define SPEAKER    26

// ---------------------------------------------------------------- colours (same palette as the web page)
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t C_INK = RGB(10, 15, 26), C_PANEL = RGB(18, 26, 43), C_PANEL2 = RGB(24, 34, 58), C_LINE = RGB(36, 48, 77);
const uint16_t C_TEXT = RGB(236, 239, 246), C_DIM = RGB(140, 151, 179), C_FAINT = RGB(92, 104, 132);
const uint16_t C_GOLD = RGB(255, 200, 0), C_RED = RGB(242, 72, 84), C_BLUE = RGB(0, 190, 240), C_GREEN = RGB(40, 212, 120);
const uint16_t C_GOLD_INK = RGB(32, 24, 0);
const uint16_t T_OFF = RGB(49, 49, 37), T_RZ = RGB(63, 35, 51), T_DEF = RGB(16, 47, 62);   // row tints
const uint16_t C_TD_BG = RGB(6, 54, 31), C_RZ_BG = RGB(74, 10, 16);

// ---------------------------------------------------------------- config
#define SLEEPER "https://api.sleeper.app/v1"
#define ESPN    "https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard"
const int RED_ZONE = 20;
const int MAXS = 16, MAXG = 16, MAXT = 16;

// ---------------------------------------------------------------- snapshot shared with the UI (plain data, safe to memcpy)
struct PRow {               // one starter
  char slot[5], name[22], tag[8], l1[16], l2[16];
  float pts;
  uint8_t lvl;              // 0 healthy, 1 questionable, 2 can't score
  uint8_t role;             // 0 none, 1 has ball, 2 on D, 3 red zone
  uint8_t gs;               // 0 no game, 1 pre, 2 live, 3 final
};
struct SideV { int rid; char name[30], meta[34]; float pts; uint8_t n, live, pre; PRow rows[MAXS]; };
struct GameV { char away[4], home[4], poss[4], l2[24]; int as, hs; char st; bool rz; uint8_t mine, opp; };
struct TeamV { int rid; char name[30], owner[22]; };
struct AlertV { char kind; char title[24], sub[40], mine[44], opp[44]; };
struct Snap {
  uint32_t ver;
  char league[34]; int week;
  char status;              // 'W' loading, 'L' live, 'I' no games on, 'E' offline
  char note[48];
  bool hasMatch; SideV me, op;
  uint8_t ng; GameV g[MAXG];
  uint8_t nt; TeamV t[MAXT];
  uint32_t alertSeq; AlertV alert;
};

// net-task private data
struct PInfo { String shortName, pos, team, inj; };
struct Team { int rid; String ownerId, name, owner; int w, l, t; };
struct Side { int rid, mid; float pts; std::vector<String> st; std::vector<float> sp; };
struct Game {
  String id, home, away, poss, dd, clock, detail;
  int hs = 0, as = 0, period = 0, togo = -1;
  char state = 'P';         // 'P' pre, 'I' in progress, 'F' final
  bool rz = false, rzFired = false;
  time_t kick = 0;
};
struct Health { uint8_t lvl; const char* tag; };

Snap shared;                // written by the net task, read by the UI, guarded by mtx
Snap view;                  // the UI's private copy
Snap scratch;               // the net task's scratch copy
SemaphoreHandle_t mtx;
TaskHandle_t netH = nullptr;
volatile int g_rid = MY_ROSTER_ID;
Preferences prefs;

static void cpy(char* dst, size_t n, const String& s) { strncpy(dst, s.c_str(), n - 1); dst[n - 1] = 0; }
#define CPY(dst, s) cpy(dst, sizeof(dst), s)

// ================================================================ NET TASK (core 0)


String N_league, N_err;
int N_week = 1;
std::vector<String> N_slots, N_byes;
std::vector<Team> N_teams;
std::vector<Side> N_sides;
std::vector<Game> N_games;
std::map<String, PInfo> N_players;
bool N_haveLeague = false, N_haveMatch = false;
String N_note = "Connecting to Wi-Fi";

// ---- helpers (ported from index.html)
String norm(String t) {
  t.trim(); t.toUpperCase();
  static const char* M[][2] = { {"WSH","WAS"}, {"JAC","JAX"}, {"LA","LAR"}, {"STL","LAR"}, {"ARZ","ARI"},
                                {"BLT","BAL"}, {"CLV","CLE"}, {"HST","HOU"}, {"SD","LAC"}, {"OAK","LV"} };
  for (auto& m : M) if (t == m[0]) return m[1];
  return t;
}
bool isDef(const String& id) { return id.length() && isalpha((unsigned char)id[0]); }
bool isEmpty(const String& id) { return !id.length() || id == "0"; }
const PInfo* pinfo(const String& id) { auto it = N_players.find(id); return it == N_players.end() ? nullptr : &it->second; }
String teamOf(const String& id) {
  if (isEmpty(id)) return "";
  if (isDef(id)) return norm(id);
  const PInfo* p = pinfo(id);
  return p ? norm(p->team) : "";
}
String posOf(const String& id) { if (isDef(id)) return "DEF"; const PInfo* p = pinfo(id); return p ? p->pos : ""; }
String pshort(const String& id) {
  if (isEmpty(id)) return "Empty";
  if (isDef(id)) return norm(id) + " DEF";
  const PInfo* p = pinfo(id);
  return p ? p->shortName : "#" + id;
}
Game* gameFor(const String& team) {
  if (!team.length()) return nullptr;
  for (auto& g : N_games) if (g.home == team || g.away == team) return &g;
  return nullptr;
}
bool anyLive() { for (auto& g : N_games) if (g.state == 'I') return true; return false; }
Team* teamBy(int rid) { for (auto& t : N_teams) if (t.rid == rid) return &t; return nullptr; }
Side* sideBy(int rid) { for (auto& s : N_sides) if (s.rid == rid) return &s; return nullptr; }
Side* oppOf(Side* s) {
  if (!s || !s->mid) return nullptr;
  for (auto& o : N_sides) if (o.mid == s->mid && o.rid != s->rid) return &o;
  return nullptr;
}
bool onBye(const String& t) { for (auto& b : N_byes) if (b == t) return true; return false; }

// Same rules as the board: 1 offense on the field, 2 defense on the field, 3 offense in the red zone.
uint8_t fieldRole(const String& id) {
  String team = teamOf(id); Game* g = gameFor(team);
  if (!g || g->state != 'I' || !g->poss.length()) return 0;
  if (isDef(id)) return g->poss != team ? 2 : 0;
  if (g->poss != team) return 0;
  if (posOf(id) == "K") return g->rz ? 3 : 0;
  return g->rz ? 3 : 1;
}

Health health(const String& id) {
  if (isEmpty(id)) return {2, "EMPTY"};
  String team = teamOf(id); const PInfo* p = pinfo(id);
  if (p) {
    const String& j = p->inj;
    if (j == "Out") return {2, "OUT"};
    if (j == "IR") return {2, "IR"};
    if (j == "Doubtful") return {2, "D"};
    if (j == "PUP") return {2, "PUP"};
    if (j == "Sus") return {2, "SUSP"};
    if (j == "NA" || j == "COV" || j == "DNR") return {2, "INACT"};
  }
  if (!isDef(id) && p && !team.length()) return {2, "FA"};
  if (team.length() && N_games.size() && !gameFor(team)) return onBye(team) ? Health{2, "BYE"} : Health{2, "NO GM"};
  if (p && p->inj == "Questionable") return {1, "Q"};
  return {0, ""};
}

String slotLabel(const String& s) {
  if (s == "SUPER_FLEX") return "SF";
  if (s == "REC_FLEX") return "R/F";
  if (s == "WRRB_FLEX") return "W/R";
  if (s == "FLEX") return "FLX";
  if (s == "IDP_FLEX") return "IDP";
  return s.substring(0, 4);
}

String clockStr(const Game& g) {
  if (g.detail.startsWith("Half")) return "Half";
  String p;
  if (g.period > 4) p = "OT"; else p = "Q" + String(g.period);
  return p + " " + g.clock;
}
String kickStr(time_t t) {
  if (!t) return "TBD";
  static const char* D[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm lt; localtime_r(&t, &lt);
  int h = lt.tm_hour % 12; if (!h) h = 12;
  char b[16]; snprintf(b, sizeof b, "%s %d:%02d%c", D[lt.tm_wday], h, lt.tm_min, lt.tm_hour < 12 ? 'a' : 'p');
  return b;
}
// "2026-11-15T18:00Z" -> epoch seconds (UTC)
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
time_t parseIso(const char* s) {
  int Y, M, D, h = 0, m = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) < 3) return 0;
  return (time_t)(daysFromCivil(Y, M, D) * 86400L + h * 3600L + m * 60L);
}

// ---- HTTP + JSON
// A small read buffer in front of the TLS client: ArduinoJson pulls one byte at a time, and the
// Sleeper player list is several MB, so this makes the download several times faster.
class BufStream : public Stream {
  WiFiClient& c; uint8_t buf[1024]; int n = 0, i = 0;
  bool fill() {
    uint32_t t0 = millis();
    while (millis() - t0 < 15000) {
      int a = c.available();
      if (a > 0) { n = c.read(buf, std::min(a, (int)sizeof buf)); i = 0; return n > 0; }
      if (!c.connected()) return false;
      delay(2);
    }
    return false;
  }
 public:
  BufStream(WiFiClient& c) : c(c) {}
  int available() override { return (n - i) + c.available(); }
  int read() override { if (i >= n && !fill()) return -1; return buf[i++]; }
  int peek() override { if (i >= n && !fill()) return -1; return buf[i]; }
  size_t write(uint8_t) override { return 0; }
};

bool fetch(const String& url, JsonDocument& doc, const char* filterJson) {
  WiFiClientSecure client;
  client.setInsecure();                      // public read-only APIs; skips certificate pinning
  HTTPClient http;
  http.useHTTP10(true);                      // no chunked encoding, so the body streams straight into ArduinoJson
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  if (!http.begin(client, url)) { N_err = "Can't connect"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { N_err = "HTTP " + String(code); http.end(); return false; }
  BufStream bs(http.getStream());
  DeserializationError e;
  if (filterJson) {
    JsonDocument filter;
    deserializeJson(filter, filterJson);
    e = deserializeJson(doc, bs, DeserializationOption::Filter(filter));
  } else {
    e = deserializeJson(doc, bs);
  }
  http.end();
  if (e) { N_err = String("JSON ") + e.c_str(); return false; }
  return true;
}

// ---- events (touchdowns, red zone trips) -> a full-screen alert on the UI side
void pushAlert(char kind, const String& title, const String& sub, const String& mine, const String& opp) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  shared.alert.kind = kind;
  CPY(shared.alert.title, title); CPY(shared.alert.sub, sub);
  CPY(shared.alert.mine, mine); CPY(shared.alert.opp, opp);
  shared.alertSeq++;
  xSemaphoreGive(mtx);
  Serial.printf("[alert] %c %s | %s\n", kind, title.c_str(), sub.c_str());
}
String namesOn(Side* s, const String& team, bool withK) {
  if (!s) return "";
  std::vector<String> v;
  for (auto& id : s->st)
    if (!isEmpty(id) && !isDef(id) && teamOf(id) == team && (withK || posOf(id) != "K")) v.push_back(pshort(id));
  String out;
  for (size_t i = 0; i < v.size() && i < 2; i++) out += (i ? ", " : "") + v[i];
  if (v.size() > 2) out += " +" + String(v.size() - 2);
  return out;
}
void gameEvents(std::vector<Game>& old, std::vector<Game>& fresh) {
  Side* me = sideBy(g_rid); Side* op = oppOf(me);
  for (auto& g : fresh) {
    Game* o = nullptr;
    for (auto& x : old) if (x.id == g.id) { o = &x; break; }
    g.rzFired = o ? o->rzFired : false;
    if (g.state != 'I') { g.rzFired = false; continue; }
    if (o && o->state == 'I') {
      for (int k = 0; k < 2; k++) {
        const String& ab = k ? g.home : g.away;
        int d = k ? g.hs - o->hs : g.as - o->as;
        if (d < 3 || d > 8 || d == 4 || d == 5) continue;
        String mine = namesOn(me, ab, true), opp = namesOn(op, ab, true);
        if (!mine.length() && !opp.length()) continue;
        String line = g.away + " " + String(g.as) + ", " + g.home + " " + String(g.hs);
        pushAlert(d >= 6 ? 'T' : 'F', String(d >= 6 ? "TOUCHDOWN " : "FIELD GOAL ") + ab, line, mine, opp);
      }
    }
    if (o && o->poss != g.poss) g.rzFired = false;
    if (!(g.rz && g.poss.length())) g.rzFired = false;
    else if (!g.rzFired) {
      g.rzFired = true;
      if (o && o->state == 'I') {
        String mine = namesOn(me, g.poss, false), opp = namesOn(op, g.poss, false);
        if (mine.length() || opp.length())
        {
          String sub = g.poss + " ball";
          if (g.togo > 0) sub += " at the " + String(g.togo);
          pushAlert('R', "RED ZONE", sub, mine, opp);
        }
      }
    }
  }
}

// ---- loaders
bool loadLeague() {
  JsonDocument d;
  if (!fetch(SLEEPER "/state/nfl", d, R"({"week":true,"display_week":true})")) return false;
  int w = d["display_week"] | 0; if (!w) w = d["week"] | 1;
  N_week = std::max(1, std::min(18, w));

  d.clear();
  if (!fetch(SLEEPER "/league/" LEAGUE_ID, d, R"({"name":true,"roster_positions":true})")) return false;
  N_league = (const char*)(d["name"] | "League");
  N_slots.clear();
  for (JsonVariant v : d["roster_positions"].as<JsonArray>()) {
    String s = v.as<const char*>() ? v.as<const char*>() : "";
    if (s != "BN" && s != "IR" && s != "TAXI") N_slots.push_back(s);
  }

  d.clear();
  if (!fetch(SLEEPER "/league/" LEAGUE_ID "/users", d, R"([{"user_id":true,"display_name":true,"metadata":{"team_name":true}}])")) return false;
  std::map<String, std::pair<String, String>> users;   // user_id -> (team name, display name)
  for (JsonObject u : d.as<JsonArray>()) {
    String tn = (const char*)(u["metadata"]["team_name"] | ""); tn.trim();
    String dn = (const char*)(u["display_name"] | "");
    users[(const char*)(u["user_id"] | "")] = { tn, dn };
  }

  d.clear();
  if (!fetch(SLEEPER "/league/" LEAGUE_ID "/rosters", d, R"([{"roster_id":true,"owner_id":true,"settings":{"wins":true,"losses":true,"ties":true}}])")) return false;
  N_teams.clear();
  for (JsonObject r : d.as<JsonArray>()) {
    Team t;
    t.rid = r["roster_id"] | 0;
    t.ownerId = (const char*)(r["owner_id"] | "");
    t.w = r["settings"]["wins"] | 0; t.l = r["settings"]["losses"] | 0; t.t = r["settings"]["ties"] | 0;
    auto it = users.find(t.ownerId);
    t.owner = it != users.end() ? it->second.second : "";
    if (it != users.end() && it->second.first.length()) t.name = it->second.first;
    else if (t.owner.length()) t.name = t.owner;
    else t.name = "Team " + String(t.rid);
    N_teams.push_back(t);
  }
  std::sort(N_teams.begin(), N_teams.end(), [](const Team& a, const Team& b) { return a.rid < b.rid; });
  N_haveLeague = true;
  return true;
}

bool loadMatchups() {
  JsonDocument d;
  String url = SLEEPER "/league/" LEAGUE_ID "/matchups/" + String(N_week);
  if (!fetch(url, d, R"([{"roster_id":true,"matchup_id":true,"points":true,"starters":true,"starters_points":true}])")) return false;
  std::vector<Side> fresh;
  for (JsonObject m : d.as<JsonArray>()) {
    Side s;
    s.rid = m["roster_id"] | 0;
    s.mid = m["matchup_id"] | 0;
    s.pts = m["points"] | 0.0f;
    for (JsonVariant v : m["starters"].as<JsonArray>()) s.st.push_back(v.as<const char*>() ? v.as<const char*>() : "0");
    for (JsonVariant v : m["starters_points"].as<JsonArray>()) s.sp.push_back(v.as<float>());
    fresh.push_back(s);
  }
  N_sides = fresh;
  N_haveMatch = true;
  return true;
}

std::vector<String> wantedIds() {
  std::vector<String> w;
  for (auto& s : N_sides) for (auto& id : s.st) if (!isEmpty(id) && !isDef(id)) w.push_back(id);
  return w;
}
bool missingPlayers() { for (auto& id : wantedIds()) if (!pinfo(id)) return true; return false; }

// Sleeper's full player list is several MB, so we stream it through a filter that keeps only the
// starters in this league, and only ask for it every 6 hours (Sleeper asks for this to be rare).
bool loadPlayers() {
  std::vector<String> ids = wantedIds();
  if (ids.empty()) return true;
  String filter = "{";
  for (size_t i = 0; i < ids.size(); i++)
    filter += (i ? ",\"" : "\"") + ids[i] + "\":{\"first_name\":true,\"last_name\":true,\"position\":true,\"team\":true,\"injury_status\":true}";
  filter += "}";
  JsonDocument d;
  uint32_t t0 = millis();
  if (!fetch(SLEEPER "/players/nfl", d, filter.c_str())) return false;
  for (JsonPair kv : d.as<JsonObject>()) {
    JsonObject p = kv.value().as<JsonObject>();
    PInfo pi;
    String fn = (const char*)(p["first_name"] | ""), ln = (const char*)(p["last_name"] | "");
    if (fn.length()) pi.shortName = String(fn[0]) + ". " + ln; else pi.shortName = ln;
    pi.pos = (const char*)(p["position"] | "");
    pi.team = (const char*)(p["team"] | "");
    pi.inj = (const char*)(p["injury_status"] | "");
    N_players[kv.key().c_str()] = pi;
  }
  Serial.printf("[players] %u kept in %lus\n", (unsigned)d.as<JsonObject>().size(), (unsigned long)((millis() - t0) / 1000));
  return true;
}

bool loadGames() {
  JsonDocument d;
  const char* F = R"({"week":{"teamsOnBye":[{"abbreviation":true}]},
    "events":[{"id":true,"date":true,"status":{"period":true,"displayClock":true,"type":{"state":true,"shortDetail":true}},
    "competitions":[{"competitors":[{"homeAway":true,"score":true,"team":{"id":true,"abbreviation":true}}],
    "situation":{"possession":true,"downDistanceText":true,"shortDownDistanceText":true,"isRedZone":true}}]}]})";
  if (!fetch(ESPN, d, F)) return false;
  N_byes.clear();
  for (JsonObject b : d["week"]["teamsOnBye"].as<JsonArray>()) N_byes.push_back(norm((const char*)(b["abbreviation"] | "")));

  std::vector<Game> fresh;
  for (JsonObject ev : d["events"].as<JsonArray>()) {
    JsonObject c = ev["competitions"][0].as<JsonObject>();
    Game g;
    g.id = (const char*)(ev["id"] | "");
    g.kick = parseIso(ev["date"] | "");
    String tid[2], tab[2];
    int n = 0;
    for (JsonObject t : c["competitors"].as<JsonArray>()) {
      String ab = norm((const char*)(t["team"]["abbreviation"] | ""));
      int sc = atoi(t["score"] | "0");
      if (n < 2) { tid[n] = (const char*)(t["team"]["id"] | ""); tab[n] = ab; n++; }
      if (String((const char*)(t["homeAway"] | "")) == "home") { g.home = ab; g.hs = sc; } else { g.away = ab; g.as = sc; }
    }
    if (!g.home.length() || !g.away.length()) continue;
    String st = (const char*)(ev["status"]["type"]["state"] | "pre");
    g.state = st == "in" ? 'I' : st == "post" ? 'F' : 'P';
    g.period = ev["status"]["period"] | 0;
    g.clock = (const char*)(ev["status"]["displayClock"] | "");
    g.detail = (const char*)(ev["status"]["type"]["shortDetail"] | "");
    JsonObject sit = c["situation"].as<JsonObject>();
    if (g.state == 'I' && !sit.isNull()) {
      String pid = (const char*)(sit["possession"] | "");
      for (int i = 0; i < n; i++) if (pid.length() && tid[i] == pid) g.poss = tab[i];
      String ddl = (const char*)(sit["downDistanceText"] | "");
      g.dd = (const char*)(sit["shortDownDistanceText"] | "");
      // "1st & 10 at BUF 35": the team named is whose side of the field the ball is on.
      int at = ddl.lastIndexOf(" at ");
      if (at >= 0 && g.poss.length()) {
        String rem = ddl.substring(at + 4); rem.trim();
        int sp = rem.indexOf(' ');
        if (sp > 0) {
          int yd = rem.substring(sp + 1).toInt();
          if (yd > 0 && yd <= 50) g.togo = norm(rem.substring(0, sp)) == g.poss ? 100 - yd : yd;
        } else if (rem.toInt() == 50) g.togo = 50;
      }
      if (!g.dd.length() && at > 0) g.dd = ddl.substring(0, at);
      g.rz = g.poss.length() && ((sit["isRedZone"] | false) || (g.togo >= 0 && g.togo <= RED_ZONE));
    }
    fresh.push_back(g);
  }
  gameEvents(N_games, fresh);
  N_games = fresh;
  return true;
}

// ---- build the snapshot the UI draws from
void fillSide(SideV& v, Side& sd) {
  v.rid = sd.rid;
  Team* t = teamBy(sd.rid);
  String name = "Team " + String(sd.rid), meta;
  if (t) {
    name = t->name;
    meta = t->owner + "  " + String(t->w) + "-" + String(t->l);
    if (t->t) meta += "-" + String(t->t);
  }
  CPY(v.name, name);
  CPY(v.meta, meta);
  v.pts = sd.pts;
  v.n = std::min((int)sd.st.size(), MAXS);
  v.live = v.pre = 0;
  for (int i = 0; i < v.n; i++) {
    const String& id = sd.st[i];
    PRow& r = v.rows[i];
    CPY(r.slot, i < (int)N_slots.size() ? slotLabel(N_slots[i]) : String());
    CPY(r.name, pshort(id));
    Health h = health(id);
    r.lvl = h.lvl; CPY(r.tag, String(h.tag));
    r.role = fieldRole(id);
    r.pts = i < (int)sd.sp.size() ? sd.sp[i] : 0;
    String team = teamOf(id), l1, l2;
    Game* g = gameFor(team);
    if (!team.length()) { if (posOf(id).length()) l1 = "Free agent"; r.gs = 0; }
    else if (!g) { l1 = posOf(id) + " " + team; l2 = onBye(team) ? "Bye" : "No game"; r.gs = 0; }
    else {
      bool home = g->home == team;
      String opp = home ? g->away : g->home;
      int us = home ? g->hs : g->as, them = home ? g->as : g->hs;
      String vs = String(home ? "v " : "@ ") + opp;
      if (g->state == 'P') { l1 = vs; l2 = kickStr(g->kick); r.gs = 1; v.pre++; }
      else if (g->state == 'I') { l1 = String(us) + "-" + String(them) + " " + vs; l2 = clockStr(*g); r.gs = 2; v.live++; }
      else { l1 = String(us > them ? "W " : us < them ? "L " : "T ") + String(us) + "-" + String(them); l2 = "Final " + vs; r.gs = 3; }
    }
    CPY(r.l1, l1); CPY(r.l2, l2);
  }
}

void publish() {
  Snap& s = scratch;
  memset(&s, 0, sizeof s);
  CPY(s.league, N_league);
  s.week = N_week;
  s.status = N_err.length() ? 'E' : !N_haveLeague ? 'W' : anyLive() ? 'L' : 'I';
  CPY(s.note, N_err.length() ? N_err : N_note);

  Side* me = sideBy(g_rid);
  Side* op = oppOf(me);
  s.hasMatch = me && op;
  if (s.hasMatch) { fillSide(s.me, *me); fillSide(s.op, *op); }

  // games: live first, then upcoming by kickoff, then finals
  std::vector<Game*> gs;
  for (auto& g : N_games) gs.push_back(&g);
  auto rank = [](const Game* g) { return g->state == 'I' ? 0 : g->state == 'P' ? 1 : 2; };
  std::stable_sort(gs.begin(), gs.end(), [&](const Game* a, const Game* b) {
    return rank(a) != rank(b) ? rank(a) < rank(b) : a->kick < b->kick;
  });
  s.ng = std::min((int)gs.size(), MAXG);
  for (int i = 0; i < s.ng; i++) {
    Game& g = *gs[i]; GameV& v = s.g[i];
    CPY(v.away, g.away); CPY(v.home, g.home); CPY(v.poss, g.poss);
    v.as = g.as; v.hs = g.hs; v.st = g.state; v.rz = g.rz;
    String l2;
    if (g.state == 'P') l2 = kickStr(g.kick);
    else if (g.state == 'F') l2 = g.detail.length() ? g.detail : "Final";
    else { l2 = clockStr(g); if (g.dd.length()) l2 += "  " + g.dd; }
    CPY(v.l2, l2);
    v.mine = v.opp = 0;
    for (int k = 0; k < 2; k++) {
      Side* sd = k ? op : me;
      if (!sd) continue;
      for (auto& id : sd->st) { String t = teamOf(id); if (t.length() && (t == g.home || t == g.away)) (k ? v.opp : v.mine)++; }
    }
  }
  s.nt = std::min((int)N_teams.size(), MAXT);
  for (int i = 0; i < s.nt; i++) { s.t[i].rid = N_teams[i].rid; CPY(s.t[i].name, N_teams[i].name); CPY(s.t[i].owner, N_teams[i].owner); }

  xSemaphoreTake(mtx, portMAX_DELAY);
  s.ver = shared.ver + 1;
  s.alertSeq = shared.alertSeq;       // alerts are written straight into `shared`; keep them
  s.alert = shared.alert;
  memcpy(&shared, &s, sizeof s);
  xSemaphoreGive(mtx);
}

bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  N_note = "Connecting to Wi-Fi"; publish();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) { N_err = "No Wi-Fi"; WiFi.reconnect(); return false; }
  Serial.printf("[wifi] %s\n", WiFi.localIP().toString().c_str());
  N_note = "";
  return true;
}

void netTask(void*) {
  uint32_t tLeague = 0, tMatch = 0, tGames = 0, tPlayers = 0, tPlayersTry = 0;
  bool firstPlayers = true;
  for (;;) {
    if (wifiUp()) {
      uint32_t now = millis();
      bool live = anyLive(), changed = false;
      auto run = [&](bool (*fn)(), uint32_t& stamp) {
        if (fn()) { N_err = ""; stamp = millis(); changed = true; return true; }
        Serial.printf("[net] %s\n", N_err.c_str()); stamp = millis(); changed = true; return false;
      };
      if (!N_haveLeague || now - tLeague > 300000) {
        if (!N_haveLeague) { N_note = "Loading league"; publish(); }
        run(loadLeague, tLeague);
      }
      if (N_haveLeague && (!N_haveMatch || now - tMatch > (live ? 30000u : 120000u))) run(loadMatchups, tMatch);
      if (!tGames || now - tGames > (live ? 15000u : 60000u)) run(loadGames, tGames);
      // Player names and injury report: at boot, every 6 h when no game is on (12 h at most),
      // and when a new starter shows up (at most every 10 min).
      bool stale = now - tPlayers > (live ? 12 : 6) * 3600000UL;
      if (N_haveMatch && (firstPlayers || stale || (missingPlayers() && now - tPlayersTry > 600000))) {
        N_note = firstPlayers ? "Loading player names (1-2 min)" : "Refreshing injury report";
        publish();
        tPlayersTry = millis();
        if (run(loadPlayers, tPlayers)) firstPlayers = false;
        N_note = "";
      }
      if (changed) publish();
    }
    // wake early when the UI picks a different team
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) publish();
  }
}

// ================================================================ UI (core 1)

TFT_eSPI tft;
TFT_eSprite band(&tft);        // one horizontal strip, reused for every row, so nothing flickers
TFT_eSprite tape(&tft);        // the scores ticker
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
const int BAND_H = 70, TICK_Y = 220, TICK_H = 20, NSCREENS = 4;

int screen = 0;
bool dirty = true, picker = false, overlay = false;
uint32_t overlayUntil = 0, lastCycle = 0;

// draw text clipped to maxW, trimming with a trailing dot
void fitDraw(TFT_eSprite& s, const char* txt, int x, int y, int font, int maxW) {
  char b[48]; strncpy(b, txt, sizeof b - 1); b[sizeof b - 1] = 0;
  int len = strlen(b);
  if (s.textWidth(b, font) > maxW) {
    while (len > 1 && s.textWidth(b, font) > maxW) { len--; b[len - 1] = '.'; b[len] = 0; }
  }
  s.drawString(b, x, y, font);
}
void pushBand(int y, int h) { band.pushSprite(0, y, 0, 0, 320, h); }

void drawHeader(const char* title) {
  band.fillSprite(C_INK);
  band.setTextDatum(ML_DATUM);
  band.setTextColor(C_TEXT); band.drawString(title ? title : "FUNDAY", 6, 11, 2);
  if (!title) { band.setTextColor(C_GOLD); band.drawString("LIVE", 6 + band.textWidth("FUNDAY ", 2), 11, 2); }
  char wk[10]; snprintf(wk, sizeof wk, "WK %d", view.week);
  band.setTextColor(C_DIM); band.drawString(wk, 112, 11, 2);
  for (int i = 0; i < NSCREENS; i++) band.fillCircle(170 + i * 10, 11, 2, i == screen ? C_GOLD : C_LINE);
  const char* st = view.status == 'L' ? "LIVE" : view.status == 'E' ? "OFFLINE" : view.status == 'W' ? "LOADING" : "NO GAMES";
  uint16_t dot = view.status == 'L' ? C_GREEN : view.status == 'E' ? C_RED : view.status == 'W' ? C_GOLD : C_FAINT;
  band.setTextDatum(MR_DATUM); band.setTextColor(view.status == 'E' ? C_RED : C_DIM);
  band.drawString(st, 314, 11, 2);
  band.fillCircle(314 - band.textWidth(st, 2) - 8, 11, 4, dot);
  band.drawFastHLine(0, 21, 320, C_LINE);
  pushBand(0, 22);
}

void drawMessage(const char* big, const char* small) {
  tft.fillRect(0, 22, 320, TICK_Y - 22, C_INK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_INK); tft.drawString(big, 160, 100, 4);
  tft.setTextColor(C_DIM, C_INK); tft.drawString(small, 160, 136, 2);
}

// ---- screen 0: scorebug
void drawSideBlock(const SideV& s, bool lead, bool mine, int y) {
  band.fillSprite(C_INK);
  band.fillRect(0, 6, 4, 56, lead ? C_GREEN : C_PANEL2);
  char p[12]; snprintf(p, sizeof p, "%.2f", s.pts);
  int pw = band.textWidth(p, 6);
  band.setTextDatum(TR_DATUM); band.setTextColor(lead ? C_GREEN : C_TEXT);
  band.drawString(p, 314, 12, 6);
  band.setTextDatum(TL_DATUM);
  band.setTextColor(C_TEXT); fitDraw(band, s.name, 12, 10, 4, 314 - pw - 22);
  band.setTextColor(C_DIM); fitDraw(band, s.meta, 12, 40, 2, 314 - pw - 22);
  band.setTextColor(mine ? C_GREEN : C_RED); band.drawString(mine ? "YOU" : "OPP", 12, 56, 1);
  pushBand(y, 68);
}
void drawScorebug() {
  drawHeader(nullptr);
  if (!view.hasMatch) {
    drawMessage(view.nt ? "No matchup" : "Funday Live", view.note[0] ? view.note : view.nt ? "No matchup for this team this week" : "Starting up");
    return;
  }
  const SideV &me = view.me, &op = view.op;
  drawSideBlock(me, me.pts > op.pts, true, 22);
  drawSideBlock(op, op.pts > me.pts, false, 90);

  // win bar + who is still to play
  band.fillSprite(C_INK);
  float tot = me.pts + op.pts;
  int share = tot > 0 ? (int)(me.pts / tot * 300) : 150;
  band.fillRect(10, 4, share, 6, C_GREEN);
  band.fillRect(10 + share, 4, 300 - share, 6, C_RED);
  char a[32], b[32];
  snprintf(a, sizeof a, "%d live  %d to play", me.live, me.pre);
  snprintf(b, sizeof b, "%d live  %d to play", op.live, op.pre);
  band.setTextColor(C_DIM);
  band.setTextDatum(TL_DATUM); band.drawString(a, 10, 14, 2);
  band.setTextDatum(TR_DATUM); band.drawString(b, 310, 14, 2);
  pushBand(158, 30);

  // lineup alert strip: a starter who can't score is the loudest thing on the screen
  band.fillSprite(C_INK);
  int bad = 0, q = 0; const PRow *fb = nullptr, *fq = nullptr;
  for (int i = 0; i < me.n; i++) {
    if (me.rows[i].lvl == 2) { bad++; if (!fb) fb = &me.rows[i]; }
    if (me.rows[i].lvl == 1) { q++; if (!fq) fq = &me.rows[i]; }
  }
  char t[64];
  band.setTextDatum(ML_DATUM);
  if (bad) {
    band.fillRoundRect(6, 2, 308, 26, 4, C_RED);
    snprintf(t, sizeof t, "LINEUP ALERT  %s %s%s", fb->name, fb->tag, bad > 1 ? "  +more" : "");
    band.setTextColor(TFT_WHITE); fitDraw(band, t, 14, 15, 2, 292);
  } else if (q) {
    band.fillRoundRect(6, 2, 308, 26, 4, C_GOLD);
    snprintf(t, sizeof t, "GAME-TIME CALL  %s (Q)%s", fq->name, q > 1 ? "  +more" : "");
    band.setTextColor(C_GOLD_INK); fitDraw(band, t, 14, 15, 2, 292);
  } else {
    band.drawRoundRect(6, 2, 308, 26, 4, RGB(31, 90, 61));
    band.fillCircle(18, 15, 4, C_GREEN);
    band.setTextColor(C_GREEN); band.drawString("Lineup clear", 30, 15, 2);
  }
  pushBand(188, 32);
}

// ---- screens 1 & 2: lineups
void drawLineup(const SideV& s, bool mine) {
  if (!view.hasMatch) { drawHeader(nullptr); drawMessage("No lineup yet", view.note[0] ? view.note : "Appears once the matchup is set"); return; }
  band.fillSprite(C_PANEL);
  band.fillRect(0, 0, 4, 20, mine ? C_GREEN : C_RED);
  char p[12]; snprintf(p, sizeof p, "%.2f", s.pts);
  int pw = band.textWidth(p, 2);
  band.setTextDatum(ML_DATUM); band.setTextColor(C_TEXT);
  fitDraw(band, s.name, 10, 10, 2, 314 - pw - 50);
  band.setTextDatum(MR_DATUM); band.setTextColor(mine ? C_GREEN : C_RED);
  band.drawString(mine ? "YOU" : "OPP", 314 - pw - 10, 10, 2);
  band.setTextColor(C_GOLD); band.drawString(p, 314, 10, 2);
  band.drawFastHLine(0, 19, 320, C_LINE);
  pushBand(0, 20);

  int top = 20, avail = TICK_Y - top, rh = s.n ? std::min(26, avail / s.n) : 24;
  for (int i = 0; i < s.n; i++) {
    const PRow& r = s.rows[i];
    uint16_t bg = r.role == 1 ? T_OFF : r.role == 3 ? T_RZ : r.role == 2 ? T_DEF : (i % 2 ? C_PANEL : C_INK);
    uint16_t edge = r.role == 1 ? C_GOLD : r.role == 3 ? C_RED : r.role == 2 ? C_BLUE : bg;
    int mid = rh / 2;
    band.fillRect(0, 0, 320, rh, bg);
    band.fillRect(0, 0, 4, rh, edge);
    band.setTextDatum(ML_DATUM);
    band.setTextColor(C_FAINT); band.drawString(r.slot, 8, mid, 1);
    int chipW = r.lvl ? band.textWidth(r.tag, 1) + 8 : 0;
    band.setTextColor(r.lvl == 2 ? C_RED : r.gs == 3 ? C_DIM : C_TEXT);
    fitDraw(band, r.name, 34, mid, 2, 150 - (chipW ? chipW + 4 : 0));
    if (chipW) {
      int x = 34 + std::min((int)band.textWidth(r.name, 2), 150 - chipW - 4) + 4;
      band.fillRoundRect(x, mid - 6, chipW, 12, 2, r.lvl == 2 ? C_RED : C_GOLD);
      band.setTextDatum(MC_DATUM); band.setTextColor(r.lvl == 2 ? TFT_WHITE : C_GOLD_INK);
      band.drawString(r.tag, x + chipW / 2, mid, 1);
    }
    band.setTextDatum(TL_DATUM);
    if (rh >= 20) {
      band.setTextColor(r.gs == 2 ? C_TEXT : C_DIM); band.drawString(r.l1, 190, mid - 9, 1);
      band.setTextColor(r.gs == 2 ? C_GREEN : C_FAINT); band.drawString(r.l2, 190, mid + 1, 1);
    } else {
      band.setTextDatum(ML_DATUM); band.setTextColor(r.gs == 2 ? C_TEXT : C_DIM); band.drawString(r.l1, 190, mid, 1);
    }
    char p2[10]; snprintf(p2, sizeof p2, "%.2f", r.pts);
    band.setTextDatum(MR_DATUM); band.setTextColor(r.gs == 1 ? C_FAINT : r.gs == 3 ? C_DIM : C_TEXT);
    band.drawString(p2, 316, mid, 2);
    band.drawFastHLine(0, rh - 1, 320, C_LINE);
    pushBand(top + i * rh, rh);
  }
  int used = top + s.n * rh;
  if (used < TICK_Y) tft.fillRect(0, used, 320, TICK_Y - used, C_INK);
}

// ---- screen 3: every NFL game this week
void drawGames() {
  drawHeader("NFL");
  if (!view.ng) { drawMessage("No games", view.note[0] ? view.note : "Waiting for the NFL schedule"); return; }
  int rows = (view.ng + 1) / 2, rh = std::min(30, (TICK_Y - 22) / std::max(rows, 1));
  for (int r = 0; r < rows; r++) {
    band.fillSprite(C_INK);
    for (int c = 0; c < 2; c++) {
      int i = r * 2 + c; if (i >= view.ng) break;
      const GameV& g = view.g[i];
      int x = c * 160;
      band.fillRect(x, 0, 159, rh - 1, g.st == 'I' ? C_PANEL2 : C_PANEL);
      // edge: green if your starter plays in it, red if only theirs, split if both
      if (g.mine && g.opp) { band.fillRect(x, 0, 3, rh / 2, C_GREEN); band.fillRect(x, rh / 2, 3, rh - 1 - rh / 2, C_RED); }
      else if (g.mine) band.fillRect(x, 0, 3, rh - 1, C_GREEN);
      else if (g.opp) band.fillRect(x, 0, 3, rh - 1, C_RED);
      bool fin = g.st == 'F';
      char sa[6], sh[6]; snprintf(sa, sizeof sa, "%d", g.as); snprintf(sh, sizeof sh, "%d", g.hs);
      uint16_t ca = fin && g.as < g.hs ? C_DIM : C_TEXT, ch = fin && g.hs < g.as ? C_DIM : C_TEXT;
      int y1 = 2;
      band.setTextDatum(TL_DATUM);
      band.setTextColor(ca); band.drawString(g.away, x + 14, y1, 2);
      band.setTextColor(ch); band.drawString(g.home, x + 88, y1, 2);
      band.setTextDatum(TR_DATUM);
      if (g.st != 'P') {
        band.setTextColor(ca); band.drawString(sa, x + 74, y1, 2);
        band.setTextColor(ch); band.drawString(sh, x + 152, y1, 2);
      }
      if (g.poss[0]) {
        int px = strcmp(g.poss, g.away) == 0 ? x + 8 : x + 82;
        band.fillCircle(px, y1 + 8, 3, g.rz ? C_RED : C_GOLD);
      }
      band.setTextDatum(TL_DATUM);
      band.setTextColor(g.rz ? C_RED : g.st == 'I' ? C_GREEN : C_DIM);
      fitDraw(band, g.l2, x + 8, y1 + 17, 1, 148);
    }
    pushBand(22 + r * rh, rh);
  }
  int used = 22 + rows * rh;
  if (used < TICK_Y) tft.fillRect(0, used, 320, TICK_Y - used, C_INK);
}

// ---- team picker (hold the screen for 1.5 s)
void drawPicker() {
  band.fillSprite(C_INK);
  band.setTextDatum(ML_DATUM);
  band.setTextColor(C_GOLD); band.drawString("WHICH TEAM IS YOURS?", 6, 11, 2);
  band.setTextDatum(MR_DATUM); band.setTextColor(C_FAINT); band.drawString("tap one", 314, 11, 2);
  band.drawFastHLine(0, 21, 320, C_LINE);
  pushBand(0, 22);
  if (!view.nt) { drawMessage("Loading teams", view.note); tft.fillRect(0, TICK_Y, 320, TICK_H, C_INK); return; }
  int rows = (view.nt + 1) / 2, rh = std::min(36, (240 - 22) / rows);
  for (int r = 0; r < rows; r++) {
    band.fillSprite(C_INK);
    for (int c = 0; c < 2; c++) {
      int i = r * 2 + c; if (i >= view.nt) break;
      const TeamV& t = view.t[i];
      int x = c * 160;
      bool cur = t.rid == g_rid;
      band.fillRoundRect(x + 2, 1, 156, rh - 2, 4, cur ? RGB(0, 50, 64) : C_PANEL);
      if (cur) band.drawRoundRect(x + 2, 1, 156, rh - 2, 4, C_BLUE);
      band.setTextDatum(TL_DATUM);
      band.setTextColor(C_TEXT); fitDraw(band, t.name, x + 8, 3, 2, 146);
      if (rh >= 28) { band.setTextColor(C_DIM); fitDraw(band, t.owner, x + 8, 20, 1, 146); }
    }
    pushBand(22 + r * rh, rh);
  }
  int used = 22 + rows * rh;
  if (used < 240) tft.fillRect(0, used, 320, 240 - used, C_INK);
}
void pickAt(int x, int y) {
  if (!view.nt) { picker = false; dirty = true; return; }
  int rows = (view.nt + 1) / 2, rh = std::min(36, (240 - 22) / rows);
  int r = (y - 22) / rh, c = x >= 160 ? 1 : 0, i = r * 2 + c;
  if (y < 22 || r >= rows || i >= view.nt) return;
  g_rid = view.t[i].rid;
  prefs.putInt("rid", g_rid);
  Serial.printf("[ui] my team = %d %s\n", g_rid, view.t[i].name);
  picker = false; screen = 0; dirty = true;
  if (netH) xTaskNotifyGive(netH);
}

// ---- ticker
int tickOff = 0, tickW = 0;
uint32_t tickAt = 0;
int drawTickerItems(int x0, bool draw) {   // returns total width
  int x = x0;
  tape.setTextDatum(ML_DATUM);
  for (int i = 0; i < view.ng; i++) {
    const GameV& g = view.g[i];
    char sc[48];
    if (g.st == 'P') snprintf(sc, sizeof sc, "%s @ %s", g.away, g.home);
    else snprintf(sc, sizeof sc, "%s %d  %s %d", g.away, g.as, g.home, g.hs);
    int w = tape.textWidth(sc, 2), w2 = tape.textWidth(g.l2, 1);
    if (draw && x + w + w2 + 30 > 0 && x < 320) {
      if (g.poss[0]) tape.fillCircle(x + 4, 10, 3, g.rz ? C_RED : C_GOLD);
      tape.setTextColor(g.st == 'F' ? C_DIM : C_TEXT); tape.drawString(sc, x + 10, 10, 2);
      tape.setTextColor(g.rz ? C_RED : g.st == 'I' ? C_GREEN : C_FAINT); tape.drawString(g.l2, x + 16 + w, 11, 1);
      tape.drawFastVLine(x + 24 + w + w2, 3, 14, C_LINE);
    }
    x += 30 + w + w2;
  }
  return x - x0;
}
void drawTicker() {
  if (millis() - tickAt < 30) return;
  tickAt = millis();
  tape.fillSprite(RGB(6, 10, 18));
  tape.drawFastHLine(0, 0, 320, C_GOLD);
  if (view.ng) {
    if (!tickW) tickW = drawTickerItems(0, false);
    tickOff = (tickOff + 1) % std::max(tickW, 1);
    for (int x = 46 - tickOff; x < 320; x += tickW) drawTickerItems(x, true);
  }
  tape.fillRect(0, 1, 42, 19, C_GOLD);
  tape.setTextDatum(MC_DATUM); tape.setTextColor(C_GOLD_INK); tape.drawString("LIVE", 21, 11, 2);
  tape.pushSprite(0, TICK_Y);
}

// ---- LED + jingles (the board's, as square waves)
void led(bool r, bool g, bool b) {
  if (!LED_ON) return;
  digitalWrite(LED_R, !r); digitalWrite(LED_G, !g); digitalWrite(LED_B, !b);   // active low
}
const uint16_t J_TD[][2] = {{523,110},{659,110},{784,110},{1047,110},{0,40},{1047,340}};
const uint16_t J_RZ[][2] = {{700,130},{950,130},{700,130},{950,130},{700,130},{950,130}};
const uint16_t J_FG[][2] = {{660,100},{880,220}};
const uint16_t (*jing)[2] = nullptr; int jingN = 0, jingI = 0; uint32_t jingAt = 0;
void playJingle(char kind) {
  if (!SOUND_ON) return;
  if (kind == 'T') { jing = J_TD; jingN = 6; } else if (kind == 'R') { jing = J_RZ; jingN = 6; } else { jing = J_FG; jingN = 2; }
  jingI = 0; jingAt = 0;
}
void stepJingle() {
  if (!jing || millis() < jingAt) return;
  if (jingI >= jingN) { noTone(SPEAKER); jing = nullptr; return; }
  if (jing[jingI][0]) tone(SPEAKER, jing[jingI][0]); else noTone(SPEAKER);
  jingAt = millis() + jing[jingI][1];
  jingI++;
}

// ---- full-screen alert
void drawOverlay(bool blink) {
  const AlertV& a = view.alert;
  bool rz = a.kind == 'R';
  uint16_t bg = rz ? C_RZ_BG : C_TD_BG, edge = rz ? C_RED : C_GOLD;
  tft.fillScreen(bg);
  for (int i = 0; i < 4; i++) tft.drawRect(i, i, 320 - 2 * i, 240 - 2 * i, blink ? edge : bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(rz ? TFT_WHITE : C_GOLD, bg);
  tft.drawString(rz ? "RED ZONE" : "TOUCHDOWN!", 160, 62, 4);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, bg);
  tft.drawString(rz ? a.sub : a.title, 160, 112, 4);
  if (!rz) { tft.setTextColor(C_DIM, bg); tft.drawString(a.sub, 160, 140, 2); }
  char t[64];
  if (a.mine[0]) { snprintf(t, sizeof t, "Yours: %s", a.mine); tft.setTextColor(C_GREEN, bg); tft.drawString(t, 160, 172, 2); }
  if (a.opp[0])  { snprintf(t, sizeof t, "Opp: %s", a.opp);   tft.setTextColor(C_RED, bg);   tft.drawString(t, 160, a.mine[0] ? 196 : 172, 2); }
}
uint32_t blinkAt = 0; bool blinkOn = true;
void onAlert() {
  const AlertV& a = view.alert;
  playJingle(a.kind);
  if (a.kind == 'F') { led(false, a.mine[0], false); overlayUntil = millis() + 1500; return; }  // field goal: LED only
  overlay = true; picker = false;
  overlayUntil = millis() + 6000;
  blinkOn = true; blinkAt = millis();
  drawOverlay(true);
}

// ---- touch
bool wasDown = false, longFired = false;
uint32_t tDown = 0, tPoll = 0;
int tx = 0, ty = 0;
void onTap(int x, int y) {
  if (overlay) { overlay = false; dirty = true; return; }
  if (picker) { pickAt(x, y); return; }
  screen = (screen + 1) % NSCREENS; dirty = true; lastCycle = millis();
}
void pollTouch() {
  if (millis() - tPoll < 20) return;
  tPoll = millis();
  bool down = ts.touched();
  if (down && !wasDown) {
    TS_Point p = ts.getPoint();
    tx = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
    ty = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
    Serial.printf("[touch] raw %d,%d -> %d,%d\n", p.x, p.y, tx, ty);
    tDown = millis(); longFired = false;
  }
  if (down && !longFired && millis() - tDown > 1500 && !picker && !overlay) { longFired = true; picker = true; dirty = true; }
  if (!down && wasDown && !longFired && millis() - tDown > 30) onTap(tx, ty);
  wasDown = down;
}

void render() {
  if (picker) { drawPicker(); return; }
  switch (screen) {
    case 0: drawScorebug(); break;
    case 1: drawLineup(view.me, true); break;
    case 2: drawLineup(view.op, false); break;
    default: drawGames(); break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(INVERT_COLORS);
  tft.fillScreen(C_INK);
  band.setColorDepth(16); band.createSprite(320, BAND_H);
  tape.setColorDepth(16); tape.createSprite(320, TICK_H);
  if (!band.created() || !tape.created()) Serial.println("[ui] sprite allocation failed");

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("funday", false);
  g_rid = prefs.getInt("rid", MY_ROSTER_ID);

  mtx = xSemaphoreCreateMutex();
  memset(&shared, 0, sizeof shared);
  shared.status = 'W'; strcpy(shared.note, "Connecting to Wi-Fi");
  memcpy(&view, &shared, sizeof view);
  view.ver = 0xFFFFFFFF;    // force the first draw

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_INK); tft.drawString("FUNDAY", 160, 100, 4);
  tft.setTextColor(C_GOLD, C_INK); tft.drawString("LIVE", 160, 130, 4);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");

  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, &netH, 0);
  Serial.printf("[boot] heap %u, my team %d\n", ESP.getFreeHeap(), g_rid);
}

void loop() {
  // pick up a new snapshot from the net task
  if (xSemaphoreTake(mtx, 0) == pdTRUE) {
    if (shared.ver != view.ver || shared.alertSeq != view.alertSeq) {
      uint32_t oldSeq = view.alertSeq;
      bool firstSnap = view.ver == 0xFFFFFFFF;
      memcpy(&view, &shared, sizeof view);
      xSemaphoreGive(mtx);
      dirty = true; tickW = 0;
      if (!firstSnap && view.alertSeq != oldSeq) onAlert();
      if (g_rid == 0 && view.nt && !picker) picker = true;    // first boot: ask whose board this is
    } else xSemaphoreGive(mtx);
  }

  pollTouch();
  stepJingle();

  if (overlay) {
    if (millis() > overlayUntil) { overlay = false; dirty = true; led(false, false, false); }
    else if (millis() - blinkAt > 300) {
      blinkAt = millis(); blinkOn = !blinkOn;
      const AlertV& a = view.alert;
      bool rz = a.kind == 'R';
      uint16_t c = blinkOn ? (rz ? C_RED : C_GOLD) : (rz ? C_RZ_BG : C_TD_BG);
      for (int i = 0; i < 4; i++) tft.drawRect(i, i, 320 - 2 * i, 240 - 2 * i, c);
      if (rz) led(blinkOn, false, false); else led(!a.mine[0] && blinkOn, a.mine[0] && blinkOn, false);
    }
    delay(5);
    return;
  }
  if (overlayUntil && millis() > overlayUntil) { overlayUntil = 0; led(false, false, false); }

  if (AUTO_CYCLE_SEC > 0 && !picker && millis() - lastCycle > AUTO_CYCLE_SEC * 1000UL) {
    lastCycle = millis(); screen = (screen + 1) % NSCREENS; dirty = true;
  }
  if (dirty) { render(); dirty = false; }
  if (!picker) drawTicker();
  delay(2);
}
