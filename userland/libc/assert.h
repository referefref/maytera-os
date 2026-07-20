// assert.h - runtime assertions for MayteraOS userland (#422).
// Previously a no-op ((void)0); now a real assert that reports and aborts.
#ifndef _MAYTERA_ASSERT_H
#define _MAYTERA_ASSERT_H

void __assert_fail(const char *expr, const char *file, int line, const char *func);

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif

#endif // _MAYTERA_ASSERT_H
