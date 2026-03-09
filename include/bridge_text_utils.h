#pragma once

#include <stddef.h>

namespace bridge {
namespace text {

void copyText(char *dst, size_t dstSize, const char *src);
char *trimInPlace(char *s);
bool equalsIgnoreCase(const char *a, const char *b);
bool startsWithIgnoreCase(const char *s, const char *prefix);
bool parseBoolLike(const char *s, bool &out);
void sanitizeTopicToken(char *value, bool allowSlash, const char *fallback);

} // namespace text
} // namespace bridge
