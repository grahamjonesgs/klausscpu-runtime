/*
 * ext_log.h — kernel logging entry point exported to LLEXT extensions.
 *
 * Extensions can't own a Zephyr log module (see ext_log.c for why), so the
 * kernel owns one "ext" module on their behalf and exports ext_log().  An
 * extension calls ext_log() — usually via the LOG_* shim in ext_log_compat.h —
 * and the kernel routes the message through its normal logging backends,
 * including the SD-card filesystem backend (/SD:/log).
 */
#ifndef KLAUSSCPU_EXT_LOG_H_
#define KLAUSSCPU_EXT_LOG_H_

enum {
	EXT_LOG_ERR = 0,
	EXT_LOG_WRN = 1,
	EXT_LOG_INF = 2,
};

/* level is one of EXT_LOG_*; msg is a complete, pre-formatted log line
 * (formatting happens in the caller, so no varargs cross the kernel/extension
 * boundary). */
void ext_log(int level, const char *msg);

#endif /* KLAUSSCPU_EXT_LOG_H_ */
