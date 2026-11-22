#include "app/AppController.hpp"
#include "core/Logger.hpp"
#include "providers/minimax/MiniMaxLiveCapture.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#endif

namespace {

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }

    const int wideCharCount = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (wideCharCount <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring wideValue(static_cast<size_t>(wideCharCount), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wideValue.data(), wideCharCount);
    wideValue.resize(static_cast<size_t>(wideCharCount - 1));
    return wideValue;
}

std::string wideToUtf8(const wchar_t* value, size_t length) {
    if (value == nullptr || length == 0) {
        return "";
    }

    const int utf8CharCount = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8CharCount <= 0) {
        std::string fallback;
        fallback.reserve(length);
        for (size_t index = 0; index < length; ++index) {
            const wchar_t wideChar = value[index];
            fallback.push_back(wideChar >= 0 && wideChar <= 0x7F
                ? static_cast<char>(wideChar)
                : '?');
        }
        return fallback;
    }

    std::string utf8Value(static_cast<size_t>(utf8CharCount), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        static_cast<int>(length),
        utf8Value.data(),
        utf8CharCount,
        nullptr,
        nullptr);
    return utf8Value;
}

int showStartupError(const std::string& message) {
    MessageBoxW(
        nullptr,
        utf8ToWide(message).c_str(),
        L"JiStats",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 1;
}
#else
int showStartupError(const std::string& message) {
    JISTATS_LOG_ERROR(JiStats::LogCategory::Startup, message);
    return 1;
}
#endif

bool isBackgroundArgument(const std::string& arg) {
    return arg == "--background" || arg == "-background";
}

#ifdef _WIN32
bool isBackgroundArgument(const std::wstring& arg) {
    return _wcsicmp(arg.c_str(), L"--background") == 0 ||
           _wcsicmp(arg.c_str(), L"-background") == 0;
}

void attachParentConsoleForCli() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio();
}
#endif

bool hasBackgroundFlag(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && isBackgroundArgument(argv[index])) {
            return true;
        }
    }
    return false;
}

struct CommandLineOptions {
    bool launchInBackground = false;
    bool miniMaxReadiness = false;
    bool miniMaxCapture = false;
    bool validateLive = true;
    std::string dbPath = "jistats.db";
    std::string planKey;
    std::string outputPath;
};

bool requiresValue(const std::string& arg) {
    return arg == "--db-path" || arg == "--minimax-capture-plan" || arg == "--minimax-output";
}

bool parseCommandLineOptions(const std::vector<std::string>& args,
                             CommandLineOptions& options,
                             std::string& error) {
    for (size_t index = 1; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (isBackgroundArgument(arg)) {
            options.launchInBackground = true;
            continue;
        }
        if (arg == "--minimax-readiness") {
            options.miniMaxReadiness = true;
            continue;
        }
        if (arg == "--skip-live-validation") {
            options.validateLive = false;
            continue;
        }
        if (requiresValue(arg)) {
            if (index + 1 >= args.size()) {
                error = "Missing value for argument " + arg;
                return false;
            }
            const std::string& value = args[++index];
            if (arg == "--db-path") {
                options.dbPath = value;
            } else if (arg == "--minimax-capture-plan") {
                options.miniMaxCapture = true;
                options.planKey = value;
            } else if (arg == "--minimax-output") {
                options.outputPath = value;
            }
            continue;
        }

        error = "Unknown argument: " + arg;
        return false;
    }

    if (options.miniMaxReadiness && options.miniMaxCapture) {
        error = "MiniMax helper modes are mutually exclusive; choose --minimax-readiness or --minimax-capture-plan";
        return false;
    }

    if (options.miniMaxCapture) {
        if (!JiStats::MiniMaxLiveCapture::isSupportedPlanKey(options.planKey)) {
            error = "MiniMax capture requires --minimax-capture-plan coding_plan|token_plan";
            return false;
        }
        if (options.outputPath.empty()) {
            error = "MiniMax capture requires --minimax-output <path>";
            return false;
        }
    }

    return true;
}

void printMiniMaxReadiness(const std::vector<JiStats::MiniMaxCapturePlanStatus>& statuses) {
    for (const auto& status : statuses) {
        std::cout
            << status.planKey
            << ": metadata=" << (status.metadataPresent ? "yes" : "no")
            << ", secret=" << (status.secretPresent ? "yes" : "no")
            << ", validation=" << status.validationState
            << ", ready=" << (status.ready ? "yes" : "no");
        if (!status.lastValidationError.empty()) {
            std::cout << ", error=" << status.lastValidationError;
        }
        std::cout << "\n";
    }
}

int runMiniMaxUtility(const CommandLineOptions& options) {
#ifdef _WIN32
    attachParentConsoleForCli();
#endif
    JiStats::MiniMaxLiveCapture capture;
    if (options.miniMaxReadiness) {
        const auto statuses = capture.inspectStoredPlans(options.dbPath, options.validateLive);
        if (statuses.empty()) {
            const std::string error = capture.getLastError();
            std::cerr
                << (error.empty()
                    ? "No MiniMax credential metadata was found in the configured JiStats database."
                    : error)
                << "\n";
            return 2;
        }
        printMiniMaxReadiness(statuses);
        const bool allReady = std::all_of(
            statuses.begin(),
            statuses.end(),
            [](const JiStats::MiniMaxCapturePlanStatus& status) {
                return status.ready;
            });
        return allReady ? 0 : 2;
    }

    const JiStats::MiniMaxCaptureResult result =
        capture.capturePlan(options.dbPath, options.planKey, options.validateLive);
    if (!result.success) {
        std::cerr
            << (result.error.empty() ? "MiniMax capture failed." : result.error)
            << "\n";
        return 1;
    }

    std::string writeError;
    if (!capture.writeCaptureJson(result, options.outputPath, writeError)) {
        std::cerr << writeError << "\n";
        return 1;
    }

    std::cout << "Wrote MiniMax capture to " << options.outputPath << "\n";
    return 0;
}

#ifdef _WIN32
bool hasBackgroundFlagWindows() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }

    bool background = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && isBackgroundArgument(argv[index])) {
            background = true;
            break;
        }
    }

    LocalFree(argv);
    return background;
}
#endif

int runApplication(const CommandLineOptions& options) {
    if (options.miniMaxReadiness || options.miniMaxCapture) {
        return runMiniMaxUtility(options);
    }

#ifdef _WIN32
    SetCurrentProcessExplicitAppUserModelID(L"JiStats");
#endif

    JiStats::AppController app;

    if (!app.init(options.launchInBackground)) {
        return showStartupError("JiStats failed to initialize the dashboard.");
    }

    return app.run();
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.emplace_back(argv[index] ? argv[index] : "");
        }

        CommandLineOptions options;
        std::string error;
        if (!parseCommandLineOptions(args, options, error)) {
            return showStartupError(error);
        }

        return runApplication(options);
    } catch (const std::exception& e) {
        return showStartupError(std::string("JiStats terminated unexpectedly: ") + e.what());
    } catch (...) {
        return showStartupError("JiStats terminated unexpectedly with an unknown error.");
    }
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr) {
            return showStartupError("Failed to parse command line.");
        }

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.push_back(wideToUtf8(argv[index], wcslen(argv[index])));
        }
        LocalFree(argv);

        CommandLineOptions options;
        std::string error;
        if (!parseCommandLineOptions(args, options, error)) {
            return showStartupError(error);
        }

        if (options.launchInBackground && !options.miniMaxCapture && !options.miniMaxReadiness) {
            options.launchInBackground = true;
        }
        return runApplication(options);
    } catch (const std::exception& e) {
        return showStartupError(std::string("JiStats terminated unexpectedly: ") + e.what());
    } catch (...) {
        return showStartupError("JiStats terminated unexpectedly with an unknown error.");
    }
}
#endif
