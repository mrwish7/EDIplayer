// EDIplayer on Windows and Linux: GLFW window, OpenGL 3, and the system
// specifics (where files go, fonts, the image clipboard). The UI is app.cpp.

#include "app.h"   // first: windows.h before GLFW

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#ifndef _WIN32
#include "icon_data.h"   // Windows uses the .ico resource instead
#endif
#include "stb_image.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>   // HWND for the clipboard
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static GLFWwindow *g_window = nullptr;

// ---------------------------------------------------------------------------
// Where files go

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

// ---------------------------------------------------------------------------
// Image clipboard. Windows: as a DIB (understood by every application) plus
// "PNG" (lossless, keeps transparency). Linux: as image/png through wl-copy or
// xclip.

bool platformCopyImage(const std::vector<uint8_t> &png, const uint8_t *px, int w, int h, std::string &err)
{
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
        ok = OpenClipboard(glfwGetWin32Window(g_window)) != 0;
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
    if (!ok) err = "the clipboard is not available";
    return ok;
#else
    (void)px;
    (void)w;
    (void)h;
    const bool wayland = getenv("WAYLAND_DISPLAY") != nullptr;
    FILE *p = popen(wayland ? "wl-copy --type image/png 2>/dev/null" : "xclip -selection clipboard -t image/png -i 2>/dev/null", "w");
    bool ok = p && fwrite(png.data(), 1, png.size(), p) == png.size();
    if (p && pclose(p) != 0) ok = false;
    if (!ok) err = wayland ? "copying images needs wl-copy (package wl-clipboard)" : "copying images needs xclip";
    return ok;
#endif
}

// ---------------------------------------------------------------------------
// Fonts

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

// Adds the system fonts; returns the bold one (nullptr if none was found)
static ImFont *loadFonts()
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
    return addFirstFont(bold, sizeof(bold) / sizeof(bold[0]));
}

// ---------------------------------------------------------------------------

int main(int, char **)
{
    App a;
    const std::string dataDir = spiBaseDir();
    appInit(a, settingsFile(), dataDir, dataDir);   // EDIplayer_audio/ next to EDIplayer_SPI/

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
    g_window = win;
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    appSetupUi(scale, loadFonts());
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    while (!glfwWindowShouldClose(win) && !appQuitRequested()) {
        glfwWaitEventsTimeout(appFrameInterval(a));
        appUpdate(a);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        appDraw(a, ImVec2(0, 0), io.DisplaySize);
        ImGui::Render();

        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.086f, 0.094f, 0.114f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        appAfterRender(a, fw, fh);
        glfwSwapBuffers(win);
    }

    appShutdown(a);
    appReleaseGl(a);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
