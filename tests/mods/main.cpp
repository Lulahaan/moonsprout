// p2mods_selftest (M5.2): runs every case registered with MODS_TEST (tests/mods/test.hpp).
#include "test.hpp"

#include <cstdio>
#include <exception>

int main()
{
	int passed = 0, failed = 0;
	for (const mods_test::Case& c : mods_test::registry()) {
		const int before = mods_test::failures();
		try {
			c.fn();
		} catch (const std::exception& e) {
			mods_test::fail(__FILE__, __LINE__, std::string(c.name) + ": exception: " + e.what());
		} catch (...) {
			mods_test::fail(__FILE__, __LINE__, std::string(c.name) + ": unknown exception");
		}
		if (mods_test::failures() == before) {
			passed++;
		} else {
			failed++;
			std::fprintf(stderr, "  in case %s\n", c.name);
		}
	}
	std::printf("p2mods_selftest: %d passed, %d failed\n", passed, failed);
	return failed == 0 ? 0 : 1;
}
