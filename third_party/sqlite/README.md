# SQLite Amalgamation

This directory should contain the SQLite amalgamation source files.

## How to obtain SQLite

1. Download from https://www.sqlite.org/download.html
   - Look for "amalgamation" zip file
2. Or use the following files:
   - sqlite3.c
   - sqlite3.h
   - sqlite3ext.h (optional, for extensions)

## Alternative: Use System SQLite

Set CMake option:
```
cmake -DJISTATS_USE_SYSTEM_SQLITE=ON ..
```

This will use the system's SQLite library instead.
