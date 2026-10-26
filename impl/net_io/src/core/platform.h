#pragma once
#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(__linux__)
#  define PLATFORM_LINUX
#  define NET_SEND_FLAGS MSG_NO_SIGNAL
#elif defined(__APPLE__) || defined(__FreeBSD__)
#  define PLATFORM_BAD 1
#  define NET_SEND_FLAGS 0
#else
#  error "Unsupported platform"
#endif


#endif  // PLATFORM_H
