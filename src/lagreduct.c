// Mem Reduct - System Lag Reduction Submodule (Lag Reduct)
// Pure C + Windows Native API (ntdll / dwmapi) implementation

#include "routine.h"
#include "routine_compat.h"

#include "main.h"
#include "lagreduct.h"
#include "resource.h"

#define LAG_MAX_WINDOW_SNAPSHOTS 256
#define LAG_CPU_TRACK_SLOTS 256

typedef struct _LAG_CPU_SLOT
{
	ULONG pid;
	LONGLONG create_time;
	LONGLONG total_ticks;
	LONGLONG kernel_ticks;
	ULONG cpu_permille;
	ULONG kernel_permille;
	ULONG high_cpu_streak;
	ULONG generation;
} LAG_CPU_SLOT, *PLAG_CPU_SLOT;

typedef struct _LAG_WINDOW_RECORD
{
	HWND hwnd;
	ULONG pid;
	INT backdrop_type;
	BOOL dark_mode;
	INT corner_pref;
	BOOLEAN has_backdrop;
	BOOLEAN has_dark_mode;
	BOOLEAN has_corner;
} LAG_WINDOW_RECORD, *PLAG_WINDOW_RECORD;

typedef struct _LAG_WINDOW_SNAPSHOT
{
	ULONG count;
	LAG_WINDOW_RECORD items[LAG_MAX_WINDOW_SNAPSHOTS];
} LAG_WINDOW_SNAPSHOT, *PLAG_WINDOW_SNAPSHOT;

typedef struct _LAG_RECOVERY_CONTEXT
{
	ULONG old_dwm_pid;
	LONGLONG trigger_qpc;
	BOOLEAN is_sync_only;
	LAG_WINDOW_SNAPSHOT snapshot;
} LAG_RECOVERY_CONTEXT, *PLAG_RECOVERY_CONTEXT;

static LAG_CPU_SLOT g_cpu_slots[LAG_CPU_TRACK_SLOTS] = {0};
static LONGLONG g_prev_sample_time_100ns = 0;
static ULONG g_sample_generation = 0;
static volatile LONG g_recovery_running = 0;

// Critical system & interactive user applications that must never be terminated by background reapers
static LPCWSTR g_protected_processes[] = {
	L"System",
	L"Secure System",
	L"Registry",
	L"Memory Compression",
	L"MemCompression",
	L"smss.exe",
	L"csrss.exe",
	L"wininit.exe",
	L"services.exe",
	L"lsass.exe",
	L"lsm.exe",
	L"winlogon.exe",
	L"svchost.exe",
	L"fontdrvhost.exe",
	L"dwm.exe",
	L"sihost.exe",
	L"explorer.exe",
	L"taskhostw.exe",
	L"ctfmon.exe",
	L"WUDFHost.exe",
	L"audiodg.exe",
	L"spoolsv.exe",
	L"SearchIndexer.exe",
	L"RuntimeBroker.exe",
	L"ApplicationFrameHost.exe",
	L"SystemSettings.exe",
	L"LockApp.exe",
	L"SearchHost.exe",
	L"StartMenuExperienceHost.exe",
	L"ShellExperienceHost.exe",
	L"TextInputHost.exe",
	L"dllhost.exe",
	L"conhost.exe",
	L"OpenConsole.exe",
	L"WindowsTerminal.exe",
	L"MsMpEng.exe",
	L"NisSrv.exe",
	L"SecurityHealthService.exe",
	L"SecurityHealthSystray.exe",
	L"HipsDaemon.exe",
	L"HipsTray.exe",
	L"wsctrlsvc.exe",
	L"NVDisplay.Container.exe",
	L"amdrsserv.exe",
	L"igfxEM.exe",
	L"Weixin.exe",
	L"WeChat.exe",
	L"chrome.exe",
	L"msedge.exe",
	L"firefox.exe",
	L"brave.exe",
	L"Code.exe",
	L"devenv.exe",
	L"Antigravity.exe",
	L"Notion.exe",
	L"ChatGPT.exe",
	L"claude.exe",
	L"WorkBuddy.exe",
	L"mailmaster.exe",
	L"python.exe",
	L"pythonw.exe",
	L"node.exe",
	L"git.exe",
	L"powershell.exe",
	L"pwsh.exe",
	L"cmd.exe",
	L"vmmem",
	L"vmmemWSL",
	L"wslservice.exe",
};

// Windows 11 DirectComposition Shell XAML hosts that detach when dwm.exe restarts
static LPCWSTR g_shell_xaml_hosts[] = {
	L"SearchHost.exe",
	L"StartMenuExperienceHost.exe",
	L"ShellExperienceHost.exe",
	L"TextInputHost.exe",
};

// Known leaky store/updater background helpers
static LPCWSTR g_known_leaky_helpers[] = {
	L"HrAppStoreAppOM.exe",
};

// Known headless background sandbox/helper candidates
static LPCWSTR g_sandbox_helpers[] = {
	L"sandbox-center.exe",
};

ULONG _lag_getsyshandlelimit_k ()
{
	return (ULONG)_r_calc_clamp ((LONG)_r_config_getulong (L"LagSysHandleLimitK", DEFAULT_LAG_SYS_HANDLES_K, NULL), 100, 2000);
}

ULONG _lag_getprochandlelimit_k ()
{
	return (ULONG)_r_calc_clamp ((LONG)_r_config_getulong (L"LagProcHandleLimitK", DEFAULT_LAG_PROC_HANDLES_K, NULL), 10, 500);
}

ULONG _lag_getmask ()
{
	return _r_config_getulong (L"LagReductMask", LAG_REDUCT_MASK_DEFAULT, NULL);
}

static BOOLEAN _lag_match_namelist (
	_In_ PR_STRINGREF image_name,
	_In_reads_ (count) LPCWSTR *list,
	_In_ ULONG_PTR count
)
{
	if (!image_name->buffer || !image_name->length)
		return FALSE;

	for (ULONG_PTR i = 0; i < count; i++)
	{
		if (_r_str_isequal2 (image_name, (LPWSTR)list[i], TRUE))
			return TRUE;
	}

	return FALSE;
}

static BOOLEAN _lag_is_protected_process (
	_In_ PR_STRINGREF image_name
)
{
	if (!image_name->buffer || !image_name->length)
		return TRUE;

	return _lag_match_namelist (image_name, g_protected_processes, RTL_NUMBER_OF (g_protected_processes));
}

// Pure Native API process termination with optional kernel handle rundown wait
static NTSTATUS _lag_terminate_pid_native (
	_In_ ULONG pid,
	_In_ ULONG wait_ms
)
{
	OBJECT_ATTRIBUTES oa;
	CLIENT_ID cid;
	LARGE_INTEGER timeout;
	HANDLE hprocess = NULL;
	NTSTATUS status;

	if (pid <= 4 || pid == HandleToUlong (NtCurrentProcessId ()))
		return STATUS_ACCESS_DENIED;

	InitializeObjectAttributes (&oa, NULL, 0, NULL, NULL);

	cid.UniqueProcess = UlongToHandle (pid);
	cid.UniqueThread = NULL;

	status = NtOpenProcess (&hprocess, PROCESS_TERMINATE | SYNCHRONIZE, &oa, &cid);

	if (!NT_SUCCESS (status))
	{
		status = NtOpenProcess (&hprocess, PROCESS_TERMINATE, &oa, &cid);

		if (!NT_SUCCESS (status))
			return status;

		wait_ms = 0;
	}

	status = NtTerminateProcess (hprocess, STATUS_SUCCESS);

	if (NT_SUCCESS (status) && wait_ms > 0)
	{
		timeout.QuadPart = -((LONGLONG)wait_ms * 10000LL);

		NtWaitForSingleObject (hprocess, FALSE, &timeout);
	}

	NtClose (hprocess);

	return status;
}

static VOID _lag_sleep_native (
	_In_ ULONG milliseconds
)
{
	LARGE_INTEGER interval;

	interval.QuadPart = -((LONGLONG)milliseconds * 10000LL);

	NtDelayExecution (FALSE, &interval);
}

static BOOL CALLBACK _lag_enum_windows_proc (
	_In_ HWND hwnd,
	_In_ LPARAM lparam
)
{
	PLAG_WINDOW_SNAPSHOT snapshot;
	PLAG_WINDOW_RECORD rec;
	RECT rc = {0};
	DWORD pid = 0;
	INT backdrop = 0;
	BOOL dark_mode = FALSE;
	INT corner = 0;

	snapshot = (PLAG_WINDOW_SNAPSHOT)lparam;

	if (!IsWindowVisible (hwnd))
		return TRUE;

	if (GetWindowRect (hwnd, &rc))
	{
		if ((rc.right - rc.left) <= 1 && (rc.bottom - rc.top) <= 1)
			return TRUE;
	}

	GetWindowThreadProcessId (hwnd, &pid);

	if (!pid)
		return TRUE;

	if (snapshot->count < LAG_MAX_WINDOW_SNAPSHOTS)
	{
		rec = &snapshot->items[snapshot->count];
		RtlZeroMemory (rec, sizeof (LAG_WINDOW_RECORD));

		rec->hwnd = hwnd;
		rec->pid = (ULONG)pid;

		if (SUCCEEDED (DwmGetWindowAttribute (hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof (backdrop))))
		{
			rec->backdrop_type = backdrop;
			rec->has_backdrop = TRUE;
		}

		if (SUCCEEDED (DwmGetWindowAttribute (hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_mode, sizeof (dark_mode))))
		{
			rec->dark_mode = dark_mode;
			rec->has_dark_mode = TRUE;
		}

		if (SUCCEEDED (DwmGetWindowAttribute (hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof (corner))))
		{
			rec->corner_pref = corner;
			rec->has_corner = TRUE;
		}

		snapshot->count += 1;
	}

	return TRUE;
}

static VOID _lag_capture_window_snapshot (
	_Out_ PLAG_WINDOW_SNAPSHOT snapshot
)
{
	RtlZeroMemory (snapshot, sizeof (LAG_WINDOW_SNAPSHOT));

	EnumWindows (&_lag_enum_windows_proc, (LPARAM)snapshot);
}

static BOOLEAN _lag_pid_owns_visible_window (
	_In_ PLAG_WINDOW_SNAPSHOT snapshot,
	_In_ ULONG pid
)
{
	for (ULONG i = 0; i < snapshot->count; i++)
	{
		if (snapshot->items[i].pid == pid)
			return TRUE;
	}

	return FALSE;
}

static BOOLEAN _lag_pid_exists_in_snapshot (
	_In_ PSYSTEM_PROCESS_INFORMATION spi_root,
	_In_ ULONG target_pid
)
{
	PSYSTEM_PROCESS_INFORMATION spi;

	if (!target_pid)
		return FALSE;

	spi = spi_root;

	while (TRUE)
	{
		if (HandleToUlong (spi->UniqueProcessId) == target_pid)
			return TRUE;

		if (!spi->NextEntryOffset)
			break;

		spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
	}

	return FALSE;
}

static PLAG_CPU_SLOT _lag_find_or_alloc_cpu_slot (
	_In_ ULONG pid,
	_In_ LONGLONG create_time
)
{
	ULONG start_idx;
	ULONG idx;
	ULONG oldest_idx = 0;
	ULONG oldest_gen = MAXULONG;

	start_idx = (pid * 2654435761UL) % LAG_CPU_TRACK_SLOTS;

	for (ULONG probe = 0; probe < 8; probe++)
	{
		idx = (start_idx + probe) % LAG_CPU_TRACK_SLOTS;

		if (g_cpu_slots[idx].pid == pid && g_cpu_slots[idx].create_time == create_time)
			return &g_cpu_slots[idx];

		if (g_cpu_slots[idx].pid == 0)
		{
			g_cpu_slots[idx].pid = pid;
			g_cpu_slots[idx].create_time = create_time;
			return &g_cpu_slots[idx];
		}

		if (g_cpu_slots[idx].generation < oldest_gen)
		{
			oldest_gen = g_cpu_slots[idx].generation;
			oldest_idx = idx;
		}
	}

	RtlZeroMemory (&g_cpu_slots[oldest_idx], sizeof (LAG_CPU_SLOT));
	g_cpu_slots[oldest_idx].pid = pid;
	g_cpu_slots[oldest_idx].create_time = create_time;

	return &g_cpu_slots[oldest_idx];
}

static PLAG_CPU_SLOT _lag_lookup_cpu_slot (
	_In_ ULONG pid,
	_In_ LONGLONG create_time
)
{
	ULONG start_idx;
	ULONG idx;

	start_idx = (pid * 2654435761UL) % LAG_CPU_TRACK_SLOTS;

	for (ULONG probe = 0; probe < 8; probe++)
	{
		idx = (start_idx + probe) % LAG_CPU_TRACK_SLOTS;

		if (g_cpu_slots[idx].pid == pid && g_cpu_slots[idx].create_time == create_time)
			return &g_cpu_slots[idx];
	}

	return NULL;
}

VOID _lag_getstatus (
	_Out_ PLAG_STATUS_INFO out_status
)
{
	SYSTEM_PERFORMANCE_INFORMATION perf_info = {0};
	PSYSTEM_PROCESS_INFORMATION spi_buffer = NULL;
	PSYSTEM_PROCESS_INFORMATION spi;
	LARGE_INTEGER sys_time = {0};
	R_STRINGREF sr_name;
	PLAG_CPU_SLOT slot;
	LONGLONG elapsed_100ns = 0;
	LONGLONG total_capacity_100ns = 0;
	ULONG cpu_count;
	ULONG sys_limit;
	ULONG pid;
	NTSTATUS status;

	RtlZeroMemory (out_status, sizeof (LAG_STATUS_INFO));

	NtQuerySystemTime (&sys_time);

	if (g_prev_sample_time_100ns > 0 && sys_time.QuadPart > g_prev_sample_time_100ns)
		elapsed_100ns = sys_time.QuadPart - g_prev_sample_time_100ns;

	g_prev_sample_time_100ns = sys_time.QuadPart;
	g_sample_generation += 1;

	cpu_count = _r_sys_getprocessorscount ();

	if (!cpu_count)
		cpu_count = 1;

	if (elapsed_100ns >= 2000000LL) // >= 200 ms
		total_capacity_100ns = elapsed_100ns * (LONGLONG)cpu_count;

	status = _r_sys_enumprocesses (&spi_buffer);

	if (NT_SUCCESS (status) && spi_buffer)
	{
		spi = spi_buffer;

		while (TRUE)
		{
			pid = HandleToUlong (spi->UniqueProcessId);

			out_status->total_processes += 1;
			out_status->total_handles += spi->HandleCount;

			_r_obj_initializestringref3 (&sr_name, &spi->ImageName);

			if (_r_str_isequal2 (&sr_name, L"dwm.exe", TRUE))
			{
				if (spi->HandleCount >= out_status->dwm_handles)
				{
					out_status->dwm_handles = spi->HandleCount;
					out_status->dwm_pid = pid;
				}
			}
			else if (pid > 4 && spi->SessionId > 0 && !_lag_is_protected_process (&sr_name))
			{
				if (spi->HandleCount > out_status->max_leaked_handles)
				{
					ULONG_PTR copy_chars;

					out_status->max_leaked_handles = spi->HandleCount;
					out_status->max_leaked_pid = pid;

					copy_chars = sr_name.length / sizeof (WCHAR);

					if (copy_chars >= RTL_NUMBER_OF (out_status->max_leaked_name))
						copy_chars = RTL_NUMBER_OF (out_status->max_leaked_name) - 1;

					RtlCopyMemory (out_status->max_leaked_name, sr_name.buffer, copy_chars * sizeof (WCHAR));
					out_status->max_leaked_name[copy_chars] = UNICODE_NULL;
				}
			}

			// Track CPU & Kernel-CPU delta for background processes
			if (pid > 4 && spi->SessionId > 0 && (spi->HandleCount >= 5000 || _lag_match_namelist (&sr_name, g_known_leaky_helpers, RTL_NUMBER_OF (g_known_leaky_helpers)) || _lag_match_namelist (&sr_name, g_sandbox_helpers, RTL_NUMBER_OF (g_sandbox_helpers)) || _r_str_isequal2 (&sr_name, L"msedgewebview2.exe", TRUE)))
			{
				LONGLONG cur_total = spi->KernelTime.QuadPart + spi->UserTime.QuadPart;
				LONGLONG cur_kernel = spi->KernelTime.QuadPart;

				slot = _lag_find_or_alloc_cpu_slot (pid, spi->CreateTime.QuadPart);

				if (slot->generation > 0 && total_capacity_100ns > 0 && cur_total >= slot->total_ticks && cur_kernel >= slot->kernel_ticks)
				{
					LONGLONG d_total = cur_total - slot->total_ticks;
					LONGLONG d_kernel = cur_kernel - slot->kernel_ticks;

					slot->cpu_permille = (ULONG)((d_total * 1000LL) / total_capacity_100ns);
					slot->kernel_permille = (ULONG)((d_kernel * 1000LL) / total_capacity_100ns);

					if (slot->cpu_permille >= 50) // >= 5% of all CPU cores sustained
					{
						slot->high_cpu_streak += 1;
					}
					else
					{
						slot->high_cpu_streak = 0;
					}
				}

				slot->total_ticks = cur_total;
				slot->kernel_ticks = cur_kernel;
				slot->generation = g_sample_generation;
			}

			if (!spi->NextEntryOffset)
				break;

			spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
		}

		_r_mem_free (spi_buffer);
	}

	status = NtQuerySystemInformation (
		SystemPerformanceInformation,
		&perf_info,
		sizeof (SYSTEM_PERFORMANCE_INFORMATION),
		NULL
	);

	if (NT_SUCCESS (status))
	{
		out_status->nonpaged_pool_bytes = (ULONG64)perf_info.NonPagedPoolPages * PAGE_SIZE;
		out_status->paged_pool_bytes = (ULONG64)perf_info.PagedPoolPages * PAGE_SIZE;
	}

	sys_limit = _lag_getsyshandlelimit_k () * 1000UL;

	if (sys_limit > 0)
	{
		ULONG pct = (ULONG)(((ULONG64)out_status->total_handles * 100ULL) / (ULONG64)sys_limit);

		out_status->handle_pressure_percent = (ULONG)_r_calc_clamp ((LONG)pct, 0, 100);
	}
}

BOOLEAN _lag_should_autoclean (
	_In_ PLAG_STATUS_INFO status_info
)
{
	ULONG lag_mask;
	ULONG sys_limit;
	ULONG proc_limit;
	LONG64 elapsed;

	if (!_r_config_getboolean (L"LagAutoEnable", FALSE, NULL))
		return FALSE;

	lag_mask = _lag_getmask ();

	if (!lag_mask)
		return FALSE;

	elapsed = _r_unixtime_now () - _r_config_getlong64 (L"StatisticLastLagReduct", 0, NULL);

	if (elapsed < LAG_AUTOREDUCT_COOLDOWN)
		return FALSE;

	sys_limit = _lag_getsyshandlelimit_k () * 1000UL;
	proc_limit = _lag_getprochandlelimit_k () * 1000UL;

	if (status_info->total_handles >= sys_limit)
		return TRUE;

	if ((lag_mask & LAG_REDUCT_HANDLE_LEAKS) != 0 && status_info->max_leaked_handles >= proc_limit)
		return TRUE;

	if ((lag_mask & LAG_REDUCT_DWM_BLOAT) != 0 && status_info->dwm_handles >= proc_limit)
	{
		LONG64 dwm_elapsed = _r_unixtime_now () - _r_config_getlong64 (L"StatisticLastDwmReset", 0, NULL);

		if (dwm_elapsed >= LAG_DWM_RESET_COOLDOWN)
			return TRUE;
	}

	return FALSE;
}

static VOID _lag_restore_window_frames (
	_In_ PLAG_WINDOW_SNAPSHOT snapshot
)
{
	DWORD_PTR msg_result = 0;

	for (ULONG i = 0; i < snapshot->count; i++)
	{
		PLAG_WINDOW_RECORD rec = &snapshot->items[i];

		if (!IsWindow (rec->hwnd) || !IsWindowVisible (rec->hwnd))
			continue;

		if (rec->has_dark_mode)
			DwmSetWindowAttribute (rec->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &rec->dark_mode, sizeof (rec->dark_mode));

		if (rec->has_corner && rec->corner_pref != 0)
			DwmSetWindowAttribute (rec->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &rec->corner_pref, sizeof (rec->corner_pref));

		if (rec->has_backdrop && rec->backdrop_type > 1)
			DwmSetWindowAttribute (rec->hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &rec->backdrop_type, sizeof (rec->backdrop_type));

		SendMessageTimeoutW (rec->hwnd, WM_DWMCOMPOSITIONCHANGED, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 60, &msg_result);
		SendMessageTimeoutW (rec->hwnd, WM_THEMECHANGED, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 60, &msg_result);
		SendMessageTimeoutW (rec->hwnd, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG | SMTO_NORMAL, 60, &msg_result);

		SetWindowPos (
			rec->hwnd,
			NULL,
			0,
			0,
			0,
			0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
		);

		RedrawWindow (
			rec->hwnd,
			NULL,
			NULL,
			RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN
		);
	}
}

static VOID _lag_refresh_shell_xaml_hosts (
	_In_ LONGLONG cutoff_create_time
)
{
	PSYSTEM_PROCESS_INFORMATION spi_buffer = NULL;
	PSYSTEM_PROCESS_INFORMATION spi;
	ULONG search_pids[8] = {0};
	ULONG search_pid_count = 0;
	R_STRINGREF sr_name;
	ULONG pid;

	if (!NT_SUCCESS (_r_sys_enumprocesses (&spi_buffer)) || !spi_buffer)
		return;

	// First pass: terminate stale Shell XAML hosts created before cutoff_create_time
	spi = spi_buffer;

	while (TRUE)
	{
		pid = HandleToUlong (spi->UniqueProcessId);

		if (pid > 4 && spi->SessionId > 0)
		{
			if (cutoff_create_time == 0 || spi->CreateTime.QuadPart < cutoff_create_time)
			{
				_r_obj_initializestringref3 (&sr_name, &spi->ImageName);

				if (_lag_match_namelist (&sr_name, g_shell_xaml_hosts, RTL_NUMBER_OF (g_shell_xaml_hosts)))
				{
					if (_r_str_isequal2 (&sr_name, L"SearchHost.exe", TRUE) && search_pid_count < RTL_NUMBER_OF (search_pids))
						search_pids[search_pid_count++] = pid;

					_lag_terminate_pid_native (pid, 150);
				}
			}
		}

		if (!spi->NextEntryOffset)
			break;

		spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
	}

	// Second pass: ensure orphaned WebView2 children of old SearchHost are also cleaned
	if (search_pid_count > 0)
	{
		spi = spi_buffer;

		while (TRUE)
		{
			pid = HandleToUlong (spi->UniqueProcessId);

			if (pid > 4 && spi->SessionId > 0)
			{
				ULONG parent_pid = HandleToUlong (spi->InheritedFromUniqueProcessId);

				for (ULONG i = 0; i < search_pid_count; i++)
				{
					if (parent_pid == search_pids[i])
					{
						_r_obj_initializestringref3 (&sr_name, &spi->ImageName);

						if (_r_str_isequal2 (&sr_name, L"msedgewebview2.exe", TRUE))
							_lag_terminate_pid_native (pid, 50);

						break;
					}
				}
			}

			if (!spi->NextEntryOffset)
				break;

			spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
		}
	}

	_r_mem_free (spi_buffer);
}

static NTSTATUS NTAPI _lag_dwm_recovery_thread (
	_In_ PVOID arglist
)
{
	PLAG_RECOVERY_CONTEXT ctx;
	PSYSTEM_PROCESS_INFORMATION spi_buffer;
	PSYSTEM_PROCESS_INFORMATION spi;
	R_STRINGREF sr_name;
	LONGLONG new_dwm_create_time = 0;
	ULONG new_dwm_pid = 0;

	ctx = (PLAG_RECOVERY_CONTEXT)arglist;

	if (!ctx)
	{
		_InterlockedExchange (&g_recovery_running, 0);
		return STATUS_INVALID_PARAMETER;
	}

	if (!ctx->is_sync_only)
	{
		// Poll via Native API until winlogon spawns the replacement dwm.exe
		for (ULONG attempt = 0; attempt < 25; attempt++)
		{
			_lag_sleep_native (200);

			spi_buffer = NULL;

			if (NT_SUCCESS (_r_sys_enumprocesses (&spi_buffer)) && spi_buffer)
			{
				spi = spi_buffer;

				while (TRUE)
				{
					ULONG pid = HandleToUlong (spi->UniqueProcessId);

					_r_obj_initializestringref3 (&sr_name, &spi->ImageName);

					if (_r_str_isequal2 (&sr_name, L"dwm.exe", TRUE) && pid != ctx->old_dwm_pid)
					{
						new_dwm_pid = pid;
						new_dwm_create_time = spi->CreateTime.QuadPart;
						break;
					}

					if (!spi->NextEntryOffset)
						break;

					spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
				}

				_r_mem_free (spi_buffer);
			}

			if (new_dwm_pid != 0)
				break;
		}

		// Allow the new DWM composition engine to finish surface initialization
		_lag_sleep_native (1100);
	}

	// Anti-Bug Step 1: Refresh detached Win11 DirectComposition Shell XAML hosts (prevents blank Search / Start Menu)
	_lag_refresh_shell_xaml_hosts (ctx->is_sync_only ? 0 : new_dwm_create_time);

	_lag_sleep_native (250);

	// Anti-Bug Step 2: Restore DWM Mica/Dark-Mode/Corner attributes & broadcast frame redraw (prevents gray borders)
	_lag_restore_window_frames (&ctx->snapshot);

	_r_mem_free (ctx);

	_InterlockedExchange (&g_recovery_running, 0);

	return STATUS_SUCCESS;
}

static BOOLEAN _lag_start_recovery_worker (
	_In_ ULONG old_dwm_pid,
	_In_ BOOLEAN is_sync_only,
	_In_ PLAG_WINDOW_SNAPSHOT snapshot
)
{
	PLAG_RECOVERY_CONTEXT ctx;
	NTSTATUS status;

	if (_InterlockedCompareExchange (&g_recovery_running, 1, 0) != 0)
		return FALSE;

	ctx = _r_mem_allocate (sizeof (LAG_RECOVERY_CONTEXT));

	ctx->old_dwm_pid = old_dwm_pid;
	ctx->is_sync_only = is_sync_only;
	RtlCopyMemory (&ctx->snapshot, snapshot, sizeof (LAG_WINDOW_SNAPSHOT));

	status = _r_sys_createthread (
		NULL,
		NtCurrentProcess (),
		&_lag_dwm_recovery_thread,
		ctx,
		NULL,
		L"LagDwmRecovery"
	);

	if (!NT_SUCCESS (status))
	{
		_r_mem_free (ctx);
		_InterlockedExchange (&g_recovery_running, 0);
		return FALSE;
	}

	return TRUE;
}

static BOOLEAN _lag_is_fullscreen_busy ()
{
	QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;

	if (SUCCEEDED (SHQueryUserNotificationState (&quns)))
	{
		if (quns == QUNS_BUSY || quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE)
			return TRUE;
	}

	return FALSE;
}

VOID _lag_sync_shell_and_frames (
	_In_opt_ HWND hwnd,
	_In_ BOOLEAN is_notify
)
{
	LAG_WINDOW_SNAPSHOT snapshot;
	ULONG flags = NIIF_INFO;

	if (!_r_config_getboolean (L"IsNotificationsSound", TRUE, NULL))
		flags |= NIIF_NOSOUND;

	_lag_capture_window_snapshot (&snapshot);

	if (_lag_start_recovery_worker (0, TRUE, &snapshot))
	{
		if (hwnd && is_notify && _r_config_getboolean (L"BalloonCleanResults", TRUE, NULL))
			_r_tray_popup (hwnd, &GUID_TrayIcon, flags, _r_app_getname (), _r_locale_getstring (IDS_LAG_SHELL_SYNCED));
	}
}

BOOLEAN _lag_clean (
	_In_opt_ HWND hwnd,
	_In_ CLEANUP_SOURCE_ENUM src,
	_In_opt_ ULONG lag_mask,
	_In_ BOOLEAN is_standalone_notify,
	_Out_opt_ PLAG_CLEAN_RESULT out_result
)
{
	LAG_CLEAN_RESULT local_result = {0};
	LAG_STATUS_INFO before_status;
	LAG_STATUS_INFO after_status;
	LAG_WINDOW_SNAPSHOT win_snapshot;
	PSYSTEM_PROCESS_INFORMATION spi_buffer = NULL;
	PSYSTEM_PROCESS_INFORMATION spi;
	LARGE_INTEGER now_sys_time = {0};
	R_STRINGREF sr_name;
	ULONG proc_handle_limit;
	ULONG sys_handle_limit;
	BOOLEAN is_antibug;
	ULONG self_pid;

	if (out_result)
		RtlZeroMemory (out_result, sizeof (LAG_CLEAN_RESULT));

	if (!_r_sys_iselevated ())
	{
		if (is_standalone_notify)
		{
			if (_r_app_runasadmin ())
			{
				if (hwnd)
					DestroyWindow (hwnd);
			}
			else if (hwnd)
			{
				_r_show_message (hwnd, MB_OK | MB_ICONSTOP, NULL, _r_locale_getstring (IDS_STATUS_NOPRIVILEGES));
			}
		}

		return FALSE;
	}

	if (lag_mask == 0)
		lag_mask = _lag_getmask ();

	if (!lag_mask)
		return FALSE;

	proc_handle_limit = _lag_getprochandlelimit_k () * 1000UL;
	sys_handle_limit = _lag_getsyshandlelimit_k () * 1000UL;
	is_antibug = _r_config_getboolean (L"LagAntiBugEnable", TRUE, NULL);
	self_pid = HandleToUlong (NtCurrentProcessId ());

	_lag_getstatus (&before_status);
	local_result.handles_before = before_status.total_handles;

	// Capture visible user windows & DWM attributes before touching any process
	_lag_capture_window_snapshot (&win_snapshot);

	NtQuerySystemTime (&now_sys_time);

	if (NT_SUCCESS (_r_sys_enumprocesses (&spi_buffer)) && spi_buffer)
	{
		spi = spi_buffer;

		while (TRUE)
		{
			ULONG pid = HandleToUlong (spi->UniqueProcessId);

			if (pid > 4 && pid != self_pid && spi->SessionId > 0)
			{
				_r_obj_initializestringref3 (&sr_name, &spi->ImageName);

				if (!_lag_is_protected_process (&sr_name) && !_lag_pid_owns_visible_window (&win_snapshot, pid))
				{
					PLAG_CPU_SLOT cpu_slot = _lag_lookup_cpu_slot (pid, spi->CreateTime.QuadPart);
					BOOLEAN is_known_leaky = _lag_match_namelist (&sr_name, g_known_leaky_helpers, RTL_NUMBER_OF (g_known_leaky_helpers));
					BOOLEAN is_sandbox = _lag_match_namelist (&sr_name, g_sandbox_helpers, RTL_NUMBER_OF (g_sandbox_helpers));
					BOOLEAN should_kill_leak = FALSE;
					BOOLEAN should_kill_sandbox = FALSE;

					// Source 1: Severe Handle Leak / Runaway Kernel-CPU Background Daemon
					if ((lag_mask & LAG_REDUCT_HANDLE_LEAKS) != 0)
					{
						if (spi->HandleCount >= proc_handle_limit)
						{
							should_kill_leak = TRUE;
						}
						else if (is_known_leaky && (spi->HandleCount >= 10000 || (cpu_slot && cpu_slot->kernel_permille >= 50)))
						{
							should_kill_leak = TRUE;
						}
						else if (cpu_slot && cpu_slot->kernel_permille >= 100 && cpu_slot->high_cpu_streak >= 5 && spi->HandleCount >= (proc_handle_limit / 2))
						{
							should_kill_leak = TRUE;
						}
					}

					// Source 3: Runaway Headless Sandbox & Orphaned Helper Reaper
					if (!should_kill_leak && (lag_mask & LAG_REDUCT_ORPHAN_SANDBOX) != 0)
					{
						ULONG parent_pid = HandleToUlong (spi->InheritedFromUniqueProcessId);
						BOOLEAN parent_alive = _lag_pid_exists_in_snapshot (spi_buffer, parent_pid);
						LONGLONG age_sec = (now_sys_time.QuadPart > spi->CreateTime.QuadPart) ? ((now_sys_time.QuadPart - spi->CreateTime.QuadPart) / 10000000LL) : 0;

						if (is_sandbox)
						{
							if (!parent_alive && age_sec >= 30)
							{
								should_kill_sandbox = TRUE;
							}
							else if (cpu_slot && cpu_slot->high_cpu_streak >= 5 && age_sec >= 300)
							{
								should_kill_sandbox = TRUE;
							}
							else if (src == SOURCE_MANUAL && is_standalone_notify && cpu_slot && cpu_slot->cpu_permille >= 40 && age_sec >= 120)
							{
								should_kill_sandbox = TRUE;
							}
						}
						else if (!parent_alive && age_sec >= 60 && (_r_str_isequal2 (&sr_name, L"msedgewebview2.exe", TRUE) || _r_str_isequal2 (&sr_name, L"crashpad_handler.exe", TRUE)))
						{
							if (spi->HandleCount >= 10000 || (cpu_slot && cpu_slot->high_cpu_streak >= 4))
								should_kill_sandbox = TRUE;
						}
					}

					if (should_kill_leak)
					{
						if (NT_SUCCESS (_lag_terminate_pid_native (pid, 250)))
							local_result.killed_leak_processes += 1;
					}
					else if (should_kill_sandbox)
					{
						if (NT_SUCCESS (_lag_terminate_pid_native (pid, 200)))
							local_result.killed_sandbox_processes += 1;
					}
				}
			}

			if (!spi->NextEntryOffset)
				break;

			spi = (PSYSTEM_PROCESS_INFORMATION)PTR_ADD_OFFSET (spi, spi->NextEntryOffset);
		}

		_r_mem_free (spi_buffer);
	}

	// Relieve kernel pool & registry cache when system handle pressure is elevated
	if ((lag_mask & LAG_REDUCT_ORPHAN_SANDBOX) != 0 && before_status.total_handles >= sys_handle_limit)
	{
		SYSTEM_MEMORY_LIST_COMMAND command = MemoryPurgeLowPriorityStandbyList;

		NtSetSystemInformation (SystemMemoryListInformation, &command, sizeof (SYSTEM_MEMORY_LIST_COMMAND));

		if (_r_sys_isosversiongreaterorequal (WINDOWS_8_1))
			NtSetSystemInformation (SystemRegistryReconciliationInformation, NULL, 0);
	}

	// Source 2: Safe DWM Handle-Bloat Reset (strictly gated on handle bloat + cooldown + fullscreen check)
	if ((lag_mask & LAG_REDUCT_DWM_BLOAT) != 0 && before_status.dwm_pid != 0 && before_status.dwm_handles >= proc_handle_limit)
	{
		LONG64 now_ts = _r_unixtime_now ();
		LONG64 last_dwm_ts = _r_config_getlong64 (L"StatisticLastDwmReset", 0, NULL);

		if ((now_ts - last_dwm_ts) >= LAG_DWM_RESET_COOLDOWN)
		{
			if (src != SOURCE_AUTO || !_lag_is_fullscreen_busy ())
			{
				if (NT_SUCCESS (_lag_terminate_pid_native (before_status.dwm_pid, 400)))
				{
					local_result.is_dwm_reset = TRUE;
					_r_config_setlong64 (L"StatisticLastDwmReset", now_ts, NULL);

					if (is_antibug)
					{
						local_result.is_shell_synced = _lag_start_recovery_worker (before_status.dwm_pid, FALSE, &win_snapshot);
					}
				}
			}
		}
	}

	_r_config_setlong64 (L"StatisticLastLagReduct", _r_unixtime_now (), NULL);

	_lag_getstatus (&after_status);
	local_result.handles_after = after_status.total_handles;
	local_result.handles_reclaimed = (local_result.handles_before > local_result.handles_after) ? (local_result.handles_before - local_result.handles_after) : 0;

	if (out_result)
		RtlCopyMemory (out_result, &local_result, sizeof (LAG_CLEAN_RESULT));

	if (is_standalone_notify && hwnd && _r_config_getboolean (L"BalloonCleanResults", TRUE, NULL))
	{
		WCHAR msg_buf[256] = {0};
		ULONG flags = NIIF_INFO;

		if (!_r_config_getboolean (L"IsNotificationsSound", TRUE, NULL))
			flags |= NIIF_NOSOUND;

		_r_str_printf (
			msg_buf,
			RTL_NUMBER_OF (msg_buf),
			_r_locale_getstring (IDS_LAG_STATUS_CLEANED),
			local_result.handles_reclaimed,
			local_result.killed_leak_processes + local_result.killed_sandbox_processes,
			local_result.is_dwm_reset ? L"Yes" : L"Skipped (Healthy)"
		);

		_r_tray_popup (hwnd, &GUID_TrayIcon, flags, _r_app_getname (), msg_buf);
	}

	return (local_result.handles_reclaimed > 0 || local_result.killed_leak_processes > 0 || local_result.killed_sandbox_processes > 0 || local_result.is_dwm_reset);
}
