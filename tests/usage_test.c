/* usage_test.c - confere usage.h em Linux. Uso:
 *   mke2fs -q -t ext4 -d <pasta_de_teste> img   (imagem ext4 com pastas roms/, Music/, DCIM/ ...)
 *   gcc -O1 -o usage_test tests/usage_test.c && ./usage_test img
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "../usage.h"
static int fd;
static int rawrd(void *c,uint64_t off,void *buf,uint32_t len){(void)c;return pread(fd,buf,len,(off_t)off)==(ssize_t)len;}
int main(int argc,char**argv){
  fd=open(argv[1],O_RDONLY); off_t sz=lseek(fd,0,SEEK_END);
  Dev2 d; Dev2Init(&d,rawrd,NULL,(uint64_t)sz,512);
  UsageStat *u=calloc(1,sizeof *u);
  int ok=UsageScanPart(&d,0,(uint64_t)sz,u,NULL,NULL);
  UsageSortExt(u);
  printf("ok=%d files=%u dirs=%u used=%llu partial=%d profile=%d linux=%d roms=%d emu=%d dcim=%d\n",ok,u->files,u->dirs,(unsigned long long)u->usedBytes,u->partial,UsageProfile(u),u->hasLinux,u->hasRoms,u->hasEmu,u->hasDcim);
  for(int i=0;i<CT_N;i++)printf("cat%d files=%u bytes=%llu\n",i,u->catFiles[i],(unsigned long long)u->catBytes[i]);
  for(int i=0;i<u->next;i++)printf(".%s %u %llu\n",u->ext[i].ext,u->ext[i].files,(unsigned long long)u->ext[i].bytes);
  return 0;}
