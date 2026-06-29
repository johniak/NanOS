/*
 * linuxkpi/include/linux/of_graph.h — OF graph (device-tree port/endpoint) shim.
 *
 * NanOS has no device tree, so OF-graph traversal always finds nothing. Only pulled in
 * by drm_bridge.c for DT-described bridge chains, which a virtio-gpu guest never has.
 */
#ifndef _LKPI_OF_GRAPH_H
#define _LKPI_OF_GRAPH_H
#include <linux/types.h>
#include <linux/of.h>

struct device_node;
struct of_endpoint { unsigned int port; unsigned int id; const struct device_node *local_node; };

static inline struct device_node *of_graph_get_next_endpoint(const struct device_node *p, struct device_node *prev){ (void)p;(void)prev; return 0; }
static inline struct device_node *of_graph_get_endpoint_by_regs(const struct device_node *p, int port, int ep){ (void)p;(void)port;(void)ep; return 0; }
static inline struct device_node *of_graph_get_remote_port_parent(const struct device_node *n){ (void)n; return 0; }
static inline struct device_node *of_graph_get_remote_port(const struct device_node *n){ (void)n; return 0; }
static inline struct device_node *of_graph_get_remote_node(const struct device_node *n, u32 port, u32 ep){ (void)n;(void)port;(void)ep; return 0; }
static inline int of_graph_parse_endpoint(const struct device_node *n, struct of_endpoint *e){ (void)n;(void)e; return -1; }
static inline unsigned int of_graph_get_endpoint_count(const struct device_node *n){ (void)n; return 0; }

#endif /* _LKPI_OF_GRAPH_H */
