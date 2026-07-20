// #71 hdadbg - userland HDA audio bring-up debug tool.
// Drives the HDA output path over the mdev bridge via SYS_HDA_DBG so the whole
// #71 diagnosis/fix can iterate from Ring 3 with NO kernel reburn per attempt.
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"

static long op(int o, long a, long b, long c) { return sys_hda_dbg(o, a, b, c); }

// dec/hex parser (accepts 0x..)
static long P(const char *s) {
    if (!s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;;) {
            char ch = *s; int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else break;
            v = v * 16 + d; s++;
        }
    } else {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    }
    return neg ? -v : v;
}

static void spin(long n) { for (volatile long d = 0; d < n; d++) { } }

static int run_play(long period, long loops);

int main(int argc, char *argv[]) {
    // No args (the mdev-bridge `run` path ignores argv, and autostart passes
    // none) => run the decisive self-test: load a tone, start the output DMA,
    // and sample LPIB to prove whether the output stream actually advances.
    if (argc < 2) {
        long v = op(10, 0, 0, 0);
        printf("hdadbg self-test: info=0x%lx (out_stream=%ld cad=%ld fg_nid=%ld init=%ld)\n",
               v, (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 1);
        printf("GPIO mask=0x%lx dir=0x%lx data=0x%lx\n",
               op(5,0,0,0) & 0xff, op(5,1,0,0) & 0xff, op(5,2,0,0) & 0xff);
        return run_play(109, 8);
    }
    if (!strcmp(argv[1], "help")) {
        printf("hdadbg (#71 HDA audio debug)\n");
        printf("  info                    controller/codec/init state\n");
        printf("  verb <cad> <nid> <verb> send codec verb -> response\n");
        printf("  ctl | sts | lpib        read out-stream SDnCTL/STS/LPIB\n");
        printf("  run <0|1>               stop/start output DMA\n");
        printf("  gpio                    dump codec GPIO mask/dir/data\n");
        printf("  gpioset <mask> <dir> <data>\n");
        printf("  tone <period_frames>    fill square wave (0=silence)\n");
        printf("  reg <off> [val]         raw BAR read/write32\n");
        printf("  play [period] [loops]   tone+run, sample LPIB (does DMA advance?)\n");
        return 0;
    }
    const char *c = argv[1];

    if (!strcmp(c, "info")) {
        long v = op(10, 0, 0, 0);
        printf("info=0x%lx  out_stream=%ld cad=%ld fg_nid=%ld initialized=%ld\n",
               v, (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 1);
        return 0;
    }
    if (!strcmp(c, "verb") && argc >= 5) {
        long r = op(0, P(argv[2]), P(argv[3]), P(argv[4]));
        printf("verb -> 0x%lx\n", r & 0xffffffffL);
        return 0;
    }
    if (!strcmp(c, "ctl"))  { printf("SDnCTL=0x%lx\n",  op(1,0,0,0)); return 0; }
    if (!strcmp(c, "sts"))  { printf("SDnSTS=0x%lx\n",  op(2,0,0,0)); return 0; }
    if (!strcmp(c, "lpib")) { printf("LPIB=%ld\n",      op(3,0,0,0)); return 0; }
    if (!strcmp(c, "run") && argc >= 3) {
        printf("run=%ld -> SDnCTL=0x%lx\n", P(argv[2]), op(4, P(argv[2]), 0, 0));
        return 0;
    }
    if (!strcmp(c, "gpio")) {
        printf("GPIO mask=0x%lx dir=0x%lx data=0x%lx\n",
               op(5,0,0,0) & 0xff, op(5,1,0,0) & 0xff, op(5,2,0,0) & 0xff);
        return 0;
    }
    if (!strcmp(c, "gpioset") && argc >= 5) {
        op(6, P(argv[2]), P(argv[3]), P(argv[4]));
        printf("gpioset done; now mask=0x%lx dir=0x%lx data=0x%lx\n",
               op(5,0,0,0) & 0xff, op(5,1,0,0) & 0xff, op(5,2,0,0) & 0xff);
        return 0;
    }
    if (!strcmp(c, "tone") && argc >= 3) {
        printf("tone: %ld frames filled\n", op(7, P(argv[2]), 0, 0));
        return 0;
    }
    if (!strcmp(c, "reg") && argc >= 3) {
        if (argc >= 4) { op(9, P(argv[2]), P(argv[3]), 0); printf("wrote 0x%lx\n", P(argv[3])); }
        else printf("reg[0x%lx]=0x%lx\n", P(argv[2]), op(8, P(argv[2]), 0, 0) & 0xffffffffL);
        return 0;
    }
    if (!strcmp(c, "play")) {
        long period = argc > 2 ? P(argv[2]) : 109;   // ~440Hz @48k
        long loops  = argc > 3 ? P(argv[3]) : 8;
        return run_play(period, loops);
    }
    printf("hdadbg: unknown command '%s' (try 'hdadbg help')\n", c);
    return 1;
}

// Load a tone, start the output DMA, and sample LPIB across `loops` reads to
// prove whether the output stream actually advances (the crux of #71).
static int run_play(long period, long loops) {
    printf("play: tone period=%ld -> %ld frames\n", period, op(7, period, 0, 0));
    printf("run=1 -> SDnCTL=0x%lx\n", op(4, 1, 0, 0));
    long prev = -1; int moved = 0;
    for (long i = 0; i < loops; i++) {
        spin(30000000L);
        long lp = op(3, 0, 0, 0);
        printf("  LPIB[%ld]=%ld\n", i, lp);
        if (prev >= 0 && lp != prev) moved = 1;
        prev = lp;
    }
    long sts = op(2, 0, 0, 0);
    op(4, 0, 0, 0);   // stop
    printf("RESULT: output DMA %s (LPIB %s), SDnSTS=0x%lx\n",
           moved ? "RUNS" : "STALLED", moved ? "advanced" : "stuck", sts);
    return 0;
}
