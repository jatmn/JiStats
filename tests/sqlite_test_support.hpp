#pragma once

#include <catch_amalgamated.hpp>

namespace JiStats::TestSupport {

inline constexpr bool sqliteAvailable() {
#if defined(JISTATS_USE_SYSTEM_SQLITE) || defined(JISTATS_HAS_SQLITE)
    return true;
#else
    return false;
#endif
}

inline void requireSqlite() {
    if (!sqliteAvailable()) {
        SKIP("SQLite is not compiled into this test build");
    }
}

} // namespace JiStats::TestSupport
