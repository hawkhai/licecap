#pragma once

//#include "licecap.rc_mac_dlg"	    
#include <stdio.h>	    
#include <windows.h>	    
#include <process.h>	    
#include <multimon.h>	    
#include "../WDL/swell/swell.h"	    
#include "../WDL/queue.h"	    
#include "../WDL/mutex.h"	    
#include "../WDL/wdlcstring.h"	    
#define LICE_CreateSysBitmap(w,h) new LICE_SysBitmap(w,h)
#define LICE_CreateMemBitmap(w,h) new LICE_MemBitmap(w,h)
#include "../WDL/lice/lice_lcf.h"	    
#include "../WDL/filebrowse.h"	    
#include "../WDL/wdltypes.h"	    
#include "../WDL/wingui/wndsize.h"	    
#include "../WDL/wdlstring.h"	    
#include "licecap_version.h"	    
#include "resource.h"	    
#include "../WDL/swell/swell-dlggen.h"	    

class base_encoder {
public:
    virtual ~base_encoder() = default;
    virtual void frame_new(LICE_IBitmap* ref, int x, int y, int w, int h) = 0;
    virtual void frame_finish() = 0;
    virtual bool frame_compare(LICE_IBitmap* bm, int diffs[4]) = 0;
    virtual void frame_advancetime(int amt) = 0;

    typedef uint32_t        uint;
    typedef signed char     schar;
    typedef unsigned char   uchar;
    typedef unsigned short  ushort;
    typedef int64_t         int64;
    typedef uint64_t        uint64;
#define CV_BIG_INT(n)   n##LL
#define CV_BIG_UINT(n)  n##ULL

    // Computes 64-bit "cyclic redundancy check" sum, as specified in ECMA-182
    uint64 crc64(const uchar* data, size_t size, uint64 crcx)
    {
        static uint64 table[256];
        static bool initialized = false;

        if (!initialized)
        {
            for (int i = 0; i < 256; i++)
            {
                uint64 c = i;
                for (int j = 0; j < 8; j++)
                    c = ((c & 1) ? CV_BIG_UINT(0xc96c5795d7870f42) : 0) ^ (c >> 1);
                table[i] = c;
            }
            initialized = true;
        }

        uint64 crc = ~crcx;
        for (size_t idx = 0; idx < size; idx++) {
            crc = table[(uchar)crc ^ data[idx]] ^ (crc >> 8);
        }
        return ~crc;
    }
};
