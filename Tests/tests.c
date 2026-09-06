// Regression samples and an actual x64 detour/trampoline lifecycle smoke test.
#include <Windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Core.h"
#include "MinHook.h"

#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "[-] Line %d: %s\n", __LINE__, #expr);                                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

typedef INT (*TestCall_t)(INT);
static TestCall_t g_lpOriginal = NULL;
static volatile LONG g_lSeed = 4;

static __declspec(noinline) INT HookTarget(
    _In_ INT iValue
) {
    return iValue + (INT) InterlockedCompareExchange(&g_lSeed, 0, 0);
}

static INT HookReplacement(
    _In_ INT iValue
) {
    return g_lpOriginal(iValue) + 10;
}

int main(
    void
) {
    BOARD_EVENT event = { 0 };
    BOARD_EVENT taken = { 0 };
    BOARD_METRICS metrics = { 0 };
    BOARD_PENDING aPending[SERVER_BOARD_PENDING_CAPACITY] = { 0 };

    TestCall_t volatile lpTarget = HookTarget;
    event.vecIncoming = (BOARD_VECTOR) {{269.37762F, -215.68187F, -748.00659F}};
    event.vecNormal = (BOARD_VECTOR) {{0.7387802F, -0.055968199F, 0.6716184F}};
    event.vecOutgoing = (BOARD_VECTOR) {{484.57901F, -231.985F, -552.36896F}};
    event.vecFinal = event.vecOutgoing;
    event.fOverbounce = 1.0F;
    event.ulPlayer = 1;
    event.ulResult = 2;
    event.iTick = 100;
    CHECK(CalculateMetrics(&event, &metrics));
    CHECK(0.001 > fabs(metrics.dDelta + 53.22117));
    CHECK(0.001 > fabs(metrics.dRetention - 93.539316));
    CHECK(0.001 > fabs(metrics.dAngle - 69.291755));
    CHECK(190.0 < metrics.dHorizontalDelta);
    CHECK(StoreCandidate(aPending, &event));
    CHECK(TakeCandidate(aPending, 2, 1, 100, &event.vecIncoming, &taken));
    CHECK(!TakeCandidate(aPending, 2, 1, 100, &event.vecIncoming, &taken));
    CHECK(StoreCandidate(aPending, &event));
    CHECK(!TakeCandidate(aPending, 2, 1, 101, &event.vecIncoming, &taken));
    CHECK(StoreCandidate(aPending, &event));
    CHECK(!TakeCandidate(aPending, 2, 9, 100, &event.vecIncoming, &taken));
    CHECK(StoreCandidate(aPending, &event));
    ForgetResult(aPending, 2);
    CHECK(!TakeCandidate(aPending, 2, 1, 100, &event.vecIncoming, &taken));
    CHECK(StoreCandidate(aPending, &event));
    taken = event;
    taken.vecIncoming.fValue[0] += 1.0F;
    CHECK(!TakeCandidate(aPending, 2, 1, 100, &taken.vecIncoming, &taken));
    for (DWORD dwIndex = 0; dwIndex < SERVER_BOARD_PENDING_CAPACITY; ++dwIndex) {
        event.ulResult = dwIndex + 10;
        CHECK(StoreCandidate(aPending, &event));
    }
    event.ulResult = 999;
    CHECK(!StoreCandidate(aPending, &event));
    event.iTick = 1; // Backwards tick/reset expires old candidates too.
    CHECK(StoreCandidate(aPending, &event));
    CHECK(!TakeCandidate(aPending, 10, 1, 1, &event.vecIncoming, &taken));
    CHECK(TakeCandidate(aPending, 999, 1, 1, &event.vecIncoming, &taken));
    // A second board with identical velocity remains a distinct applied event.
    CHECK(StoreCandidate(aPending, &event));
    CHECK(TakeCandidate(aPending, 999, 1, 1, &event.vecIncoming, &taken));
    event.vecIncoming.fValue[0] = NAN;
    CHECK(!CalculateMetrics(&event, &metrics));
    event.vecIncoming = (BOARD_VECTOR) {{0, 0, 0}};
    CHECK(!CalculateMetrics(&event, &metrics));
    event.vecIncoming = (BOARD_VECTOR) {{-100, 0, 0}};
    event.vecNormal = (BOARD_VECTOR) {{1, 0, 0}}; // Wall, not a surf ramp.
    CHECK(!CalculateMetrics(&event, &metrics));
    CHECK(ERROR_MOD_NOT_FOUND == ValidateServer(NULL));
    CHECK(!CalculateMetrics(NULL, &metrics));
    event.vecNormal = (BOARD_VECTOR) {{0.8F, 0, 0.6F}};
    event.vecIncoming = (BOARD_VECTOR) {{0, 100, 0}};
    event.vecOutgoing = event.vecIncoming;
    CHECK(CalculateMetrics(&event, &metrics));
    CHECK(0.0001 > fabs(metrics.dRetention - 100.0));
    CHECK(0.0001 > fabs(metrics.dAngle - 90.0));
    event.vecIncoming = (BOARD_VECTOR) {{80, 0, 60}}; // Moving away from the plane.
    CHECK(!CalculateMetrics(&event, &metrics));
    {
        WCHAR szDirectory[MAX_PATH] = { 0 };
        WCHAR szFile[MAX_PATH] = { 0 };
        CHAR szHash[65] = { 0 };
        HANDLE hFile = INVALID_HANDLE_VALUE;
        DWORD cbWritten = 0;

        CHECK(0 != GetTempPathW(ARRAYSIZE(szDirectory), szDirectory));
        CHECK(0 != GetTempFileNameW(szDirectory, L"bst", 0, szFile));
        hFile = CreateFileW(szFile, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
        CHECK(INVALID_HANDLE_VALUE != hFile);
        CHECK(WriteFile(hFile, "abc", 3, &cbWritten, NULL));
        CloseHandle(hFile);
        CHECK(ERROR_SUCCESS == HashFile(szFile, szHash));
        CHECK(DeleteFileW(szFile));
        CHECK(0 == strcmp(szHash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        CHECK(ERROR_REVISION_MISMATCH == ValidateServer(GetModuleHandleW(NULL)));
    }
    CHECK(6 == lpTarget(2));
    CHECK(MH_OK == MH_Initialize());
    CHECK(
        MH_OK == MH_CreateHook( (LPVOID) (ULONG_PTR) HookTarget, (LPVOID) (ULONG_PTR) HookReplacement, (LPVOID *) &g_lpOriginal )
    );
    CHECK(MH_OK == MH_EnableHook((LPVOID) (ULONG_PTR) HookTarget));
    CHECK(16 == lpTarget(2));
    CHECK(MH_OK == MH_DisableHook((LPVOID) (ULONG_PTR) HookTarget));
    CHECK(6 == lpTarget(2));
    CHECK(MH_OK == MH_Uninitialize());
    printf("[+] Live sample, correlation/reset/overflow, invalid geometry, and x64 hook lifecycle passed.\n");

    return 0;
}
