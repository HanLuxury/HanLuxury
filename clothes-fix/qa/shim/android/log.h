#pragma once
#include <cstdio>
enum {ANDROID_LOG_INFO=4,ANDROID_LOG_WARN=5,ANDROID_LOG_ERROR=6};
#define __android_log_print(prio,tag,...) (std::fprintf(stderr,"[%s] ",tag),std::fprintf(stderr,__VA_ARGS__),std::fputc('\n',stderr),0)
#define __android_log_write(prio,tag,text) (std::fprintf(stderr,"[%s] %s\n",tag,text),0)
