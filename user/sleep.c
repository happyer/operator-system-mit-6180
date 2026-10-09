#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int ticks;

  // 参数校验：必须且只能有一个参数
  if(argc != 2){
    fprintf(2, "usage: sleep <ticks>\n");
    exit(1);
  }

  ticks = atoi(argv[1]);

  // 调用 xv6 的休眠系统调用（单位：时钟中断 tick）
  // 注意：本年度 xv6 把它命名为 pause(n)，不是 sleep(n)
  pause(ticks);

  exit(0);
}
