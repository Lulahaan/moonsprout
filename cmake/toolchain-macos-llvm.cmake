# macOS dev toolchain: Homebrew LLVM (Aurora needs a newer libc++ than Xcode 16.1 ships, e.g. std::jthread).
# Usage: cmake -B build-pc -G Ninja --toolchain cmake/toolchain-macos-llvm.cmake
# Optional: Apple Clang also works (CMakeLists.txt adds -fexperimental-library for it).
execute_process(COMMAND brew --prefix llvm OUTPUT_VARIABLE _llvm OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_C_COMPILER "${_llvm}/bin/clang")
set(CMAKE_CXX_COMPILER "${_llvm}/bin/clang++")
set(CMAKE_OBJC_COMPILER "${_llvm}/bin/clang")
set(CMAKE_OBJCXX_COMPILER "${_llvm}/bin/clang++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${_llvm}/lib/c++ -L${_llvm}/lib/unwind -lunwind -Wl,-rpath,${_llvm}/lib/c++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
