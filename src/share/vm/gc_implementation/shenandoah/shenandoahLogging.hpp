#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHLOGGING_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHLOGGING_HPP

#define log_trace(...)   if (ShenandoahLogTrace) gclog_or_tty->print_cr
#define log_debug(...)   if (ShenandoahLogDebug) gclog_or_tty->print_cr
#define log_warning(...) if (ShenandoahLogInfo) gclog_or_tty->print_cr

// With ShenandoahLogInfo, only print out the single-"gc"-tag messages.
#define log_info(...)    if (((strcmp(#__VA_ARGS__, "gc") == 0) && ShenandoahLogInfo) || \
                             ((strcmp(#__VA_ARGS__, "gc") != 0) && ShenandoahLogDebug))  \
                                gclog_or_tty->print_cr

#ifndef PRODUCT
#define log_develop_trace(...) if (ShenandoahLogTrace) gclog_or_tty->print_cr
#define log_develop_debug(...) if (ShenandoahLogDebug) gclog_or_tty->print_cr
#else
#define DUMMY_ARGUMENT_CONSUMER(...)
#define log_develop_trace(...) DUMMY_ARGUMENT_CONSUMER
#define log_develop_debug(...) DUMMY_ARGUMENT_CONSUMER
#endif

#endif
