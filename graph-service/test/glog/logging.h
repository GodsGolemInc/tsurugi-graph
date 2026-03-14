#pragma once
#include <iostream>
#define LOG(X) if(false) std::cerr << #X << ": "
#define VLOG(X) if(false) std::cerr << "V" << #X << ": "
#define INFO 0
#define ERROR 1
#define WARNING 2
#define FATAL 3
