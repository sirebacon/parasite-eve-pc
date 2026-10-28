#ifndef PE1_CRASH_REPORT_H
#define PE1_CRASH_REPORT_H

#include <stddef.h>

/* Starts the crash report: a log file of everything the game prints (when
 * interactive) and, after a crash, one text file with what went wrong, every
 * thread's call stack, the game's state, the mods, the PC and the log.
 * interactive also shows a message box after a crash.
 * describe fills in what the game was doing; POSIX runs it on a reporter thread,
 * Windows inside the exception handler,
 * so it may only read values. Windows: crash_report.c; Linux and macOS:
 * crash_report_posix.c; does nothing in the web build. */
void CrashReport_Install(int interactive, void (*describe)(char *out, size_t size));

/* Adds a Mods section to crash reports. describe runs inside the crash
 * reporter; on_game_thread: the crash was on the thread that runs mod code. */
void CrashReport_SetMods(void (*describe)(char *out, size_t size, int on_game_thread));

/* Adds the game's save data near the end of crash reports (after the loaded
 * modules, before the log), so a save can be rebuilt from a report. describe
 * runs during reporting and must only read previously published state. */
void CrashReport_SetSaveData(void (*describe)(char *out, size_t size));

#endif
