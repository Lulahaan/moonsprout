# p2libm: vendored math library

The game's math functions, taken from musl so that every host and CPU computes the same bits
(determinism checklist item 11 in `docs/ONLINE_MULTIPLAYER.md`; build notes in
`docs/online/M5_1B_BUILD.md`). glibc picks FMA or AVX2 code paths by CPU and its results change
between versions; macOS has its own libm. Lockstep online play needs one result everywhere.

## Origin

- musl 1.2.5, <https://musl.libc.org/releases/musl-1.2.5.tar.gz>
- sha256 of the archive: `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`
- Licence: MIT, see `COPYRIGHT` (musl's file, unchanged). The fdlibm-derived files carry Sun's
  permissive notice ("Permission to use, copy, modify, and distribute this software is freely
  granted, provided that this notice is preserved") at their top, the newer ones (exp, log, pow
  and their float versions, `__math_*`) are ARM's MIT code with their own header. Files without a
  header upstream are musl's own code under `COPYRIGHT`. Every header is kept as it was.

## What is here

| Path | What |
|---|---|
| `src/*.c`, `src/*_data.h` | musl `src/math/` files, byte for byte (list and sha256 below) |
| `internal/libm.h` | musl `src/internal/libm.h`, trimmed (see Changes) |
| `internal/fp_arch.h` | musl `arch/generic/fp_arch.h`, unchanged (empty: the generic barriers in libm.h are used on every target) |
| `p2m_names.h` | forced first include of every source: renames each function and internal helper to `p2m_*` |
| `p2m.h` | the public declarations (`p2m_sinf`, ...), C linkage |
| `p2m_sqrt.c` | `sqrt`, `sqrtf`: the IEEE-754 instruction (`__builtin_sqrt`, built with `-fno-math-errno`) |
| `p2m_route_mtx.h` | forced include of Aurora's `aurora_mtx`: its math calls go to `p2m_*` |
| `names.txt` | the public functions; CMake reads it |
| `selftest.c`, `selftest_expected.txt` | `p2libm_selftest` and its expected hashes |

Public functions (each exported as `p2m_<name>`):
`sinf cosf tanf asinf acosf atanf atan2f expf logf log10f powf fmodf floorf ceilf lrintf sqrtf`
and `sin cos tan asin acos atan atan2 exp log log10 pow fmod floor ceil ldexp frexp modf sqrt`.
Internal helpers that come with them: `rintf scalbn __sin __cos __tan __sindf __cosdf __tandf
__rem_pio2 __rem_pio2f __rem_pio2_large __math_*` and the data tables `__exp_data __exp2f_data
__logf_data __log_data __pow_log_data __powf_log2_data` (all renamed with the `p2m_` prefix).

## Changes from upstream

- No `.c` file is edited. The names are changed by `p2m_names.h` (`#define sin p2m_sin`, ...),
  which also reads the system `<math.h>` first so its declarations keep their own names.
- `p2m_names.h` undefines `__FP_FAST_FMA`, so `log.c` and `pow.c` take their non-FMA path on every
  target (arm64 predefines it); `fabs`/`fabsf` are the compiler builtins even under `-fno-builtin`.
- `internal/libm.h`: `#include <endian.h>` removed (not on macOS, only needed by the removed
  block); the `union ldshape` block, `fp_barrierl`, `fp_force_evall`, the long double branch of
  `FORCE_EVAL` and the declarations of long double and lgamma helpers are removed. Nothing in this
  subset uses long double.
- Built as C with `-ffp-contract=off -fno-builtin -fno-fast-math -fno-math-errno`, no `-march`.
  No function reads the rounding mode (they assume round-to-nearest, which the game never changes)
  and none sets errno (musl never does).

## Checks

- `ninja -C build-pc p2libm_selftest && ./build-pc/p2libm_selftest port/libm/selftest_expected.txt`
  prints a hash of each function over 4 million fixed inputs, for the vendored and the host
  version, plus the ulp distance between them, and exits 1 when a vendored hash differs from the
  file. Under `GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA` the host `sinf`, `cosf`, `sin` hashes
  change; the vendored ones must not.
- The build fails when the game library or `aurora_mtx` still imports a system libm function
  (`cmake/p2_libm_check.cmake`).

## Updating

Replace the files from a newer musl release, keep `p2m_names.h` in step with any new internal
helper, record the new archive's sha256 here, run the selftest and commit the new
`selftest_expected.txt` only when the change of results is intended (it changes the simulation,
so the golden checksum logs in `tools/net/` must be regenerated too).

## File list (sha256)

```
cc6f9017e7e2e0ef01516fa12c68aa63865e6415c90dc039dfce9abffae09a4c src/acos.c
b20b3dcb58ce7cc55963945f8cb83efbf064b29d2da3011f2cec5d08459f2aa7 src/acosf.c
9302dd0b2f302f7b7decce82e737ed250bae1bcc65c19eaf3d7ec19010892e63 src/asin.c
6bc9b2392b81efa4c6a80c8602a2c0898d3b5c77444f6d3266ac71c18df222c5 src/asinf.c
a5f94b09b10b4e72bb15d94ae403c9dfd27d7ad881ecd729000004429acfca63 src/atan2.c
31a61a0a4c7b00db625b708525f24673a83df868a3e363042c9cc9dcf0aaeec8 src/atan2f.c
1cffb67a65456ae0bc5f07a94df04a4f18ebc1ac0bffe3219250c4e0555ee01d src/atan.c
e0533d7b72aabce311a758a1f94fd15e5d4067be73b716cb1f425df5bc9039d6 src/atanf.c
f616b50efc61eb35f946308b222f5f83ca707b4efaf7ea175969a5b62d2d6637 src/ceil.c
691d3a8e183ebe5f09a68120766877872e10f9267fe6e2bba0341b93d2863cc8 src/ceilf.c
3f0f774aab60a18d7ef935ffdf121bcda17cbadabff14096c614da22e31a42c1 src/__cos.c
3a74e054057d636fa494bdd76a53a40db5cee618264f6ab31cb0538ba63c6bda src/cos.c
0ba872cc7e26b93bb9784d19fee84b263e1fa3543a76ec3c0893aea47453a2a7 src/__cosdf.c
33cf828e8e733da2b72c723b9ff20057b9a4f10df72a3923dadcb626c23d222c src/cosf.c
3da38c471cc5eb8dc1c2e283c6c00a36b1d353ba5c8ca1e82d95180948e26bd5 src/exp2f_data.c
fb52b690bef63df2e8996f2d2b3fd77f9ea08795863e73afdc882df04460e850 src/exp2f_data.h
7db56f21ca9b8915d36dd5099b707aafeb6a736eae798e19b76b074764e75f34 src/exp.c
c55dff64e369fe816cd8ce367064989bb119f85783599680d1a329031c660689 src/exp_data.c
df77fca837edd02b260dd09a2914202775909ae67a059218ae38cfab104e6d16 src/exp_data.h
225123b9b3a47a083c06f2580c6b73d5cadae5cec545f9e928f7041ff837b4b4 src/expf.c
b9ccb9363719a84be218d60c56717f4fb0234e170c305e36ce21c30e1f42f8e1 src/floor.c
55a62f12a4bcd151ca3a82e9ec98b1744c15017f1b6cdea56b33220ffdb166f3 src/floorf.c
33fb61093e04783081b9e8f8a8a303456b2f919c215cd0c378c1ed0acf41934c src/fmod.c
eecc1cf499f143181880a33a95f1614be323aca704815a7b57d4969a7cacc2d4 src/fmodf.c
58a2973f80f9def7940708850e97f8080da38be2496cd473f7cb9023e9d59e94 src/frexp.c
702b608d3a5cf9c5d26c26ef2c528a3003555ee48ececec3b817c817d297bfe8 src/ldexp.c
ef9b4b1f0918cf1fcb797f9fa4d4ffe177bfa27f668d74a41d613bfbcd83850a src/log10.c
b8de5c0f14be2c622eca1cb33e80e5b131576c7c229aeffb8ea9f1d61555f6b9 src/log10f.c
a0694d8587a0d145d0f8ae87cbffba014d7988bc2ebcbfad94cea73b394c9ecc src/log.c
e96bcd99d88fc57e0f3686299014f4b949a6212603ce237f39e16cedc155617a src/log_data.c
a8dced6f0821f30cad040ba326b03e419fbe0a750ed8f3b0d7145992bee4bccc src/log_data.h
eecf8e5dd0afd5f64f938ccae07c85b0a8e82919f5d4269936a3b8493fb64e47 src/logf.c
60b08342fcef877b393a76fc36d92c576d6340ce153459aad03aad3d4716f9b5 src/logf_data.c
80176d1f045f162e917ca5c2879994ab0ed9e27b479086d752e5793c91135adf src/logf_data.h
d6673476094b190bd80f92bf89d3c6254beaf1c33fc7f7245059bcdcd7f4c6c9 src/lrintf.c
84aa910bdc5e7ccfed42098c37c44e278aa942d46e5a51d5ee8273336c79e719 src/__math_divzero.c
194c95f6cd238ffa14e78087b9acffca3a064f2fe46320d67b090bb7a559b1ca src/__math_divzerof.c
b56440ed59fa1e1aaaf6bf8b672c8bb71eaf844457906f7773da1d67d7ef013b src/__math_invalid.c
d2af8360271109d77dfab3c538eb158e34633454707dea4efa9f2f1e7e2bd0c7 src/__math_invalidf.c
74669de53bc4b250183cfa262a8a03ee8106f33a83e3aca6b217bafc7363b83c src/__math_oflow.c
69414934bb798f5261e2d80f4b878e2bbe4dfa6025adbd06586195ba5554b04a src/__math_oflowf.c
9a841197bbe9ade9bbd684f8bc65424a97c481d37dabb9fa1790bafe7959034a src/__math_uflow.c
2371ed08fc95e48833a67122b5ef538ce7fc87f40fc7a4df451d71e1787d1d42 src/__math_uflowf.c
5feafe10347636884a071a5096f912d5ca4a0939844804ca09b88241405648ad src/__math_xflow.c
38da031cfed0ba90e68a14dfbc57404ebcb827c9ac34648161c7d11cb212c9ba src/__math_xflowf.c
9681ec8b49299a6fc2b2592241232535a1bc8e785f149efadfbb7637d006ca00 src/modf.c
b06f0a08d74fcff8cced7afa2c0e47e301e3e8a19c0de5becdec28cd7b014027 src/pow.c
2052ec913f9b4a2170a98a6fcbcd7694f5dec220036b00c36106a2b4b65a4c55 src/pow_data.c
5014345a7958adcf6ff835b5e96afa99dd5a98c40dfe968442288e002b8e01a3 src/pow_data.h
74069b3a380cf954361e0243b38de2094ae680e2ecfb3e50af61ab576f13babf src/powf.c
136f491b0f904842b926784134d0adba7a9103d2ce8a226276dd6e9260457398 src/powf_data.c
5d632276e5f21cdcd47f35c53cf792640ad347aabce1e1bd5774fad5371fc8d8 src/powf_data.h
938329e0939cd83ee2185a52154b41242f618d4a6df1b08395f344da33d4f1e6 src/__rem_pio2.c
b73a0219ba4a34e39ed904b5008126ff1b12dfa1a4a380941363c8db5088d756 src/__rem_pio2f.c
b97f19fb5951c83a0c10506fab35646caefde13bd03100ad2af55ef00af0b9fb src/__rem_pio2_large.c
9adef215d38823468cd77d7d5a9b305041d7020196ce65caba35f05ba2dd6f34 src/rintf.c
a2e766283ed30c2b30c71e01dbdabef58faf92cc9aa84a01c1ce1069b6625696 src/scalbn.c
f3a37ef9bcfb8a2f9c809d8c7f1aa2c17c8c3319ad7fb2fe7c0a6284ea8859d2 src/__sin.c
24e53a9d85f196f66919cc19247be5480bb607ef38844b1a24bc47c4fe4974ef src/sin.c
de7026bcd159b7d06a8cbcc162df722014f3b1a3cc4183116b6b7829eef1d118 src/__sindf.c
2583af55446f07da3225186c368a7aae5a0a0a0edab268967f4122b92e222b87 src/sinf.c
e18b65b69fced8dbb2d3ed1a3dc6daea0e9f857f2762f274380bd89a6f89d2c4 src/__tan.c
422d411bac962a1077ae394d14e54313012a6faa3476c20b2a66d3fb3f7a923f src/tan.c
60455f3cc0ac743f3eaf0017a3a4f0adbab37c0ee30d29397852d22904c40444 src/__tandf.c
38b39aaf03d33341e606b67b3743ee5fa436b7e8338af155994d4b794b366003 src/tanf.c
563db430091e2ba5156c828ab0c174441b0fda83167faa7d9e9ace99e81d9145 internal/libm.h
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 internal/fp_arch.h
f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af COPYRIGHT
```

`internal/libm.h` is the trimmed copy; upstream `src/internal/libm.h` has sha256
`d5a11757541471030d8f4bf7a39a29de35732ecbb291b2f45bf03296be4f25a2`.
