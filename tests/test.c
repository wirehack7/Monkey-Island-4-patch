// Loads build\dinput.dll into this exe and checks that the IAT hooks and the dinput forwarding work.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *PDICreate)(HINSTANCE, DWORD, void **, void *);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int isMonitorRect(HWND w)
{
    MONITORINFO mi = { sizeof mi };
    RECT r;
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    GetWindowRect(w, &r);
    return EqualRect(&r, &mi.rcMonitor);
}

int main(void)
{
    char path[MAX_PATH];
    HMODULE dll;
    PDICreate create;
    IUnknown *di = NULL;
    DEVMODEA dm = { 0 };
    HWND w;

    GetModuleFileNameA(NULL, path, sizeof path);
    strcpy(strrchr(path, '\\') + 1, "dinput.dll");
    dll = LoadLibraryA(path);
    CHECK(dll);
    if (!dll) return 1;

    create = (PDICreate)GetProcAddress(dll, "DirectInputCreateA");
    CHECK(create);
    if (create) CHECK(create(GetModuleHandleA(NULL), 0x0700, (void **)&di, NULL) == S_OK && di);
    if (di) di->lpVtbl->Release(di);

    // a mode no display supports: the real call fails, the hook pretends success
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = 12345;
    dm.dmPelsHeight = 678;
    CHECK(ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL);

    w = CreateWindowExA(0, "STATIC", "emifix test", WS_POPUP, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    CHECK(w);
    CHECK(isMonitorRect(w));
    MoveWindow(w, 0, 0, 800, 600, FALSE);
    CHECK(isMonitorRect(w));
    DestroyWindow(w);

    printf(fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
