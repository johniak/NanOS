#ifndef _LKPI_TRACEPOINT_H
#define _LKPI_TRACEPOINT_H
#define TP_PROTO(...) __VA_ARGS__
#define TP_ARGS(...)
#define TP_STRUCT__entry(...)
#define TP_fast_assign(...)
#define TP_printk(...)
#define TP_CONDITION(...)
#define TRACE_EVENT(name, proto, ...) static inline void trace_##name(proto) {}
/* Conditional/print variants (i915 uses TRACE_EVENT_CONDITION for i915_reg_rw; the display trace
 * headers use the DEFINE_EVENT_{CONDITION,PRINT} forms). The condition/print is a no-op here — the
 * tracepoint compiles to an empty inline, same as the plain forms. */
#define TRACE_EVENT_CONDITION(name, proto, args, cond, ...) static inline void trace_##name(proto) {}
#define DECLARE_EVENT_CLASS(name, proto, ...)
#define DEFINE_EVENT(tmpl, name, proto, ...) static inline void trace_##name(proto) {}
#define DEFINE_EVENT_CONDITION(tmpl, name, proto, args, cond) static inline void trace_##name(proto) {}
#define DEFINE_EVENT_PRINT(tmpl, name, proto, args, print) static inline void trace_##name(proto) {}
#define DECLARE_TRACE(name, proto, ...) static inline void trace_##name(proto) {}
#define EXPORT_TRACEPOINT_SYMBOL(name)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name)
#endif
