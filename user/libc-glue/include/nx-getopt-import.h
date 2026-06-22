/*
 * nx-getopt-import.h — route getopt's DATA globals (optarg/optind/opterr/optopt) through their
 * libc.ndl dllimport slots, the same indirection nx-dllimport.h applies to stdout/errno.
 *
 * A program that uses the system getopt reads `optarg` by address after getopt_long(); NanOS's
 * libc.ndl exports these only as `__imp_<name>` slots (a program can't reference a shared
 * library's data object directly), so without this redirect mknx reports `undefined ... optarg`.
 *
 * Force-included (extra -include) ONLY by the htop port, NOT globally: a port that bundles its
 * own getopt defines `optarg` itself, and a global macro would corrupt that definition. Pull the
 * real declarations first (<unistd.h>/<getopt.h>) so the #define rewrites references, not decls.
 */
#ifndef NX_GETOPT_IMPORT_H
#define NX_GETOPT_IMPORT_H

#include <unistd.h>
#include <getopt.h>

extern char **__imp_optarg;
extern int   *__imp_optind;
extern int   *__imp_opterr;
extern int   *__imp_optopt;

#define optarg (*__imp_optarg)
#define optind (*__imp_optind)
#define opterr (*__imp_opterr)
#define optopt (*__imp_optopt)

#endif
