#pragma once

/*
 * Log hook for the EDI / PAD decoding code. The application provides the
 * implementation (see src/core/log.cpp) so messages can be shown in the GUI
 * as well as on stderr. A trailing newline in fmt is optional.
 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void edi_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
