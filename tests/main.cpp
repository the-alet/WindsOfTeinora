#include <cstring>

#include "no_dialogs.h"
#include "test.h"

int main(int argc, char** argv) {
    taelia::sendErrorsToStderr();
    using namespace taelia::test;
    int run = 0, failedCases = 0;
    for (const Case& c : registry()) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        const int before = failures();
        c.fn();
        ++run;
        const bool ok = failures() == before;
        failedCases += ok ? 0 : 1;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    }
    std::printf("%d tests, %d failed\n", run, failedCases);
    return (failedCases == 0 && run > 0) ? 0 : 1;
}
