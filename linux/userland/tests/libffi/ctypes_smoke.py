# ctypes_smoke.py: Python's ctypes on KlaussCPU (libffi port), run by
# run.sh's "ctypes" boot as "python3 -s -S ctypes_smoke.py".  Calls into
# musl through ctypes: scalars, a variadic function, double/float/long
# double results, structs by value in both directions, a callback through
# qsort, pointers, errno, _Complex.  Prints PASS/FAIL lines and
# ALL-CTYPES-DONE; exits 1 if anything failed.
import sys
import ctypes
from ctypes import (CDLL, CFUNCTYPE, POINTER, Structure, byref, c_char, c_char_p, c_double,
                    c_float, c_int, c_long, c_longdouble, c_longlong, c_size_t, c_uint,
                    c_uint8, c_uint32, c_void_p, cast, create_string_buffer, sizeof)

fails = 0


def check(name, cond, detail=''):
    global fails
    if cond:
        print(f'PASS {name}')
    else:
        fails += 1
        print(f'FAIL {name} {detail}')


def run(name, fn):
    try:
        fn()
    except Exception as e:  # report and go on
        check(name, False, f'{type(e).__name__}: {e}')


print(f'INFO python {sys.version.split()[0]} platform {sys.platform}')
check('sizes', (sizeof(c_long), sizeof(c_void_p), sizeof(c_int), sizeof(c_longdouble)) == (8, 8, 4, 8),
      (sizeof(c_long), sizeof(c_void_p), sizeof(c_int), sizeof(c_longdouble)))
libc = CDLL(None, use_errno=True)	# the program and its libraries: musl (libm is in libc)


def t_strlen():
    libc.strlen.restype = c_size_t
    libc.strlen.argtypes = [c_char_p]
    check('strlen', libc.strlen(b'hello, klausscpu') == 16)
    check('strlen-empty', libc.strlen(b'') == 0)


def t_snprintf():
    # Variadic: argtypes name the fixed arguments, so ctypes uses
    # ffi_prep_cif_var (home area); 8 arguments, some on the stack.
    f = libc.snprintf
    f.restype = c_int
    f.argtypes = [c_char_p, c_size_t, c_char_p]
    buf = create_string_buffer(128)
    n = f(buf, 128, b'%d|%s|%.3f|%ld|%u|%lld|%c', c_int(-42), b'str', c_double(2.5), c_long(-7),
          c_uint(0x80000000), c_longlong(-(1 << 40)), c_int(ord('Z')))
    want = b'-42|str|2.500|-7|2147483648|-1099511627776|Z'
    check('snprintf-variadic', buf.value == want and n == len(want), (n, buf.value))
    # Without argtypes ctypes makes a non-variadic call; with every
    # argument in R0-R3 libffi's call area of at least 32 bytes (the
    # callee's home area) makes it work anyway.
    g = CDLL(None).snprintf
    g.restype = c_int
    buf2 = create_string_buffer(32)
    n = g(buf2, c_size_t(32), b'<%d>', c_int(1234))
    check('snprintf-no-argtypes', buf2.value == b'<1234>' and n == 6, (n, buf2.value))
    # With stack arguments: libffi stores the 1-4 stack words of a
    # non-variadic call both where a non-variadic callee reads them and,
    # after the home area, where a variadic one does.  buf, n, fmt and 5
    # ints are 8 words, the most that works this way.
    buf3 = create_string_buffer(64)
    n = g(buf3, c_size_t(64), b'%d %d %d %d %d', 1, -2, 3, -4, 5)
    check('snprintf-no-argtypes-5', buf3.value == b'1 -2 3 -4 5' and n == 11, (n, buf3.value))
    # 6 ints are 9 words: the two layouts overlap, and only a variadic
    # cif (argtypes for the fixed arguments, as above) gets it right.
    n = g(buf3, c_size_t(64), b'%d %d %d %d %d %d', 1, 2, 3, 4, 5, 6)
    print(f'INFO snprintf-no-argtypes-6 (9 words, not supported): {buf3.value!r}')


def t_math():
    libc.pow.restype = c_double
    libc.pow.argtypes = [c_double, c_double]
    check('pow', libc.pow(2.0, 10.0) == 1024.0)
    check('pow-frac', abs(libc.pow(2.0, 0.5) - 1.4142135623730951) < 1e-15, libc.pow(2.0, 0.5))
    libc.powf.restype = c_float
    libc.powf.argtypes = [c_float, c_float]
    check('powf', abs(libc.powf(2.0, 0.5) - 1.41421356) < 1e-6, libc.powf(2.0, 0.5))
    libc.ldexp.restype = c_double
    libc.ldexp.argtypes = [c_double, c_int]
    check('ldexp', libc.ldexp(0.75, -3) == 0.09375)
    libc.powl.restype = c_longdouble
    libc.powl.argtypes = [c_longdouble, c_longdouble]
    check('powl', libc.powl(3.0, 4.0) == 81.0)
    libc.fmaf.restype = c_float
    libc.fmaf.argtypes = [c_float, c_float, c_float]
    check('fmaf', libc.fmaf(1.5, -2.0, 0.25) == -2.75)


class div_t(Structure):
    _fields_ = [('quot', c_int), ('rem', c_int)]


class ldiv_t(Structure):
    _fields_ = [('quot', c_long), ('rem', c_long)]


class in_addr(Structure):
    _fields_ = [('s_addr', c_uint32)]


def t_structs():
    # Results in R12 (8 bytes) and R12/R11 (16 bytes); a 4-byte struct
    # argument.
    libc.div.restype = div_t
    libc.div.argtypes = [c_int, c_int]
    d = libc.div(-17, 5)
    check('div', (d.quot, d.rem) == (-3, -2), (d.quot, d.rem))
    libc.ldiv.restype = ldiv_t
    libc.ldiv.argtypes = [c_long, c_long]
    d = libc.ldiv(-(1 << 40) - 3, 1 << 20)
    check('ldiv', (d.quot, d.rem) == (-(1 << 20), -3), (d.quot, d.rem))
    libc.inet_ntoa.restype = c_char_p
    libc.inet_ntoa.argtypes = [in_addr]
    check('inet_ntoa', libc.inet_ntoa(in_addr(0x0403A8C0)) == b'192.168.3.4')


def t_qsort():
    CMP = CFUNCTYPE(c_int, POINTER(c_int), POINTER(c_int))
    calls = [0]

    def cmp(a, b):
        calls[0] += 1
        return (a[0] > b[0]) - (a[0] < b[0])
    vals = [5, -3, 2147483647, 0, -2147483648, 42, 7, -1, 100, 3]
    arr = (c_int * len(vals))(*vals)
    libc.qsort.restype = None
    libc.qsort.argtypes = [c_void_p, c_size_t, c_size_t, CMP]
    libc.qsort(arr, len(arr), sizeof(c_int), CMP(cmp))
    check('qsort-callback', list(arr) == sorted(vals) and calls[0] > 0, (list(arr), calls[0]))


class Big(Structure):
    _fields_ = [('a', c_long), ('b', c_long), ('c', c_long)]	# 24 bytes: by reference


class Mixed(Structure):
    _fields_ = [('f', c_float), ('d', c_double)]	# 16 bytes, padding after f


def t_callbacks():
    # Python callbacks called through ctypes function pointers: libffi's
    # ffi_call into its own closures, with arguments split between R3
    # and the stack, a struct by reference and narrow integers.
    seen = {}
    F = CFUNCTYPE(c_long, c_long, c_long, c_long, Mixed, Big, c_uint8, c_float, c_double)

    def cb(a, b, c, m, big, u8, f, d):
        seen['args'] = (a, b, c, m.f, m.d, big.a, big.b, big.c, u8, f, d)
        big.a = -1	# the callee's copy
        return a + b + c + big.c
    fn = F(cb)
    big = Big(10, 20, 30)
    r = fn(1, 2, 3, Mixed(0.5, -1.25), big, 250, 1.5, 6.25)
    check('callback-args', seen.get('args') == (1, 2, 3, 0.5, -1.25, 10, 20, 30, 250, 1.5, 6.25),
          seen.get('args'))
    check('callback-result', r == 36, r)
    check('callback-copy', big.a == 10, big.a)
    U = CFUNCTYPE(c_uint, c_uint)
    u = U(lambda x: x ^ 0xFFFFFFFF)
    check('callback-uint', u(0x7FFFFFFF) == 0x80000000, u(0x7FFFFFFF))
    D = CFUNCTYPE(c_double, c_double, c_float)
    dd = D(lambda x, y: x * y)
    check('callback-double', dd(1.5, 4.0) == 6.0)


def t_pointers():
    buf = create_string_buffer(b'abcdefgh', 16)
    libc.memset.restype = c_void_p
    libc.memset.argtypes = [c_void_p, c_int, c_size_t]
    p = libc.memset(ctypes.addressof(buf) + 2, ord('X'), 3)
    check('memset', buf.value == b'abXXXfgh' and p == ctypes.addressof(buf) + 2)
    libc.strchr.restype = POINTER(c_char)
    libc.strchr.argtypes = [c_char_p, c_int]
    s = b'find-the-dash'
    q = libc.strchr(s, ord('t'))
    check('strchr', q[0] == b't' and q[1] == b'h', (q[0], q[1]))
    n = c_long(0)
    libc.sscanf.restype = c_int
    libc.sscanf.argtypes = [c_char_p, c_char_p]
    k = libc.sscanf(b'  -123456789012', b'%ld', byref(n))
    check('sscanf-byref', k == 1 and n.value == -123456789012, (k, n.value))
    arr = (c_double * 4)(1.0, 2.0, 3.0, 4.0)
    pd = cast(arr, POINTER(c_double))
    check('pointer-index', pd[3] == 4.0 and sizeof(arr) == 32)


def t_errno():
    libc.close.argtypes = [c_int]
    ctypes.set_errno(0)
    rc = libc.close(-1)
    check('errno', rc == -1 and ctypes.get_errno() == 9, (rc, ctypes.get_errno()))


def t_complex():
    if not hasattr(ctypes, 'c_double_complex'):
        check('complex', False, 'no ctypes.c_double_complex (Python built without libffi complex)')
        return
    libc.cabs.restype = c_double
    libc.cabs.argtypes = [ctypes.c_double_complex]
    check('cabs', libc.cabs(3 + 4j) == 5.0)
    libc.conj.restype = ctypes.c_double_complex
    libc.conj.argtypes = [ctypes.c_double_complex]
    check('conj', libc.conj(1.5 - 2.5j) == 1.5 + 2.5j, libc.conj(1.5 - 2.5j))
    libc.conjf.restype = ctypes.c_float_complex
    libc.conjf.argtypes = [ctypes.c_float_complex]
    check('conjf', libc.conjf(0.5 + 0.25j) == 0.5 - 0.25j, libc.conjf(0.5 + 0.25j))


for name, fn in [('strlen', t_strlen), ('snprintf', t_snprintf), ('math', t_math),
                 ('structs', t_structs), ('qsort', t_qsort), ('callbacks', t_callbacks),
                 ('pointers', t_pointers), ('errno', t_errno), ('complex', t_complex)]:
    run(name, fn)
print(f'ALL-CTYPES-DONE failures={fails}')
sys.exit(1 if fails else 0)
