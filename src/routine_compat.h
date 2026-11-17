// Mem Reduct
// Compatibility layer for public routine SDK (v2.7.11)

#pragma once

#if !defined(MOUNTMGRCONTROLTYPE)
#define MOUNTMGRCONTROLTYPE 0x0000006D // 'm'
#define MOUNTMGR_DEVICE_NAME L"\\Device\\MountPointManager"
#define IOCTL_MOUNTMGR_QUERY_POINTS CTL_CODE(MOUNTMGRCONTROLTYPE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _MOUNTMGR_MOUNT_POINT
{
	ULONG SymbolicLinkNameOffset;
	USHORT SymbolicLinkNameLength;
	USHORT Reserved1;
	ULONG UniqueIdOffset;
	USHORT UniqueIdLength;
	USHORT Reserved2;
	ULONG DeviceNameOffset;
	USHORT DeviceNameLength;
	USHORT Reserved3;
} MOUNTMGR_MOUNT_POINT, *PMOUNTMGR_MOUNT_POINT;

typedef struct _MOUNTMGR_MOUNT_POINTS
{
	ULONG Size;
	ULONG NumberOfMountPoints;
	MOUNTMGR_MOUNT_POINT MountPoints[1];
} MOUNTMGR_MOUNT_POINTS, *PMOUNTMGR_MOUNT_POINTS;

#define MOUNTMGR_IS_VOLUME_NAME(s) ( \
	((s)->Length == 96 || ((s)->Length == 98 && (s)->Buffer[48] == L'\\')) && \
	(s)->Buffer[0] == L'\\' && \
	((s)->Buffer[1] == L'?' || (s)->Buffer[1] == L'\\') && \
	(s)->Buffer[2] == L'?' && \
	(s)->Buffer[3] == L'\\' && \
	(s)->Buffer[4] == L'V' && \
	(s)->Buffer[5] == L'o' && \
	(s)->Buffer[6] == L'l' && \
	(s)->Buffer[7] == L'u' && \
	(s)->Buffer[8] == L'm' && \
	(s)->Buffer[9] == L'e' && \
	(s)->Buffer[10] == L'{' && \
	(s)->Buffer[19] == L'-' && \
	(s)->Buffer[24] == L'-' && \
	(s)->Buffer[29] == L'-' && \
	(s)->Buffer[34] == L'-' && \
	(s)->Buffer[47] == L'}' \
)
#endif // !MOUNTMGRCONTROLTYPE

#if !defined(_r_button_ischecked)
#define _r_button_ischecked(hwnd, ctrl_id) _r_ctrl_isbuttonchecked ((hwnd), (ctrl_id))
#endif

#if !defined(_r_button_setcheck)
#define _r_button_setcheck(hwnd, ctrl_id, is_check) _r_ctrl_checkbutton ((hwnd), (ctrl_id), (is_check))
#endif

#if !defined(_r_button_setmargins)
#define _r_button_setmargins(hwnd, ctrl_id, dpi_value) _r_ctrl_setbuttonmargins ((hwnd), (ctrl_id), (dpi_value))
#endif

#if !defined(_r_button_setshield)
#define _r_button_setshield(hwnd, ctrl_id, is_enable) _r_ctrl_setbuttonshield ((hwnd), (ctrl_id), (is_enable))
#endif

#if !defined(_r_wnd_sendcommand)
#define _r_wnd_sendcommand(hwnd, cmd_id, lparam) _r_ctrl_sendcommand ((hwnd), (ULONG)(cmd_id), (LPARAM)(lparam))
#endif

#if !defined(_r_sys_settimer)
#define _r_sys_settimer(hwnd, id_event, elapse, timer_func) SetTimer ((hwnd), (id_event), (elapse), (timer_func))
#endif

FORCEINLINE BOOLEAN _r_config_invertboolean_compat (
	_In_ LPCWSTR key_name,
	_In_ BOOLEAN def_value,
	_In_opt_ LPCWSTR section_name
)
{
	BOOLEAN new_val;

	new_val = !_r_config_getboolean_ex (key_name, def_value, section_name);

	_r_config_setboolean_ex (key_name, new_val, section_name);

	return new_val;
}

#if !defined(_r_config_invertboolean)
#define _r_config_invertboolean(key_name, def_value, section_name) _r_config_invertboolean_compat ((key_name), (def_value), (section_name))
#endif

FORCEINLINE NTSTATUS _r_fs_getvolumemountpoints (
	_Out_ PMOUNTMGR_MOUNT_POINTS *out_buffer,
	_In_ HANDLE hdevice
)
{
	MOUNTMGR_MOUNT_POINT mount_point = {0};
	PMOUNTMGR_MOUNT_POINTS mount_points;
	ULONG length = 0x1000;
	NTSTATUS status;

	*out_buffer = NULL;

	mount_points = _r_mem_allocate (length);

	while (TRUE)
	{
		status = _r_fs_deviceiocontrol (
			hdevice,
			IOCTL_MOUNTMGR_QUERY_POINTS,
			&mount_point,
			sizeof (MOUNTMGR_MOUNT_POINT),
			mount_points,
			length,
			NULL
		);

		if (status == STATUS_BUFFER_OVERFLOW)
		{
			length = mount_points->Size;
			mount_points = _r_mem_reallocate (mount_points, length);
		}
		else
		{
			break;
		}
	}

	if (NT_SUCCESS (status))
	{
		*out_buffer = mount_points;
	}
	else
	{
		_r_mem_free (mount_points);
	}

	return status;
}

// Map 3-arg / 4-arg routine v3.0 config calls to routine v2.7 _ex APIs
#define _r_config_getboolean(key_name, def_value, section_name) _r_config_getboolean_ex ((key_name), (def_value), (section_name))
#define _r_config_getlong(key_name, def_value, section_name) _r_config_getlong_ex ((key_name), (def_value), (section_name))
#define _r_config_getlong64(key_name, def_value, section_name) _r_config_getlong64_ex ((key_name), (def_value), (section_name))
#define _r_config_getulong(key_name, def_value, section_name) _r_config_getulong_ex ((key_name), (def_value), (section_name))
#define _r_config_getulong64(key_name, def_value, section_name) _r_config_getulong64_ex ((key_name), (def_value), (section_name))
#define _r_config_getfont(key_name, logfont, dpi_value, section_name) _r_config_getfont_ex ((key_name), (logfont), (dpi_value), (section_name))

#define _r_config_setboolean(key_name, value, section_name) _r_config_setboolean_ex ((key_name), (value), (section_name))
#define _r_config_setlong(key_name, value, section_name) _r_config_setlong_ex ((key_name), (value), (section_name))
#define _r_config_setlong64(key_name, value, section_name) _r_config_setlong64_ex ((key_name), (value), (section_name))
#define _r_config_setulong(key_name, value, section_name) _r_config_setulong_ex ((key_name), (value), (section_name))
#define _r_config_setulong64(key_name, value, section_name) _r_config_setulong64_ex ((key_name), (value), (section_name))
#define _r_config_setfont(key_name, logfont, dpi_value, section_name) _r_config_setfont_ex ((key_name), (logfont), (dpi_value), (section_name))
