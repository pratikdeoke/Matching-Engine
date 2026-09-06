#include "replay/EventLog.hpp"

#include <algorithm>
#include <cstring>

namespace te {

// ---------------------------------------------------------------------------
// CommandJournalWriter
// ---------------------------------------------------------------------------

CommandJournalWriter::~CommandJournalWriter() { close(); }

bool CommandJournalWriter::open(const std::string &path) {
  close();
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    return false;
  }
  const JournalHeader hdr;
  if (std::fwrite(&hdr, sizeof(hdr), 1, file_) != 1) {
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  count_ = 0;
  return true;
}

void CommandJournalWriter::append(const Command &cmd) {
  if (file_ == nullptr) {
    return;
  }
  // stdio's buffer is the batching layer here; no need for a hand-rolled one.
  if (std::fwrite(&cmd, sizeof(cmd), 1, file_) == 1) {
    ++count_;
  }
}

bool CommandJournalWriter::close() {
  if (file_ == nullptr) {
    return true;
  }
  bool ok = true;
  // Patch the record count so the reader can size its vector in one shot.
  if (std::fseek(file_, 0, SEEK_SET) == 0) {
    JournalHeader hdr;
    hdr.record_count = count_;
    ok = std::fwrite(&hdr, sizeof(hdr), 1, file_) == 1;
  } else {
    ok = false;
  }
  ok = (std::fclose(file_) == 0) && ok;
  file_ = nullptr;
  return ok;
}

// ---------------------------------------------------------------------------
// CommandJournalReader
// ---------------------------------------------------------------------------

bool CommandJournalReader::load(const std::string &path) {
  commands_.clear();
  error_.clear();

  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    error_ = "cannot open " + path;
    return false;
  }

  JournalHeader hdr;
  if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
    error_ = "truncated header";
    std::fclose(f);
    return false;
  }
  if (hdr.magic != kJournalMagic) {
    error_ = "bad magic (not a command journal)";
    std::fclose(f);
    return false;
  }
  if (hdr.version != kJournalVersion) {
    error_ = "unsupported journal version";
    std::fclose(f);
    return false;
  }
  if (hdr.record_size != sizeof(Command)) {
    // Command layout changed since the journal was written; replaying would
    // reinterpret bytes as the wrong fields, which is worse than refusing.
    error_ = "record size mismatch: journal written by an incompatible build";
    std::fclose(f);
    return false;
  }

  if (hdr.record_count > 0) {
    commands_.resize(hdr.record_count);
    const std::size_t got =
        std::fread(commands_.data(), sizeof(Command), hdr.record_count, f);
    commands_.resize(got); // tolerate a truncated tail from an unclean shutdown
  } else {
    // record_count == 0 means the writer never closed cleanly. Scan to EOF.
    Command c;
    while (std::fread(&c, sizeof(Command), 1, f) == 1) {
      commands_.push_back(c);
    }
  }

  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// EventLogWriter
// ---------------------------------------------------------------------------

EventLogWriter::~EventLogWriter() { close(); }

bool EventLogWriter::open(const std::string &path) {
  close();
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    return false;
  }
  JournalHeader hdr;
  hdr.record_size = static_cast<std::uint16_t>(sizeof(Event));
  if (std::fwrite(&hdr, sizeof(hdr), 1, file_) != 1) {
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  count_ = 0;
  return true;
}

void EventLogWriter::append(const Event &e) {
  if (file_ != nullptr && std::fwrite(&e, sizeof(e), 1, file_) == 1) {
    ++count_;
  }
}

void EventLogWriter::append(const std::vector<Event> &events) {
  for (const Event &e : events) {
    append(e);
  }
}

bool EventLogWriter::close() {
  if (file_ == nullptr) {
    return true;
  }
  bool ok = true;
  if (std::fseek(file_, 0, SEEK_SET) == 0) {
    JournalHeader hdr;
    hdr.record_size = static_cast<std::uint16_t>(sizeof(Event));
    hdr.record_count = count_;
    ok = std::fwrite(&hdr, sizeof(hdr), 1, file_) == 1;
  } else {
    ok = false;
  }
  ok = (std::fclose(file_) == 0) && ok;
  file_ = nullptr;
  return ok;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

bool events_equivalent(const Event &a, const Event &b) {
  // Field-by-field rather than memcmp: memcmp would compare padding bytes and
  // the ts field, producing spurious mismatches.
  return a.type == b.type && a.seq == b.seq && a.symbol == b.symbol &&
         a.order_id == b.order_id && a.client == b.client && a.side == b.side &&
         a.order_type == b.order_type && a.status == b.status &&
         a.price == b.price && a.quantity == b.quantity &&
         a.remaining == b.remaining && a.reject == b.reject &&
         a.cancel_cause == b.cancel_cause && a.trade_id == b.trade_id &&
         a.trade_price == b.trade_price && a.trade_qty == b.trade_qty &&
         a.aggressor_id == b.aggressor_id && a.resting_id == b.resting_id &&
         a.aggressor_client == b.aggressor_client &&
         a.resting_client == b.resting_client &&
         a.aggressor_side == b.aggressor_side && a.best_bid == b.best_bid &&
         a.best_bid_qty == b.best_bid_qty && a.best_ask == b.best_ask &&
         a.best_ask_qty == b.best_ask_qty && a.old_price == b.old_price &&
         a.old_quantity == b.old_quantity &&
         a.priority_retained == b.priority_retained;
}

ReplayDiff compare_event_streams(const std::vector<Event> &a,
                                 const std::vector<Event> &b) {
  ReplayDiff d;
  if (a.size() != b.size()) {
    d.equal = false;
    d.index = std::min(a.size(), b.size());
    d.detail = "event count differs: " + std::to_string(a.size()) + " vs " +
               std::to_string(b.size());
    return d;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!events_equivalent(a[i], b[i])) {
      d.equal = false;
      d.index = i;
      d.detail = "event " + std::to_string(i) + " differs (type " +
                 std::string(to_string(a[i].type)) + " vs " +
                 std::string(to_string(b[i].type)) + ", seq " +
                 std::to_string(a[i].seq) + " vs " + std::to_string(b[i].seq) +
                 ")";
      return d;
    }
  }
  return d;
}

} // namespace te
