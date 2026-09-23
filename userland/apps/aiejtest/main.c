// AIEJTEST - #708 verifier for the FIRST real AI DEVICE executor: safe-eject.
// Modelled on ESCROWT: a Ring-3 app, launched at boot by the gated
// proc/aieject_vmtest.c launcher (marker /CONFIG/AIEJECT.TEST), whose verdict
// goes to stdout (serial via /dev/console) AND to /BOOTLOG.TXT via sys_bootlog,
// so a headless VM can capture it either way.
//
// It exercises the SHIPPED aiclient_run_action() device path against a REAL
// removable USB volume on the REAL kernel (SYS_VOL_LIST / SYS_VOL_BUSY /
// SYS_VOL_EJECT). It proves, in one run:
//   (1) device.list enumerates the removable stick (never the boot disk);
//   (2) a FORBIDden verb (format) is refused by the per-device manifest;
//   (3) the boot/system disk is never a target (eject "/" -> not found);
//   (4) a BUSY volume (a file left open on it) is refused cleanly, NOT forced;
//   (5) the safe-eject flushes + unmounts + stops it (status: ejected);
//   (6) the ejected volume is gone from the mount table.
// Data-intact is checked host-side after the run by mounting the stick image.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"
#include "../../libc/aiclient.h"

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static void line(const char *label, int ok) {
    char l[256]; snprintf(l, sizeof(l), "AIEJT: %s %s\n", label, ok ? "PASS" : "FAIL"); say(l);
}
// Extract the first "mount":"/USBx" value out of a device.list observation.
static int first_mount(const char *obs, char *out, int ocap) {
    const char *k = strstr(obs, "\"mount\":\"");
    if (!k) return 0;
    k += 9; int o = 0;
    while (*k && *k != '"' && o < ocap - 1) out[o++] = *k++;
    out[o] = 0;
    return o > 0;
}

static char obs[4096];

int main(void) {
    say("\n========== AIEJTEST (#708 AI safe-eject executor) ==========\n");

    // Make eject headless-ALLOW and format FORBID for this test via the supported
    // per-device override file (default policy is eject=CONSENT; the override
    // exists exactly so a deployment can tune per-device policy without a rebuild).
    int fd = sys_open("/CONFIG/AIDEVCAP.CFG", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { const char *c = "block|*|a|c|f|c|c|f|f|a|c\n"; sys_write(fd, c, strlen(c)); sys_close(fd); }

    // 1. discover the removable volume through the real SYS_VOL_LIST. The stick
    // is HOT-PLUGGED shortly after boot (QEMU device_add), which is the realistic
    // path the feature exists for (a device arriving after the desktop is up), so
    // poll device.list until it appears (up to ~45s).
    char mp[64]; int found = 0;
    for (int tries = 0; tries < 45; tries++) {
        aiclient_run_action("device.list", "{}", obs, sizeof obs);
        if (first_mount(obs, mp, sizeof mp)) { found = 1; break; }
        sleep(1);
    }
    { char l[4200]; snprintf(l, sizeof l, "AIEJT: device.list -> %s\n", obs); say(l); }
    if (!found) {
        say("AIEJT: NO-REMOVABLE (no stick enumerated after 45s); running not-found path only\n");
        aiclient_run_action("device.block.eject", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
        line("notfound-refuse", strstr(obs, "device-not-found") || strstr(obs, "no-removable-volumes"));
        say("========== AIEJTEST DONE ==========\n");
        return 0;
    }
    { char l[128]; snprintf(l, sizeof l, "AIEJT: removable volume = %s\n", mp); say(l); }

    char args[128];
    snprintf(args, sizeof args, "{\"mount\":\"%s\"}", mp);

    // 2. FORBIDDEN VERB: format refused by the manifest, no action runs.
    aiclient_run_action("device.block.format", args, obs, sizeof obs);
    { char l[4200]; snprintf(l, sizeof l, "AIEJT: format obs -> %s\n", obs); say(l); }
    line("forbidden-format-refused", strstr(obs, "device-denied") != 0);

    // 3. BOOT / SYSTEM DISK never a target.
    aiclient_run_action("device.block.eject", "{\"mount\":\"/\"}", obs, sizeof obs);
    line("bootdisk-never-target", strstr(obs, "device-not-found") != 0);

    // 4. BUSY refuse: hold a file open on the stick; eject must refuse (not force).
    char probe[96]; snprintf(probe, sizeof probe, "%s/BUSYPROBE.TXT", mp);
    int bfd = sys_open(probe, O_WRONLY | O_CREAT | O_TRUNC);
    if (bfd >= 0) { const char *d = "busy-probe intact data 2026\n"; sys_write(bfd, d, strlen(d)); }
    aiclient_run_action("device.block.eject", args, obs, sizeof obs);
    { char l[4200]; snprintf(l, sizeof l, "AIEJT: busy obs -> %s\n", obs); say(l); }
    line("busy-refuse-not-forced", strstr(obs, "device-busy") != 0);
    if (bfd >= 0) sys_close(bfd);   // now quiescent

    // 5. EJECT: flush + unmount + stop.
    aiclient_run_action("device.block.eject", args, obs, sizeof obs);
    { char l[4200]; snprintf(l, sizeof l, "AIEJT: eject obs -> %s\n", obs); say(l); }
    line("eject-flush-unmount-stop", strstr(obs, "\"status\":\"ejected\"") != 0);

    // 6. gone from the mount table.
    aiclient_run_action("device.list", "{}", obs, sizeof obs);
    { char l[4200]; snprintf(l, sizeof l, "AIEJT: post-eject device.list -> %s\n", obs); say(l); }
    line("gone-from-mount-table", strstr(obs, mp) == 0);

    say("========== AIEJTEST DONE ==========\n");
    return 0;
}
