// uishot: open a VST2 plugin's editor in a window and save a screenshot (BMP). Windows only.
// (From the mausynth-rebuild project; used to check plugin UIs without a GUI host.)
//   uishot <plugin.dll> <out.bmp> [program-param-index=value ...]
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct AEffect;
typedef intptr_t (*HostCb)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
    void* p; void (*setParameter)(AEffect*, int32_t, float); float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t r1, r2; int32_t d, rq, oq; float io; void* obj; void* user; int32_t id, ver;
    void* pr; void* pdr; char future[56];
};
struct ERect { int16_t top, left, bottom, right; };

static intptr_t hostCb(AEffect*, int32_t op, int32_t, intptr_t, void*, float) { return op == 1 ? 2400 : 0; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: uishot <plugin.dll> <out.bmp> [i=v ...]\n"); return 2; }
    SetProcessDPIAware();
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h) { fprintf(stderr, "load failed\n"); return 1; }
    auto entry = (AEffect * (*)(HostCb))GetProcAddress(h, "VSTPluginMain");
    AEffect* e = entry(hostCb);
    e->dispatcher(e, 0, 0, 0, nullptr, 0);             // effOpen
    e->dispatcher(e, 10, 0, 0, nullptr, 48000.f);      // sample rate
    for (int i = 3; i < argc; ++i) { int k; float v; if (sscanf(argv[i], "%d=%f", &k, &v) == 2) e->setParameter(e, k, v); }
    ERect* r = nullptr;
    e->dispatcher(e, 13, 0, 0, &r, 0);                 // effEditGetRect
    int w = r ? r->right - r->left : 960, hgt = r ? r->bottom - r->top : 600;
    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "uishot";
    RegisterClassA(&wc);
    RECT rc = {0, 0, w, hgt}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("uishot", "uishot", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    e->dispatcher(e, 14, 0, 0, hwnd, 0);               // effEditOpen
    e->dispatcher(e, 13, 0, 0, &r, 0);
    if (r) { w = r->right - r->left; hgt = r->bottom - r->top; }
    SetForegroundWindow(hwnd);
    for (int t = 0; t < 150; ++t) {
        MSG m; while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        e->dispatcher(e, 19, 0, 0, nullptr, 0);        // effEditIdle
        Sleep(10);
    }
    // Capture the window's own contents (PW_CLIENTONLY | PW_RENDERFULLCONTENT), never the desktop.
    HDC wdc = GetDC(hwnd), mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, hgt);
    SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, 0x1 | 0x2);
    ReleaseDC(hwnd, wdc);
    BITMAPINFOHEADER bi = {sizeof bi, w, -hgt, 1, 24, BI_RGB};
    int stride = ((w * 3 + 3) & ~3);
    char* px = (char*)malloc((size_t)stride * hgt);
    GetDIBits(mem, bmp, 0, hgt, px, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    FILE* f = fopen(argv[2], "wb");
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi + stride * hgt), 0, 0, (DWORD)(sizeof fh + sizeof bi)};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px, stride, hgt, f); fclose(f);
    e->dispatcher(e, 15, 0, 0, nullptr, 0);            // effEditClose
    DestroyWindow(hwnd);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    printf("saved %s (%dx%d)\n", argv[2], w, hgt);
    return 0;
}
