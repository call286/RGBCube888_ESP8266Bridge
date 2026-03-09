#include "bridge_text_utils.h"

#include <string.h>

namespace {

bool isTopicChar(char c, bool allowSlash) {
  if (c >= 'a' && c <= 'z') {
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    return true;
  }
  if (c >= '0' && c <= '9') {
    return true;
  }
  if (c == '_' || c == '-' || c == '.') {
    return true;
  }
  return allowSlash && c == '/';
}

char *ltrimInPlace(char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  return s;
}

void rtrimInPlace(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

} // namespace

namespace bridge {
namespace text {

void sanitizeTopicToken(char *value, bool allowSlash, const char *fallback) {
  if (value == nullptr) {
    return;
  }

  size_t writePos = 0;
  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (isTopicChar(c, allowSlash)) {
      value[writePos++] = c;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '"' || c == '\'') {
      break;
    }
    // Skip unsupported characters in the middle.
  }
  value[writePos] = '\0';

  if (writePos == 0 && fallback != nullptr) {
    size_t j = 0;
    while (fallback[j] != '\0' && j + 1 < 64) {
      value[j] = fallback[j];
      j++;
    }
    value[j] = '\0';
  }
}

void copyText(char *dst, size_t dstSize, const char *src) {
  if (dstSize == 0) {
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && i + 1 < dstSize) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

char *trimInPlace(char *s) {
  char *p = ltrimInPlace(s);
  rtrimInPlace(p);
  return p;
}

bool equalsIgnoreCase(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

bool startsWithIgnoreCase(const char *s, const char *prefix) {
  while (*prefix != '\0' && *s != '\0') {
    char cs = *s;
    char cp = *prefix;
    if (cs >= 'A' && cs <= 'Z') {
      cs = (char)(cs - 'A' + 'a');
    }
    if (cp >= 'A' && cp <= 'Z') {
      cp = (char)(cp - 'A' + 'a');
    }
    if (cs != cp) {
      return false;
    }
    s++;
    prefix++;
  }
  return *prefix == '\0';
}

bool parseBoolLike(const char *s, bool &out) {
  if (equalsIgnoreCase(s, "1") || equalsIgnoreCase(s, "true") || equalsIgnoreCase(s, "on") ||
      equalsIgnoreCase(s, "yes") || equalsIgnoreCase(s, "enabled")) {
    out = true;
    return true;
  }
  if (equalsIgnoreCase(s, "0") || equalsIgnoreCase(s, "false") || equalsIgnoreCase(s, "off") ||
      equalsIgnoreCase(s, "no") || equalsIgnoreCase(s, "disabled")) {
    out = false;
    return true;
  }
  return false;
}

} // namespace text
} // namespace bridge
