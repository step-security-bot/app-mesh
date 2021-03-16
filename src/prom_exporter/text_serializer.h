#pragma once

#include <iosfwd>
#include <vector>

#include "detail/core_export.h"
#include "metric_family.h"
#include "serializer.h"

namespace prometheus {

class PROMETHEUS_CPP_CORE_EXPORT TextSerializer : public Serializer {
 public:
  using Serializer::Serialize;
  void Serialize(std::ostream& out,
                 const std::vector<MetricFamily>& metrics) const override;
};

}  // namespace prometheus
