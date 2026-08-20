/* Compatibility shim: esp-sr libfst.a references _ctype_ which is not
 * exported by newlib in ESP-IDF v6.0 + GCC 15.2 toolchain.
 * Provide a minimal ASCII ctype table. */
#include <stdint.h>

/* Bit flags matching newlib's _ctype_ table format */
#define _U  0x01  /* upper */
#define _L  0x02  /* lower */
#define _N  0x04  /* digit */
#define _S  0x08  /* space */
#define _P  0x10  /* punct */
#define _C  0x20  /* control */
#define _X  0x40  /* hex digit */
#define _B  0x80  /* blank */

const char _ctype_[257] = {
    0,  /* EOF (-1) maps to index 0 */
    _C, _C, _C, _C, _C, _C, _C, _C,                         /* 0x00-0x07 */
    _C, _C|_S|_B, _C|_S, _C|_S, _C|_S, _C|_S, _C, _C,      /* 0x08-0x0F */
    _C, _C, _C, _C, _C, _C, _C, _C,                         /* 0x10-0x17 */
    _C, _C, _C, _C, _C, _C, _C, _C,                         /* 0x18-0x1F */
    _S|_B, _P, _P, _P, _P, _P, _P, _P,                      /* 0x20-0x27 */
    _P, _P, _P, _P, _P, _P, _P, _P,                         /* 0x28-0x2F */
    _N, _N, _N, _N, _N, _N, _N, _N,                         /* 0x30-0x37 */
    _N, _N, _P, _P, _P, _P, _P, _P,                         /* 0x38-0x3F */
    _P, _U|_X, _U|_X, _U|_X, _U|_X, _U|_X, _U|_X, _U,     /* 0x40-0x47 */
    _U, _U, _U, _U, _U, _U, _U, _U,                         /* 0x48-0x4F */
    _U, _U, _U, _U, _U, _U, _U, _U,                         /* 0x50-0x57 */
    _U, _U, _U, _P, _P, _P, _P, _P,                         /* 0x58-0x5F */
    _P, _L|_X, _L|_X, _L|_X, _L|_X, _L|_X, _L|_X, _L,     /* 0x60-0x67 */
    _L, _L, _L, _L, _L, _L, _L, _L,                         /* 0x68-0x6F */
    _L, _L, _L, _L, _L, _L, _L, _L,                         /* 0x70-0x77 */
    _L, _L, _L, _P, _P, _P, _P, _C,                         /* 0x78-0x7F */
    /* 0x80-0xFF: all zero (non-ASCII) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
