#ifndef _LKPI_TRACEPOINT_H
#define _LKPI_TRACEPOINT_H
#define TP_PROTO(...) __VA_ARGS__
#define TP_ARGS(...)
#define TP_STRUCT__entry(...)
#define TP_fast_assign(...)
#define TP_printk(...)
#define TRACE_EVENT(name, proto, ...) static inline void trace_##name(proto) {}
#define DECLARE_EVENT_CLASS(name, proto, ...)
#define DEFINE_EVENT(tmpl, name, proto, ...) static inline void trace_##name(proto) {}
#define DECLARE_TRACE(name, proto, ...) static inline void trace_##name(proto) {}
#define EXPORT_TRACEPOINT_SYMBOL(name)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name)
#endif
