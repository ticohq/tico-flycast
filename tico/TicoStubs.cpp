/// @file TicoStubs.cpp
/// @brief Stub implementations for functions not needed in the tico overlay build
/// These functions are declared but not used in libretro mode with USE_SDL

// SDL Haptic stubs - declared in core/input/haptic.h when USE_SDL is defined
// but implemented in core/sdl/sdl.cpp which we don't compile for libretro

void sdl_setTorque(int port, float v) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)v;
}

void sdl_setDamper(int port, float param, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)param;
    (void)speed;
}

void sdl_setSpring(int port, float saturation, float speed) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)saturation;
    (void)speed;
}

void sdl_setSine(int port, float power, float frequency, unsigned int duration_ms) {
    // Stub - haptic not supported in this build
    (void)port;
    (void)power;
    (void)frequency;
    (void)duration_ms;
}

void sdl_stopHaptic(int port) {
    // Stub - haptic not supported in this build
    (void)port;
}

// SDL2's video subsystem links its EGL helpers (SDL_egl.c) even though tico
// never creates an SDL window: rendering is Vulkan on NVK. Linking the
// portlibs Mesa EGL would bring a second copy of Mesa's util code that clashes
// with NVK's, so these stand in and report that no EGL is available.
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;
#define STUB_EGL_FALSE 0u

extern "C" {
EGLint eglGetError(void) { return 0x3001; /* EGL_NOT_INITIALIZED */ }
EGLDisplay eglGetDisplay(void *) { return nullptr; }
EGLDisplay eglGetPlatformDisplay(EGLenum, void *, const void *) { return nullptr; }
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *) { return STUB_EGL_FALSE; }
EGLBoolean eglTerminate(EGLDisplay) { return STUB_EGL_FALSE; }
const char *eglQueryString(EGLDisplay, EGLint) { return nullptr; }
EGLBoolean eglBindAPI(EGLenum) { return STUB_EGL_FALSE; }
EGLenum eglQueryAPI(void) { return 0; }
void *eglGetProcAddress(const char *) { return nullptr; }
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *n) { if (n) *n = 0; return STUB_EGL_FALSE; }
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint, EGLint *) { return STUB_EGL_FALSE; }
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *) { return nullptr; }
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { return STUB_EGL_FALSE; }
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, void *, const EGLint *) { return nullptr; }
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *) { return nullptr; }
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return STUB_EGL_FALSE; }
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return STUB_EGL_FALSE; }
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface) { return STUB_EGL_FALSE; }
EGLBoolean eglSwapInterval(EGLDisplay, EGLint) { return STUB_EGL_FALSE; }
EGLBoolean eglWaitGL(void) { return STUB_EGL_FALSE; }
EGLBoolean eglWaitNative(EGLint) { return STUB_EGL_FALSE; }
}
