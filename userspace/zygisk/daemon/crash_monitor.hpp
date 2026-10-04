#pragma once

#include "../crash_evidence.hpp"
#include "../crash_protection.hpp"
#include "../native_exit_evidence.hpp"
#include "uapi/yukizygisk.h"

#include <map>

namespace yukizygisk::crash {

struct Module {
  std::string id;
  std::string path;
  std::string identity;
  bool zygisk = false;
};

struct NativeModuleOutcome {
  std::string id;
  uint8_t state = 0;
};

struct NativeExitContext {
  std::string process;
  std::string target;
  uint8_t target_type = 0;
  uint8_t state = 0;
  std::vector<NativeModuleOutcome> modules;
  bool modules_observed = false;
};

class Monitor {
public:
  using Logger = void (*)(const char *);
  explicit Monitor(std::string tombstones = "/data/tombstones",
                   Logger logger = nullptr)
      : tombstones_(std::move(tombstones)), logger_(logger) {}
  ~Monitor();
  Monitor(const Monitor &) = delete;
  Monitor &operator=(const Monitor &) = delete;
  Monitor(Monitor &&) = delete;
  Monitor &operator=(Monitor &&) = delete;
  void set_modules(std::vector<Module> modules);
  void start(std::string directory, uint8_t abi);
  void on_exit(const yz_zygote_exit_event &event);
  void on_native_exit(const yz_target_exit_event &event,
                      const NativeExitContext &context, uint64_t epoch = 0,
                      uint64_t sequence = 0, bool batch = false);
  void begin_native_batch() { drain(); }
  void set_native_journal(const NativeExitJournalState &state) {
    native_journal_ = state;
    native_journal_present_ = true;
    native_dirty_ = true;
  }
  [[nodiscard]] const NativeExitJournalState &native_journal() const {
    return native_journal_;
  }
  bool flush_native() {
    save(true);
    return !native_dirty_;
  }
  void drain();
  void tick();
  void set_protection_enabled(bool enabled) {
    protection_.set_enabled(enabled);
  }
  bool suspended(const std::string &id) { return protection_.blocked(id); }
  [[nodiscard]] int fd() const { return notify_; }
  [[nodiscard]] int timeout_ms() const;

private:
  struct Stamp {
    uint64_t device = 0;
    uint64_t inode = 0;
    int64_t size = 0;
    int64_t seconds = 0;
    int64_t nanos = 0;
    bool operator==(const Stamp &other) const;
  };
  struct Candidate {
    std::string name;
    std::string identity;
    uint64_t completed_ns = 0;
    Tombstone tombstone;
  };
  void watch();
  void scan();
  void consume(const std::string &name, uint64_t inode = 0);
  void consume_closed(const std::string &name);
  void log(const std::string &message) const;
  void correlate(const Exit &event, const Candidate &candidate);
  void correlate_native(const Candidate &candidate);
  void reconcile_native_candidates();
  void save(bool include_native = false);
  bool save_document(const char *name, const json::Value &document);
  std::string directory_;
  std::string tombstones_;
  Logger logger_ = nullptr;
  std::string boot_;
  uint8_t abi_ = 0;
  int notify_ = -1;
  int watch_ = -1;
  uint64_t retry_ns_ = 0;
  std::map<std::string, Stamp> stamps_;
  std::vector<Module> modules_;
  std::vector<Exit> exits_;
  std::vector<Exit> native_exits_;
  std::vector<Candidate> candidates_;
  std::vector<Candidate> native_candidates_;
  json::Value evidence_ = json::Value::array();
  json::Value native_evidence_ = json::Value::array();
  bool dirty_ = false;
  bool native_dirty_ = false;
  NativeExitJournalState native_journal_;
  bool native_journal_present_ = false;
  Protection protection_;
};

} // namespace yukizygisk::crash
