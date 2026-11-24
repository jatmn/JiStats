#include "providers/kimi/KimiAdapter.hpp"

#include "core/HttpClient.hpp"
#include "core/JsonParser.hpp"
#include "db/StorageService.hpp"
#include "observation/ObservationPersistenceBridge.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <unistd.h>
#endif

namespace JiStats {

namespace {

struct ParsedKimiConfig {
    std::string defaultModelKey;
    std::string providerType;
    std::string providerBaseUrl;
};

struct ParsedKimiState {
    bool planMode = false;
    KimiApprovalMode approvalMode;
    int dynamicSubagentCount = 0;
    int additionalDirCount = 0;
};

std::string trimCopy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string stripTomlComment(const std::string& line) {
    bool inQuotes = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (ch == '"' && (index == 0 || line[index - 1] != '\\')) {
            inQuotes = !inQuotes;
            continue;
        }

        if (ch == '#' && !inQuotes) {
            return line.substr(0, index);
        }
    }

    return line;
}

std::string stripOptionalQuotes(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }

    return value;
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalizeTomlSectionModelKey(const std::string& sectionName) {
    static constexpr const char* kPrefix = "models.";
    if (sectionName.rfind(kPrefix, 0) != 0) {
        return {};
    }

    return stripOptionalQuotes(sectionName.substr(std::char_traits<char>::length(kPrefix)));
}

bool readFileToString(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    content = buffer.str();
    return true;
}

bool parseJsonFile(const std::filesystem::path& path, JsonValue& root) {
    std::string content;
    return readFileToString(path, content) && JsonParser::parse(content, root);
}

std::string tryGetEnvironmentVariable(const char* name);

bool tryGetStringField(const JsonValue& object, const char* fieldName, std::string& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !value->isString()) {
        return false;
    }

    out = value->getString();
    return true;
}

bool tryGetBooleanField(const JsonValue& object, const char* fieldName, bool& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value || !value->isBoolean()) {
        return false;
    }

    out = value->getBoolean();
    return true;
}

bool tryGetIntegralValue(const JsonValue& value, long long& out) {
    if (!value.isNumber()) {
        return false;
    }

    const double number = value.getNumber();
    if (!std::isfinite(number)) {
        return false;
    }

    double integerPart = 0.0;
    if (std::modf(number, &integerPart) != 0.0) {
        return false;
    }

    if (integerPart < static_cast<double>(std::numeric_limits<long long>::min()) ||
        integerPart >= static_cast<double>(std::numeric_limits<long long>::max())) {
        return false;
    }

    out = static_cast<long long>(integerPart);
    return true;
}

bool tryGetIntegralField(const JsonValue& object, const char* fieldName, long long& out) {
    const JsonValue* value = object.getMember(fieldName);
    return value && tryGetIntegralValue(*value, out);
}

bool tryParseZuluTimestamp(const std::string& value, std::time_t& out) {
    if (value.empty()) {
        return false;
    }

    bool numericOnly = true;
    int dotCount = 0;
    for (char ch : value) {
        if (ch == '.') {
            ++dotCount;
        } else if (!std::isdigit(static_cast<unsigned char>(ch))) {
            numericOnly = false;
            break;
        }
    }

    if (numericOnly && dotCount <= 1) {
        try {
            const double d = std::stod(value);
            if (!std::isfinite(d) || d <= 0.0) {
                return false;
            }
            const long long numeric = static_cast<long long>(d);
            out = numeric > 100000000000LL
                ? static_cast<std::time_t>(numeric / 1000LL)
                : static_cast<std::time_t>(numeric);
            return out > 0;
        } catch (...) {
            return false;
        }
    }

    std::string normalized = value;
    const std::size_t decimalPoint = normalized.find('.');
    const std::size_t zSuffix = normalized.rfind('Z');
    if (decimalPoint != std::string::npos && zSuffix != std::string::npos && decimalPoint < zSuffix) {
        normalized.erase(decimalPoint, zSuffix - decimalPoint);
    }

    std::tm utcTime{};
    std::istringstream stream(normalized);
    stream >> std::get_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    if (stream.fail()) {
        return false;
    }

#ifdef _WIN32
    out = _mkgmtime(&utcTime);
#else
    out = timegm(&utcTime);
#endif
    return out != static_cast<std::time_t>(-1);
}

bool tryGetTimeField(const JsonValue& object, const char* fieldName, std::time_t& out) {
    const JsonValue* value = object.getMember(fieldName);
    if (!value) {
        return false;
    }

    if (value->isString()) {
        return tryParseZuluTimestamp(value->getString(), out);
    }

    if (!value->isNumber()) {
        return false;
    }

    const double number = value->getNumber();
    if (!std::isfinite(number) || number <= 0.0) {
        return false;
    }

    const long long numeric = static_cast<long long>(number);
    out = numeric > 100000000000LL
        ? static_cast<std::time_t>(numeric / 1000LL)
        : static_cast<std::time_t>(numeric);
    return out > 0;
}

int countBoundedValue(const JsonValue* value) {
    if (!value) {
        return 0;
    }

    if (value->isArray()) {
        return static_cast<int>(value->getArray().size());
    }

    if (value->isObject()) {
        return static_cast<int>(value->getObject().size());
    }

    if (value->isBoolean()) {
        return value->getBoolean() ? 1 : 0;
    }

    long long numeric = 0;
    if (tryGetIntegralValue(*value, numeric) && numeric >= 0 &&
        numeric <= static_cast<long long>(std::numeric_limits<int>::max())) {
        return static_cast<int>(numeric);
    }

    return 0;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\r': escaped << "\\r"; break;
        case '\n': escaped << "\\n"; break;
        case '\t': escaped << "\\t"; break;
        default:
            escaped << ch;
            break;
        }
    }
    return escaped.str();
}

std::string resolveEffectiveRootPath(const std::string& configuredRootPath) {
    return configuredRootPath.empty() ? KimiAdapter::resolveDefaultRootPath() : configuredRootPath;
}

struct KimiOAuthCredentials {
    std::string accessToken;
    std::string refreshToken;
    std::string scope;
    std::string tokenType;
    double expiresAt = 0.0;
    double expiresIn = 0.0;
};

struct ProviderQuotaFetchResult {
    QuotaResponse response;
    int statusCode = 0;
};

constexpr const char* kKimiOAuthClientId = "17e5f671-d194-4dfb-9706-5516cb48c098";
constexpr const char* kKimiOAuthPlatform = "kimi_cli";
constexpr const char* kKimiOAuthVersion = "1.37.0";
constexpr const char* kDefaultKimiUsagesUrl = "https://api.kimi.com/coding/v1/usages";
constexpr const char* kDefaultKimiOAuthTokenUrl = "https://auth.kimi.com/api/oauth/token";

std::string makeAsciiSafe(std::string value, const std::string& fallback) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 32 || ch > 126;
    }), value.end());
    value = trimCopy(value);
    return value.empty() ? fallback : value;
}

std::string detectWindowsDeviceModel() {
#if defined(_M_X64) || defined(__x86_64__)
    return "Windows x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "Windows arm64";
#else
    return "Windows";
#endif
}

std::string readOrCreateDeviceId(const std::filesystem::path& rootPath) {
    const std::filesystem::path path = rootPath / "device_id";
    std::string existing;
    if (readFileToString(path, existing)) {
        existing = trimCopy(existing);
        if (!existing.empty()) {
            return existing;
        }
    }

    std::ostringstream generated;
    generated << std::hex << std::time(nullptr);
    generated << std::hex << static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&rootPath));
    const std::string deviceId = generated.str();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output.is_open()) {
        output << deviceId;
    }

    return deviceId;
}

std::string resolveKimiUsagesUrl() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
    const std::string overrideUrl = tryGetEnvironmentVariable("JISTATS_KIMI_USAGES_URL");
    if (!overrideUrl.empty()) {
        const std::string value = trimCopy(overrideUrl);
        if (!value.empty()) {
            return value;
        }
    }
#endif
    return kDefaultKimiUsagesUrl;
}

std::string resolveKimiOAuthTokenUrl() {
#ifdef JISTATS_ENABLE_TEST_HOOKS
    const std::string overrideUrl = tryGetEnvironmentVariable("JISTATS_KIMI_OAUTH_TOKEN_URL");
    if (!overrideUrl.empty()) {
        const std::string value = trimCopy(overrideUrl);
        if (!value.empty()) {
            return value;
        }
    }
#endif
    return kDefaultKimiOAuthTokenUrl;
}

std::optional<KimiOAuthCredentials> readOAuthCredentials(const std::filesystem::path& rootPath) {
    const std::filesystem::path credPath = rootPath / "credentials" / "kimi-code.json";
    JsonValue root;
    if (!parseJsonFile(credPath, root) || !root.isObject()) {
        return std::nullopt;
    }

    KimiOAuthCredentials credentials;

    const JsonValue* tokenType = root.getMember("token_type");
    if (!tokenType || !tokenType->isString()) {
        return std::nullopt;
    }
    credentials.tokenType = tokenType->getString();
    if (toLowerAscii(credentials.tokenType) != "bearer") {
        return std::nullopt;
    }

    const JsonValue* accessToken = root.getMember("access_token");
    if (accessToken && accessToken->isString()) {
        credentials.accessToken = accessToken->getString();
    }

    const JsonValue* refreshToken = root.getMember("refresh_token");
    if (refreshToken && refreshToken->isString()) {
        credentials.refreshToken = refreshToken->getString();
    }

    const JsonValue* scope = root.getMember("scope");
    if (scope && scope->isString()) {
        credentials.scope = scope->getString();
    }

    const JsonValue* expiresAt = root.getMember("expires_at");
    if (expiresAt && expiresAt->isNumber()) {
        credentials.expiresAt = expiresAt->getNumber();
    }

    const JsonValue* expiresIn = root.getMember("expires_in");
    if (expiresIn && expiresIn->isNumber()) {
        credentials.expiresIn = expiresIn->getNumber();
    }

    if (credentials.accessToken.empty() && credentials.refreshToken.empty()) {
        return std::nullopt;
    }

    return credentials;
}

bool hasUsableAccessToken(const KimiOAuthCredentials& credentials) {
    if (credentials.accessToken.empty()) {
        return false;
    }

    if (credentials.expiresAt <= 0.0) {
        return true;
    }

    return credentials.expiresAt > static_cast<double>(std::time(nullptr));
}

bool hasQuotaApiCredentials(const KimiOAuthCredentials& credentials) {
    return hasUsableAccessToken(credentials) || !credentials.refreshToken.empty();
}

#ifdef _WIN32
constexpr auto kCredentialSecurity =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

bool stagedCredentialAccessMatches(HANDLE file, PSID owner, PSID group, PACL dacl,
                                   SECURITY_DESCRIPTOR_CONTROL expectedControl) {
    PSID stagedOwner = nullptr;
    PSID stagedGroup = nullptr;
    PACL stagedDacl = nullptr;
    PSECURITY_DESCRIPTOR stagedDescriptor = nullptr;
    const DWORD result = GetSecurityInfo(file, SE_FILE_OBJECT, kCredentialSecurity, &stagedOwner, &stagedGroup,
                                         &stagedDacl, nullptr, &stagedDescriptor);
    const std::unique_ptr<void, decltype(&LocalFree)> stagedSecurity(stagedDescriptor, LocalFree);
    SECURITY_DESCRIPTOR_CONTROL stagedControl = 0;
    DWORD revision = 0;
    constexpr auto inheritance = SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED;
    if (result != ERROR_SUCCESS || !stagedOwner || !stagedGroup || !stagedDacl || !EqualSid(owner, stagedOwner) ||
        !EqualSid(group, stagedGroup) || !GetSecurityDescriptorControl(stagedDescriptor, &stagedControl, &revision) ||
        (expectedControl & inheritance) != (stagedControl & inheritance)) {
        return false;
    }

    ACL_SIZE_INFORMATION expectedSize{}, stagedSize{};
    return GetAclInformation(dacl, &expectedSize, sizeof(expectedSize), AclSizeInformation) &&
           GetAclInformation(stagedDacl, &stagedSize, sizeof(stagedSize), AclSizeInformation) &&
           dacl->AclRevision == stagedDacl->AclRevision && expectedSize.AceCount == stagedSize.AceCount &&
           expectedSize.AclBytesInUse == stagedSize.AclBytesInUse &&
           std::memcmp(reinterpret_cast<const unsigned char*>(dacl) + sizeof(ACL),
                       reinterpret_cast<const unsigned char*>(stagedDacl) + sizeof(ACL),
                       expectedSize.AclBytesInUse - sizeof(ACL)) == 0;
}
#endif

bool replaceCredentialFile(const std::filesystem::path& path, const std::string& contents) {
#ifdef _WIN32
    if (contents.size() > MAXDWORD) {
        return false;
    }

    const DWORD originalAttributes = GetFileAttributesW(path.c_str());
    if (originalAttributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const bool encrypted = (originalAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0;

    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR original = nullptr;
    const DWORD securityResult = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, kCredentialSecurity, &owner,
                                                       &group, &dacl, nullptr, &original);
    const std::unique_ptr<void, decltype(&LocalFree)> originalSecurity(original, LocalFree);
    SECURITY_DESCRIPTOR_CONTROL originalControl = 0;
    DWORD revision = 0;
    if (securityResult != ERROR_SUCCESS || !owner || !group || !dacl ||
        !GetSecurityDescriptorControl(original, &originalControl, &revision)) {
        return false;
    }

    PACL creationDacl = dacl;
    std::vector<unsigned char> explicitEntries;
    if (!(originalControl & SE_DACL_PROTECTED)) {
        // Regenerate inherited entries from the parent without turning them into permanent grants.
        const auto* originalAcl = reinterpret_cast<const unsigned char*>(dacl);
        explicitEntries.assign(originalAcl, originalAcl + dacl->AclSize);
        creationDacl = reinterpret_cast<PACL>(explicitEntries.data());
        for (DWORD count = dacl->AceCount; count > 0; --count) {
            void* ace = nullptr;
            if (!GetAce(dacl, count - 1, &ace) || ((static_cast<const ACE_HEADER*>(ace)->AceFlags & INHERITED_ACE) &&
                                                   !DeleteAce(creationDacl, count - 1))) {
                return false;
            }
        }
    }

    SECURITY_DESCRIPTOR creationSecurity{};
    if (!InitializeSecurityDescriptor(&creationSecurity, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&creationSecurity, owner, FALSE) ||
        !SetSecurityDescriptorGroup(&creationSecurity, group, FALSE) ||
        !SetSecurityDescriptorDacl(&creationSecurity, TRUE, creationDacl, FALSE) ||
        !SetSecurityDescriptorControl(&creationSecurity, SE_DACL_PROTECTED, originalControl & SE_DACL_PROTECTED)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &creationSecurity, FALSE};

    FILE_RENAME_INFO rename{};
    rename.ReplaceIfExists = TRUE;
    rename.FileNameLength = static_cast<DWORD>(path.native().size() * sizeof(wchar_t));
    constexpr auto nameOffset = offsetof(FILE_RENAME_INFO, FileName);
    const auto nameSize = rename.FileNameLength + sizeof(wchar_t);
    std::vector<unsigned char> renameRequest(nameOffset + nameSize);
    std::memcpy(renameRequest.data(), &rename, nameOffset);
    std::memcpy(renameRequest.data() + nameOffset, path.c_str(), nameSize);

    static std::atomic<unsigned long> sequence{0};
    std::filesystem::path staged;
    HANDLE file;
    do {
        staged = path;
        staged += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        file = CreateFileW(staged.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE, 0, &attributes,
                           CREATE_NEW, encrypted ? FILE_ATTRIBUTE_ENCRYPTED : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
            return false;
        }
    } while (file == INVALID_HANDLE_VALUE);

    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(contents.size());
    BY_HANDLE_FILE_INFORMATION stagedInfo{};
    const bool encryptionPreserved = !encrypted || (GetFileInformationByHandle(file, &stagedInfo) &&
                                                    (stagedInfo.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0);
    const bool saved =
        encryptionPreserved && stagedCredentialAccessMatches(file, owner, group, dacl, originalControl) &&
        WriteFile(file, contents.data(), size, &written, nullptr) && written == size && FlushFileBuffers(file) &&
        SetFileInformationByHandle(file, FileRenameInfo, renameRequest.data(),
                                   static_cast<DWORD>(renameRequest.size()));
    if (!saved) {
        FILE_DISPOSITION_INFO removeStaged{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &removeStaged, sizeof(removeStaged));
    }
    CloseHandle(file);
    return saved;
#else
    std::string staged = path.string() + ".tmp-XXXXXX";
    const int descriptor = mkstemp(staged.data());
    if (descriptor == -1) {
        return false;
    }

    std::FILE* file = fdopen(descriptor, "wb");
    if (file) {
        const bool flushed = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size() &&
                             std::fflush(file) == 0 && fsync(descriptor) == 0;
        const bool closed = std::fclose(file) == 0;
        if (flushed && closed && std::rename(staged.c_str(), path.c_str()) == 0) {
            return true;
        }
    } else {
        close(descriptor);
    }
    unlink(staged.c_str());
    return false;
#endif
}

bool saveOAuthCredentials(const std::filesystem::path& rootPath, const KimiOAuthCredentials& credentials) {
    if (credentials.accessToken.empty() || credentials.tokenType.empty()) {
        return false;
    }

    std::error_code ec;
    const auto credPath = std::filesystem::canonical(rootPath / "credentials" / "kimi-code.json", ec);
    if (ec) {
        return false;
    }

    std::ostringstream output;
    output << "{\n";
    output << "  \"access_token\": \"" << jsonEscape(credentials.accessToken) << "\",\n";
    output << "  \"refresh_token\": \"" << jsonEscape(credentials.refreshToken) << "\",\n";
    output << "  \"scope\": \"" << jsonEscape(credentials.scope) << "\",\n";
    output << "  \"token_type\": \"" << jsonEscape(credentials.tokenType) << "\",\n";
    output << "  \"expires_in\": " << std::fixed << std::setprecision(0) << credentials.expiresIn << ",\n";
    output << "  \"expires_at\": " << std::fixed << std::setprecision(6) << credentials.expiresAt << "\n";
    output << "}\n";
    return output.good() && replaceCredentialFile(credPath, output.str());
}

std::string urlEncode(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<char>(ch);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return encoded.str();
}

KimiOAuthCredentials parseOAuthRefreshResponse(const std::string& jsonBody, std::string& error) {
    JsonValue root;
    if (!JsonParser::parse(jsonBody, root) || !root.isObject()) {
        error = "Failed to parse Kimi OAuth refresh response";
        return {};
    }

    KimiOAuthCredentials credentials;
    if (!tryGetStringField(root, "access_token", credentials.accessToken) ||
        !tryGetStringField(root, "refresh_token", credentials.refreshToken) ||
        !tryGetStringField(root, "scope", credentials.scope) ||
        !tryGetStringField(root, "token_type", credentials.tokenType)) {
        error = "Kimi OAuth refresh response is missing token fields";
        return {};
    }

    const JsonValue* expiresIn = root.getMember("expires_in");
    if (!expiresIn || !expiresIn->isNumber()) {
        error = "Kimi OAuth refresh response is missing expires_in";
        return {};
    }

    credentials.expiresIn = expiresIn->getNumber();
    credentials.expiresAt = static_cast<double>(std::time(nullptr)) + credentials.expiresIn;
    return credentials;
}

std::optional<KimiOAuthCredentials> refreshOAuthCredentials(
    const std::filesystem::path& rootPath,
    const KimiOAuthCredentials& current,
    std::string& error) {
    if (current.refreshToken.empty()) {
        error = "Kimi quota API credentials are not available";
        return std::nullopt;
    }

    HttpClient client;
    client.setTimeout(10000);
    client.setContentType("application/x-www-form-urlencoded");
    client.setHeader("X-Msh-Platform", kKimiOAuthPlatform);
    client.setHeader("X-Msh-Version", kKimiOAuthVersion);
    const std::string computerName = tryGetEnvironmentVariable("COMPUTERNAME");
    client.setHeader("X-Msh-Device-Name", makeAsciiSafe(computerName, "unknown"));
    client.setHeader("X-Msh-Device-Model", detectWindowsDeviceModel());
    const std::string osVersion = tryGetEnvironmentVariable("OS");
    client.setHeader("X-Msh-Os-Version", makeAsciiSafe(osVersion.empty() ? std::string("Windows") : osVersion, "Windows"));
    client.setHeader("X-Msh-Device-Id", makeAsciiSafe(readOrCreateDeviceId(rootPath), "unknown"));

    const std::string body =
        "client_id=" + urlEncode(kKimiOAuthClientId) +
        "&grant_type=refresh_token" +
        "&refresh_token=" + urlEncode(current.refreshToken);
    const HttpResponse httpResponse = client.post(resolveKimiOAuthTokenUrl(), body);
    if (!httpResponse.success) {
        error = "Kimi OAuth refresh failed: " + (!httpResponse.body.empty() ? httpResponse.body : httpResponse.error);
        return std::nullopt;
    }

    std::string parseError;
    KimiOAuthCredentials refreshed = parseOAuthRefreshResponse(httpResponse.body, parseError);
    if (!parseError.empty()) {
        error = parseError;
        return std::nullopt;
    }

    if (!saveOAuthCredentials(rootPath, refreshed)) {
        error = "Failed to persist refreshed Kimi OAuth credentials";
        return std::nullopt;
    }

    return refreshed;
}

bool tryParseStringNumber(const JsonValue& value, long long& out) {
    if (value.isString()) {
        try {
            out = std::stoll(value.getString());
            return out >= 0;
        } catch (...) {
            return false;
        }
    }
    if (value.isNumber()) {
        out = static_cast<long long>(value.getNumber());
        return out >= 0;
    }
    return false;
}

bool tryParseWeeklyQuotaCount(const JsonValue& value, long long& out) {
    long long number = 0;
    if (value.isString()) {
        const auto& text = value.getString();
        std::size_t parsed = 0;
        try {
            number = std::stoll(text, &parsed);
        } catch (...) {
            return false;
        }
        if (parsed != text.size()) {
            return false;
        }
    } else if (!tryGetIntegralValue(value, number)) {
        return false;
    }
    if (number < 0) {
        return false;
    }
    out = number;
    return true;
}

QuotaResponse parseUsagesResponse(const std::string& jsonBody) {
    QuotaResponse response;
    response.providerSurface = "kimi_subscription_api";

    JsonValue root;
    if (!JsonParser::parse(jsonBody, root) || !root.isObject()) {
        response.success = false;
        response.error = "Failed to parse usages response";
        return response;
    }

    const JsonValue* usage = root.getMember("usage");
    if (!usage || !usage->isObject()) {
        response.success = false;
        response.error = "Missing usage object in response";
        return response;
    }

    long long weeklyLimit = 0;
    long long weeklyUsed = 0;
    long long weeklyRemaining = 0;
    std::time_t weeklyResetTime = 0;

    if (const JsonValue* limit = usage->getMember("limit")) {
        tryParseWeeklyQuotaCount(*limit, weeklyLimit);
    }
    const JsonValue* used = usage->getMember("used");
    const JsonValue* remaining = usage->getMember("remaining");
    const bool hasWeeklyUsed = used && tryParseWeeklyQuotaCount(*used, weeklyUsed);
    const bool hasWeeklyRemaining = remaining && tryParseWeeklyQuotaCount(*remaining, weeklyRemaining);
    const bool hasWeeklyBalance = weeklyLimit > 0 && (hasWeeklyUsed || hasWeeklyRemaining);
    if (hasWeeklyBalance) {
        weeklyRemaining = hasWeeklyRemaining ? std::min(weeklyRemaining, weeklyLimit)
                                             : weeklyLimit - std::min(weeklyUsed, weeklyLimit);
        weeklyUsed = weeklyLimit - weeklyRemaining;
    }
    if (const JsonValue* resetTime = usage->getMember("resetTime")) {
        if (resetTime->isString()) {
            tryParseZuluTimestamp(resetTime->getString(), weeklyResetTime);
        }
    }

    long long fiveHourLimit = 0;
    long long fiveHourUsed = 0;
    long long fiveHourRemaining = 0;
    std::time_t fiveHourResetTime = 0;

    const JsonValue* limits = root.getMember("limits");
    if (limits && limits->isArray()) {
        for (const JsonValue& entry : limits->getArray()) {
            if (!entry.isObject()) {
                continue;
            }

            const JsonValue* window = entry.getMember("window");
            if (!window || !window->isObject()) {
                continue;
            }

            const JsonValue* duration = window->getMember("duration");
            if (!duration || !duration->isNumber() || static_cast<int>(duration->getNumber()) != 300) {
                continue;
            }

            const JsonValue* detail = entry.getMember("detail");
            if (!detail || !detail->isObject()) {
                continue;
            }

            if (const JsonValue* limit = detail->getMember("limit")) {
                tryParseStringNumber(*limit, fiveHourLimit);
            }
            if (const JsonValue* used = detail->getMember("used")) {
                tryParseStringNumber(*used, fiveHourUsed);
            }
            if (const JsonValue* remaining = detail->getMember("remaining")) {
                tryParseStringNumber(*remaining, fiveHourRemaining);
            }
            if (const JsonValue* resetTime = detail->getMember("resetTime")) {
                if (resetTime->isString()) {
                    tryParseZuluTimestamp(resetTime->getString(), fiveHourResetTime);
                }
            }
            break;
        }
    }

    response.success = true;
    response.windowType = "interval";
    response.windowStart = fiveHourResetTime > 0
        ? std::max<std::time_t>(0, fiveHourResetTime - (5 * 60 * 60))
        : 0;
    response.windowEnd = fiveHourResetTime;
    response.used = fiveHourUsed;
    response.limit = fiveHourLimit;
    response.remaining = fiveHourRemaining;
    if (hasWeeklyBalance) {
        const auto weeklyStart =
            weeklyResetTime > 0 ? std::max<std::time_t>(0, weeklyResetTime - (7 * 24 * 60 * 60)) : 0;
        if (fiveHourLimit <= 0) {
            response.windowType = "weekly";
            response.windowStart = weeklyStart;
            response.windowEnd = weeklyResetTime;
            response.used = weeklyUsed;
            response.limit = weeklyLimit;
            response.remaining = weeklyRemaining;
        } else {
            response.secondaryWindowType = "weekly";
            response.secondaryWindowStart = weeklyStart;
            response.secondaryWindowEnd = weeklyResetTime;
            response.secondaryUsed = weeklyUsed;
            response.secondaryLimit = weeklyLimit;
            response.secondaryRemaining = weeklyRemaining;
        }
    }
    response.snapshotTime = std::time(nullptr);
    return response;
}

ProviderQuotaFetchResult fetchProviderQuota(const std::string& token) {
    ProviderQuotaFetchResult result;
    result.response.providerSurface = "kimi_subscription_api";
    HttpClient client;
    client.setApiKey(token);
    client.setTimeout(10000);

    const HttpResponse httpResponse = client.get(resolveKimiUsagesUrl());
    result.statusCode = httpResponse.statusCode;
    if (!httpResponse.success) {
        result.response.success = false;
        result.response.error = "Kimi usages API request failed: " + (!httpResponse.body.empty() ? httpResponse.body : httpResponse.error);
        return result;
    }

    result.response = parseUsagesResponse(httpResponse.body);
    return result;
}

std::string makeSanitizedReference(
    const std::string& sessionId,
    const std::string& taskId) {
    return "kimi_local_runtime:" + sessionId + "/" + taskId;
}

ParsedKimiConfig parseConfigToml(const std::filesystem::path& path) {
    ParsedKimiConfig config;
    std::string content;
    if (!readFileToString(path, content)) {
        return config;
    }

    struct ModelSectionInfo {
        std::string type;
        std::string baseUrl;
    };

    std::map<std::string, ModelSectionInfo> modelSections;
    std::istringstream stream(content);
    std::string line;
    std::string currentSection;

    while (std::getline(stream, line)) {
        line = trimCopy(stripTomlComment(line));
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            currentSection = trimCopy(line.substr(1, line.size() - 2));
            continue;
        }

        const std::size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        const std::string key = trimCopy(line.substr(0, equalsPos));
        const std::string value = stripOptionalQuotes(trimCopy(line.substr(equalsPos + 1)));

        if (currentSection.empty() && key == "default_model") {
            config.defaultModelKey = value;
            continue;
        }

        const std::string modelKey = normalizeTomlSectionModelKey(currentSection);
        if (modelKey.empty()) {
            continue;
        }

        ModelSectionInfo& modelInfo = modelSections[modelKey];
        if (key == "type") {
            modelInfo.type = value;
        } else if (key == "base_url") {
            modelInfo.baseUrl = value;
        }
    }

    auto chooseModelSection = [&](const std::string& key) -> const ModelSectionInfo* {
        const auto it = modelSections.find(key);
        if (it != modelSections.end()) {
            return &it->second;
        }

        return modelSections.empty() ? nullptr : &modelSections.begin()->second;
    };

    if (const ModelSectionInfo* selected = chooseModelSection(config.defaultModelKey)) {
        config.providerType = selected->type;
        config.providerBaseUrl = selected->baseUrl;
    }

    return config;
}

std::map<std::string, std::string> parseKimiJsonWorkDirScopes(const std::filesystem::path& path) {
    std::map<std::string, std::string> scopes;
    JsonValue root;
    if (!parseJsonFile(path, root) || !root.isObject()) {
        return scopes;
    }

    const JsonValue* workDirs = root.getMember("work_dirs");
    if (!workDirs || !workDirs->isArray()) {
        return scopes;
    }

    for (const JsonValue& entry : workDirs->getArray()) {
        if (!entry.isObject()) {
            continue;
        }

        std::string lastSessionId;
        std::string rawPath;
        if (!tryGetStringField(entry, "last_session_id", lastSessionId) ||
            !tryGetStringField(entry, "path", rawPath) ||
            lastSessionId.empty() ||
            rawPath.empty()) {
            continue;
        }

        scopes[lastSessionId] = KimiAdapter::hashLocalPathScope(rawPath);
    }

    return scopes;
}

bool directoryHasKimiSessionArtifacts(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path / "state.json", ec) ||
        std::filesystem::exists(path / "tasks", ec) ||
        std::filesystem::exists(path / "notifications", ec);
}

struct KimiSessionCandidate {
    std::string sessionRootId;
    std::string sessionId;
    std::filesystem::path path;
};

std::vector<KimiSessionCandidate> collectKimiSessionCandidates(
    const std::filesystem::path& sessionsRoot) {
    std::vector<KimiSessionCandidate> candidates;
    std::error_code ec;

    for (const std::filesystem::directory_entry& sessionRootEntry :
         std::filesystem::directory_iterator(
             sessionsRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        if (!sessionRootEntry.is_directory(ec) || ec) {
            ec.clear();
            continue;
        }

        const std::filesystem::path sessionRootPath = sessionRootEntry.path();
        const std::string sessionRootId = sessionRootPath.filename().string();
        bool foundNestedCandidate = false;

        if (directoryHasKimiSessionArtifacts(sessionRootPath)) {
            candidates.push_back({sessionRootId, sessionRootId, sessionRootPath});
            continue;
        }

        for (const std::filesystem::directory_entry& sessionEntry :
             std::filesystem::directory_iterator(
                 sessionRootPath,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec)) {
            if (ec) {
                ec.clear();
                continue;
            }

            if (!sessionEntry.is_directory(ec) || ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::path sessionPath = sessionEntry.path();
            if (!directoryHasKimiSessionArtifacts(sessionPath)) {
                continue;
            }

            foundNestedCandidate = true;
            candidates.push_back(
                {sessionRootId, sessionRootId + "/" + sessionPath.filename().string(), sessionPath});
        }

        if (!foundNestedCandidate && directoryHasKimiSessionArtifacts(sessionRootPath)) {
            candidates.push_back({sessionRootId, sessionRootId, sessionRootPath});
        }
    }

    return candidates;
}

ParsedKimiState parseSessionState(const std::filesystem::path& path) {
    ParsedKimiState state;
    JsonValue root;
    if (!parseJsonFile(path, root) || !root.isObject()) {
        return state;
    }

    tryGetBooleanField(root, "plan_mode", state.planMode);

    const JsonValue* approval = root.getMember("approval");
    if (approval && approval->isObject()) {
        tryGetBooleanField(*approval, "yolo", state.approvalMode.yolo);
        tryGetBooleanField(*approval, "auto_approve_actions", state.approvalMode.autoApproveActions);
    }

    state.dynamicSubagentCount = countBoundedValue(root.getMember("dynamic_subagents"));
    state.additionalDirCount = countBoundedValue(root.getMember("additional_dirs"));
    return state;
}

std::optional<KimiTaskRuntimeSummary> parseTaskRuntime(
    const std::filesystem::path& path) {
    JsonValue root;
    if (!parseJsonFile(path, root) || !root.isObject()) {
        return std::nullopt;
    }

    KimiTaskRuntimeSummary runtime;
    runtime.taskId = path.parent_path().filename().string();
    tryGetStringField(root, "status", runtime.status);
    tryGetTimeField(root, "started_at", runtime.startedAt);
    tryGetTimeField(root, "updated_at", runtime.updatedAt);
    tryGetTimeField(root, "finished_at", runtime.finishedAt);
    tryGetBooleanField(root, "interrupted", runtime.interrupted);
    tryGetBooleanField(root, "timed_out", runtime.timedOut);

    long long exitCode = 0;
    if (tryGetIntegralField(root, "exit_code", exitCode) &&
        exitCode >= std::numeric_limits<int>::min() &&
        exitCode <= std::numeric_limits<int>::max()) {
        runtime.exitCode = static_cast<int>(exitCode);
        runtime.hasExitCode = true;
    }

    if (runtime.status.empty() && runtime.startedAt == 0 && runtime.updatedAt == 0 &&
        runtime.finishedAt == 0) {
        return std::nullopt;
    }

    return runtime;
}

std::optional<KimiNotificationSummary> parseNotificationEvent(
    const std::filesystem::path& path) {
    JsonValue root;
    if (!parseJsonFile(path, root) || !root.isObject()) {
        return std::nullopt;
    }

    KimiNotificationSummary notification;
    notification.notificationId = path.parent_path().filename().string();
    tryGetStringField(root, "category", notification.category);
    tryGetStringField(root, "type", notification.type);
    tryGetStringField(root, "source_kind", notification.sourceKind);
    tryGetStringField(root, "severity", notification.severity);
    tryGetTimeField(root, "created_at", notification.createdAt);

    const JsonValue* payload = root.getMember("payload");
    if (payload && payload->isObject()) {
        tryGetStringField(*payload, "status", notification.payloadStatus);
        long long exitCode = 0;
        if (tryGetIntegralField(*payload, "exit_code", exitCode) &&
            exitCode >= std::numeric_limits<int>::min() &&
            exitCode <= std::numeric_limits<int>::max()) {
            notification.payloadExitCode = static_cast<int>(exitCode);
            notification.hasPayloadExitCode = true;
        }
        tryGetBooleanField(*payload, "interrupted", notification.payloadInterrupted);
        tryGetBooleanField(*payload, "timed_out", notification.payloadTimedOut);
    }

    return notification;
}

std::string parseDeliveryStatus(const std::filesystem::path& path) {
    JsonValue root;
    if (!parseJsonFile(path, root) || !root.isObject()) {
        return {};
    }

    std::string directStatus;
    if (tryGetStringField(root, "status", directStatus) && !directStatus.empty()) {
        return directStatus;
    }

    std::vector<std::string> statuses;
    for (const auto& [key, value] : root.getObject()) {
        if (!value.isObject()) {
            continue;
        }

        std::string status;
        if (tryGetStringField(value, "status", status) && !status.empty()) {
            statuses.push_back(key + ":" + status);
        }
    }

    if (statuses.empty()) {
        return {};
    }

    std::sort(statuses.begin(), statuses.end());
    std::ostringstream joined;
    for (std::size_t index = 0; index < statuses.size(); ++index) {
        if (index > 0) {
            joined << ",";
        }
        joined << statuses[index];
    }
    return joined.str();
}

const KimiNotificationSummary* chooseNotificationForTask(
    const std::vector<KimiNotificationSummary>& notifications,
    const KimiTaskRuntimeSummary& runtime) {
    const KimiNotificationSummary* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();
    std::time_t bestDistance = std::numeric_limits<std::time_t>::max();

    const std::time_t targetTime = runtime.finishedAt > 0
        ? runtime.finishedAt
        : (runtime.updatedAt > 0 ? runtime.updatedAt : runtime.startedAt);

    for (const KimiNotificationSummary& notification : notifications) {
        int score = 0;
        if (!runtime.status.empty() && notification.payloadStatus == runtime.status) {
            score += 4;
        }
        if (runtime.timedOut && notification.payloadTimedOut) {
            score += 3;
        }
        if (runtime.interrupted && notification.payloadInterrupted) {
            score += 3;
        }
        if (runtime.hasExitCode && notification.hasPayloadExitCode &&
            runtime.exitCode == notification.payloadExitCode) {
            score += 2;
        }

        std::time_t distance = std::numeric_limits<std::time_t>::max();
        if (targetTime > 0 && notification.createdAt > 0) {
            distance = std::llabs(static_cast<long long>(notification.createdAt) -
                                  static_cast<long long>(targetTime));
        }

        if (!best || score > bestScore || (score == bestScore && distance < bestDistance) ||
            (score == bestScore && distance == bestDistance &&
             notification.createdAt > best->createdAt)) {
            best = &notification;
            bestScore = score;
            bestDistance = distance;
        }
    }

    return best;
}

std::string tryGetEnvironmentVariable(const char* name) {
    if (!name || *name == '\0') {
        return {};
    }

    char* value = nullptr;
    std::size_t valueLength = 0;
    if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr) {
        return {};
    }

    const std::string resolvedValue(value);
    free(value);
    return resolvedValue;
}

std::string buildLocalSummaryJson(const KimiLocalParseResult& result) {
    std::ostringstream output;
    output << "{"
           << "\"provider_key\":\"kimi\","
           << "\"provider_surface\":\"kimi_local_runtime\","
           << "\"session_count\":" << result.sessionCount << ","
           << "\"task_count\":" << result.taskCount << ","
           << "\"completed_task_count\":" << result.completedTaskCount << ","
           << "\"failed_task_count\":" << result.failedTaskCount << ","
           << "\"timed_out_task_count\":" << result.timedOutTaskCount << ","
           << "\"default_model_key\":\"" << jsonEscape(result.defaultModelKey) << "\","
           << "\"provider_type\":\"" << jsonEscape(result.providerType) << "\","
           << "\"provider_base_url\":\"" << jsonEscape(result.providerBaseUrl) << "\""
           << "}";
    return output.str();
}

} // namespace

struct KimiAdapter::Impl {
    bool enabled = true;
    std::string rootPath;
};

ActivityEvent KimiLocalActivitySnapshot::toActivityEvent() const {
    ActivityEvent event;
    event.providerKey = providerKey;
    event.providerSurface = providerSurface;
    event.activityKind = ActivityKind::SessionActive;
    event.startedAt = taskRuntime.startedAt;
    event.endedAt = taskRuntime.finishedAt > 0
        ? taskRuntime.finishedAt
        : (taskRuntime.updatedAt > 0 ? taskRuntime.updatedAt : taskRuntime.startedAt);
    if (event.endedAt < event.startedAt) {
        event.endedAt = event.startedAt;
    }
    if (event.startedAt > 0 && event.endedAt >= event.startedAt) {
        event.durationSeconds = static_cast<long long>(event.endedAt - event.startedAt);
    }
    event.sourceType = "local";
    event.sourceRunId = sessionId;
    event.surfaceKey = taskRuntime.status.empty() ? "runtime" : taskRuntime.status;
    event.windowTitleHint = workingDirScope;
    event.confidence = ActivityConfidence::LocalExact;
    event.rawReference = makeSanitizedReference(sessionId, taskId);
    return event;
}

KimiAdapter::KimiAdapter(const std::string& rootPath)
    : pImpl(std::make_unique<Impl>()) {
    pImpl->rootPath = rootPath;
}

KimiAdapter::~KimiAdapter() = default;

UsageResponse KimiAdapter::fetchUsage(std::time_t from, std::time_t to) {
    UsageResponse response;
    response.windowStart = from;
    response.windowEnd = to;

    response.error = "Kimi local runtime records do not report token usage";
    return response;
}

QuotaResponse KimiAdapter::fetchQuota() {
    QuotaResponse response;
    response.providerSurface = "kimi_subscription_api";

    const std::string resolvedRoot = resolveEffectiveRootPath(pImpl ? pImpl->rootPath : std::string());
    if (resolvedRoot.empty()) {
        response.success = false;
        response.error = "Kimi quota API credentials are not available";
        return response;
    }

    const std::filesystem::path rootPath(resolvedRoot);
    std::optional<KimiOAuthCredentials> credentials = readOAuthCredentials(rootPath);
    if (!credentials.has_value() || !hasQuotaApiCredentials(*credentials)) {
        response.success = false;
        response.error = "Kimi quota API credentials are not available";
        return response;
    }

    if (!hasUsableAccessToken(*credentials)) {
        std::string refreshError;
        credentials = refreshOAuthCredentials(rootPath, *credentials, refreshError);
        if (!credentials.has_value()) {
            response.success = false;
            response.error = refreshError.empty() ? "Kimi quota API credentials are not available" : refreshError;
            return response;
        }
    }

    ProviderQuotaFetchResult quotaResult = fetchProviderQuota(credentials->accessToken);
    if (!quotaResult.response.success &&
        quotaResult.statusCode == 401 &&
        !credentials->refreshToken.empty()) {
        std::string refreshError;
        const std::optional<KimiOAuthCredentials> refreshed =
            refreshOAuthCredentials(rootPath, *credentials, refreshError);
        if (refreshed.has_value()) {
            quotaResult = fetchProviderQuota(refreshed->accessToken);
        } else if (!refreshError.empty()) {
            quotaResult.response.error = refreshError;
        }
    }

    return quotaResult.response;
}

bool KimiAdapter::validateCredentials() {
    return hasLocalInstallation(pImpl ? pImpl->rootPath : std::string());
}

std::vector<Capability> KimiAdapter::getCapabilities() {
    const std::string resolvedRoot = resolveEffectiveRootPath(pImpl ? pImpl->rootPath : std::string());
    const bool hasCredentials = !resolvedRoot.empty() &&
        [resolvedRoot]() {
            const std::optional<KimiOAuthCredentials> credentials =
                readOAuthCredentials(std::filesystem::path(resolvedRoot));
            return credentials.has_value() && hasQuotaApiCredentials(*credentials);
        }();

    return {
        {"local_runtime_activity", "Parse bounded Kimi local runtime artifacts", true,
            "Approved inputs only: config.toml, kimi.json, state.json, runtime.json, notification JSON"},
        {"provider_exact_quota", "Provider-exact Kimi quota mirroring", hasCredentials,
            hasCredentials
                ? "Uses Kimi /usages API when OAuth credentials or refresh tokens are available"
                : "Blocked until OAuth credentials are available"}
    };
}

std::string KimiAdapter::getProviderKey() const {
    return "kimi";
}

std::string KimiAdapter::getDisplayName() const {
    return "Kimi Code";
}

bool KimiAdapter::isEnabled() const {
    return pImpl ? pImpl->enabled : true;
}

void KimiAdapter::setEnabled(bool enabled) {
    if (pImpl) {
        pImpl->enabled = enabled;
    }
}

bool KimiAdapter::hasDirectQuotaApiConfigured() const {
    const std::string resolvedRoot = resolveEffectiveRootPath(pImpl ? pImpl->rootPath : std::string());
    if (resolvedRoot.empty()) {
        return false;
    }
    const std::optional<KimiOAuthCredentials> credentials =
        readOAuthCredentials(std::filesystem::path(resolvedRoot));
    return credentials.has_value() && hasQuotaApiCredentials(*credentials);
}

std::string KimiAdapter::directQuotaApiThrottleKey() const {
    return hasDirectQuotaApiConfigured() ? "kimi:/usages" : std::string();
}

KimiLocalParseResult KimiAdapter::fetchLocalActivity() const {
    return parseLocalState(pImpl ? pImpl->rootPath : std::string());
}

KimiLocalParseResult KimiAdapter::parseLocalState(const std::string& rootPath) {
    KimiLocalParseResult result;
    result.rootPath = rootPath.empty() ? resolveDefaultRootPath() : rootPath;

    if (result.rootPath.empty()) {
        result.error = "Unable to determine the Kimi local root path";
        return result;
    }

    std::error_code ec;
    const std::filesystem::path root(result.rootPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        result.error = "Kimi local root path does not exist";
        return result;
    }

    const ParsedKimiConfig config = parseConfigToml(root / "config.toml");
    result.defaultModelKey = config.defaultModelKey;
    result.providerType = config.providerType;
    result.providerBaseUrl = config.providerBaseUrl;

    const std::map<std::string, std::string> workDirScopes =
        parseKimiJsonWorkDirScopes(root / "kimi.json");

    const std::filesystem::path sessionsRoot = root / "sessions";
    if (!std::filesystem::exists(sessionsRoot, ec) || !std::filesystem::is_directory(sessionsRoot, ec)) {
        result.error = "Kimi sessions directory does not exist";
        return result;
    }

    for (const KimiSessionCandidate& candidate : collectKimiSessionCandidates(sessionsRoot)) {
        const std::filesystem::path sessionPath = candidate.path;
        const std::string sessionRootId = candidate.sessionRootId;
        const std::string sessionId = candidate.sessionId;
        const ParsedKimiState state = parseSessionState(sessionPath / "state.json");

        std::vector<KimiTaskRuntimeSummary> runtimes;
        const std::filesystem::path tasksRoot = sessionPath / "tasks";
        if (std::filesystem::exists(tasksRoot, ec) && std::filesystem::is_directory(tasksRoot, ec)) {
            for (const std::filesystem::directory_entry& taskEntry :
                 std::filesystem::directory_iterator(tasksRoot, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }

                if (!taskEntry.is_directory(ec) || ec) {
                    ec.clear();
                    continue;
                }

                if (const std::optional<KimiTaskRuntimeSummary> runtime =
                        parseTaskRuntime(taskEntry.path() / "runtime.json")) {
                    runtimes.push_back(*runtime);
                }
            }
        }

        std::vector<KimiNotificationSummary> notifications;
        const std::filesystem::path notificationsRoot = sessionPath / "notifications";
        if (std::filesystem::exists(notificationsRoot, ec) &&
            std::filesystem::is_directory(notificationsRoot, ec)) {
            for (const std::filesystem::directory_entry& notificationEntry :
                 std::filesystem::directory_iterator(
                     notificationsRoot,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }

                if (!notificationEntry.is_directory(ec) || ec) {
                    ec.clear();
                    continue;
                }

                std::optional<KimiNotificationSummary> notification =
                    parseNotificationEvent(notificationEntry.path() / "event.json");
                if (!notification.has_value()) {
                    continue;
                }

                notification->deliveryStatus =
                    parseDeliveryStatus(notificationEntry.path() / "delivery.json");
                notifications.push_back(*notification);
            }
        }

        if (runtimes.empty()) {
            continue;
        }

        ++result.sessionCount;
        const auto scopeIt = workDirScopes.find(sessionId);
        const auto rootScopeIt = workDirScopes.find(sessionRootId);
        const std::string workingDirScope =
            scopeIt != workDirScopes.end()
                ? scopeIt->second
                : (rootScopeIt == workDirScopes.end() ? std::string() : rootScopeIt->second);

        for (const KimiTaskRuntimeSummary& runtime : runtimes) {
            KimiLocalActivitySnapshot snapshot;
            snapshot.sessionRootId = sessionRootId;
            snapshot.sessionId = sessionId;
            snapshot.taskId = runtime.taskId;
            snapshot.workingDirScope = workingDirScope;
            snapshot.modelKey = result.defaultModelKey;
            snapshot.providerType = result.providerType;
            snapshot.providerBaseUrl = result.providerBaseUrl;
            snapshot.planMode = state.planMode;
            snapshot.approvalMode = state.approvalMode;
            snapshot.dynamicSubagentCount = state.dynamicSubagentCount;
            snapshot.additionalDirCount = state.additionalDirCount;
            snapshot.taskRuntime = runtime;
            snapshot.observedAt = runtime.finishedAt > 0
                ? runtime.finishedAt
                : (runtime.updatedAt > 0 ? runtime.updatedAt : runtime.startedAt);
            snapshot.confidenceNotes = {
                "Derived from approved local Kimi runtime artifacts",
                "LocalExact for local activity and runtime metadata only",
                "Not provider-exact Kimi quota truth without corroboration"
            };

            if (const KimiNotificationSummary* notification =
                    chooseNotificationForTask(notifications, runtime)) {
                snapshot.notificationSummary = *notification;
            }

            result.activitySnapshots.push_back(snapshot);
            result.normalizedActivityEvents.push_back(snapshot.toActivityEvent());
            ++result.taskCount;

            if (runtime.timedOut) {
                ++result.timedOutTaskCount;
            }

            const std::string loweredStatus = toLowerAscii(runtime.status);
            if (loweredStatus == "completed" || loweredStatus == "succeeded" ||
                loweredStatus == "success") {
                ++result.completedTaskCount;
            } else if (!loweredStatus.empty() && loweredStatus != "running" &&
                       loweredStatus != "queued" && loweredStatus != "pending") {
                ++result.failedTaskCount;
            }

            if (snapshot.observedAt > 0) {
                if (result.earliestObservedAt == 0 || snapshot.observedAt < result.earliestObservedAt) {
                    result.earliestObservedAt = snapshot.observedAt;
                }
                if (result.latestObservedAt == 0 || snapshot.observedAt > result.latestObservedAt) {
                    result.latestObservedAt = snapshot.observedAt;
                }
            }
        }
    }

    std::sort(result.activitySnapshots.begin(), result.activitySnapshots.end(),
        [](const KimiLocalActivitySnapshot& left, const KimiLocalActivitySnapshot& right) {
            if (left.observedAt != right.observedAt) {
                return left.observedAt < right.observedAt;
            }
            if (left.sessionId != right.sessionId) {
                return left.sessionId < right.sessionId;
            }
            return left.taskId < right.taskId;
        });

    std::sort(result.normalizedActivityEvents.begin(), result.normalizedActivityEvents.end(),
        [](const ActivityEvent& left, const ActivityEvent& right) {
            if (left.startedAt != right.startedAt) {
                return left.startedAt < right.startedAt;
            }
            return left.rawReference < right.rawReference;
        });

    if (result.activitySnapshots.empty()) {
        result.error = "No approved Kimi runtime snapshots were found";
        return result;
    }

    result.success = true;
    result.rawSummary = buildLocalSummaryJson(result);
    return result;
}

bool KimiAdapter::importLocalActivity(StorageService& storage, const std::string& rootPath) {
    const KimiLocalParseResult result = parseLocalState(rootPath);
    if (!result.success) {
        return false;
    }

    ObservationPersistenceBridge bridge(storage);
    bool anyPersisted = false;
    for (const ActivityEvent& event : result.normalizedActivityEvents) {
        if (bridge.persist(event)) {
            anyPersisted = true;
        }
    }

    return anyPersisted || result.normalizedActivityEvents.empty();
}

bool KimiAdapter::hasLocalInstallation(const std::string& rootPath) {
    const std::string resolvedRoot = rootPath.empty() ? resolveDefaultRootPath() : rootPath;
    if (resolvedRoot.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path root(resolvedRoot);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return false;
    }

    const std::filesystem::path configPath = root / "config.toml";
    const std::filesystem::path kimiJsonPath = root / "kimi.json";
    const std::filesystem::path sessionsPath = root / "sessions";
    return std::filesystem::exists(configPath, ec) ||
        std::filesystem::exists(kimiJsonPath, ec) ||
        std::filesystem::exists(sessionsPath, ec);
}

std::string KimiAdapter::hashLocalPathScope(const std::string& rawPath) {
    if (rawPath.empty()) {
        return {};
    }

    std::filesystem::path path(rawPath);
    std::error_code ec;
    path = path.lexically_normal();
    std::string normalized = toLowerAscii(path.generic_string());
    if (normalized.empty()) {
        normalized = toLowerAscii(rawPath);
    }

    constexpr unsigned long long kOffsetBasis = 14695981039346656037ull;
    constexpr unsigned long long kPrime = 1099511628211ull;
    unsigned long long hash = kOffsetBasis;
    for (unsigned char ch : normalized) {
        hash ^= ch;
        hash *= kPrime;
    }

    std::ostringstream output;
    output << "scope:fnv1a64:" << std::hex << std::nouppercase << std::setw(16)
           << std::setfill('0') << hash;
    return output.str();
}

#ifdef JISTATS_ENABLE_TEST_HOOKS
QuotaResponse KimiAdapter::parseUsagesResponseForTesting(const std::string& jsonBody) {
    return parseUsagesResponse(jsonBody);
}

std::string KimiAdapter::resolveEffectiveRootPathForTesting(const std::string& configuredRootPath) {
    return resolveEffectiveRootPath(configuredRootPath);
}
#endif

std::string KimiAdapter::resolveDefaultRootPath() {
    const std::string kimiHome = tryGetEnvironmentVariable("KIMI_HOME");
    if (!kimiHome.empty()) {
        return kimiHome;
    }

    const std::string userProfile = tryGetEnvironmentVariable("USERPROFILE");
    if (!userProfile.empty()) {
        return (std::filesystem::path(userProfile) / ".kimi").string();
    }

    const std::string home = tryGetEnvironmentVariable("HOME");
    if (!home.empty()) {
        return (std::filesystem::path(home) / ".kimi").string();
    }

    return {};
}

} // namespace JiStats
