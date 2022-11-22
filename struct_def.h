#ifndef STRUCT_DEF_H
#define STRUCT_DEF_H

#include "qglobal.h"

struct MemoryInfo
{
    qulonglong mem_total;
    qulonglong mem_free;
    qulonglong mem_available;
    qulonglong cached;
    qulonglong buffers;
    qulonglong swap_total;
    qulonglong swap_free;

    qulonglong mem_used;
    qulonglong swap_used;
};

#endif // STRUCT_DEF_H
