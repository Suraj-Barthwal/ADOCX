#pragma once

#include <cstddef>

namespace adocx {

struct AdocConfig {
  std::size_t hot_set_capacity = 1024;
  double promote_threshold = 2.0;
  double demote_threshold = 1.0;
  double ema_alpha = 0.2;
  bool enable_auto_tuning = true;
  std::size_t tuning_interval_ops = 10000;
};

}  // namespace adocx
