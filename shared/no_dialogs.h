// Automated runs and tests must never open a window: asserts, CRT errors and crashes go to stderr instead of
// modal dialogs (which would also take focus and stall the run).
#pragma once
#ifdef _WIN32
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>

namespace taelia {

inline void sendErrorsToStderr() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

}  // namespace taelia
#else
namespace taelia {
inline void sendErrorsToStderr() {}
}  // namespace taelia
#endif
