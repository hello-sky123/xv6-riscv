#include "types.h"

// 把从 dst 开始的 n 个字节都设成 c 的低八位
void *
memset(void *dst, int c, uint n)
{
  char *cdst = (char *)dst;
  int i;
  for (i = 0; i < n; i++) {
    cdst[i] = c; // 把 int 赋值给 char，会截断成一个字节
  }
  return dst;
}

// 逐字节比较两块内存的前 n 个字节，全部相同返回 0
int
memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while (n-- > 0) {
    if (*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

// 把 src 开始的 n 个字节复制到 dst，然后返回 dst。和普通的复制区别是，两块内存重叠时，它也能
// 复制正确
void *
memmove(void *dst, const void *src, uint n)
{
  const char *s;
  char *d;

  if (n == 0)
    return dst;

  s = src;
  d = dst;
  // dst 在 src 后面并且两块内存重叠，从后往前复制；其他情况从前往后复制
  if (s < d && s + n > d) {
    s += n;
    d += n;
    while (n-- > 0)
      *--d = *--s;
  } else
    while (n-- > 0)
      *d++ = *s++;

  return dst;
}

// memcpy exists to placate GCC.  Use memmove.
void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

// 最多比较两个字符串的前 n 个字符。遇到以下三种情况之一就停止：比完了 n 个字符、遇到不同的字符、
// p 到了结尾的 '\0'
int
strncmp(const char *p, const char *q, uint n)
{
  // n > 0：还没有比够 n 个字符，*p：p 还没到 '\0'，*p == *q：当前字符相同
  while (n > 0 && *p && *p == *q)
    n--, p++, q++;
  // 退出后的判断，如果 n == 0：前 n 个字符全部相同
  if (n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

// 把字符串 t 复制到 s，一共正好写 n 个字节。t 的长度小于 n 时，复制完 '\0' 后剩下位置全部补 0。t 的
// 长度大于等于 n 时，只复制前 n 个字符，不会在末尾写 '\0'
char *
strncpy(char *s, const char *t, int n)
{
  // 先保存原来的值
  char *os;

  os = s;
  // 循环体是空的，事情都在条件里做完了：1. n-- > 0：还有剩余的名额，同时计数减 1；
  // 2. *s++ = *t++：复制一个字符，两个指针都往后移；3. != 0：刚复制的字符如果是 '\0'，就停下
  while (n-- > 0 && (*s++ = *t++) != 0)
    ;
  // 补 0
  while (n-- > 0)
    *s++ = 0;
  return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char *
safestrcpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  if (n <= 0)
    return os;
  while (--n > 0 && (*s++ = *t++) != 0)
    ;
  *s = 0;
  return os;
}

int
strlen(const char *s)
{
  int n;

  for (n = 0; s[n]; n++)
    ;
  return n;
}
