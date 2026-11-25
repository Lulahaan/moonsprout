// The vtable pointer an object holds when its dynamic type is exactly a given class, for the few
// host checks that cannot use RTTI (the game is built without it).
//
//   P2_VTABLE(_ZTVN4Game17SingleGameSectionE, "??_7SingleGameSection@Game@@6B@")  // declare
//   if (*(void* const*)obj == P2_VPTR(_ZTVN4Game17SingleGameSectionE)) ...           // compare
//
// Itanium C++ ABI (Linux, macOS): the vtable symbol starts with two slots (offset to top, type
// info), and an object's vptr points just past them. MSVC ABI (Windows): the `vftable symbol
// already points at the first virtual function, which is what the vptr holds.
#pragma once

#if defined(_MSC_VER)
#define P2_VTABLE(itanium, msvc) extern "C" void* itanium[] __asm__(msvc);
#define P2_VPTR(itanium)         ((const void*)&itanium[0])
#else
#define P2_VTABLE(itanium, msvc) extern "C" void* itanium[];
#define P2_VPTR(itanium)         ((const void*)&itanium[2])
#endif
