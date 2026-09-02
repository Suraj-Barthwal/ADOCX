#pragma once

#include <optional>
#include <string>

namespace adocx {

struct Status {
  bool ok;
  std::string message;

  static Status Ok() { return {true, ""}; }
  static Status Error(std::string msg) { return {false, std::move(msg)}; }
};

class KVStore {
 public:
  virtual ~KVStore() = default;
  virtual Status Put(const std::string& key, const std::string& value) = 0;
  virtual std::optional<std::string> Get(const std::string& key) = 0;
  virtual Status Delete(const std::string& key) = 0;
};

}  // namespace adocx
