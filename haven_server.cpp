// ============================================================================
//  haven_server.cpp — авторитарный сервер песочницы (WINDOWS / WINSOCK)
//  Вся симуляция считается на сервере. Клиент шлёт только намерения.
//
//  Сборка (MinGW):  g++ -O2 -std=c++17 -o haven_server.exe haven_server.cpp -lws2_32
//  Запуск:          haven_server.exe [порт]   (по умолчанию 8080)
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <climits>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <queue>
#include <tuple>
#include <random>
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

using namespace std;

// ============================== МИНИ-ПАРСЕР JSON ============================
struct J {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double num = 0;
    string str;
    vector<J> arr;
    vector<pair<string, J>> obj;

    const J* find(const string& k) const {
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
    const J& at(const string& k) const {
        static J nil;
        const J* p = find(k);
        return p ? *p : nil;
    }
    double asNum(double d = 0) const { return t == NUM ? num : d; }
    string asStr(const string& d = "") const { return t == STR ? str : d; }
    bool isObj() const { return t == OBJ; }
    bool isArr() const { return t == ARR; }
};

static void jws(const char*& p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

static bool jparseStr(const char*& p, string& out) {
    if (*p != '"') return false;
    p++; out.clear();
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    char c = p[1 + i]; v <<= 4;
                    if (c >= '0' && c <= '9') v += c - '0';
                    else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
                }
                p += 4;
                if (v < 0x80) out += (char)v;
                else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 63)); }
                else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 63)); out += (char)(0x80 | (v & 63)); }
            } break;
            default: return false;
            }
            p++;
        }
        else out += *p++;
    }
    if (*p != '"') return false;
    p++;
    return true;
}

static bool jparseVal(const char*& p, J& o) {
    jws(p);
    if (!strncmp(p, "null", 4)) { p += 4; return true; }
    if (!strncmp(p, "true", 4)) { o.t = J::BOOL; o.b = true; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { o.t = J::BOOL; o.b = false; p += 5; return true; }
    if (*p == '"') { o.t = J::STR; return jparseStr(p, o.str); }
    if (*p == '[') {
        p++; o.t = J::ARR; jws(p);
        if (*p == ']') { p++; return true; }
        while (true) {
            J v; if (!jparseVal(p, v)) return false;
            o.arr.push_back(move(v)); jws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
    }
    if (*p == '{') {
        p++; o.t = J::OBJ; jws(p);
        if (*p == '}') { p++; return true; }
        while (true) {
            jws(p);
            string k; if (!jparseStr(p, k)) return false;
            jws(p); if (*p != ':') return false; p++;
            J v; if (!jparseVal(p, v)) return false;
            o.obj.push_back({ move(k), move(v) });
            jws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
    }
    char* end = nullptr;
    double d = strtod(p, &end);
    if (end == p) return false;
    o.t = J::NUM; o.num = d; p = end;
    return true;
}

static bool readJsonFile(const string& fn, J& out) {
    FILE* f = fopen(fn.c_str(), "rb");
    if (!f) return false;
    string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) s.append(buf, n);
    fclose(f);
    const char* p = s.c_str();
    return jparseVal(p, out);
}

// ============================== JSON-ЗАПИСЬ =================================
static string jesc(const string& s) {
    string r;
    for (char c : s) {
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); r += b; }
            else r += c;
        }
    }
    return r;
}
static string jn(long long v) { char b[32]; snprintf(b, 32, "%lld", v); return b; }
static string jf(double v) { char b[40]; snprintf(b, 40, "%.2f", v); return b; }

// ============================== SHA1 + BASE64 ===============================
static void sha1(const uint8_t* data, size_t n, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t bitlen = (uint64_t)n * 8;
    vector<uint8_t> msg(data, data + n);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) msg.push_back((bitlen >> (i * 8)) & 0xFF);
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (msg[off + i * 4] << 24) | (msg[off + i * 4 + 1] << 16) | (msg[off + i * 4 + 2] << 8) | msg[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) { uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (v << 1) | (v >> 31); }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hh[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4] = hh[i] >> 24; out[i * 4 + 1] = hh[i] >> 16;
        out[i * 4 + 2] = hh[i] >> 8; out[i * 4 + 3] = hh[i];
    }
}
static string b64enc(const uint8_t* d, size_t n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string s;
    size_t i = 0;
    while (i + 2 < n) {
        uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        s += A[v >> 18]; s += A[(v >> 12) & 63]; s += A[(v >> 6) & 63]; s += A[v & 63];
        i += 3;
    }
    if (i + 1 == n) { uint32_t v = d[i] << 16; s += A[v >> 18]; s += A[(v >> 12) & 63]; s += "=="; }
    else if (i + 2 == n) { uint32_t v = (d[i] << 16) | (d[i + 1] << 8); s += A[v >> 18]; s += A[(v >> 12) & 63]; s += A[(v >> 6) & 63]; s += '='; }
    return s;
}

// ============================== КОНФИГ =======================================
static J ITEMS, RES, RECIPES;
static map<string, J> RECBY;

static string itemName(const string& id) {
    const J& it = ITEMS.at(id);
    return it.isObj() ? it.at("name").asStr(id) : id;
}

static bool loadConfig() {
    if (!readJsonFile("items.json", ITEMS)) {
        fprintf(stderr, "items.json not found\n");
        return false;
    }
    RES = ITEMS.at("resources");
    RECIPES = ITEMS.at("recipes");
    if (!ITEMS.at("items").isObj() || !RES.isObj()) {
        fprintf(stderr, "items.json: missing items/resources sections\n");
        return false;
    }
    ITEMS = ITEMS.at("items");
    for (auto& r : RECIPES.arr) {
        string id = r.at("id").asStr(r.at("result").asStr());
        RECBY[id] = r;
    }
    for (auto& rp : RES.obj) {
        for (auto& y : rp.second.at("yields").arr)
            if (!ITEMS.find(y.at("item").asStr()))
                fprintf(stderr, "[json] resource %s yields unknown item %s\n", rp.first.c_str(), y.at("item").asStr().c_str());
        if (rp.second.find("needTool") && !ITEMS.find(rp.second.at("needTool").asStr()))
            fprintf(stderr, "[json] resource %s requires unknown tool\n", rp.first.c_str());
    }
    for (auto& r : RECIPES.arr) {
        if (!ITEMS.find(r.at("result").asStr()))
            fprintf(stderr, "[json] recipe %s yields unknown item %s\n", r.at("id").asStr().c_str(), r.at("result").asStr().c_str());
        for (auto& ing : r.at("ingredients").arr)
            if (!ITEMS.find(ing.at("item").asStr()))
                fprintf(stderr, "[json] recipe %s: unknown ingredient %s\n", r.at("id").asStr().c_str(), ing.at("item").asStr().c_str());
    }
    return true;
}

// ============================== МИР ==========================================
static const int W = 56, H = 56;
enum { T_GRASS = 0, T_WATER = 1, T_SAND = 2 };
static vector<uint8_t> tiles;
#define IDX(x, y) ((y) * W + (x))

static mt19937 RND((uint32_t)chrono::steady_clock::now().time_since_epoch().count());
static int irnd(int n) { return n > 0 ? (int)(RND() % n) : 0; }

struct Obj {
    int id = 0;
    string kind;
    int x = 0, y = 0;
    int hp = 1, maxhp = 1;
    bool alive = true;
    double respawn = 0;
};
static map<int, Obj> objs;
static map<pair<int, int>, int> occ;
static int nextObj = 1;
static set<int> dirtyObjs;

struct P {
    int id = 0;
    string name;
    bool online = false;
    double x = 0, y = 0;
    deque<pair<int, int>> path;
    int goalObj = -1;
    int actObj = -1;
    bool actActive = false;
    double actLeft = 0, actTotal = 1;
    string craftId;
    double craftLeft = 0, craftTotal = 1;
    double energy = 100;
    map<string, int> inv;
};
static map<int, P> players;
static int nextPid = 1;
static int spawnX = W / 2, spawnY = H / 2;

struct Ev { int pid; string kind; string msg; };
static vector<Ev> pendingEv;
static void ev(int pid, const string& kind, const string& msg) { pendingEv.push_back({ pid, kind, msg }); }

static double gameTime() {
    static const double t0 = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count() - t0;
}

static bool solidKind(const string& k) { return k != "fishspot"; }

static bool walkable(int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return false;
    if (tiles[IDX(x, y)] == T_WATER) return false;
    return occ.find({ x, y }) == occ.end();
}

static void addObj(const string& kind, int x, int y) {
    Obj o;
    o.id = nextObj++;
    o.kind = kind; o.x = x; o.y = y;
    o.maxhp = o.hp = max(1, (int)RES.at(kind).at("hp").asNum(1));
    objs[o.id] = o;
    if (solidKind(kind)) occ[{x, y}] = o.id;
}

static void genWorld() {
    tiles.assign(W * H, T_GRASS);
    int cx[2] = { 13 + irnd(5), W - 15 + irnd(5) };
    int cy[2] = { 13 + irnd(5), H - 16 + irnd(5) };
    for (int l = 0; l < 2; l++) {
        double r = 5.5 + irnd(3);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double d = hypot(x - cx[l], y - cy[l]);
                if (d < r + sin(x * 0.7 + y * 1.3) * 1.3) tiles[IDX(x, y)] = T_WATER;
            }
    }
    vector<uint8_t> t2 = tiles;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (tiles[IDX(x, y)] != T_GRASS) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < W && ny < H && tiles[IDX(nx, ny)] == T_WATER) t2[IDX(x, y)] = T_SAND;
                }
        }
    tiles = t2;

    int placed = 0;
    for (int a = 0; a < 6000 && placed < 118; a++) {
        int x = 2 + irnd(W - 4), y = 2 + irnd(H - 4);
        if (tiles[IDX(x, y)] != T_GRASS) continue;
        if (occ.count({ x, y })) continue;
        bool nearby = false;
        for (auto& kv : occ)
            if (abs(kv.first.first - x) < 2 && abs(kv.first.second - y) < 2) { nearby = true; break; }
        if (nearby) continue;
        int r = irnd(100);
        addObj(r < 55 ? "tree" : (r < 85 ? "rock" : "ironore"), x, y);
        placed++;
    }
    int fish = 0;
    for (int a = 0; a < 4000 && fish < 14; a++) {
        int x = 1 + irnd(W - 2), y = 1 + irnd(H - 2);
        if (tiles[IDX(x, y)] != T_WATER) continue;
        bool shore = false, lone = true;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < W && ny < H && tiles[IDX(nx, ny)] == T_SAND) shore = true;
            }
        for (auto& kv : objs)
            if (kv.second.kind == "fishspot" && abs(kv.second.x - x) + abs(kv.second.y - y) < 5) lone = false;
        if (!shore || !lone) continue;
        addObj("fishspot", x, y);
        fish++;
    }
    bool foundSpawn = false;
    for (int r = 0; r < 20 && !foundSpawn; r++) {
        for (int k = 0; k < 40 && !foundSpawn; k++) {
            int x = W / 2 - r + irnd(2 * r + 1), y = H / 2 - r + irnd(2 * r + 1);
            if (walkable(x, y)) { spawnX = x; spawnY = y; foundSpawn = true; }
        }
    }
    for (auto& kv : objs) {
        Obj& o = kv.second;
        if (abs(o.x - spawnX) <= 2 && abs(o.y - spawnY) <= 2) {
            o.alive = false;
            occ.erase({ o.x, o.y });
            o.respawn = 25;
        }
    }
}

// ============================== ПОИСК ПУТИ ====================================
static deque<pair<int, int>> findPath(int sx, int sy, const set<pair<int, int>>& goals) {
    deque<pair<int, int>> res;
    if (goals.empty()) return res;
    if (goals.count({ sx, sy })) return res;
    auto heur = [&](int x, int y) {
        int m = INT_MAX;
        for (auto& g : goals) { int d = abs(x - g.first) + abs(y - g.second); if (d < m) m = d; }
        return m;
        };
    map<int, int> came, gcost;
    using Node = pair<int, int>;
    priority_queue<Node, vector<Node>, greater<Node>> open;
    int sid = sy * W + sx;
    gcost[sid] = 0;
    open.push({ heur(sx, sy), sid });
    const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
    int goalId = -1, popped = 0;
    while (!open.empty() && popped++ < 9000) {
        auto top = open.top(); open.pop();
        int f = top.first, id = top.second;
        int x = id % W, y = id / W;
        int gg = gcost.count(id) ? gcost[id] : INT_MAX;
        if (f - gg > heur(x, y)) continue;
        if (goals.count({ x, y })) { goalId = id; break; }
        for (int i = 0; i < 4; i++) {
            int nx = x + DX[i], ny = y + DY[i];
            if (!walkable(nx, ny)) continue;
            int nid = ny * W + nx, ng = gg + 1;
            auto it = gcost.find(nid);
            if (it == gcost.end() || ng < it->second) {
                gcost[nid] = ng; came[nid] = id;
                open.push({ ng + heur(nx, ny), nid });
            }
        }
    }
    if (goalId < 0) return res;
    int cur = goalId;
    while (cur != sid) { res.push_front({ cur % W, cur / W }); cur = came[cur]; }
    return res;
}

static set<pair<int, int>> adjacentGoals(const Obj& o) {
    set<pair<int, int>> gs;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = o.x + dx, y = o.y + dy;
            if (walkable(x, y)) gs.insert({ x, y });
        }
    return gs;
}

static double distTo(const P& p, const Obj& o) { return hypot(p.x - o.x, p.y - o.y); }

// ============================== ДЕЙСТВИЯ ======================================
static void give(P& p, const string& item, int n) {
    p.inv[item] += n;
    ev(p.id, "good", "+" + jn(n) + " x " + itemName(item));
}

static bool beginAct(P& p, Obj& o) {
    const J& res = RES.at(o.kind);
    string need = res.at("needTool").asStr();
    if (!need.empty() && p.inv[need] <= 0) {
        ev(p.id, "warn", "Need item: " + itemName(need));
        return false;
    }
    double cost = res.at("energy").asNum(4);
    if (p.energy < cost) {
        ev(p.id, "warn", "You are too tired - eat or rest.");
        return false;
    }
    p.energy -= cost;
    double t = res.at("time").asNum(3);
    string uses = res.at("usesTool").asStr();
    if (!uses.empty() && p.inv[uses] > 0) t *= 0.55;
    p.actActive = true; p.actObj = o.id;
    p.actTotal = t; p.actLeft = t;
    return true;
}

static void killObj(Obj& o) {
    o.alive = false;
    occ.erase({ o.x, o.y });
    double rs = RES.at(o.kind).at("respawn").asNum(60);
    o.respawn = gameTime() + rs * (0.75 + (RND() % 100) / 200.0);
    dirtyObjs.insert(o.id);
}

static void completeAction(P& p, Obj& o) {
    const J& res = RES.at(o.kind);
    p.actActive = false;
    string uses = res.at("usesTool").asStr();
    bool tool = !uses.empty() && p.inv[uses] > 0;
    for (auto& y : res.at("yields").arr) {
        string item = y.at("item").asStr();
        int mn = (int)y.at("min").asNum(1), mx = (int)y.at("max").asNum(mn);
        int n = mn + (mx > mn ? irnd(mx - mn + 1) : 0) + (tool ? 1 : 0);
        give(p, item, n);
    }
    o.hp--;
    dirtyObjs.insert(o.id);
    if (o.hp <= 0) killObj(o);
}

static void cancelBusy(P& p, bool quiet) {
    if (p.craftLeft > 0) { p.craftLeft = 0; p.craftId.clear(); if (!quiet) ev(p.id, "warn", "Crafting interrupted."); }
    p.actActive = false; p.goalObj = -1; p.actObj = -1;
}

static void cmdGoto(P& p, int x, int y) {
    cancelBusy(p, false);
    p.path.clear();
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    set<pair<int, int>> goals;
    if (walkable(x, y)) goals.insert({ x, y });
    else {
        for (int r = 1; r <= 3 && goals.empty(); r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++)
                    if (max(abs(dx), abs(dy)) == r && walkable(x + dx, y + dy)) goals.insert({ x + dx, y + dy });
    }
    if (goals.empty()) { ev(p.id, "warn", "Can't get through."); return; }
    p.path = findPath((int)round(p.x), (int)round(p.y), goals);
}

static void cmdAct(P& p, int oid) {
    cancelBusy(p, true);
    p.path.clear();
    auto it = objs.find(oid);
    if (it == objs.end() || !it->second.alive) return;
    Obj& o = it->second;
    p.goalObj = oid;
    if (distTo(p, o) <= 1.75) {
        if (!beginAct(p, o)) p.goalObj = -1;
    }
    else {
        auto pf = findPath((int)round(p.x), (int)round(p.y), adjacentGoals(o));
        if (pf.empty()) { ev(p.id, "warn", "Can't reach it."); p.goalObj = -1; }
        else p.path = move(pf);
    }
}

static void cmdCraft(P& p, const string& rid) {
    auto it = RECBY.find(rid);
    if (it == RECBY.end()) return;
    if (p.craftLeft > 0 || p.actActive) { ev(p.id, "warn", "You are busy."); return; }
    const J& r = it->second;
    for (auto& ing : r.at("ingredients").arr) {
        string item = ing.at("item").asStr();
        int need = (int)ing.at("count").asNum(1);
        if (p.inv[item] < need) { ev(p.id, "warn", "Not enough: " + itemName(item)); return; }
    }
    for (auto& ing : r.at("ingredients").arr)
        p.inv[ing.at("item").asStr()] -= (int)ing.at("count").asNum(1);
    p.craftId = rid;
    p.craftTotal = p.craftLeft = r.at("time").asNum(2);
    ev(p.id, "info", "You started crafting: " + itemName(r.at("result").asStr()));
}

static void cmdUse(P& p, const string& item) {
    const J* it = ITEMS.find(item);
    if (!it || p.inv[item] <= 0) return;
    const J& food = it->at("food");
    if (food.isObj()) {
        p.inv[item]--;
        double e = food.at("energy").asNum(10);
        p.energy = min(100.0, p.energy + e);
        ev(p.id, "good", "Eaten: " + it->at("name").asStr(item) + " (+" + jn((long long)e) + " energy)");
    }
}

static void cmdStop(P& p) { cancelBusy(p, false); p.path.clear(); }

// ============================== СИМУЛЯЦИЯ =====================================
static void tick(double dt) {
    double t = gameTime();
    for (auto& kv : objs) {
        Obj& o = kv.second;
        if (!o.alive && t >= o.respawn) {
            bool blocked = false;
            for (auto& pkv : players)
                if (pkv.second.online && fabs(pkv.second.x - o.x) < 4 && fabs(pkv.second.y - o.y) < 4) { blocked = true; break; }
            if (!blocked) {
                o.alive = true; o.hp = o.maxhp;
                if (solidKind(o.kind)) occ[{o.x, o.y}] = o.id;
                dirtyObjs.insert(o.id);
            }
            else o.respawn = t + 4;
        }
    }
    for (auto& pkv : players) {
        P& p = pkv.second;
        int pid = pkv.first;
        if (!p.online) continue;
        p.energy = min(100.0, p.energy + dt * (p.actActive ? 0.3 : 1.4));
        if (p.craftLeft > 0) {
            p.craftLeft -= dt;
            if (p.craftLeft <= 0) {
                const J& r = RECBY[p.craftId];
                string out = r.at("result").asStr();
                p.inv[out] += (int)r.at("count").asNum(1);
                ev(pid, "good", "You crafted: " + itemName(out));
                p.craftId.clear();
            }
        }
        if (p.actActive) {
            auto it = objs.find(p.actObj);
            if (it == objs.end() || !it->second.alive || distTo(p, it->second) > 1.75) {
                p.actActive = false; p.goalObj = -1; p.actObj = -1;
            }
            else {
                p.actLeft -= dt;
                if (p.actLeft <= 0) completeAction(p, it->second);
            }
        }
        else if (p.goalObj >= 0) {
            auto it = objs.find(p.goalObj);
            if (it == objs.end() || !it->second.alive) p.goalObj = -1;
            else if (distTo(p, it->second) <= 1.75) {
                if (!beginAct(p, it->second)) p.goalObj = -1;
            }
            else if (p.path.empty()) {
                auto pf = findPath((int)round(p.x), (int)round(p.y), adjacentGoals(it->second));
                if (pf.empty()) { ev(pid, "warn", "Can't reach it."); p.goalObj = -1; }
                else p.path = move(pf);
            }
        }
        if (!p.path.empty() && !p.actActive) {
            pair<int, int> tp = p.path.front();
            double dx = tp.first - p.x, dy = tp.second - p.y;
            double d = hypot(dx, dy), sp = 3.6 * dt;
            if (d <= sp) { p.x = tp.first; p.y = tp.second; p.path.pop_front(); }
            else { p.x += dx / d * sp; p.y += dy / d * sp; }
        }
    }
}

// ============================== СЕТЬ ==========================================
struct Client {
    SOCKET fd = INVALID_SOCKET;
    bool ws = false, dead = false, closeAfter = false;
    string in, out, frag;
    int pid = -1;
};
static vector<Client*> clients;
static SOCKET listenFd = INVALID_SOCKET;

static string invJson(const P& p) {
    string s = "{";
    bool first = true;
    for (auto& kv : p.inv) {
        if (kv.second <= 0) continue;
        if (!first) s += ",";
        first = false;
        s += "\"" + jesc(kv.first) + "\":" + jn(kv.second);
    }
    return s + "}";
}

static string plEntry(const P& p) {
    string s = "{\"id\":" + jn(p.id) + ",\"n\":\"" + jesc(p.name) + "\",\"x\":" + jf(p.x) + ",\"y\":" + jf(p.y);
    if (p.actActive && p.actObj >= 0)
        s += ",\"act\":{\"o\":" + jn(p.actObj) + ",\"p\":" + jf(1.0 - p.actLeft / p.actTotal) + "}";
    if (p.craftLeft > 0) s += ",\"craft\":1";
    return s + "}";
}

static void wsSend(Client& c, const string& msg) {
    string f;
    f.push_back((char)0x81);
    size_t n = msg.size();
    if (n < 126) f.push_back((char)n);
    else if (n < 65536) { f.push_back((char)126); f.push_back((char)(n >> 8)); f.push_back((char)(n & 255)); }
    else { f.push_back((char)127); for (int i = 7; i >= 0; i--) f.push_back((char)((n >> (i * 8)) & 255)); }
    f += msg;
    c.out += f;
}

static void sendInit(Client& c, P& p) {
    string s = "{\"t\":\"init\",\"you\":" + jn(p.id) + ",\"w\":" + jn(W) + ",\"h\":" + jn(H) + ",\"tiles\":\"";
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            s += tiles[IDX(x, y)] == T_GRASS ? 'g' : (tiles[IDX(x, y)] == T_WATER ? 'w' : 's');
    s += "\",\"objs\":[";
    bool first = true;
    for (auto& kv : objs) {
        const Obj& o = kv.second;
        if (!o.alive) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"id\":" + jn(o.id) + ",\"k\":\"" + jesc(o.kind) + "\",\"x\":" + jn(o.x) + ",\"y\":" + jn(o.y) + ",\"hp\":" + jn(o.hp) + "}";
    }
    s += "],\"pl\":[";
    first = true;
    for (auto& kv : players) {
        const P& q = kv.second;
        if (!q.online) continue;
        if (!first) s += ",";
        first = false;
        s += plEntry(q);
    }
    s += "],\"inv\":" + invJson(p) + ",\"e\":" + jn((long long)p.energy);
    s += ",\"ev\":[{\"k\":\"sys\",\"m\":\"Welcome to the quiet backwater. LMB - walk/gather, RMB - stop.\"}]";
    s += "}";
    wsSend(c, s);
}

static void broadcast() {
    bool anyone = false;
    for (size_t i = 0; i < clients.size(); i++) if (clients[i]->ws && clients[i]->pid >= 0) { anyone = true; break; }
    if (!anyone) { dirtyObjs.clear(); pendingEv.clear(); return; }

    string common = "\"pl\":[";
    bool first = true;
    for (auto& kv : players) {
        const P& p = kv.second;
        if (!p.online) continue;
        if (!first) common += ",";
        first = false;
        common += plEntry(p);
    }
    common += "],\"ob\":[";
    first = true;
    for (set<int>::iterator it = dirtyObjs.begin(); it != dirtyObjs.end(); ++it) {
        int oid = *it;
        auto oit = objs.find(oid);
        if (oit == objs.end()) continue;
        if (!first) common += ",";
        first = false;
        if (oit->second.alive) common += "{\"id\":" + jn(oid) + ",\"hp\":" + jn(oit->second.hp) + "}";
        else common += "{\"id\":" + jn(oid) + ",\"del\":1}";
    }
    common += "]";
    dirtyObjs.clear();

    map<int, string> evByPid;
    string evAll;
    for (size_t i = 0; i < pendingEv.size(); i++) {
        const Ev& e = pendingEv[i];
        string one = "{\"k\":\"" + jesc(e.kind) + "\",\"m\":\"" + jesc(e.msg) + "\"}";
        if (e.pid == -1) { if (!evAll.empty()) evAll += ","; evAll += one; }
        else { if (!evByPid[e.pid].empty()) evByPid[e.pid] += ","; evByPid[e.pid] += one; }
    }
    pendingEv.clear();

    for (size_t i = 0; i < clients.size(); i++) {
        Client* c = clients[i];
        if (!c->ws || c->pid < 0) continue;
        P& p = players[c->pid];
        string s = "{\"t\":\"st\"," + common;
        s += ",\"you\":{\"e\":" + jn((long long)p.energy) + ",\"inv\":" + invJson(p);
        if (p.craftLeft > 0)
            s += ",\"craft\":{\"id\":\"" + jesc(p.craftId) + "\",\"p\":" + jf(1.0 - p.craftLeft / p.craftTotal) + "}";
        s += "}";
        s += ",\"ev\":[" + evAll;
        auto eit = evByPid.find(c->pid);
        if (eit != evByPid.end()) s += string(evAll.empty() ? "" : ",") + eit->second;
        s += "]}";
        wsSend(*c, s);
    }
}

static string trimCopy(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}
static bool iequals(const string& a, const string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (tolower(a[i]) != tolower(b[i])) return false;
    return true;
}

static void handleMsg(Client& c, const string& s) {
    J m;
    const char* p = s.c_str();
    if (!jparseVal(p, m)) return;
    string t = m.at("t").asStr();

    if (t == "join") {
        if (c.pid >= 0) return;
        string name = trimCopy(m.at("name").asStr());
        if (name.empty()) name = "Wanderer";
        if (name.size() > 24) name = name.substr(0, 24);
        int pid = -1;
        for (auto& kv : players)
            if (iequals(kv.second.name, name) && !kv.second.online) { pid = kv.first; break; }
        if (pid < 0) {
            pid = nextPid++;
            P np;
            np.id = pid; np.name = name;
            np.x = spawnX + irnd(3) - 1; np.y = spawnY + irnd(3) - 1;
            players[pid] = np;
        }
        players[pid].online = true;
        c.pid = pid;
        ev(-1, "sys", players[pid].name + " enters the world.");
        sendInit(c, players[pid]);
        printf("[+] %s joined\n", name.c_str());
        return;
    }
    if (c.pid < 0 || players.find(c.pid) == players.end()) return;
    P& pl = players[c.pid];
    if (t == "goto") cmdGoto(pl, (int)m.at("x").asNum(-1), (int)m.at("y").asNum(-1));
    else if (t == "act") cmdAct(pl, (int)m.at("id").asNum(-1));
    else if (t == "craft") cmdCraft(pl, m.at("id").asStr());
    else if (t == "use") cmdUse(pl, m.at("id").asStr());
    else if (t == "stop") cmdStop(pl);
}

static string lowerCopy(const string& s) { string r = s; for (size_t i = 0; i < r.size(); i++) r[i] = (char)tolower(r[i]); return r; }
static string headerVal(const string& head, const string& name) {
    string lh = lowerCopy(head);
    size_t pos = lh.find(name + ":");
    if (pos == string::npos) return "";
    pos += name.size() + 1;
    size_t e = head.find("\r\n", pos);
    string v = head.substr(pos, e - pos);
    return trimCopy(v);
}

static string mimeOf(const string& p) {
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".html") return "text/html; charset=utf-8";
    if (p.size() >= 3 && p.substr(p.size() - 3) == ".js") return "text/javascript; charset=utf-8";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".css") return "text/css; charset=utf-8";
    if (p.size() >= 5 && p.substr(p.size() - 5) == ".json") return "application/json; charset=utf-8";
    if (p.size() >= 4 && p.substr(p.size() - 4) == ".png") return "image/png";
    return "application/octet-stream";
}

static void serveFile(Client& c, const string& rawPath) {
    string path = rawPath;
    size_t q = path.find('?');
    if (q != string::npos) path = path.substr(0, q);
    if (path == "/") path = "/index.html";
    if (path.find("..") != string::npos) { c.out += "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"; c.closeAfter = true; return; }
    string fp = (path == "/items.json") ? "items.json" : "web" + path;
    FILE* f = fopen(fp.c_str(), "rb");
    if (!f) {
        string body = "404: " + path;
        c.out += "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + jn(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        c.closeAfter = true;
        return;
    }
    string body;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, 8192, f)) > 0) body.append(buf, n);
    fclose(f);
    c.out += "HTTP/1.1 200 OK\r\nContent-Type: " + mimeOf(fp) + "\r\nContent-Length: " + jn(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    c.closeAfter = true;
}

static void processHttp(Client& c) {
    size_t he = c.in.find("\r\n\r\n");
    if (he == string::npos) return;
    string head = c.in.substr(0, he);
    string rest = c.in.substr(he + 4);
    size_t l1 = head.find("\r\n");
    string line = head.substr(0, l1);
    size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
    string method = line.substr(0, s1);
    string path = (s1 == string::npos || s2 == string::npos) ? "/" : line.substr(s1 + 1, s2 - s1 - 1);

    if (lowerCopy(head).find("upgrade: websocket") != string::npos) {
        string key = headerVal(head, "sec-websocket-key");
        string accIn = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t dig[20];
        sha1((const uint8_t*)accIn.data(), accIn.size(), dig);
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + b64enc(dig, 20) + "\r\n\r\n";
        c.ws = true;
        c.in = rest;
        return;
    }
    c.in = rest;
    if (method == "GET") serveFile(c, path);
    else { c.out += "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n"; c.closeAfter = true; }
}

static void processWs(Client& c) {
    while (true) {
        if (c.in.size() < 2) break;
        uint8_t b0 = c.in[0], b1 = c.in[1];
        bool fin = b0 & 0x80;
        int op = b0 & 0x0F;
        uint64_t len = b1 & 0x7F;
        size_t idx = 2;
        if (len == 126) { if (c.in.size() < 4) break; len = ((uint8_t)c.in[2] << 8) | (uint8_t)c.in[3]; idx = 4; }
        else if (len == 127) {
            if (c.in.size() < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)c.in[2 + i];
            idx = 10;
        }
        bool masked = b1 & 0x80;
        uint8_t mk[4] = { 0, 0, 0, 0 };
        if (masked) { if (c.in.size() < idx + 4) break; memcpy(mk, &c.in[idx], 4); idx += 4; }
        if (c.in.size() < idx + len) break;
        string payload = c.in.substr(idx, len);
        if (masked) for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mk[i % 4];
        c.in.erase(0, idx + len);
        if (op == 8) { string cl; cl.push_back((char)0x88); cl.push_back(0); c.out += cl; c.closeAfter = true; return; }
        else if (op == 9) { string pong; pong.push_back((char)0x8A); pong.push_back(0); c.out += pong; }
        else if (op == 1 || op == 2) {
            if (fin) { string full = c.frag + payload; c.frag.clear(); handleMsg(c, full); }
            else c.frag += payload;
        }
        else if (op == 0) {
            c.frag += payload;
            if (fin) { handleMsg(c, c.frag); c.frag.clear(); }
        }
        if (c.in.size() > (1 << 20)) { c.dead = true; return; }
    }
}

static void flush(Client& c) {
    while (!c.out.empty()) {
        int n = send(c.fd, c.out.data(), (int)c.out.size(), 0);
        if (n > 0) c.out.erase(0, n);
        else if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            c.dead = true; return;
        }
    }
    if (c.closeAfter && c.out.empty()) c.dead = true;
}

// ============================== MAIN ==========================================
int main(int argc, char** argv) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int port = argc > 1 ? atoi(argv[1]) : 8080;

    if (!loadConfig()) { WSACleanup(); return 1; }
    genWorld();

    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        closesocket(listenFd);
        WSACleanup();
        return 1;
    }
    if (listen(listenFd, 64) == SOCKET_ERROR) {
        fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
        closesocket(listenFd);
        WSACleanup();
        return 1;
    }
    u_long nonblocking = 1;
    ioctlsocket(listenFd, FIONBIO, &nonblocking);

    printf("World ready: %dx%d, objects: %d. Server: http://localhost:%d\n", W, H, (int)objs.size(), port);

    double nextTick = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
    int tickN = 0;

    while (true) {
        double nw = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
        int timeout = (int)max(1.0, (nextTick - nw) * 1000.0);

        vector<WSAPOLLFD> pf;
        WSAPOLLFD lfd; lfd.fd = listenFd; lfd.events = POLLIN; lfd.revents = 0;
        pf.push_back(lfd);
        for (size_t i = 0; i < clients.size(); i++) {
            WSAPOLLFD cfd;
            cfd.fd = clients[i]->fd;
            cfd.events = POLLIN | (clients[i]->out.empty() ? 0 : POLLOUT);
            cfd.revents = 0;
            pf.push_back(cfd);
        }
        int rc = WSAPoll(pf.data(), (ULONG)pf.size(), timeout);
        if (rc == SOCKET_ERROR) {
            this_thread::sleep_for(chrono::milliseconds(1));
            continue;
        }

        if (pf[0].revents & POLLIN) {
            while (true) {
                SOCKET fd = accept(listenFd, nullptr, nullptr);
                if (fd == INVALID_SOCKET) break;
                u_long nb = 1;
                ioctlsocket(fd, FIONBIO, &nb);
                Client* c = new Client();
                c->fd = fd;
                clients.push_back(c);
            }
        }
        for (size_t i = 0; i < clients.size(); i++) {
            Client* c = clients[i];
            if (c->dead) continue;
            SHORT re = pf[i + 1].revents;
            if (re & (POLLERR | POLLHUP)) { if (c->out.empty()) c->dead = true; }
            if (!c->dead && (re & POLLIN)) {
                char buf[8192];
                int n = recv(c->fd, buf, (int)sizeof(buf), 0);
                if (n > 0) c->in.append(buf, n);
                else if (n == 0) c->dead = true;
                else {
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK) c->dead = true;
                }
            }
            if (!c->dead && !c->ws && c->in.find("\r\n\r\n") != string::npos) processHttp(*c);
            if (!c->dead && c->ws && !c->in.empty()) processWs(*c);
            if (!c->dead && (re & POLLOUT)) flush(*c);
            if (!c->dead) flush(*c);
        }
        for (size_t i = 0; i < clients.size();) {
            if (clients[i]->dead) {
                Client* c = clients[i];
                if (c->pid >= 0 && players.count(c->pid)) players[c->pid].online = false;
                closesocket(c->fd);
                delete c;
                clients.erase(clients.begin() + i);
            }
            else i++;
        }
        double nw2 = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
        while (nw2 >= nextTick) {
            tick(0.05);
            nextTick += 0.05;
            tickN++;
            if (tickN % 2 == 0) broadcast();
            nw2 = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    WSACleanup();
    return 0;
}