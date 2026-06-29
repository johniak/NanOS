#ifndef _LKPI_TRACEPOINT_H
#define _LKPI_TRACEPOINT_H
#define TRACE_EVENT(name,proto,args,...) 
#define DECLARE_EVENT_CLASS(...) 
#define DEFINE_EVENT(...) 
#define TP_PROTO(...) 
#define TP_ARGS(...) 
#define TP_STRUCT__entry(...) 
#define TP_fast_assign(...) 
#define TP_printk(...) 
#define DECLARE_TRACE(name,proto,args) static inline void trace_##name(void){} 
#define EXPORT_TRACEPOINT_SYMBOL(name) 
#define EXPORT_TRACEPOINT_SYMBOL_GPL(name) 
#endif
