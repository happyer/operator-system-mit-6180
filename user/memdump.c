#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

void memdump(char *fmt, char *data);

int
main(int argc, char *argv[])
{
  if(argc == 1){
    printf("Example 1:\n");
    int a[2] = { 61810, 2025 };
    memdump("ii", (char*) a);
    
    printf("Example 2:\n");
    memdump("S", "a string");
    
    printf("Example 3:\n");
    char *s = "another";
    memdump("s", (char *) &s);

    struct sss {
      char *ptr;
      int num1;
      short num2;
      char byte;
      char bytes[8];
    } example;
    
    example.ptr = "hello";
    example.num1 = 1819438967;
    example.num2 = 100;
    example.byte = 'z';
    strcpy(example.bytes, "xyzzy");
    
    printf("Example 4:\n");
    memdump("pihcS", (char*) &example);
    
    printf("Example 5:\n");
    memdump("sccccc", (char*) &example);
  } else if(argc == 2){
    // format in argv[1], up to 512 bytes of data from standard input.
    char data[512];
    int n = 0;
    memset(data, '\0', sizeof(data));
    while(n < sizeof(data)){
      int nn = read(0, data + n, sizeof(data) - n);
      if(nn <= 0)
        break;
      n += nn;
    }
    memdump(argv[1], data);
  } else {
    printf("Usage: memdump [format]\n");
    exit(1);
  }
  exit(0);
}

// 按 fmt 描述的“类型序列”，把 data 指向的内存解释成对应的 C 类型并打印。
// 每处理一个格式字符，data 就前进该类型占用的字节数（这是本题的核心）。
void
memdump(char *fmt, char *data)
{
  for(char *f = fmt; *f; f++){
    switch(*f){

    case 'i':                        // int：4 字节，按十进制打印
      printf("%d\n", *(int*)data);
      data += sizeof(int);
      break;

    case 'h':                        // short：2 字节，按十进制打印
      printf("%d\n", (int)*(short*)data);
      data += sizeof(short);
      break;

    case 'c':                        // char：1 字节
      printf("%c\n", *data);
      data += sizeof(char);
      break;

    case 'p':                        // 指针：8 字节，按十六进制打印
      printf("%x\n", (uint)(*(uint64*)data & 0xffffffff));
      data += sizeof(void*);
      break;

    case 's':                        // char*：8 字节指针，打印它指向的那个字符串
      printf("%s\n", *(char**)data);
      data += sizeof(char*);
      break;

    case 'S':                        // 以 '\0' 结尾的字符串：原地打印，并跳过整串
      printf("%s\n", data);
      data += strlen(data) + 1;
      break;

    default:
      fprintf(2, "memdump: unknown format character '%c'\n", *f);
      exit(1);
    }
  }
}
