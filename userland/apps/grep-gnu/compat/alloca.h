/* alloca.h - compat for MayteraOS (gcc builtin). */
#ifndef COMPAT_ALLOCA_H
#define COMPAT_ALLOCA_H

#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif

#endif /* COMPAT_ALLOCA_H */
