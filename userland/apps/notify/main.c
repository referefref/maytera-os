// notify - command-line/test producer for the MayteraOS notifications subsystem
// (#168). Posts notifications via libc notify_post(); the compositor shows them
// as toasts and logs them in the tray-bell notification center.
//   notify <sev 0-3> "title" "body"   - post one notification
//   notify "title" "body"             - post one info notification
//   notify                            - post a demo set (one of each severity)
#include "../../libc/notify.h"
static int atoi_(const char *s){ int v=0,n=0; if(s[0]=='-'){n=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return n?-v:v; }
int main(int argc, char **argv) {
    if (argc >= 4) {
        notify_post(argv[2], argv[3], atoi_(argv[1]));
    } else if (argc >= 3) {
        notify_post(argv[1], argv[2], NOTIFY_INFO);
    } else {
        notify_post("Welcome", "MayteraOS notifications are live", NOTIFY_INFO);
        notify_post("Update available", "Version 1.31 is ready to install", NOTIFY_SUCCESS);
        notify_post("Low disk space", "Boot volume is 92% full", NOTIFY_WARNING);
        notify_post("Network error", "Could not reach update server", NOTIFY_ERROR);
    }
    return 0;
}
