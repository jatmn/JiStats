#pragma once

#include <vector>

namespace JiStats {
// Forward declare shared provider data types
struct UsageEvent;
struct QuotaResponse;

// Lightweight data-source interface for provider data delivery
class IProviderDataSource {
public:
    virtual ~IProviderDataSource() = default;

    // Notify when new usage events are available
    virtual void onUsageEvents(const std::vector<UsageEvent>& events) = 0;

    // Notify when new quota data is available
    virtual void onQuotaResponse(const QuotaResponse& response) = 0;
};
} // namespace JiStats
