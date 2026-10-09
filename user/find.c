// find：在目录树里查找指定名字的文件。
//
// 用法：
//   find <dir> <name>                 打印找到的文件的完整路径
//   find <dir> <name> -exec <cmd...>  对每个找到的文件执行 "<cmd...> <file>"
//
// 例：
//   $ find . b            -> ./b  ./a/b  ./a/aa/b
//   $ find . wc -exec echo -> (对 ./wc 执行 "echo ./wc" -> 输出 ./wc)

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

char *name;              // 要找的文件名（只比较路径的“文件名部分”）
char *execcmd[MAXARG];   // -exec 后面的命令词，例如 ["grep", "hello"]
int nexec = 0;           // 命令词的个数；0 表示没有 -exec，只打印路径

// 返回 path 里最后一个 '/' 之后的部分（即文件名本身）。
// 抄的是 ls.c 里 fmtname 找最后一个斜杠的做法，只是不做空格填充。
char*
lastname(char *path)
{
  char *p;

  for(p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  return p + 1;
}

// 找到了一个匹配的文件 file：
//   - 有 -exec：fork 一个子进程执行 "cmd... file"
//   - 没有 -exec：直接打印路径
void
report(char *file)
{
  int pid;

  if(nexec == 0){
    printf("%s\n", file);
    return;
  }

  // 把文件名接到命令词后面：["grep","hello"] + file -> ["grep","hello",file,0]
  execcmd[nexec] = file;
  execcmd[nexec + 1] = 0;

  if((pid = fork()) < 0){
    fprintf(2, "find: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    // 子进程：被 exec 替换掉；执行不到下一行才说明 exec 失败
    exec(execcmd[0], execcmd);
    fprintf(2, "find: exec %s failed\n", execcmd[0]);
    exit(1);
  }
  // 父进程等子进程做完，保证输出顺序稳定
  wait(0);
}

// 递归遍历 path
void
find(char *path)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE:
  case T_FILE:
    // 普通文件（find 的起点本身也可能是文件）
    if(strcmp(lastname(path), name) == 0)
      report(path);
    break;

  case T_DIR:
    // 拼出 "path/" 前缀，后面往里追加每一项的名字
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)          // 空槽位
        continue;
      if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
        continue;               // 不递归进 . 和 ..，否则会无限递归
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;            // de.name 只有 14 字节且不保证以 0 结尾
      find(buf);                // 递归：文件会被上面的 T_FILE 分支处理
    }
    break;
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    fprintf(2, "usage: find dir name [-exec cmd args...]\n");
    exit(1);
  }

  name = argv[2];

  // 解析可选的 -exec 部分
  if(argc > 3){
    if(strcmp(argv[3], "-exec") != 0){
      fprintf(2, "find: expected -exec, got %s\n", argv[3]);
      exit(1);
    }
    for(int i = 4; i < argc; i++){
      if(nexec >= MAXARG - 2){    // 还要留出文件名和结尾的 0
        fprintf(2, "find: too many arguments\n");
        exit(1);
      }
      execcmd[nexec++] = argv[i];
    }
    if(nexec == 0){
      fprintf(2, "find: -exec needs a command\n");
      exit(1);
    }
  }

  find(argv[1]);
  exit(0);
}
