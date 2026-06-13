/* crypt.h — password hashing (NanOS provides crypt() in libc-glue/crypt.c, SHA-512 $6$). */
#ifndef _CRYPT_H
#define _CRYPT_H
#ifdef __cplusplus
extern "C" {
#endif
char* crypt(const char* key, const char* setting);
#ifdef __cplusplus
}
#endif
#endif
