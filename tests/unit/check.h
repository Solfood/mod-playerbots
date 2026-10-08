// tests/unit/check.h (copied from mod-guild-bridge)
// Minimal test helper: CHECK_EQ(expected, actual) records a failure with file:line; main() returns the count.
#pragma once
#include <iostream>
#include <sstream>
#include <string>

inline int& UnitFailures()
{
    static int failures = 0;
    return failures;
}

#define CHECK_EQ(expected, actual)                                                                       \
    do                                                                                                   \
    {                                                                                                    \
        auto const& e_ = (expected);                                                                     \
        auto const& a_ = (actual);                                                                       \
        if (!(e_ == a_))                                                                                 \
        {                                                                                                \
            std::ostringstream o_;                                                                       \
            o_ << __FILE__ << ":" << __LINE__ << "\n  expected: " << e_ << "\n  actual:   " << a_;      \
            std::cout << "FAIL " << o_.str() << "\n";                                                    \
            ++UnitFailures();                                                                            \
        }                                                                                                \
    } while (0)

#define CHECK_TRUE(cond) CHECK_EQ(true, static_cast<bool>(cond))
