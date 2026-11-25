/* p2libm: stand-in for <features.h> on hosts whose C library has none (macOS, Windows). musl's
 * sources include it for the `hidden` and `weak_alias` macros, which p2m_names.h defines. On Linux
 * this folder is not on the include path, so glibc's own features.h is used. */
#pragma once
