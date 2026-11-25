#ifndef _JSTUDIO_TCREATEOBJECT_H
#define _JSTUDIO_TCREATEOBJECT_H

#include "JSystem/JStudio/stb-data-parse.h"
#include "JSystem/JGadget/linklist.h"

namespace JStudio {
struct TObject;

struct TCreateObject {

	virtual ~TCreateObject()                                                              = 0; // _08
	virtual bool create(TObject** newObject, const stb::data::TParse_TBlock_object& data) = 0; // _0C

	// _00 = VTBL
	JGadget::TLinkListNode mLinkListNode; // _04
};

// Offset of the list node, for JGadget::TLinkList (the vtable pointer is 8 bytes on the host).
#define P2_JSTUDIO_CREATEOBJECT_NODE PC_SWITCH(-(int)__builtin_offsetof(JStudio::TCreateObject, mLinkListNode), -4)
} // namespace JStudio

#endif
