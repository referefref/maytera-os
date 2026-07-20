// login.h - Login screen for MayteraOS
#ifndef LOGIN_H
#define LOGIN_H

#include "../types.h"

// Login result
typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[32];
    char home[64];
} login_result_t;

// Initialize the login screen
void login_init(void);

// Run the login screen (blocks until successful login)
// Returns 0 on success, fills result with authenticated user info
int login_run(login_result_t *result);

// Show the lock screen (blocks until re-authenticated)
// uid = the currently logged-in user
int login_lock_screen(uint32_t uid);

// Check if auto-login is configured
// Returns 1 if auto-login is enabled, fills result with auto-login user
int login_check_autologin(login_result_t *result);

#endif // LOGIN_H
