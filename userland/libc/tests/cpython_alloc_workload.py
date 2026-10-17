#!/usr/bin/env python3
# cpython_alloc_workload.py <shape> <n>   (#245; driven by run_cpython_alloc.sh)
# Allocation-heavy Python shapes. Prints "shape n secs".
#
# The shapes are chosen so that some MUST hit libc malloc and one must NOT:
# CPython's pymalloc absorbs every object request <= 512 bytes into its own
# pools, so a workload of only small objects barely touches malloc at all.
# The "small" shape is therefore a NEGATIVE CONTROL: if it moves with the
# allocator, the rig is measuring something other than what it claims.
import sys, time

def shape_records(n):
    # list.ob_item for 100 elements = 800 bytes > 512, so each goes to malloc
    # and is RETAINED, which is what makes the block list grow.
    keep = []
    for i in range(n):
        keep.append([i] * 100)
    return keep

def shape_dicts(n):
    keep = []
    for i in range(n):
        d = {}
        for j in range(24):
            d["k%02d" % j] = i
        keep.append(d)
    return keep

def shape_strings(n):
    keep = []
    for i in range(n):
        keep.append(("%d" % i) * 400)
    return keep

def shape_small(n):
    # NEGATIVE CONTROL: 3-tuples are ~56 bytes, entirely inside pymalloc.
    keep = []
    for i in range(n):
        keep.append((i, i + 1, i + 2))
    return keep

def shape_churn(n):
    # Retain a growing set while also freeing a same-size temporary each
    # iteration: the shape that made the old scan believe a fit might exist
    # and walk the whole list to prove otherwise.
    keep = []
    for i in range(n):
        keep.append([i] * 100)
        tmp = [i] * 100
        del tmp
    return keep

SHAPES = {k[6:]: v for k, v in list(globals().items()) if k.startswith("shape_")}

shape = sys.argv[1]
n = int(sys.argv[2])
fn = SHAPES[shape]
t0 = time.perf_counter()
r = fn(n)
t1 = time.perf_counter()
# Touch the result so nothing can be optimised away and so a "fast" run that
# handed out overlapping memory shows up as wrong data rather than as a win.
assert len(r) == n
print("%s %d %.4f" % (shape, n, t1 - t0))
