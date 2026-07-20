// mstring.h - MayteraOS malloc-backed replacement for std::string.
//
// Why this exists: linking libstdc++'s std::basic_string drags in locale,
// iostream static init and exception machinery, none of which exist on a
// freestanding -nostdlib userland (see CURAENGINE_PORT_PLAN.md section 3.4).
// CuraEngine 15.04 uses only a small slice of the std::string surface, so we
// vendor a header-only class named "mstring" and typedef it to "pstring" in
// the handful of files that need it, then substitute std::string -> pstring.
//
// Surface implemented is exactly what the vendored files touch:
//   ctor(default / const char* / const char*,n / copy), operator=,
//   c_str, data, size, length, empty, clear, operator[],
//   operator+= (char* / mstring / char), operator+ , operator== / != ,
//   find, find_first_of, substr, erase, append, static npos.
//
// No exceptions are thrown (the whole app is built -fno-exceptions); on a bad
// index or allocation failure we clamp or abort via the libc heap, matching
// the plan's "abort-style" policy rather than throwing.
#ifndef MSTRING_H
#define MSTRING_H

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

class mstring
{
    char*  m_buf;   // always null-terminated, never NULL after any op
    size_t m_len;   // length in bytes, excluding the terminator
    size_t m_cap;   // allocated bytes, including room for the terminator

    void ensure(size_t need_len)
    {
        // need_len is the desired string length (without terminator).
        if (need_len + 1 <= m_cap && m_buf)
            return;
        size_t newcap = m_cap ? m_cap : 16;
        while (newcap < need_len + 1)
            newcap *= 2;
        char* nb = (char*)realloc(m_buf, newcap);
        if (!nb)
            abort();
        m_buf = nb;
        m_cap = newcap;
    }

    void init_from(const char* s, size_t n)
    {
        m_buf = 0;
        m_len = 0;
        m_cap = 0;
        ensure(n);
        if (n && s)
            memcpy(m_buf, s, n);
        m_len = n;
        m_buf[m_len] = '\0';
    }

public:
    static const size_t npos = (size_t)-1;

    mstring()                 { init_from("", 0); }
    mstring(const char* s)    { init_from(s, s ? strlen(s) : 0); }
    mstring(const char* s, size_t n) { init_from(s, n); }
    mstring(const mstring& o) { init_from(o.m_buf, o.m_len); }

    ~mstring() { if (m_buf) free(m_buf); m_buf = 0; }

    mstring& operator=(const mstring& o)
    {
        if (this == &o) return *this;
        ensure(o.m_len);
        if (o.m_len) memcpy(m_buf, o.m_buf, o.m_len);
        m_len = o.m_len;
        m_buf[m_len] = '\0';
        return *this;
    }
    mstring& operator=(const char* s)
    {
        size_t n = s ? strlen(s) : 0;
        ensure(n);
        if (n) memcpy(m_buf, s, n);
        m_len = n;
        m_buf[m_len] = '\0';
        return *this;
    }
    mstring& operator=(char c)
    {
        ensure(1);
        m_buf[0] = c;
        m_len = 1;
        m_buf[1] = '\0';
        return *this;
    }

    const char* c_str() const { return m_buf; }
    const char* data()  const { return m_buf; }
    size_t size()   const { return m_len; }
    size_t length() const { return m_len; }
    bool   empty()  const { return m_len == 0; }
    void   clear()  { m_len = 0; if (m_buf) m_buf[0] = '\0'; }

    char& operator[](size_t i)             { return m_buf[i]; }
    const char& operator[](size_t i) const { return m_buf[i]; }

    mstring& append(const char* s, size_t n)
    {
        if (!s || n == 0) return *this;
        ensure(m_len + n);
        memcpy(m_buf + m_len, s, n);
        m_len += n;
        m_buf[m_len] = '\0';
        return *this;
    }
    mstring& append(const char* s) { return append(s, s ? strlen(s) : 0); }

    mstring& operator+=(const char* s)    { return append(s); }
    mstring& operator+=(const mstring& o) { return append(o.m_buf, o.m_len); }
    mstring& operator+=(char c)           { char t[2] = { c, 0 }; return append(t, 1); }

    // find first byte equal to c, starting at pos; npos if none.
    size_t find_first_of(char c, size_t pos = 0) const
    {
        for (size_t i = pos; i < m_len; i++)
            if (m_buf[i] == c) return i;
        return npos;
    }
    size_t find(char c, size_t pos = 0) const { return find_first_of(c, pos); }
    size_t find(const char* s, size_t pos = 0) const
    {
        if (!s) return npos;
        size_t sl = strlen(s);
        if (sl == 0) return pos <= m_len ? pos : npos;
        if (m_len < sl) return npos;
        for (size_t i = pos; i + sl <= m_len; i++)
            if (memcmp(m_buf + i, s, sl) == 0) return i;
        return npos;
    }

    mstring substr(size_t pos = 0, size_t n = npos) const
    {
        if (pos > m_len) pos = m_len;
        size_t avail = m_len - pos;
        if (n > avail) n = avail;
        return mstring(m_buf + pos, n);
    }

    // erase [pos, pos+n); n==npos erases to the end. Returns *this.
    mstring& erase(size_t pos = 0, size_t n = npos)
    {
        if (pos >= m_len) return *this;
        size_t avail = m_len - pos;
        if (n > avail) n = avail;
        // shift the tail (including terminator) left over the erased span.
        memmove(m_buf + pos, m_buf + pos + n, m_len - pos - n + 1);
        m_len -= n;
        return *this;
    }

    bool operator==(const mstring& o) const
    {
        return m_len == o.m_len && memcmp(m_buf, o.m_buf, m_len) == 0;
    }
    bool operator==(const char* s) const
    {
        if (!s) return false;
        return strcmp(m_buf, s) == 0;
    }
    bool operator!=(const mstring& o) const { return !(*this == o); }
    bool operator!=(const char* s)    const { return !(*this == s); }
};

inline mstring operator+(const mstring& a, const mstring& b)
{
    mstring r(a); r += b; return r;
}
inline mstring operator+(const mstring& a, const char* b)
{
    mstring r(a); r += b; return r;
}
inline mstring operator+(const char* a, const mstring& b)
{
    mstring r(a); r += b; return r;
}

inline bool operator==(const char* a, const mstring& b) { return b == a; }
inline bool operator!=(const char* a, const mstring& b) { return b != a; }

#endif // MSTRING_H
