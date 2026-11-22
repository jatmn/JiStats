// MinGW-w64 toolchains that target msvcrt.dll (the Debian and Ubuntu packages)
// declare _dupenv_s but do not export it. Provide it so cross builds link the
// same code that MSVC builds. UCRT-based MinGW toolchains already export it.
#if defined(__MINGW32__) && !defined(_UCRT)

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

errno_t __cdecl dupenvCompat(char** buffer, size_t* count, const char* name) {
    if (buffer == nullptr || name == nullptr) {
        return EINVAL;
    }
    *buffer = nullptr;
    if (count != nullptr) {
        *count = 0;
    }

    const char* value = std::getenv(name);
    if (value == nullptr) {
        return 0;
    }

    const size_t size = std::strlen(value) + 1;
    auto* copy = static_cast<char*>(std::malloc(size));
    if (copy == nullptr) {
        return ENOMEM;
    }
    std::memcpy(copy, value, size);
    *buffer = copy;
    if (count != nullptr) {
        *count = size;
    }
    return 0;
}

} // namespace

// The CRT headers declare _dupenv_s as dllimport, so callers resolve __imp__dupenv_s.
extern "C" {
errno_t(__cdecl* __imp__dupenv_s)(char**, size_t*, const char*) = dupenvCompat;
}

#endif
