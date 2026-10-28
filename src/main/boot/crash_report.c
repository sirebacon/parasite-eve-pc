/* Crash reports for players.
 *
 * Everything the game prints still reaches the console and is also kept in
 * logs\parasite-eve-log.txt next to the executable (or in
 * %LOCALAPPDATA%\Parasite Eve\logs when that folder is read-only) while the
 * game runs; a normal quit deletes it. After a crash that log becomes one
 * file, logs\crash-<date>.txt: what went wrong and where, every thread's call
 * stack, registers and memory around the crash, the game's state, the mods,
 * the PC, the loaded modules, then the whole log.
 * A message box says where the file is.
 *
 * Function names come from the executable's own symbol table, so a report
 * from any build can be read without that build at hand (unless the build was
 * stripped: the log's header says so).
 *
 * Nothing here runs per frame: two threads wait on pipes and only wake up when
 * the game prints something. */
#include "pe1/crash_report.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#ifndef PE_VERSION
#define PE_VERSION "unknown"
#endif

/* The report plus the log stays well under Discord's 10 MB upload limit. */
#define CRASH_LOG_LIMIT (4u << 20)
#define CRASH_TAIL_SIZE (256u << 10)
#define CRASH_TEXT_SIZE (1u << 20)
#define CRASH_MAX_THREADS 64
#define CRASH_MAX_FRAMES 48
#define CRASH_ABORT_CODE 0xE0000ABCu

static struct {
    wchar_t dir[MAX_PATH];
    HANDLE log;
    DWORD log_size;
    HANDLE pipes[2];
    HANDLE console[2];
    volatile LONG busy;
    CRITICAL_SECTION lock;
    char tail[CRASH_TAIL_SIZE];
    size_t tail_pos;
    int tail_wrapped;
    int interactive;
    DWORD game_thread;
    ULONGLONG started;
    char system[4096];
    void (*describe)(char *out, size_t size);
    void (*describe_mods)(char *out, size_t size, int on_game_thread);
    void (*describe_save)(char *out, size_t size);
    volatile LONG crashing;
    volatile DWORD report_thread;
    wchar_t report_path[MAX_PATH];
} crash = {.log = INVALID_HANDLE_VALUE};

/* MinGW/UCRT can route an access violation through a CRT signal before the
 * top-level exception filter. Keep the original registers for that path;
 * first-chance exceptions otherwise continue through the application's handlers. */
#ifdef _MSC_VER
#define CRASH_TLS __declspec(thread)
#else
#define CRASH_TLS __thread
#endif
static CRASH_TLS CONTEXT crash_fault_context;
static CRASH_TLS EXCEPTION_RECORD crash_fault_record;
static CRASH_TLS int crash_fault_valid;
static LONG CALLBACK Crash_CacheException(EXCEPTION_POINTERS *info) {
    DWORD code=info->ExceptionRecord->ExceptionCode;
    if(code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR ||
       code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_INT_DIVIDE_BY_ZERO ||
       code==EXCEPTION_FLT_DIVIDE_BY_ZERO || code==EXCEPTION_STACK_OVERFLOW) {
        crash_fault_context=*info->ContextRecord;
        crash_fault_record=*info->ExceptionRecord;
        crash_fault_valid=1;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static HANDLE crash_watch_event;
static HANDLE crash_emergency=INVALID_HANDLE_VALUE;
static wchar_t crash_emergency_path[MAX_PATH];
static volatile LONG crash_report_written,crash_emergency_started;

static DWORD WINAPI Crash_Watchdog(void *unused) {
    (void)unused;WaitForSingleObject(crash_watch_event,INFINITE);Sleep(10000);
    if(!InterlockedCompareExchange(&crash_report_written,0,0))TerminateProcess(GetCurrentProcess(),3);
    return 0;
}

static void Crash_EmergencyField(const char *name,uintptr_t value) {
    char text[96];size_t n=0;DWORD written;
    while(*name && n<sizeof(text)-2*sizeof(value)-4)text[n++]=*name++;
    text[n++]='0';text[n++]='x';
    for(int shift=(int)(sizeof(value)*8)-4;shift>=0;shift-=4)text[n++]="0123456789abcdef"[(value>>shift)&15];
    text[n++]='\n';WriteFile(crash_emergency,text,(DWORD)n,&written,NULL);
}

static void Crash_BeginEmergency(const EXCEPTION_POINTERS *info) {
    if(InterlockedExchange(&crash_emergency_started,1))return;
    DWORD written;
    static const char header[]="Parasite Eve emergency crash report\nFull reporting may be blocked by a lock held at the crash.\n";
    WriteFile(crash_emergency,header,sizeof(header)-1,&written,NULL);
    Crash_EmergencyField("Exception: ",info->ExceptionRecord->ExceptionCode);
    Crash_EmergencyField("Thread: ",GetCurrentThreadId());
    Crash_EmergencyField("PC: ",(uintptr_t)info->ExceptionRecord->ExceptionAddress);
    if(!crash_watch_event)TerminateProcess(GetCurrentProcess(),3);
    SetEvent(crash_watch_event);
}

static char crash_text[CRASH_TEXT_SIZE];
static size_t crash_text_size;

static void Crash_Path(wchar_t *out, const wchar_t *name, const wchar_t *suffix) {
    _snwprintf(out, MAX_PATH, L"%ls\\%ls%ls", crash.dir, name, suffix);
    out[MAX_PATH - 1] = 0;
}

/* ---- the log ---- */

/* A full log starts over with its last 256 KB, so a crash late in a long
 * session still has what led up to it. */
static void Crash_RestartLog(void) {
    static const char note[] = "[log reached 4 MB and started over; the 256 KB before that follow]\r\n";
    DWORD written;

    SetFilePointer(crash.log, 0, NULL, FILE_BEGIN);
    SetEndOfFile(crash.log);
    WriteFile(crash.log, note, sizeof(note) - 1, &written, NULL);
    crash.log_size = sizeof(note) - 1;
    if (crash.tail_wrapped) {
        WriteFile(crash.log, crash.tail + crash.tail_pos, (DWORD)(CRASH_TAIL_SIZE - crash.tail_pos), &written, NULL);
        crash.log_size += (DWORD)(CRASH_TAIL_SIZE - crash.tail_pos);
    }
    WriteFile(crash.log, crash.tail, (DWORD)crash.tail_pos, &written, NULL);
    crash.log_size += (DWORD)crash.tail_pos;
}

static void Crash_Keep(const char *data, DWORD size) {
    DWORD written;

    EnterCriticalSection(&crash.lock);
    if (crash.log != INVALID_HANDLE_VALUE) {
        if (crash.log_size + size > CRASH_LOG_LIMIT) {
            Crash_RestartLog();
        }
        WriteFile(crash.log, data, size, &written, NULL);
        crash.log_size += size;
    }
    while (size) {
        size_t part = CRASH_TAIL_SIZE - crash.tail_pos;
        if (part > size) {
            part = size;
        }
        memcpy(crash.tail + crash.tail_pos, data, part);
        crash.tail_pos += part;
        if (crash.tail_pos == CRASH_TAIL_SIZE) {
            crash.tail_pos = 0;
            crash.tail_wrapped = 1;
        }
        data += part;
        size -= (DWORD)part;
    }
    LeaveCriticalSection(&crash.lock);
}

static DWORD WINAPI Crash_Forward(void *param) {
    int stream = (int)(intptr_t)param;
    char buffer[4096];
    DWORD size, written;

    while (ReadFile(crash.pipes[stream], buffer, sizeof(buffer), &size, NULL) && size) {
        InterlockedIncrement(&crash.busy);
        if (crash.console[stream]) {
            WriteFile(crash.console[stream], buffer, size, &written, NULL);
        }
        Crash_Keep(buffer, size);
        InterlockedDecrement(&crash.busy);
    }
    return 0;
}

/* Gives the threads time to pass on everything printed so far. (The pipes
 * cannot be checked: a pipe is locked while its thread waits on it.) */
static void Crash_Drain(DWORD timeout_ms) {
    if (!crash.pipes[0] && !crash.pipes[1]) {
        return;
    }
    Sleep(50);
    for (DWORD waited = 50; waited < timeout_ms && crash.busy; waited += 5) {
        Sleep(5);
    }
}

/* A normal quit leaves no file behind: the log is only kept inside a crash
 * report. (A crash after this still gets the last 256 KB in its report.) */
static void Crash_Finish(void) {
    wchar_t path[MAX_PATH];

    fflush(stdout);
    fflush(stderr);
    Crash_Drain(500);
    EnterCriticalSection(&crash.lock);
    if (crash.log != INVALID_HANDLE_VALUE && !crash.crashing) {
        CloseHandle(crash.log);
        crash.log = INVALID_HANDLE_VALUE;
        Crash_Path(path, L"parasite-eve-log", L".txt");
        DeleteFileW(path);
    }
    LeaveCriticalSection(&crash.lock);
    if(!crash.crashing && crash_emergency!=INVALID_HANDLE_VALUE) {
        CloseHandle(crash_emergency);crash_emergency=INVALID_HANDLE_VALUE;DeleteFileW(crash_emergency_path);
    }
}

/* Sends stdout (0) or stderr (1) through a pipe, so it can be copied to both
 * the console and the log. */
static int Crash_Redirect(int stream) {
    DWORD id = stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    FILE *file = stream ? stderr : stdout;
    HANDLE process = GetCurrentProcess();
    HANDLE original = GetStdHandle(id);
    HANDLE read, write;

    fflush(file);
    if (original && original != INVALID_HANDLE_VALUE &&
        !DuplicateHandle(process, original, process, &crash.console[stream], 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        crash.console[stream] = NULL;
    }
    if (!CreatePipe(&read, &write, NULL, 64u << 10)) {
        return 0;
    }
    int fd = _open_osfhandle((intptr_t)write, _O_TEXT);
    if (fd < 0) {
        CloseHandle(read);
        CloseHandle(write);
        return 0;
    }
    if (_dup2(fd, stream + 1)) {
        _close(fd);
        CloseHandle(read);
        return 0;
    }
    SetStdHandle(id, (HANDLE)_get_osfhandle(stream + 1));
    setvbuf(file, NULL, _IONBF, 0);
    crash.pipes[stream] = read;
    HANDLE thread = CreateThread(NULL, 64u << 10, Crash_Forward, (void *)(intptr_t)stream, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    }
    return thread != NULL;
}

static int Crash_TryFolder(const wchar_t *base) {
    wchar_t probe[MAX_PATH];

    _snwprintf(crash.dir, MAX_PATH, L"%ls\\logs", base);
    crash.dir[MAX_PATH - 1] = 0;
    if (!CreateDirectoryW(crash.dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    Crash_Path(probe, L".write-test", L"");
    HANDLE file = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CloseHandle(file);
    return 1;
}

static int Crash_FindFolder(void) {
    wchar_t base[MAX_PATH];
    DWORD size = GetModuleFileNameW(NULL, base, MAX_PATH);

    if (size && size < MAX_PATH) {
        wchar_t *slash = wcsrchr(base, L'\\');
        if (slash) {
            *slash = 0;
            if (Crash_TryFolder(base)) {
                return 1;
            }
        }
    }
    size = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH - 16);
    if (size && size < MAX_PATH - 16) {
        wcscat(base, L"\\Parasite Eve");
        CreateDirectoryW(base, NULL);
        if (Crash_TryFolder(base)) {
            return 1;
        }
    }
    crash.dir[0] = 0;
    return 0;
}

static int Crash_OpenLog(void) {
    wchar_t path[MAX_PATH];
    SYSTEMTIME now;
    char header[160];

    Crash_Path(path, L"parasite-eve-log", L".txt");
    crash.log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (crash.log == INVALID_HANDLE_VALUE) {
        return 0;
    }
    GetLocalTime(&now);
    int size = snprintf(header, sizeof(header), "Parasite Eve %s log, started %04u-%02u-%02u %02u:%02u:%02u\r\n",
                        PE_VERSION, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    Crash_Keep(header, (DWORD)size);
    Crash_Keep(crash.system, (DWORD)strlen(crash.system));
    Crash_Keep("\r\n", 2);
    return 1;
}

/* ---- report text ---- */

static void Crash_Append(const char *format, ...) {
    va_list args;

    if (crash_text_size >= sizeof(crash_text) - 1) {
        return;
    }
    va_start(args, format);
    int size = vsnprintf(crash_text + crash_text_size, sizeof(crash_text) - crash_text_size, format, args);
    va_end(args);
    if (size > 0) {
        crash_text_size += (size_t)size;
        if (crash_text_size > sizeof(crash_text) - 1) {
            crash_text_size = sizeof(crash_text) - 1;
        }
    }
}

static void Crash_Utf8(const wchar_t *text, char *out, size_t size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)size, NULL, NULL)) {
        out[0] = 0;
    }
}

static int Crash_Readable(DWORD64 address, size_t size) {
    for (DWORD64 page = address & ~(DWORD64)4095; page < address + size; page += 4096) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)(uintptr_t)page, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !info.Protect) {
            return 0;
        }
    }
    return 1;
}

/* ---- names for addresses ----
 * The executable keeps a COFF symbol table (functions and globals, static ones
 * too) unless it was stripped; it is read from the file only after a crash.
 * Other modules get their nearest export. */

static struct {
    const BYTE *symbols;
    DWORD count;
    const char *strings;
    DWORD strings_size;
    int tried;
} crash_names;

static const IMAGE_NT_HEADERS *Crash_Headers(const BYTE *base) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    return (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
}

static void Crash_LoadNames(void) {
    const IMAGE_NT_HEADERS *nt = Crash_Headers((const BYTE *)GetModuleHandleW(NULL));
    DWORD offset = nt->FileHeader.PointerToSymbolTable, count = nt->FileHeader.NumberOfSymbols;
    wchar_t path[MAX_PATH];
    DWORD read, strings = 0;
    LARGE_INTEGER position;

    crash_names.tried = 1;
    if (!offset || !count || !GetModuleFileNameW(NULL, path, MAX_PATH)) {
        return;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD table = count * IMAGE_SIZEOF_SYMBOL;
    position.QuadPart = (LONGLONG)offset + table;
    if (SetFilePointerEx(file, position, NULL, FILE_BEGIN) && ReadFile(file, &strings, 4, &read, NULL) &&
        read == 4 && strings >= 4 && strings < (64u << 20)) {
        BYTE *memory = VirtualAlloc(NULL, (SIZE_T)table + strings, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        position.QuadPart = offset;
        if (memory && SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
            ReadFile(file, memory, table + strings, &read, NULL) && read == table + strings) {
            crash_names.symbols = memory;
            crash_names.count = count;
            crash_names.strings = (const char *)memory + table;
            crash_names.strings_size = strings;
        }
    }
    CloseHandle(file);
}

/* Nearest symbol at or before rva in the same section of the executable. */
static int Crash_ExeName(DWORD rva, char *name, size_t size, DWORD *offset) {
    const IMAGE_NT_HEADERS *nt = Crash_Headers((const BYTE *)GetModuleHandleW(NULL));
    const IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
    int section = -1;
    DWORD best = 0;
    const IMAGE_SYMBOL *found = NULL;

    if (!crash_names.tried) {
        Crash_LoadNames();
    }
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        DWORD extent = sections[i].Misc.VirtualSize > sections[i].SizeOfRawData ? sections[i].Misc.VirtualSize
                                                                               : sections[i].SizeOfRawData;
        if (rva >= sections[i].VirtualAddress && rva < sections[i].VirtualAddress + extent) {
            section = i;
        }
    }
    if (section < 0 || !crash_names.symbols) {
        return 0;
    }
    for (DWORD i = 0; i < crash_names.count; ++i) {
        const IMAGE_SYMBOL *symbol = (const IMAGE_SYMBOL *)(crash_names.symbols + (size_t)i * IMAGE_SIZEOF_SYMBOL);
        if (symbol->SectionNumber == section + 1 &&
            (symbol->StorageClass == IMAGE_SYM_CLASS_EXTERNAL || symbol->StorageClass == IMAGE_SYM_CLASS_STATIC)) {
            DWORD start = sections[section].VirtualAddress + symbol->Value;
            const char *text = symbol->N.Name.Short ? (const char *)symbol->N.ShortName : NULL;
            if (!text && symbol->N.Name.Long < crash_names.strings_size) {
                text = crash_names.strings + symbol->N.Name.Long;
            }
            if (text && text[0] != '.' && start <= rva && start >= best) {
                best = start;
                found = symbol;
            }
        }
        i += symbol->NumberOfAuxSymbols;
    }
    if (!found) {
        return 0;
    }
    if (found->N.Name.Short) {
        size_t length = strnlen((const char *)found->N.ShortName, 8);
        snprintf(name, size, "%.*s", (int)length, (const char *)found->N.ShortName);
    } else {
        snprintf(name, size, "%.*s", 160, crash_names.strings + found->N.Name.Long);
    }
    *offset = rva - best;
    return 1;
}

static int Crash_ExportName(const BYTE *base, DWORD rva, char *name, size_t size, DWORD *offset) {
    const IMAGE_DATA_DIRECTORY *directory =
        &Crash_Headers(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory->VirtualAddress || !directory->Size) {
        return 0;
    }
    const IMAGE_EXPORT_DIRECTORY *exports = (const IMAGE_EXPORT_DIRECTORY *)(base + directory->VirtualAddress);
    const DWORD *functions = (const DWORD *)(base + exports->AddressOfFunctions);
    const DWORD *names = (const DWORD *)(base + exports->AddressOfNames);
    const WORD *ordinals = (const WORD *)(base + exports->AddressOfNameOrdinals);
    DWORD best = 0;
    const char *found = NULL;

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        DWORD start = ordinals[i] < exports->NumberOfFunctions ? functions[ordinals[i]] : 0;
        int forwarder = start >= directory->VirtualAddress && start < directory->VirtualAddress + directory->Size;
        if (start && !forwarder && start <= rva && start >= best) {
            best = start;
            found = (const char *)(base + names[i]);
        }
    }
    if (!found) {
        return 0;
    }
    snprintf(name, size, "%.*s", 160, found);
    *offset = rva - best;
    return 1;
}

/* "Function+0x12  (module.exe+0x1234)" or "module.dll+0x1234 (Export+0x12)";
 * empty when the address is in no module. */
static int Crash_AppendAddress(DWORD64 address) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    char module_name[MAX_PATH], name[192];
    DWORD offset;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const wchar_t *)(uintptr_t)address, &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH)) {
        return 0;
    }
    const wchar_t *base_name = wcsrchr(path, L'\\');
    Crash_Utf8(base_name ? base_name + 1 : path, module_name, sizeof(module_name));
    DWORD rva = (DWORD)(address - (DWORD64)(uintptr_t)module);
    if (module == GetModuleHandleW(NULL)) {
        if (Crash_ExeName(rva, name, sizeof(name), &offset)) {
            Crash_Append("%s+0x%lX  (%s+0x%lX)", name, offset, module_name, rva);
        } else {
            Crash_Append("%s+0x%lX", module_name, rva);
        }
    } else if (Crash_ExportName((const BYTE *)module, rva, name, sizeof(name), &offset) && offset < 0x4000) {
        Crash_Append("%s+0x%lX (near export %s+0x%lX)", module_name, rva, name, offset);
    } else {
        Crash_Append("%s+0x%lX", module_name, rva);
    }
    return 1;
}

/* ---- call stacks ---- */

typedef struct {
    DWORD id;
    HANDLE handle;
    int crashed;
    int frame_count;
    DWORD64 frames[CRASH_MAX_FRAMES];
} CrashThread;

static CrashThread crash_threads[CRASH_MAX_THREADS];
static int crash_thread_count;

#if defined(_M_X64) || defined(__x86_64__)
static int Crash_Unwind(CONTEXT context, DWORD64 *frames, int max) {
    int count = 0;

    while (count < max && context.Rip && Crash_Readable(context.Rsp, 8)) {
        DWORD64 image_base = 0, previous = context.Rsp;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(count ? context.Rip - 1 : context.Rip, &image_base, NULL);

        frames[count++] = context.Rip;
        if (function) {
            void *handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                             &establisher, NULL);
        } else {
            /* A leaf function, or a jump to a bad address: the return address is on top. */
            context.Rip = *(const DWORD64 *)(uintptr_t)context.Rsp;
            context.Rsp += 8;
        }
        if (context.Rsp <= previous) {
            break;
        }
    }
    return count;
}
#endif

/* Every thread's frames are taken first, each thread stopped only while its
 * stack is read; names and the text come after, with all threads running. */
static void Crash_TakeStacks(const EXCEPTION_POINTERS *info) {
    CrashThread *thread = &crash_threads[0];
    DWORD self = GetCurrentThreadId();

    crash_thread_count = 1;
    thread->id = self;
    thread->crashed = 1;
    thread->handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, self);
#if defined(_M_X64) || defined(__x86_64__)
    thread->frame_count = Crash_Unwind(*info->ContextRecord, thread->frames, CRASH_MAX_FRAMES);
#else
    /* Other CPUs: only where it crashed. */
    thread->frames[0] = (DWORD64)(uintptr_t)info->ExceptionRecord->ExceptionAddress;
    thread->frame_count = 1;
#endif

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && crash_thread_count < CRASH_MAX_THREADS;
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
            continue;
        }
        thread = &crash_threads[crash_thread_count];
        memset(thread, 0, sizeof(*thread));
        thread->id = entry.th32ThreadID;
        thread->handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                    FALSE, entry.th32ThreadID);
        if (!thread->handle) {
            continue;
        }
        ++crash_thread_count;
#if defined(_M_X64) || defined(__x86_64__)
        static CONTEXT context;
        if (SuspendThread(thread->handle) != (DWORD)-1) {
            memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread->handle, &context)) {
                thread->frame_count = Crash_Unwind(context, thread->frames, CRASH_MAX_FRAMES);
            }
            ResumeThread(thread->handle);
        }
#endif
    }
    CloseHandle(snapshot);
}

static void Crash_AppendStacks(void) {
    HRESULT (WINAPI *get_name)(HANDLE, PWSTR *) = (HRESULT (WINAPI *)(HANDLE, PWSTR *))(void *)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");

    Crash_Append("\r\n== Call stacks (newest call first) ==\r\n");
    for (int i = 0; i < crash_thread_count; ++i) {
        CrashThread *thread = &crash_threads[i];
        char name[256] = "";
        PWSTR wide = NULL;

        if (get_name && thread->handle && SUCCEEDED(get_name(thread->handle, &wide)) && wide) {
            Crash_Utf8(wide, name, sizeof(name));
            LocalFree(wide);
        }
        Crash_Append("\r\nThread %lu%s%s%s%s%s:\r\n", thread->id, name[0] ? " \"" : "", name, name[0] ? "\"" : "",
                     thread->id == crash.game_thread ? " (game thread)" : "", thread->crashed ? " - CRASHED HERE" : "");
        if (!thread->frame_count) {
            Crash_Append("  (stack could not be read)\r\n");
        }
        for (int frame = 0; frame < thread->frame_count; ++frame) {
            Crash_Append("  %2d  ", frame);
            if (!Crash_AppendAddress(thread->frames[frame])) {
                Crash_Append("0x%llX (not in any module)", (unsigned long long)thread->frames[frame]);
            }
            Crash_Append("\r\n");
        }
        if (thread->handle) {
            CloseHandle(thread->handle);
        }
    }
}

/* ---- the PC ---- */

static void Crash_Registry(HKEY root, const wchar_t *key, const wchar_t *value, char *out, size_t size) {
    wchar_t text[512];
    DWORD bytes = sizeof(text);

    out[0] = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, NULL, text, &bytes) == ERROR_SUCCESS) {
        Crash_Utf8(text, out, size);
    }
}

static size_t Crash_SystemAppend(size_t used, const char *format, ...) {
    va_list args;

    if (used >= sizeof(crash.system)) {
        return used;
    }
    va_start(args, format);
    int size = vsnprintf(crash.system + used, sizeof(crash.system) - used, format, args);
    va_end(args);
    return size > 0 ? used + (size_t)size : used;
}

/* The exe's identity, so a report can be matched to its build. */
static size_t Crash_DescribeExecutable(size_t used) {
    const BYTE *base = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *nt = Crash_Headers(base);
    const IMAGE_DATA_DIRECTORY *debug = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    char name[MAX_PATH] = "?", id[64] = "none";

    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        const wchar_t *slash = wcsrchr(path, L'\\');
        Crash_Utf8(slash ? slash + 1 : path, name, sizeof(name));
    }
    for (DWORD at = 0; debug->VirtualAddress && at + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug->Size;
         at += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        const IMAGE_DEBUG_DIRECTORY *entry = (const IMAGE_DEBUG_DIRECTORY *)(base + debug->VirtualAddress + at);
        const BYTE *record = entry->AddressOfRawData ? base + entry->AddressOfRawData : NULL;
        if (entry->Type == IMAGE_DEBUG_TYPE_CODEVIEW && record && entry->SizeOfData >= 24 && !memcmp(record, "RSDS", 4)) {
            const BYTE *g = record + 4;
            snprintf(id, sizeof(id), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X-%lu",
                     g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13],
                     g[14], g[15], (unsigned long)*(const DWORD *)(record + 20));
        }
    }
    unsigned long long size = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &file)) {
        size = ((unsigned long long)file.nFileSizeHigh << 32) | file.nFileSizeLow;
    }
    used = Crash_SystemAppend(used, "Executable: %s, version %s, %llu bytes, link stamp 0x%08lX, build id %s\r\n",
                              name, PE_VERSION, size, (unsigned long)nt->FileHeader.TimeDateStamp, id);
    if (nt->FileHeader.PointerToSymbolTable && nt->FileHeader.NumberOfSymbols) {
        used = Crash_SystemAppend(used, "Function names: in the executable (%lu symbols)\r\n",
                                  (unsigned long)nt->FileHeader.NumberOfSymbols);
    } else {
        used = Crash_SystemAppend(used, "Function names: NONE, this build was stripped; crash reports only "
                                        "have addresses and need this exact build to be read\r\n");
    }
    return used;
}

static void Crash_DescribeSystem(void) {
    OSVERSIONINFOW version;
    LONG (WINAPI *get_version)(OSVERSIONINFOW *) =
        (LONG (WINAPI *)(OSVERSIONINFOW *))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    static const wchar_t windows_key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    static const wchar_t display_key[] =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    char text[512], extra[256];
    DWORD revision = 0, bytes = sizeof(revision);
    size_t used = 0;

    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version) {
        get_version(&version);
    }
    RegGetValueW(HKEY_LOCAL_MACHINE, windows_key, L"UBR", RRF_RT_REG_DWORD, NULL, &revision, &bytes);
    Crash_Registry(HKEY_LOCAL_MACHINE, windows_key, L"DisplayVersion", text, sizeof(text));
    if (!text[0]) {
        Crash_Registry(HKEY_LOCAL_MACHINE, windows_key, L"ReleaseId", text, sizeof(text));
    }
    used = Crash_SystemAppend(used, "Windows: %lu.%lu.%lu.%lu%s%s%s\r\n", version.dwMajorVersion,
                              version.dwMinorVersion, version.dwBuildNumber, revision, text[0] ? " (" : "", text,
                              text[0] ? ")" : "");

    Crash_Registry(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   L"ProcessorNameString", text, sizeof(text));
    used = Crash_SystemAppend(used, "CPU: %s, %lu logical processors\r\n", text[0] ? text : "?",
                              (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        used = Crash_SystemAppend(used, "Memory: %.1f GB, %.1f GB free at start\r\n",
                                  (double)memory.ullTotalPhys / 1073741824.0, (double)memory.ullAvailPhys / 1073741824.0);
    }

    DEVMODEW mode;
    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode)) {
        used = Crash_SystemAppend(used, "Main display: %lux%lu at %lu Hz\r\n", mode.dmPelsWidth, mode.dmPelsHeight,
                                  mode.dmDisplayFrequency);
    }

    HKEY adapters;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, display_key, 0, KEY_READ, &adapters) == ERROR_SUCCESS) {
        wchar_t sub[64], key[256];
        for (DWORD i = 0; i < 16; ++i) {
            DWORD length = 64;
            if (RegEnumKeyExW(adapters, i, sub, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
                break;
            }
            if (sub[0] < L'0' || sub[0] > L'9') {
                continue;
            }
            _snwprintf(key, 256, L"%ls\\%ls", display_key, sub);
            key[255] = 0;
            Crash_Registry(HKEY_LOCAL_MACHINE, key, L"DriverDesc", text, sizeof(text));
            if (!text[0]) {
                continue;
            }
            used = Crash_SystemAppend(used, "Graphics: %s", text);
            Crash_Registry(HKEY_LOCAL_MACHINE, key, L"DriverVersion", extra, sizeof(extra));
            used = Crash_SystemAppend(used, ", driver %s", extra[0] ? extra : "?");
            Crash_Registry(HKEY_LOCAL_MACHINE, key, L"DriverDate", extra, sizeof(extra));
            used = Crash_SystemAppend(used, "%s%s%s\r\n", extra[0] ? " (" : "", extra, extra[0] ? ")" : "");
        }
        RegCloseKey(adapters);
    }
    Crash_DescribeExecutable(used);
}

static void Crash_ModuleVersion(HMODULE module, char *out, size_t size) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    HGLOBAL loaded = resource ? LoadResource(module, resource) : NULL;
    const BYTE *data = loaded ? (const BYTE *)LockResource(loaded) : NULL;
    DWORD length = resource ? SizeofResource(module, resource) : 0;

    out[0] = 0;
    for (DWORD at = 0; data && at + sizeof(VS_FIXEDFILEINFO) <= length; at += 4) {
        const VS_FIXEDFILEINFO *info = (const VS_FIXEDFILEINFO *)(data + at);
        if (info->dwSignature == 0xFEEF04BD) {
            snprintf(out, size, "%lu.%lu.%lu.%lu", info->dwFileVersionMS >> 16, info->dwFileVersionMS & 0xFFFF,
                     info->dwFileVersionLS >> 16, info->dwFileVersionLS & 0xFFFF);
            return;
        }
    }
}

static void Crash_AppendModules(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32W entry;

    Crash_Append("\r\n== Loaded modules (overlays and injected tools show up here) ==\r\n");
    if (snapshot == INVALID_HANDLE_VALUE) {
        Crash_Append("  (list unavailable)\r\n");
        return;
    }
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        char name[MAX_PATH], version[64];
        Crash_Utf8(entry.szModule, name, sizeof(name));
        Crash_ModuleVersion(entry.hModule, version, sizeof(version));
        Crash_Append("  0x%016llX %8lu KB  %-32s %s\r\n", (unsigned long long)(uintptr_t)entry.modBaseAddr,
                     (unsigned long)(entry.modBaseSize >> 10), name, version);
    }
    CloseHandle(snapshot);
}

/* ---- the crash itself ---- */

static const char *Crash_Problem(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation (bad memory access)";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow (runaway recursion or a huge local array)";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "page could not be read from disk";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating-point operation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case CRASH_ABORT_CODE: return "the game stopped itself (abort; see the last log lines)";
    default: return "unexpected exception";
    }
}

static void Crash_AppendMemoryAt(DWORD64 address) {
    MEMORY_BASIC_INFORMATION info;

    if (!VirtualQuery((const void *)(uintptr_t)address, &info, sizeof(info))) {
        Crash_Append("outside the program's memory");
    } else if (info.State == MEM_FREE) {
        Crash_Append("nothing there (free memory%s)", address < 0x10000 ? ", near address 0: a null pointer" : "");
    } else if (info.State == MEM_RESERVE) {
        Crash_Append("reserved but not in use");
    } else {
        Crash_Append("in use, protection 0x%lX%s%s", info.Protect, info.Protect & PAGE_GUARD ? " (guard page)" : "",
                     info.Type == MEM_IMAGE ? ", inside " : "");
        if (info.Type == MEM_IMAGE) {
            Crash_AppendAddress(address);
        }
    }
}

static void Crash_AppendBytes(const char *title, DWORD64 address, size_t size) {
    const BYTE *bytes = (const BYTE *)(uintptr_t)address;

    if (!Crash_Readable(address, size)) {
        Crash_Append("%s: (not readable at 0x%llX)\r\n", title, (unsigned long long)address);
        return;
    }
    Crash_Append("%s:\r\n", title);
    for (size_t row = 0; row < size; row += 16) {
        Crash_Append("  %016llX ", (unsigned long long)(address + row));
        for (size_t i = row; i < row + 16 && i < size; ++i) {
            Crash_Append(" %02X", bytes[i]);
        }
        Crash_Append("\r\n");
    }
}

static void Crash_AppendRegister(const char *name, DWORD64 value) {
    Crash_Append("  %-3s %016llX", name, (unsigned long long)value);
    if (value >= 0x10000) {
        size_t before = crash_text_size;
        Crash_Append("  ");
        if (!Crash_AppendAddress(value)) {
            crash_text_size = before;
            crash_text[before] = 0;
        }
    }
    Crash_Append("\r\n");
}

static void Crash_AppendDetails(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;

    Crash_Append("\r\n== Details ==\r\n");
    Crash_Append("Exception: 0x%08lX, flags 0x%lX, at 0x%llX", record->ExceptionCode, record->ExceptionFlags,
                 (unsigned long long)(uintptr_t)record->ExceptionAddress);
    for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        Crash_Append("%s0x%llX", i ? ", " : ", parameters ", (unsigned long long)record->ExceptionInformation[i]);
    }
    Crash_Append("\r\n");
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        Crash_Append("Memory at the bad address: ");
        Crash_AppendMemoryAt(record->ExceptionInformation[1]);
        Crash_Append("\r\n");
    }
#if defined(_M_X64) || defined(__x86_64__)
    Crash_Append("\r\nRegisters (with what they point to):\r\n");
    Crash_AppendRegister("rip", c->Rip);
    Crash_AppendRegister("rsp", c->Rsp);
    Crash_AppendRegister("rbp", c->Rbp);
    Crash_AppendRegister("rax", c->Rax);
    Crash_AppendRegister("rbx", c->Rbx);
    Crash_AppendRegister("rcx", c->Rcx);
    Crash_AppendRegister("rdx", c->Rdx);
    Crash_AppendRegister("rsi", c->Rsi);
    Crash_AppendRegister("rdi", c->Rdi);
    Crash_AppendRegister("r8", c->R8);
    Crash_AppendRegister("r9", c->R9);
    Crash_AppendRegister("r10", c->R10);
    Crash_AppendRegister("r11", c->R11);
    Crash_AppendRegister("r12", c->R12);
    Crash_AppendRegister("r13", c->R13);
    Crash_AppendRegister("r14", c->R14);
    Crash_AppendRegister("r15", c->R15);
    Crash_Append("  eflags %08lX\r\n", c->EFlags);
    Crash_Append("\r\n");
    Crash_AppendBytes("Code before and at the crash (the crash is at the start of the third row)", c->Rip - 32, 64);
    Crash_AppendBytes("Top of the stack", c->Rsp, 256);
#else
    (void)c;
#endif
}

static void Crash_BuildText(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    SYSTEMTIME now;
    ULONGLONG seconds = (GetTickCount64() - crash.started) / 1000;
    int on_game_thread = GetCurrentThreadId() == crash.game_thread;
    static char section[64u << 10];

    GetLocalTime(&now);
    crash_text_size = 0;
    Crash_Append("Parasite Eve %s crash report\r\n", PE_VERSION);
    Crash_Append("\r\n== What happened ==\r\n");
    Crash_Append("Problem: %s (0x%08lX)", Crash_Problem(record->ExceptionCode), record->ExceptionCode);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        ULONG_PTR kind = record->ExceptionInformation[0];
        Crash_Append(", %s address 0x%llX", kind == 0 ? "reading" : kind == 8 ? "running code at" : "writing",
                     (unsigned long long)record->ExceptionInformation[1]);
    }
    Crash_Append("\r\nWhere: ");
    if (!Crash_AppendAddress((DWORD64)(uintptr_t)record->ExceptionAddress)) {
        Crash_Append("0x%llX (not in any module)", (unsigned long long)(uintptr_t)record->ExceptionAddress);
    }
    Crash_Append("\r\nThread: %lu%s\r\n", GetCurrentThreadId(), on_game_thread ? " (game thread)" : " (not the game thread)");
    Crash_Append("Time: %04u-%02u-%02u %02u:%02u:%02u, %llu min %llu s after the game started\r\n", now.wYear,
                 now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, seconds / 60, seconds % 60);
    if (crash.describe) {
        section[0] = 0;
        crash.describe(section, sizeof(section));
        Crash_Append("%s", section);
    }

    if (crash.describe_mods) {
        Crash_Append("\r\n== Mods ==\r\n");
        section[0] = 0;
        crash.describe_mods(section, sizeof(section), on_game_thread);
        Crash_Append("%s", section);
    }

    Crash_AppendDetails(info);
    Crash_AppendStacks();

    Crash_Append("\r\n== Memory when it crashed ==\r\n");
    MEMORYSTATUSEX memory;
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        Crash_Append("PC: %.1f GB free of %.1f GB\r\n", (double)memory.ullAvailPhys / 1073741824.0,
                     (double)memory.ullTotalPhys / 1073741824.0);
    }
    BOOL (WINAPI *get_memory)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD) =
        (BOOL (WINAPI *)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD))(void *)
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    if (get_memory && get_memory(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters))) {
        Crash_Append("Game: %.0f MB in use (peak %.0f MB), %.0f MB private\r\n",
                     (double)counters.WorkingSetSize / 1048576.0, (double)counters.PeakWorkingSetSize / 1048576.0,
                     (double)counters.PrivateUsage / 1048576.0);
    }

    Crash_Append("\r\n== PC ==\r\n%s", crash.system);
    Crash_AppendModules();

    if (crash.describe_save) {
        section[0] = 0;
        crash.describe_save(section, sizeof(section));
        Crash_Append("%s", section);
    }
}

/* ---- telling the player ---- */

static BOOL CALLBACK Crash_HideWindow(HWND window, LPARAM param) {
    DWORD process = 0;
    (void)param;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() && window != GetConsoleWindow() && IsWindowVisible(window)) {
        ShowWindowAsync(window, SW_HIDE);
    }
    return TRUE;
}

static HHOOK crash_button_hook;

/* A message box cannot name its button, so the button is renamed (and
 * widened to fit) as the box opens. */
static LRESULT CALLBACK Crash_NameButton(int code, WPARAM wparam, LPARAM lparam) {
    static const wchar_t label[] = L"Open crash folder";

    if (code != HCBT_ACTIVATE) {
        return CallNextHookEx(crash_button_hook, code, wparam, lparam);
    }
    UnhookWindowsHookEx(crash_button_hook);
    crash_button_hook = NULL;
    HWND box = (HWND)wparam;
    HWND button = FindWindowExW(box, NULL, L"Button", NULL);
    if (button) {
        RECT rect;
        SIZE text = {0, 0};
        HDC dc = GetDC(button);
        HFONT font = (HFONT)SendMessageW(button, WM_GETFONT, 0, 0);
        HGDIOBJ previous = font ? SelectObject(dc, font) : NULL;
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &text);
        if (previous) {
            SelectObject(dc, previous);
        }
        ReleaseDC(button, dc);
        GetWindowRect(button, &rect);
        MapWindowPoints(NULL, box, (POINT *)&rect, 2);
        int width = text.cx + 2 * (rect.bottom - rect.top);
        if (width > rect.right - rect.left) {
            rect.left = rect.right - width > 8 ? rect.right - width : 8;
            SetWindowPos(button, NULL, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SetWindowTextW(button, label);
    }
    return 0;
}

static void Crash_Tell(const wchar_t *file, ULONGLONG size) {
    wchar_t path[MAX_PATH], text[2048], amount[32];

    Crash_Path(path, file, L"");
    if (!crash.interactive) {
        char line[MAX_PATH * 3 + 32];
        DWORD written;
        int length = snprintf(line, sizeof(line), "Crash report: %ls\r\n", path);
        HANDLE out = crash.console[1] ? crash.console[1] : GetStdHandle(STD_ERROR_HANDLE);
        if (length > 0 && out && out != INVALID_HANDLE_VALUE) {
            WriteFile(out, line, (DWORD)length, &written, NULL);
        }
        return;
    }

    if (size >= (1u << 20)) {
        _snwprintf(amount, 32, L"%.1f MB", (double)size / 1048576.0);
    } else {
        _snwprintf(amount, 32, L"%.0f KB", (double)size / 1024.0 + 0.5);
    }
    amount[31] = 0;
    EnumWindows(Crash_HideWindow, 0);
    _snwprintf(text, 2048,
               L"Sorry, Parasite Eve has crashed.\n\n"
               L"A crash report was saved. Please send this file to us on Discord:\n\n"
               L"    %ls  (%ls)\n\n"
               L"It is in this folder:\n%ls\n\n"
               L"Thank you for helping us with the game!",
               file, amount, crash.dir);
    text[2047] = 0;
    crash_button_hook = SetWindowsHookExW(WH_CBT, Crash_NameButton, NULL, GetCurrentThreadId());
    MessageBoxW(NULL, text, L"Parasite Eve", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    if (crash_button_hook) {
        UnhookWindowsHookEx(crash_button_hook);
    }

    wchar_t command[MAX_PATH + 32];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    _snwprintf(command, MAX_PATH + 32, L"explorer.exe /select,\"%ls\"", path);
    command[MAX_PATH + 31] = 0;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

/* The report, then the whole log: the start of the file, and if the log
 * outgrew its limit, its last 256 KB. */
static void Crash_WriteLog(HANDLE out) {
    static char buffer[64u << 10];
    wchar_t path[MAX_PATH];
    DWORD size, written;
    int locked = 0;

    Crash_Path(path, L"parasite-eve-log", L".txt");
    HANDLE log = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (log != INVALID_HANDLE_VALUE) {
        static const char title[] = "\r\n== Log of this run ==\r\n";
        WriteFile(out, title, sizeof(title) - 1, &written, NULL);
        while (ReadFile(log, buffer, sizeof(buffer), &size, NULL) && size) {
            WriteFile(out, buffer, size, &written, NULL);
        }
        CloseHandle(log);
    }
    for (int tries = 0; tries < 50 && !(locked = TryEnterCriticalSection(&crash.lock)); ++tries) {
        Sleep(2);
    }
    if (log == INVALID_HANDLE_VALUE && (crash.tail_wrapped || crash.tail_pos)) {
        static const char title[] = "\r\n== End of the log ==\r\n";
        WriteFile(out, title, sizeof(title) - 1, &written, NULL);
        if (crash.tail_wrapped) {
            WriteFile(out, crash.tail + crash.tail_pos, (DWORD)(CRASH_TAIL_SIZE - crash.tail_pos), &written, NULL);
        }
        WriteFile(out, crash.tail, (DWORD)crash.tail_pos, &written, NULL);
    }
    /* The log is in the report now; one file is left for the player. */
    if (crash.log != INVALID_HANDLE_VALUE) {
        CloseHandle(crash.log);
        crash.log = INVALID_HANDLE_VALUE;
        DeleteFileW(path);
    }
    if (locked) {
        LeaveCriticalSection(&crash.lock);
    }
}

/* Building the report crashed again (its sections run game and mod code, such
 * as the mods' save write handlers): keep what was built so far. */
static void Crash_KeepPartial(void) {
    static const char note[] = "\r\n== The report stops here ==\r\n"
                               "The game crashed again while this report was being written.\r\n";
    DWORD written;
    HANDLE file = CreateFileW(crash.report_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(file, crash_text, (DWORD)crash_text_size, &written, NULL);
    WriteFile(file, note, sizeof(note) - 1, &written, NULL);
    CloseHandle(file);
}

static void Crash_Report(EXCEPTION_POINTERS *info) {
    wchar_t name[64], path[MAX_PATH];
    SYSTEMTIME now;
    DWORD written;

    if (InterlockedExchange(&crash.crashing, 1) || !crash.dir[0]) {
        if (crash.report_thread == GetCurrentThreadId() && crash.report_path[0]) {
            Crash_KeepPartial();
        }
        TerminateProcess(GetCurrentProcess(), 3);
    }
    /* The faulting thread may own a CRT stream lock. */
    Crash_Drain(300);

    GetLocalTime(&now);
    _snwprintf(name, 64, L"crash-%04u-%02u-%02u_%02u-%02u-%02u.txt",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    name[63] = 0;
    Crash_Path(path, name, L"");
    memcpy(crash.report_path, path, sizeof(path));
    crash.report_thread = GetCurrentThreadId();

    Crash_TakeStacks(info);
    Crash_BuildText(info);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    int complete=WriteFile(file,crash_text,(DWORD)crash_text_size,&written,NULL) && written==crash_text_size;
    if(complete)Crash_WriteLog(file);
    LARGE_INTEGER size = {0};
    if(!GetFileSizeEx(file,&size) || size.QuadPart<(LONGLONG)crash_text_size)complete=0;
    if(!FlushFileBuffers(file))complete=0;
    CloseHandle(file);
    if(!complete){DeleteFileW(path);return;} /* preserve the emergency report */
    InterlockedExchange(&crash_report_written,1);
    if(crash_emergency!=INVALID_HANDLE_VALUE) {
        CloseHandle(crash_emergency);crash_emergency=INVALID_HANDLE_VALUE;DeleteFileW(crash_emergency_path);
    }
    Crash_Tell(name, (ULONGLONG)size.QuadPart);
}

static LONG WINAPI Crash_Filter(EXCEPTION_POINTERS *info) {
    Crash_BeginEmergency(info);
    Crash_Report(info);
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* abort() (including failed asserts) raises SIGABRT, not an exception. */
static void Crash_Abort(int signal_number) {
    CONTEXT context;
    EXCEPTION_RECORD record;
    EXCEPTION_POINTERS info = {&record, &context};

    (void)signal_number;
    memset(&record, 0, sizeof(record));
    RtlCaptureContext(&context);
    record.ExceptionCode = CRASH_ABORT_CODE;
#if defined(_M_X64) || defined(__x86_64__)
    record.ExceptionAddress = (void *)(uintptr_t)context.Rip;
#elif defined(_M_ARM64) || defined(__aarch64__)
    record.ExceptionAddress = (void *)(uintptr_t)context.Pc;
#else
    record.ExceptionAddress = (void *)(uintptr_t)context.Eip;
#endif
    if(signal_number!=SIGABRT && crash_fault_valid) {
        context=crash_fault_context;record=crash_fault_record;
    }
    Crash_BeginEmergency(&info);
    Crash_Report(&info);
    TerminateProcess(GetCurrentProcess(), 3);
}

void CrashReport_Install(int interactive, void (*describe)(char *out, size_t size)) {
    ULONG stack_reserve = 64u << 10;

    crash.interactive = interactive;
    crash.describe = describe;
    crash.game_thread = GetCurrentThreadId();
    crash.started = GetTickCount64();
    InitializeCriticalSection(&crash.lock);
    Crash_DescribeSystem();
    if (Crash_FindFolder() && interactive && Crash_OpenLog()) {
        Crash_Redirect(0);
        Crash_Redirect(1);
    }
    if(crash.dir[0]) {
        wchar_t name[80];SYSTEMTIME now;GetLocalTime(&now);
        _snwprintf(name,80,L"crash-%04u-%02u-%02u_%02u-%02u-%02u-%lu-emergency.txt",
            now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
        Crash_Path(crash_emergency_path,name,L"");
        crash_emergency=CreateFileW(crash_emergency_path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        crash_watch_event=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(crash_watch_event) {
            HANDLE thread=CreateThread(NULL,64u<<10,Crash_Watchdog,NULL,0,NULL);
            if(thread)CloseHandle(thread);
            else {CloseHandle(crash_watch_event);crash_watch_event=NULL;}
        }
        atexit(Crash_Finish);
    }
    /* Leaves room for the report after a stack overflow on the main thread. */
    SetThreadStackGuarantee(&stack_reserve);
    AddVectoredExceptionHandler(1,Crash_CacheException);
    SetUnhandledExceptionFilter(Crash_Filter);
    signal(SIGABRT, Crash_Abort);
    signal(SIGSEGV, Crash_Abort);
    signal(SIGILL, Crash_Abort);
    signal(SIGFPE, Crash_Abort);
}

void CrashReport_SetMods(void (*describe)(char *out, size_t size, int on_game_thread)) {
    crash.describe_mods = describe;
}

void CrashReport_SetSaveData(void (*describe)(char *out, size_t size)) {
    crash.describe_save = describe;
}

#elif !(defined(__linux__) || defined(__APPLE__)) || defined(__EMSCRIPTEN__)

/* Linux and macOS: crash_report_posix.c. */
void CrashReport_Install(int interactive, void (*describe)(char *out, size_t size)) {
    (void)interactive;
    (void)describe;
}

void CrashReport_SetMods(void (*describe)(char *out, size_t size, int on_game_thread)) {
    (void)describe;
}

void CrashReport_SetSaveData(void (*describe)(char *out, size_t size)) {
    (void)describe;
}

#endif
