#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    // A forked child has only the forking thread, so a crash flush waits forever on the log thread; re-exec instead.
    testing::GTEST_FLAG(death_test_style) = "threadsafe";
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
