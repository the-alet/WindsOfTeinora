// The rigid bodies on PhysX, from the standalone game. PhysX has identifiers of its own
// that Unreal's macros would rewrite, so they are put aside for this file.
#include "HAL/Platform.h"

THIRD_PARTY_INCLUDES_START
#pragma push_macro("check")
#pragma push_macro("verify")
#pragma push_macro("ensure")
#undef check
#undef verify
#undef ensure
#include "Physics/Physics.cpp"
#pragma pop_macro("ensure")
#pragma pop_macro("verify")
#pragma pop_macro("check")
THIRD_PARTY_INCLUDES_END
