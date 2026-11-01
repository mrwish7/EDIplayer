// EDIplayer on Android: a NativeActivity (through android_native_app_glue)
// drawing with EGL / OpenGL ES 3 and Dear ImGui's Android backend. The Java
// activity (android/app/src/main/java/.../MainActivity.java) adds what the NDK
// can't do: the text input dialog, the clipboard, the system bar insets and
// the Back gesture.
//
// The App lives as long as the process. The GL and ImGui contexts come and go
// with the activity's window (e.g. when the app goes to the background), and
// android_main runs once per activity instance.

#include "app.h"
#include "core/log.h"

#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"

#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

static App *g_app = nullptr;   // made by the first activity; lives as long as the process

// ---------------------------------------------------------------------------
// Calls between android_main's thread and the Java activity. Strings go both
// ways as UTF-8 byte arrays (JNI's own string functions use modified UTF-8).

static android_app *g_android = nullptr;
static JNIEnv *g_env = nullptr;   // android_main's thread

// set by the activity on its UI thread
static std::mutex g_mutex;
static std::map<std::string, std::string> g_texts;   // confirmed text by field (platformTakeText)
static int g_insets[4] = {0, 0, 0, 0};               // system bars + cutout, px: left, top, right, bottom
static bool g_back = false;

static std::string fromBytes(JNIEnv *env, jbyteArray b)
{
    std::string s((size_t)env->GetArrayLength(b), '\0');
    env->GetByteArrayRegion(b, 0, (jsize)s.size(), (jbyte *)s.data());
    return s;
}

static jbyteArray toBytes(const char *s)
{
    const jsize n = (jsize)strlen(s);
    jbyteArray b = g_env->NewByteArray(n);
    g_env->SetByteArrayRegion(b, 0, n, (const jbyte *)s);
    return b;
}

// Call a void method of the activity
static void callActivity(const char *name, const char *sig, ...)
{
    if (!g_env || !g_android) return;
    jobject act = g_android->activity->clazz;
    jclass cls = g_env->GetObjectClass(act);
    if (jmethodID m = g_env->GetMethodID(cls, name, sig)) {
        va_list ap;
        va_start(ap, sig);
        g_env->CallVoidMethodV(act, m, ap);
        va_end(ap);
    }
    if (g_env->ExceptionCheck()) {
        g_env->ExceptionDescribe();
        g_env->ExceptionClear();
    }
    g_env->DeleteLocalRef(cls);
}

// Wake android_main's loop to handle what the activity just set (g_mutex held)
static void wakeLoop()
{
    if (g_android) ALooper_wake(g_android->looper);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_mrwish7_ediplayer_MainActivity_nativeTextResult(JNIEnv *env, jclass,
                                                                                                jbyteArray key, jbyteArray text)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_texts[fromBytes(env, key)] = fromBytes(env, text);
    wakeLoop();
}

extern "C" JNIEXPORT void JNICALL Java_io_github_mrwish7_ediplayer_MainActivity_nativeInsets(JNIEnv *, jclass, jint left,
                                                                                            jint top, jint right, jint bottom)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_insets[0] = left;
    g_insets[1] = top;
    g_insets[2] = right;
    g_insets[3] = bottom;
    wakeLoop();
}

extern "C" JNIEXPORT void JNICALL Java_io_github_mrwish7_ediplayer_MainActivity_nativeBack(JNIEnv *, jclass)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_back = true;
    wakeLoop();
}

void platformRequestText(const char *key, const char *title, const char *hint, const char *text)
{
    jbyteArray k = toBytes(key), t = toBytes(title), h = toBytes(hint), v = toBytes(text);
    callActivity("showTextInput", "([B[B[B[B)V", k, t, h, v);
    for (jbyteArray b : {k, t, h, v}) g_env->DeleteLocalRef(b);
}

bool platformTakeText(const char *key, std::string &text)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_texts.find(key);
    if (it == g_texts.end()) return false;
    text = std::move(it->second);
    g_texts.erase(it);
    return true;
}

bool platformCopyImage(const std::vector<uint8_t> &, const uint8_t *, int, int, std::string &err)
{
    err = "copying images isn't available on Android yet";
    return false;
}

static void setClipboardText(ImGuiContext *, const char *text)
{
    jbyteArray t = toBytes(text);
    callActivity("setClipboardText", "([B)V", t);
    g_env->DeleteLocalRef(t);
}

// ---------------------------------------------------------------------------
// Window, GL and ImGui

struct Gfx {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    float scale = 1.0f;   // display density relative to 160 dpi
    bool ready = false;
};
static Gfx g_gfx;

static float displayScale(android_app *app)
{
    const int32_t d = AConfiguration_getDensity(app->config);
    return d > 0 && d < ACONFIGURATION_DENSITY_ANY ? d / 160.0f : 1.0f;
}

// Font from the APK's assets (third_party/fonts)
static ImFont *addAssetFont(android_app *app, const char *name)
{
    static std::map<std::string, std::vector<uint8_t>> data;   // ImGui reads it as long as the atlas exists
    std::vector<uint8_t> &d = data[name];
    if (d.empty()) {
        AAsset *as = AAssetManager_open(app->activity->assetManager, name, AASSET_MODE_BUFFER);
        if (!as) return nullptr;
        if (const uint8_t *p = (const uint8_t *)AAsset_getBuffer(as)) d.assign(p, p + AAsset_getLength(as));
        AAsset_close(as);
        if (d.empty()) return nullptr;
    }
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(d.data(), (int)d.size(), 0.0f, &cfg);
}

static void shutdownUi();

static bool initUi(android_app *app)
{
    Gfx &g = g_gfx;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g.display, nullptr, nullptr);
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(g.display, attribs, &config, 1, &n) || n < 1) {
        applog::add("android: no suitable EGL configuration");
        shutdownUi();
        return false;
    }
    EGLint format;
    eglGetConfigAttrib(g.display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.context = eglCreateContext(g.display, config, EGL_NO_CONTEXT, ctxAttribs);
    g.surface = eglCreateWindowSurface(g.display, config, app->window, nullptr);
    if (g.context == EGL_NO_CONTEXT || g.surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(g.display, g.surface, g.surface, g.context)) {
        applog::add("android: cannot create the OpenGL ES 3 context");
        shutdownUi();
        return false;
    }

    g.scale = displayScale(app);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (!addAssetFont(app, "NotoSans-Regular.ttf")) ImGui::GetIO().Fonts->AddFontDefault();   // the first is the default
    appSetupUi(g.scale, addAssetFont(app, "NotoSans-Bold.ttf"));
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClipboardText;
    ImGui_ImplAndroid_Init(app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g.ready = true;
    return true;
}

static void shutdownUi()
{
    Gfx &g = g_gfx;
    if (g.ready) {
        appReleaseGl(*g_app);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }
    if (g.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.context != EGL_NO_CONTEXT) eglDestroyContext(g.display, g.context);
        if (g.surface != EGL_NO_SURFACE) eglDestroySurface(g.display, g.surface);
        eglTerminate(g.display);
    }
    g = Gfx();
}

static void drawFrame()
{
    App &a = *g_app;
    appUpdate(a);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    int in[4];
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        memcpy(in, g_insets, sizeof(in));
    }
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    appDraw(a, ImVec2((float)in[0], (float)in[1]), ImVec2(ds.x - in[0] - in[2], ds.y - in[1] - in[3]));
    ImGui::Render();

    glViewport(0, 0, (int)ds.x, (int)ds.y);
    glClearColor(0.086f, 0.094f, 0.114f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    appAfterRender(a, (int)ds.x, (int)ds.y);
    eglSwapBuffers(g_gfx.display, g_gfx.surface);
}

// ---------------------------------------------------------------------------
// Activity events and the main loop

static void onAppCmd(android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) initUi(app);
        break;
    case APP_CMD_TERM_WINDOW:
        shutdownUi();
        break;
    case APP_CMD_CONFIG_CHANGED:
        // e.g. moved to a display with another density: rebuild at the new scale
        if (g_gfx.ready && displayScale(app) != g_gfx.scale) {
            shutdownUi();
            if (app->window) initUi(app);
        }
        break;
    default:
        break;
    }
}

static int32_t onInputEvent(android_app *, AInputEvent *event)
{
    // keys ImGui doesn't use (volume, Back) return 0 and go on to the system
    return g_gfx.ready ? ImGui_ImplAndroid_HandleInputEvent(event) : 0;
}

void android_main(android_app *app)
{
    app->activity->vm->AttachCurrentThread(&g_env, nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_android = app;
    }
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInputEvent;
    if (!g_app) {
        g_app = new App;
        const std::string internal = app->activity->internalDataPath;
        const char *external = app->activity->externalDataPath;   // recordings: reachable over USB
        appInit(*g_app, internal + "/settings.ini", internal, external ? external : internal);
    }

    while (!app->destroyRequested) {
        // ~20 fps while there's a window (as on the desktop), else sleep until the next event
        int timeout = g_gfx.ready ? (int)(appFrameInterval(*g_app) * 1000) : -1;
        for (;;) {
            android_poll_source *source = nullptr;
            const int r = ALooper_pollOnce(timeout, nullptr, nullptr, (void **)&source);
            if (r < 0 && r != ALOOPER_POLL_CALLBACK) break;   // timeout or woken
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = 0;   // handle everything queued, then draw
        }
        if (app->destroyRequested) break;

        bool back;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            back = g_back;
            g_back = false;
        }
        // Back closes a popup, else sends the app to the background (it keeps playing)
        if (back && !(g_gfx.ready && appBack())) callActivity("goToBackground", "()V");
        if (appQuitRequested()) ANativeActivity_finish(app->activity);
        if (g_gfx.ready) drawFrame();
    }

    shutdownUi();
    appShutdown(*g_app);   // the activity is gone: stop playing
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_android = nullptr;
    }
    app->activity->vm->DetachCurrentThread();
    g_env = nullptr;
}
