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
#include "core/countries.h"
#include "core/log.h"
#include "flags_data.h"
#ifndef _WIN32
#include "icon_data.h"   // Windows uses the .ico resource instead
#endif
#include "core/player.h"

#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"   // PNG for copying images to the clipboard

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // HWND for the clipboard
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
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

struct LogoTextures {
    SharedBytes smallSrc, largeSrc;    // what the textures were made from
    GLuint logo32 = 0, logo320 = 0;
    int largeW = 0, largeH = 0;
};

struct FlagTexture {
    GLuint tex = 0;
    int w = 0, h = 0;
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
    std::map<uint16_t, LogoTextures> logos;
    uint64_t logosVersion = 0;
    LogoTextures ensembleLogo;                  // logo32 / smallSrc only
    std::map<std::string, FlagTexture> flags;   // by ISO code
    std::string message;               // last start error
    std::string toast;                 // short-lived status message
    double toastTime = -100;
    GLFWwindow *window = nullptr;
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

static std::string executableDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    char u8[MAX_PATH * 3];
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, u8, sizeof(u8), nullptr, nullptr);
    std::string p(u8, m > 0 ? (size_t)m : 0);
    size_t s = p.find_last_of("\\/");
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = n > 0 ? std::string(buf, (size_t)n) : ".";
    size_t s = p.rfind('/');
#endif
    return s == std::string::npos ? "." : p.substr(0, s);
}

// EDIplayer_SPI/ goes next to the program; on Linux fall back to the config
// folder when the program is installed somewhere read-only
static std::string spiBaseDir()
{
    std::string d = executableDir();
#ifndef _WIN32
    if (access(d.c_str(), W_OK) != 0) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        d = (xdg && *xdg ? std::string(xdg) : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config") + "/ediplayer";
    }
#endif
    return d;
}

static std::string settingsFile()
{
#ifdef _WIN32
    // next to the executable, as dab2edi-gui does
    return executableDir() + "\\EDIplayer.ini";
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
    std::ifstream f(std::filesystem::u8path(a.settingsPath));
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
    std::ofstream f(std::filesystem::u8path(a.settingsPath));
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

static void clearLogos(App &a)
{
    for (auto &l : a.logos) {
        if (l.second.logo32) glDeleteTextures(1, &l.second.logo32);
        if (l.second.logo320) glDeleteTextures(1, &l.second.logo320);
    }
    a.logos.clear();
    if (a.ensembleLogo.logo32) glDeleteTextures(1, &a.ensembleLogo.logo32);
    a.ensembleLogo = LogoTextures();
    a.logosVersion = 0;
}

static void startPlayer(App &a)
{
    if (a.player.running()) return;
    clearLogos(a);
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

// Decode an image file into texture `tex` (created if 0). Returns false if
// the image can't be decoded.
// Shrink RGBA pixels so the longer side is maxSide, averaging the source area
// of each output pixel (alpha-weighted, so transparent edges don't darken).
// Plain bilinear sampling of a much larger texture looks jagged.
static std::vector<uint8_t> shrinkRgba(const uint8_t *src, int w, int h, int maxSide, int &ow, int &oh)
{
    const float k = (float)maxSide / (float)std::max(w, h);
    ow = std::max(1, (int)lroundf(w * k));
    oh = std::max(1, (int)lroundf(h * k));
    std::vector<uint8_t> out((size_t)ow * oh * 4);
    for (int oy = 0; oy < oh; oy++) {
        const int y0 = oy * h / oh, y1 = std::max(y0 + 1, (oy + 1) * h / oh);
        for (int ox = 0; ox < ow; ox++) {
            const int x0 = ox * w / ow, x1 = std::max(x0 + 1, (ox + 1) * w / ow);
            double r = 0, g = 0, b = 0, a = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint8_t *p = src + ((size_t)y * w + x) * 4;
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                }
            uint8_t *q = &out[((size_t)oy * ow + ox) * 4];
            const int n = (x1 - x0) * (y1 - y0);
            q[0] = a > 0 ? (uint8_t)lround(r / a) : 0;
            q[1] = a > 0 ? (uint8_t)lround(g / a) : 0;
            q[2] = a > 0 ? (uint8_t)lround(b / a) : 0;
            q[3] = (uint8_t)lround(a / n);
        }
    }
    return out;
}

// Decode an image into texture `tex` (created if 0). With maxSide > 0 the
// image is first shrunk to about that size. w/h return the decoded image's
// size; false if it can't be decoded.
static bool loadTexture(const std::vector<uint8_t> &d, GLuint &tex, int &w, int &h, const char *what, int maxSide = 0)
{
    int n;
    unsigned char *px = stbi_load_from_memory(d.data(), (int)d.size(), &w, &h, &n, 4);
    if (!px) {
        applog::add(std::string(what) + ": cannot decode image: " + stbi_failure_reason());
        w = h = 0;
        return false;
    }
    int tw = w, th = h;
    std::vector<uint8_t> small;
    const uint8_t *data = px;
    if (maxSide > 0 && std::max(w, h) > maxSide + maxSide / 2) {
        small = shrinkRgba(px, w, h, maxSide, tw, th);
        data = small.data();
    }
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(px);
    return true;
}

// Decode a new slide into the texture (on the GUI thread)
static void updateSlide(App &a)
{
    if (a.snap.slideVersion == a.slide.version) return;
    a.slide.version = a.snap.slideVersion;
    a.slide.slide = a.snap.slide;
    a.slide.w = a.slide.h = 0;
    if (a.snap.slide) loadTexture(a.snap.slide->data, a.slide.tex, a.slide.w, a.slide.h, "SLS");
}

// Make textures for new or changed SPI logos
static void updateLogos(App &a)
{
    if (a.snap.logosVersion == a.logosVersion) return;
    a.logosVersion = a.snap.logosVersion;
    if (a.snap.ensembleLogo && a.snap.ensembleLogo != a.ensembleLogo.smallSrc) {
        int w, h;
        a.ensembleLogo.smallSrc = a.snap.ensembleLogo;
        loadTexture(*a.snap.ensembleLogo, a.ensembleLogo.logo32, w, h, "SPI ensemble logo",
                    (int)lroundf(32 * ImGui::GetStyle().FontScaleDpi));
    }
    for (const auto &l : a.snap.logos) {
        LogoTextures &t = a.logos[l.first];
        int w, h;
        if (l.second.logo32 && l.second.logo32 != t.smallSrc) {
            t.smallSrc = l.second.logo32;
            // the list icon may be a larger square (e.g. 128x128): shrink it to its display size
            loadTexture(*l.second.logo32, t.logo32, w, h, "SPI logo", (int)lroundf(32 * ImGui::GetStyle().FontScaleDpi));
        }
        if (l.second.logo320 && l.second.logo320 != t.largeSrc) {
            t.largeSrc = l.second.logo320;
            if (!loadTexture(*l.second.logo320, t.logo320, t.largeW, t.largeH, "SPI logo")) t.largeW = t.largeH = 0;
        }
    }
}

// Flag for an ISO country code, decoded from the embedded PNGs on first use
static const FlagTexture *flagTexture(App &a, const char *iso)
{
    auto it = a.flags.find(iso);
    if (it != a.flags.end()) return it->second.tex ? &it->second : nullptr;
    FlagTexture &f = a.flags[iso];
    for (const auto &k : kFlags) {
        if (strcmp(k.iso, iso) != 0) continue;
        std::vector<uint8_t> png(k.data, k.data + k.size);
        // drawn about one line high, so shrink the 40 px high images to match
        if (!loadTexture(png, f.tex, f.w, f.h, "flag", (int)lroundf(32 * ImGui::GetStyle().FontScaleDpi))) f.tex = 0;
        break;
    }
    return f.tex ? &f : nullptr;
}

// Put an image (JPEG/PNG/GIF/BMP file contents) on the system clipboard.
// Windows: as a DIB (understood by every application) plus "PNG" (lossless,
// keeps transparency). Linux: as image/png through wl-copy or xclip.
static bool copyImageToClipboard(App &a, const std::vector<uint8_t> &file, std::string &err)
{
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
    if (!px) {
        err = "cannot decode the image";
        return false;
    }
    std::vector<uint8_t> png;
    if (file.size() > 8 && file[0] == 0x89 && file[1] == 'P' && file[2] == 'N' && file[3] == 'G') {
        png = file;
    } else {
        stbi_write_png_to_func([](void *ctx, void *data, int size) {
            auto *v = (std::vector<uint8_t> *)ctx;
            v->insert(v->end(), (uint8_t *)data, (uint8_t *)data + size);
        }, &png, w, h, 4, px, w * 4);
    }
#ifdef _WIN32
    // CF_DIB: BITMAPINFOHEADER + bottom-up BGRX rows; transparency composited onto white
    const size_t pixBytes = (size_t)w * h * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixBytes);
    HGLOBAL pngMem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    bool ok = dib && pngMem;
    if (ok) {
        auto *bi = (BITMAPINFOHEADER *)GlobalLock(dib);
        memset(bi, 0, sizeof(*bi));
        bi->biSize = sizeof(*bi);
        bi->biWidth = w;
        bi->biHeight = h;   // positive: bottom-up
        bi->biPlanes = 1;
        bi->biBitCount = 32;
        bi->biCompression = BI_RGB;
        uint8_t *dst = (uint8_t *)(bi + 1);
        for (int y = 0; y < h; y++) {
            const uint8_t *s = px + (size_t)(h - 1 - y) * w * 4;
            uint8_t *d = dst + (size_t)y * w * 4;
            for (int x = 0; x < w; x++, s += 4, d += 4) {
                const int al = s[3];
                d[0] = (uint8_t)((s[2] * al + 255 * (255 - al)) / 255);
                d[1] = (uint8_t)((s[1] * al + 255 * (255 - al)) / 255);
                d[2] = (uint8_t)((s[0] * al + 255 * (255 - al)) / 255);
                d[3] = 255;
            }
        }
        GlobalUnlock(dib);
        memcpy(GlobalLock(pngMem), png.data(), png.size());
        GlobalUnlock(pngMem);
        ok = OpenClipboard(glfwGetWin32Window(a.window)) != 0;
    }
    if (ok) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, dib) != nullptr;   // the clipboard owns it now
        if (ok) dib = nullptr;
        if (SetClipboardData(RegisterClipboardFormatA("PNG"), pngMem)) pngMem = nullptr;
        CloseClipboard();
    }
    if (dib) GlobalFree(dib);
    if (pngMem) GlobalFree(pngMem);
    stbi_image_free(px);
    if (!ok) err = "the clipboard is not available";
    return ok;
#else
    (void)a;
    stbi_image_free(px);
    const bool wayland = getenv("WAYLAND_DISPLAY") != nullptr;
    FILE *p = popen(wayland ? "wl-copy --type image/png 2>/dev/null" : "xclip -selection clipboard -t image/png -i 2>/dev/null", "w");
    bool ok = p && fwrite(png.data(), 1, png.size(), p) == png.size();
    if (p && pclose(p) != 0) ok = false;
    if (!ok) err = wayland ? "copying images needs wl-copy (package wl-clipboard)" : "copying images needs xclip";
    return ok;
#endif
}

static void showToast(App &a, const std::string &msg)
{
    a.toast = msg;
    a.toastTime = now();
}

static const LogoTextures *findLogos(const App &a, int sid)
{
    auto it = a.logos.find((uint16_t)sid);
    return it == a.logos.end() ? nullptr : &it->second;
}

// Copy what the slideshow area shows: the slide, or else the station's 320x240 logo
static void copyShownImage(App &a)
{
    const LogoTextures *lt = a.snap.selectedSid >= 0 ? findLogos(a, a.snap.selectedSid) : nullptr;
    const std::vector<uint8_t> *img = a.snap.slide ? &a.snap.slide->data
                                      : lt && lt->largeSrc ? lt->largeSrc.get() : nullptr;
    if (!img) return;
    std::string err;
    if (copyImageToClipboard(a, *img, err)) showToast(a, "Image copied to the clipboard");
    else showToast(a, "Copy failed: " + err);
}

static void pollPlayer(App &a)
{
    a.snap = a.player.snapshot();
    updateSlide(a);
    updateLogos(a);

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

// Hand pointer over clickable items (disabled items don't report hovering)
static void handCursor()
{
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

enum Icon { IconPlay, IconStop };

static bool iconButton(const char *id, Icon icon, float size, ImU32 bg, ImU32 fg, const char *tip)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    handCursor();
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
    handCursor();
    if (ImGui::IsItemHovered() && !running) ImGui::SetTooltip("Recent sources");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("recent")) {
        if (a.recent.empty()) ImGui::TextDisabled("No recent sources");
        for (const auto &r : a.recent) {
            if (ImGui::Selectable(r.c_str())) snprintf(a.url, sizeof(a.url), "%s", r.c_str());
            handCursor();
        }
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

// "ROAD TRAFFIC, NEWS" style list of the announcement types in `flags`
static std::string announcementList(uint16_t flags, bool upper)
{
    std::string s;
    for (int b = 0; b < 16; b++) {
        if (!(flags & (1 << b))) continue;
        std::string n = Player::announcementName(b);
        if (upper) for (auto &c : n) c = (char)toupper((unsigned char)c);
        s += (s.empty() ? "" : ", ") + n;
    }
    return s;
}

static int g_forceServiceTip = -1;   // test hook EDIPLAYER_SERVICE_TIP

// Service list tooltip: identity (with the service's own country), format, PTY, announcements
static void drawServiceTooltip(App &a, const ServiceEntry &e, const std::string &name)
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.c_str());

    ImGui::Text("SId %04X", e.sid);
    if (e.ecc >= 0) {
        ImGui::SameLine(0, ImGui::GetFontSize());
        ImGui::Text("ECC %02X", e.ecc);
        // the country comes from the service's ECC and the first digit of its SId
        if (const Country *c = dabCountry((uint8_t)e.ecc, e.sid)) {
            if (const FlagTexture *f = flagTexture(a, c->iso)) {
                const float h = ImGui::GetFontSize() * 0.9f;
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - h) * 0.5f);
                ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)f->tex), ImVec2(h * f->w / f->h, h));
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(c->name);
        }
    }
    ImGui::SameLine(0, ImGui::GetFontSize());
    ImGui::Text("SubChId %d", e.scid);

    ImGui::Text("%s, %d kbit/s%s%s", e.dabplus ? "DAB+ (HE-AAC)" : "DAB (MPEG Layer II)", e.kbps,
                e.protection.empty() ? "" : ", ", e.protection.c_str());
    ImGui::Text("Programme type: %s", e.pty > 0 ? Player::ptyName(e.pty) : e.pty == 0 ? "none" : "unknown");
    if (e.asu) ImGui::Text("Announcements: %s", announcementList(e.asu, false).c_str());
    if (e.announcing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.45f, 1.0f));
        ImGui::Text("Now: %s announcement", announcementList(e.announcing, false).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
}

// "SPI  12 / 33" with a thin bar while the SPI carousel loads
static void drawSpiProgress(App &a)
{
    const SpiProgress &pr = a.snap.spi;
    if (!a.player.running() || pr.carousels == 0) return;

    const bool done = pr.directory && pr.objects > 0 && pr.complete >= pr.objects;
    char txt[64];
    if (!pr.directory) snprintf(txt, sizeof(txt), "SPI  waiting for directory");
    else if (done) snprintf(txt, sizeof(txt), "SPI  %d files loaded", pr.objects);
    else snprintf(txt, sizeof(txt), "SPI  %d / %d", pr.complete, pr.objects);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(txt);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Service and Programme Information: station logos.\nFiles are saved to\n%s",
                          a.snap.spiFolder.empty() ? "(waiting for the ensemble ID)" : a.snap.spiFolder.c_str());
    if (done) return;

    // progress bar (moving segment until the directory has arrived)
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float h = 3.0f * ImGui::GetStyle().FontScaleDpi;
    const float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(1, 1, 1, 0.07f), h * 0.5f);
    const ImU32 c = ImGui::GetColorU32(kAccent);
    if (pr.directory && pr.objects > 0) {
        const float f = (float)pr.complete / pr.objects;
        if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + std::max(h, w * f), p.y + h), c, h * 0.5f);
    } else {
        const float seg = w * 0.25f;
        const float x = (float)fmod(now() * 0.6, 1.0) * (w + seg) - seg;
        dl->AddRectFilled(ImVec2(p.x + std::max(0.0f, x), p.y), ImVec2(p.x + std::min(w, x + seg), p.y + h), c, h * 0.5f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

static void drawServices(App &a, float width)
{
    beginPanel("services", ImVec2(width, 0));
    const PlayerSnapshot &s = a.snap;

    // ensemble header, with the country flag on the right
    const Country *country = s.ecc >= 0 && !s.services.empty() ? dabCountry((uint8_t)s.ecc, s.eid) : nullptr;
    const FlagTexture *flag = country ? flagTexture(a, country->iso) : nullptr;
    const float flagH = ImGui::GetFontSize() * 0.95f;
    const float flagW = flag ? flagH * flag->w / flag->h : 0;
    std::string ens = s.ensemble.empty() ? "Services" : s.ensemble;
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    const float hw = ImGui::GetContentRegionAvail().x;

    // ensemble logo (SPI) to the left of the name and the "N services" line
    const bool ensLogo = a.ensembleLogo.logo32 != 0 && !s.services.empty();
    const float ensScale = ImGui::GetStyle().FontScaleDpi;
    const float ensLogoSz = 32 * ensScale;
    const float ensIndent = ensLogo ? ensLogoSz + ImGui::GetStyle().ItemSpacing.x * 1.2f : 0;
    if (ensLogo) {
        const float linesH = ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 lp(hp.x, hp.y + (linesH - ensLogoSz) * 0.5f);
        ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)a.ensembleLogo.logo32), lp,
                                                    ImVec2(lp.x + ensLogoSz, lp.y + ensLogoSz), ImVec2(0, 0), ImVec2(1, 1),
                                                    IM_COL32_WHITE, 4.0f * ensScale);
        ImGui::Indent(ensIndent);
    }
    ImGui::TextUnformatted(fitText(ens, hw - ensIndent - (flag ? flagW + ImGui::GetStyle().ItemSpacing.x : 0)).c_str());
    if (flag) {
        const ImVec2 fp(hp.x + hw - flagW, hp.y + (ImGui::GetTextLineHeight() - flagH) * 0.5f);
        ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)flag->tex), fp,
                                                    ImVec2(fp.x + flagW, fp.y + flagH), ImVec2(0, 0), ImVec2(1, 1),
                                                    IM_COL32_WHITE, 2.0f * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::IsMouseHoveringRect(fp, ImVec2(fp.x + flagW, fp.y + flagH)))
            ImGui::SetTooltip("%s\nECC %02X, country id %X", country->name, s.ecc, s.eid >> 12);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    char sub[96] = "";
    if (!s.services.empty()) {
        snprintf(sub, sizeof(sub), "%zu service%s   EId %04X", s.services.size(), s.services.size() == 1 ? "" : "s",
                 s.eid);
        if (s.ecc >= 0) snprintf(sub + strlen(sub), sizeof(sub) - strlen(sub), "   ECC %02X", s.ecc);
    }
    ImGui::TextUnformatted(sub[0] ? sub : (a.player.running() ? "Waiting for the FIC..." : "Not receiving"));
    ImGui::PopStyleColor();
    if (ensLogo) ImGui::Unindent(ensIndent);
    drawSpiProgress(a);
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
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float rowH = fs * 2.0f + ImGui::GetStyle().FramePadding.y * 2;
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float logoSz = std::min(32 * scale, rowH - 6 * scale);   // SPI 32x32 logo
    const float logoX = padX * 0.7f;
    const float textX = logoX + logoSz + padX * 0.8f;
    for (const auto &e : s.services) {
        ImGui::PushID(e.sid);
        const bool sel = e.sid == s.selectedSid;
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("row", ImVec2(w, rowH))) {
            a.scrollToSelected = false;
            if (!sel) playService(a, e.sid);
        }
        if (!sel) handCursor();
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
        // logo square: the SPI 32x32 logo, or an empty placeholder until it arrives
        const ImVec2 lp(p.x + logoX, p.y + (rowH - logoSz) * 0.5f);
        const LogoTextures *lt = findLogos(a, e.sid);
        if (lt && lt->logo32)
            dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)lt->logo32), lp, ImVec2(lp.x + logoSz, lp.y + logoSz),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f * scale);
        else
            dl->AddRectFilled(lp, ImVec2(lp.x + logoSz, lp.y + logoSz), col(1, 1, 1, 0.05f), 4.0f * scale);

        const float ty = p.y + (rowH - fs * 2.05f) * 0.5f;
        // details in a smaller font: the SId after the name, and a second line with
        // type, bitrate, protection, programme type
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
        const float sfs = ImGui::GetFontSize();
        const ImU32 detailCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        char sidHex[8];
        snprintf(sidHex, sizeof(sidHex), "%04X", e.sid);
        const bool showSid = !e.label.empty();   // otherwise the name already is "SId XXXX"
        const float sidGap = fs * 0.45f;
        const float sidW = showSid ? ImGui::CalcTextSize(sidHex).x + sidGap : 0;
        ImGui::PopFont();

        const std::string shownName = fitText(name, w - textX - padX - barsW - sidW);
        dl->AddText(ImVec2(p.x + textX, ty), ImGui::GetColorU32(ImGuiCol_Text), shownName.c_str());
        const float nameW = ImGui::CalcTextSize(shownName.c_str()).x;

        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
        if (showSid)   // on the name's baseline
            dl->AddText(ImGui::GetFont(), sfs, ImVec2(p.x + textX + nameW + sidGap, ty + (fs - sfs) * 0.78f), detailCol, sidHex);
        std::string info = std::string(e.dabplus ? "DAB+" : "DAB") + " \xC2\xB7 " + std::to_string(e.kbps) + " kbit/s";
        if (!e.protection.empty()) info += " \xC2\xB7 " + e.protection;
        if (e.pty > 0) info += std::string(" \xC2\xB7 ") + Player::ptyName(e.pty);
        dl->AddText(ImGui::GetFont(), sfs, ImVec2(p.x + textX, ty + fs * 1.12f), detailCol,
                    fitText(info, w - textX - padX - barsW).c_str());
        ImGui::PopFont();
        if (sel)
            drawPlayingBars(dl, ImVec2(p.x + w - padX - fs * 0.85f, p.y + (rowH - fs) * 0.5f), fs,
                            std::max(a.audio.peakLeft(), a.audio.peakRight()), s.audioOk && a.player.running(),
                            ImGui::GetColorU32(kAccent));
        if (hov || e.sid == g_forceServiceTip) drawServiceTooltip(a, e, name);
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

static ImFont *g_boldFont = nullptr;   // loaded in loadFont(); nullptr if none was found

// Record pill geometry: compact, sized to sit on a line of text; the label
// is small bold capitals
struct PillMetrics {
    float h, icon, padX, gap, textSize;
    ImFont *font;
    PillMetrics()
    {
        const float fs = ImGui::GetFontSize();
        h = fs * 1.3f;
        icon = fs * 0.5f;
        padX = fs * 0.45f;
        gap = fs * 0.3f;
        textSize = fs * 0.8f;
        font = g_boldFont ? g_boldFont : ImGui::GetFont();
    }
    float textWidth(const char *t) const { return font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, t).x; }
};

static void pillText(char *txt, size_t n, const char *label, const Recorder::State &st)
{
    if (!st.active) {
        snprintf(txt, n, "%s", label);
        return;
    }
    int t = (int)st.seconds;
    if (t >= 3600) snprintf(txt, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(txt, n, "%02d:%02d", t / 60, t % 60);
}

static float pillWidth(const char *txt)
{
    PillMetrics m;
    // fits "00:00" so the pill doesn't change width when it starts
    const float textW = std::max(m.textWidth("00:00"), m.textWidth(txt));
    return m.padX + m.icon + m.gap + textW + m.padX;
}

// Pill button: round red record dot + label, or (while recording) a stop
// square + elapsed time on a red tint
static bool recordPill(const char *id, const char *label, const Recorder::State &st, bool enabled, const char *tip)
{
    const PillMetrics m;
    const float h = m.h, icon = m.icon, padX = m.padX;
    char txt[32];
    pillText(txt, sizeof(txt), label, st);
    const float w = pillWidth(txt);

    ImGui::BeginDisabled(!enabled && !st.active);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    handCursor();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec4 bg = st.active ? ImVec4(0.80f, 0.22f, 0.22f, 0.30f) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    if (hov) { bg.x *= 1.2f; bg.y *= 1.2f; bg.z *= 1.2f; }
    if (act) { bg.x *= 0.85f; bg.y *= 0.85f; bg.z *= 0.85f; }
    if (!enabled && !st.active) bg.w *= 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), h * 0.5f);

    const ImVec2 c(p.x + padX + icon * 0.5f, p.y + h * 0.5f);
    const ImU32 red = (!enabled && !st.active) ? col(0.55f, 0.30f, 0.30f) : col(0.92f, 0.26f, 0.24f);
    if (st.active) {
        // stop square, gently pulsing so a running recording is obvious
        const float pulse = 0.75f + 0.25f * (float)sin(now() * 4.0);
        const float s = icon * 0.42f;
        dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s),
                          ImGui::GetColorU32(ImVec4(0.92f, 0.26f, 0.24f, pulse)), s * 0.3f);
    } else {
        dl->AddCircleFilled(c, icon * 0.45f, red, 24);
    }
    const ImU32 tc = ImGui::GetColorU32((!enabled && !st.active) ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    const ImVec2 tp(p.x + padX + icon + m.gap, p.y + (h - m.textSize) * 0.5f);
    dl->AddText(m.font, m.textSize, tp, tc, txt);
    if (!g_boldFont) dl->AddText(m.font, m.textSize, ImVec2(tp.x + 0.7f, tp.y), tc, txt);   // poor man's bold
    ImGui::EndDisabled();
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

// Width of the two record pills, for laying them out after the codec text
static float recorderWidth(const PlayerSnapshot &s)
{
    char t0[32], t1[32];
    pillText(t0, sizeof(t0), "RAW", s.rec[Recorder::Raw]);
    pillText(t1, sizeof(t1), "WAV", s.rec[Recorder::Wav]);
    return pillWidth(t0) + ImGui::GetStyle().ItemSpacing.x * 0.6f + pillWidth(t1);
}

static void drawRecorder(App &a, const ServiceEntry *svc)
{
    const PlayerSnapshot &s = a.snap;
    const bool can = a.player.running() && svc;
    const char *rawExt = svc && svc->dabplus ? ".aac (LATM)" : ".mp2";
    const Recorder::Kind kinds[2] = {Recorder::Raw, Recorder::Wav};
    const char *labels[2] = {"RAW", "WAV"};
    const float y = ImGui::GetCursorPosY();   // SameLine would realign the second pill to the text
    for (int i = 0; i < 2; i++) {
        const Recorder::State &st = s.rec[kinds[i]];
        std::string tip;
        if (st.active) tip = "Recording to\n" + st.file + "\nClick to stop";
        else if (!can) tip = "Play a service to record it";
        else if (i == 0) tip = std::string("Record the broadcast audio unchanged (") + rawExt + ") into EDIplayer_audio";
        else tip = "Record the decoded audio as a WAV file into EDIplayer_audio";
        if (i) {
            ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 0.6f);
            ImGui::SetCursorPosY(y);
        }
        if (recordPill(i ? "##recwav" : "##recraw", labels[i], st, can, tip.c_str())) {
            if (st.active) {
                a.player.stopRecording(kinds[i]);
            } else {
                std::string err;
                if (!a.player.startRecording(kinds[i], err)) a.message = err;
                else a.message.clear();
            }
        }
    }
}

static bool g_forceRateTooltip = false;   // test hook EDIPLAYER_RATES

// Dotted underline under the bitrate text (r0-r1; hints that it has a
// tooltip), and the live bitrate breakdown when hovered
static void drawRateTooltip(const PlayerSnapshot &s, const ServiceEntry *svc, ImVec2 r0, ImVec2 r1, bool hovered)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 c = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.6f);
    const float dot = std::max(1.0f, ImGui::GetStyle().FontScaleDpi);
    for (float x = r0.x; x < r1.x; x += dot * 3) dl->AddRectFilled(ImVec2(x, r1.y), ImVec2(x + dot, r1.y + dot), c);

    if (!hovered && !g_forceRateTooltip) return;
    const PlayerSnapshot::Rates &r = s.rates;
    ImGui::SetNextWindowPos(ImVec2(r0.x, r1.y + ImGui::GetStyle().ItemSpacing.y));   // just below the bitrate
    ImGui::BeginTooltip();
    if (!r.valid) {
        ImGui::TextUnformatted("Measuring...");
        ImGui::EndTooltip();
        return;
    }
    ImGui::TextUnformatted("Bitrate breakdown (last ~4 s)");
    ImGui::Separator();
    auto pct = [&](double v) { return r.useful > 0 ? 100.0 * v / r.useful : 0.0; };
    auto row = [&](const char *name, double kbps, double percent, const char *note) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        char b[32];
        snprintf(b, sizeof(b), "%.1f kbit/s", kbps);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(b).x);
        ImGui::TextUnformatted(b);
        ImGui::TableSetColumnIndex(2);
        if (percent >= 0) {
            snprintf(b, sizeof(b), "%.1f %%", percent);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(b).x);
            ImGui::TextDisabled("%s", b);
        }
        ImGui::TableSetColumnIndex(3);
        if (note) ImGui::TextDisabled("%s", note);
    };
    if (ImGui::BeginTable("rates", 4, ImGuiTableFlags_SizingFixedFit)) {
        // number columns sized up front so right-aligned values aren't clipped on the first frame
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                                std::max(ImGui::CalcTextSize("Sub-channel").x, ImGui::CalcTextSize("  Overhead").x));
        ImGui::TableSetupColumn("kbps", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000.0 kbit/s").x);
        ImGui::TableSetupColumn("pct", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("100.0 %").x);
        ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthFixed);
        std::string sub = svc && !svc->protection.empty() ? svc->protection : "";
        row("Sub-channel", r.subchannel, -1, sub.empty() ? nullptr : sub.c_str());
        row("Useful", r.useful, 100.0, r.dabplus ? "after Reed-Solomon (110/120)" : "no outer FEC in DAB (MP2)");
        row("  Audio", r.audio, pct(r.audio), r.dabplus ? "AAC access units" : "MPEG frames incl. header/CRC");
        row("  PAD", r.pad, pct(r.pad), r.dabplus ? "DLS, slideshow... (DSE)" : "F-PAD + X-PAD in use");
        if (r.dabplus) row("  Overhead", r.overhead, pct(r.overhead), "superframe header, AU CRCs, padding");
        ImGui::EndTable();
    }
    ImGui::EndTooltip();
}

// Small pill for services with announcement support (FIG 0/18): plain while
// idle, green while an announcement is signalled (FIG 0/19)
static void drawAnnouncementPill(const ServiceEntry &e)
{
    const bool on = e.announcing != 0;
    const PillMetrics m;
    const float size = m.textSize * 0.9f;
    std::string label = announcementList(on ? e.announcing : e.asu, true);
    if (!on && std::count(label.begin(), label.end(), ',') >= 2) label = "ANNOUNCEMENTS";
    const float tw = m.font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str()).x;
    const float padX = size * 0.6f, h = size * 1.45f;
    const ImVec2 p(ImGui::GetCursorScreenPos().x,
                   ImGui::GetCursorScreenPos().y + (ImGui::GetTextLineHeight() * 0.85f - h) * 0.5f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + tw + padX * 2, p.y + h);
    if (on) {
        // a gentle pulse so an active announcement is noticed
        const float k = 0.85f + 0.15f * (float)sin(now() * 3.0);
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(0.18f * k, 0.62f * k, 0.32f * k, 1.0f)), h * 0.5f);
        dl->AddText(m.font, size, ImVec2(p.x + padX, p.y + (h - size) * 0.5f), col(1, 1, 1), label.c_str());
    } else {
        dl->AddRect(p, q, col(1, 1, 1, 0.18f), h * 0.5f, 0, std::max(1.0f, ImGui::GetStyle().FontScaleDpi));
        dl->AddText(m.font, size, ImVec2(p.x + padX, p.y + (h - size) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
    }
    ImGui::Dummy(ImVec2(q.x - p.x, ImGui::GetTextLineHeight() * 0.85f));
    if (ImGui::IsItemHovered()) {
        if (on)
            ImGui::SetTooltip("%s announcement in progress (sub-channel %d)\nSupported: %s",
                              announcementList(e.announcing, false).c_str(), e.announceSubch,
                              announcementList(e.asu, false).c_str());
        else
            ImGui::SetTooltip("Announcements supported: %s\nLights up green while one is signalled",
                              announcementList(e.asu, false).c_str());
    }
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
    // three tightly spaced lines: name, "SId D3A3 · Pop Music", codec + record pills
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0f * scale));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
    std::string title = svc && !svc->label.empty() ? svc->label : s.selectedSid >= 0 ? "Tuning..." : "No service selected";
    ImGui::TextUnformatted(fitText(title, textW).c_str());
    ImGui::PopFont();
    if (svc) {
        char ident[64];
        snprintf(ident, sizeof(ident), "SId %04X", svc->sid);
        std::string line = ident;
        if (svc->pty > 0) line += std::string(" \xC2\xB7 ") + Player::ptyName(svc->pty);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(fitText(line, textW).c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (svc->asu) {
            ImGui::SameLine(0, ImGui::GetFontSize() * 0.6f);
            drawAnnouncementPill(*svc);
        }
    }
    // codec line: "<format>  -  <bitrate>  -  buffering"; the bitrate is its own
    // item with a live breakdown tooltip
    std::string det, rate, tail;
    if (s.selectedSid >= 0 && a.player.running()) {
        if (!s.audioFormat.empty()) det = s.audioFormat;
        else if (svc) det = svc->dabplus ? "DAB+" : "DAB";
        if (svc) rate = std::to_string(svc->kbps) + " kbit/s";
        if (!s.audioOk) tail = "  -  buffering";
    } else if (s.selectedSid < 0) {
        det = "Choose a service from the list";
    }
    // record pills go to the right of it, centred on the text
    const float recW = recorderWidth(s);
    const float recGap = ImGui::GetStyle().ItemSpacing.x * 1.5f;
    const std::string sep = rate.empty() ? "" : "  -  ";
    const float fixedW = ImGui::CalcTextSize((sep + rate + tail).c_str()).x;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(fitText(det, std::max(0.0f, textW - recW - recGap - fixedW)).c_str());
    ImVec2 rateMin, rateMax;
    bool rateHovered = false;
    if (!rate.empty()) {
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(sep.c_str());
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(rate.c_str());
        rateMin = ImGui::GetItemRectMin();
        rateMax = ImGui::GetItemRectMax();
        rateHovered = ImGui::IsItemHovered();
    }
    if (!tail.empty()) {
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(tail.c_str());
    }
    ImGui::PopStyleColor();
    if (!rate.empty()) drawRateTooltip(s, svc, rateMin, rateMax, rateHovered);
    ImGui::SameLine(0, det.empty() ? 0 : recGap);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (PillMetrics().h - ImGui::GetTextLineHeight()) * 0.5f);
    drawRecorder(a, svc);
    ImGui::PopStyleVar();
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
    // an SLS slide, or else the service's 320x240 SPI logo
    const LogoTextures *lt = s.selectedSid >= 0 ? findLogos(a, s.selectedSid) : nullptr;
    const bool haveSlide = a.slide.tex && a.slide.w > 0 && s.slide;
    const bool haveLogo = !haveSlide && lt && lt->logo320 && lt->largeW > 0;
    if (haveSlide || haveLogo) {
        const GLuint tex = haveSlide ? a.slide.tex : lt->logo320;
        const float iw = (float)(haveSlide ? a.slide.w : lt->largeW), ih = (float)(haveSlide ? a.slide.h : lt->largeH);
        float k = std::min(box.x / iw, box.y / ih);
        ImVec2 sz(iw * k, ih * k);
        ImVec2 o(p.x + (box.x - sz.x) * 0.5f, p.y + (box.y - sz.y) * 0.5f);
        dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)tex), o, ImVec2(o.x + sz.x, o.y + sz.y),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6.0f);
    } else {
        const char *t = s.selectedSid >= 0 ? "No slideshow" : "";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p.x + (box.x - ts.x) * 0.5f, p.y + (box.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    }
    ImGui::InvisibleButton("slide", box, ImGuiButtonFlags_MouseButtonRight);
    const bool showTip = ImGui::IsItemHovered() && !ImGui::IsPopupOpen("slidemenu");
    if (showTip && haveLogo)
        ImGui::SetTooltip("Station logo (SPI)\nShown until the station sends a slideshow image\nRight-click to copy");
    if (showTip && haveSlide) {
        const Slide &sl = *s.slide;
        std::string tip = sl.name.empty() ? "Slide" : sl.name;
        tip += "\n" + std::to_string(a.slide.w) + " x " + std::to_string(a.slide.h) + "  " + sl.ext + ", " +
               std::to_string(sl.data.size() / 1024) + " kB";
        if (!sl.categoryTitle.empty()) tip += "\nCategory: " + sl.categoryTitle;
        if (!sl.clickUrl.empty()) tip += "\n" + sl.clickUrl;
        tip += "\nRight-click to copy";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    if ((haveSlide || haveLogo) && ImGui::BeginPopupContextItem("slidemenu")) {
        if (ImGui::MenuItem("Copy image")) copyShownImage(a);
        handCursor();
        ImGui::EndPopup();
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
    if (now() - a.toastTime < 3.0) {
        t = a.toast;
    } else if (!a.message.empty()) {
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
    handCursor();
    logPopup(a);
}

// ---------------------------------------------------------------------------
// Test hooks (environment variables, unused in normal operation):
//   EDIPLAYER_URL=<source>            use this source instead of the saved one
//   EDIPLAYER_AUTOSTART=1             start receiving at launch
//   EDIPLAYER_SID=<hex>               play this service
//   EDIPLAYER_SHOTS=<sec>:<file.png>,<sec>:<file.png>...   framebuffer captures
//   EDIPLAYER_EXIT=<sec>              quit after <sec>
//   EDIPLAYER_RECORD=<sec>            start raw + wav recording after <sec>
//   EDIPLAYER_RATES=1                 keep the bitrate breakdown tooltip open
//   EDIPLAYER_COPY=<sec>              copy the shown slide / logo to the clipboard after <sec>
//   EDIPLAYER_SERVICE_TIP=<hex>       keep this service's list tooltip open

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
    double exitAt = -1, recordAt = -1, copyAt = -1;
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
        if (const char *r = getenv("EDIPLAYER_RECORD")) recordAt = atof(r);
        g_forceRateTooltip = getenv("EDIPLAYER_RATES") != nullptr;
        if (const char *c = getenv("EDIPLAYER_COPY")) copyAt = atof(c);
        if (const char *t = getenv("EDIPLAYER_SERVICE_TIP")) g_forceServiceTip = (int)strtol(t, nullptr, 16);
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

static ImFont *addFirstFont(const char *const *candidates, size_t n)
{
    ImGuiIO &io = ImGui::GetIO();
    for (size_t i = 0; i < n; i++) {
        FILE *t = fopen(candidates[i], "rb");
        if (!t) continue;
        fclose(t);
        if (ImFont *f = io.Fonts->AddFontFromFileTTF(candidates[i])) return f;
    }
    return nullptr;
}

static void loadFont()
{
    static const char *regular[] = {
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
    static const char *bold[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeuib.ttf",
        "C:\\Windows\\Fonts\\arialbd.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
#endif
    };
    // the first font added is the default
    if (!addFirstFont(regular, sizeof(regular) / sizeof(regular[0]))) ImGui::GetIO().Fonts->AddFontDefault();
    g_boldFont = addFirstFont(bold, sizeof(bold) / sizeof(bold[0]));
    ImGui::GetStyle().FontSizeBase = 16.0f;
}

// ---------------------------------------------------------------------------

int main(int, char **)
{
    App a;
    a.settingsPath = settingsFile();
    loadSettings(a);
    a.audio.setVolume(a.volume);
    a.player.setSpiBaseDir(spiBaseDir());
    a.player.setRecordBaseDir(spiBaseDir());   // EDIplayer_audio/ next to EDIplayer_SPI/

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow *win = glfwCreateWindow((int)(940 * scale), (int)(610 * scale), "EDIplayer", nullptr, nullptr);
    if (!win) return 1;
    glfwSetWindowSizeLimits(win, (int)(720 * scale), (int)(520 * scale), GLFW_DONT_CARE, GLFW_DONT_CARE);
#ifndef _WIN32
    // window icon (on Windows it comes from the GLFW_ICON resource)
    {
        std::vector<GLFWimage> icons;
        for (const auto &ic : kIconPngs) {
            GLFWimage im;
            int n;
            im.pixels = stbi_load_from_memory(ic.data, (int)ic.size, &im.width, &im.height, &n, 4);
            if (im.pixels) icons.push_back(im);
        }
        if (!icons.empty()) glfwSetWindowIcon(win, (int)icons.size(), icons.data());
        for (auto &im : icons) stbi_image_free(im.pixels);
    }
#endif
    glfwMakeContextCurrent(win);
    a.window = win;
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
        if (th.copyAt >= 0 && now() - t0 >= th.copyAt) {
            copyShownImage(a);
            th.copyAt = -1;
        }
        if (th.recordAt >= 0 && now() - t0 >= th.recordAt) {
            std::string err;
            a.player.startRecording(Recorder::Raw, err);
            a.player.startRecording(Recorder::Wav, err);
            th.recordAt = -1;
        }

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
