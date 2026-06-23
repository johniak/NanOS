/* sys/ttydefaults.h — default terminal control characters (toybox includes it). Standard BSD
 * content; values match Linux/glibc. */
#ifndef _SYS_TTYDEFAULTS_H
#define _SYS_TTYDEFAULTS_H

#define CTRL(x) ((x) & 037)

#define CEOF     CTRL('d')
#define CERASE   0177
#define CINTR    CTRL('c')
#define CKILL    CTRL('u')
#define CMIN     1
#define CQUIT    034
#define CSUSP    CTRL('z')
#define CTIME    0
#define CDSUSP   CTRL('y')
#define CSTART   CTRL('q')
#define CSTOP    CTRL('s')
#define CLNEXT   CTRL('v')
#define CDISCARD CTRL('o')
#define CWERASE  CTRL('w')
#define CREPRINT CTRL('r')
#define CEOT     CEOF
#define CBRK     CEOL
#define CRPRNT   CREPRINT
#define CFLUSH   CDISCARD
#ifndef CEOL
#define CEOL     0
#endif

#endif /* _SYS_TTYDEFAULTS_H */
