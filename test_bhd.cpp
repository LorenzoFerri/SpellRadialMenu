#include <windows.h>
#include <stdio.h>
int main() {
    HANDLE f = CreateFileA("Z:\\home\\faith\\RadialSpellMenu\\menu\\hi\\00_solo.tpfbhd", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        printf("Failed\n");
    } else {
        printf("Success\n");
        CloseHandle(f);
    }
    return 0;
}
