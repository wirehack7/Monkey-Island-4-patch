// emifix: Escape from Monkey Island (Monkey4.exe, OpenGL mode -gl) fixes for modern Windows.
// Loaded as dinput.dll proxy. Hooks Monkey4's IAT:
//  - no display mode change; borderless window covering the monitor
//  - game renders at its own resolution (640x480, 800x600, ...) into an FBO,
//    scaled with preserved aspect ratio to the window on SwapBuffers
//  - Bink movies drawn through OpenGL instead of GDI (BinkBufferBlit)
// Env EMIFIX_DUMP=<dir> dumps frames as BMP for debugging.
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_BGRA_EXT 0x80E1
#define GL_CLAMP_TO_EDGE 0x812F

typedef void (APIENTRY *PGenFB)(GLsizei, GLuint *);
typedef void (APIENTRY *PBindFB)(GLenum, GLuint);
typedef void (APIENTRY *PFBRB)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *PRBStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef GLenum (APIENTRY *PCheckFB)(GLenum);
typedef void (APIENTRY *PBlitFB)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY *PDelFB)(GLsizei, const GLuint *);

static PGenFB pGenFramebuffers, pGenRenderbuffers;
static PBindFB pBindFramebuffer, pBindRenderbuffer;
static PFBRB pFramebufferRenderbuffer;
static PRBStorage pRenderbufferStorage;
static PCheckFB pCheckFramebufferStatus;
static PBlitFB pBlitFramebuffer;
static PDelFB pDeleteFramebuffers, pDeleteRenderbuffers;

static int gameW = 640, gameH = 480; // resolution the game asked for
static int fboW, fboH;

static HGLRC fboCtx;
static GLuint fbo, rbs[2], movieTex;
static HWND mainWnd;
static char dumpDir[MAX_PATH];
static unsigned frameNo;
static FILE *logf;

static void *movieBuf;
static int moviePitch, movieH;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!logf) return;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

static RECT monitorRect(HWND w)
{
    MONITORINFO mi = { sizeof mi };
    HMONITOR m = w ? MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY)
                   : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(m, &mi);
    return mi.rcMonitor;
}

static int isPopup(DWORD style) { return (style & WS_POPUP) != 0; }

static void setGameSize(int w, int h)
{
    if (w <= 0 || h <= 0 || (w == gameW && h == gameH)) return;
    gameW = w; gameH = h;
    logmsg("game resolution %dx%d", w, h);
}

// ---- user32 hooks ----

static LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    logmsg("ChangeDisplaySettingsA ignored (%dx%d)", dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0);
    if (dm && (dm->dmFields & DM_PELSWIDTH) && (dm->dmFields & DM_PELSHEIGHT))
        setGameSize((int)dm->dmPelsWidth, (int)dm->dmPelsHeight);
    return DISP_CHANGE_SUCCESSFUL;
}

static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y,
                                      int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID p)
{
    HWND r;
    if (!mainWnd && isPopup(style)) {
        RECT m = monitorRect(NULL);
        setGameSize(w, h);
        x = m.left; y = m.top; w = m.right - m.left; h = m.bottom - m.top;
    }
    r = CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, p);
    if (!mainWnd) mainWnd = r;
    logmsg("CreateWindowExA style=%08lx %d,%d %dx%d -> %p", style, x, y, w, h, r);
    return r;
}

static BOOL WINAPI hk_MoveWindow(HWND wnd, int x, int y, int w, int h, BOOL repaint)
{
    if (wnd == mainWnd && isPopup(GetWindowLongA(wnd, GWL_STYLE))) {
        RECT m = monitorRect(wnd);
        x = m.left; y = m.top; w = m.right - m.left; h = m.bottom - m.top;
    }
    return MoveWindow(wnd, x, y, w, h, repaint);
}

// ---- OpenGL ----

static int loadFboFuncs(void)
{
    pGenFramebuffers = (PGenFB)wglGetProcAddress("glGenFramebuffers");
    pGenRenderbuffers = (PGenFB)wglGetProcAddress("glGenRenderbuffers");
    pBindFramebuffer = (PBindFB)wglGetProcAddress("glBindFramebuffer");
    pBindRenderbuffer = (PBindFB)wglGetProcAddress("glBindRenderbuffer");
    pFramebufferRenderbuffer = (PFBRB)wglGetProcAddress("glFramebufferRenderbuffer");
    pRenderbufferStorage = (PRBStorage)wglGetProcAddress("glRenderbufferStorage");
    pCheckFramebufferStatus = (PCheckFB)wglGetProcAddress("glCheckFramebufferStatus");
    pBlitFramebuffer = (PBlitFB)wglGetProcAddress("glBlitFramebuffer");
    pDeleteFramebuffers = (PDelFB)wglGetProcAddress("glDeleteFramebuffers");
    pDeleteRenderbuffers = (PDelFB)wglGetProcAddress("glDeleteRenderbuffers");
    return pGenFramebuffers && pGenRenderbuffers && pBindFramebuffer && pBindRenderbuffer &&
           pFramebufferRenderbuffer && pRenderbufferStorage && pCheckFramebufferStatus && pBlitFramebuffer &&
           pDeleteFramebuffers && pDeleteRenderbuffers;
}

// Creates the FBO at the current game resolution, replacing an existing one in the same context.
static void createFbo(void)
{
    GLenum st;
    if (fbo) {
        pBindFramebuffer(GL_FRAMEBUFFER, 0);
        pDeleteFramebuffers(1, &fbo);
        pDeleteRenderbuffers(2, rbs);
        fbo = 0;
    }
    fboW = gameW; fboH = gameH;
    pGenRenderbuffers(2, rbs);
    pBindRenderbuffer(GL_RENDERBUFFER, rbs[0]);
    pRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fboW, fboH);
    pBindRenderbuffer(GL_RENDERBUFFER, rbs[1]);
    pRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fboW, fboH);
    pGenFramebuffers(1, &fbo);
    pBindFramebuffer(GL_FRAMEBUFFER, fbo);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbs[0]);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbs[1]);
    st = pCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        logmsg("FBO incomplete %x", st);
        pBindFramebuffer(GL_FRAMEBUFFER, 0);
        fbo = 0;
        return;
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    logmsg("FBO %dx%d ready, GL %s / %s", fboW, fboH, glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

static BOOL WINAPI hk_wglMakeCurrent(HDC dc, HGLRC ctx)
{
    BOOL r = wglMakeCurrent(dc, ctx);
    if (r && ctx && ctx != fboCtx) {
        // names from the previous context died with it
        fboCtx = ctx;
        fbo = 0;
        movieTex = 0;
        if (loadFboFuncs()) createFbo();
        else logmsg("FBO functions missing");
    }
    if (!ctx) fboCtx = 0;
    return r;
}

static void APIENTRY hk_glReadBuffer(GLenum mode)
{
    if (fbo && mode != GL_COLOR_ATTACHMENT0) mode = GL_COLOR_ATTACHMENT0;
    glReadBuffer(mode);
}

static void writeBmp(const char *path, const unsigned char *px, int w, int h)
{
    BITMAPFILEHEADER fh = { 0x4D42 };
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 32, BI_RGB };
    FILE *f = fopen(path, "wb");
    if (!f) { logmsg("cannot write %s", path); return; }
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + w * h * 4;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

// Dumps the currently bound read buffer.
static void dump(const char *tag, int w, int h)
{
    char path[MAX_PATH];
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    _snprintf(path, sizeof path, "%s\\%s_%05u.bmp", dumpDir, tag, frameNo);
    writeBmp(path, px, w, h);
    free(px);
}

static int gameLogged, movieLogged;

static void present(HDC dc, const char *tag)
{
    RECT cr;
    int cw, ch, dw, dh, dx, dy;
    GLfloat cc[4];
    GLboolean cm[4], sc;

    frameNo++;
    if (!fbo || wglGetCurrentContext() != fboCtx) {
        static int logged;
        if (!logged++) logmsg("present %s: passthrough, fbo=%u ctx=%p fboCtx=%p", tag, fbo, wglGetCurrentContext(), fboCtx);
        SwapBuffers(dc);
        return;
    }

    pBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    if (dumpDir[0] && frameNo % 30 == 0) dump(tag, fboW, fboH);

    GetClientRect(WindowFromDC(dc), &cr);
    cw = cr.right; ch = cr.bottom;
    dh = ch; dw = ch * fboW / fboH;
    if (dw > cw) { dw = cw; dh = cw * fboH / fboW; }
    dx = (cw - dw) / 2; dy = (ch - dh) / 2;

    sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);

    pBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pBlitFramebuffer(0, 0, fboW, fboH, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (!strcmp(tag, "game") ? !gameLogged++ : !movieLogged++)
        logmsg("present %s: wnd %p client %dx%d -> %d,%d %dx%d, glError %x", tag, WindowFromDC(dc), cw, ch, dx, dy, dw, dh, glGetError());
    if (dumpDir[0] && frameNo % 300 == 0) {
        pBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        dump("screen", cw, ch);
    }
    SwapBuffers(dc);
    if (fboW != gameW || fboH != gameH) createFbo();
    if (fbo) pBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

static BOOL WINAPI hk_SwapBuffers(HDC dc)
{
    present(dc, "game");
    return TRUE;
}

// ---- Bink ----

typedef int (WINAPI *PBinkCopyToBuffer)(void *, void *, int, unsigned, unsigned, unsigned, unsigned);
typedef void (WINAPI *PBinkBufferBlit)(void *, void *, unsigned);
static PBinkCopyToBuffer oBinkCopyToBuffer;
static PBinkBufferBlit oBinkBufferBlit;

static int WINAPI hk_BinkCopyToBuffer(void *bnk, void *dest, int pitch, unsigned h, unsigned x, unsigned y, unsigned flags)
{
    static int logged;
    if (!logged++) logmsg("BinkCopyToBuffer flags %x pitch %d h %u", flags, pitch, h);
    // ponytail: only 32-bit surfaces (what EMI uses); others fall back to GDI blit
    if ((flags & 0xF) == 3 || (flags & 0xF) == 4) {
        movieBuf = dest; moviePitch = pitch; movieH = (int)h;
    } else {
        movieBuf = NULL;
    }
    return oBinkCopyToBuffer(bnk, dest, pitch, h, x, y, flags);
}

static void drawMovie(void)
{
    int w = moviePitch / 4;
    if (!movieTex) {
        glGenTextures(1, &movieTex);
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CLIP_PLANE0); glDisable(GL_CLIP_PLANE1); glDisable(GL_CLIP_PLANE2);
    glDisable(GL_CLIP_PLANE3); glDisable(GL_CLIP_PLANE4); glDisable(GL_CLIP_PLANE5);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, fboW, fboH);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, movieTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, movieH, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, movieBuf);

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();

    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_TEXTURE); glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}

static void WINAPI hk_BinkBufferBlit(void *buf, void *rects, unsigned n)
{
    if (!movieBuf || !fbo || wglGetCurrentContext() != fboCtx) {
        static int logged;
        if (!logged++) logmsg("BinkBufferBlit GDI fallback: movieBuf=%p fbo=%u ctx=%p fboCtx=%p", movieBuf, fbo, wglGetCurrentContext(), fboCtx);
        oBinkBufferBlit(buf, rects, n);
        return;
    }
    drawMovie();
    present(wglGetCurrentDC(), "movie");
}

// ---- IAT patching ----

static void hookImport(const char *dll, const char *func, void *hook, void **orig)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (_stricmp((char *)(base + d->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, func)) continue;
            if (orig) *orig = GetProcAddress(GetModuleHandleA(dll), func);
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            logmsg("hooked %s!%s", dll, func);
            return;
        }
    }
    logmsg("NOT hooked %s!%s", dll, func);
}

static void install(void)
{
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, sizeof path)) {
        char *s = strrchr(path, '\\');
        if (s) strcpy(s + 1, "emifix.log");
        logf = fopen(path, "w");
    }
    if (!GetEnvironmentVariableA("EMIFIX_DUMP", dumpDir, sizeof dumpDir)) dumpDir[0] = 0;
    if (dumpDir[0]) logmsg("dumping frames to %s", dumpDir);
    SetProcessDPIAware();

    hookImport("user32.dll", "ChangeDisplaySettingsA", hk_ChangeDisplaySettingsA, NULL);
    hookImport("user32.dll", "CreateWindowExA", hk_CreateWindowExA, NULL);
    hookImport("user32.dll", "MoveWindow", hk_MoveWindow, NULL);
    hookImport("gdi32.dll", "SwapBuffers", hk_SwapBuffers, NULL);
    hookImport("opengl32.dll", "wglMakeCurrent", hk_wglMakeCurrent, NULL);
    hookImport("opengl32.dll", "glReadBuffer", hk_glReadBuffer, NULL);
    hookImport("binkw32.dll", "_BinkCopyToBuffer@28", hk_BinkCopyToBuffer, (void **)&oBinkCopyToBuffer);
    hookImport("binkw32.dll", "_BinkBufferBlit@12", hk_BinkBufferBlit, (void **)&oBinkBufferBlit);
}

// ---- dinput proxy ----

typedef HRESULT (WINAPI *PDICreate)(HINSTANCE, DWORD, void **, void *);

HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    static PDICreate real;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput.dll");
        real = (PDICreate)GetProcAddress(LoadLibraryA(path), "DirectInputCreateA");
        if (!real) return E_FAIL;
    }
    return real(inst, ver, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        install();
    }
    return TRUE;
}
