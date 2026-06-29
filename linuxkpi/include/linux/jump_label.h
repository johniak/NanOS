#ifndef _LKPI_JUMP_LABEL_H
#define _LKPI_JUMP_LABEL_H
struct static_key_false { int v; };
struct static_key_true { int v; };
#define DEFINE_STATIC_KEY_FALSE(name) struct static_key_false name = { 0 }
#define DEFINE_STATIC_KEY_TRUE(name) struct static_key_true name = { 1 }
#define static_branch_likely(k) (0)
#define static_branch_unlikely(k) (0)
#define static_branch_enable(k) do{ (k)->v=1; }while(0)
#define static_branch_disable(k) do{ (k)->v=0; }while(0)
#define static_key_enabled(k) ((k)->v)
#endif
