// EDIplayer - plays the services of a DAB multiplex received as EDI over
// UDP or TCP, with Dynamic Label (DL Plus) and Slideshow (Dear ImGui).
//
// Layout: source bar on top, the services from the FIC on the left, and the
// selected service's slideshow and dynamic label on the right.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // before GLFW, which would otherwise define APIENTRY itself
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "core/audio_out.h"
#include "core/log.h"
#include "core/player.h"

#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ---------------------------------------------------------------------------
// Data

struct SlideTexture {
    GLuint tex = 0;
    int w = 0, h = 0;
    uint64_t version = 0;
    std::shared_ptr<const Slide> slide;
};

struct App {
    // settings
    char url[256] = "";
    float volume = 0.8f;
    int lastSid = -1;
    std::vector<std::string> recent;

    // runtime
    AudioOut audio;
    Player player{&audio};
    PlayerSnapshot snap;
    SlideTexture slide;
    std::string message;               // last start error
    std::deque<std::string> log;
    size_t logCounter = 0;
    uint64_t lastBytes = 0;
    double lastRateTime = 0, rxKbps = 0;
    bool scrollToSelected = false;
    int selectedIndex = -1;            // list row last scrolled to
    std::string settingsPath;
};

static double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Settings (key=value text file)

static std::string settingsFile()
{
#ifdef _WIN32
    // next to the executable, as dab2edi-gui does
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "\\EDIplayer.ini";
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string dir = xdg && *xdg ? xdg : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config";
    dir += "/ediplayer";
    mkdir(dir.c_str(), 0755);
    return dir + "/settings.ini";
#endif
}

static void loadSettings(App &a)
{
    std::ifstream f(a.settingsPath);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "url") snprintf(a.url, sizeof(a.url), "%s", v.c_str());
        else if (k == "volume") a.volume = std::max(0.0f, std::min(1.0f, (float)atof(v.c_str())));
        else if (k == "sid") a.lastSid = v.empty() ? -1 : (int)strtol(v.c_str(), nullptr, 16);
        else if (k == "recent" && !v.empty() && a.recent.size() < 10) a.recent.push_back(v);
    }
}

static void saveSettings(const App &a)
{
    std::ofstream f(a.settingsPath);
    if (!f) return;
    char sid[16] = "";
    if (a.lastSid >= 0) snprintf(sid, sizeof(sid), "%04X", a.lastSid);
    f << "url=" << a.url << "\n" << "volume=" << a.volume << "\n" << "sid=" << sid << "\n";
    for (const auto &r : a.recent) f << "recent=" << r << "\n";
}

static void addRecent(App &a, const std::string &url)
{
    a.recent.erase(std::remove(a.recent.begin(), a.recent.end(), url), a.recent.end());
    a.recent.insert(a.recent.begin(), url);
    if (a.recent.size() > 10) a.recent.resize(10);
}

// ---------------------------------------------------------------------------
// Player control

static void startPlayer(App &a)
{
    if (a.player.running()) return;
    std::string err;
    a.player.select(a.lastSid);
    if (!a.player.start(a.url, err)) {
        a.message = err;
        return;
    }
    a.message.clear();
    a.scrollToSelected = a.lastSid >= 0;
    a.selectedIndex = -1;
    addRecent(a, a.url);
    saveSettings(a);
}

static void stopPlayer(App &a)
{
    a.player.stop();
    saveSettings(a);
}

static void playService(App &a, int sid)
{
    a.lastSid = sid;
    a.player.select(sid);
    if (!a.player.running()) startPlayer(a);
    saveSettings(a);
}

// Decode a new slide into the texture (on the GUI thread)
static void updateSlide(App &a)
{
    if (a.snap.slideVersion == a.slide.version) return;
    a.slide.version = a.snap.slideVersion;
    a.slide.slide = a.snap.slide;
    if (!a.snap.slide) {
        a.slide.w = a.slide.h = 0;
        return;
    }
    int w, h, n;
    const auto &d = a.snap.slide->data;
    unsigned char *px = stbi_load_from_memory(d.data(), (int)d.size(), &w, &h, &n, 4);
    if (!px) {
        applog::add(std::string("SLS: cannot decode image: ") + stbi_failure_reason());
        a.slide.w = a.slide.h = 0;
        return;
    }
    if (!a.slide.tex) glGenTextures(1, &a.slide.tex);
    glBindTexture(GL_TEXTURE_2D, a.slide.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    a.slide.w = w;
    a.slide.h = h;
}

static void pollPlayer(App &a)
{
    a.snap = a.player.snapshot();
    updateSlide(a);

    std::vector<std::string> lines;
    a.logCounter = applog::fetch(a.logCounter, lines);
    for (auto &l : lines) a.log.push_back(l);
    while (a.log.size() > 500) a.log.pop_front();

    const double t = now();
    if (t - a.lastRateTime >= 1.0) {
        a.rxKbps = a.snap.rxBytes >= a.lastBytes && a.lastRateTime > 0
                       ? (a.snap.rxBytes - a.lastBytes) * 8.0 / 1000.0 / (t - a.lastRateTime) : 0;
        a.lastBytes = a.snap.rxBytes;
        a.lastRateTime = t;
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers

static ImU32 col(float r, float g, float b, float al = 1.0f) { return ImGui::GetColorU32(ImVec4(r, g, b, al)); }
static const ImVec4 kAccent(0.20f, 0.55f, 0.95f, 1.0f);

enum Icon { IconPlay, IconStop };

static bool iconButton(const char *id, Icon icon, float size, ImU32 bg, ImU32 fg, const char *tip)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r = size * 0.5f;
    if (bg) {
        ImVec4 b = ImGui::ColorConvertU32ToFloat4(bg);
        if (hov) { b.x *= 1.15f; b.y *= 1.15f; b.z *= 1.15f; }
        if (act) { b.x *= 0.85f; b.y *= 0.85f; b.z *= 0.85f; }
        dl->AddCircleFilled(c, r, ImGui::GetColorU32(b), 48);
    } else if (hov) {
        dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 32);
    }
    if (icon == IconPlay) {
        const float s = size * 0.22f;
        dl->AddTriangleFilled(ImVec2(c.x - s * 0.8f, c.y - s * 1.15f), ImVec2(c.x - s * 0.8f, c.y + s * 1.15f),
                              ImVec2(c.x + s * 1.25f, c.y), fg);
    } else {
        const float s = size * 0.18f;
        dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), fg, s * 0.25f);
    }
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

static std::string fitText(const std::string &s, float width)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();   // whole UTF-8 characters
        if (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0xC0) t.pop_back();
    }
    return t + "...";
}

static void beginPanel(const char *id, ImVec2 size, ImGuiChildFlags flags = 0)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::BeginChild(id, size, flags | ImGuiChildFlags_AlwaysUseWindowPadding);
}

static void endPanel()
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static const ServiceEntry *findService(const App &a, int sid)
{
    for (const auto &s : a.snap.services)
        if (s.sid == sid) return &s;
    return nullptr;
}

// Three bouncing bars: "this one is playing"
static void drawPlayingBars(ImDrawList *dl, ImVec2 p, float h, float level, bool live, ImU32 c)
{
    const float w = h * 0.18f, gap = h * 0.12f;
    const double t = now();
    for (int i = 0; i < 3; i++) {
        float f = live ? 0.25f + 0.75f * std::min(1.0f, level * 2.5f) *
                                     (0.55f + 0.45f * (float)sin(t * (7.0 + i * 2.3) + i * 1.7))
                       : 0.25f;
        float x = p.x + i * (w + gap);
        dl->AddRectFilled(ImVec2(x, p.y + h * (1 - f)), ImVec2(x + w, p.y + h), c, w * 0.3f);
    }
}

// ---------------------------------------------------------------------------
// UI sections

static void drawSourceBar(App &a)
{
    const ImGuiStyle &st = ImGui::GetStyle();
    const bool running = a.player.running();
    beginPanel("source", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);

    const float fh = ImGui::GetFrameHeight();
    const float btn = fh * 1.25f;

    // state pill
    const char *stxt = "IDLE";
    ImU32 dot = col(0.45f, 0.47f, 0.52f);
    if (running) {
        switch (a.snap.sourceState) {
        case EdiSource::State::Receiving:
            stxt = "RECEIVING";
            dot = col(0.25f, 0.80f, 0.40f);
            break;
        case EdiSource::State::Waiting:
            stxt = "RETRYING";
            dot = col(0.90f, 0.30f, 0.25f);
            break;
        default:
            stxt = "CONNECTING";
            dot = col(0.95f, 0.70f, 0.20f);
            break;
        }
    }
    const float pillW = ImGui::CalcTextSize("CONNECTING").x + fh;
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize();
    const float yOff = (btn - fs) * 0.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + fs * 0.35f, p.y + yOff + fs * 0.55f), fs * 0.3f, dot, 16);
    ImGui::SetCursorScreenPos(ImVec2(p.x + fs * 0.9f, p.y + yOff));
    ImGui::TextUnformatted(stxt);
    ImGui::SetCursorScreenPos(ImVec2(p.x + pillW, p.y + (btn - fh) * 0.5f));

    // URL + recent list
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - btn - st.ItemSpacing.x * 2);
    bool enter = ImGui::InputTextWithHint("##url", "tcp://host:port   udp://239.1.2.3:5004   udp://5004", a.url,
                                          sizeof(a.url), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("EDI source\n"
                          "  tcp://host:port            connect to an EDI TCP server\n"
                          "  udp://port                 receive unicast UDP\n"
                          "  udp://group:port           join a multicast group\n"
                          "  udp://group:port?iface=ip  ... on a specific interface");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##recent", ImGuiDir_Down)) ImGui::OpenPopup("recent");
    if (ImGui::IsItemHovered() && !running) ImGui::SetTooltip("Recent sources");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("recent")) {
        if (a.recent.empty()) ImGui::TextDisabled("No recent sources");
        for (const auto &r : a.recent)
            if (ImGui::Selectable(r.c_str())) snprintf(a.url, sizeof(a.url), "%s", r.c_str());
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
    if (running) {
        if (iconButton("##stop", IconStop, btn, col(0.80f, 0.25f, 0.25f), col(1, 1, 1), "Stop")) stopPlayer(a);
    } else {
        ImGui::BeginDisabled(!a.url[0]);
        if (iconButton("##play", IconPlay, btn, col(0.18f, 0.62f, 0.32f), col(1, 1, 1), "Start receiving") || (enter && a.url[0]))
            startPlayer(a);
        ImGui::EndDisabled();
    }
    endPanel();
}

static void drawServices(App &a, float width)
{
    beginPanel("services", ImVec2(width, 0));
    const PlayerSnapshot &s = a.snap;

    // ensemble header
    std::string ens = s.ensemble.empty() ? "Services" : s.ensemble;
    ImGui::TextUnformatted(fitText(ens, ImGui::GetContentRegionAvail().x).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    char sub[96] = "";
    if (!s.services.empty())
        snprintf(sub, sizeof(sub), "%zu service%s   EId %04X", s.services.size(), s.services.size() == 1 ? "" : "s",
                 s.eid);
    ImGui::TextUnformatted(sub[0] ? sub : (a.player.running() ? "Waiting for the FIC..." : "Not receiving"));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::BeginChild("list", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    if (s.services.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.player.running() ? "The services will appear here once the multiplex "
                                                       "information has been received."
                                                     : "Enter an EDI source above and press play.");
        ImGui::PopTextWrapPos();
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float rowH = fs * 2.0f + ImGui::GetStyle().FramePadding.y * 2;
    const float padX = ImGui::GetStyle().FramePadding.x;
    for (const auto &e : s.services) {
        ImGui::PushID(e.sid);
        const bool sel = e.sid == s.selectedSid;
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("row", ImVec2(w, rowH))) {
            a.scrollToSelected = false;
            if (!sel) playService(a, e.sid);
        }
        const bool hov = ImGui::IsItemHovered();
        // resumed service: keep it in view while labels arrive and re-sort the list
        const int index = (int)(&e - s.services.data());
        if (sel && a.scrollToSelected && index != a.selectedIndex) {
            ImGui::SetScrollHereY(0.5f);
            a.selectedIndex = index;
        }
        if (sel)
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.28f)), 6.0f);
        else if (hov)
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6.0f);

        const float barsW = sel ? fs * 1.2f : 0;
        char sidName[24];
        snprintf(sidName, sizeof(sidName), "SId %04X", e.sid);
        const std::string name = e.label.empty() ? sidName : e.label;
        const float ty = p.y + (rowH - fs * 2.05f) * 0.5f;
        dl->AddText(ImVec2(p.x + padX, ty), ImGui::GetColorU32(ImGuiCol_Text),
                    fitText(name, w - padX * 2 - barsW).c_str());
        char info[64];
        snprintf(info, sizeof(info), "%s  %d kbit/s", e.dabplus ? "DAB+" : "DAB", e.kbps);
        dl->AddText(ImVec2(p.x + padX, ty + fs * 1.05f), ImGui::GetColorU32(ImGuiCol_TextDisabled), info);
        if (sel)
            drawPlayingBars(dl, ImVec2(p.x + w - padX - fs * 0.85f, p.y + (rowH - fs) * 0.5f), fs,
                            std::max(a.audio.peakLeft(), a.audio.peakRight()), s.audioOk && a.player.running(),
                            ImGui::GetColorU32(kAccent));
        if (hov) {
            ImGui::SetTooltip("%s\nSId %04X   SubChId %d\n%s, %d kbit/s", name.c_str(), e.sid, e.scid,
                              e.dabplus ? "DAB+ (HE-AAC)" : "DAB (MPEG Layer II)", e.kbps);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    endPanel();
}

static void drawVolume(App &a, float width)
{
    // level meter (two thin bars) above the volume slider
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 3.0f * ImGui::GetStyle().FontScaleDpi;
    const float lv[2] = {a.audio.peakLeft(), a.audio.peakRight()};
    for (int i = 0; i < 2; i++) {
        float y = p.y + i * (h + 2);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width, y + h), col(1, 1, 1, 0.07f), h * 0.5f);
        float f = std::min(1.0f, lv[i]);
        ImU32 c = f > 0.97f ? col(0.90f, 0.30f, 0.25f) : col(0.25f, 0.80f, 0.40f);
        if (f > 0.005f) dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + width * f, y + h), c, h * 0.5f);
    }
    ImGui::Dummy(ImVec2(width, h * 2 + 2));
    ImGui::SetNextItemWidth(width);
    int pct = (int)lroundf(a.volume * 100);
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "Vol %d%%%%", pct);   // fmt is itself a format string
    if (ImGui::SliderFloat("##vol", &a.volume, 0.0f, 1.0f, fmt)) a.audio.setVolume(a.volume);
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings(a);
}

static void drawNowPlaying(App &a)
{
    beginPanel("now", ImVec2(0, 0));
    const PlayerSnapshot &s = a.snap;
    const ServiceEntry *svc = findService(a, s.selectedSid);
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float volW = 150 * scale;

    // header: station + format, volume on the right
    ImGui::BeginGroup();
    const float textW = ImGui::GetContentRegionAvail().x - volW - ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    std::string title = svc && !svc->label.empty() ? svc->label : s.selectedSid >= 0 ? "Tuning..." : "No service selected";
    ImGui::TextUnformatted(fitText(title, textW).c_str());
    ImGui::PopFont();
    std::string det;
    if (s.selectedSid >= 0 && a.player.running()) {
        if (!s.audioFormat.empty()) det = s.audioFormat;
        else if (svc) det = svc->dabplus ? "DAB+" : "DAB";
        if (svc) det += "  -  " + std::to_string(svc->kbps) + " kbit/s";
        if (!s.audioOk) det += "  -  buffering";
    } else if (s.selectedSid < 0) {
        det = "Choose a service from the list";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(fitText(det, textW).c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - volW);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
    drawVolume(a, volW);
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // slideshow, 320 x 240
    const ImVec2 box(320 * scale, 240 * scale);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), col(0, 0, 0, 0.35f), 6.0f);
    if (a.slide.tex && a.slide.w > 0 && s.slide) {
        float k = std::min(box.x / a.slide.w, box.y / a.slide.h);
        ImVec2 sz(a.slide.w * k, a.slide.h * k);
        ImVec2 o(p.x + (box.x - sz.x) * 0.5f, p.y + (box.y - sz.y) * 0.5f);
        dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)a.slide.tex), o, ImVec2(o.x + sz.x, o.y + sz.y),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6.0f);
    } else {
        const char *t = s.selectedSid >= 0 ? "No slideshow" : "";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (box.x - ts.x) * 0.5f, p.y + (box.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    }
    ImGui::InvisibleButton("slide", box);
    if (ImGui::IsItemHovered() && s.slide) {
        const Slide &sl = *s.slide;
        std::string tip = sl.name.empty() ? "Slide" : sl.name;
        tip += "\n" + std::to_string(a.slide.w) + " x " + std::to_string(a.slide.h) + "  " + sl.ext + ", " +
               std::to_string(sl.data.size() / 1024) + " kB";
        if (!sl.categoryTitle.empty()) tip += "\nCategory: " + sl.categoryTitle;
        if (!sl.clickUrl.empty()) tip += "\n" + sl.clickUrl;
        ImGui::SetTooltip("%s", tip.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // dynamic label
    ImGui::BeginChild("dls", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    if (!s.dls.empty()) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(s.dls.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    } else if (s.selectedSid >= 0) {
        ImGui::TextDisabled("No dynamic label yet");
    }

    if (!s.dlplus.empty()) {
        ImGui::Spacing();
        float lw = 0;
        for (const auto &d : s.dlplus) lw = std::max(lw, ImGui::CalcTextSize(Player::dlPlusTypeName(d.type)).x);
        if (ImGui::BeginTable("dlplus", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, lw + ImGui::GetFontSize());
            ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
            for (const auto &d : s.dlplus) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", Player::dlPlusTypeName(d.type));
                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(d.text.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
        if (!s.itemRunning) {
            bool hasItem = false;
            for (const auto &d : s.dlplus) hasItem |= d.type >= 1 && d.type <= 11;
            if (hasItem) ImGui::TextDisabled("(item not running)");
        }
    }
    ImGui::EndChild();
    endPanel();
}

static void logPopup(App &a)
{
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vs.x * 0.9f, vs.y * 0.8f));
    if (!ImGui::BeginPopupModal("Log", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    ImGui::BeginChild("logtext", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &l : a.log) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (ImGui::Button("Copy")) {
        std::string all;
        for (const auto &l : a.log) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void drawStatus(App &a)
{
    const PlayerSnapshot &s = a.snap;
    std::string t;
    char b[160];
    if (!a.message.empty()) {
        t = a.message;
    } else if (a.player.running()) {
        if (!s.sourceError.empty() && s.sourceState != EdiSource::State::Receiving) {
            t = s.sourceError;
        } else {
            snprintf(b, sizeof(b), "%s   %.0f kbit/s   %llu frames", s.sourceUrl.c_str(), a.rxKbps,
                     (unsigned long long)s.afFrames);
            t = b;
            if (s.selectedSid >= 0 && s.auOk + s.auErrors > 0) {
                snprintf(b, sizeof(b), "   AU errors %u   buffer %d ms", s.auErrors, a.audio.bufferedMs());
                t += b;
            }
        }
        std::string ae = a.audio.lastError();
        if (!ae.empty()) t = ae;
    } else {
        t = "Stopped";
    }
    const float logW = ImGui::CalcTextSize("Log").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(fitText(t, ImGui::GetContentRegionAvail().x - logW - ImGui::GetStyle().ItemSpacing.x).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - logW);
    if (ImGui::Button("Log")) ImGui::OpenPopup("Log");
    logPopup(a);
}

// ---------------------------------------------------------------------------
// Test hooks (environment variables, unused in normal operation):
//   EDIPLAYER_URL=<source>            use this source instead of the saved one
//   EDIPLAYER_AUTOSTART=1             start receiving at launch
//   EDIPLAYER_SID=<hex>               play this service
//   EDIPLAYER_SHOTS=<sec>:<file.png>,<sec>:<file.png>...   framebuffer captures
//   EDIPLAYER_EXIT=<sec>              quit after <sec>

static uint32_t crc32Png(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

static void writePng(const char *path, int w, int h, const std::vector<uint8_t> &rgba)
{
    // uncompressed (stored) deflate blocks: simple and dependency free
    std::vector<uint8_t> raw;
    for (int y = h - 1; y >= 0; y--) {                 // GL rows are bottom-up
        raw.push_back(0);
        for (int x = 0; x < w; x++)
            for (int k = 0; k < 3; k++) raw.push_back(rgba[(size_t)(y * w + x) * 4 + k]);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xff); z.push_back(n >> 8);
        z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    uint32_t ad = (s2 << 16) | s1;
    for (int k = 3; k >= 0; k--) z.push_back((ad >> (k * 8)) & 0xff);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    auto chunk = [&](const char *type, const std::vector<uint8_t> &d) {
        uint8_t len[4] = {(uint8_t)(d.size() >> 24), (uint8_t)(d.size() >> 16), (uint8_t)(d.size() >> 8), (uint8_t)d.size()};
        fwrite(len, 1, 4, f);
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        uint32_t c = crc32Png(td.data(), td.size()) ^ 0xFFFFFFFFu;
        uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(cb, 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                                 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

struct TestHooks {
    std::vector<std::pair<double, std::string>> shots;
    double exitAt = -1;
    bool autostart = false;
    int sid = -1;
    void load(App &a)
    {
        if (const char *s = getenv("EDIPLAYER_SHOTS")) {
            std::stringstream ss(s);
            std::string item;
            while (std::getline(ss, item, ',')) {
                size_t c = item.find(':');
                if (c != std::string::npos) shots.push_back({atof(item.c_str()), item.substr(c + 1)});
            }
        }
        if (const char *e = getenv("EDIPLAYER_EXIT")) exitAt = atof(e);
        if (const char *u = getenv("EDIPLAYER_URL")) snprintf(a.url, sizeof(a.url), "%s", u);
        if (const char *s = getenv("EDIPLAYER_SID")) a.lastSid = (int)strtol(s, nullptr, 16);
        autostart = getenv("EDIPLAYER_AUTOSTART") != nullptr;
    }
};

// ---------------------------------------------------------------------------
// Theme and fonts

static void applyTheme(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(8, 8);
    s.CellPadding = ImVec2(6, 3);
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 6;
    s.ChildRounding = 8;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.094f, 0.114f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.118f, 0.141f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = ImVec4(0.149f, 0.161f, 0.192f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.188f, 0.204f, 0.243f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.220f, 0.239f, 0.286f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.169f, 0.184f, 0.220f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.224f, 0.243f, 0.290f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.263f, 0.286f, 0.341f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.169f, 0.184f, 0.220f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.224f, 0.243f, 0.290f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.263f, 0.286f, 0.341f, 1.0f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.64f, 1.0f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_TitleBg] = ImVec4(0.133f, 0.145f, 0.173f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.133f, 0.145f, 0.173f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.133f, 0.145f, 0.173f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

static void loadFont()
{
    static const char *candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    ImGuiIO &io = ImGui::GetIO();
    for (const char *f : candidates) {
        FILE *t = fopen(f, "rb");
        if (!t) continue;
        fclose(t);
        if (io.Fonts->AddFontFromFileTTF(f)) break;
    }
    ImGui::GetStyle().FontSizeBase = 16.0f;
}

// ---------------------------------------------------------------------------

int main(int, char **)
{
    App a;
    a.settingsPath = settingsFile();
    loadSettings(a);
    a.audio.setVolume(a.volume);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow *win = glfwCreateWindow((int)(940 * scale), (int)(600 * scale), "EDIplayer", nullptr, nullptr);
    if (!win) return 1;
    glfwSetWindowSizeLimits(win, (int)(720 * scale), (int)(520 * scale), GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyTheme(scale);
    loadFont();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    TestHooks th;
    th.load(a);
    const double t0 = now();
    if (th.autostart && a.url[0]) startPlayer(a);

    while (!glfwWindowShouldClose(win)) {
        if (th.exitAt >= 0 && now() - t0 >= th.exitAt) glfwSetWindowShouldClose(win, GLFW_TRUE);

        // ~20 fps while playing (level meter, label updates), slower when idle
        glfwWaitEventsTimeout(a.player.running() ? 0.05 : 0.25);
        pollPlayer(a);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        drawSourceBar(a);
        ImGui::Spacing();

        // the two columns fill the space down to the status line
        const float statusH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("columns", ImVec2(0, -statusH), 0, ImGuiWindowFlags_NoBackground);
        const float listW = std::max(240 * scale, std::min(320 * scale, ImGui::GetContentRegionAvail().x * 0.32f));
        drawServices(a, listW);
        ImGui::SameLine();
        drawNowPlaying(a);
        ImGui::EndChild();
        drawStatus(a);
        ImGui::End();

        ImGui::Render();
        int fw, fhh;
        glfwGetFramebufferSize(win, &fw, &fhh);
        glViewport(0, 0, fw, fhh);
        glClearColor(0.086f, 0.094f, 0.114f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!th.shots.empty() && now() - t0 >= th.shots.front().first) {
            std::vector<uint8_t> px((size_t)fw * fhh * 4);
            glReadPixels(0, 0, fw, fhh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            writePng(th.shots.front().second.c_str(), fw, fhh, px);
            th.shots.erase(th.shots.begin());
        }
        glfwSwapBuffers(win);
    }

    a.player.stop();
    saveSettings(a);
    if (a.slide.tex) glDeleteTextures(1, &a.slide.tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
