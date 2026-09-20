// ==============================================================================
// Step 2.5 infrastructure smoke test.
//
// Purpose is deliberately narrow. This file exists to prove two things:
//
//   1. GoogleTest + CTest actually run in this repository, through the
//      CMake test target, end to end.
//   2. The test target can include project headers -- here "const.h" -- so
//      that future pure Meeting-domain tests can consume production types
//      without restructuring the source tree.
//
// It is NOT Meeting coverage. The 57 first-round Meeting correctness scenarios
// from the Phase 1 test strategy stay unimplemented until M2/M3; this step only
// creates the infrastructure that will make them runnable.
//
// The subject under test is the existing Defer RAII utility in const.h. It was
// chosen because it is real production code, is already compiled into the
// application (so it is not a synthetic green test), and has zero runtime
// dependencies -- no Redis, no MySQL, no gRPC, no sockets, no config.ini.
// ==============================================================================

#include <gtest/gtest.h>

#include "const.h"

TEST(TestInfrastructureSmoke, DeferExecutesOnScopeExit)
{
    int invocation_count = 0;

    {
        Defer defer([&invocation_count]() {
            ++invocation_count;
        });

        // Nothing may run while the deferring scope is still alive.
        EXPECT_EQ(invocation_count, 0);
    }

    // The destructor ran the callback exactly once as the scope ended.
    EXPECT_EQ(invocation_count, 1);
}
