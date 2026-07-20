// alloca.h stub - use doom_malloc instead of stack allocation
#ifndef DOOM_ALLOCA_H
#define DOOM_ALLOCA_H

#define alloca(size) doom_malloc(size)

#endif
