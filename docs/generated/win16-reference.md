<!-- GENERATED FILE. Do not hand-edit; regenerate with -->
<!-- tools/docgen/gen_win16_reference.py kernel/exec/win16api.c -->

Generated: 2026-08-06 14:45 UTC
Source commit: `fe20ed6a20d4168400b7e74aded6ec800fd51590`
Source file: `kernel/exec/win16api.c` (1447 table entries parsed)

## Coverage by module

| Module | Implemented | ordinal+name | ordinal-only | name-only | Stubbed | Total table entries |
|---|---:|---:|---:|---:|---:|---:|
| GDI | 115 | 115 | 0 | 0 | 295 | 410 |
| KERNEL | 85 | 84 | 0 | 1 | 364 | 449 |
| KEYBOARD | 5 | 5 | 0 | 0 | 0 | 5 |
| LZEXPAND | 3 | 3 | 0 | 0 | 0 | 3 |
| MMSYSTEM | 3 | 3 | 0 | 0 | 0 | 3 |
| OLESVR | 9 | 9 | 0 | 0 | 0 | 9 |
| PRNDRV | 4 | 0 | 0 | 4 | 0 | 4 |
| SHELL | 19 | 11 | 0 | 8 | 0 | 19 |
| USER | 181 | 181 | 0 | 0 | 359 | 540 |
| WEP4UTIL | 1 | 0 | 1 | 0 | 0 | 1 |
| WEPUTIL | 1 | 0 | 1 | 0 | 0 | 1 |
| WIN87EM | 1 | 0 | 1 | 0 | 0 | 1 |
| WINMM | 2 | 1 | 0 | 1 | 0 | 2 |
| **Total** | **429** | **412** | **3** | **14** | **1018** | **1447** |

"Implemented" = a real C handler in `g_api_table` runs and does the work.
"ordinal+name" entries are matched either way (most apps import by
ordinal; the name is recorded for readability and WINE-spec cross-check).
"ordinal-only" entries have no by-name alias registered (an app importing
that function by name instead of ordinal would MISS). "name-only" entries
(SHELL.DLL, PRNDRV, ...) have no fixed ordinal in the source Win3.1 DLLs and
are matched by import name exclusively.

"Stubbed" = `g_stub_table` supplies the exact Pascal argbyte count so the
caller's stack does not desync, and a canned return value, but no real
behaviour happens. Everything outside both tables is unimplemented and is
discovered per-application at runtime; see "Ordinal discovery" below.

Note: 3 (module, ordinal) pairs appear in both tables; the real handler
always wins at dispatch, so these stub entries are currently dead code:
- GDI.38
- USER.61
- USER.319

## Ordinal discovery

Coverage above is closed: it is exactly the two tables in
`kernel/exec/win16api.c`. What an untested application needs is not
closed, and this generator does not guess at it. The interpreter itself
is the discovery tool: `win16_api_dispatch()` looks up every call an app
makes against `g_api_table` then `g_stub_table`, in that order:

- Found in `g_api_table`: real handler runs, logged `ok` (first
  occurrence only, to `/WIN16LOG.TXT`).
- Found in `g_stub_table` only: stack is popped with the exact recorded
  `argbytes` and the canned `retval` is returned, logged `ok` as well
  (the source code's own dedup trace does not distinguish real from
  stubbed at this log line; the two generated tables above are what
  distinguishes them).
- Found in neither: logged `MISS` on first occurrence, then
  `UNIMPL <module>.<ordinal-or-name> (args unknown)`. Because the true
  argbyte count is unknown, the dispatcher can only pop the far-call
  return frame, not the caller's pushed arguments, and returns AX=0.
  **This is a real, previously-hit failure mode**: an unimplemented
  import desyncs the emulated Pascal stack by exactly the number of
  bytes the app expected popped, corrupting whatever the next `POP` or
  `RET` reads. The fix pattern used throughout this file's history is
  always the same: find the correct argbyte count (from a WINE .spec
  file or by reading the caller's own push sequence) and add either a
  real handler or, at minimum, a `g_stub_table` entry so the stack
  stays consistent even before real behaviour is implemented.

Practical effect: run the target application, then read
`/WIN16LOG.TXT` (or `/BOOTLOG.TXT` for boot-time/autolaunch traces) for
`MISS`/`UNIMPL` lines naming exactly what that application needs next.

## Implemented API table

Grouped by module, sorted by ordinal (0 = matched by import name only,
not by ordinal; the app imported this function by name).

### GDI (115 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 1 | SETBKCOLOR | `g_setbkcolor` |
| 2 | SETBKMODE | `g_setbkmode` |
| 3 | SETMAPMODE | `g_setmapmode` |
| 4 | SETROP2 | `g_setrop2` |
| 6 | SETPOLYFILLMODE | `g_setpolyfill` |
| 7 | SETSTRETCHBLTMODE | `g_setstretchmode` |
| 9 | SETTEXTCOLOR | `g_settextcolor` |
| 11 | SETWINDOWORG | `g_setwindoworg` |
| 12 | SETWINDOWEXT | `g_setwindowext` |
| 13 | SETVIEWPORTORG | `g_setvieworg` |
| 14 | SETVIEWPORTEXT | `g_setviewext` |
| 15 | OFFSETWINDOWORG | `g_offsetwindoworg` |
| 16 | SCALEWINDOWEXT | `g_scalewindowext` |
| 17 | OFFSETVIEWPORTORG | `g_offsetvieworg` |
| 18 | SCALEVIEWPORTEXT | `g_scaleviewext` |
| 19 | LINETO | `g_lineto` |
| 20 | MOVETO | `g_moveto` |
| 21 | EXCLUDECLIPRECT | `g_excludecliprect` |
| 22 | INTERSECTCLIPRECT | `g_intersectcliprect` |
| 23 | ARC | `g_arc` |
| 24 | ELLIPSE | `g_ellipse` |
| 26 | PIE | `g_pie` |
| 27 | RECTANGLE | `g_rectangle` |
| 28 | ROUNDRECT | `g_roundrect` |
| 29 | PATBLT | `g_patblt` |
| 30 | SAVEDC | `g_savedc` |
| 31 | SETPIXEL | `g_setpixel` |
| 33 | TEXTOUT | `g_textout` |
| 34 | BITBLT | `g_bitblt` |
| 35 | STRETCHBLT | `g_stretchblt` |
| 36 | POLYGON | `g_polygon` |
| 37 | POLYLINE | `g_polyline` |
| 38 | ESCAPE | `g_escape` |
| 39 | RESTOREDC | `g_restoredc` |
| 44 | SELECTCLIPRGN | `g_selectcliprgn` |
| 45 | SELECTOBJECT | `g_selectobject` |
| 48 | CREATEBITMAP | `g_createbitmap` |
| 49 | CREATEBITMAPINDIRECT | `g_createbitmapindirect` |
| 50 | CREATEBRUSHINDIRECT | `g_createbrushindirect` |
| 51 | CREATECOMPATIBLEBITMAP | `g_createcompatiblebitmap` |
| 52 | CREATECOMPATIBLEDC | `g_createcompatibledc` |
| 53 | CREATEDC | `g_createdc` |
| 54 | CREATEELLIPTICRGN | `g_createregion8` |
| 55 | CREATEELLIPTICRGNINDIRECT | `g_createregion4` |
| 56 | CREATEFONT | `g_createfont` |
| 57 | CREATEFONTINDIRECT | `g_createfontindirect` |
| 58 | CREATEHATCHBRUSH | `g_createhatchbrush` |
| 60 | CREATEPATTERNBRUSH | `g_createpatternbrush` |
| 61 | CREATEPEN | `g_createpen` |
| 62 | CREATEPENINDIRECT | `g_createpenindirect` |
| 63 | CREATEPOLYGONRGN | `g_createpolygonrgn` |
| 64 | CREATERECTRGN | `g_createrectrgn` |
| 65 | CREATERECTRGNINDIRECT | `g_createrectrgnindirect` |
| 66 | CREATESOLIDBRUSH | `g_createsolidbrush` |
| 67 | DPTOLP | `g_dptolp` |
| 68 | DELETEDC | `g_deletedc` |
| 69 | DELETEOBJECT | `g_deleteobject` |
| 70 | ENUMFONTS | `g_enumfonts` |
| 75 | GETBKCOLOR | `g_getbkcolor` |
| 76 | GETBKMODE | `g_getbkmode` |
| 77 | GETCLIPBOX | `g_getclipbox` |
| 78 | GETCURRENTPOSITION | `g_getcurrentpos` |
| 80 | GETDEVICECAPS | `g_getdevicecaps` |
| 81 | GETMAPMODE | `g_getmapmode` |
| 82 | GETOBJECT | `g_getobject` |
| 83 | GETPIXEL | `g_getpixel` |
| 84 | GETPOLYFILLMODE | `g_getpolyfill` |
| 85 | GETROP2 | `g_getrop2` |
| 87 | GETSTOCKOBJECT | `g_getstockobject` |
| 88 | GETSTRETCHBLTMODE | `g_getstretchmode` |
| 90 | GETTEXTCOLOR | `g_gettextcolor` |
| 91 | GETTEXTEXTENT | `g_gettextextent` |
| 92 | GETTEXTFACE | `g_gettextface` |
| 93 | GETTEXTMETRICS | `g_gettextmetrics` |
| 94 | GETVIEWPORTEXT | `g_getviewportext` |
| 95 | GETVIEWPORTORG | `g_getviewportorg` |
| 96 | GETWINDOWEXT | `g_getwindowext` |
| 97 | GETWINDOWORG | `g_getwindoworg` |
| 99 | LPTODP | `g_lptodp` |
| 100 | LINEDDA | `g_linedda` |
| 104 | RECTVISIBLE | `g_rectvisible` |
| 106 | SETBITMAPBITS | `g_setbitmapbits` |
| 128 | MULDIV | `g_muldiv` |
| 134 | GETRGNBOX | `g_getrgnbox` |
| 136 | REMOVEFONTRESOURCE | `g_removefontresource` |
| 153 | CREATEIC | `g_createic` |
| 154 | GETNEARESTCOLOR | `g_getnearestcolor` |
| 156 | CREATEDISCARDABLEBITMAP | `g_creatdiscardablebmp` |
| 193 | SETBOUNDSRECT | `g_setboundsrect` |
| 194 | GETBOUNDSRECT | `g_getboundsrect` |
| 345 | GETTEXTALIGN | `g_gettextalign_r` |
| 346 | SETTEXTALIGN | `g_settextalign` |
| 348 | CHORD | `g_pie` |
| 349 | SETMAPPERFLAGS | `g_setmapperflags` |
| 350 | GETCHARWIDTH | `g_getcharwidth` |
| 351 | EXTTEXTOUT | `g_exttextout` |
| 360 | CREATEPALETTE | `g_createpalette` |
| 400 | FASTWINDOWFRAME | `g_fastwindowframe` |
| 439 | STRETCHDIBITS | `g_stretchdibits` |
| 442 | CREATEDIBITMAP | `g_createdibitmap` |
| 443 | SETDIBITSTODEVICE | `g_setdibitstodevice` |
| 444 | CREATEROUNDRECTRGN | `g_createroundrectrgn` |
| 445 | CREATEDIBPATTERNBRUSH | `g_createdibpatternbrush` |
| 450 | POLYPOLYGON | `g_polypolygon` |
| 470 | GETCURRENTPOSITIONEX | `g_getcurposex` |
| 471 | GETTEXTEXTENTPOINT | `g_gettextextentpoint` |
| 472 | GETVIEWPORTEXTEX | `g_getviewportextex` |
| 473 | GETVIEWPORTORGEX | `g_getviewportorgex` |
| 474 | GETWINDOWEXTEX | `g_getwindowextex` |
| 475 | GETWINDOWORGEX | `g_getwindoworgex` |
| 479 | SETVIEWPORTEXTEX | `g_setviewportextex` |
| 480 | SETVIEWPORTORGEX | `g_setviewportorgex` |
| 481 | SETWINDOWEXTEX | `g_setwindowextex` |
| 482 | SETWINDOWORGEX | `g_setwindoworgex` |
| 483 | MOVETOEX | `g_movetoex` |

### KERNEL (85 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| (by name) | GETCURRENTTASK | `k_getcurrenttask` |
| 1 | FATALEXIT | `k_word1_ok` |
| 3 | GETVERSION | `k_getversion` |
| 4 | LOCALINIT | `k_localinit` |
| 5 | LOCALALLOC | `k_localalloc` |
| 6 | LOCALREALLOC | `k_localrealloc` |
| 7 | LOCALFREE | `k_localfree` |
| 8 | LOCALLOCK | `k_locallock` |
| 9 | LOCALUNLOCK | `k_localunlock` |
| 10 | LOCALSIZE | `k_localsize` |
| 13 | LOCALCOMPACT | `k_localcompact` |
| 15 | GLOBALALLOC | `k_globalalloc` |
| 16 | GLOBALREALLOC | `k_globalrealloc` |
| 17 | GLOBALFREE | `k_globalfree` |
| 18 | GLOBALLOCK | `k_globallock` |
| 19 | GLOBALUNLOCK | `k_globalunlock` |
| 20 | GLOBALSIZE | `k_globalsize` |
| 21 | GLOBALHANDLE | `k_globalhandle` |
| 23 | LOCKSEGMENT | `k_locksegment` |
| 24 | UNLOCKSEGMENT | `k_locksegment` |
| 25 | GLOBALCOMPACT | `k_globalcompact` |
| 29 | GLOBALMASTERHANDLE | `k_globalmasterhandle` |
| 30 | WAITEVENT | `k_waitevent` |
| 36 | GETCURRENTTASK | `k_getcurrenttask` |
| 37 | GETCURRENTPDB | `k_getcurrentpdb` |
| 47 | GETMODULEHANDLE | `k_getmodulehandle` |
| 48 | GETMODULEUSAGE | `k_getmoduleusage` |
| 49 | GETMODULEFILENAME | `k_getmodulefilename` |
| 50 | GETPROCADDRESS | `k_getprocaddress` |
| 51 | MAKEPROCINSTANCE | `k_makeprocinstance` |
| 52 | FREEPROCINSTANCE | `k_freeprocinstance` |
| 55 | CATCH | `k_far1_zero` |
| 56 | THROW | `k_throw` |
| 57 | GETPROFILEINT | `k_getprofileint` |
| 58 | GETPROFILESTRING | `k_getprofilestr` |
| 59 | WRITEPROFILESTRING | `k_writeprofilestr` |
| 60 | FINDRESOURCE | `k_findresource` |
| 61 | LOADRESOURCE | `k_loadresource` |
| 62 | LOCKRESOURCE | `k_lockresource` |
| 63 | FREERESOURCE | `k_freeresource` |
| 64 | ACCESSRESOURCE | `k_accessresource` |
| 65 | SIZEOFRESOURCE | `k_sizeofresource` |
| 74 | OPENFILE | `k_openfile` |
| 81 | _LCLOSE | `k_lclose` |
| 82 | _LREAD | `k_lread` |
| 83 | _LCREAT | `k_lcreat` |
| 84 | _LLSEEK | `k_llseek` |
| 85 | _LOPEN | `k_lopen` |
| 86 | _LWRITE | `k_lwrite` |
| 88 | LSTRCPY | `k_lstrcpy` |
| 89 | LSTRCAT | `k_lstrcat` |
| 90 | LSTRLEN | `k_lstrlen` |
| 91 | INITTASK | `k_inittask` |
| 94 | LSTRCMP | `k_lstrcmp` |
| 95 | LOADLIBRARY | `k_loadlibrary` |
| 96 | FREELIBRARY | `k_freelibrary` |
| 102 | DOS3CALL | `k_dos3call` |
| 107 | SETERRORMODE | `k_seterrormode` |
| 115 | OUTPUTDEBUGSTRING | `k_outputdebugstr` |
| 127 | GETPRIVATEPROFILEINT | `k_getprivprofileint` |
| 128 | GETPRIVATEPROFILESTRING | `k_getprivprofilestr` |
| 129 | WRITEPRIVATEPROFILESTRING | `k_writeprivprofile` |
| 131 | GETDOSENVIRONMENT | `k_getdosenv` |
| 132 | GETWINFLAGS | `k_getwinflags` |
| 134 | GETWINDOWSDIRECTORY | `k_getwindir` |
| 135 | GETSYSTEMDIRECTORY | `k_getsysdir` |
| 136 | GETDRIVETYPE | `k_getdrivetype` |
| 137 | FATALAPPEXIT | `k_fatalappexit` |
| 166 | WINEXEC | `k_winexec` |
| 169 | GETFREESPACE | `k_getfreespace` |
| 170 | ALLOCCSTODSALIAS | `k_alloc_alias` |
| 171 | ALLOCDSTOCSALIAS | `k_alloc_alias` |
| 172 | ALLOCSELECTOR | `k_alloc_alias` |
| 176 | FREESELECTOR | `k_freeselector` |
| 177 | PRESTOCHANGOSELECTOR | `k_prestochango` |
| 199 | SELECTORACCESSRIGHTS | `k_selectoraccessrights` |
| 334 | GETSELECTORLIMIT | `k_getselectorlimit` |
| 335 | GETSELECTORBASE | `k_getselectorbase` |
| 336 | SETSELECTORBASE | `k_setselectorbase` |
| 337 | SETSELECTORLIMIT | `k_setselectorlimit` |
| 346 | KERNEL346 | `k_kernel346` |
| 347 | KERNEL347 | `k_kernel347` |
| 348 | HMEMCPY | `k_hmemcpy` |
| 349 | _HREAD | `k_hread` |
| 350 | _HWRITE | `k_kernel350` |

### KEYBOARD (5 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 5 | ANSITOOEM | `kbd_xlate_copy` |
| 6 | OEMTOANSI | `kbd_xlate_copy` |
| 129 | VKKEYSCAN | `kbd_vkkeyscan` |
| 131 | MAPVIRTUALKEY | `kbd_mapvirtualkey` |
| 133 | GETKEYNAMETEXT | `kbd_getkeynametext` |

### LZEXPAND (3 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 1 | LZCOPY | `lz_copy` |
| 2 | LZOPENFILE | `lz_openfile` |
| 6 | LZCLOSE | `lz_close` |

### MMSYSTEM (3 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 2 | SNDPLAYSOUND | `mm_sndplaysound` |
| 3 | PLAYSOUND | `mm_playsound` |
| 5 | MMSYSTEMGETVERSION | `mm_getversion` |

### OLESVR (9 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 2 | OLEREGISTERSERVER | `os_registerserver` |
| 3 | OLEREVOKESERVER | `os_revokeserver` |
| 4 | OLEBLOCKSERVER | `os_blockserver` |
| 5 | OLEUNBLOCKSERVER | `os_unblockserver` |
| 6 | OLEREGISTERSERVERDOC | `os_registerserverdoc` |
| 7 | OLEREVOKESERVERDOC | `os_revokeserverdoc` |
| 8 | OLERENAMESERVERDOC | `os_renameserverdoc` |
| 9 | OLEREVERTSERVERDOC | `os_revertserverdoc` |
| 10 | OLESAVEDSERVERDOC | `os_savedserverdoc` |

### PRNDRV (4 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| (by name) | DEVICECAPABILITIES | `prn_devcaps` |
| (by name) | DEVICEMODE | `prn_devicemode` |
| (by name) | EXTDEVICEMODE | `prn_extdevicemode` |
| (by name) | GENERIC | `prn_generic` |

### SHELL (19 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| (by name) | DRAGACCEPTFILES | `sh_dragacceptfiles` |
| (by name) | DRAGFINISH | `sh_dragfinish` |
| (by name) | DRAGQUERYFILE | `sh_dragqueryfile` |
| (by name) | DRAGQUERYPOINT | `sh_dragquerypoint` |
| (by name) | EXTRACTICON | `sh_extracticon` |
| (by name) | FINDEXECUTABLE | `sh_findexecutable` |
| (by name) | SHELLABOUT | `sh_shellabout` |
| (by name) | SHELLEXECUTE | `sh_shellexecute` |
| 1 | REGOPENKEY | `sh_regopenkey` |
| 2 | REGCREATEKEY | `sh_regcreatekey` |
| 3 | REGCLOSEKEY | `sh_regclosekey` |
| 4 | REGDELETEKEY | `sh_regdeletekey` |
| 5 | REGSETVALUE | `sh_regsetvalue` |
| 6 | REGQUERYVALUE | `sh_regqueryvalue` |
| 7 | REGENUMKEY | `sh_regenumkey` |
| 9 | DRAGACCEPTFILES | `sh_dragacceptfiles` |
| 11 | DRAGQUERYFILE | `sh_dragqueryfile` |
| 12 | DRAGFINISH | `sh_dragfinish` |
| 13 | DRAGQUERYPOINT | `sh_dragquerypoint` |

### USER (181 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 1 | MESSAGEBOX | `u_messagebox` |
| 2 | OLDEXITWINDOWS | `u_oldexitwindows` |
| 5 | INITAPP | `u_initapp` |
| 6 | POSTQUITMESSAGE | `u_postquit` |
| 10 | SETTIMER | `u_settimer` |
| 12 | KILLTIMER | `u_killtimer` |
| 13 | GETTICKCOUNT | `u_gettickcount` |
| 15 | GETCURRENTTIME | `u_gettickcount` |
| 18 | SETCAPTURE | `u_setcapture` |
| 19 | RELEASECAPTURE | `u_releasecapture` |
| 22 | SETFOCUS | `u_word1` |
| 23 | GETFOCUS | `u_getfocus` |
| 28 | CLIENTTOSCREEN | `u_clienttoscreen` |
| 29 | SCREENTOCLIENT | `u_screentoclient` |
| 31 | ISICONIC | `u_isiconic` |
| 32 | GETWINDOWRECT | `u_getwindowrect` |
| 33 | GETCLIENTRECT | `u_getclientrect` |
| 34 | ENABLEWINDOW | `u_enablewindow` |
| 35 | ISWINDOWENABLED | `u_iswindowenabled` |
| 36 | GETWINDOWTEXT | `u_getwindowtext` |
| 37 | SETWINDOWTEXT | `u_setwindowtext` |
| 39 | BEGINPAINT | `u_beginpaint` |
| 40 | ENDPAINT | `u_endpaint` |
| 41 | CREATEWINDOW | `u_createwindow` |
| 42 | SHOWWINDOW | `u_showwindow` |
| 46 | GETPARENT | `u_getparent` |
| 47 | ISWINDOW | `u_iswindow` |
| 49 | ISWINDOWVISIBLE | `u_iswindowvisible` |
| 50 | FINDWINDOW | `u_findwindow` |
| 53 | DESTROYWINDOW | `u_destroywindow` |
| 56 | MOVEWINDOW | `u_movewindow` |
| 57 | REGISTERCLASS | `u_registerclass` |
| 58 | GETCLASSNAME | `u_getclassname` |
| 60 | GETACTIVEWINDOW | `u_getfocus` |
| 61 | SCROLLWINDOW | `u_scrollwindow` |
| 62 | SETSCROLLPOS | `u_setscrollpos` |
| 63 | GETSCROLLPOS | `u_getscrollpos` |
| 64 | SETSCROLLRANGE | `u_setscrollrange` |
| 65 | GETSCROLLRANGE | `u_getscrollrange` |
| 66 | GETDC | `u_getdc` |
| 67 | GETWINDOWDC | `u_getwindowdc` |
| 68 | RELEASEDC | `u_releasedc` |
| 69 | SETCURSOR | `u_setcursor` |
| 70 | SETCURSORPOS | `u_setcursorpos` |
| 71 | SHOWCURSOR | `u_showcursor` |
| 72 | SETRECT | `u_setrect` |
| 73 | SETRECTEMPTY | `u_setrectempty` |
| 74 | COPYRECT | `u_copyrect` |
| 75 | ISRECTEMPTY | `u_isrectempty` |
| 76 | PTINRECT | `u_ptinrect` |
| 77 | OFFSETRECT | `u_offsetrect` |
| 78 | INFLATERECT | `u_inflaterect` |
| 79 | INTERSECTRECT | `u_intersectrect` |
| 81 | FILLRECT | `u_fillrect` |
| 82 | INVERTRECT | `u_invertrect` |
| 83 | FRAMERECT | `u_framerect` |
| 84 | DRAWICON | `u_drawicon` |
| 85 | DRAWTEXT | `u_drawtext` |
| 87 | DIALOGBOX | `u_dialogbox` |
| 88 | ENDDIALOG | `u_enddialog` |
| 89 | CREATEDIALOG | `u_createdialog` |
| 90 | ISDIALOGMESSAGE | `u_isdialogmessage` |
| 91 | GETDLGITEM | `u_getdlgitem` |
| 92 | SETDLGITEMTEXT | `u_setdlgitemtext` |
| 93 | GETDLGITEMTEXT | `u_getdlgitemtext` |
| 94 | SETDLGITEMINT | `u_setdlgitemint` |
| 95 | GETDLGITEMINT | `u_getdlgitemint` |
| 96 | CHECKRADIOBUTTON | `u_checkradiobutton` |
| 97 | CHECKDLGBUTTON | `u_checkdlgbutton` |
| 98 | ISDLGBUTTONCHECKED | `u_isdlgbuttonchecked` |
| 101 | SENDDLGITEMMESSAGE | `u_senddlgitemmessage` |
| 102 | ADJUSTWINDOWRECT | `u_adjustwindowrect` |
| 103 | MAPDIALOGRECT | `u_mapdialogrect` |
| 104 | MESSAGEBEEP | `u_messagebeep` |
| 106 | GETKEYSTATE | `u_getkeystate` |
| 107 | DEFWINDOWPROC | `u_defwindowproc` |
| 108 | GETMESSAGE | `u_getmessage` |
| 109 | PEEKMESSAGE | `u_peekmessage` |
| 110 | POSTMESSAGE | `u_postmessage` |
| 111 | SENDMESSAGE | `u_sendmessage` |
| 112 | WAITMESSAGE | `u_waitmessage` |
| 113 | TRANSLATEMESSAGE | `u_translatemessage` |
| 114 | DISPATCHMESSAGE | `u_dispatchmessage` |
| 115 | REPLYMESSAGE | `u_replymessage` |
| 116 | POSTAPPMESSAGE | `u_postappmessage` |
| 118 | REGISTERWINDOWMESSAGE | `u_registerwindowmessage` |
| 119 | GETMESSAGEPOS | `u_getmessagepos` |
| 120 | GETMESSAGETIME | `u_getmessagetime` |
| 121 | SETWINDOWSHOOK | `u_setwindowshook` |
| 122 | CALLWINDOWPROC | `u_callwindowproc` |
| 124 | UPDATEWINDOW | `u_updatewindow` |
| 125 | INVALIDATERECT | `u_invalidaterect` |
| 127 | VALIDATERECT | `u_validaterect` |
| 131 | GETCLASSLONG | `u_getclasslong` |
| 133 | GETWINDOWWORD | `u_getwindowword` |
| 134 | SETWINDOWWORD | `u_setwindowword` |
| 135 | GETWINDOWLONG | `u_getwindowlong` |
| 136 | SETWINDOWLONG | `u_setwindowlong` |
| 145 | REGISTERCLIPBOARDFORMAT | `u_registerclipboardformat` |
| 150 | LOADMENU | `u_loadmenu` |
| 151 | CREATEMENU | `u_createmenu` |
| 152 | DESTROYMENU | `u_destroymenu` |
| 153 | CHANGEMENU | `u_changemenu` |
| 154 | CHECKMENUITEM | `u_menuitem3` |
| 155 | ENABLEMENUITEM | `u_menuitem3` |
| 156 | GETSYSTEMMENU | `u_getsystemmenu` |
| 157 | GETMENU | `u_getmenu` |
| 158 | SETMENU | `u_setmenu` |
| 159 | GETSUBMENU | `u_getsubmenu` |
| 160 | DRAWMENUBAR | `u_drawmenubar` |
| 161 | GETMENUSTRING | `u_getmenustring` |
| 162 | HILITEMENUITEM | `u_hilitemenuitem` |
| 163 | CREATECARET | `u_createcaret` |
| 164 | DESTROYCARET | `u_destroycaret` |
| 165 | SETCARETPOS | `u_setcaretpos` |
| 166 | HIDECARET | `u_hidecaret` |
| 167 | SHOWCARET | `u_showcaret` |
| 168 | SETCARETBLINKTIME | `u_setcaretblink` |
| 169 | GETCARETBLINKTIME | `u_getcaretblink` |
| 171 | WINHELP | `u_winhelp` |
| 173 | LOADCURSOR | `u_loadcursor` |
| 174 | LOADICON | `u_loadicon` |
| 175 | LOADBITMAP | `u_loadbitmap` |
| 176 | LOADSTRING | `u_loadstring` |
| 177 | LOADACCELERATORS | `u_loadaccel` |
| 178 | TRANSLATEACCELERATOR | `u_translateaccel` |
| 179 | GETSYSTEMMETRICS | `u_getsystemmetrics` |
| 180 | GETSYSCOLOR | `u_getsyscolor` |
| 190 | GETSYSMODALWINDOW | `u_user190_word6` |
| 192 | INSENDMESSAGE | `u_insendmessage` |
| 218 | DIALOGBOXINDIRECT | `u_dialogboxindirect` |
| 219 | CREATEDIALOGINDIRECT | `u_dialogboxindirect` |
| 221 | SCROLLDC | `u_scrolldc` |
| 225 | ENUMTASKWINDOWS | `u_enumtaskwindows` |
| 232 | SETWINDOWPOS | `u_setwindowpos` |
| 239 | DIALOGBOXPARAM | `u_dialogboxparam` |
| 240 | DIALOGBOXINDIRECTPARAM | `u_dialogboxindirectparam` |
| 241 | CREATEDIALOGPARAM | `u_createdialogparam` |
| 242 | CREATEDIALOGINDIRECTPARAM | `u_dialogboxindirectparam` |
| 243 | GETDIALOGBASEUNITS | `u_getdialogbaseunits` |
| 249 | GETASYNCKEYSTATE | `u_getasynckeystate` |
| 250 | GETMENUSTATE | `u_getmenustate` |
| 262 | GETWINDOW | `u_getwindow` |
| 263 | GETMENUITEMCOUNT | `u_getmenuitemcount` |
| 264 | GETMENUITEMID | `u_getmenuitemid` |
| 266 | SETMESSAGEQUEUE | `u_setmessagequeue` |
| 268 | GLOBALADDATOM | `u_globaladdatom` |
| 272 | ISZOOMED | `u_iszoomed` |
| 277 | GETDLGCTRLID | `u_getdlgctrlid` |
| 282 | SELECTPALETTE | `u_selectpalette` |
| 283 | REALIZEPALETTE | `u_realizepalette` |
| 284 | GETFREESYSTEMRESOURCES | `u_getfreesystemresources` |
| 286 | GETDESKTOPWINDOW | `u_getdesktopwindow` |
| 288 | GETMESSAGEEXTRAINFO | `u_getmessageextrainfo` |
| 308 | DEFDLGPROC | `u_defdlgproc` |
| 319 | SCROLLWINDOWEX | `u_scrollwindowex` |
| 334 | GETQUEUESTATUS | `u_getqueuestatus` |
| 335 | GETINPUTSTATE | `u_getinputstate` |
| 358 | ISMENU | `u_ismenu` |
| 403 | UNREGISTERCLASS | `u_unregisterclass` |
| 404 | GETCLASSINFO | `u_getclassinfo` |
| 410 | INSERTMENU | `u_insertmenu` |
| 411 | APPENDMENU | `u_appendmenu` |
| 412 | REMOVEMENU | `u_removemenu` |
| 413 | DELETEMENU | `u_deletemenu` |
| 414 | MODIFYMENU | `u_modifymenu` |
| 415 | CREATEPOPUPMENU | `u_createpopupmenu` |
| 416 | TRACKPOPUPMENU | `u_trackpopupmenu` |
| 420 | WSPRINTF | `u_wsprintf` |
| 421 | WVSPRINTF | `u_wvsprintf` |
| 431 | ANSIUPPER | `u_ansiupper` |
| 435 | ISCHARUPPER | `u_ischarupper` |
| 436 | ISCHARLOWER | `u_ischarlower` |
| 452 | CREATEWINDOWEX | `u_createwindowex` |
| 454 | ADJUSTWINDOWRECTEX | `u_adjustwindowrectex` |
| 457 | DESTROYICON | `u_destroyicon` |
| 471 | LSTRCMPI | `u_lstrcmpi` |
| 472 | ANSINEXT | `u_ansinext` |
| 473 | ANSIPREV | `u_ansiprev` |
| 512 | WNETGETCONNECTION | `u_wnetgetconnection` |
| 513 | WNETGETCAPS | `u_wnetgetcaps` |

### WEP4UTIL (1 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 2 | *(ordinal-only import, no by-name alias registered)* | `k_wep_ok` |

### WEPUTIL (1 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 2 | *(ordinal-only import, no by-name alias registered)* | `k_wep_ok` |

### WIN87EM (1 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| 1 | *(ordinal-only import, no by-name alias registered)* | `k_win87em_init` |

### WINMM (2 implemented)

| Ordinal | Name | Handler |
|---:|---|---|
| (by name) | PLAYSOUNDA | `mm_playsound` |
| 2 | SNDPLAYSOUND | `mm_sndplaysound` |

## Stubbed ordinal table

These ordinals are recognized (the loader will not desync the caller's
stack) but return a fixed value with no real behaviour. `argbytes` is the
exact number of bytes the Pascal calling convention pops off the 16-bit
stack; `retval` is `0` (a "no object"/handle-shaped failure) or `1`
(a BOOL success) per the source's own convention.

### GDI (295 stubbed)

| Ordinal | Name (from source comment) | argbytes | retval |
|---:|---|---:|---:|
| 5 | SetRelAbs | 4 | 1 |
| 8 | SetTextCharacterExtra | 4 | 1 |
| 10 | SetTextJustification | 6 | 1 |
| 25 | FloodFill | 10 | 1 |
| 32 | OffsetClipRgn | 6 | 1 |
| 38 | Escape | 14 | 0 |
| 40 | FillRgn | 6 | 1 |
| 41 | FrameRgn | 10 | 1 |
| 42 | InvertRgn | 4 | 1 |
| 43 | PaintRgn | 4 | 1 |
| 46 | BITMAPBITS | 0 | 1 |
| 47 | CombineRgn | 8 | 1 |
| 71 | EnumObjects | 12 | 0 |
| 72 | EqualRgn | 4 | 1 |
| 73 | ExcludeVisRect | 10 | 1 |
| 74 | GetBitmapBits | 10 | 0 |
| 79 | GetDCOrg | 2 | 0 |
| 86 | GetRelAbs | 2 | 0 |
| 89 | GetTextCharacterExtra | 2 | 0 |
| 98 | IntersectVisRect | 10 | 1 |
| 101 | OffsetRgn | 6 | 1 |
| 102 | OffsetVisRgn | 6 | 1 |
| 103 | PtVisible | 6 | 1 |
| 105 | SelectVisRgn | 4 | 0 |
| 117 | SetDCOrg | 6 | 0 |
| 118 | InternalCreateDC | 0 | 1 |
| 119 | AddFontResource | 4 | 1 |
| 120 | GetContinuingTextExtent | 0 | 0 |
| 121 | Death | 2 | 1 |
| 122 | Resurrection | 14 | 1 |
| 123 | PlayMetaFile | 4 | 1 |
| 124 | GetMetaFile | 4 | 0 |
| 125 | CreateMetaFile | 4 | 0 |
| 126 | CloseMetaFile | 2 | 1 |
| 127 | DeleteMetaFile | 2 | 1 |
| 129 | SaveVisRgn | 2 | 0 |
| 130 | RestoreVisRgn | 2 | 1 |
| 131 | InquireVisRgn | 2 | 0 |
| 132 | SetEnvironment | 10 | 1 |
| 133 | GetEnvironment | 10 | 0 |
| 137 | GSV | 0 | 1 |
| 138 | DPXlate | 0 | 1 |
| 139 | SetWinViewExt | 0 | 1 |
| 140 | ScaleExt | 0 | 1 |
| 141 | WordSet | 0 | 1 |
| 142 | RectStuff | 0 | 1 |
| 143 | OffsetOrg | 0 | 1 |
| 144 | LockDC | 0 | 1 |
| 145 | UnlockDC | 0 | 1 |
| 146 | LockUnlock | 0 | 1 |
| 147 | GDI_FarFrame | 0 | 1 |
| 148 | SetBrushOrg | 6 | 0 |
| 149 | GetBrushOrg | 2 | 0 |
| 150 | UnrealizeObject | 2 | 1 |
| 151 | CopyMetaFile | 6 | 0 |
| 152 | GDIInitApp | 0 | 1 |
| 155 | QueryAbort | 4 | 0 |
| 157 | CompatibleBitmap | 0 | 1 |
| 158 | EnumCallback | 0 | 0 |
| 159 | GetMetaFileBits | 2 | 0 |
| 160 | SetMetaFileBits | 2 | 1 |
| 161 | PtInRegion | 6 | 1 |
| 162 | GetBitmapDimension | 2 | 0 |
| 163 | SetBitmapDimension | 6 | 0 |
| 164 | PixToLine | 0 | 1 |
| 169 | IsDCDirty | 0 | 1 |
| 170 | SetDCStatus | 0 | 1 |
| 171 | LVBUNION | 0 | 1 |
| 172 | SetRectRgn | 10 | 1 |
| 173 | GetClipRgn | 2 | 0 |
| 174 | BLOAT | 0 | 1 |
| 175 | EnumMetaFile | 12 | 0 |
| 176 | PlayMetaFileRecord | 12 | 1 |
| 177 | RCOS | 0 | 1 |
| 178 | RSIN | 0 | 1 |
| 179 | GetDCState | 2 | 0 |
| 180 | SetDCState | 4 | 1 |
| 181 | RectInRegionOld | 6 | 1 |
| 182 | REQUESTSEM | 0 | 1 |
| 183 | CLEARSEM | 0 | 1 |
| 184 | STUFFVISIBLE | 0 | 1 |
| 185 | STUFFINREGION | 0 | 1 |
| 186 | DELETEABOVELINEFONTS | 0 | 1 |
| 188 | GetTextExtentEx | 0 | 0 |
| 190 | SetDCHook | 10 | 1 |
| 191 | GetDCHook | 6 | 0 |
| 192 | SetHookFlags | 4 | 1 |
| 195 | SelectBitmap | 0 | 0 |
| 196 | SetMetaFileBitsBetter | 2 | 1 |
| 201 | DMBITBLT | 0 | 1 |
| 202 | DMCOLORINFO | 0 | 1 |
| 206 | dmEnumDFonts | 0 | 1 |
| 207 | DMENUMOBJ | 0 | 1 |
| 208 | DMOUTPUT | 0 | 1 |
| 209 | DMPIXEL | 0 | 1 |
| 210 | dmRealizeObject | 0 | 1 |
| 211 | DMSTRBLT | 0 | 1 |
| 212 | DMSCANLR | 0 | 1 |
| 213 | BRUTE | 0 | 1 |
| 214 | DMEXTTEXTOUT | 0 | 1 |
| 215 | DMGETCHARWIDTH | 0 | 1 |
| 216 | DMSTRETCHBLT | 0 | 1 |
| 217 | DMDIBBITS | 0 | 1 |
| 218 | DMSTRETCHDIBITS | 0 | 1 |
| 219 | DMSETDIBTODEV | 0 | 1 |
| 220 | DMTRANSPOSE | 0 | 1 |
| 230 | CreatePQ | 2 | 0 |
| 231 | MinPQ | 2 | 1 |
| 232 | ExtractPQ | 2 | 0 |
| 233 | InsertPQ | 6 | 1 |
| 234 | SizePQ | 4 | 1 |
| 235 | DeletePQ | 2 | 1 |
| 240 | OpenJob | 10 | 0 |
| 241 | WriteSpool | 8 | 1 |
| 242 | WriteDialog | 8 | 1 |
| 243 | CloseJob | 2 | 1 |
| 244 | DeleteJob | 4 | 1 |
| 245 | GetSpoolJob | 6 | 0 |
| 246 | StartSpoolPage | 2 | 1 |
| 247 | EndSpoolPage | 2 | 1 |
| 248 | QueryJob | 0 | 0 |
| 250 | Copy | 10 | 0 |
| 253 | DeleteSpoolPage | 0 | 1 |
| 254 | SpoolFile | 0 | 1 |
| 267 | StartDocPrintEra | 0 | 1 |
| 268 | StartPagePrinter | 0 | 1 |
| 269 | WritePrinter | 0 | 1 |
| 270 | EndPagePrinter | 0 | 1 |
| 271 | AbortPrinter | 0 | 1 |
| 272 | EndDocPrinter | 0 | 1 |
| 274 | ClosePrinter | 0 | 1 |
| 280 | GetRealDriverInfo | 0 | 0 |
| 281 | DrvSetPrinterData | 20 | 0 |
| 282 | DrvGetPrinterData | 24 | 0 |
| 299 | ENGINEGETCHARWIDTHEX | 0 | 1 |
| 300 | EngineEnumerateFont | 12 | 0 |
| 301 | EngineDeleteFont | 4 | 1 |
| 302 | EngineRealizeFont | 12 | 0 |
| 303 | EngineGetCharWidth | 12 | 1 |
| 304 | EngineSetFontContext | 6 | 1 |
| 305 | EngineGetGlyphBMP | 22 | 1 |
| 306 | EngineMakeFontDir | 10 | 0 |
| 307 | GetCharABCWidths | 10 | 0 |
| 308 | GetOutlineTextMetrics | 8 | 0 |
| 309 | GetGlyphOutline | 22 | 0 |
| 310 | CreateScalableFontResource | 14 | 0 |
| 311 | GetFontData | 18 | 0 |
| 312 | ConvertOutLineFontFile | 0 | 1 |
| 313 | GetRasterizerCaps | 6 | 0 |
| 314 | EngineExtTextOut | 0 | 1 |
| 315 | EngineRealizeFontExt | 16 | 0 |
| 316 | EngineGetCharWidthStr | 0 | 1 |
| 317 | EngineGetGlyphBmpExt | 0 | 1 |
| 330 | EnumFontFamilies | 14 | 0 |
| 332 | GetKerningPairs | 8 | 0 |
| 347 | MFDRAWTEXT | 0 | 1 |
| 352 | GetPhysicalFontHandle | 0 | 0 |
| 353 | GetAspectRatioFilter | 0 | 0 |
| 354 | ShrinkGDIHeap | 0 | 1 |
| 355 | FTrapping0 | 0 | 1 |
| 361 | GDISelectPalette | 6 | 1 |
| 362 | GDIRealizePalette | 2 | 1 |
| 363 | GetPaletteEntries | 10 | 0 |
| 364 | SetPaletteEntries | 10 | 1 |
| 365 | RealizeDefaultPalette | 2 | 0 |
| 366 | UpdateColors | 2 | 1 |
| 367 | AnimatePalette | 10 | 1 |
| 368 | ResizePalette | 4 | 1 |
| 370 | GetNearestPaletteIndex | 6 | 0 |
| 372 | ExtFloodFill | 12 | 1 |
| 373 | SetSystemPaletteUse | 4 | 1 |
| 374 | GetSystemPaletteUse | 2 | 0 |
| 375 | GetSystemPaletteEntries | 10 | 0 |
| 376 | ResetDC | 6 | 1 |
| 377 | StartDoc | 6 | 1 |
| 378 | EndDoc | 2 | 1 |
| 379 | StartPage | 2 | 1 |
| 380 | EndPage | 2 | 1 |
| 381 | SetAbortProc | 6 | 1 |
| 382 | AbortDoc | 2 | 1 |
| 401 | GDIMOVEBITMAP | 0 | 1 |
| 402 | GDIGETBITSGLOBAL | 0 | 1 |
| 403 | GdiInit2 | 4 | 1 |
| 404 | GetTTGlyphIndexMap | 0 | 0 |
| 405 | FinalGdiInit | 2 | 1 |
| 406 | CREATEREALBITMAPINDIRECT | 0 | 1 |
| 407 | CreateUserBitmap | 12 | 0 |
| 408 | CREATEREALBITMAP | 0 | 1 |
| 409 | CreateUserDiscardableBitmap | 6 | 0 |
| 410 | IsValidMetaFile | 2 | 1 |
| 411 | GetCurLogFont | 2 | 0 |
| 412 | IsDCCurrentPalette | 2 | 1 |
| 440 | SetDIBits | 18 | 1 |
| 441 | GetDIBits | 18 | 0 |
| 449 | DEVICECOLORMATCH | 0 | 1 |
| 451 | CreatePolyPolygonRgn | 12 | 0 |
| 452 | GdiSeeGdiDo | 8 | 0 |
| 460 | GDITASKTERMINATION | 0 | 1 |
| 461 | SetObjectOwner | 4 | 1 |
| 462 | IsGDIObject | 2 | 1 |
| 463 | MakeObjectPrivate | 4 | 0 |
| 464 | FIXUPBOGUSPUBLISHERMETAFILE | 0 | 1 |
| 465 | RectVisible | 6 | 1 |
| 466 | RectInRegion | 6 | 1 |
| 467 | UNICODETOANSI | 0 | 1 |
| 468 | GetBitmapDimensionEx | 6 | 0 |
| 469 | GetBrushOrgEx | 6 | 0 |
| 476 | OffsetViewportOrgEx | 10 | 1 |
| 477 | OffsetWindowOrgEx | 10 | 1 |
| 478 | SetBitmapDimensionEx | 10 | 1 |
| 484 | ScaleViewportExtEx | 14 | 1 |
| 485 | ScaleWindowExtEx | 14 | 1 |
| 486 | GetAspectRatioFilterEx | 6 | 0 |
| 489 | CreateDIBSection | 20 | 0 |
| 490 | CloseEnhMetafile | 0 | 1 |
| 491 | CopyEnhMetafile | 0 | 0 |
| 492 | CreateEnhMetafile | 0 | 0 |
| 493 | DeleteEnhMetafile | 0 | 1 |
| 495 | GDIComment | 0 | 1 |
| 496 | GetEnhMetafile | 0 | 0 |
| 497 | GetEnhMetafileBits | 0 | 0 |
| 498 | GetEnhMetafileDescription | 0 | 0 |
| 499 | GetEnhMetafileHeader | 0 | 0 |
| 501 | GetEnhMetafilePaletteEntries | 0 | 0 |
| 502 | PolyBezier | 8 | 1 |
| 503 | PolyBezierTo | 8 | 1 |
| 504 | PlayEnhMetafileRecord | 0 | 1 |
| 505 | SetEnhMetafileBits | 0 | 1 |
| 506 | SetMetaRgn | 0 | 1 |
| 508 | ExtSelectClipRgn | 6 | 1 |
| 511 | AbortPath | 2 | 1 |
| 512 | BeginPath | 2 | 1 |
| 513 | CloseFigure | 2 | 1 |
| 514 | EndPath | 2 | 1 |
| 515 | FillPath | 2 | 1 |
| 516 | FlattenPath | 2 | 1 |
| 517 | GetPath | 12 | 0 |
| 518 | PathToRegion | 2 | 1 |
| 519 | SelectClipPath | 4 | 0 |
| 520 | StrokeAndFillPath | 2 | 1 |
| 521 | StrokePath | 2 | 1 |
| 522 | WidenPath | 2 | 1 |
| 523 | ExtCreatePen | 0 | 1 |
| 524 | GetArcDirection | 2 | 0 |
| 525 | SetArcDirection | 4 | 1 |
| 526 | GetMiterLimit | 0 | 0 |
| 527 | SetMiterLimit | 0 | 1 |
| 528 | GDIParametersInfo | 0 | 1 |
| 529 | CreateHalftonePalette | 2 | 0 |
| 530 | RawTextOut | 0 | 1 |
| 531 | RawExtTextOut | 0 | 1 |
| 532 | RawGetTextExtent | 8 | 1 |
| 536 | BiDiLayout | 0 | 1 |
| 538 | BiDiCreateTabString | 0 | 1 |
| 540 | BiDiGlyphOut | 0 | 1 |
| 543 | BiDiGetStringExtent | 0 | 1 |
| 555 | BiDiDeleteString | 0 | 1 |
| 556 | BiDiSetDefaults | 0 | 1 |
| 558 | BiDiGetDefaults | 0 | 1 |
| 560 | BiDiShape | 0 | 1 |
| 561 | BiDiFontComplement | 0 | 1 |
| 564 | BiDiSetKashida | 0 | 1 |
| 565 | BiDiKExtTextOut | 0 | 1 |
| 566 | BiDiShapeEx | 0 | 1 |
| 569 | BiDiCreateStringEx | 0 | 1 |
| 571 | GetTextExtentRtoL | 0 | 0 |
| 572 | GetHDCCharSet | 0 | 0 |
| 573 | BiDiLayoutEx | 0 | 1 |
| 602 | SetDIBColorTable | 10 | 1 |
| 603 | GetDIBColorTable | 10 | 0 |
| 604 | SetSolidBrush | 6 | 1 |
| 605 | SysDeleteObject | 2 | 1 |
| 606 | SetMagicColors | 8 | 1 |
| 607 | GetRegionData | 10 | 0 |
| 608 | ExtCreateRegion | 0 | 1 |
| 609 | GdiFreeResources | 4 | 1 |
| 610 | GdiSignalProc32 | 14 | 1 |
| 611 | GetRandomRgn | 0 | 0 |
| 612 | GetTextCharset | 2 | 0 |
| 613 | EnumFontFamiliesEx | 18 | 0 |
| 614 | AddLpkToGDI | 0 | 1 |
| 615 | GetCharacterPlacement | 0 | 0 |
| 616 | GetFontLanguageInfo | 2 | 0 |
| 650 | BuildInverseTableDIB | 0 | 1 |
| 701 | GDITHKCONNECTIONDATALS | 0 | 1 |
| 702 | FT_GDIFTHKTHKCONNECTIONDATA | 0 | 1 |
| 703 | FDTHKCONNECTIONDATASL | 0 | 1 |
| 704 | ICMTHKCONNECTIONDATASL | 0 | 1 |
| 820 | ICMCreateTransform | 0 | 1 |
| 821 | ICMDeleteTransform | 0 | 1 |
| 822 | ICMTranslateRGB | 0 | 1 |
| 823 | ICMTranslateRGBs | 0 | 1 |
| 824 | ICMCheckColorsInGamut | 0 | 1 |
| 1000 | SetLayout | 6 | 1 |
| 1001 | GetLayout | 0 | 0 |

### KERNEL (364 stubbed)

| Ordinal | Name (from source comment) | argbytes | retval |
|---:|---|---:|---:|
| 2 | ExitKernel | 0 | 1 |
| 11 | LocalHandle | 2 | 1 |
| 12 | LocalFlags | 2 | 1 |
| 14 | LocalNotify | 4 | 0 |
| 22 | GlobalFlags | 2 | 1 |
| 26 | GlobalFreeAll | 2 | 1 |
| 27 | GetModuleName | 8 | 0 |
| 28 | GlobalMasterHandle | 0 | 0 |
| 31 | PostEvent | 2 | 1 |
| 32 | SetPriority | 4 | 1 |
| 33 | LockCurrentTask | 2 | 1 |
| 34 | SetTaskQueue | 4 | 1 |
| 35 | GetTaskQueue | 2 | 0 |
| 38 | SetTaskSignalProc | 6 | 0 |
| 39 | SetTaskSwitchProc | 0 | 1 |
| 40 | SetTaskInterchange | 0 | 1 |
| 41 | EnableDos | 0 | 1 |
| 42 | DisableDos | 0 | 1 |
| 43 | IsScreenGrab | 0 | 1 |
| 44 | BuildPDB | 0 | 1 |
| 45 | LoadModule | 8 | 0 |
| 46 | FreeModule | 2 | 1 |
| 53 | CallProcInstance | 0 | 1 |
| 54 | GetInstanceData | 6 | 0 |
| 66 | AllocResource | 8 | 0 |
| 67 | SetResourceHandler | 10 | 0 |
| 68 | InitAtomTable | 2 | 1 |
| 69 | FindAtom | 4 | 0 |
| 70 | AddAtom | 4 | 1 |
| 71 | DeleteAtom | 2 | 1 |
| 72 | GetAtomName | 8 | 0 |
| 73 | GetAtomHandle | 2 | 0 |
| 75 | OpenPathName | 0 | 0 |
| 76 | DeletePathName | 0 | 1 |
| 77 | Reserved1 | 4 | 0 |
| 78 | Reserved2 | 8 | 0 |
| 79 | Reserved3 | 4 | 0 |
| 80 | Reserved4 | 4 | 0 |
| 87 | Reserved5 | 8 | 1 |
| 92 | GetTempDrive | 2 | 0 |
| 93 | GetCodeHandle | 4 | 0 |
| 97 | GetTempFileName | 12 | 0 |
| 98 | GetLastDiskChange | 0 | 0 |
| 99 | GetLPErrMode | 0 | 0 |
| 100 | ValidateCodeSegments | 0 | 1 |
| 101 | NoHookDosCall | 0 | 1 |
| 103 | NetBIOSCall | 0 | 0 |
| 104 | GetCodeInfo | 8 | 0 |
| 105 | GetExeVersion | 0 | 0 |
| 106 | SetSwapAreaSize | 2 | 0 |
| 108 | SwitchStackTo | 6 | 1 |
| 109 | SwitchStackBack | 0 | 0 |
| 110 | PatchCodeHandle | 2 | 0 |
| 111 | GlobalWire | 2 | 0 |
| 112 | GlobalUnWire | 2 | 1 |
| 116 | InitLib | 0 | 1 |
| 117 | OldYield | 0 | 1 |
| 118 | GetTaskQueueDS | 0 | 0 |
| 119 | GetTaskQueueES | 0 | 0 |
| 120 | UndefDynLink | 0 | 1 |
| 121 | LocalShrink | 4 | 1 |
| 122 | IsTaskLocked | 0 | 1 |
| 123 | KbdRst | 0 | 1 |
| 124 | EnableKernel | 0 | 1 |
| 125 | DisableKernel | 0 | 1 |
| 126 | MemoryFreed | 0 | 1 |
| 130 | FileCDR | 4 | 0 |
| 133 | GetExePtr | 2 | 0 |
| 138 | GetHeapSpaces | 2 | 0 |
| 139 | DoSignal | 0 | 1 |
| 140 | SetSigHandler | 16 | 1 |
| 141 | InitTask1 | 0 | 1 |
| 142 | GetProfileSectionNames | 6 | 0 |
| 143 | GetPrivateProfileSectionNames | 10 | 0 |
| 144 | CreateDirectory | 8 | 0 |
| 145 | RemoveDirectory | 4 | 1 |
| 146 | DeleteFile | 4 | 1 |
| 147 | SetLastError | 4 | 1 |
| 148 | GetLastError | 0 | 0 |
| 149 | GetVersionEx | 4 | 0 |
| 150 | DirectedYield | 2 | 1 |
| 151 | WinOldApCall | 0 | 1 |
| 152 | GetNumTasks | 0 | 0 |
| 154 | GlobalNotify | 4 | 1 |
| 155 | GetTaskDS | 0 | 0 |
| 156 | LimitEMSPages | 4 | 0 |
| 157 | GetCurPID | 4 | 0 |
| 158 | IsWinOldApTask | 2 | 1 |
| 159 | GlobalHandleNoRIP | 2 | 0 |
| 160 | EMSCopy | 0 | 1 |
| 161 | LocalCountFree | 0 | 1 |
| 162 | LocalHeapSize | 0 | 1 |
| 163 | GlobalLRUOldest | 2 | 1 |
| 164 | GlobalLRUNewest | 2 | 1 |
| 165 | A20Proc | 2 | 1 |
| 167 | GetExpWinVer | 2 | 0 |
| 168 | DirectResAlloc | 6 | 1 |
| 175 | AllocSelector | 2 | 0 |
| 180 | LongPtrAdd | 8 | 1 |
| 184 | GlobalDOSAlloc | 4 | 0 |
| 185 | GlobalDOSFree | 2 | 1 |
| 186 | GetSelectorBase | 2 | 0 |
| 187 | SetSelectorBase | 6 | 1 |
| 188 | GetSelectorLimit | 2 | 0 |
| 189 | SetSelectorLimit | 6 | 1 |
| 191 | GlobalPageLock | 2 | 1 |
| 192 | GlobalPageUnlock | 2 | 1 |
| 196 | SelectorAccessRights | 6 | 0 |
| 197 | GlobalFix | 2 | 1 |
| 198 | GlobalUnfix | 2 | 1 |
| 200 | ValidateFreeSpaces | 0 | 1 |
| 201 | ReplaceInst | 0 | 1 |
| 202 | RegisterPtrace | 0 | 1 |
| 203 | DebugBreak | 0 | 0 |
| 204 | SwapRecording | 0 | 1 |
| 205 | CVWBreak | 0 | 1 |
| 206 | AllocSelectorArray | 2 | 0 |
| 207 | IsDBCSLeadByte | 2 | 1 |
| 208 | K208 | 14 | 0 |
| 209 | K209 | 14 | 0 |
| 210 | K210 | 18 | 0 |
| 211 | K211 | 10 | 0 |
| 213 | K213 | 12 | 0 |
| 214 | K214 | 10 | 0 |
| 215 | K215 | 6 | 0 |
| 216 | RegEnumKey | 16 | 0 |
| 217 | RegOpenKey | 12 | 0 |
| 218 | RegCreateKey | 12 | 0 |
| 219 | RegDeleteKey | 8 | 0 |
| 220 | RegCloseKey | 4 | 0 |
| 221 | RegSetValue | 20 | 0 |
| 222 | RegDeleteValue | 8 | 0 |
| 223 | RegEnumValue | 32 | 0 |
| 224 | RegQueryValue | 16 | 0 |
| 225 | RegQueryValueEx | 24 | 0 |
| 226 | RegSetValueEx | 24 | 0 |
| 227 | RegFlushKey | 4 | 0 |
| 228 | K228 | 2 | 1 |
| 229 | K229 | 4 | 1 |
| 230 | GlobalSmartPageLock | 2 | 0 |
| 231 | GlobalSmartPageUnlock | 2 | 0 |
| 232 | RegLoadKey | 0 | 1 |
| 233 | RegUnloadKey | 0 | 1 |
| 234 | RegSaveKey | 0 | 1 |
| 235 | InvalidateNlsCache | 0 | 1 |
| 236 | GetProductName | 0 | 0 |
| 237 | K237 | 0 | 1 |
| 262 | WOWWaitForMsgAndEvent | 0 | 1 |
| 263 | WOWMsgBox | 0 | 1 |
| 273 | K273 | 0 | 1 |
| 274 | GetShortPathName | 10 | 0 |
| 310 | LocalHandleDelta | 2 | 1 |
| 311 | GetSetKernelDOSProc | 4 | 0 |
| 314 | DebugDefineSegment | 0 | 1 |
| 315 | WriteOutProfiles | 0 | 1 |
| 316 | GetFreeMemInfo | 0 | 0 |
| 318 | FatalExitHook | 0 | 1 |
| 319 | FlushCachedFileHandle | 0 | 1 |
| 320 | IsTask | 2 | 1 |
| 323 | IsRomModule | 2 | 1 |
| 324 | LogError | 6 | 1 |
| 325 | LogParamError | 10 | 1 |
| 326 | IsRomFile | 2 | 1 |
| 327 | K327 | 0 | 0 |
| 328 | _DebugOutput | 6 | 1 |
| 329 | K329 | 6 | 1 |
| 338 | HasGPHandler | 4 | 1 |
| 339 | DiagQuery | 0 | 1 |
| 340 | DiagOutput | 4 | 1 |
| 341 | ToolHelpHook | 4 | 0 |
| 343 | RegisterWinOldApHook | 0 | 1 |
| 344 | GetWinOldApHooks | 0 | 0 |
| 345 | IsSharedSelector | 2 | 1 |
| 351 | BUNNY_351 | 0 | 1 |
| 352 | lstrcatn | 10 | 0 |
| 353 | lstrcpyn | 10 | 0 |
| 354 | GetAppCompatFlags | 2 | 0 |
| 355 | GetWinDebugInfo | 6 | 0 |
| 356 | SetWinDebugInfo | 4 | 1 |
| 357 | MapSL | 4 | 0 |
| 358 | MapLS | 4 | 0 |
| 359 | UnMapLS | 4 | 0 |
| 360 | OpenFileEx | 10 | 0 |
| 361 | PIGLET_361 | 0 | 1 |
| 362 | ThunkTerminateProcess | 0 | 1 |
| 365 | GlobalChangeLockCount | 4 | 0 |
| 403 | FarSetOwner | 4 | 1 |
| 404 | FarGetOwner | 2 | 1 |
| 406 | WritePrivateProfileStruct | 18 | 1 |
| 407 | GetPrivateProfileStruct | 18 | 0 |
| 408 | KERNEL_408 | 0 | 1 |
| 409 | KERNEL_409 | 0 | 1 |
| 410 | CreateProcessFromWinExec | 0 | 0 |
| 411 | GetCurrentDirectory | 8 | 0 |
| 412 | SetCurrentDirectory | 4 | 1 |
| 413 | FindFirstFile | 8 | 0 |
| 414 | FindNextFile | 6 | 0 |
| 415 | FindClose | 2 | 0 |
| 416 | WritePrivateProfileSection | 12 | 1 |
| 417 | WriteProfileSection | 8 | 1 |
| 418 | GetPrivateProfileSection | 14 | 0 |
| 419 | GetProfileSection | 10 | 0 |
| 420 | GetFileAttributes | 4 | 0 |
| 421 | SetFileAttributes | 8 | 1 |
| 422 | GetDiskFreeSpace | 20 | 0 |
| 423 | LogApiThk | 4 | 1 |
| 431 | IsPeFormat | 6 | 1 |
| 432 | FileTimeToLocalFileTime | 0 | 1 |
| 434 | UnicodeToAnsi | 10 | 1 |
| 435 | GetTaskFlags | 0 | 0 |
| 436 | _ConfirmSysLevel | 4 | 1 |
| 437 | _CheckNotSysLevel | 4 | 1 |
| 438 | _CreateSysLevel | 8 | 1 |
| 439 | _EnterSysLevel | 4 | 1 |
| 440 | _LeaveSysLevel | 4 | 1 |
| 441 | CreateThread16 | 24 | 0 |
| 442 | VWin32_EventCreate | 0 | 0 |
| 443 | VWin32_EventDestroy | 4 | 0 |
| 444 | Local32Info | 6 | 1 |
| 445 | Local32First | 6 | 1 |
| 446 | Local32Next | 4 | 1 |
| 447 | WIN32_OldYield | 0 | 1 |
| 448 | KERNEL_448 | 0 | 1 |
| 449 | GetpWin16Lock | 0 | 0 |
| 450 | VWin32_EventWait | 4 | 0 |
| 451 | VWin32_EventSet | 4 | 0 |
| 452 | LoadLibrary32 | 4 | 0 |
| 453 | GetProcAddress32 | 8 | 0 |
| 456 | DefResourceHandler | 6 | 0 |
| 457 | CreateW32Event | 8 | 0 |
| 458 | SetW32Event | 4 | 0 |
| 459 | ResetW32Event | 4 | 0 |
| 460 | WaitForSingleObject | 8 | 0 |
| 461 | WaitForMultipleObjects | 16 | 0 |
| 462 | GetCurrentThreadId | 0 | 0 |
| 463 | SetThreadQueue | 6 | 0 |
| 464 | GetThreadQueue | 4 | 0 |
| 465 | NukeProcess | 0 | 1 |
| 466 | ExitProcess | 2 | 1 |
| 467 | WOACreateConsole | 0 | 1 |
| 468 | WOASpawnConApp | 0 | 1 |
| 469 | WOAGimmeTitle | 0 | 1 |
| 470 | WOADestroyConsole | 0 | 1 |
| 471 | GetCurrentProcessId | 0 | 0 |
| 472 | MapHInstLS | 0 | 0 |
| 473 | MapHInstSL | 0 | 0 |
| 474 | CloseW32Handle | 4 | 0 |
| 475 | GetTEBSelectorFS | 0 | 0 |
| 476 | ConvertToGlobalHandle | 4 | 0 |
| 477 | WOAFullScreen | 0 | 1 |
| 478 | WOATerminateProcess | 0 | 1 |
| 479 | KERNEL_479 | 4 | 0 |
| 480 | _EnterWin16Lock | 0 | 1 |
| 481 | _LeaveWin16Lock | 0 | 1 |
| 482 | LoadSystemLibrary32 | 4 | 0 |
| 483 | MapProcessHandle | 4 | 0 |
| 484 | SetProcessDword | 10 | 0 |
| 485 | GetProcessDword | 6 | 0 |
| 486 | FreeLibrary32 | 4 | 0 |
| 487 | GetModuleFileName32 | 10 | 0 |
| 488 | GetModuleHandle32 | 4 | 0 |
| 489 | KERNEL_489 | 0 | 1 |
| 490 | KERNEL_490 | 2 | 1 |
| 491 | RegisterServiceProcess | 8 | 0 |
| 492 | WOAAbort | 0 | 1 |
| 493 | UTInit | 16 | 1 |
| 494 | KERNEL_494 | 0 | 1 |
| 495 | WaitForMultipleObjectsEx | 20 | 0 |
| 500 | WOW16Call | 6 | 0 |
| 501 | KDDBGOUT | 0 | 1 |
| 502 | WOWGETNEXTVDMCOMMAND | 0 | 1 |
| 503 | WOWREGISTERSHELLWINDOWHANDLE | 0 | 1 |
| 504 | WOWLOADMODULE | 0 | 1 |
| 505 | WOWQUERYPERFORMANCECOUNTER | 0 | 1 |
| 506 | WOWCURSORICONOP | 0 | 1 |
| 507 | WOWFAILEDEXEC | 0 | 1 |
| 508 | WOWCLOSECOMPORT | 0 | 1 |
| 511 | WOWKILLREMOTETASK | 0 | 1 |
| 512 | WOWQUERYDEBUG | 0 | 1 |
| 513 | LoadLibraryEx32W | 12 | 0 |
| 514 | FreeLibrary32W | 4 | 0 |
| 515 | GetProcAddress32W | 8 | 0 |
| 516 | GetVDMPointer32W | 6 | 0 |
| 517 | CallProc32W | 12 | 0 |
| 518 | _CallProcEx32W | 12 | 0 |
| 519 | EXITKERNELTHUNK | 0 | 1 |
| 533 | ConvertDDEHandleLS | 0 | 1 |
| 534 | ConvertDDEHandleSL | 0 | 1 |
| 535 | VWin32_BoostThreadGroup | 8 | 0 |
| 536 | VWin32_BoostThreadStatic | 8 | 0 |
| 537 | KERNEL_537 | 0 | 1 |
| 538 | ThunkTheTemplateHandle | 0 | 1 |
| 540 | KERNEL_540 | 0 | 1 |
| 541 | WOWSETEXITONLASTAPP | 0 | 1 |
| 542 | KERNEL_542 | 0 | 1 |
| 543 | KERNEL_543 | 0 | 1 |
| 544 | WOWSETCOMPATHANDLE | 0 | 1 |
| 560 | SetThunkletCallbackGlue | 8 | 0 |
| 561 | AllocLSThunkletCallback | 8 | 0 |
| 562 | AllocSLThunkletCallback | 8 | 0 |
| 563 | FindLSThunkletCallback | 8 | 0 |
| 564 | FindSLThunkletCallback | 8 | 0 |
| 566 | KERNEL_566 | 0 | 1 |
| 567 | AllocLSThunkletCallbackEx | 10 | 0 |
| 568 | AllocSLThunkletCallbackEx | 10 | 0 |
| 600 | AllocCodeAlias | 0 | 0 |
| 601 | FreeCodeAlias | 0 | 1 |
| 602 | GetDummyModuleHandleDS | 0 | 0 |
| 603 | KERNEL_603 | 0 | 1 |
| 604 | CBClientGlueSL | 0 | 0 |
| 605 | AllocSLThunkletCallback_dup | 8 | 0 |
| 606 | AllocLSThunkletCallback_dup | 8 | 0 |
| 607 | AllocLSThunkletSysthunk | 12 | 0 |
| 608 | AllocSLThunkletSysthunk | 12 | 0 |
| 609 | FindLSThunkletCallback_dup | 8 | 0 |
| 610 | FindSLThunkletCallback_dup | 8 | 0 |
| 611 | FreeThunklet | 8 | 1 |
| 612 | IsSLThunklet | 4 | 1 |
| 613 | HugeMapLS | 0 | 1 |
| 614 | HugeUnMapLS | 0 | 1 |
| 615 | ConvertDialog32To16 | 12 | 1 |
| 616 | ConvertMenu32To16 | 12 | 1 |
| 617 | GetMenu32Size | 4 | 0 |
| 618 | GetDialog32Size | 4 | 0 |
| 619 | RegisterCBClient | 10 | 1 |
| 620 | CBClientThunkSL | 0 | 0 |
| 621 | CBClientThunkSLEx | 0 | 0 |
| 622 | UnRegisterCBClient | 10 | 1 |
| 623 | InitCBClient | 4 | 1 |
| 624 | SetFastQueue | 8 | 0 |
| 625 | GetFastQueue | 0 | 0 |
| 626 | SmashEnvironment | 0 | 1 |
| 627 | IsBadFlatReadWritePtr | 10 | 1 |
| 630 | C16ThkSL | 0 | 0 |
| 631 | C16ThkSL01 | 0 | 0 |
| 651 | ThunkConnect16 | 24 | 0 |
| 652 | IsThreadId | 0 | 1 |
| 653 | OkWithKernelToChangeUsers | 0 | 1 |
| 666 | UTGlue16 | 16 | 0 |
| 667 | EntryAddrProc | 4 | 0 |
| 668 | MyAlloc | 6 | 0 |
| 669 | DllEntryPoint | 16 | 1 |
| 700 | SSInit | 0 | 0 |
| 701 | SSOnBigStack | 0 | 1 |
| 702 | SSCall | 0 | 1 |
| 703 | CallProc32WFix | 0 | 1 |
| 704 | SSConfirmSmallStack | 0 | 0 |
| 901 | __wine_vxd_vmm | 0 | 0 |
| 905 | __wine_vxd_timer | 0 | 0 |
| 909 | __wine_vxd_reboot | 0 | 0 |
| 910 | __wine_vxd_vdd | 0 | 0 |
| 912 | __wine_vxd_vmd | 0 | 0 |
| 914 | __wine_vxd_comm | 0 | 0 |
| 923 | __wine_vxd_shell | 0 | 0 |
| 933 | __wine_vxd_pagefile | 0 | 0 |
| 938 | __wine_vxd_apm | 0 | 0 |
| 939 | __wine_vxd_vxdloader | 0 | 0 |
| 945 | __wine_vxd_win32s | 0 | 0 |
| 951 | __wine_vxd_configmg | 0 | 0 |
| 955 | __wine_vxd_enable | 0 | 0 |
| 1990 | __wine_vxd_timerapi | 0 | 0 |
| 2000 | __wine_call_int_handler | 2 | 0 |
| 2001 | __wine_snoop_entry | 0 | 0 |
| 2002 | __wine_snoop_return | 0 | 0 |

### USER (359 stubbed)

| Ordinal | Name (from source comment) | argbytes | retval |
|---:|---|---:|---:|
| 3 | EnableOEMLayer | 0 | 1 |
| 4 | DisableOEMLayer | 0 | 1 |
| 7 | ExitWindows | 6 | 1 |
| 11 | SetSystemTimer | 10 | 1 |
| 14 | GetTimerResolution | 0 | 0 |
| 16 | ClipCursor | 4 | 1 |
| 17 | GetCursorPos | 4 | 0 |
| 20 | SetDoubleClickTime | 2 | 1 |
| 21 | GetDoubleClickTime | 0 | 0 |
| 24 | RemoveProp | 6 | 1 |
| 25 | GetProp | 6 | 0 |
| 26 | SetProp | 8 | 1 |
| 27 | EnumProps | 6 | 0 |
| 30 | WindowFromPoint | 4 | 1 |
| 38 | GetWindowTextLength | 2 | 0 |
| 43 | CloseWindow | 2 | 1 |
| 44 | OpenIcon | 2 | 0 |
| 45 | BringWindowToTop | 2 | 1 |
| 48 | IsChild | 4 | 1 |
| 51 | BEAR51 | 0 | 1 |
| 52 | AnyPopup | 0 | 1 |
| 54 | EnumWindows | 8 | 0 |
| 55 | EnumChildWindows | 10 | 0 |
| 59 | SetActiveWindow | 2 | 1 |
| 61 | ScrollWindow | 14 | 1 |
| 80 | UnionRect | 12 | 1 |
| 86 | IconSize | 0 | 0 |
| 99 | DlgDirSelect | 8 | 1 |
| 100 | DlgDirList | 12 | 1 |
| 105 | FlashWindow | 4 | 1 |
| 117 | WindowFromDC | 2 | 1 |
| 123 | CallMsgFilter | 6 | 1 |
| 126 | InvalidateRgn | 6 | 1 |
| 128 | ValidateRgn | 4 | 1 |
| 129 | GetClassWord | 4 | 0 |
| 130 | SetClassWord | 6 | 1 |
| 132 | SetClassLong | 8 | 0 |
| 137 | OpenClipboard | 2 | 0 |
| 138 | CloseClipboard | 0 | 1 |
| 139 | EmptyClipboard | 0 | 1 |
| 140 | GetClipboardOwner | 0 | 0 |
| 141 | SetClipboardData | 4 | 1 |
| 142 | GetClipboardData | 2 | 0 |
| 143 | CountClipboardFormats | 0 | 1 |
| 144 | EnumClipboardFormats | 2 | 0 |
| 146 | GetClipboardFormatName | 8 | 0 |
| 147 | SetClipboardViewer | 2 | 1 |
| 148 | GetClipboardViewer | 0 | 0 |
| 149 | ChangeClipboardChain | 4 | 1 |
| 170 | ArrangeIconicWindows | 2 | 1 |
| 172 | SwitchToThisWindow | 4 | 1 |
| 181 | SetSysColors | 10 | 1 |
| 182 | KillSystemTimer | 4 | 1 |
| 183 | GetCaretPos | 4 | 0 |
| 184 | QuerySendMessage | 0 | 0 |
| 185 | GrayString | 22 | 1 |
| 186 | SwapMouseButton | 2 | 1 |
| 187 | EndMenu | 0 | 1 |
| 188 | SetSysModalWindow | 2 | 1 |
| 189 | GetSysModalWindow | 0 | 0 |
| 191 | ChildWindowFromPoint | 6 | 1 |
| 193 | IsClipboardFormatAvailable | 2 | 1 |
| 194 | DlgDirSelectComboBox | 8 | 1 |
| 195 | DlgDirListComboBox | 12 | 1 |
| 196 | TabbedTextOut | 20 | 0 |
| 197 | GetTabbedTextExtent | 14 | 0 |
| 198 | CascadeChildWindows | 4 | 1 |
| 199 | TileChildWindows | 4 | 1 |
| 200 | OpenComm | 8 | 0 |
| 201 | SetCommState | 4 | 1 |
| 202 | GetCommState | 6 | 0 |
| 203 | GetCommError | 6 | 0 |
| 204 | ReadComm | 8 | 1 |
| 205 | WriteComm | 8 | 1 |
| 206 | TransmitCommChar | 4 | 1 |
| 207 | CloseComm | 2 | 1 |
| 208 | SetCommEventMask | 4 | 0 |
| 209 | GetCommEventMask | 4 | 0 |
| 210 | SetCommBreak | 2 | 1 |
| 211 | ClearCommBreak | 2 | 1 |
| 212 | UngetCommChar | 4 | 1 |
| 213 | BuildCommDCB | 8 | 1 |
| 214 | EscapeCommFunction | 4 | 0 |
| 215 | FlushComm | 4 | 1 |
| 216 | UserSeeUserDo | 8 | 0 |
| 217 | LookupMenuHandle | 4 | 1 |
| 220 | LoadMenuIndirect | 4 | 0 |
| 222 | GetKeyboardState | 4 | 0 |
| 223 | SetKeyboardState | 4 | 1 |
| 224 | GetWindowTask | 2 | 0 |
| 226 | LockInput | 0 | 1 |
| 227 | GetNextDlgGroupItem | 6 | 0 |
| 228 | GetNextDlgTabItem | 6 | 0 |
| 229 | GetTopWindow | 2 | 0 |
| 230 | GetNextWindow | 4 | 0 |
| 231 | GetSystemDebugState | 0 | 0 |
| 233 | SetParent | 4 | 1 |
| 234 | UnhookWindowsHook | 6 | 1 |
| 235 | DefHookProc | 12 | 0 |
| 236 | GetCapture | 0 | 0 |
| 237 | GetUpdateRgn | 6 | 0 |
| 238 | ExcludeUpdateRgn | 4 | 1 |
| 244 | EqualRect | 8 | 1 |
| 245 | EnableCommNotification | 8 | 1 |
| 246 | ExitWindowsExec | 8 | 1 |
| 247 | GetCursor | 0 | 0 |
| 248 | GetOpenClipboardWindow | 0 | 0 |
| 251 | SendDriverMessage | 12 | 0 |
| 252 | OpenDriver | 12 | 0 |
| 253 | CloseDriver | 10 | 0 |
| 254 | GetDriverModuleHandle | 2 | 0 |
| 255 | DefDriverProc | 16 | 0 |
| 256 | GetDriverInfo | 6 | 0 |
| 257 | GetNextDriver | 6 | 0 |
| 258 | MapWindowPoints | 10 | 1 |
| 259 | BeginDeferWindowPos | 2 | 1 |
| 260 | DeferWindowPos | 16 | 1 |
| 261 | EndDeferWindowPos | 2 | 1 |
| 265 | ShowOwnedPopups | 4 | 1 |
| 267 | ShowScrollBar | 6 | 1 |
| 269 | GlobalDeleteAtom | 2 | 1 |
| 270 | GlobalFindAtom | 4 | 1 |
| 271 | GlobalGetAtomName | 8 | 1 |
| 273 | ControlPanelInfo | 8 | 1 |
| 274 | GetNextQueueWindow | 0 | 0 |
| 275 | RepaintScreen | 0 | 1 |
| 276 | LockMyTask | 0 | 1 |
| 278 | GetDesktopHwnd | 0 | 0 |
| 279 | OldSetDeskPattern | 0 | 1 |
| 280 | SetSystemMenu | 4 | 1 |
| 281 | GetSysColorBrush | 2 | 0 |
| 285 | SetDeskWallpaper | 4 | 1 |
| 287 | GetLastActivePopup | 2 | 0 |
| 289 | keybd_event | 0 | 0 |
| 290 | RedrawWindow | 10 | 1 |
| 291 | SetWindowsHookEx | 10 | 0 |
| 292 | UnhookWindowsHookEx | 4 | 1 |
| 293 | CallNextHookEx | 12 | 0 |
| 294 | LockWindowUpdate | 2 | 1 |
| 299 | mouse_event | 0 | 0 |
| 300 | UnloadInstalledDrivers | 0 | 1 |
| 301 | EDITWNDPROC | 0 | 1 |
| 302 | STATICWNDPROC | 0 | 1 |
| 303 | BUTTONWNDPROC | 0 | 1 |
| 304 | SBWNDPROC | 0 | 1 |
| 305 | DESKTOPWNDPROC | 0 | 1 |
| 306 | MENUWNDPROC | 0 | 1 |
| 307 | LBOXCTLWNDPROC | 0 | 1 |
| 309 | GetClipCursor | 4 | 0 |
| 314 | SignalProc | 10 | 1 |
| 319 | ScrollWindowEx | 22 | 1 |
| 320 | SysErrorBox | 0 | 1 |
| 321 | SetEventHook | 4 | 0 |
| 322 | WinOldAppHackOMatic | 0 | 1 |
| 323 | GetMessage2 | 0 | 0 |
| 324 | FillWindow | 8 | 1 |
| 325 | PaintRect | 12 | 1 |
| 326 | GetControlBrush | 6 | 0 |
| 331 | EnableHardwareInput | 2 | 1 |
| 332 | UserYield | 0 | 1 |
| 333 | IsUserIdle | 0 | 1 |
| 336 | LoadCursorIconHandler | 6 | 0 |
| 337 | GetMouseEventProc | 0 | 0 |
| 338 | ECGETDS | 0 | 1 |
| 343 | GetFilePortName | 0 | 0 |
| 344 | COMBOBOXCTLWNDPROC | 0 | 1 |
| 345 | BEAR345 | 0 | 1 |
| 356 | LoadDIBCursorHandler | 6 | 0 |
| 357 | LoadDIBIconHandler | 6 | 0 |
| 359 | GetDCEx | 8 | 0 |
| 362 | DCHook | 12 | 1 |
| 364 | LookupIconIdFromDirectoryEx | 12 | 1 |
| 368 | CopyIcon | 4 | 0 |
| 369 | CopyCursor | 4 | 0 |
| 370 | GetWindowPlacement | 6 | 0 |
| 371 | SetWindowPlacement | 6 | 1 |
| 372 | GetInternalIconHeader | 0 | 0 |
| 373 | SubtractRect | 12 | 1 |
| 374 | DllEntryPoint | 16 | 1 |
| 375 | DrawTextEx | 0 | 1 |
| 376 | SetMessageExtraInfo | 0 | 1 |
| 378 | SetPropEx | 0 | 1 |
| 379 | GetPropEx | 0 | 0 |
| 380 | RemovePropEx | 0 | 1 |
| 382 | SetWindowContextHelpID | 0 | 1 |
| 383 | GetWindowContextHelpID | 0 | 0 |
| 384 | SetMenuContextHelpId | 4 | 1 |
| 385 | GetMenuContextHelpId | 2 | 0 |
| 389 | LoadImage | 14 | 0 |
| 390 | CopyImage | 10 | 0 |
| 391 | SignalProc32 | 14 | 1 |
| 394 | DrawIconEx | 18 | 1 |
| 395 | GetIconInfo | 6 | 0 |
| 397 | RegisterClassEx | 4 | 1 |
| 398 | GetClassInfoEx | 10 | 0 |
| 399 | ChildWindowFromPointEx | 8 | 1 |
| 400 | FinalUserInit | 0 | 1 |
| 402 | GetPriorityClipboardFormat | 6 | 0 |
| 406 | CreateCursor | 18 | 0 |
| 407 | CreateIcon | 18 | 0 |
| 408 | CreateCursorIconIndirect | 14 | 0 |
| 409 | InitThreadInput | 4 | 1 |
| 417 | GetMenuCheckMarkDimensions | 0 | 0 |
| 418 | SetMenuItemBitmaps | 10 | 1 |
| 422 | DlgDirSelectEx | 10 | 1 |
| 423 | DlgDirSelectComboBoxEx | 10 | 1 |
| 427 | FindWindowEx | 12 | 0 |
| 428 | TileWindows | 0 | 1 |
| 429 | CascadeWindows | 0 | 1 |
| 430 | lstrcmp | 8 | 1 |
| 432 | AnsiLower | 4 | 0 |
| 433 | IsCharAlpha | 2 | 1 |
| 434 | IsCharAlphaNumeric | 2 | 1 |
| 437 | AnsiUpperBuff | 6 | 1 |
| 438 | AnsiLowerBuff | 6 | 1 |
| 441 | InsertMenuItem | 10 | 1 |
| 443 | GetMenuItemInfo | 0 | 0 |
| 445 | DefFrameProc | 12 | 0 |
| 446 | SetMenuItemInfo | 0 | 1 |
| 447 | DefMDIChildProc | 10 | 0 |
| 448 | DrawAnimatedRects | 12 | 1 |
| 449 | DrawState | 24 | 1 |
| 450 | CreateIconFromResourceEx | 20 | 0 |
| 451 | TranslateMDISysAccel | 6 | 1 |
| 455 | GetIconID | 6 | 0 |
| 456 | LoadIconHandler | 4 | 0 |
| 458 | DestroyCursor | 2 | 1 |
| 459 | DumpIcon | 16 | 0 |
| 460 | GetInternalWindowPos | 10 | 0 |
| 461 | SetInternalWindowPos | 12 | 1 |
| 462 | CalcChildScroll | 4 | 1 |
| 463 | ScrollChildren | 10 | 1 |
| 464 | DragObject | 12 | 0 |
| 465 | DragDetect | 6 | 1 |
| 466 | DrawFocusRect | 6 | 1 |
| 470 | StringFunc | 0 | 1 |
| 475 | SetScrollInfo | 10 | 1 |
| 476 | GetScrollInfo | 8 | 0 |
| 477 | GetKeyboardLayoutName | 4 | 0 |
| 478 | LoadKeyboardLayout | 0 | 0 |
| 479 | MenuItemFromPoint | 0 | 1 |
| 480 | GetUserLocalObjType | 0 | 0 |
| 482 | EnableScrollBar | 6 | 1 |
| 483 | SystemParametersInfo | 10 | 1 |
| 489 | USER_489 | 0 | 1 |
| 490 | USER_490 | 0 | 1 |
| 492 | USER_492 | 0 | 1 |
| 496 | USER_496 | 0 | 1 |
| 498 | BEAR498 | 0 | 1 |
| 499 | WNetErrorText | 8 | 1 |
| 500 | FARCALLNETDRIVER | 0 | 1 |
| 501 | WNetOpenJob | 14 | 1 |
| 502 | WNetCloseJob | 10 | 1 |
| 503 | WNetAbortJob | 6 | 1 |
| 504 | WNetHoldJob | 6 | 1 |
| 505 | WNetReleaseJob | 6 | 1 |
| 506 | WNetCancelJob | 6 | 1 |
| 507 | WNetSetJobCopies | 8 | 1 |
| 508 | WNetWatchQueue | 12 | 1 |
| 509 | WNetUnwatchQueue | 4 | 1 |
| 510 | WNetLockQueueData | 12 | 1 |
| 511 | WNetUnlockQueueData | 4 | 1 |
| 514 | WNetDeviceMode | 2 | 1 |
| 515 | WNetBrowseDialog | 8 | 1 |
| 516 | WNetGetUser | 8 | 1 |
| 517 | WNetAddConnection | 12 | 1 |
| 518 | WNetCancelConnection | 6 | 1 |
| 519 | WNetGetError | 4 | 1 |
| 520 | WNetGetErrorText | 10 | 1 |
| 521 | WNetEnable | 0 | 1 |
| 522 | WNetDisable | 0 | 1 |
| 523 | WNetRestoreConnection | 6 | 1 |
| 524 | WNetWriteJob | 10 | 1 |
| 525 | WNetConnectDialog | 4 | 1 |
| 526 | WNetDisconnectDialog | 4 | 1 |
| 527 | WNetConnectionDialog | 4 | 1 |
| 528 | WNetViewQueueDialog | 6 | 1 |
| 529 | WNetPropertyDialog | 12 | 1 |
| 530 | WNetGetDirectoryType | 8 | 1 |
| 531 | WNetDirectoryNotify | 8 | 1 |
| 532 | WNetGetPropertyText | 16 | 1 |
| 533 | WNetInitialize | 0 | 1 |
| 534 | WNetLogon | 0 | 1 |
| 535 | WOWWORDBREAKPROC | 0 | 1 |
| 537 | MOUSEEVENT | 0 | 1 |
| 538 | KEYBDEVENT | 0 | 1 |
| 595 | OLDEXITWINDOWS | 0 | 1 |
| 600 | GetShellWindow | 0 | 0 |
| 601 | DoHotkeyStuff | 0 | 1 |
| 602 | SetCheckCursorTimer | 0 | 1 |
| 604 | BroadcastSystemMessage | 0 | 1 |
| 605 | HackTaskMonitor | 0 | 1 |
| 606 | FormatMessage | 22 | 1 |
| 608 | GetForegroundWindow | 0 | 0 |
| 609 | SetForegroundWindow | 2 | 1 |
| 610 | DestroyIcon32 | 4 | 1 |
| 620 | ChangeDisplaySettings | 8 | 0 |
| 621 | EnumDisplaySettings | 12 | 0 |
| 640 | MsgWaitForMultipleObjects | 20 | 0 |
| 650 | ActivateKeyboardLayout | 0 | 1 |
| 651 | GetKeyboardLayout | 0 | 0 |
| 652 | GetKeyboardLayoutList | 0 | 0 |
| 654 | UnloadKeyboardLayout | 0 | 1 |
| 655 | PostPostedMessages | 0 | 1 |
| 656 | DrawFrameControl | 10 | 1 |
| 657 | DrawCaptionTemp | 18 | 1 |
| 658 | DispatchInput | 0 | 1 |
| 659 | DrawEdge | 10 | 1 |
| 660 | DrawCaption | 10 | 1 |
| 661 | SetSysColorsTemp | 0 | 1 |
| 662 | DrawMenubarTemp | 0 | 1 |
| 663 | GetMenuDefaultItem | 0 | 0 |
| 664 | SetMenuDefaultItem | 0 | 1 |
| 665 | GetMenuItemRect | 10 | 0 |
| 666 | CheckMenuRadioItem | 10 | 1 |
| 667 | TrackPopupMenuEx | 0 | 1 |
| 668 | SetWindowRgn | 6 | 1 |
| 669 | GetWindowRgn | 0 | 0 |
| 800 | CHOOSEFONT_CALLBACK16 | 0 | 1 |
| 801 | FINDREPLACE_CALLBACK16 | 0 | 1 |
| 802 | OPENFILENAME_CALLBACK16 | 0 | 1 |
| 803 | PRINTDLG_CALLBACK16 | 0 | 1 |
| 804 | CHOOSECOLOR_CALLBACK16 | 0 | 1 |
| 819 | PeekMessage32 | 14 | 1 |
| 820 | GetMessage32 | 12 | 0 |
| 821 | TranslateMessage32 | 6 | 1 |
| 822 | DispatchMessage32 | 6 | 0 |
| 823 | CallMsgFilter32 | 8 | 1 |
| 825 | PostMessage32 | 0 | 1 |
| 826 | PostThreadMessage32 | 0 | 1 |
| 827 | MessageBoxIndirect | 4 | 1 |
| 851 | MsgThkConnectionDataLS | 0 | 1 |
| 853 | FT_USRFTHKTHKCONNECTIONDATA | 0 | 1 |
| 854 | FT__USRF2THKTHKCONNECTIONDATA | 0 | 1 |
| 855 | Usr32ThkConnectionDataSL | 0 | 1 |
| 890 | InstallIMT | 0 | 1 |
| 891 | UninstallIMT | 0 | 1 |
| 902 | LoadSystemLanguageString | 12 | 0 |
| 905 | ChangeDialogTemplate | 0 | 1 |
| 906 | GetNumLanguages | 0 | 0 |
| 907 | GetLanguageName | 10 | 0 |
| 909 | SetWindowTextEx | 8 | 1 |
| 910 | BiDiMessageBoxEx | 0 | 1 |
| 911 | SetDlgItemTextEx | 10 | 1 |
| 912 | ChangeKeyboardLanguage | 4 | 0 |
| 913 | GetCodePageSystemFont | 4 | 0 |
| 914 | QueryCodePage | 10 | 0 |
| 915 | GetAppCodePage | 2 | 0 |
| 916 | CreateDialogIndirectParamML | 26 | 0 |
| 918 | DialogBoxIndirectParamML | 24 | 1 |
| 919 | LoadLanguageString | 12 | 0 |
| 920 | SetAppCodePage | 8 | 0 |
| 922 | GetBaseCodePage | 0 | 0 |
| 923 | FindLanguageResource | 12 | 0 |
| 924 | ChangeKeyboardCodePage | 4 | 0 |
| 930 | MessageBoxEx | 14 | 1 |
| 1000 | SetProcessDefaultLayout | 4 | 1 |
| 1001 | GetProcessDefaultLayout | 4 | 0 |
| 1010 | __wine_call_wndproc | 14 | 0 |

