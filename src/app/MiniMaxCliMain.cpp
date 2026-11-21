#include "providers/minimax/MiniMaxLiveCapture.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
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

struct CommandLineOptions {
    bool miniMaxReadiness = false;
    bool miniMaxCapture = false;
    bool miniMaxDiagnostics = false;
    bool miniMaxStoreSecret = false;
    bool validateLive = true;
    std::string dbPath = "jistats.db";
    std::string planKey;
    std::string outputPath;
    std::string secret;
};

bool requiresValue(const std::string& arg) {
    return arg == "--db-path" ||
        arg == "--minimax-capture-plan" ||
        arg == "--minimax-output" ||
        arg == "--minimax-store-secret" ||
        arg == "--minimax-secret";
}

bool parseCommandLineOptions(const std::vector<std::string>& args,
                             CommandLineOptions& options,
                             std::string& error) {
    for (size_t index = 1; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--minimax-readiness") {
            options.miniMaxReadiness = true;
            continue;
        }
        if (arg == "--minimax-diagnostics") {
            options.miniMaxDiagnostics = true;
            continue;
        }
        if (arg == "--minimax-store-secret-mode") {
            options.miniMaxStoreSecret = true;
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
            } else if (arg == "--minimax-store-secret") {
                options.miniMaxStoreSecret = true;
                options.planKey = value;
            } else if (arg == "--minimax-secret") {
                options.secret = value;
            }
            continue;
        }

        error = "Unknown argument: " + arg;
        return false;
    }

    const int selectedModes =
        (options.miniMaxReadiness ? 1 : 0) +
        (options.miniMaxCapture ? 1 : 0) +
        (options.miniMaxDiagnostics ? 1 : 0) +
        (options.miniMaxStoreSecret ? 1 : 0);
    if (selectedModes > 1) {
        error = "MiniMax helper modes are mutually exclusive; choose exactly one of --minimax-readiness, --minimax-capture-plan, --minimax-diagnostics, or --minimax-store-secret";
        return false;
    }
    if (selectedModes == 0) {
        error = "Choose one of --minimax-readiness, --minimax-capture-plan, --minimax-diagnostics, or --minimax-store-secret";
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
    if (options.miniMaxDiagnostics && options.outputPath.empty()) {
        error = "MiniMax diagnostics requires --minimax-output <path>";
        return false;
    }
    if (options.miniMaxStoreSecret) {
        if (!JiStats::MiniMaxLiveCapture::isSupportedPlanKey(options.planKey)) {
            error = "MiniMax secret storage requires --minimax-store-secret coding_plan|token_plan";
            return false;
        }
        if (options.secret.empty()) {
            error = "MiniMax secret storage requires --minimax-secret <value>";
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

    if (options.miniMaxDiagnostics) {
        const JiStats::MiniMaxDiagnosticReport report =
            capture.collectDiagnostics(options.dbPath, options.validateLive);
        std::string writeError;
        if (!capture.writeDiagnosticJson(report, options.outputPath, writeError)) {
            std::cerr << writeError << "\n";
            return 1;
        }

        std::cout << "Wrote MiniMax diagnostics to " << options.outputPath << "\n";
        const bool allReady = std::all_of(
            report.planStatuses.begin(),
            report.planStatuses.end(),
            [](const JiStats::MiniMaxCapturePlanStatus& status) {
                return status.ready;
            });
        return allReady ? 0 : 2;
    }

    if (options.miniMaxStoreSecret) {
        std::string storeError;
        if (!capture.storePlanSecret(options.dbPath, options.planKey, options.secret, storeError)) {
            std::cerr << storeError << "\n";
            return 1;
        }

        std::cout << "Stored MiniMax secret for " << options.planKey << "\n";
        return 0;
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
            std::cerr << error << "\n";
            return 1;
        }

        return runMiniMaxUtility(options);
    } catch (const std::exception& e) {
        std::cerr << "JiStats MiniMax helper terminated unexpectedly: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "JiStats MiniMax helper terminated unexpectedly.\n";
        return 1;
    }
}
