/*
 * usedll — demonstrates dynamic linking. It calls nx_greet()/nx_greeting(), which it
 * imports BY NAME from greet.ndl (resolved by the loader through the import library
 * user/lib/greet_import.S). usedll itself contains no copy of those functions — only IAT
 * slots the loader fills in. Proof of a stable ABI: rebuild greet.ndl alone (change the
 * return value) and usedll picks it up without being recompiled.
 */
#include <stdio.h>

extern int nx_greet(void);
extern const char* nx_greeting(void);

int main(void) {
	printf("usedll: nx_greet() = %d\n", nx_greet());
	printf("usedll: nx_greeting() = %s\n", nx_greeting());
	return 0;
}
