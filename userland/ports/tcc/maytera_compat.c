/* maytera_compat.c - the one libc symbol tcc 0.9.27 needs that the MayteraOS
 * freestanding libc does not define.
 *
 * WHAT THIS IS. Exactly one symbol: strtold. tcc's preprocessor (tccpp.c,
 * parse_number) calls strtold() to parse long-double floating constants. The
 * MayteraOS libc has strtod but not strtold, so a link of tcc without this
 * fails with the single undefined reference `strtold' and nothing else
 * (MEASURED: this file is the ONLY thing standing between a clean libtcc.a
 * and the link).
 *
 * WHY WEAK, and why not extend the shared libc. It is WEAK so that if the
 * shared libc ever grows a real strtold, the real one wins and this shim
 * silently steps aside; and it is scoped to this archive so no other app that
 * links libc is affected. This is the same one-symbol-compat-shim pattern the
 * quickjs port uses for pthread_condattr_setclock: owner rule 1's "do not fork
 * a shared primitive" does not apply, because this is not a reimplementation of
 * a shared primitive, it is a missing leaf the shared libc simply lacks.
 *
 * KNOWN LIMITATION, stated rather than hidden. On x86_64 `long double` is the
 * 80-bit extended type, but this shim parses via strtod(), i.e. at 64-bit
 * double precision, then widens. A long-double literal in the program tcc is
 * compiling therefore keeps only ~15-16 significant digits, not ~18-19. This
 * affects ONLY the last few digits of an explicit `long double` constant in
 * source tcc compiles; it does not affect tcc's own operation, ordinary double
 * or float constants, or any integer path. A real 80-bit strtold belongs in
 * the shared libc if a program ever needs full long-double literal precision;
 * that is a libc change, not a tcc-port change, which is exactly why this is a
 * WEAK shim and not a private fork.
 */

extern double strtod(const char *nptr, char **endptr);

#pragma weak strtold
long double strtold(const char *nptr, char **endptr)
{
    return (long double)strtod(nptr, endptr);
}
