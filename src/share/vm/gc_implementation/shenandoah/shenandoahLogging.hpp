
#define log_trace(...) if (ShenandoahLogTrace) gclog_or_tty->print_cr
#define log_debug(...) if (ShenandoahLogDebug) gclog_or_tty->print_cr
#define log_info(...) if (ShenandoahLogInfo) gclog_or_tty->print_cr
#define log_warning(...) if (ShenandoahLogInfo) gclog_or_tty->print_cr

#ifndef PRODUCT
#define log_develop_trace(...) if (ShenandoahLogTrace) gclog_or_tty->print_cr
#define log_develop_debug(...) if (ShenandoahLogDebug) gclog_or_tty->print_cr
#else
#define DUMMY_ARGUMENT_CONSUMER(...)
#define log_develop_trace(...) DUMMY_ARGUMENT_CONSUMER
#define log_develop_debug(...) DUMMY_ARGUMENT_CONSUMER
#endif
