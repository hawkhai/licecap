#include "base_encoder.h"
#include <atlstr.h>
#include <string>
#include <assert.h>

class log_encoder : public base_encoder {

    LICE_IBitmap* lastbm; // set if a new frame is in progress
    std::string fpath;

    int lastbm_coords[4]; // coordinates of previous frame which need to be updated, [2], [3] will always be >0 if in progress
    int lastbm_accumdelay; // delay of previous frame which is latent
    int loopcnt;
    LICE_pixel trans_mask;

    // 返回开始写文件的地址偏移。
    static long appendfile(const char* fpath, const char* data, long length) {
        FILE* fs = _wfopen(CA2W(fpath), L"ab");
        assert(fs);
        if (!fs) return -1;
        fseek(fs, 0, SEEK_END);
        long offset = ftell(fs);
        fwrite(data, 1, length, fs);
        fclose(fs);
        return offset;
    }

public:
    log_encoder(const char* logPath, int use_loopcnt, int trans_chan_mask = 0xff)
    {
        lastbm = NULL;
        memset(lastbm_coords, 0, sizeof(lastbm_coords));
        lastbm_accumdelay = 0;
        fpath = logPath;
        fpath.append(".licecap.log.txt");
        loopcnt = use_loopcnt;
        trans_mask = LICE_RGBA(trans_chan_mask, trans_chan_mask, trans_chan_mask, 0);
    }
    ~log_encoder()
    {
        frame_finish();
        //LICE_WriteGIFEnd(ctx);
        delete lastbm;
    }


    bool frame_compare(LICE_IBitmap* bm, int diffs[4])
    {
        diffs[0] = diffs[1] = 0;
        diffs[2] = bm->getWidth();
        diffs[3] = bm->getHeight();
        return !lastbm || LICE_BitmapCmpEx(lastbm, bm, trans_mask, diffs);
    }

    void frame_finish()
    {
        if (lastbm && lastbm_coords[2] > 0 && lastbm_coords[3] > 0)
        {
            LICE_SubBitmap bm(lastbm, lastbm_coords[0], lastbm_coords[1], lastbm_coords[2], lastbm_coords[3]);

            int del = lastbm_accumdelay;
            if (del < 1) del = 1;

            //LICE_WriteGIFFrame(ctx, &bm, lastbm_coords[0], lastbm_coords[1], true, del, loopcnt);
        }
        lastbm_accumdelay = 0;
        lastbm_coords[2] = lastbm_coords[3] = 0;
    }

    void frame_advancetime(int amt)
    {
        lastbm_accumdelay += amt;
    }

    void frame_new(LICE_IBitmap* ref, int ix, int iy, int iw, int ih)
    {
        if (iw > 0 && ih > 0)
        {
            frame_finish();

            lastbm_coords[0] = ix;
            lastbm_coords[1] = iy;
            lastbm_coords[2] = iw;
            lastbm_coords[3] = ih;

            if (!lastbm) lastbm = LICE_CreateMemBitmap(ref->getWidth(), ref->getHeight());
            LICE_Blit(lastbm, ref, ix, iy, ix, iy, iw, ih, 1.0f, LICE_BLIT_MODE_COPY);

            // 整张图算 hash...
            uint64 crc = 0;
            int size = ref->getWidth() * ref->getHeight() * sizeof(LICE_pixel);
            crc = crc64((const uchar*)ref->getBits(), size, crc);

            static uint64 lastcrc = 0;
            if (crc != lastcrc) {
                lastcrc = crc;
                char buffer[1024];
                uint64 crcp = crc & 0x7fffffff;
                sprintf(buffer, "{ \"tick\":%lld, \"crc\":%llu, \"w\":%d, \"h\":%d }\r\n", //
                    GetTickCount64(), crcp, ref->getWidth(), ref->getHeight()
                );
                appendfile(fpath.c_str(), buffer, strlen(buffer));
            }
        }
    }

    void clear_history() // forces next frame to be a fully new frame
    {
        frame_finish();
        delete lastbm;
        lastbm = NULL;
    }
    LICE_IBitmap* prev_bitmap() { return lastbm; }
};

