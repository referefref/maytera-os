// net_debug.h - Network debug logging control
#ifndef NET_DEBUG_H
#define NET_DEBUG_H

// Set to 1 to enable network debug logging, 0 to disable
#define NET_DEBUG_ENABLED 0

#if NET_DEBUG_ENABLED
  #define NET_LOG(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#else
  #define NET_LOG(fmt, ...) do { } while(0)
#endif

#endif // NET_DEBUG_H
