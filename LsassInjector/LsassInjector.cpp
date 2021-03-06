#include "LsassInjector.h"
#include "HandleFinder.h"
#include <time.h>

#define Normal_Function_Length 0x1000

using namespace std;

#define RELOC_FLAG32(RelInfo) ((RelInfo >> 0x0C) == IMAGE_REL_BASED_HIGHLOW)
#define RELOC_FLAG64(RelInfo) ((RelInfo >> 0x0C) == IMAGE_REL_BASED_DIR64)

#ifdef _WIN64
#define RELOC_FLAG RELOC_FLAG64
#else
#define RELOC_FLAG RELOC_FLAG32
#endif

void __stdcall Shellcode(MANUAL_MAPPING_DATA * pData)
{
	if (!pData)
		return;

	if (pData->Signal != 2) {
		pData->Signal = 1;
		return;
	}

	BYTE * pBase = reinterpret_cast<BYTE*>(pData->ModuleBase);
	auto * pOpt = &reinterpret_cast<IMAGE_NT_HEADERS*>(pBase + reinterpret_cast<IMAGE_DOS_HEADER*>(pBase)->e_lfanew)->OptionalHeader;

	auto _LoadLibraryA = pData->pLoadLibraryA;
	auto _GetProcAddress = pData->pGetProcAddress;
	auto _RtlAddFunctionTable = pData->pRtlAddFunctionTable;
	auto _DllMain = reinterpret_cast<f_DLL_ENTRY_POINT>(pBase + pOpt->AddressOfEntryPoint);

	BYTE * LocationDelta = pBase - pOpt->ImageBase;
	if (LocationDelta)
	{
		if (!pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
			pData->Signal = 1;
			return;
		}
		auto * pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
		while (pRelocData->VirtualAddress)
		{
			UINT AmountOfEntries = (pRelocData->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
			WORD * pRelativeInfo = reinterpret_cast<WORD*>(pRelocData + 1);

			for (UINT i = 0; i != AmountOfEntries; ++i, ++pRelativeInfo)
			{
				if (RELOC_FLAG(*pRelativeInfo))
				{
					UINT_PTR * pPatch = reinterpret_cast<UINT_PTR*>(pBase + pRelocData->VirtualAddress + ((*pRelativeInfo) & 0xFFF));
					*pPatch += reinterpret_cast<UINT_PTR>(LocationDelta);
				}
			}
			pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<BYTE*>(pRelocData) + pRelocData->SizeOfBlock);
		}
	}

	if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
	{
		auto * pImportDescr = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
		while (pImportDescr->Name)
		{
			char * szMod = reinterpret_cast<char*>(pBase + pImportDescr->Name);
			HINSTANCE hDll = _LoadLibraryA(szMod);

			ULONG_PTR * pThunkRef = reinterpret_cast<ULONG_PTR*>(pBase + pImportDescr->OriginalFirstThunk);
			ULONG_PTR * pFuncRef = reinterpret_cast<ULONG_PTR*>(pBase + pImportDescr->FirstThunk);

			if (!pThunkRef)
				pThunkRef = pFuncRef;

			for (; *pThunkRef; ++pThunkRef, ++pFuncRef)
			{
				if (IMAGE_SNAP_BY_ORDINAL(*pThunkRef))
				{
					*pFuncRef = reinterpret_cast<ULONG_PTR>(_GetProcAddress(hDll, reinterpret_cast<char*>(*pThunkRef & 0xFFFF)));
				}
				else
				{
					auto * pImport = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(pBase + (*pThunkRef));
					*pFuncRef = reinterpret_cast<ULONG_PTR>(_GetProcAddress(hDll, pImport->Name));
				}
			}
			++pImportDescr;
		}
	}
	
	if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
	{
		auto * pTLS = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
		auto * pCallback = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(pTLS->AddressOfCallBacks);
		for (; pCallback && *pCallback; ++pCallback)
			(*pCallback)(pBase, DLL_PROCESS_ATTACH, nullptr);
	}
	

	if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size)
	{
		auto pTLS = reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress);
		if (pTLS) {
			DWORD Count = (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) - 1;
			if (Count) {
				if(_RtlAddFunctionTable((PRUNTIME_FUNCTION)pTLS, Count, (DWORD64)pBase))
					pData->Signal = 0;
				else{
					pData->Signal = 1;
					return;
				}
			}
		}
	}


	if (_DllMain(pBase, DLL_PROCESS_ATTACH, nullptr))
		pData->Signal = 0;
	else
		pData->Signal = 1;
}

bool ManualMap(HANDLE hProc, DLL_PARAM * DllParam)
{
	BYTE *					pSrcData = reinterpret_cast<BYTE*>(DllParam->pTargetDllBuffer);
	IMAGE_NT_HEADERS *		pOldNtHeader = nullptr;
	IMAGE_OPTIONAL_HEADER * pOldOptHeader = nullptr;
	IMAGE_FILE_HEADER *		pOldFileHeader = nullptr;
	BYTE *					pTargetBase = nullptr;
	LPVOID					addressOfHookFunction = DllParam->addressOfHookFunction;

	if (reinterpret_cast<IMAGE_DOS_HEADER*>(pSrcData)->e_magic != 0x5A4D) //"MZ"
	{
		return false;
	}

	pOldNtHeader = reinterpret_cast<IMAGE_NT_HEADERS*>(pSrcData + reinterpret_cast<IMAGE_DOS_HEADER*>(pSrcData)->e_lfanew);
	pOldOptHeader = &pOldNtHeader->OptionalHeader;
	pOldFileHeader = &pOldNtHeader->FileHeader;

#ifdef _WIN64
	if (pOldFileHeader->Machine != IMAGE_FILE_MACHINE_AMD64)
	{
		return false;
	}
#else
	if (pOldFileHeader->Machine != IMAGE_FILE_MACHINE_I386)
	{
		delete[] pSrcData;
		return false;
	}
#endif

	//타겟 프로세스에 메모리 할당
	pTargetBase = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, reinterpret_cast<void*>(pOldOptHeader->ImageBase), pOldOptHeader->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!pTargetBase)
	{
		pTargetBase = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, pOldOptHeader->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!pTargetBase)
		{
			return false;
		}
	}

	if (!WriteProcessMemory(hProc, pTargetBase, pSrcData, pOldOptHeader->SizeOfHeaders, nullptr)) { // PE 헤더 작성
		return false;
	}

	auto * pSectionHeader = IMAGE_FIRST_SECTION(pOldNtHeader);

	for (UINT i = 0; i != pOldFileHeader->NumberOfSections; ++i, ++pSectionHeader)
	{
		if (pSectionHeader->SizeOfRawData)
		{
			if (!WriteProcessMemory(hProc, pTargetBase + pSectionHeader->VirtualAddress, pSrcData + pSectionHeader->PointerToRawData, pSectionHeader->SizeOfRawData, nullptr))
			{
				VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
				return false;
			}
			if ((pSectionHeader->Characteristics & IMAGE_SCN_MEM_EXECUTE) == IMAGE_SCN_MEM_EXECUTE) {
				DWORD oldProtection = 0;
				VirtualProtectEx(hProc, pTargetBase + pSectionHeader->VirtualAddress, pSectionHeader->SizeOfRawData, PAGE_EXECUTE_READWRITE, &oldProtection);
			}
		}
	}


    // 매핑 패러미터용 준비+할당+쓰기
	MANUAL_MAPPING_DATA data{ 0 };
	data.pLoadLibraryA = LoadLibraryA;
	data.pGetProcAddress = reinterpret_cast<f_GetProcAddress>(GetProcAddress);
	data.pRtlAddFunctionTable = reinterpret_cast<f_RtlAddFunctionTable>(RtlAddFunctionTable);
	data.Signal = 2;
	data.ModuleBase = pTargetBase;

	BYTE* pMappingData = reinterpret_cast<BYTE*>(VirtualAllocEx(hProc, nullptr, sizeof(MANUAL_MAPPING_DATA), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // Create Mapping data in game
	WriteProcessMemory(hProc, pMappingData, &data, sizeof(MANUAL_MAPPING_DATA), nullptr);

	// 타겟 프로세스에 쓸 쉘코드 준비+할당+쓰기

	BYTE NtUserPeekMessageOriginalBYTE[12] = {
		0x4C, 0x8B, 0xD1, 0xB8, 0x04, 0x10, 0x00, 0x00, 0xF6, 0x04, 0x25, 0x08
	};

	BYTE preShellCode[] =
	{
		0x51,                                                       // +0   push        rcx  
		0x52,                                                       // +1   push        rdx  
		0x41, 0x50,                                                 // +2   push        r8  
		0x41, 0x51,                                                 // +4   push        r9  
		0x41, 0x52,                                                 // +6   push        r10  
		0x41, 0x53,                                                 // +8   push        r11  
		0x48, 0x33, 0xD2,                                           // +10  xor         rdx,rdx  
		0x48, 0x8B, 0xC4,                                           // +13  mov         rax,rsp  
		0x48, 0xC7, 0xC1, 0x10, 0x00, 0x00, 0x00,                   // +16  mov         rcx,10h  
		0x48, 0xF7, 0xF1,                                           // +23  div         rax,rcx  
		0x48, 0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // +26  mov         rcx,0FFFFFFFFFFFFFFFFh  
		0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // +36  mov         rax,0FFFFFFFFFFFFFFFFh  
		0x48, 0x83, 0xFA, 0x00,                                     // +46  cmp         rdx,0  
		0x74, 0x0C,                                                 // +50  jee          NoAlign (07FF7016915A0h)  
		0x48, 0x83, 0xEC, 0x28,                                     // +52  sub         rsp,28h  
		0xFF, 0xD0,                                                 // +56  call        rax  
		0x48, 0x83, 0xC4, 0x28,                                     // +58  add         rsp,28h  
		0xEB, 0x0A,                                                 // +62  jmp         NoAlign+0Ah (07FF7016915AAh)  
		0x48, 0x83, 0xEC, 0x20,                                     // +64  sub         rsp,20h  
		0xFF, 0xD0,                                                 // +68  call        rax  
		0x48, 0x83, 0xC4, 0x20,                                     // +70  add         rsp,20h  
		0x48, 0xBE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // +74  mov         rsi,0FFFFFFFFFFFFFFFFh  
		0x48, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // +84  mov         rdi,0FFFFFFFFFFFFFFFFh  
		0x48, 0x8B, 0xC7,                                           // +94  mov         rax,rdi  
		0x48, 0xC7, 0xC1, 0x0C, 0x00, 0x00, 0x00,                   // +97  mov         rcx,0Ch  
		0xF3, 0xA4,                                                 // +104  rep movs    byte ptr [rdi],byte ptr [rsi]  
		0x41, 0x5B,                                                 // +106  pop         r11  
		0x41, 0x5A,                                                 // +108  pop         r10  
		0x41, 0x59,                                                 // +110  pop         r9  
		0x41, 0x58,                                                 // +112  pop         r8  
		0x5A,                                                       // +114  pop         rdx  
		0x59,                                                       // +115  pop         rcx  
		0xFF, 0xE0                                                  // +116  jmp         rax  
	};

	LPVOID pShellcode = VirtualAllocEx(
		hProc,
		nullptr,
		sizeof(preShellCode) + sizeof(NtUserPeekMessageOriginalBYTE) + Normal_Function_Length,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_EXECUTE_READWRITE);

	if (!pShellcode)
	{
		VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
		VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
		return false;
	}

	*(DWORD64*)(preShellCode + 28) = (DWORD64)pMappingData;										//save MappingData
	*(DWORD64*)(preShellCode + 38) = (DWORD64)((BYTE*)pShellcode + sizeof(preShellCode) + sizeof(NtUserPeekMessageOriginalBYTE));  //ShellCode Address
	*(DWORD64*)(preShellCode + 76) = (DWORD64)((BYTE*)pShellcode + sizeof(preShellCode));
	*(DWORD64*)(preShellCode + 86) = (DWORD64)(addressOfHookFunction);

	//preshellcode writing
	WriteProcessMemory(hProc, pShellcode, preShellCode, sizeof(preShellCode), nullptr);
	//HookedFunction originalbytes writing
	WriteProcessMemory(hProc, (BYTE*)pShellcode + sizeof(preShellCode), NtUserPeekMessageOriginalBYTE, sizeof(NtUserPeekMessageOriginalBYTE), nullptr);
	//Shellcode function writing
	WriteProcessMemory(hProc, (BYTE*)pShellcode + sizeof(preShellCode) + sizeof(NtUserPeekMessageOriginalBYTE), Shellcode, Normal_Function_Length, nullptr);

	//쉘코드 실행을 위해 NtUserTranslateMessage 후킹하기
	
	BYTE NtUserPeekMessageBYTEPATCH[12] =
	{
		0x48, 0xB8, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,  // +0   mov         rax,8888888888888888h  
		0x50,                                                        // +10  push        rax  
		0xC3                                                         // +11  ret  
	};

	DWORD OldProtection = 0;
	if (!VirtualProtectEx(hProc, addressOfHookFunction, 12, PAGE_EXECUTE_READWRITE, &OldProtection))   // 페이지를 수정할 수 있게 바꾼다.
		return false;

	*(DWORD64*)(NtUserPeekMessageBYTEPATCH + 2) = (DWORD64)pShellcode;
	WriteProcessMemory(hProc, addressOfHookFunction, NtUserPeekMessageBYTEPATCH, sizeof(NtUserPeekMessageBYTEPATCH), nullptr);


	//쉘코드가 끝났는 지 체크한다.

	auto TIME = clock();
	MANUAL_MAPPING_DATA data_checked = { 0 };
	do
	{
		ReadProcessMemory(hProc, pMappingData, &data_checked, sizeof(MANUAL_MAPPING_DATA), nullptr);
		Sleep(10);
		if (((clock() - TIME) / CLOCKS_PER_SEC) > 3) {       // timeout
			data_checked.Signal = 1;
			break;
		}
	} while (data_checked.Signal == 2);
	Sleep(100); //프리 쉘코드가 쉘코드 함수를 호출 후 나머지 언훅과 원본 함수를 호출하기가 5초 안에 이뤄지기를 바라면서 기다렸다가 메모리를 해제

	if (data_checked.Signal == 0) {
		BYTE* byte = new BYTE[pOldOptHeader->SizeOfHeaders];
		ZeroMemory(byte, pOldOptHeader->SizeOfHeaders);
		WriteProcessMemory(hProc, pTargetBase, byte, pOldOptHeader->SizeOfHeaders, nullptr); // Erasing PE Headers
		delete[] byte;
	}
	
	VirtualProtectEx(hProc, addressOfHookFunction, 12, OldProtection, &OldProtection);       // 페이지 속성 복구
	VirtualFreeEx(hProc, pShellcode, 0, MEM_RELEASE);                                 // 쉘코드 해제
	VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);                               // 매핑 데이터 해제
	
	return data_checked.Signal ? false : true;
}