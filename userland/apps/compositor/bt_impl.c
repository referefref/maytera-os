// bt_impl.c - the compositor's single translation unit that emits the (#372)
// Bluetooth mock definitions. taskbar.c and traymenu.c include bt_client.h for
// declarations only; this TU owns the state. When the SYS_BT_* stack lands the
// mock bodies in bt_client.h become one-line syscall wrappers and this file
// still compiles unchanged.
#define BT_MOCK_IMPL
#include "../../libc/bt_client.h"
#define WIFI_MOCK_IMPL
#include "../../libc/wifi_client.h"
