/* Запуск программы в job-объекте с лимитом памяти процесса: joblimit <МБ> "<командная строка>"
   Печатает код завершения и пик памяти. Нужен tests\run_tests.ps1. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        printf("usage: joblimit <MB> \"<cmdline>\"\n");
        return 2;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    li.ProcessMemoryLimit = (SIZE_T)_wtoi(argv[1]) << 20;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, argv[2], NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed: %lu\n", GetLastError());
        return 2;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION q = {0};
    QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &q, sizeof q, NULL);
    printf("exit=0x%lx peak=%llu MB\n", ec, (unsigned long long)(q.PeakProcessMemoryUsed >> 20));
    return ec == 0 ? 0 : 1;
}
