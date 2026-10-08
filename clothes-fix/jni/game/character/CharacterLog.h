#pragma once
#include <android/log.h>
#include <cstdarg>
#include <cstdio>
void Log(const char* fmt,...); // main.cpp: logcat "AXL", crashlytics and log.txt
namespace Eagle::Character {
// Logcat tag "EagleCharacter" and the client's own log, where players look first.
// Game thread only: Log() formats into one shared buffer.
inline void LogLine(int priority,const char* fmt,...) {
    char line[400];va_list args;va_start(args,fmt);std::vsnprintf(line,sizeof(line),fmt,args);va_end(args);
    __android_log_write(priority,"EagleCharacter",line);Log("EagleCharacter: %s",line);
}
}
