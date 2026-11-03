// EDIplayer user interface, shared by the desktop (main_glfw.cpp) and Android
// (main_android.cpp) front ends. The front end owns the window, the GL context
// and the main loop, and calls the app* functions below; the UI calls back
// through the platform* functions for what differs between systems.
#pragma once

#include "gl.h"
#include "imgui.h"

#include "core/audio_out.h"
#include "core/player.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
    // desktop window (main_glfw.cpp): size in unscaled pixels, 0 for the default, when not maximised
    int windowW = 0, windowH = 0;
    bool windowMaximized = false;

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
    std::deque<std::string> log;
    size_t logCounter = 0;
    uint64_t lastBytes = 0;
    double lastRateTime = 0, rxKbps = 0;
    bool scrollToSelected = false;
    int selectedIndex = -1;            // list row last scrolled to
    std::string settingsPath;
    bool narrow = false;               // phone layout: one panel at a time
    enum Tab { TabNowPlaying, TabServices } tab = TabNowPlaying;
    bool muted = false;                // MediaCommand::Mute (e.g. during a phone call)
};

// What the system's media controls show (Android: notification, lock screen)
struct MediaInfo {
    bool playing = false;              // receiving, with a service selected
    bool multicast = false;            // the source is a multicast group
    std::string station, title, artist, album;
    SharedBytes artwork;               // the slide, else the station's SPI logo (image file)
    bool operator==(const MediaInfo &) const = default;
};

// From the system's media controls; the values are shared with PlaybackService.java
enum class MediaCommand { Play, Stop, Next, Previous, Mute, Unmute };

// ---- called by the front end

// Once per process: settings, the folders for SPI files and recordings, test hooks
void appInit(App &a, const std::string &settingsPath, const std::string &spiDir, const std::string &recordDir);
// After creating the ImGui context and adding the fonts (the first is the
// default): theme, sizes, the bold font (nullptr if none)
void appSetupUi(float scale, ImFont *boldFont);
// Each frame, before ImGui::NewFrame(): test hooks, player state
void appUpdate(App &a);
// Between ImGui::NewFrame() and ImGui::Render(): the whole UI in this rectangle
void appDraw(App &a, ImVec2 pos, ImVec2 size);
// After rendering, with the framebuffer still bound (test hook screenshots)
void appAfterRender(App &a, int fbWidth, int fbHeight);
// Seconds to wait for input between frames
double appFrameInterval(const App &a);
// Test hook EDIPLAYER_EXIT
bool appQuitRequested();
// Back button/gesture: true if it closed a popup
bool appBack();
// Delete the textures; they are made again from the player's data when the
// next GL context draws
void appReleaseGl(App &a);
// Stop playing and save the settings
void appShutdown(App &a);
// Media controls: what is playing (from snapshot s), and their commands
MediaInfo appMediaInfo(const App &a, const PlayerSnapshot &s);
void appMediaCommand(App &a, MediaCommand c);

// ---- implemented by the front end

// Put an image on the system clipboard: png is the image as a PNG file, rgba its pixels
bool platformCopyImage(const std::vector<uint8_t> &png, const uint8_t *rgba, int w, int h, std::string &err);

#ifdef __ANDROID__
// Text entry through the system's input dialog: request it, then poll for the
// text the user confirmed (nothing if they cancelled)
void platformRequestText(const char *key, const char *title, const char *hint, const char *text);
bool platformTakeText(const char *key, std::string &text);
// Share an image (image file contents) through the system's share sheet, or
// save it to Pictures/EDIplayer; name: the file name without extension
void platformShareImage(const std::vector<uint8_t> &file, const std::string &name, bool save);
#endif
