#pragma once

#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/http/codes.h"

#include "source/server/admin/utils.h"

#include "re2/re2.h"

namespace Envoy {
namespace Server {

namespace StatLabels {
constexpr absl::string_view All = "All";
constexpr absl::string_view Counters = "Counters";
constexpr absl::string_view Gauges = "Gauges";
constexpr absl::string_view Histograms = "Histograms";
constexpr absl::string_view TextReadouts = "TextReadouts";
} // namespace StatLabels

enum class StatsFormat {
#ifdef ENVOY_ADMIN_HTML
  Html,
  ActiveHtml,
#endif
  Json,
  Prometheus,
  Text,
};

// The order is used to linearize the ordering of stats of all types.
enum class StatsType {
  TextReadouts,
  Counters,
  Gauges,
  Histograms,
  All,
};

enum class HiddenFlag {
  Include,  // Will include hidden stats along side non-hidden stats
  ShowOnly, // Will only show hidden stats and exclude hidden stats
  Exclude,  // Default behavior. Will exclude all hidden stats
};

struct StatsParams {
  /**
   * Parses the URL's query parameter, populating this.
   *
   * @param url the URL from which to parse the query params.
   * @param response used to write error messages, if necessary.
   */
  Http::Code parse(absl::string_view url, Buffer::Instance& response);

  /**
   * @return a string representation for a type.
   */
  static absl::string_view typeToString(StatsType type);

  StatsType type_{StatsType::All};
  bool used_only_{false};
  bool prometheus_text_readouts_{false};
  bool pretty_{false};
  StatsFormat format_{StatsFormat::Text};
  HiddenFlag hidden_{HiddenFlag::Exclude};
  std::string filter_string_;
  std::shared_ptr<re2::RE2> re2_filter_;
  Utility::HistogramBucketsMode histogram_buckets_mode_{Utility::HistogramBucketsMode::NoBuckets};
  Http::Utility::QueryParams query_;

  /**
   * Determines whether a metric should be shown based on the specified query-parameters. This
   * covers:
   * ``usedonly``, hidden, and filter.
   *
   * @param metric the metric to test
   * @param name_out if non-null, and the return value is true,
   *   will contain the metric name. This improves performance because computing the name is
   *   somewhat expensive, and in some cases it isn't needed.
   */
  template <class StatType>
  bool shouldShowMetric(const StatType& metric, std::string* name_out = nullptr) const {
    // This duplicates logic in StatsRequest::populateStatsFromScopes, but differs
    // in one subtle way: in Prometheus we only use metric.name() for filtering,
    // not rendering, so we only construct the name if there's a filter.
    if (used_only_ && !metric.used()) {
      return false;
    }
    if (hidden_ == HiddenFlag::ShowOnly && !metric.hidden()) {
      return false;
    }
    if (hidden_ == HiddenFlag::Exclude && metric.hidden()) {
      return false;
    }

    // Performance note: computing the name is expensive.
    //
    // In the prometheus case, this is only necessary if doing regex filtering and a regex is
    // defined (and the function parameter will be nullptr).
    //
    // In the non-prometheus case, the name is needed if the metric should be shown, so compute it
    // once and use it for both regex filtering and in emitting the stat (consumed by caller by
    // setting `name_out` to non-nullptr).
    //
    // For per-endpoint metrics, the name is precomputed and returned as a reference, so storing
    // it in a std::string causes an extra allocation.
    if (name_out != nullptr) {
      *name_out = metric.name();
    }

    if (re2_filter_ != nullptr &&
        !re2::RE2::PartialMatch((name_out != nullptr) ? *name_out : metric.name(), *re2_filter_)) {
      return false;
    }
    return true;
  }
};

} // namespace Server
} // namespace Envoy
