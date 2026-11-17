// Mem Reduct - System Lag Reduction Submodule (Lag Reduct)
// Pure C + Windows Native API (ntdll / dwmapi) implementation

#pragma once

#include "routine.h"
#include "main.h"

// Lag reduction cleaning mask (three major lag sources)
#define LAG_REDUCT_HANDLE_LEAKS 0x01
#define LAG_REDUCT_DWM_BLOAT 0x02
#define LAG_REDUCT_ORPHAN_SANDBOX 0x04

#define LAG_REDUCT_MASK_ALL (LAG_REDUCT_HANDLE_LEAKS | LAG_REDUCT_DWM_BLOAT | LAG_REDUCT_ORPHAN_SANDBOX)
#define LAG_REDUCT_MASK_DEFAULT LAG_REDUCT_MASK_ALL

// Default thresholds (in thousands of handles, e.g. 450 = 450,000 handles)
#define DEFAULT_LAG_SYS_HANDLES_K 450
#define DEFAULT_LAG_PROC_HANDLES_K 50

// Cooldowns (in seconds)
#define LAG_AUTOREDUCT_COOLDOWN 120
#define LAG_DWM_RESET_COOLDOWN 600

// Undocumented / newer DWM window attributes (Windows 11+)
#if !defined(DWMWA_USE_IMMERSIVE_DARK_MODE)
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#if !defined(DWMWA_WINDOW_CORNER_PREFERENCE)
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#if !defined(DWMWA_SYSTEMBACKDROP_TYPE)
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

typedef struct _LAG_STATUS_INFO
{
	ULONG total_handles;
	ULONG total_processes;
	ULONG dwm_handles;
	ULONG dwm_pid;
	ULONG max_leaked_handles;
	ULONG max_leaked_pid;
	WCHAR max_leaked_name[64];
	ULONG64 nonpaged_pool_bytes;
	ULONG64 paged_pool_bytes;
	ULONG handle_pressure_percent;
} LAG_STATUS_INFO, *PLAG_STATUS_INFO;

typedef struct _LAG_CLEAN_RESULT
{
	ULONG handles_before;
	ULONG handles_after;
	ULONG handles_reclaimed;
	ULONG killed_leak_processes;
	ULONG killed_sandbox_processes;
	BOOLEAN is_dwm_reset;
	BOOLEAN is_shell_synced;
} LAG_CLEAN_RESULT, *PLAG_CLEAN_RESULT;

ULONG _lag_getsyshandlelimit_k ();

ULONG _lag_getprochandlelimit_k ();

ULONG _lag_getmask ();

VOID _lag_getstatus (
	_Out_ PLAG_STATUS_INFO out_status
);

BOOLEAN _lag_should_autoclean (
	_In_ PLAG_STATUS_INFO status_info
);

BOOLEAN _lag_clean (
	_In_opt_ HWND hwnd,
	_In_ CLEANUP_SOURCE_ENUM src,
	_In_opt_ ULONG lag_mask,
	_In_ BOOLEAN is_standalone_notify,
	_Out_opt_ PLAG_CLEAN_RESULT out_result
);

VOID _lag_sync_shell_and_frames (
	_In_opt_ HWND hwnd,
	_In_ BOOLEAN is_notify
);
