// Command journal + event log, and the replay driver.
//
// Design decision: the journal records *commands*, not book state. Because the
// engine is deterministic (integer prices, sequence-number time priority, no
// clock or randomness in any decision), replaying the same command sequence into
// a freshly constructed exchange reproduces the same books and the same event
// stream. That gives us regression testing, post-mortem debugging, and
// benchmark reproducibility from one small artefact.
//
// Format: a fixed 32-byte header followed by packed fixed-size records. Simple
// enough to read with od(1), no schema library, no database. Deliberately not
// self-versioning beyond a magic + version field — this is a local debugging
// artefact, not a wire protocol.
//
// The engine never writes here on the hot path. The gateway journals a command
// before submitting it (write-ahead), and event recording is opt-in for tools.
#pragma once

#include "core/Commands.hpp"
#include "events/Events.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace te {

inline constexpr std::uint32_t kJournalMagic = 0x54454A31; // "TEJ1"
inline constexpr std::uint16_t kJournalVersion = 1;

struct JournalHeader {
  std::uint32_t magic{kJournalMagic};
  std::uint16_t version{kJournalVersion};
  std::uint16_t record_size{static_cast<std::uint16_t>(sizeof(Command))};
  std::uint64_t record_count{0}; // patched on close
  std::uint64_t reserved0{0};
  std::uint64_t reserved1{0};
};

static_assert(sizeof(JournalHeader) == 32, "journal header must stay 32 bytes");

// Buffered writer for the command journal.
class CommandJournalWriter {
public:
  CommandJournalWriter() = default;
  ~CommandJournalWriter();

  CommandJournalWriter(const CommandJournalWriter &) = delete;
  CommandJournalWriter &operator=(const CommandJournalWriter &) = delete;

  [[nodiscard]] bool open(const std::string &path);
  void append(const Command &cmd);
  // Patches the record count into the header and closes. Called by the dtor too,
  // so a crash still leaves a readable file with count == 0, which the reader
  // handles by scanning to EOF.
  bool close();

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
  std::FILE *file_{nullptr};
  std::uint64_t count_{0};
};

// Reads a journal fully into memory. Command streams for replay are small
// (millions of 64-byte records at most) and replay wants random access.
class CommandJournalReader {
public:
  // Returns false when the file is missing, truncated at the header, or has a
  // record size that does not match this build (a struct layout change).
  [[nodiscard]] bool load(const std::string &path);

  [[nodiscard]] const std::vector<Command> &commands() const noexcept {
    return commands_;
  }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

private:
  std::vector<Command> commands_;
  std::string error_;
};

// Optional event recorder, for capturing an engine's output stream so two runs
// can be compared field by field.
class EventLogWriter {
public:
  ~EventLogWriter();
  [[nodiscard]] bool open(const std::string &path);
  void append(const Event &e);
  void append(const std::vector<Event> &events);
  bool close();

private:
  std::FILE *file_{nullptr};
  std::uint64_t count_{0};
};

// ---------------------------------------------------------------------------
// Replay verification
// ---------------------------------------------------------------------------

// Compares two event streams for logical equality. Timestamps are excluded:
// they are telemetry, not engine state, and a replay legitimately runs at a
// different wall-clock time. Everything that affects trading outcomes is
// compared, including sequence numbers — if replay produced a different
// sequencing, determinism is broken and we want the test to fail.
[[nodiscard]] bool events_equivalent(const Event &a, const Event &b);

struct ReplayDiff {
  bool equal{true};
  std::size_t index{0};
  std::string detail;
};

[[nodiscard]] ReplayDiff compare_event_streams(const std::vector<Event> &a,
                                               const std::vector<Event> &b);

} // namespace te
