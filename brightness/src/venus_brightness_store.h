#ifndef VENUS_BRIGHTNESS_STORE_H
#define VENUS_BRIGHTNESS_STORE_H
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define VENUS_BRIGHTNESS_FILE "/private/var/mobile/Library/Preferences/com.hakios.venus-brightness"
static int venus_brightness_store_read(const char *path)
{
    char text[64];
    struct stat st;
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return -1;
    if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>=63){close(fd);return -1;}
    ssize_t n=read(fd,text,sizeof(text)-1);
    close(fd);
    if(n!=st.st_size)return -1;
    text[n]=0;
    const char prefix[]="VENUS_BRIGHTNESS_V1 ";
    if(strncmp(text,prefix,sizeof(prefix)-1))return -1;
    char *end=NULL;errno=0;
    long level=strtol(text+sizeof(prefix)-1,&end,10);
    if(errno||end==text+sizeof(prefix)-1||strcmp(end,"\n")||level<4||level>2047)return -1;
    return (int)level;
}
static int venus_brightness_store_write(const char *path,int level)
{
    char temporary[1024],text[64];
    if(level<4||level>2047||snprintf(temporary,sizeof(temporary),"%s.XXXXXX",path)>=(int)sizeof(temporary))return -1;
    int n=snprintf(text,sizeof(text),"VENUS_BRIGHTNESS_V1 %d\n",level);
    int fd=mkstemp(temporary);
    if(fd<0)return -1;
    int ok=write(fd,text,(size_t)n)==n;
    if(close(fd)!=0)ok=0;
    if(ok&&rename(temporary,path)==0)return 0;
    unlink(temporary);
    return -1;
}
#endif
