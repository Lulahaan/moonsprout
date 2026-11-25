#if TARGET_PC
#ifdef __cplusplus
extern "C++" {
#endif
#ifndef _P2_PC_FUNCTIONAL_H
#define _P2_PC_FUNCTIONAL_H
#include "types.h"
#include <functional>
#if defined(_LIBCPP_VERSION)
namespace std {
// Removed from libc++ in C++17 (libstdc++ still has it); the decomp still derives from it.
template <typename LHS, typename RHS, typename Result>
struct binary_function {
	typedef LHS first_argument_type;
	typedef RHS second_argument_type;
	typedef Result result_type;
};
} // namespace std
#endif
#endif
#ifdef __cplusplus
}
#endif
#else // TARGET_PC

#ifndef _STD_FUNCTIONAL_H
#define _STD_FUNCTIONAL_H

#include "types.h"

namespace std {
template <typename LHS, typename RHS, typename Result>
struct binary_function {
	typedef LHS first_argument_type;
	typedef RHS second_argument_type;
	typedef Result result_type;
};

template <typename T>
struct less : public binary_function<T, T, bool> {
	bool operator()(const T& lhs, const T& rhs) const { return lhs < rhs; }
};
} // namespace std

#endif

#endif // TARGET_PC
