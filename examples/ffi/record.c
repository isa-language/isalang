#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
struct Inner {
  int16_t small;
  bool active;
};
struct Sample {
  uint8_t tag;
  int64_t number;
  struct Inner inner;
  double ratio;
  struct Sample *next;
};
struct Sample *sample_new(void) {
  struct Sample *s = calloc(1, sizeof *s);
  if (!s)
    abort();
  s->tag = 3;
  s->number = 40;
  s->inner.small = 2;
  s->inner.active = true;
  return s;
}
void sample_free(struct Sample *s) {
  free(s);
}
int64_t sample_score(const struct Sample *s) {
  return s->number + s->inner.small;
}
