#ifndef _EFX_TSYNCGROUP_H
#define _EFX_TSYNCGROUP_H

#include "efx/TBase.h"
#include "efx/TSync.h"

// #define TSYNCGROUP_DECL(count)

namespace efx {

#if TARGET_PC
// Host builds define the members inline so every user instantiates them (MWCC's out-of-line
// definitions in efxBase.cpp relied on its looser template instantiation; N=5 had none at all).
#define DEF_SYNC_GROUP(N)                                                                   \
	template <typename T>                                                                   \
	struct TSyncGroup##N : TBase {                                                          \
		virtual bool create(Arg* arg)                                                       \
		{                                                                                   \
			for (u32 i = 0; i < N; i++) {                                                   \
				if (!mItems[i].create(arg)) {                                               \
					return false;                                                           \
				}                                                                           \
			}                                                                               \
			return true;                                                                    \
		}                                                                                   \
		virtual void fade()                                                                 \
		{                                                                                   \
			for (u32 i = 0; i < N; i++) {                                                   \
				mItems[i].fade();                                                           \
			}                                                                               \
		}                                                                                   \
		virtual void forceKill()                                                            \
		{                                                                                   \
			for (u32 i = 0; i < N; i++) {                                                   \
				mItems[i].forceKill();                                                      \
			}                                                                               \
		}                                                                                   \
		virtual void startDemoDrawOff()                                                     \
		{                                                                                   \
			for (u32 i = 0; i < N; i++) {                                                   \
				mItems[i].startDemoDrawOff();                                               \
			}                                                                               \
		}                                                                                   \
		virtual void endDemoDrawOn()                                                        \
		{                                                                                   \
			for (u32 i = 0; i < N; i++) {                                                   \
				mItems[i].endDemoDrawOn();                                                  \
			}                                                                               \
		}                                                                                   \
		T mItems[N]; /* _04 */                                                              \
	}
#else
#define DEF_SYNC_GROUP(N)                \
	template <typename T>                \
	struct TSyncGroup##N : TBase {       \
		/* VTABLE */                     \
		virtual bool create(Arg* arg);   \
		virtual void fade();             \
		virtual void forceKill();        \
		virtual void startDemoDrawOff(); \
		virtual void endDemoDrawOn();    \
		/* VTABLE END */                 \
                                         \
		/* _00 	= VTBL */                \
		T mItems[N]; /* _04 */           \
	}
#endif

DEF_SYNC_GROUP(2);
DEF_SYNC_GROUP(3);
DEF_SYNC_GROUP(4);
DEF_SYNC_GROUP(5);
DEF_SYNC_GROUP(6);

} // namespace efx

#endif
