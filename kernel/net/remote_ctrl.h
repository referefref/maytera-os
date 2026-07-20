// remote_ctrl.h - Kernel-mode TCP remote control service for MayteraOS
// Listens on TCP port 2323, accepts connections and dispatches shell commands.
//
// ROADMAP: Future user-mode version will run as a userland process using
// the sys_tcp_* syscalls, allowing remote control without ring-0 privileges.
// The protocol (newline-delimited commands, text responses) will remain identical
// so existing clients work unmodified.
#ifndef REMOTE_CTRL_H
#define REMOTE_CTRL_H

// TCP port the remote shell listens on
#define REMOTE_CTRL_PORT    2323

// Auth credentials - change before deployment
#define REMOTE_CTRL_USER    "admin"
#define REMOTE_CTRL_PASS    "maytera"
#define REMOTE_CTRL_MAX_ATTEMPTS  3

// Initialize and start the remote control kernel thread
void remote_ctrl_init(void);

#endif // REMOTE_CTRL_H
