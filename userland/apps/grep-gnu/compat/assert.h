/* assert.h - compat for MayteraOS. grep/dfa/regex use assert() as sanity
 * checks; we compile them out (release build, like -DNDEBUG). */
#ifndef COMPAT_ASSERT_H
#define COMPAT_ASSERT_H

#define assert(expr) ((void)0)

#endif /* COMPAT_ASSERT_H */
