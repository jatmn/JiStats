#include "providers/SubscriptionQuotaAdapter.hpp"
#include "core/HttpClient.hpp"
#include "core/JsonParser.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>

namespace JiStats {
namespace {

constexpr auto kGrokQuotaUrl = "https://cli-chat-proxy.grok.com/v1/billing?format=credits";
constexpr auto kCursorExchangeUrl = "https://api2.cursor.sh/auth/exchange_user_api_key";
constexpr auto kCursorQuotaUrl = "https://api2.cursor.sh/aiserver.v1.DashboardService/GetCurrentPeriodUsage";

std::string env(const char* name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) {
        return {};
    }
    std::string result(value);
    std::free(value);
    const auto first = result.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : result.substr(first, result.find_last_not_of(" \t\r\n") - first + 1);
}

const JsonValue* field(const JsonValue* object, const char* name) {
    return object && object->isObject() ? object->getMember(name) : nullptr;
}

std::string stringField(const JsonValue* object, const char* name) {
    const auto* value = field(object, name);
    return value && value->isString() ? value->getString() : std::string{};
}

bool usableBearer(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch > 32 && ch < 127; });
}

std::string grokToken() {
    for (const auto* setting :
         {"XAI_API_KEY", "GROK_CONFIG", "GROK_CONFIG_PATH", "GROK_CLI_CHAT_PROXY_BASE_URL", "GROK_MODELS_BASE_URL",
          "GROK_OIDC_ISSUER", "GROK_OIDC_CLIENT_ID", "GROK_OAUTH2_ISSUER", "GROK_OAUTH2_CLIENT_ID",
          "GROK_OAUTH2_PRINCIPAL_TYPE", "GROK_OAUTH2_PRINCIPAL_ID", "GROK_AUTH_PROVIDER_COMMAND", "GROK_LOCAL_AUTH"}) {
        if (!env(setting).empty()) {
            return {};
        }
    }

    std::filesystem::path home = env("GROK_HOME");
    if (home.empty()) {
        auto profile = env("USERPROFILE");
        if (profile.empty()) {
            profile = env("HOME");
        }
        if (profile.empty()) {
            return {};
        }
        home = std::filesystem::path(profile) / ".grok";
    }

    static const std::regex overrides(R"(^\s*(?:\[\[?\s*)?["']?(?:auth|grok_com_config|endpoints)["']?\s*[.\]=])");
    for (const auto* file : {"config.toml", "managed_config.toml", "requirements.toml"}) {
        const auto path = home / file;
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            return {};
        }
        if (!exists) {
            continue;
        }
        std::ifstream config(path);
        std::string line;
        while (std::getline(config, line)) {
            if (std::regex_search(line, overrides)) {
                return {};
            }
        }
        if (!config.eof()) {
            return {};
        }
    }

    auto contents = env("GROK_AUTH");
    if (contents.empty()) {
        std::ifstream file(home / "auth.json", std::ios::binary);
        if (!file) {
            return {};
        }
        // The CLI credential store is small. Bound the read even when the file has been replaced.
        contents.resize(1024 * 1024 + 1);
        file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file.eof() || file.bad()) {
            return {};
        }
        contents.resize(static_cast<size_t>(file.gcount()));
    }
    JsonValue root;
    if (!JsonParser::parse(contents, root) || !root.isObject()) {
        return {};
    }
    auto* account = root.getMember("https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828");
    if (!account) {
        account = root.getMember("https://accounts.x.ai/sign-in");
    }
    const auto token = stringField(account, "key");
    return stringField(account, "auth_mode") != "api_key" && usableBearer(token) ? token : std::string{};
}

bool cursorDefaultEndpoint() {
    const auto endpoint = env("CURSOR_API_ENDPOINT");
    return endpoint.empty() || endpoint == "https://api2.cursor.sh" || endpoint == "https://api2.cursor.sh/";
}

std::optional<long long> readPercent(const JsonValue* object, const char* member) {
    const auto* value = field(object, member);
    if (!value || !value->isNumber() || !std::isfinite(value->getNumber())) {
        return std::nullopt;
    }
    return std::llround(std::clamp(value->getNumber(), 0.0, 100.0));
}

std::time_t readEpochMilliseconds(const JsonValue* object, const char* member) {
    const auto* value = field(object, member);
    if (!value) {
        return 0;
    }
    constexpr long long kMaxEpochMilliseconds = 253402300799999; // Last millisecond of year 9999.
    long long milliseconds = 0;
    if (value->isString()) {
        const auto& raw = value->getString();
        const auto end = raw.data() + raw.size();
        const auto result = std::from_chars(raw.data(), end, milliseconds);
        if (result.ec != std::errc{} || result.ptr != end) {
            return 0;
        }
    } else if (value->isNumber() && value->getNumber() > 0 && value->getNumber() <= kMaxEpochMilliseconds) {
        milliseconds = static_cast<long long>(value->getNumber());
    }
    return milliseconds > 0 && milliseconds <= kMaxEpochMilliseconds ? milliseconds / 1000 : 0;
}

std::time_t readIsoTime(const std::string& value) {
    static const std::regex format(
        R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2})(?::(\d{2})(?:\.\d+)?)?(Z|[+-](?:[01][0-9]|2[0-3]):[0-5][0-9])$)");
    std::smatch parts;
    if (!std::regex_match(value, parts, format)) {
        return 0;
    }
    std::istringstream stream(parts[1].str() + ":" + (parts[2].matched ? parts[2].str() : "00"));
    std::tm date{};
    stream >> std::get_time(&date, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return 0;
    }
    const auto day = date.tm_mday;
    const auto hour = date.tm_hour;
    const auto minute = date.tm_min;
    const auto seconds = date.tm_sec;
    const auto stamp = _mkgmtime(&date);
    if (stamp <= 0 || date.tm_mday != day || date.tm_hour != hour || date.tm_min != minute || date.tm_sec != seconds) {
        return 0;
    }
    const auto zone = parts[3].str();
    if (zone == "Z") {
        return stamp;
    }
    const auto offset = (std::stoi(zone.substr(1, 2)) * 60 + std::stoi(zone.substr(4, 2))) * 60;
    return stamp + (zone.front() == '+' ? -offset : offset);
}

std::string surface(SubscriptionProvider provider) {
    return provider == SubscriptionProvider::Grok ? "grok_subscription_api" : "cursor_subscription_api";
}

} // namespace

SubscriptionQuotaAdapter::SubscriptionQuotaAdapter(SubscriptionProvider provider) : provider_(provider) {}

QuotaResponse SubscriptionQuotaAdapter::parseQuotaResponse(SubscriptionProvider provider, const HttpResponse& response,
                                                           std::time_t checkedAt) {
    QuotaResponse result;
    result.snapshotTime = checkedAt;
    result.providerSurface = surface(provider);
    if (!response.success) {
        if (provider == SubscriptionProvider::Grok && response.statusCode == 401) {
            result.unavailable = true;
            result.error = "Run grok login again";
            return result;
        }
        result.unavailable = response.statusCode == 401 || response.statusCode == 403;
        result.error = std::string(provider == SubscriptionProvider::Grok ? "Grok" : "Cursor") +
                       " subscription quota request failed";
        if (response.statusCode > 0) {
            result.error += " (HTTP " + std::to_string(response.statusCode) + ")";
        }
        return result;
    }
    result.error = "This account did not report a subscription quota";
    result.unavailable = true;
    JsonValue root;
    if (!JsonParser::parse(response.body, root) || !root.isObject()) {
        return result;
    }

    std::optional<long long> used;
    if (provider == SubscriptionProvider::Grok) {
        const auto* config = root.getMember("config");
        const auto* period = field(config, "currentPeriod");
        const auto type = stringField(period, "type");
        if (type == "USAGE_PERIOD_TYPE_WEEKLY" || type == "WEEKLY") {
            result.windowType = "weekly";
        } else if (type == "USAGE_PERIOD_TYPE_MONTHLY" || type == "MONTHLY") {
            result.windowType = "monthly";
        } else {
            result.windowType = "subscription";
        }
        result.windowStart = readIsoTime(stringField(period, "start"));
        result.windowEnd = readIsoTime(stringField(period, "end"));
        used = readPercent(config, "creditUsagePercent");
        // Zero can be omitted on a complete paid period.
        if (!used && !field(config, "creditUsagePercent") && result.windowType != "subscription" &&
            result.windowStart > 0 && result.windowEnd > result.windowStart) {
            used = 0;
        }
    } else {
        const auto* plan = root.getMember("planUsage");
        used = readPercent(plan, "totalPercentUsed");
        result.windowStart = readEpochMilliseconds(&root, "billingCycleStart");
        result.windowEnd = readEpochMilliseconds(&root, "billingCycleEnd");
        result.windowType = result.windowStart > 0 && result.windowEnd > result.windowStart &&
                                    result.windowEnd - result.windowStart <= 8 * 86400
                                ? "weekly"
                                : "monthly";
        if (const auto autoUsed = readPercent(plan, "autoPercentUsed"); !used && autoUsed) {
            used = autoUsed;
            result.windowType = "auto";
        }
        if (const auto apiUsed = readPercent(plan, "apiPercentUsed"); apiUsed && result.windowType == "auto") {
            result.secondaryWindowType = "api";
            result.secondaryUsed = *apiUsed;
            result.secondaryLimit = 100;
            result.secondaryRemaining = 100 - *apiUsed;
        } else if (!used && apiUsed) {
            used = apiUsed;
            result.windowType = "api";
        }
    }

    if (!used) {
        return result;
    }
    if (result.windowEnd > 0 && result.windowStart >= result.windowEnd) {
        result.windowStart = 0;
    }
    if (result.secondaryLimit > 0) {
        result.secondaryWindowStart = result.windowStart;
        result.secondaryWindowEnd = result.windowEnd;
    }
    result.success = true;
    result.unavailable = false;
    result.error.clear();
    result.used = *used;
    result.limit = 100;
    result.remaining = result.limit - result.used;
    return result;
}

QuotaResponse SubscriptionQuotaAdapter::fetchQuota() {
    QuotaResponse unavailable;
    unavailable.providerSurface = surface(provider_);
    unavailable.unavailable = true;
    unavailable.error = provider_ == SubscriptionProvider::Grok
                            ? "Sign in with Grok CLI to read subscription quota"
                            : "Set CURSOR_API_KEY or CURSOR_AUTH_TOKEN to read Cursor subscription quota";
    HttpClient http;
    http.setTimeout(5000);
    http.disableRedirects();
    HttpResponse response;
    if (provider_ == SubscriptionProvider::Grok) {
        const auto token = grokToken();
        if (token.empty()) {
            return unavailable;
        }
        http.setApiKey(token);
        response = http.get(kGrokQuotaUrl);
    } else {
        if (!cursorDefaultEndpoint()) {
            unavailable.error = "Cursor subscription quota requires the default Cursor endpoint";
            return unavailable;
        }
        auto token = env("CURSOR_AUTH_TOKEN");
        if (token.empty()) {
            const auto key = env("CURSOR_API_KEY");
            if (!usableBearer(key)) {
                return unavailable;
            }
            http.setApiKey(key);
            const auto exchange = http.post(kCursorExchangeUrl, "{}");
            JsonValue body;
            if (!exchange.success || !JsonParser::parse(exchange.body, body)) {
                unavailable.error = "Cursor could not authenticate the usage request";
                unavailable.unavailable = exchange.statusCode == 401 || exchange.statusCode == 403;
                return unavailable;
            }
            token = stringField(&body, "accessToken");
        }
        if (!usableBearer(token)) {
            return unavailable;
        }
        http.setApiKey(token);
        http.setHeader("connect-protocol-version", "1");
        http.setHeader("x-cursor-client-type", "cli");
        response = http.post(kCursorQuotaUrl, "{}");
    }
    auto quota = parseQuotaResponse(provider_, response, std::time(nullptr));
    if (provider_ == SubscriptionProvider::Grok && response.statusCode == 401 && !env("GROK_AUTH").empty()) {
        quota.error = "Update GROK_AUTH and restart JiStats";
    }
    return quota;
}

UsageResponse SubscriptionQuotaAdapter::fetchUsage(std::time_t from, std::time_t to) {
    UsageResponse result;
    result.windowStart = from;
    result.windowEnd = to;
    result.error = "The subscription quota connection does not provide dated token usage";
    return result;
}

bool SubscriptionQuotaAdapter::validateCredentials() {
    if (provider_ == SubscriptionProvider::Grok) {
        return !grokToken().empty();
    }
    return cursorDefaultEndpoint() && (usableBearer(env("CURSOR_AUTH_TOKEN")) || usableBearer(env("CURSOR_API_KEY")));
}

std::vector<Capability> SubscriptionQuotaAdapter::getCapabilities() {
    return {{"quota", "Account subscription quota", true, "Provider-reported billing window"}};
}

std::string SubscriptionQuotaAdapter::getProviderKey() const {
    return provider_ == SubscriptionProvider::Grok ? "grok" : "cursor";
}

std::string SubscriptionQuotaAdapter::getDisplayName() const {
    return provider_ == SubscriptionProvider::Grok ? "Grok" : "Cursor";
}

bool SubscriptionQuotaAdapter::isEnabled() const {
    return enabled_;
}

void SubscriptionQuotaAdapter::setEnabled(bool enabled) {
    enabled_ = enabled;
}

bool SubscriptionQuotaAdapter::hasDirectQuotaApiConfigured() const {
    // Budget unsuccessful attempts as well. Credential discovery also performs local file I/O.
    return true;
}

} // namespace JiStats
