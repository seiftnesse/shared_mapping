// Target process: owns a marker buffer; the container mirrors its address
// space. Commands: [p] print buffer, [t] trim working set, [q] quit.

#include <stdio.h>
#include <windows.h>

static char* g_buffer;

static void FillMarker(const char* text) {
    sprintf_s(g_buffer, 0x1000, "SM_MIRROR_MARKER_v1 | %s | pid=%lu", text,
              GetCurrentProcessId());
}

int main(void) {
    g_buffer = (char*)VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
    if (g_buffer == NULL) {
        printf("VirtualAlloc failed: %lu\n", GetLastError());
        return 1;
    }
    FillMarker("initial");

    printf("SM_TARGET_READY pid=%lu buffer=%p\n", GetCurrentProcessId(),
           (void*)g_buffer);
    printf("marker: %s\n", g_buffer);
    printf("commands: [p] print  [t] trim working set  [q] quit\n");
    fflush(stdout);

    for (;;) {
        int c = getchar();
        if (c == 'p') {
            printf("buffer=%p: %s\n", (void*)g_buffer, g_buffer);
        } else if (c == 't') {
            if (SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1,
                                         (SIZE_T)-1)) {
                printf("working set trimmed\n");
            } else {
                printf("trim failed: %lu\n", GetLastError());
            }
        } else if (c == 'q') {
            break;
        }
        fflush(stdout);
    }

    VirtualFree(g_buffer, 0, MEM_RELEASE);
    return 0;
}
