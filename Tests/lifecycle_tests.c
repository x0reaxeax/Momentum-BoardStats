// Exercise the actual internal collector with private executable fixture hook sites.
#define BOARD_COLLECTOR_TEST 1
#include "../BoardStats/Collector.c"
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                                           \
            ExitProcess(1);                                                                                  \
        }                                                                                                    \
    } while (0)

static int WINAPI RenderSentinel(
    VOID
) {
    return 7;
}

int wmain(
    void
) {
    HUD_STATE hudState = { 0 };
    DWORD adwSites[] = {
        SERVER_BOARD_SOLVER_RVA, 
        SERVER_BOARD_CLIP_RVA, 
        SERVER_BOARD_APPLY_RVA, 
        SERVER_BOARD_SETTER_RVA
    };

    DWORD fOld = 0;

    g_hServer = VirtualAlloc(NULL, 0xA0000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(NULL != g_hServer);
    for (DWORD dwIndex = 0; ARRAYSIZE(adwSites) > dwIndex; ++dwIndex) {
        // Enough relocatable instructions for a real MinHook entry patch.
        memset((BYTE *) g_hServer + adwSites[dwIndex], 0x90, 16);
        *((BYTE *) g_hServer + adwSites[dwIndex] + 16) = 0xC3;
    }
    memset((BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA + 0x100, 0x90, 16);
    *((BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA + 0x110) = 0xC3;
    CHECK(VirtualProtect(g_hServer, 0xA0000, PAGE_EXECUTE_READ, &fOld));
    FlushInstructionCache(GetCurrentProcess(), g_hServer, 0xA0000);
    {
        LPVOID lpOriginal = NULL;

        CHECK(MH_OK == MH_Initialize());
        CHECK(
            MH_OK == MH_CreateHook( (BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA + 0x100, (LPVOID) (ULONG_PTR) RenderSentinel, &lpOriginal )
        );
        CHECK(MH_OK == MH_EnableHook((BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA + 0x100));
    }

    for (DWORD dwSession = 0; dwSession < 3; ++dwSession) {
        CHECK(ERROR_SUCCESS == CollectorStart(NULL));
        CHECK(CollectorGetSnapshot(&hudState));
        CHECK(hudState.bSession && !hudState.bStopped && 0 == hudState.dwHistoryCount);
        {
            BOARD_EVENT event = { 0 };
            BOARD_METRICS expected = { 0 };

            event.vecIncoming.fValue[0] = 100.123F;
            event.vecOutgoing.fValue[0] = 60.789F;
            event.vecNormal.fValue[1] = 0.8660254F;
            event.vecNormal.fValue[2] = 0.5F;
            CHECK(CalculateMetrics(&event, &expected));
            for (UINT64 i = 1; i <= 6; ++i) {
                event.qwSequence = i;
                PushEvent(&event, g_lEpoch);
            }
            // Duplicate/out-of-order events must not displace the latest history.
            event.qwSequence = 2;
            PushEvent(&event, g_lEpoch);
            event.vecIncoming.fValue[0] = NAN;
            event.qwSequence = 7;
            PushEvent(&event, g_lEpoch);
            CHECK(ERROR_SUCCESS == CollectorStop(NULL));
            CHECK(CollectorGetSnapshot(&hudState) && hudState.bStopped);
            CHECK(HUD_HISTORY_COUNT == hudState.dwHistoryCount);
            CHECK(6 == hudState.aHistory[0].qwSequence && 3 == hudState.aHistory[3].qwSequence);
            CHECK(expected.dIncoming == hudState.aHistory[0].dIncoming);
            CHECK(expected.dRetention == hudState.aHistory[0].dRetention);
            CHECK(expected.dAngle == hudState.aHistory[0].dAngle);
            CHECK(expected.dDelta == hudState.aHistory[0].dDelta);
            CHECK((double) (dwSession + 1) == hudState.dInvalid && 0 == hudState.dStopError);
        }
        CHECK(ERROR_SUCCESS == CollectorStart(NULL));
        CHECK(CollectorGetSnapshot(&hudState));
        CHECK(0 == hudState.dwHistoryCount && !hudState.bStopped && 0 == hudState.dInvalid);
        CHECK(ERROR_SUCCESS == CollectorStop(NULL));
        CHECK(CollectorGetSnapshot(&hudState) && hudState.bStopped);
        CHECK(7 == ((int(WINAPI *)(VOID))((BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA + 0x100))());
        for (DWORD i = 0; i < ARRAYSIZE(adwSites); ++i) {
            CHECK(0x90 == *((BYTE *) g_hServer + adwSites[i]));
        }
    }
    CloseHandle(g_hWorker);
    CloseHandle(g_hStop);
    CHECK(MH_OK == MH_Uninitialize());
    VirtualFree(g_hServer, 0, MEM_RELEASE);
    puts("[+] Internal history, precision, invalid samples and restart passed.");

    return 0;
}
