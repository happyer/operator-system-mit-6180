// sixfive：对于每个输入文件，打印文件中所有能被 5 或 6 整除的数字。
//
// lab 对“数字”的定义（必须严格遵守，否则 "xv6" 里的 6 会被误判）：
//   数字 = 一串十进制数字，它的两边都是分隔符。
//   分隔符集合 = " -\r\t\n./,"（空格、减号、回车、tab、换行、点、斜杠、逗号）
//   文件的开头和结尾也算“隐式分隔符”。
//
// 于是：
//   "xv6"  -> 不是数字（'6' 左边是 'v'，不是分隔符）→ 不打印
//   "/6,"  -> 是数字 6 → 打印
//   "18-4" -> 是两个数字 18 和 4（'-' 是分隔符）

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define MAXDIGITS 9   // 9 位数最大 999999999，放在 int 里不会溢出

// c 是不是 8 个分隔符之一
// （lab 的提示里说可以用 strchr，这里显式比较，逻辑更直白）
static int
issep(char c)
{
  return c == ' ' || c == '-' || c == '\r' || c == '\t' ||
         c == '\n' || c == '.' || c == '/' || c == ',';
}

// 一个数字刚刚结束时调用（遇到分隔符或读到文件结尾）：
// 如果它是合法数字、且能被 5 或 6 整除，就打印；然后把累积状态清零。
static void
endnum(int *num, int *ndigits, int *invalid)
{
  if(*ndigits > 0 && !*invalid && *num != 0 &&
     (*num % 5 == 0 || *num % 6 == 0))
    printf("%d\n", *num);

  *num = 0;
  *ndigits = 0;
  *invalid = 0;
}

static void
sixfive(char *fname)
{
  int fd;
  char c;
  int num = 0;       // 当前正在累积的数值
  int ndigits = 0;   // 已经累积了几位；0 表示“当前不在数字里”
  int invalid = 0;   // 当前 token 不是合法数字（含非数字字符，或者位数太多）

  if((fd = open(fname, O_RDONLY)) < 0){
    fprintf(2, "sixfive: cannot open %s\n", fname);
    exit(1);
  }

  // 一次读一个字符：题目要求的做法，也省得自己处理缓冲区边界
  while(read(fd, &c, 1) == 1){
    if(issep(c)){
      // 分隔符：当前数字（如果存在）到此为止
      endnum(&num, &ndigits, &invalid);
    } else if(c >= '0' && c <= '9'){
      if(ndigits < MAXDIGITS)
        num = num * 10 + (c - '0');
      else
        invalid = 1;              // 位数太多会溢出，整个 token 当非法丢掉
      ndigits++;
    } else {
      invalid = 1;                // 例如 "xv6" 里的 'x' 和 'v'：整个 token 不是数字
    }
  }
  endnum(&num, &ndigits, &invalid);  // 文件结尾也是隐式分隔符

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: sixfive file...\n");
    exit(1);
  }

  for(int i = 1; i < argc; i++)
    sixfive(argv[i]);

  exit(0);
}
