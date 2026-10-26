#include "log.h"
#include "edi/edi_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace applog {

static std::mutex g_mutex;
static std::deque<std::string> g_lines;
static size_t g_total = 0;              // lines ever added
static const size_t kMaxLines = 1000;

void add(const std::string &line)
{
    std::fprintf(stderr, "%s\n", line.c_str());
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lines.push_back(line);
    g_total++;
    while (g_lines.size() > kMaxLines) g_lines.pop_front();
}

size_t fetch(size_t since, std::vector<std::string> &out)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t first = g_total - g_lines.size();
    for (size_t i = since > first ? since : first; i < g_total; i++)
        out.push_back(g_lines[i - first]);
    return g_total;
}

} // namespace applog

extern "C" void edi_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (n && buf[0] == '\n') applog::add(buf + 1);
    else applog::add(buf);
}
