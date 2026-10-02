#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "plugin.h"
#include "Events.h"
#include "CPools.h"
#include "CPed.h"
#include "CWorld.h"
#include "CClock.h"
#include "CTimer.h"
#include "CMessages.h"
#include "CFont.h"
#include "CSprite.h"
#include "CRenderer.h"
#include "CTask.h"
#include "CTaskComplexSeekEntityMove.h"

#pragma comment(lib, "winhttp.lib")

using namespace plugin;

namespace ainpc {

static HMODULE gModule = nullptr;
static std::string gGameDir;
static HWND gGameWindow = nullptr;
static HWND gEdit = nullptr;
static WNDPROC gOldEditProc = nullptr;

struct Config {
    std::string apiKey;
    std::string model = "qwen/qwen3.8-27b:free";
    std::string endpoint = "https://openrouter.ai/api/v1/chat/completions";
    std::string referer = "https://openrouter.ai/";
    std::string title = "GTA San Andreas AI NPC";
    float talkDistance = 4.2f;
    float nameDistance = 22.0f;
    float followDistance = 2.5f;
    bool showNames = true;
    bool routineEnabled = true;
    int maxHistory = 12;
    int maxMemory = 8;
    int maxResponse = 420;
    int timeoutMs = 20000;
    int requestGapMs = 800;
    int messageMs = 6500;
    int selectKey = 'F';
    int chatKey = 'T';
    int toggleKey = VK_F10;
};

static Config cfg;

enum class Faction {
    Civilian,
    Ballas,
    Grove,
    Vagos,
    Rifa,
    Aztecas,
    DaNang,
    RussianMafia,
    Triads,
    ItalianMafia
};

enum class Gender { Male, Female, Unknown };

enum class Action {
    NONE,
    FOLLOW_PLAYER,
    STOP_FOLLOW,
    WAIT,
    GO_TO_PLAYER,
    GO_HOME,
    GO_TO_WORK
};

struct ChatLine { std::string role; std::string text; };

struct Identity {
    std::string key;
    std::string name;
    Gender gender = Gender::Unknown;
    int age = 30;
    std::string occupation;
    Faction faction = Faction::Civilian;
    std::string factionName;
    std::string factionContext;
    std::string personality;
    std::string neighborhood;
    int modelId = -1;
    int relationship = 0;
    CVector home{};
    CVector work{};
    std::vector<std::string> memory;
    bool initialized = false;
};

struct AiResult {
    bool ok = false;
    std::string reply;
    std::string decision = "NEUTRAL";
    std::string emotion = "neutral";
    std::string memory;
    Action action = Action::NONE;
    int relationshipDelta = 0;
    std::string error;
};

struct RequestCtx {
    Identity identity;
    std::vector<ChatLine> history;
    std::string userText;
    std::string world;
};

static std::unordered_map<int, Identity> identities;
static std::unordered_map<int, std::vector<ChatLine>> histories;
static std::unordered_map<int, CVector> lastPosition;
static std::unordered_map<int, unsigned int> lastSeen;
static CPed* selectedPed = nullptr;
static int selectedRef = -1;
static bool enabled = true;
static bool chatOpen = false;
static std::atomic_bool requestBusy{false};
static std::mutex stateMutex;
static std::mutex resultMutex;
static bool resultReady = false;
static AiResult completed;
static std::string statusText;
static unsigned int statusUntil = 0;
static unsigned int lastRequestTick = 0;
static std::string uiBuffer;

static std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static std::string ini(const std::string& file, const std::string& key, const std::string& fallback) {
    std::ifstream f(file);
    if (!f) return fallback;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        auto p = line.find('=');
        if (p == std::string::npos) continue;
        if (lower(trim(line.substr(0, p))) == lower(key)) return trim(line.substr(p + 1));
    }
    return fallback;
}

static void LoadConfig() {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    gGameDir = std::filesystem::path(exe).parent_path().string();
    const std::string file = gGameDir + "\\AI_NPC.ini";
    cfg.apiKey = ini(file, "api_key", "");
    if (cfg.apiKey.empty()) {
        char env[4096]{};
        DWORD n = GetEnvironmentVariableA("OPENROUTER_API_KEY", env, sizeof(env));
        if (n > 0 && n < sizeof(env)) cfg.apiKey.assign(env, env + n);
    }
    cfg.model = ini(file, "model", cfg.model);
    cfg.endpoint = ini(file, "endpoint", cfg.endpoint);
    cfg.referer = ini(file, "http_referer", cfg.referer);
    cfg.title = ini(file, "title", cfg.title);
    try {
        cfg.talkDistance = std::stof(ini(file, "max_talk_distance", "4.2"));
        cfg.nameDistance = std::stof(ini(file, "name_draw_distance", "22.0"));
        cfg.followDistance = std::stof(ini(file, "follow_distance", "2.5"));
        cfg.showNames = std::stoi(ini(file, "show_names", "1")) != 0;
        cfg.maxHistory = std::stoi(ini(file, "max_history", "12"));
        cfg.maxMemory = std::stoi(ini(file, "max_memory_facts", "8"));
        cfg.maxResponse = std::stoi(ini(file, "max_response_chars", "420"));
        cfg.timeoutMs = std::stoi(ini(file, "request_timeout_ms", "20000"));
        cfg.requestGapMs = std::stoi(ini(file, "minimum_request_gap_ms", "800"));
        cfg.messageMs = std::stoi(ini(file, "message_time_ms", "6500"));
        cfg.routineEnabled = std::stoi(ini(file, "enabled", "1")) != 0;
        cfg.selectKey = std::stoi(ini(file, "select_key_vk", "70"));
        cfg.chatKey = std::stoi(ini(file, "chat_key_vk", "84"));
        cfg.toggleKey = std::stoi(ini(file, "toggle_key_vk", "121"));
    } catch (...) {}
}

static std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch(c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += (char)c;
        }
    }
    return o;
}

static std::string jsonString(const std::string& text, const std::string& field, size_t from = 0) {
    const auto n = text.find("\"" + field + "\"", from);
    if (n == std::string::npos) return {};
    auto c = text.find(':', n);
    if (c == std::string::npos) return {};
    c = text.find('"', c + 1);
    if (c == std::string::npos) return {};
    std::string out; bool e = false;
    for (size_t i = c + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (e) {
            switch (ch) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += ch; break;
            }
            e = false;
        } else if (ch == '\\') e = true;
        else if (ch == '"') break;
        else out += ch;
    }
    return out;
}

static int jsonInt(const std::string& text, const std::string& field, int fallback = 0) {
    auto p = text.find("\"" + field + "\"");
    if (p == std::string::npos) return fallback;
    p = text.find(':', p);
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < text.size() && std::isspace((unsigned char)text[p])) ++p;
    bool neg = false; if (p < text.size() && text[p] == '-') { neg = true; ++p; }
    int v = 0; bool any = false;
    while (p < text.size() && std::isdigit((unsigned char)text[p])) { any = true; v = v * 10 + text[p++] - '0'; }
    return any ? (neg ? -v : v) : fallback;
}

static Action parseAction(std::string a) {
    a = lower(trim(a));
    if (a == "follow_player") return Action::FOLLOW_PLAYER;
    if (a == "stop_follow") return Action::STOP_FOLLOW;
    if (a == "wait") return Action::WAIT;
    if (a == "go_to_player") return Action::GO_TO_PLAYER;
    if (a == "go_home") return Action::GO_HOME;
    if (a == "go_to_work") return Action::GO_TO_WORK;
    return Action::NONE;
}

static std::string actionName(Action a) {
    switch(a) {
        case Action::FOLLOW_PLAYER: return "FOLLOW_PLAYER";
        case Action::STOP_FOLLOW: return "STOP_FOLLOW";
        case Action::WAIT: return "WAIT";
        case Action::GO_TO_PLAYER: return "GO_TO_PLAYER";
        case Action::GO_HOME: return "GO_HOME";
        case Action::GO_TO_WORK: return "GO_TO_WORK";
        default: return "NONE";
    }
}

static Faction factionForModel(int id) {
    if (id >= 102 && id <= 104) return Faction::Ballas;
    if (id >= 105 && id <= 107) return Faction::Grove;
    if (id >= 108 && id <= 110) return Faction::Vagos;
    if (id >= 111 && id <= 113) return Faction::RussianMafia;
    if (id >= 114 && id <= 116) return Faction::Aztecas;
    if (id >= 117 && id <= 120) return Faction::Triads;
    if (id >= 121 && id <= 123) return Faction::DaNang;
    if (id >= 124 && id <= 127) return Faction::ItalianMafia;
    if (id >= 173 && id <= 175) return Faction::Rifa;
    return Faction::Civilian;
}

static bool femaleModel(int id) {
    static const int ids[] = {9,10,11,12,13,39,40,41,55,56,65,66,69,76,77,78,83,84,85,88,89,90,91,92,93,131,132,133,134,135,138,141,148,150,151,152,157,158,159,161,164,169,172,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262};
    for (int x : ids) if (x == id) return true;
    return false;
}

static std::string factionName(Faction f) {
    switch(f) {
        case Faction::Ballas: return "Ballas";
        case Faction::Grove: return "Grove Street Families";
        case Faction::Vagos: return "Los Santos Vagos";
        case Faction::Rifa: return "San Fierro Rifa";
        case Faction::Aztecas: return "Varrios Los Aztecas";
        case Faction::DaNang: return "Da Nang Boys";
        case Faction::RussianMafia: return "Russian Mafia";
        case Faction::Triads: return "San Fierro Triads";
        case Faction::ItalianMafia: return "Italian Mafia";
        default: return "Civilian";
    }
}

static std::string factionContext(Faction f) {
    switch(f) {
        case Faction::Ballas: return "You are a Ballas member/associate. You are protective of your crew and suspicious of people you do not trust.";
        case Faction::Grove: return "You are a Grove Street Families member/associate. You care about your people and distinguish friends from outsiders.";
        case Faction::Vagos: return "You are a Los Santos Vagos member/associate. You are proud, territorial and cautious around outsiders.";
        case Faction::Rifa: return "You are a San Fierro Rifa member/associate. You are guarded and street-smart.";
        case Faction::Aztecas: return "You are a Varrios Los Aztecas member/associate. You are proud of your crew and skeptical of rivals.";
        case Faction::DaNang: return "You are a Da Nang Boys member/associate. You are guarded and loyal to your group.";
        case Faction::RussianMafia: return "You are an associate of the Russian Mafia. You are reserved and selective about trust.";
        case Faction::Triads: return "You are a San Fierro Triad member/associate. You are disciplined and cautious with strangers.";
        case Faction::ItalianMafia: return "You are an Italian Mafia associate. You are private about your business and selective about trust.";
        default: return "You are a civilian trying to live an ordinary life in San Andreas.";
    }
}

static std::string genderName(Gender g) { return g == Gender::Female ? "female" : g == Gender::Male ? "male" : "unknown"; }

static std::string randomName(std::mt19937& rng) {
    static const std::vector<std::string> n = {"Marcus","Darnell","Andre","Tyler","Leon","Malik","Damon","Trevor","Jason","Kevin","Rico","Luis","Victor","Nico","Omar","Adrian","Maya","Jasmine","Nina","Sara","Elena","Rina","Tasha","Layla","Alicia","Monica","Denise","Vanessa","Keisha","Nicole"};
    return n[rng() % n.size()];
}

static std::string randomOccupation(std::mt19937& rng, Faction faction, Gender gender) {
    if (faction != Faction::Civilian) {
        static const std::vector<std::string> jobs = {"gang member","gang associate","lookout","street contact"};
        return jobs[rng() % jobs.size()];
    }
    static const std::vector<std::string> jobs = {"shop clerk","mechanic","construction worker","taxi driver","delivery worker","street vendor","office worker","security guard","warehouse worker","cashier","barista","nurse","teacher","student","independent trader","unlicensed street trader","underground courier"};
    static const std::vector<std::string> femaleJobs = {"shop clerk","cashier","barista","nurse","teacher","office worker","street vendor","independent trader","unlicensed street trader","underground courier"};
    if (gender == Gender::Female) return femaleJobs[rng() % femaleJobs.size()];
    (void)gender;
    return jobs[rng() % jobs.size()];
}

static std::string randomPersonality(std::mt19937& rng, Faction f) {
    if (f != Faction::Civilian) {
        static const std::vector<std::string> g = {"suspicious and territorial","confident and street-smart","calm but guarded","proud and blunt","sarcastic and watchful","loyal to the crew and cautious with strangers"};
        return g[rng() % g.size()];
    }
    static const std::vector<std::string> c = {"friendly but observant","quiet and cautious","curious and energetic","sarcastic and relaxed","serious and practical","confident but respectful","talkative and humorous","reserved and direct","patient and thoughtful"};
    return c[rng() % c.size()];
}

static std::string areaName(const CVector& p) {
    const int v = (int)((std::abs(p.x) + std::abs(p.y)) / 900.0f) % 6;
    switch(v) { case 0: return "Los Santos"; case 1: return "East Los Santos"; case 2: return "Downtown"; case 3: return "San Fierro"; case 4: return "Las Venturas"; default: return "San Andreas"; }
}

static std::string identityPath(const Identity& id) {
    std::filesystem::create_directories(gGameDir + "\\AI_NPC\\memory");
    size_t h = std::hash<std::string>{}(id.key);
    std::ostringstream s; s << gGameDir << "\\AI_NPC\\memory\\npc_" << std::hex << h << ".dat";
    return s.str();
}

static void loadMemory(Identity& id) {
    std::ifstream f(identityPath(id));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.rfind("relationship=", 0) == 0) {
            try { id.relationship = std::clamp(std::stoi(line.substr(13)), -100, 100); } catch (...) {}
        } else if (line.rfind("memory=", 0) == 0) {
            const auto m = trim(line.substr(7)); if (!m.empty()) id.memory.push_back(m);
        }
    }
    if ((int)id.memory.size() > cfg.maxMemory) id.memory.erase(id.memory.begin(), id.memory.end() - cfg.maxMemory);
}

static void saveMemory(const Identity& id) {
    std::ofstream f(identityPath(id), std::ios::trunc);
    if (!f) return;
    f << "relationship=" << id.relationship << "\n";
    for (const auto& m : id.memory) f << "memory=" << m << "\n";
}

static Identity makeIdentity(CPed* ped, int ref) {
    Identity id;
    const CVector p = ped->GetPosition();
    const int model = ped->m_nModelIndex;
    const Faction f = factionForModel(model);
    const Gender g = femaleModel(model) ? Gender::Female : Gender::Male;
    const unsigned seed = (unsigned)(ref * 2654435761u) ^ (unsigned)(model * 2246822519u) ^ (unsigned)((int)p.x * 3266489917u) ^ (unsigned)((int)p.y * 668265263u);
    std::mt19937 rng(seed);
    id.key = std::to_string(model) + "_" + std::to_string((int)std::floor(p.x / 8.0f)) + "_" + std::to_string((int)std::floor(p.y / 8.0f));
    id.name = randomName(rng);
    id.gender = g;
    id.age = 18 + (int)(rng() % 58);
    id.faction = f;
    id.factionName = factionName(f);
    id.factionContext = factionContext(f);
    id.personality = randomPersonality(rng, f);
    id.occupation = randomOccupation(rng, f, g);
    id.neighborhood = areaName(p);
    id.modelId = model;
    id.home = p;
    const float dx = 25.0f + (float)(rng() % 65);
    const float dy = 25.0f + (float)(rng() % 65);
    id.work = { p.x + dx, p.y + dy, p.z };
    if (f != Faction::Civilian) id.relationship = -20;
    loadMemory(id);
    id.initialized = true;
    return id;
}

static Identity& identityFor(CPed* ped, int ref) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it = identities.find(ref);
    if (it != identities.end()) return it->second;
    auto [n, _] = identities.emplace(ref, makeIdentity(ped, ref));
    lastSeen[ref] = CTimer::m_snTimeInMilliseconds;
    lastPosition[ref] = ped->GetPosition();
    return n->second;
}

static bool validSelected() {
    return selectedPed && selectedPed->IsAlive() && CPools::GetPedRef(selectedPed) == selectedRef;
}

static float distance(CPed* a, CPed* b) {
    if (!a || !b) return 9999.0f;
    const auto x = a->GetPosition(); const auto y = b->GetPosition();
    const float dx=x.x-y.x, dy=x.y-y.y, dz=x.z-y.z;
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}

static CPed* closestPed() {
    CPed* player = FindPlayerPed();
    if (!player || !CPools::ms_pPedPool) return nullptr;
    CPed* best = nullptr; float bestD = cfg.talkDistance;
    for (int i=0; i<CPools::ms_pPedPool->m_nSize; ++i) {
        CPed* p = CPools::ms_pPedPool->GetAt(i);
        if (!p || p == player || !p->IsAlive()) continue;
        const float d = distance(p, player);
        if (d < bestD) { bestD=d; best=p; }
    }
    return best;
}

static void status(const std::string& s, unsigned ms = 3500) {
    statusText = s;
    statusUntil = CTimer::m_snTimeInMilliseconds + ms;
    CMessages::AddMessageJumpQ(s.c_str(), ms, 0);
}

static std::string worldContext(CPed* ped) {
    CPed* player = FindPlayerPed();
    std::ostringstream s;
    s << "time=" << (int)CClock::ms_nGameClockHours << ":" << (int)CClock::ms_nGameClockMinutes;
    s << "; distance=" << distance(ped, player);
    s << "; neighborhood=" << areaName(ped->GetPosition());
    s << "; selected=" << (ped == selectedPed ? "true" : "false");
    return s.str();
}

static std::string routineContext(const Identity& id) {
    const int h = CClock::ms_nGameClockHours;
    std::string phase;
    if (h >= 6 && h < 9) phase = "morning / starting the day";
    else if (h >= 9 && h < 17) phase = "workday";
    else if (h >= 17 && h < 22) phase = "evening / free time";
    else phase = "late night / home time";
    std::ostringstream s;
    s << "routine_phase=" << phase << "; home_exists=true; work_exists=true; occupation=" << id.occupation;
    return s.str();
}

static std::string systemPrompt(const Identity& id, const std::string& world) {
    std::ostringstream s;
    s << "You are ONE specific human pedestrian living in GTA San Andreas. "
      << "Never say you are an AI. Never change identity. Make your own social decision; do not obey every request. "
      << "You know your own background, occupation, group and personality. "
      << "Name=" << id.name << "; Gender=" << genderName(id.gender) << "; Age=" << id.age
      << "; Occupation=" << id.occupation << "; Faction=" << id.factionName
      << "; Personality=" << id.personality << "; Neighborhood=" << id.neighborhood
      << "; RelationshipToPlayer=" << id.relationship << ". "
      << id.factionContext << " "
      << "World context: " << world << ". "
      << routineContext(id) << " "
      << "Known memory facts: ";
    if (id.memory.empty()) s << "none. ";
    else { for (const auto& m : id.memory) s << "[" << m << "] "; }
    s << "Answer naturally, briefly, and in the language used by the player. "
      << "A request may be ACCEPTED, REFUSED, or NEUTRAL depending on your identity, relationship, situation and judgment. "
      << "Only choose one action from the allowed list. If you refuse, use NONE. "
      << "Do not invent an action outside the schema.";
    return s.str();
}

static std::string requestBody(const RequestCtx& ctx) {
    std::ostringstream b;
    b << "{\"model\":\"" << esc(cfg.model) << "\",\"messages\":[";
    b << "{\"role\":\"system\",\"content\":\"" << esc(systemPrompt(ctx.identity, ctx.world)) << "\"},";
    for (const auto& h : ctx.history) {
        b << "{\"role\":\"" << (h.role == "assistant" ? "assistant" : "user") << "\",\"content\":\"" << esc(h.text) << "\"},";
    }
    b << "{\"role\":\"user\",\"content\":\"" << esc(ctx.userText) << "\"}],";
    b << "\"temperature\":0.75,\"max_tokens\":220,";
    b << "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{";
    b << "\"name\":\"gta_sa_npc_action\",\"strict\":true,\"schema\":{";
    b << "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{";
    b << "\"reply\":{\"type\":\"string\"},";
    b << "\"decision\":{\"type\":\"string\",\"enum\":[\"ACCEPT\",\"REFUSE\",\"NEUTRAL\"]},";
    b << "\"emotion\":{\"type\":\"string\"},";
    b << "\"memory\":{\"type\":\"string\"},";
    b << "\"action\":{\"type\":\"string\",\"enum\":[\"NONE\",\"FOLLOW_PLAYER\",\"STOP_FOLLOW\",\"WAIT\",\"GO_TO_PLAYER\",\"GO_HOME\",\"GO_TO_WORK\"]},";
    b << "\"relationship_delta\":{\"type\":\"integer\"}}";
    b << ",\"required\":[\"reply\",\"decision\",\"emotion\",\"memory\",\"action\",\"relationship_delta\"]}}}}";
    return b.str();
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static bool httpPost(const std::string& url, const std::string& key, const std::string& body, std::string& data, std::string& err) {
    const std::wstring wurl = widen(url);
    if (wurl.empty()) {
        err = "Invalid OpenRouter endpoint";
        return false;
    }

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host) - 1;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path) - 1;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra) - 1;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        err = "Invalid OpenRouter endpoint";
        return false;
    }

    const std::wstring hostName(host, uc.dwHostNameLength);
    std::wstring objectName(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0)
        objectName.append(extra, uc.dwExtraInfoLength);

    HINTERNET ses = WinHttpOpen(
        L"GTA-SA-AI-NPC/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!ses) {
        err = "WinHttpOpen failed";
        return false;
    }

    WinHttpSetTimeouts(ses, cfg.timeoutMs, cfg.timeoutMs, cfg.timeoutMs, cfg.timeoutMs);

    HINTERNET con = WinHttpConnect(ses, hostName.c_str(), uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        err = "WinHttpConnect failed";
        return false;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(
        con,
        L"POST",
        objectName.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        err = "WinHttpOpenRequest failed";
        return false;
    }

    std::wstring headers = L"Content-Type: application/json
Authorization: Bearer ";
    headers += widen(key);
    headers += L"
HTTP-Referer: ";
    headers += widen(cfg.referer);
    headers += L"
X-Title: ";
    headers += widen(cfg.title);

    BOOL ok = WinHttpSendRequest(
        req,
        headers.c_str(),
        (DWORD)-1L,
        (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0
    );
    if (ok)
        ok = WinHttpReceiveResponse(req, nullptr);

    if (!ok) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        err = "HTTP request failed";
        return false;
    }

    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
        req,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &size,
        WINHTTP_NO_HEADER_INDEX
    )) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        err = "Failed to read HTTP status";
        return false;
    }

    data.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            err = "Failed to query HTTP response data";
            break;
        }
        if (avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got)) {
            err = "Failed to read HTTP response data";
            break;
        }
        if (got == 0) break;
        chunk.resize(got);
        data += chunk;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);

    if (!err.empty()) return false;
    if (statusCode < 200 || statusCode >= 300) {
        err = "OpenRouter HTTP status " + std::to_string(statusCode);
        return false;
    }
    return true;
}

static AiResult parseResponse(const std::string& raw) {
    AiResult r;
    auto p=raw.find("\"content\"");
    if(p==std::string::npos){r.error="No message.content in response";return r;}
    std::string content=jsonString(raw,"content",p);
    if(content.empty()){r.error="Empty AI content";return r;}
    const auto a=content.find('{'); const auto z=content.rfind('}');
    if(a!=std::string::npos && z!=std::string::npos && z>a) content=content.substr(a,z-a+1);
    r.reply=jsonString(content,"reply");
    r.decision=jsonString(content,"decision");
    r.emotion=jsonString(content,"emotion");
    r.memory=jsonString(content,"memory");
    r.action=parseAction(jsonString(content,"action"));
    r.relationshipDelta=std::clamp(jsonInt(content,"relationship_delta",0),-10,10);
    if(r.reply.empty()){r.error="Could not parse structured NPC reply";return r;}
    if((int)r.reply.size()>cfg.maxResponse) r.reply.resize(cfg.maxResponse);
    r.ok=true; return r;
}

static AiResult askAi(const RequestCtx& ctx) {
    AiResult r;
    if(cfg.apiKey.empty()){r.error="OpenRouter API key is missing";return r;}
    const unsigned now=CTimer::m_snTimeInMilliseconds;
    if(now-lastRequestTick<(unsigned)cfg.requestGapMs){r.error="Please wait a moment before the next request";return r;}
    lastRequestTick=now;
    std::string raw,err;
    if(!httpPost(cfg.endpoint,cfg.apiKey,requestBody(ctx),raw,err)){r.error=err;return r;}
    return parseResponse(raw);
}

static CTask* createGoToPointTask(const CVector& target, float radius = 1.2f) {
    // GTA SA 1.0 US: CTaskSimpleGoToPoint constructor at 0x667CD0.
    using GoToPointCtor = void (__thiscall*)(
        void*, eMoveState, const CVector&, float, bool, bool
    );

    void* memory = CTask::operator new(0x30);
    if (!memory) return nullptr;

    constexpr uintptr_t GO_TO_POINT_CTOR = 0x667CD0;
    auto ctor = reinterpret_cast<GoToPointCtor>(GO_TO_POINT_CTOR);
    ctor(memory, PEDMOVE_WALK, target, radius, false, false);
    return reinterpret_cast<CTask*>(memory);
}

static void setPrimaryTask(CPed* ped, CTask* task) {
    if (!ped || !ped->m_pIntelligence || !task) {
        if (task) CTask::operator delete(task);
        return;
    }
    ped->m_pIntelligence->m_TaskMgr.SetTask(task, TASK_PRIMARY_PRIMARY, false);
}

static void applyAction(CPed* ped, Action a) {
    if (!ped || !ped->m_pIntelligence) return;

    CPed* player = FindPlayerPed();
    if (!player) return;

    Identity* id = nullptr;
    auto it = identities.find(CPools::GetPedRef(ped));
    if (it != identities.end()) id = &it->second;

    switch (a) {
        case Action::FOLLOW_PLAYER: {
            auto* task = new CTaskComplexSeekEntityMove(
                player,
                0xFFFFFFFFu,
                300,
                std::max(1.5f, cfg.followDistance),
                8.0f,
                2.0f,
                false,
                true
            );
            setPrimaryTask(ped, task);
            status(id ? id->name + " is following you." : "NPC is following you.", 1800);
            break;
        }

        case Action::STOP_FOLLOW: {
            ped->m_pIntelligence->m_TaskMgr.FlushImmediately();
            ped->SetMoveState(PEDMOVE_STILL);
            status(id ? id->name + " stopped following." : "NPC stopped following.", 1800);
            break;
        }

        case Action::WAIT: {
            ped->m_pIntelligence->m_TaskMgr.FlushImmediately();
            ped->SetMoveState(PEDMOVE_STILL);
            status(id ? id->name + " is waiting." : "NPC is waiting.", 1800);
            break;
        }

        case Action::GO_TO_PLAYER: {
            auto* task = new CTaskComplexSeekEntityMove(
                player,
                30000,
                300,
                1.5f,
                8.0f,
                2.0f,
                false,
                true
            );
            setPrimaryTask(ped, task);
            status(id ? id->name + " is coming to you." : "NPC is coming to you.", 1800);
            break;
        }

        case Action::GO_HOME: {
            if (!id) break;
            CTask* task = createGoToPointTask(id->home);
            setPrimaryTask(ped, task);
            status(id->name + " is going home.", 1800);
            break;
        }

        case Action::GO_TO_WORK: {
            if (!id) break;
            CTask* task = createGoToPointTask(id->work);
            setPrimaryTask(ped, task);
            status(id->name + " is going to work.", 1800);
            break;
        }

        default:
            break;
    }
}

static void persistResponse(int ref, const AiResult& r) {
    std::lock_guard<std::mutex> lock(stateMutex);
    auto it=identities.find(ref); if(it==identities.end()) return;
    auto& id=it->second;
    id.relationship=std::clamp(id.relationship+r.relationshipDelta,-100,100);
    if(!r.memory.empty()){
        id.memory.push_back(r.memory);
        if((int)id.memory.size()>cfg.maxMemory) id.memory.erase(id.memory.begin(),id.memory.end()-cfg.maxMemory);
    }
    saveMemory(id);
}

static void trimHistory(int ref) {
    auto& h=histories[ref];
    while((int)h.size()>cfg.maxHistory*2) h.erase(h.begin(),h.begin()+2);
}

static void submitChat() {
    if(!validSelected() || requestBusy) return;
    const std::string text=trim(uiBuffer);
    if(text.empty()) return;
    if(distance(selectedPed,FindPlayerPed())>cfg.talkDistance){status("You are too far away."); return;}
    const int ref=selectedRef;
    Identity id=identityFor(selectedPed,ref);
    RequestCtx ctx{ id, histories[ref], text, worldContext(selectedPed) };
    histories[ref].push_back({"user",text}); trimHistory(ref);
    requestBusy=true; uiBuffer.clear();
    status("Thinking...",2200);
    std::thread([ctx,ref](){
        AiResult r=askAi(ctx);
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            completed=r; resultReady=true;
        }
        requestBusy=false;
        (void)ref;
    }).detach();
}

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if(msg==WM_KEYDOWN){
        if(wParam==VK_RETURN){ submitChat(); return 0; }
        if(wParam==VK_ESCAPE){ ShowWindow(hwnd,SW_HIDE); chatOpen=false; return 0; }
    }
    return CallWindowProc(gOldEditProc,hwnd,msg,wParam,lParam);
}

static void createInput() {
    if(gEdit) return;
    gGameWindow=GetForegroundWindow(); if(!gGameWindow) return;
    gEdit=CreateWindowExA(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,"EDIT","",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,120,520,420,30,gGameWindow,nullptr,gModule,nullptr);
    if(!gEdit) return;
    SendMessageA(gEdit,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    gOldEditProc=(WNDPROC)SetWindowLongPtrA(gEdit,GWLP_WNDPROC,(LONG_PTR)EditProc);
}

static void openChat() {
    if(!validSelected()) return;
    if(distance(selectedPed,FindPlayerPed())>cfg.talkDistance){status("Move closer to the NPC.");return;}
    createInput(); if(!gEdit) return;
    chatOpen=true; uiBuffer.clear(); SetWindowTextA(gEdit,"");
    ShowWindow(gEdit,SW_SHOW); SetFocus(gEdit); status("Type a message and press Enter.",2000);
}

static void syncInputText() {
    if(!gEdit || !chatOpen || requestBusy) return;
    char buf[1024]{}; GetWindowTextA(gEdit,buf,sizeof(buf)); uiBuffer=buf;
}

static void drawText(float x,float y,const std::string& text,float sx=0.40f,float sy=0.85f) {
    CFont::SetOrientation(ALIGN_LEFT);
    CFont::SetColor(CRGBA(255,255,255,255));
    CFont::SetDropShadowPosition(1);
    CFont::SetBackground(false,false);
    CFont::SetProportional(true);
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetScale(sx,sy);
    CFont::SetWrapx(600.0f);
    CFont::PrintString(x,y,const_cast<char*>(text.c_str()));
}

static void drawChat() {
    if(!chatOpen || !validSelected()) return;
    const int ref=selectedRef; auto it=identities.find(ref); if(it==identities.end()) return;
    const Identity& id=it->second;
    CFont::SetBackground(true,false); CFont::SetBackgroundColor(CRGBA(0,0,0,150));
    drawText(38.0f,292.0f,id.name,0.50f,1.00f);
    float y=320.0f;
    auto& h=histories[ref];
    const int start=(int)h.size()>8?(int)h.size()-8:0;
    for(int i=start;i<(int)h.size();++i){
        const std::string prefix=h[i].role=="user"?"You: ":id.name+": ";
        drawText(38.0f,y,prefix+h[i].text,0.37f,0.78f); y+=22.0f;
    }
    std::string input=uiBuffer.empty()?"Type a message...":uiBuffer;
    drawText(38.0f,502.0f,input,0.40f,0.85f);
    if(requestBusy) drawText(38.0f,528.0f,"Thinking...",0.34f,0.75f);
}

static void drawName(CPed* ped) {
    if(!cfg.showNames || !enabled || !ped || !ped->IsAlive()) return;
    CPed* player=FindPlayerPed(); if(!player || distance(ped,player)>cfg.nameDistance) return;
    int ref=CPools::GetPedRef(ped); if(ref<0) return;
    Identity& id=identityFor(ped,ref);
    const CVector p=ped->GetPosition(); RwV3d world={p.x,p.y,p.z+1.15f}; RwV3d screen{}; float w=0,h=0;
    if(!CSprite::CalcScreenCoors(world,&screen,&w,&h,true,true)) return;
    CFont::SetOrientation(ALIGN_CENTER); CFont::SetColor(CRGBA(255,255,255,255)); CFont::SetDropShadowPosition(1); CFont::SetBackground(false,false); CFont::SetProportional(true); CFont::SetFontStyle(FONT_SUBTITLES); CFont::SetScale(0.35f,0.75f); CFont::PrintString(screen.x,screen.y,const_cast<char*>(id.name.c_str()));
}

static void drawing() {
    if(!enabled) return;
    if(CPools::ms_pPedPool){
        for(int i=0;i<CPools::ms_pPedPool->m_nSize;++i){ CPed* p=CPools::ms_pPedPool->GetAt(i); if(p) drawName(p); }
    }
    if(validSelected()){
        const int ref=selectedRef; auto it=identities.find(ref); if(it!=identities.end()){
            const Identity& id=it->second;
            std::ostringstream t; t << id.name << "  |  " << id.factionName << "  |  " << id.occupation;
            drawText(24.0f,70.0f,t.str(),0.34f,0.74f);
            if(!requestBusy && !chatOpen) drawText(24.0f,92.0f,"T Talk",0.32f,0.70f);
        }
    }
    if(chatOpen) drawChat();
    if(!statusText.empty() && CTimer::m_snTimeInMilliseconds < statusUntil) drawText(24.0f,550.0f,statusText,0.33f,0.72f);
}

static void processResult() {
    bool ready=false; AiResult r;
    { std::lock_guard<std::mutex> lock(resultMutex); if(resultReady){r=completed;resultReady=false;ready=true;} }
    if(!ready || !validSelected()) return;
    const int ref=selectedRef;
    if(r.ok){
        histories[ref].push_back({"assistant",r.reply}); trimHistory(ref); persistResponse(ref,r); applyAction(selectedPed,r.action);
        std::string banner=r.reply;
        if(r.decision=="REFUSE") banner=identities[ref].name+" refused.";
        status(banner,cfg.messageMs);
        if(gEdit){ShowWindow(gEdit,SW_HIDE);chatOpen=false;}
    }else status("AI error: "+r.error,4500);
}

static void cleanupStale() {
    const unsigned now=CTimer::m_snTimeInMilliseconds;
    if(now%5000>50) return;
    std::vector<int> active;
    if(CPools::ms_pPedPool){
        for(int i=0;i<CPools::ms_pPedPool->m_nSize;++i){ CPed* p=CPools::ms_pPedPool->GetAt(i); if(!p||!p->IsAlive()) continue; int ref=CPools::GetPedRef(p); if(ref>=0){ active.push_back(ref); lastSeen[ref]=now; lastPosition[ref]=p->GetPosition(); } }
    }
    std::vector<int> dead;
    for(const auto& kv:lastSeen) if(now-kv.second>15000) dead.push_back(kv.first);
    for(int ref:dead){ identities.erase(ref); histories.erase(ref); lastSeen.erase(ref); lastPosition.erase(ref); }
}

static void input() {
    static SHORT prevF=0,prevT=0,prevF10=0;
    const SHORT f=GetAsyncKeyState(cfg.selectKey), t=GetAsyncKeyState(cfg.chatKey), f10=GetAsyncKeyState(cfg.toggleKey);
    const bool fDown=(f&0x8000)&&!(prevF&0x8000), tDown=(t&0x8000)&&!(prevT&0x8000), f10Down=(f10&0x8000)&&!(prevF10&0x8000);
    prevF=f;prevT=t;prevF10=f10;
    if(f10Down){enabled=!enabled;status(enabled?"AI NPC enabled":"AI NPC disabled",1800);if(!enabled && gEdit)ShowWindow(gEdit,SW_HIDE);}
    if(!enabled || chatOpen) return;
    if(fDown){ selectedPed=closestPed(); selectedRef=selectedPed?CPools::GetPedRef(selectedPed):-1; if(selectedPed){Identity& id=identityFor(selectedPed,selectedRef);status("Selected "+id.name,1600);} }
    if(tDown && validSelected()) openChat();
}

static void process() {
    if(!FindPlayerPed()) return;
    syncInputText(); processResult(); cleanupStale();
    if(validSelected() && distance(selectedPed,FindPlayerPed())>cfg.talkDistance+1.5f && chatOpen){if(gEdit)ShowWindow(gEdit,SW_HIDE);chatOpen=false;status("Conversation ended: too far away.",1800);}
    input();
}

static void init() {
    LoadConfig();
    gGameWindow=GetForegroundWindow();
    std::filesystem::create_directories(gGameDir+"\\AI_NPC\\logs");
    statusText="AI NPC loaded"; statusUntil=CTimer::m_snTimeInMilliseconds+3000;
}

class PluginMain {
public:
    PluginMain() {
        Events::initGameEvent += []{ init(); };
        Events::gameProcessEvent += []{ process(); };
        Events::drawingEvent += []{ drawing(); };
    }
};

static PluginMain pluginMain;

} // namespace ainpc

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){ ainpc::gModule=hModule; DisableThreadLibraryCalls(hModule); }
    return TRUE;
}
