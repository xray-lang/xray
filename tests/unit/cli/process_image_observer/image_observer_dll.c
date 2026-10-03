#include <windows.h>
__declspec(dllexport) int image_observer_value(void) { return 47; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance; (void)reason; (void)reserved;
    return TRUE;
}
