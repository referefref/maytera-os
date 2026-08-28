// bt_impl.c - the compositor's single translation unit that emits the (#237,
// formerly #372) Bluetooth/Wi-Fi stub definitions. taskbar.c and traymenu.c
// include bt_client.h/wifi_client.h for declarations only; this TU owns the
// (now stateless) bodies. When the SYS_BT_*/SYS_WIFI_* syscall groups land,
// the stub bodies become one-line syscall wrappers and this file still
// compiles unchanged.
#define BT_STUB_IMPL
#include "../../libc/bt_client.h"
#define WIFI_STUB_IMPL
#include "../../libc/wifi_client.h"
