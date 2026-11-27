// SPDX-License-Identifier: MIT
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static _Thread_local int marker = 41;
int main(void) {
  void* so = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!so) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  double (*cosine)(double) = (double (*)(double))dlsym(so, "cos");
  if (!cosine || cosine(0.0) != 1.0) return 2;
  char* bytes = malloc(64);
  if (!bytes) return 3;
  snprintf(bytes, 64, "glibc + TLS + dlopen = %d", ++marker);
  puts(bytes);
  free(bytes);
  dlclose(so);
  return marker;
}
