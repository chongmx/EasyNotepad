// The source of the binary fixtures in this folder (see make_fixtures.sh).
#ifdef _WIN32
#define API __declspec(dllexport)
extern "C" __declspec(dllimport) unsigned long __stdcall GetTickCount();
extern "C" __declspec(dllimport) void* __stdcall GetProcessHeap();
extern "C" __declspec(dllimport) int __stdcall MessageBeep(unsigned type); // delay-loaded
extern "C" void* __delayLoadHelper2(const void*, void**) { return nullptr; } // stands in for delayimp.lib
#else
#define API __attribute__((visibility("default")))
extern "C" unsigned long tick();
#endif

namespace sample {
struct Widget {
    API int area(int width, int height) const;
    int scale = 2;
};
int Widget::area(int width, int height) const { return width * height * scale; }
API int counter = 7;
} // namespace sample

extern "C" API int plain_add(int a, int b)
{
#ifdef _WIN32
    return a + b + int(GetTickCount() & 1) + (GetProcessHeap() ? 0 : 1) + MessageBeep(0);
#else
    return a + b + int(tick() & 1);
#endif
}
static int hidden_helper(int x) { return x * 3; }
extern "C" API int use_helper(int x) { return hidden_helper(x); }
