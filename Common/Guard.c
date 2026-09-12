// Read-only build verification; unknown builds never reach hook installation.
#include <Windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <string.h>

#include "Core.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif // STATUS_SUCCESS

DWORD HashFile(
    _In_ LPCWSTR cwszPath,
    _Out_writes_(65) PCHAR szHash
) {
    if (NULL == cwszPath || NULL == szHash) {
        return ERROR_INVALID_PARAMETER;
    }

    ZeroMemory(szHash, 65);

    HANDLE hFile = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE hAlgorithm = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    
    BYTE abDigest[32] = { 0 };
    DWORD cbRead = 0;
    DWORD dwError = ERROR_GEN_FAILURE;
    HANDLE hProcessHeap = GetProcessHeap();

    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    SIZE_T cbBufSiz = 65536U;
    LPBYTE abBuffer = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, cbBufSiz);
    if (NULL == abBuffer) {
        return ERROR_OUTOFMEMORY;
    }


    hFile = CreateFileW(
        cwszPath,
        GENERIC_READ, 
        FILE_SHARE_READ, 
        NULL, 
        OPEN_EXISTING, 
        0, 
        NULL
    );

    if (INVALID_HANDLE_VALUE == hFile) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (STATUS_SUCCESS != BCryptOpenAlgorithmProvider(
        &hAlgorithm, 
        BCRYPT_SHA256_ALGORITHM, 
        NULL, 
        0
    )) {
        goto _FINAL;
    }

    if (STATUS_SUCCESS != BCryptCreateHash(
        hAlgorithm, 
        &hHash, 
        NULL, 0, NULL, 0, 0
    )) {
        goto _FINAL;
    }

    while (1) {
        if (!ReadFile(hFile, abBuffer, cbBufSiz, &cbRead, NULL)) {
            dwError = GetLastError();
            goto _FINAL;
        }

        if (0 == cbRead) {
            break;
        }

        if (STATUS_SUCCESS != BCryptHashData(hHash, abBuffer, cbRead, 0)) {
            goto _FINAL;
        }
    }

    if (STATUS_SUCCESS != BCryptFinishHash(hHash, abDigest, sizeof(abDigest), 0)) {
        goto _FINAL;
    }

    static CONST CHAR szHex[] = "0123456789abcdef";

    for (DWORD dwIndex = 0; dwIndex < 32U; ++dwIndex) {
        szHash[dwIndex * 2] = szHex[abDigest[dwIndex] >> 4];
        szHash[dwIndex * 2 + 1] = szHex[abDigest[dwIndex] & 0x0F];
    }

    szHash[64] = '\0';

    dwError = ERROR_SUCCESS;
_FINAL:

    if (NULL != abBuffer) {
        HeapFree(hProcessHeap, 0, abBuffer);
    }

    if (NULL != hHash) {
        BCryptDestroyHash(hHash);
    }

    if (NULL != hAlgorithm) {
        BCryptCloseAlgorithmProvider(hAlgorithm, 0);
    }

    if (INVALID_HANDLE_VALUE != hFile) {
        CloseHandle(hFile);
    }

    return dwError;
}

DWORD ValidateServer(
    _Inout_opt_ HMODULE hServer
) {
    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    PWCHAR wszPath = HeapAlloc(
        hProcessHeap, 
        HEAP_ZERO_MEMORY,
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    if (NULL == wszPath) {
        return GetLastError();
    }

    CHAR szHash[65] = { 0 };
    DWORD dwError = ERROR_SUCCESS;

    static CONST DWORD adwRvas[] = {
        SERVER_BOARD_SOLVER_RVA, 
        SERVER_BOARD_APPLY_RVA, 
        SERVER_BOARD_SETTER_RVA, 
        SERVER_BOARD_CLIP_RVA
    };

    static CONST BYTE aabEntries[4][16] = {
        { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x78 },
        { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28 },
        { 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x28, 0xF3, 0x0F, 0x10, 0x81, 0x8C, 0x1B, 0x00 },
        { 0x48, 0x83, 0xEC, 0x38, 0x44, 0x0F, 0x29, 0x44, 0x24, 0x20, 0x0F, 0x29, 0x7C, 0x24, 0x10, 0x0F }
    };

    if (NULL == hServer) {
        return ERROR_MOD_NOT_FOUND;
    }

    if (0 == GetModuleFileNameW(hServer, wszPath, MAX_PATH_UNICODE)) {
        return GetLastError();
    }

    dwError = HashFile(wszPath, szHash);
    if (ERROR_SUCCESS != dwError) {
        return dwError;
    }

    if (0 != strcmp(szHash, SERVER_BOARD_SERVER_HASH)) {
        return ERROR_REVISION_MISMATCH;
    }

    __try {
        CONST BYTE *lpBase = (CONST BYTE *) hServer;
        CONST IMAGE_DOS_HEADER *lpDosHeader = (CONST IMAGE_DOS_HEADER *) lpBase;

        CONST IMAGE_NT_HEADERS64 *lpNt = (CONST IMAGE_NT_HEADERS64 *) (lpBase + lpDosHeader->e_lfanew);
        
        if (
            IMAGE_DOS_SIGNATURE != lpDosHeader->e_magic || 
            IMAGE_NT_SIGNATURE != lpNt->Signature ||
            IMAGE_FILE_MACHINE_AMD64 != lpNt->FileHeader.Machine ||
            0xC55000U != lpNt->OptionalHeader.SizeOfImage
        ) {
            return ERROR_BAD_EXE_FORMAT;
        }

        for (DWORD dwIndex = 0; dwIndex < ARRAYSIZE(adwRvas); ++dwIndex) {
            if (0 != memcmp(lpBase + adwRvas[dwIndex], aabEntries[dwIndex], 16)) {
                return ERROR_INVALID_DATA;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ERROR_NOACCESS;
    }

    return ERROR_SUCCESS;
}
