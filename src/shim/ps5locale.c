// ps5locale.c: the BSD "xlocale" *_l functions that libc++'s iostream/locale code (locale.cpp) calls and the console's libc does not export.
// The shaderc / glslang / SPIRV-Tools objects (build_spirv.sh) use std::ostringstream / std::locale, which pulls libc++'s locale.cpp into the link; HotSpot never did.
// Everything runs in the "C" locale: the locale_t argument is ignored and the plain function does the work. All functions are weak so a real definition elsewhere (the HotSpot port's
// ps5_support.cpp, a newer SDK libc) wins without a duplicate-symbol error.
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <runetype.h>
#include <xlocale.h>
#include <nl_types.h>

#define W __attribute__((weak))
static char ps5_c_locale[16];   // the one locale object: "C"

W locale_t newlocale(int mask, const char *name, locale_t base) {
    (void)mask; (void)base;
    if (!name || !*name || !strcmp(name, "C") || !strcmp(name, "POSIX")) return (locale_t)ps5_c_locale;
    errno = ENOENT; return 0;                                   // std::locale("xx_XX") then throws runtime_error, as on a system without that locale
}
W int freelocale(locale_t l) { (void)l; return 0; }

W _RuneLocale *__runes_for_locale(locale_t l, int *limit) { (void)l; if (limit) *limit = 256; return (_RuneLocale *)&_DefaultRuneLocale; }
W unsigned long ___runetype_l(int c, locale_t l) { (void)l; return (c >= 0 && c < 256) ? _DefaultRuneLocale.__runetype[c] : 0; }
W int ___tolower_l(int c, locale_t l) { (void)l; return (c >= 0 && c < 256) ? _DefaultRuneLocale.__maplower[c] : c; }
W int ___toupper_l(int c, locale_t l) { (void)l; return (c >= 0 && c < 256) ? _DefaultRuneLocale.__mapupper[c] : c; }
W int ___mb_cur_max_l(locale_t l) { (void)l; return 1; }
// the non-_l ctype helpers behind the inline __maskrune / __tolower / __toupper of <ctype.h> (not exported by the console libc either)
W unsigned long ___runetype(int c) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.__runetype[c] : 0; }
W int ___tolower(int c) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.__maplower[c] : c; }
W int ___toupper(int c) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.__mapupper[c] : c; }
W const _RuneLocale *__getCurrentRuneLocale(void) { return &_DefaultRuneLocale; }

W long long strtoll_l(const char *s, char **e, int b, locale_t l) { (void)l; return strtoll(s, e, b); }
W unsigned long long strtoull_l(const char *s, char **e, int b, locale_t l) { (void)l; return strtoull(s, e, b); }
W float strtof_l(const char *s, char **e, locale_t l) { (void)l; return strtof(s, e); }
W double strtod_l(const char *s, char **e, locale_t l) { (void)l; return strtod(s, e); }
W long double strtold_l(const char *s, char **e, locale_t l) { (void)l; return strtold(s, e); }
W int strcoll_l(const char *a, const char *b, locale_t l) { (void)l; return strcoll(a, b); }
W size_t strxfrm_l(char *d, const char *s, size_t n, locale_t l) { (void)l; return strxfrm(d, s, n); }
W int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l) { (void)l; return wcscoll(a, b); }
W size_t wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, locale_t l) { (void)l; return wcsxfrm(d, s, n); }
W size_t strftime_l(char *s, size_t m, const char *f, const struct tm *t, locale_t l) { (void)l; return strftime(s, m, f, t); }
W int iswctype_l(wint_t c, wctype_t t, locale_t l) { (void)l; return iswctype(c, t); }
W wint_t btowc_l(int c, locale_t l) { (void)l; return btowc(c); }
W int wctob_l(wint_t c, locale_t l) { (void)l; return wctob(c); }
W int mbtowc_l(wchar_t *w, const char *s, size_t n, locale_t l) { (void)l; return mbtowc(w, s, n); }
W size_t mbrlen_l(const char *s, size_t n, mbstate_t *ps, locale_t l) { (void)l; return mbrlen(s, n, ps); }
W size_t mbrtowc_l(wchar_t *w, const char *s, size_t n, mbstate_t *ps, locale_t l) { (void)l; return mbrtowc(w, s, n, ps); }
W size_t wcrtomb_l(char *s, wchar_t w, mbstate_t *ps, locale_t l) { (void)l; return wcrtomb(s, w, ps); }
W size_t mbsrtowcs_l(wchar_t *d, const char **src, size_t len, mbstate_t *ps, locale_t l) { (void)l; return mbsrtowcs(d, src, len, ps); }
W struct lconv *localeconv_l(locale_t l) { (void)l; return localeconv(); }

// the "n" variants (convert at most nms source units) are not exported by the console libc: loop over mbrtowc / wcrtomb
W size_t mbsnrtowcs_l(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps, locale_t l) {
    (void)l; const char *s = *src; size_t n = 0;
    while (nms > 0 && (!dst || n < len)) {
        wchar_t w; size_t r = mbrtowc(&w, s, nms, ps);
        if (r == (size_t)-1) { *src = s; return (size_t)-1; }
        if (r == (size_t)-2) break;
        if (r == 0) { if (dst) { dst[n] = 0; *src = 0; } return n; }
        if (dst) dst[n] = w;
        n++; s += r; nms -= r;
    }
    if (dst) *src = s;
    return n;
}
W size_t wcsnrtombs_l(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps, locale_t l) {
    (void)l; const wchar_t *s = *src; size_t n = 0; char buf[8];
    while (nwc > 0) {
        size_t r = wcrtomb(buf, *s, ps);
        if (r == (size_t)-1) { *src = s; return (size_t)-1; }
        if (dst && n + r > len) break;
        if (*s == 0) { if (dst) { memcpy(dst + n, buf, r); *src = 0; } return n + r - 1; }
        if (dst) memcpy(dst + n, buf, r);
        n += r; s++; nwc--;
    }
    if (dst) *src = s;
    return n;
}

// printf/scanf family with a locale argument (libc++ formats every number with snprintf_l / asprintf_l and parses pointers with sscanf_l)
W int snprintf_l(char *s, size_t n, locale_t l, const char *f, ...) { (void)l; va_list a; va_start(a, f); int r = vsnprintf(s, n, f, a); va_end(a); return r; }
W int asprintf_l(char **s, locale_t l, const char *f, ...) { (void)l; va_list a; va_start(a, f); int r = vasprintf(s, f, a); va_end(a); return r; }
W int sscanf_l(const char *s, locale_t l, const char *f, ...) { (void)l; va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }

// message catalogs (std::messages): none exist
W nl_catd catopen(const char *name, int oflag) { (void)name; (void)oflag; return (nl_catd)-1; }
W char *catgets(nl_catd c, int set, int num, const char *s) { (void)c; (void)set; (void)num; return (char *)s; }
W int catclose(nl_catd c) { (void)c; return 0; }

// libc++ filesystem (last_write_time) references it; shaderc never sets file times
#include <sys/stat.h>
W int utimensat(int fd, const char *path, const struct timespec t[2], int flag) { (void)fd; (void)path; (void)t; (void)flag; errno = ENOSYS; return -1; }
