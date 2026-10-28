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
#include <ctime>
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
    struct Favourite {
        std::string url, label;
        int sid = -1;   // service last played from it
    };
    std::vector<Favourite> favourites;

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
        else if (k == "favourite") {   // <url> TAB <label> [TAB <SId hex>]
            std::vector<std::string> parts;
            std::stringstream ss(v);
            for (std::string p; std::getline(ss, p, '\t');) parts.push_back(p);
            if (parts.empty() || parts[0].empty()) continue;
            App::Favourite fav;
            fav.url = parts[0];
            fav.label = parts.size() > 1 && !parts[1].empty() ? parts[1] : parts[0];
            if (parts.size() > 2 && !parts[2].empty()) fav.sid = (int)strtol(parts[2].c_str(), nullptr, 16);
            a.favourites.push_back(fav);
        }
    }
}

static void saveSettings(const App &a)
{
    std::ofstream f(std::filesystem::u8path(a.settingsPath));
    if (!f) return;
    char sid[16] = "";
    if (a.lastSid >= 0) snprintf(sid, sizeof(sid), "%04X", a.lastSid);
    f << "url=" << a.url << "\n" << "volume=" << a.volume << "\n" << "sid=" << sid << "\n";
    for (const auto &fav : a.favourites) {
        char fsid[16] = "";
        if (fav.sid >= 0) snprintf(fsid, sizeof(fsid), "%04X", fav.sid);
        f << "favourite=" << fav.url << "\t" << fav.label << "\t" << fsid << "\n";
    }
}

static App::Favourite *findFavourite(App &a, const std::string &url)
{
    for (auto &f : a.favourites)
        if (f.url == url) return &f;
    return nullptr;
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
    if (App::Favourite *f = findFavourite(a, a.url)) f->sid = sid;
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
static ImFont *g_boldFont = nullptr;   // loaded in loadFont(); nullptr if none was found

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

static void drawStar(ImDrawList *dl, ImVec2 c, float r, ImU32 colour, bool filled)
{
    const float pi = 3.14159265f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        const float ang = -pi * 0.5f + i * pi / 5, rr = i % 2 ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr);
    }
    if (filled) dl->AddConcavePolyFilled(pts, 10, colour);
    else dl->AddPolyline(pts, 10, colour, ImDrawFlags_Closed, 1.3f * ImGui::GetStyle().FontScaleDpi);
}

static std::string g_openSourcePopup;   // test hook EDIPLAYER_POPUP: "favourites" or "favedit"

// Receive a favourite, resuming the service last played from it
static void switchSource(App &a, const std::string &url)
{
    if (url != a.url) {
        const App::Favourite *f = findFavourite(a, url);
        a.lastSid = f ? f->sid : -1;
    }
    snprintf(a.url, sizeof(a.url), "%s", url.c_str());
    if (a.player.running()) a.player.stop();
    startPlayer(a);
}

// Star next to the address: outlined, or gold if the address is a favourite.
// Opens a small editor for the favourite's label.
static void drawFavouriteStar(App &a, float size)
{
    static char label[128] = "";
    App::Favourite *fav = findFavourite(a, a.url);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!a.url[0]);
    const bool hook = g_openSourcePopup == "favedit";
    if (ImGui::Button("##star", ImVec2(size, size)) || hook) {
        if (hook) g_openSourcePopup.clear();
        // the default label: the saved one, else the ensemble being received from this address
        std::string def = fav ? fav->label : a.url;
        if (!fav && a.player.running() && !a.snap.ensemble.empty()) def = a.snap.ensemble;
        snprintf(label, sizeof(label), "%s", def.c_str());
        ImGui::OpenPopup("favedit");
    }
    handCursor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Edit favourite" : "Save as favourite");
    const ImVec2 anchor(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y * 0.5f);
    ImGui::EndDisabled();
    const ImU32 gold = col(0.98f, 0.76f, 0.20f);
    drawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.3f,
             fav ? gold : ImGui::GetColorU32(a.url[0] ? ImGuiCol_Text : ImGuiCol_TextDisabled), fav != nullptr);

    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, ImVec2(1, 0));   // drops down from the star
    if (!ImGui::BeginPopup("favedit")) return;
    ImGui::TextDisabled(fav ? "Favourite" : "Save as favourite");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    const bool enter = ImGui::InputTextWithHint("##label", "Label", label, sizeof(label), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("%s", a.url);
    if (ImGui::Button(fav ? "Save" : "Add") || enter) {
        const std::string l = label[0] ? label : a.url;
        if (fav) fav->label = l;
        else a.favourites.push_back({a.url, l, a.player.running() ? a.snap.selectedSid : -1});
        saveSettings(a);
        ImGui::CloseCurrentPopup();
    }
    handCursor();
    if (fav) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            const std::string url = a.url;
            a.favourites.erase(std::remove_if(a.favourites.begin(), a.favourites.end(),
                                              [&](const App::Favourite &f) { return f.url == url; }),
                               a.favourites.end());
            saveSettings(a);
            ImGui::CloseCurrentPopup();
        }
        handCursor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    handCursor();
    ImGui::EndPopup();
}

// The favourites dropdown: label and address on one line, x to remove on hover
static void drawFavouritesList(App &a, ImVec2 anchor)
{
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, ImVec2(1, 0));   // drops down from the arrow
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22, 0), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup("favourites")) return;
    if (a.favourites.empty()) {
        ImGui::TextDisabled("No favourites yet.");
        ImGui::TextDisabled("Use the star next to the address to save one.");
    }
    int remove = -1;
    const float fs = ImGui::GetFontSize();
    for (size_t i = 0; i < a.favourites.size(); i++) {
        const App::Favourite &f = a.favourites[i];
        ImGui::PushID((int)i);
        const bool current = f.url == a.url;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::max(ImGui::GetContentRegionAvail().x, fs * 22);
        const float xw = ImGui::GetFrameHeight();   // remove button
        if (ImGui::Selectable("##fav", current, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, 0))) {
            if (!current || !a.player.running()) switchSource(a, f.url);
        }
        handCursor();
        const bool hov = ImGui::IsItemHovered();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float labelW = std::min(ImGui::CalcTextSize(f.label.c_str()).x, w * 0.55f);
        dl->AddText(p, ImGui::GetColorU32(ImGuiCol_Text), fitText(f.label, w * 0.55f).c_str());
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
        const float sfs = ImGui::GetFontSize();
        dl->AddText(ImVec2(p.x + labelW + fs * 0.8f, p.y + (fs - sfs) * 0.75f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    fitText(f.url, w - labelW - fs * 0.8f - xw).c_str());
        ImGui::PopFont();
        if (hov || ImGui::IsMouseHoveringRect(ImVec2(p.x + w - xw, p.y), ImVec2(p.x + w, p.y + fs))) {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - xw, p.y));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            if (ImGui::Button("x", ImVec2(xw, fs))) remove = (int)i;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            handCursor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        a.favourites.erase(a.favourites.begin() + remove);
        saveSettings(a);
    }
    ImGui::EndPopup();
}

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

    // URL, favourite star, favourites list
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh * 2 - btn - st.ItemSpacing.x * 3);
    bool enter = ImGui::InputTextWithHint("##url", "tcp://host:port   udp://239.1.2.3:5004   udp://5004", a.url,
                                          sizeof(a.url), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("EDI source\n"
                          "  tcp://host:port            connect to an EDI TCP server\n"
                          "  udp://port                 receive unicast UDP\n"
                          "  udp://group:port           join a multicast group\n"
                          "  udp://group:port?iface=ip  ... on a specific interface");
    ImGui::EndDisabled();
    ImGui::SameLine();
    drawFavouriteStar(a, fh);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##favourites", ImGuiDir_Down) || g_openSourcePopup == "favourites") {
        g_openSourcePopup.clear();
        ImGui::OpenPopup("favourites");
    }
    handCursor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Favourites");
    drawFavouritesList(a, ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + st.ItemSpacing.y * 0.5f));

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
static bool g_openMuxInfo = false;   // set by the info button, opened at the main window level
static float g_muxInfoScroll = -1;   // test hook: scroll the ensemble information to this position

// Small round "i" button drawn at p (no layout), returns true when clicked
static bool infoButton(ImVec2 p, float sz, const char *tip)
{
    const ImVec2 q(p.x + sz, p.y + sz), c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    const bool hov = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, q);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 fg = ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (hov) dl->AddCircleFilled(c, sz * 0.5f, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 24);
    dl->AddCircle(c, sz * 0.5f - 0.5f, fg, 24, 1.2f);
    ImFont *f = g_boldFont ? g_boldFont : ImGui::GetFont();
    const float ts = sz * 0.78f;
    const ImVec2 tsz = f->CalcTextSizeA(ts, FLT_MAX, 0, "i");
    dl->AddText(f, ts, ImVec2(c.x - tsz.x * 0.5f, c.y - tsz.y * 0.5f), fg, "i");
    if (!hov) return false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("%s", tip);
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

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

static bool g_openEpg = false;   // set by the EPG button, opened at the main window level

// Small "EPG" pill that opens the programme guide
static bool epgButton()
{
    ImFont *f = g_boldFont ? g_boldFont : ImGui::GetFont();
    const float ts = ImGui::GetFontSize() * 0.72f;
    const ImVec2 tsz = f->CalcTextSizeA(ts, FLT_MAX, 0, "EPG");
    const float padX = ts * 0.55f, h = ts * 1.45f, lineH = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (lineH - h) * 0.5f;
    ImGui::SetCursorScreenPos(p);
    const bool pressed = ImGui::InvisibleButton("##epg", ImVec2(tsz.x + padX * 2, h));
    handCursor();
    const bool hov = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + tsz.x + padX * 2, p.y + h);
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, hov ? 1.0f : 0.8f)), h * 0.3f);
    dl->AddText(f, ts, ImVec2(p.x + padX, p.y + (h - tsz.y) * 0.5f), IM_COL32_WHITE, "EPG");
    if (hov) ImGui::SetTooltip("Programme guide");
    return pressed;
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
        ImGui::SetTooltip("Service and Programme Information: station logos and programme guide.\nFiles are saved to\n%s",
                          a.snap.spiFolder.empty() ? "(waiting for the ensemble ID)" : a.snap.spiFolder.c_str());
    if (a.snap.epg && !a.snap.epg->empty()) {
        ImGui::SameLine(0, ImGui::GetFontSize() * 0.6f);
        if (epgButton()) g_openEpg = true;
    }
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
    // ensemble information button, below the flag
    if (s.mux && !s.services.empty()) {
        const float sz = flagH;
        const float cx = flag ? hp.x + hw - flagW * 0.5f : hp.x + hw - sz * 0.5f;
        const float y = ImGui::GetCursorScreenPos().y + (ImGui::GetTextLineHeight() - sz) * 0.5f;
        if (infoButton(ImVec2(cx - sz * 0.5f, y), sz, "Ensemble information")) g_openMuxInfo = true;
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

// ---------------------------------------------------------------------------
// Ensemble information: capacity map and everything from the FIC

static const int kTotalCus = 864;   // capacity units per CIF (all transmission modes)

static std::string hex(uint32_t v, int digits)
{
    char b[16];
    snprintf(b, sizeof(b), "%0*X", digits, v);
    return b;
}

static const char *userAppName(int t)   // ETSI TS 101 756 table 16
{
    switch (t) {
    case 0x002: return "Slideshow";
    case 0x003: return "Broadcast Web Site";
    case 0x004: return "TPEG";
    case 0x005: return "DGPS";
    case 0x006: return "TMC";
    case 0x007: return "SPI";
    case 0x008: return "DAB Java";
    case 0x009: return "DMB";
    case 0x00A: return "IPDC services";
    case 0x00B: return "Voice applications";
    case 0x00C: return "Middleware";
    case 0x00D: return "Filecasting";
    case 0x44A: return "Journaline";
    default: return nullptr;
    }
}

static std::string dscTyName(int t)   // ETSI TS 101 756 table 2b
{
    switch (t) {
    case 0: return "unspecified data";
    case 1: return "TMC";
    case 2: return "EWS";
    case 3: return "ITTS";
    case 4: return "paging";
    case 5: return "TDC";
    case 24: return "MPEG-2 TS";
    case 59: return "IP";
    case 60: return "MOT";
    case 61: return "proprietary";
    default: return "DSCTy " + std::to_string(t);
    }
}

static const char *languageName(int l)   // ETSI TS 101 756 table 9 (European languages)
{
    static const char *const names[] = {
        "unknown", "Albanian", "Breton", "Catalan", "Croatian", "Welsh", "Czech", "Danish", "German", "English",
        "Spanish", "Esperanto", "Estonian", "Basque", "Faroese", "French", "Frisian", "Irish", "Gaelic", "Galician",
        "Icelandic", "Italian", "Sami", "Latin", "Latvian", "Luxembourgish", "Lithuanian", "Hungarian", "Maltese",
        "Dutch", "Norwegian", "Occitan", "Polish", "Portuguese", "Romanian", "Romansh", "Serbian", "Slovak",
        "Slovene", "Finnish", "Swedish", "Turkish", "Flemish", "Walloon",
    };
    return l >= 0 && l < (int)(sizeof(names) / sizeof(names[0])) ? names[l] : nullptr;
}

static std::string language(int l)
{
    const char *n = languageName(l);
    return n ? n : "language 0x" + hex(l, 2);
}

static const char *charsetName(int c)
{
    return c == 0 ? "EBU Latin" : c == 6 ? "UCS-2" : c == 15 ? "UTF-8" : "other";
}

static std::string protectionText(const fic_subch_t &sc)
{
    char b[24];
    if (sc.eep) snprintf(b, sizeof(b), "EEP-%d%c", sc.level, sc.eep_b ? 'B' : 'A');
    else snprintf(b, sizeof(b), "UEP-%d", sc.level);
    return b;
}

// "2026-10-27" from a Modified Julian Date (EN 300 468 annex C)
static std::string mjdDate(uint32_t mjd)
{
    const int yp = (int)((mjd - 15078.2) / 365.25);
    const int mp = (int)((mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001);
    const int d = (int)mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);
    const int k = mp == 14 || mp == 15;
    char b[40];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", yp + k + 1900, mp - 1 - k * 12, d);
    return b;
}

static std::string serviceName(const fic_service_t &s)
{
    return s.has_label && s.label[0] ? s.label : "SId " + hex(s.sid, s.pd ? 8 : 4);
}

static const fic_packet_comp_t *packetComp(const fic_state_t &f, uint16_t scid)
{
    for (int k = 0; k < f.n_packet; k++)
        if (f.packet[k].scid == scid) return &f.packet[k];
    return nullptr;
}

// Sub-channel of a component, -1 if none (FIDC) or not yet known
static int compSubch(const fic_state_t &f, const fic_svc_comp_t &c)
{
    if (c.tmid == 0 || c.tmid == 1) return c.subch;
    if (c.tmid == 3)
        if (const fic_packet_comp_t *p = packetComp(f, c.scid)) return p->subch;
    return -1;
}

// SCIdS of a component (0 for the primary one; secondary ones from FIG 0/8), -1 unknown
static int compScids(const fic_state_t &f, const fic_service_t &s, const fic_svc_comp_t &c)
{
    if (c.primary) return 0;
    for (int k = 0; k < f.n_defs; k++) {
        const fic_comp_def_t &d = f.defs[k];
        if (d.sid != s.sid) continue;
        if (c.tmid == 3 ? d.long_form && d.scid == c.scid : !d.long_form && d.subch == c.subch) return d.scids;
    }
    return -1;
}

// One sub-channel of the capacity map
struct SubchUse {
    int id = 0;
    fic_subch_t sc{};
    std::string kind;                     // "DAB+", "DAB", "packet data", ...
    std::vector<std::string> users;       // services using it
    ImU32 color = 0;
};

static std::vector<SubchUse> subchannelUses(const MuxInfo &m)
{
    const fic_state_t &f = m.fic;
    std::vector<SubchUse> out;
    for (int id = 0; id < 64; id++) {
        if (!f.subch[id].valid || !f.subch[id].size) continue;
        // FIG 0/1 can still describe sub-channels removed by a reconfiguration:
        // keep those the EDI stream carries
        bool present = m.streams.empty();
        for (const auto &st : m.streams) present |= st.subch == id;
        if (!present) continue;
        SubchUse u;
        u.id = id;
        u.sc = f.subch[id];
        for (int i = 0; i < f.n_services; i++) {
            const fic_service_t &s = f.services[i];
            for (int c = 0; c < s.n_comps; c++) {
                const fic_svc_comp_t &comp = s.comps[c];
                if (compSubch(f, comp) != id) continue;
                const std::string name = serviceName(s);
                if (std::find(u.users.begin(), u.users.end(), name) == u.users.end())   // primary users first
                    u.users.insert(comp.primary ? u.users.begin() : u.users.end(), name);
                if (u.kind.empty() || comp.primary)
                    u.kind = comp.tmid == 0 ? (comp.type == ASCTY_DABPLUS ? "DAB+" : comp.type == ASCTY_MP2 ? "DAB" : "audio")
                             : comp.tmid == 3 ? "packet data"
                                              : "stream data";
            }
        }
        out.push_back(u);
    }
    std::sort(out.begin(), out.end(), [](const SubchUse &a, const SubchUse &b) { return a.sc.start < b.sc.start; });
    static const ImVec4 palette[] = {
        {0.27f, 0.52f, 0.96f, 1}, {0.13f, 0.72f, 0.50f, 1}, {0.96f, 0.62f, 0.07f, 1}, {0.96f, 0.30f, 0.40f, 1},
        {0.07f, 0.71f, 0.85f, 1}, {0.40f, 0.40f, 0.95f, 1}, {0.52f, 0.80f, 0.10f, 1}, {0.93f, 0.29f, 0.60f, 1},
        {0.98f, 0.45f, 0.09f, 1}, {0.08f, 0.72f, 0.65f, 1}, {0.85f, 0.27f, 0.94f, 1}, {0.60f, 0.45f, 0.95f, 1},
    };
    for (size_t i = 0; i < out.size(); i++)
        out[i].color = ImGui::GetColorU32(palette[i % (sizeof(palette) / sizeof(palette[0]))]);
    return out;
}

static std::string joined(const std::vector<std::string> &v, const char *sep = ", ")
{
    std::string s;
    for (const auto &x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

static void panelTitle(const char *title, const char *sub)
{
    if (g_boldFont) ImGui::PushFont(g_boldFont, 0.0f);
    ImGui::TextUnformatted(title);
    if (g_boldFont) ImGui::PopFont();
    if (sub) {
        ImGui::SameLine(0, ImGui::GetFontSize() * 0.6f);
        ImGui::TextDisabled("%s", sub);
    }
}

// Capacity map: the sub-channels in order of their start address, like a disk map
static void drawCapacity(const MuxInfo &m)
{
    static int hovered = -1;   // sub-channel under the mouse (bar or table), from the last frame
    int hoverNow = -1;
    const std::vector<SubchUse> uses = subchannelUses(m);
    int used = 0;
    for (const auto &u : uses) used += u.sc.size;

    beginPanel("capacity", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float fs = ImGui::GetFontSize();
    const float scale = ImGui::GetStyle().FontScaleDpi;

    // title, and "862 / 864" on the right
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImFont *bf = g_boldFont ? g_boldFont : ImGui::GetFont();
    char usedTxt[16], totalTxt[16];
    snprintf(usedTxt, sizeof(usedTxt), "%d", used);
    snprintf(totalTxt, sizeof(totalTxt), " / %d", kTotalCus);
    ImGui::PushFont(bf, ImGui::GetStyle().FontSizeBase * 1.9f);
    const float bigPx = ImGui::GetFontSize();
    const ImVec2 usedSz = ImGui::CalcTextSize(usedTxt);
    const float bigAscent = bf->GetFontBaked(bigPx)->Ascent;
    ImGui::PopFont();
    const ImVec2 totalSz = ImGui::CalcTextSize(totalTxt);
    const float smallAscent = ImGui::GetFont()->GetFontBaked(fs)->Ascent;
    const float rightX = tp.x + ImGui::GetContentRegionAvail().x;
    const float titleH = usedSz.y;
    ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + (titleH - fs) * 0.5f));
    panelTitle("Capacity units", "sub-channels by start address (FIG 0/1)");
    const ImU32 green = col(0.25f, 0.82f, 0.58f);
    dl->AddText(bf, bigPx, ImVec2(rightX - totalSz.x - usedSz.x, tp.y), green, usedTxt);
    dl->AddText(ImVec2(rightX - totalSz.x, tp.y + bigAscent - smallAscent), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                totalTxt);   // on the same baseline
    ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + titleH + ImGui::GetStyle().ItemSpacing.y * 0.6f));

    // the bar; free capacity hatched
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x, bh = fs * 1.15f, r = bh * 0.35f;
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), col(1, 1, 1, 0.05f), r);
    dl->PushClipRect(bp, ImVec2(bp.x + bw, bp.y + bh), true);
    for (float x = -bh; x < bw; x += 6 * scale)
        dl->AddLine(ImVec2(bp.x + x, bp.y + bh), ImVec2(bp.x + x + bh, bp.y), col(1, 1, 1, 0.06f), 1.0f);
    dl->PopClipRect();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool barHovered = ImGui::IsWindowHovered() && mouse.y >= bp.y && mouse.y < bp.y + bh;
    for (const auto &u : uses) {
        const float x0 = bp.x + bw * u.sc.start / kTotalCus;
        const float x1 = bp.x + bw * (u.sc.start + u.sc.size) / kTotalCus;
        ImDrawFlags rf = ImDrawFlags_RoundCornersNone;
        if (u.sc.start == 0) rf |= ImDrawFlags_RoundCornersLeft;
        if (u.sc.start + u.sc.size >= kTotalCus) rf |= ImDrawFlags_RoundCornersRight;
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(u.color);
        if (hovered >= 0 && hovered != u.id) {   // fade the others towards the background (opaque: hides the hatching)
            const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
            c = ImVec4(bg.x + (c.x - bg.x) * 0.35f, bg.y + (c.y - bg.y) * 0.35f, bg.z + (c.z - bg.z) * 0.35f, 1);
        }
        dl->AddRectFilled(ImVec2(x0, bp.y), ImVec2(std::max(x0 + 1, x1 - 1), bp.y + bh), ImGui::GetColorU32(c), r, rf);
        if (barHovered && mouse.x >= x0 && mouse.x < x1) hoverNow = u.id;
    }
    ImGui::Dummy(ImVec2(bw, bh));

    // scale below the bar
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.78f);
    {
        const float sfs = ImGui::GetFontSize();
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const float y = sp.y - ImGui::GetStyle().ItemSpacing.y * 0.6f;
        for (int t = 0; t <= kTotalCus; t += kTotalCus / 4) {
            char b[8];
            snprintf(b, sizeof(b), "%d", t);
            const float tw = ImGui::CalcTextSize(b).x;
            float x = bp.x + bw * t / kTotalCus - (t == 0 ? 0 : t == kTotalCus ? tw : tw * 0.5f);
            dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
        }
        ImGui::Dummy(ImVec2(bw, sfs - ImGui::GetStyle().ItemSpacing.y * 0.6f));
    }
    ImGui::PopFont();

    // one row per sub-channel, in the same order and colours
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
    if (!uses.empty() && ImGui::BeginTable("subch", 9, tf)) {
        ImGui::TableSetupColumn("SubCh");
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("End");
        ImGui::TableSetupColumn("CUs");
        ImGui::TableSetupColumn("Share");
        ImGui::TableSetupColumn("Protection");
        ImGui::TableSetupColumn("Bit rate");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthStretch);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (const auto &u : uses) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            const float sw = ImGui::GetFontSize() * 0.7f;
            ImGui::PushID(u.id);
            ImGui::Selectable("##row", hovered == u.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            if (ImGui::IsItemHovered()) hoverNow = u.id;
            ImGui::PopID();
            const float th = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x, cp.y + (th - sw) * 0.5f),
                                                      ImVec2(cp.x + sw, cp.y + (th + sw) * 0.5f), u.color, 2 * scale);
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(cp.x + sw + fs * 0.4f, cp.y));
            ImGui::Text("%d", u.id);
            ImGui::TableNextColumn();
            ImGui::Text("%d", u.sc.start);
            ImGui::TableNextColumn();
            ImGui::Text("%d", u.sc.start + u.sc.size - 1);
            ImGui::TableNextColumn();
            ImGui::Text("%d", u.sc.size);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %%", 100.0 * u.sc.size / kTotalCus);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(protectionText(u.sc).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d kbit/s", u.sc.kbps);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u.kind.empty() ? "-" : u.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u.users.empty() ? "(not used by a service)" : joined(u.users).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    if (uses.empty()) ImGui::TextDisabled("Waiting for the sub-channel organisation (FIG 0/1)...");
    else if (used < kTotalCus) ImGui::TextDisabled("%d CUs free (%.1f %%)", kTotalCus - used, 100.0 * (kTotalCus - used) / kTotalCus);
    endPanel();

    // tooltip for the bar
    if (hoverNow >= 0 && barHovered)
        for (const auto &u : uses) {
            if (u.id != hoverNow) continue;
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(u.users.empty() ? "(not used by a service)" : joined(u.users).c_str());
            ImGui::TextDisabled("Sub-channel %d   CUs %d-%d (%d)", u.id, u.sc.start, u.sc.start + u.sc.size - 1, u.sc.size);
            ImGui::TextDisabled("%s, %d kbit/s%s%s", protectionText(u.sc).c_str(), u.sc.kbps, u.kind.empty() ? "" : ", ",
                                u.kind.c_str());
            ImGui::EndTooltip();
        }
    hovered = hoverNow;
}

// Key / value pairs, two per row
static void drawPairs(const char *id, const std::vector<std::pair<std::string, std::string>> &items)
{
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) return;
    // the same key width everywhere, so all the tables line up
    const float kw = ImGui::CalcTextSize("Announcement clusters").x;
    ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed, kw);
    ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("k2", ImGuiTableColumnFlags_WidthFixed, kw);
    ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < items.size(); i++) {
        if (i % 2 == 0) ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", items[i].first.c_str());
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(items[i].second.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
}

static void drawEnsembleInfo(const MuxInfo &m)
{
    const fic_state_t &f = m.fic;
    beginPanel("ensemble", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    panelTitle("Ensemble", "FIG 0/0, 0/7, 0/9, 0/10, 1/0 and the EDI frame (DETI)");
    ImGui::Spacing();
    std::vector<std::pair<std::string, std::string>> it;
    char b[160];

    it.push_back({"Label", f.has_ens_label ? f.ens_label : "-"});
    it.push_back({"Short label", f.has_ens_label ? f.ens_short_label : "-"});
    it.push_back({"EId", hex(f.eid, 4)});
    std::string cc = f.has_ecc ? hex(f.ecc, 2) : "-";
    if (f.has_ecc)
        if (const Country *c = dabCountry(f.ecc, f.eid)) cc += std::string("  ") + c->name;
    it.push_back({"ECC", cc});
    it.push_back({"Character set", f.has_ens_label ? charsetName(f.ens_charset) : "-"});
    if (f.has_ecc) {
        const int lto = f.lto * 30;
        snprintf(b, sizeof(b), "UTC%c%02d:%02d", lto < 0 ? '-' : '+', abs(lto) / 60, abs(lto) % 60);
        it.push_back({"Local time offset", b});
        it.push_back({"International table", f.inter_table_id == 1 ? "1 (Europe: RDS PTYs)"
                                             : f.inter_table_id == 2 ? "2 (North America: RBDS PTYs)"
                                                                     : std::to_string(f.inter_table_id)});
    }
    if (f.has_time) {
        snprintf(b, sizeof(b), "%s %02d:%02d", mjdDate(f.mjd).c_str(), f.hour, f.minute);
        std::string t = b;
        if (f.utc_long) {
            snprintf(b, sizeof(b), ":%02d.%03d", f.second, f.ms);
            t += b;
        }
        it.push_back({"Date and time", t + " UTC" + (f.lsi ? " (leap second)" : "")});
    } else {
        it.push_back({"Date and time", "not signalled (FIG 0/10)"});
    }
    if (m.mode >= 0) {
        static const char *const modes[4] = {"IV", "I", "II", "III"};
        it.push_back({"Transmission mode", modes[m.mode & 3]});
    }
    if (m.ficLen) {
        snprintf(b, sizeof(b), "%d bytes / 24 ms (%d FIBs)", m.ficLen, m.ficLen / 32);
        it.push_back({"FIC", b});
    }
    if (f.has_fig0_0) {
        it.push_back({"CIF count", std::to_string(f.cif_count)});
        static const char *const changes[4] = {"none", "sub-channels", "services", "sub-channels and services"};
        std::string ch = changes[f.change_flags & 3];
        if (f.has_occurrence_change) ch += " at CIF " + std::to_string(f.occurrence_change);
        it.push_back({"Change announced", ch});
        it.push_back({"Alarm announcements", f.al_flag ? "enabled (AL flag)" : "not enabled"});
    }
    if (f.has_fig0_7) {
        snprintf(b, sizeof(b), "%d services, reconfiguration %d", f.cfg_services, f.reconfig_count);
        it.push_back({"Configuration", b});
    }
    int prog = 0, data = 0;
    for (int i = 0; i < f.n_services; i++) (f.services[i].pd ? data : prog)++;
    snprintf(b, sizeof(b), "%d programme, %d data", prog, data);
    it.push_back({"Services", b});
    int nsub = 0, used = 0;
    for (const auto &u : subchannelUses(m)) {
        nsub++;
        used += u.sc.size;
    }
    snprintf(b, sizeof(b), "%d (%d of %d CUs used)", nsub, used, kTotalCus);
    it.push_back({"Sub-channels", b});
    it.push_back({"EDI frame counter", std::to_string(m.dflc)});
    if (m.atst && !m.seconds) {
        it.push_back({"EDI timestamp", "flagged but not set (zero)"});
    } else if (m.atst) {
        // TS 102 693: Seconds counts from 2000-01-01 00:00 UTC on the TAI time scale
        // less 32 s; UTCO is the remaining TAI-UTC offset (TAI-UTC - 32 s)
        const time_t t = (time_t)946684800 + (time_t)m.seconds - m.utco;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S UTC", &tm);
        it.push_back({"EDI timestamp", b});
    }
    snprintf(b, sizeof(b), "%llu (%.0f s)", (unsigned long long)m.frames, m.frames * 0.024);
    it.push_back({"Frames received", b});

    // announcement clusters (FIG 0/18 / 0/19)
    std::vector<int> clusters;
    for (int i = 0; i < f.n_svcs; i++)
        for (int k = 0; k < f.svcs[i].n_clusters; k++)
            if (std::find(clusters.begin(), clusters.end(), f.svcs[i].clusters[k]) == clusters.end())
                clusters.push_back(f.svcs[i].clusters[k]);
    std::sort(clusters.begin(), clusters.end());
    std::string cl;
    for (int c : clusters) {
        const fic_cluster_t &k = f.clusters[c];
        cl += (cl.empty() ? "" : ";  ") + std::to_string(c);
        if (k.asw && f.fic_count - k.seen <= 84)
            cl += ": " + announcementList(k.asw, false) + " on SubCh " + std::to_string(k.subch);
    }
    it.push_back({"Announcement clusters", clusters.empty() ? "none" : cl});
    drawPairs("enspairs", it);

    // which FIGs have been received, wrapped between entries
    ImGui::Spacing();
    ImGui::TextDisabled("FIGs received (count)");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetFontSize() * 1.1f;
    bool any = false;
    for (int t = 0; t < 8; t++)
        for (int e = 0; e < 32; e++) {
            if (!f.fig_count[t][e]) continue;
            snprintf(b, sizeof(b), "%d/%d (%u)", t, e, f.fig_count[t][e]);
            if (any) {
                ImGui::SameLine(0, gap);
                if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(b).x > right) ImGui::NewLine();
            }
            ImGui::TextUnformatted(b);
            any = true;
        }
    if (!any) ImGui::TextUnformatted("-");
    ImGui::PopFont();
    endPanel();
}

// One line per component: type, sub-channel and its place in the multiplex, user applications
static std::string componentText(const fic_state_t &f, const fic_service_t &s, const fic_svc_comp_t &c)
{
    std::string t;
    char b[160];
    if (c.tmid == 0)
        t = c.type == ASCTY_DABPLUS ? "DAB+ audio (HE-AAC)" : c.type == ASCTY_MP2 ? "DAB audio (MPEG Layer II)"
                                                                                : "audio, ASCTy " + std::to_string(c.type);
    else if (c.tmid == 1)
        t = "stream data (" + dscTyName(c.type) + ")";
    else if (c.tmid == 2)
        t = "FIDC data (" + dscTyName(c.type) + "), FIDCId " + std::to_string(c.subch);
    else {
        const fic_packet_comp_t *p = packetComp(f, c.scid);
        t = "packet data";
        if (p) {
            snprintf(b, sizeof(b), " (%s), SCId %d, packet address %d%s", dscTyName(p->dscty).c_str(), c.scid, p->address,
                     p->dg ? ", no data groups" : "");
            t += b;
        } else {
            t += ", SCId " + std::to_string(c.scid);
        }
    }
    const int sc = compSubch(f, c);
    if (sc >= 0) {
        const fic_subch_t &x = f.subch[sc];
        if (x.valid)
            snprintf(b, sizeof(b), "\nSubCh %d   CUs %d-%d (%d)   %s   %d kbit/s", sc, x.start, x.start + x.size - 1, x.size,
                     protectionText(x).c_str(), x.kbps);
        else
            snprintf(b, sizeof(b), "\nSubCh %d", sc);
        t += b;
        if (x.has_fec && x.fec) t += "   FEC (RS 204,188)";
        if (x.has_lang && c.tmid != 3) t += "   " + language(x.lang);
    }
    if (c.tmid == 3)
        for (int k = 0; k < f.n_comp_langs; k++)
            if (f.comp_langs[k].scid == c.scid) t += "   " + language(f.comp_langs[k].lang);
    if (c.ca) t += "   conditional access";

    // user applications (FIG 0/13) of this component
    const int scids = compScids(f, s, c);
    std::vector<std::string> apps;
    for (int k = 0; k < f.n_uas && scids >= 0; k++) {
        if (f.uas[k].sid != s.sid || f.uas[k].scids != scids) continue;
        const char *n = userAppName(f.uas[k].type);
        apps.push_back(n ? n : "app 0x" + hex(f.uas[k].type, 3));
    }
    if (!apps.empty()) t += "\nApplications: " + joined(apps);
    return t;
}

static void drawServiceDetails(App &a, const MuxInfo &m)
{
    const fic_state_t &f = m.fic;
    beginPanel("svcdetails", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    panelTitle("Services", "FIG 0/2, 0/3, 0/5, 0/8, 0/9, 0/13, 0/14, 0/17, 0/18, 1/1, 1/5");

    // programme services first, then data services; each by label
    std::vector<const fic_service_t *> list;
    for (int i = 0; i < f.n_services; i++)
        if (f.services[i].n_comps) list.push_back(&f.services[i]);
    std::sort(list.begin(), list.end(), [](const fic_service_t *x, const fic_service_t *y) {
        if (x->pd != y->pd) return !x->pd;
        std::string lx = serviceName(*x), ly = serviceName(*y);
        for (auto &c : lx) c = (char)tolower((unsigned char)c);
        for (auto &c : ly) c = (char)tolower((unsigned char)c);
        return lx != ly ? lx < ly : x->sid < y->sid;
    });
    if (list.empty()) ImGui::TextDisabled("Waiting for the service organisation (FIG 0/2)...");

    const float fs = ImGui::GetFontSize();
    for (const fic_service_t *sp : list) {
        const fic_service_t &s = *sp;
        const svc_info_t *info = nullptr;
        for (int i = 0; i < f.n_svcs && !s.pd; i++)
            if (f.svcs[i].sid == s.sid) info = &f.svcs[i];
        ImGui::PushID(sp);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // name, short label; SId, ECC and flag on the right
        if (g_boldFont) ImGui::PushFont(g_boldFont, 0.0f);
        ImGui::TextUnformatted(serviceName(s).c_str());
        if (g_boldFont) ImGui::PopFont();
        if (s.has_label && s.short_label[0]) {
            ImGui::SameLine(0, fs * 0.6f);
            ImGui::TextDisabled("%s", s.short_label);
        }
        const int ecc = info && info->has_ecc ? info->ecc : f.has_ecc ? f.ecc : -1;
        std::string right = "SId " + hex(s.sid, s.pd ? 8 : 4);
        if (ecc >= 0) right += "   ECC " + hex(ecc, 2);
        const Country *country = ecc >= 0 && !s.pd ? dabCountry((uint8_t)ecc, (uint16_t)s.sid) : nullptr;
        const FlagTexture *flag = country ? flagTexture(a, country->iso) : nullptr;
        const float flagH = fs * 0.85f, flagW = flag ? flagH * flag->w / flag->h : 0;
        const float rw = ImGui::CalcTextSize(right.c_str()).x + (flag ? flagW + fs * 0.4f : 0);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - rw));
        ImGui::TextDisabled("%s", right.c_str());
        if (flag) {
            ImGui::SameLine(0, fs * 0.4f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - flagH) * 0.5f);
            ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)flag->tex), ImVec2(flagW, flagH));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", country->name);
        }

        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
        ImGui::Indent(fs * 0.8f);
        std::vector<std::pair<std::string, std::string>> it;
        it.push_back({"Type", s.pd ? "data service" : "programme service"});
        if (info && info->has_pty)
            it.push_back({"Programme type", std::string(info->pty ? Player::ptyName(info->pty) : "none") +
                                                (info->pty_dynamic ? " (dynamic)" : " (static)")});
        if (country) it.push_back({"Country", country->name});
        if (info && info->has_ecc) it.push_back({"Own ECC", hex(info->ecc, 2) + " (FIG 0/9 extended)"});
        it.push_back({"Label charset", s.has_label ? charsetName(s.charset) : "-"});
        it.push_back({"Local service", s.local ? "yes" : "no"});
        if (s.caid) it.push_back({"Conditional access", "CAId " + std::to_string(s.caid)});
        if (info && info->asu) {
            std::string an = announcementList(info->asu, false);
            std::vector<std::string> cs;
            for (int k = 0; k < info->n_clusters; k++) cs.push_back(std::to_string(info->clusters[k]));
            if (!cs.empty()) an += " (cluster " + joined(cs) + ")";
            it.push_back({"Announcements", an});
            int sub = -1;
            const uint16_t active = fic_active_announcements(&f, info, &sub);
            if (active) it.push_back({"Announcing now", announcementList(active, false) + " on SubCh " + std::to_string(sub)});
        }
        drawPairs("svcpairs", it);

        for (int c = 0; c < s.n_comps; c++) {
            const fic_svc_comp_t &comp = s.comps[c];
            const int scids = compScids(f, s, comp);
            char head[40];
            if (comp.primary) snprintf(head, sizeof(head), "Primary component");
            else if (scids >= 0) snprintf(head, sizeof(head), "Component %d", scids);
            else snprintf(head, sizeof(head), "Secondary component");
            ImGui::TextDisabled("%s", head);
            ImGui::Indent(fs * 0.8f);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(componentText(f, s, comp).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(fs * 0.8f);
        }
        ImGui::Unindent(fs * 0.8f);
        ImGui::PopFont();
        ImGui::PopID();
    }
    endPanel();
}

static void muxInfoPopup(App &a)
{
    if (g_openMuxInfo) {
        ImGui::OpenPopup("Ensemble information");
        g_openMuxInfo = false;
    }
    const ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vs.x * 0.94f, vs.y * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(vs.x * 0.5f, vs.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Ensemble information", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (g_muxInfoScroll >= 0 && ImGui::GetScrollMaxY() > 0) {
        ImGui::SetScrollY(g_muxInfoScroll);
        g_muxInfoScroll = -1;
    }
    const std::shared_ptr<const MuxInfo> m = a.snap.mux;
    if (!m) {
        ImGui::TextDisabled("No multiplex information yet.");
    } else {
        drawCapacity(*m);
        ImGui::Spacing();
        drawEnsembleInfo(*m);
        ImGui::Spacing();
        drawServiceDetails(a, *m);
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Programme guide (EPG from SPI): services one above the other, time running across

static std::tm localTm(int64_t t)
{
    const time_t tt = (time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

// Local midnight at the start of the day containing t, plus `days` days
static int64_t localMidnight(int64_t t, int days = 0)
{
    std::tm tm = localTm(t);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static std::string hhmm(int64_t t)
{
    const std::tm tm = localTm(t);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

static const char *programmeTitle(const EpgProgramme &p)
{
    if (!p.longName.empty()) return p.longName.c_str();
    if (!p.mediumName.empty()) return p.mediumName.c_str();
    return p.shortName.c_str();
}

static void epgPopup(App &a)
{
    static int64_t day = 0;          // local midnight of the day shown
    static bool scrollToNow = false;
    const int64_t nowT = (int64_t)time(nullptr);
    if (g_openEpg) {
        ImGui::OpenPopup("Programme guide");
        g_openEpg = false;
        day = localMidnight(nowT);
        scrollToNow = true;
    }
    const ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vs.x * 0.96f, vs.y * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(vs.x * 0.5f, vs.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Programme guide", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    const std::shared_ptr<const EpgSchedule> epg = a.snap.epg;
    if (!epg || epg->empty()) {
        ImGui::TextDisabled("No programme information.");
        ImGui::EndPopup();
        return;
    }

    // rows: services in list order, then any others the EPG covers
    std::vector<uint16_t> rows;
    for (const auto &e : a.snap.services)
        if (epg->count(e.sid)) rows.push_back(e.sid);
    for (const auto &s : *epg)
        if (std::find(rows.begin(), rows.end(), s.first) == rows.end()) rows.push_back(s.first);

    // days with programmes
    std::vector<int64_t> days;
    for (const auto &s : *epg)
        for (const auto &p : s.second) {
            const int64_t d = localMidnight(p.start);
            if (std::find(days.begin(), days.end(), d) == days.end()) days.push_back(d);
        }
    std::sort(days.begin(), days.end());
    if (std::find(days.begin(), days.end(), day) == days.end()) {   // nearest day with data
        auto it = std::lower_bound(days.begin(), days.end(), day);
        day = it == days.end() ? days.back() : *it;
    }
    const int64_t dayEnd = localMidnight(day, 1);
    const auto dayIt = std::find(days.begin(), days.end(), day);

    // day selector
    ImGui::BeginDisabled(dayIt == days.begin());
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) day = *(dayIt - 1);
    handCursor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        const std::tm tm = localTm(day);
        char b[64];
        strftime(b, sizeof(b), "%A %d %B %Y", &tm);
        ImGui::AlignTextToFramePadding();
        if (g_boldFont) ImGui::PushFont(g_boldFont, 0.0f);
        ImGui::TextUnformatted(b);
        if (g_boldFont) ImGui::PopFont();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(dayIt + 1 == days.end());
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) day = *(dayIt + 1);
    handCursor();
    ImGui::EndDisabled();
    ImGui::SameLine(0, ImGui::GetFontSize());
    if (ImGui::Button("Now")) {
        day = localMidnight(nowT);
        scrollToNow = true;
    }
    handCursor();
    ImGui::SameLine(0, ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Times are local. Shift + mouse wheel scrolls through the day; click a station to play it.");

    // the grid
    const float fs = ImGui::GetFontSize();
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float nameW = 200 * scale, pph = 230 * scale;   // name column, pixels per hour
    const float headerH = fs * 1.6f, rowH = fs * 3.0f;
    const float dayW = (float)(dayEnd - day) / 3600.0f * pph;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.02f));
    ImGui::BeginChild("epggrid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    const ImVec2 origin = ImGui::GetCursorScreenPos();   // content origin (moves with scrolling)
    ImGui::Dummy(ImVec2(nameW + dayW, headerH + rows.size() * rowH));
    const ImVec2 wp = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowSize().x - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0);
    const float wh = ImGui::GetWindowSize().y - (ImGui::GetScrollMaxX() > 0 ? ImGui::GetStyle().ScrollbarSize : 0);
    if (scrollToNow) {
        if (nowT >= day && nowT < dayEnd) ImGui::SetScrollX((float)(nowT - day) / 3600.0f * pph - pph * 0.5f);
        scrollToNow = false;
    }
    auto xAt = [&](int64_t t) { return origin.x + nameW + (float)(t - day) / 3600.0f * pph; };

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool gridHov = ImGui::IsWindowHovered();
    const ImVec2 area0(wp.x + nameW, wp.y + headerH), area1(wp.x + ww, wp.y + wh);   // programmes area
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text), dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const EpgProgramme *tip = nullptr;
    uint16_t tipSid = 0;

    // programmes
    dl->PushClipRect(area0, area1, true);
    for (int h = 0; h <= 24; h++) {   // hour lines
        const float x = xAt(day + h * 3600);
        dl->AddLine(ImVec2(x, area0.y), ImVec2(x, area1.y), col(1, 1, 1, 0.05f));
    }
    for (size_t r = 0; r < rows.size(); r++) {
        const float y = origin.y + headerH + r * rowH;
        if (y + rowH < area0.y || y > area1.y) continue;
        for (const EpgProgramme &p : epg->at(rows[r])) {
            const int64_t end = p.start + std::max(p.duration, 60);
            if (end <= day || p.start >= dayEnd) continue;
            const float x0 = xAt(std::max(p.start, day)), x1 = xAt(std::min(end, dayEnd));
            if (x1 < area0.x || x0 > area1.x) continue;
            const ImVec2 b0(x0 + 1.5f * scale, y + 2 * scale), b1(x1 - 1.5f * scale, y + rowH - 2 * scale);
            const bool onAir = nowT >= p.start && nowT < end;
            const bool hov = gridHov && mouse.x >= std::max(b0.x, area0.x) && mouse.x < b1.x && mouse.y >= std::max(b0.y, area0.y) &&
                             mouse.y < b1.y;
            ImVec4 fill = onAir ? ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f) : ImVec4(1, 1, 1, 0.06f);
            if (hov) fill.w += 0.08f;
            dl->AddRectFilled(b0, b1, ImGui::GetColorU32(fill), 4 * scale);
            if (hov) {
                tip = &p;
                tipSid = rows[r];
            }
            // text, kept in view when the block starts left of the visible area
            const float pad = fs * 0.4f;
            const float tx = std::max(b0.x + pad, area0.x + pad);
            const float avail = b1.x - pad - tx;
            if (avail < fs * 1.2f) continue;
            dl->PushClipRect(b0, b1, true);
            std::string title = programmeTitle(p);
            if (ImGui::CalcTextSize(title.c_str()).x > avail && !p.mediumName.empty()) title = p.mediumName;
            dl->AddText(ImVec2(tx, y + rowH * 0.5f - fs * 1.05f), textCol, fitText(title, avail).c_str());
            const std::string when = hhmm(p.start) + " - " + hhmm(end);
            dl->AddText(ImGui::GetFont(), fs * 0.82f, ImVec2(tx, y + rowH * 0.5f + fs * 0.1f), dimCol, when.c_str());
            dl->PopClipRect();
        }
    }
    if (nowT >= day && nowT < dayEnd) {   // now
        const float x = xAt(nowT);
        dl->AddLine(ImVec2(x, area0.y), ImVec2(x, origin.y + headerH + rows.size() * rowH), col(0.95f, 0.30f, 0.30f), 2 * scale);
    }
    dl->PopClipRect();

    // time header (stays at the top)
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_PopupBg);
    dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(area1.x, area0.y), bg);
    dl->PushClipRect(ImVec2(area0.x, wp.y), ImVec2(area1.x, area0.y), true);
    for (int h = 0; h < 24; h++) {
        const int64_t t = day + h * 3600;
        const float x = xAt(t);
        dl->AddLine(ImVec2(x, area0.y - fs * 0.4f), ImVec2(x, area0.y), col(1, 1, 1, 0.25f));
        dl->AddLine(ImVec2(x + pph * 0.5f, area0.y - fs * 0.2f), ImVec2(x + pph * 0.5f, area0.y), col(1, 1, 1, 0.15f));
        dl->AddText(ImVec2(x + fs * 0.25f, wp.y + (headerH - fs) * 0.35f), dimCol, hhmm(t).c_str());
    }
    if (nowT >= day && nowT < dayEnd) {
        const float x = xAt(nowT);
        dl->AddTriangleFilled(ImVec2(x - fs * 0.3f, area0.y - fs * 0.35f), ImVec2(x + fs * 0.3f, area0.y - fs * 0.35f),
                              ImVec2(x, area0.y), col(0.95f, 0.30f, 0.30f));
    }
    dl->PopClipRect();

    // station column (stays on the left)
    dl->AddRectFilled(ImVec2(wp.x, area0.y), ImVec2(area0.x, area1.y), bg);
    dl->PushClipRect(ImVec2(wp.x, area0.y), ImVec2(area0.x, area1.y), true);
    const float logoSz = std::min(32 * scale, rowH - 8 * scale);
    for (size_t r = 0; r < rows.size(); r++) {
        const float y = origin.y + headerH + r * rowH;
        if (y + rowH < area0.y || y > area1.y) continue;
        const uint16_t sid = rows[r];
        const ServiceEntry *e = findService(a, sid);
        const ImVec2 r0(wp.x + 2 * scale, y + 2 * scale), r1(area0.x - 4 * scale, y + rowH - 2 * scale);
        const bool hov = e && gridHov && mouse.x >= r0.x && mouse.x < r1.x && mouse.y >= std::max(r0.y, area0.y) &&
                         mouse.y < r1.y && mouse.y < area1.y;
        if (e && e->sid == a.snap.selectedSid)
            dl->AddRectFilled(r0, r1, ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.28f)), 6 * scale);
        else if (hov)
            dl->AddRectFilled(r0, r1, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6 * scale);
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && e->sid != a.snap.selectedSid) playService(a, e->sid);
        }
        const ImVec2 lp(wp.x + fs * 0.4f, y + (rowH - logoSz) * 0.5f);
        const LogoTextures *lt = findLogos(a, sid);
        if (lt && lt->logo32)
            dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)lt->logo32), lp, ImVec2(lp.x + logoSz, lp.y + logoSz),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4 * scale);
        else
            dl->AddRectFilled(lp, ImVec2(lp.x + logoSz, lp.y + logoSz), col(1, 1, 1, 0.05f), 4 * scale);
        const float tx = lp.x + logoSz + fs * 0.5f;
        char sidTxt[16];
        snprintf(sidTxt, sizeof(sidTxt), "%04X", sid);
        const std::string name = e && !e->label.empty() ? e->label : std::string("SId ") + sidTxt;
        dl->AddText(ImVec2(tx, y + rowH * 0.5f - fs * 1.05f), e ? textCol : dimCol, fitText(name, area0.x - tx - fs * 0.4f).c_str());
        dl->AddText(ImGui::GetFont(), fs * 0.82f, ImVec2(tx, y + rowH * 0.5f + fs * 0.1f), dimCol,
                    e ? sidTxt : (std::string(sidTxt) + "  not in this ensemble").c_str());
    }
    dl->PopClipRect();
    dl->AddRectFilled(wp, ImVec2(area0.x, area0.y), bg);   // corner
    ImGui::EndChild();

    if (tip) {
        const int64_t end = tip->start + tip->duration;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(fs * 28);
        if (g_boldFont) ImGui::PushFont(g_boldFont, 0.0f);
        ImGui::TextUnformatted(programmeTitle(*tip));
        if (g_boldFont) ImGui::PopFont();
        const ServiceEntry *e = findService(a, tipSid);
        ImGui::TextDisabled("%s - %s  (%d min)%s%s", hhmm(tip->start).c_str(), hhmm(end).c_str(), tip->duration / 60,
                            e ? "   " : "", e ? e->label.c_str() : "");
        if (!tip->description.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted(tip->description.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::EndPopup();
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
//   EDIPLAYER_MUXINFO=<sec>[:<px>]    open the ensemble information after <sec>, scrolled down <px>
//   EDIPLAYER_EPG=<sec>               open the programme guide after <sec>
//   EDIPLAYER_POPUP=<sec>:<name>      open the "favourites" list or the "favedit" editor after <sec>

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
    double exitAt = -1, recordAt = -1, copyAt = -1, muxInfoAt = -1, epgAt = -1, popupAt = -1;
    std::string popup;
    float muxInfoScroll = -1;
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
        if (const char *e = getenv("EDIPLAYER_EPG")) epgAt = atof(e);
        if (const char *p = getenv("EDIPLAYER_POPUP")) {
            popupAt = atof(p);
            if (const char *c = strchr(p, ':')) popup = c + 1;
        }
        if (const char *m = getenv("EDIPLAYER_MUXINFO")) {
            muxInfoAt = atof(m);
            if (const char *sc = strchr(m, ':')) muxInfoScroll = (float)atof(sc + 1);
        }
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
        if (th.muxInfoAt >= 0 && now() - t0 >= th.muxInfoAt) {
            g_openMuxInfo = true;
            g_muxInfoScroll = th.muxInfoScroll * scale;
            th.muxInfoAt = -1;
        }
        if (th.popupAt >= 0 && now() - t0 >= th.popupAt) {
            g_openSourcePopup = th.popup;
            th.popupAt = -1;
        }
        if (th.epgAt >= 0 && now() - t0 >= th.epgAt) {
            g_openEpg = true;
            th.epgAt = -1;
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
        muxInfoPopup(a);
        epgPopup(a);
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
