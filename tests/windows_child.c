#include <string.h>
int main(int argc, char **argv) {
  if (argc == 1)
    return 17;
  if (argc == 2 && !strcmp(argv[1], "a b\"c\\\\ & exit 91"))
    return 23;
  if (argc == 2 && !strcmp(argv[1], "a b\\\\"))
    return 24;
  return 90;
}
