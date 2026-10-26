#pragma once

// Application log: collects the edi_log() messages from all threads so the
// GUI can show them, and echoes them to stderr.

#include <string>
#include <vector>

namespace applog {

void add(const std::string &line);

// Lines added since `since` (a counter returned by a previous call).
// Returns the new counter; old lines beyond the ring size are dropped.
size_t fetch(size_t since, std::vector<std::string> &out);

} // namespace applog
