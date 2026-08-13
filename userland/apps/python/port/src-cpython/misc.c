/* misc.c - #359 Phase 2 trimmed. gettimeofday/clock_gettime/clock_getres/
   nanosleep now come from the shared libc (posixextra.c, sub-second via the
   monotonic ms tick). This supplement keeps setlocale/localeconv and the itimer/
   tz stubs that libc does not provide. */
#include <locale.h>
#include <time.h>
#include <sys/time.h>
/* struct timespec now comes from <time.h> (shared libc). */
static struct lconv c_lconv;
static char *e_(){return (char*)"";}
char *setlocale(int cat,const char*loc){(void)cat;(void)loc;return (char*)"C";}
struct lconv *localeconv(void){
  char *E=e_(); char *D=(char*)".";
  c_lconv.decimal_point=D; c_lconv.thousands_sep=E; c_lconv.grouping=E;
  c_lconv.int_curr_symbol=E; c_lconv.currency_symbol=E;
  c_lconv.mon_decimal_point=E; c_lconv.mon_thousands_sep=E;
  c_lconv.mon_grouping=E; c_lconv.positive_sign=E; c_lconv.negative_sign=E;
  c_lconv.int_frac_digits=127; c_lconv.frac_digits=127; c_lconv.p_cs_precedes=127;
  c_lconv.p_sep_by_space=127; c_lconv.n_cs_precedes=127; c_lconv.n_sep_by_space=127;
  c_lconv.p_sign_posn=127; c_lconv.n_sign_posn=127;
  return &c_lconv;
}
int settimeofday(const struct timeval*tv,const struct timezone*tz){(void)tv;(void)tz;return 0;}
int clock_settime(clockid_t id, const struct timespec*ts){(void)id;(void)ts;return 0;}
int setitimer(int w,const struct itimerval*n,struct itimerval*o){(void)w;(void)n;(void)o;return 0;}
int getitimer(int w,struct itimerval*o){(void)w;(void)o;return 0;}
char *tzname[2]={(char*)"UTC",(char*)"UTC"};
long timezone=0;
int daylight=0;
void tzset(void){}
