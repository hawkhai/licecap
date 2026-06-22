/*
  Cockos WDL - LICE - Lightweight Image Compositing Engine
  Copyright (C) 2007 and later, Cockos Incorporated
  File: lice_gif.cpp (GIF loading for LICE)
  See lice.h for license and other information
*/

#include "lice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "../wdltypes.h"
#include "../filewrite.h"

extern "C" {

#include "../giflib/gif_lib.h"
//int _GifError;
};


struct liceGifWriteRec
{
  GifFileType *f;
  WDL_FileWrite *fh;
  ColorMapObject *cmap;
  GifPixelType *linebuf;
  LICE_IBitmap *prevframe; // used when multiframe, transalpha<0
  void *last_octree;
  LICE_pixel last_palette[256];
  unsigned char from15to8bit[32][32][32];//r,g,b

  int transalpha;
  int w,h;
  bool append;
  bool dither;
  bool has_had_frame;
  bool has_global_cmap; 

  bool has_from15to8bit; // set when last_octree has been generated into from15to8bit
};

static inline GifPixelType QuantPixel(LICE_pixel p, liceGifWriteRec *wr)
{
  return wr->from15to8bit[LICE_GETR(p)>>3][LICE_GETG(p)>>3][LICE_GETB(p)>>3];
}

static int generate_palette_from_octree(void *ww, void *octree, int numcolors)
{
  liceGifWriteRec  *wr = (liceGifWriteRec *)ww;
  if (!octree||!ww||numcolors>256) return 0;

  ColorMapObject *cmap = wr->cmap;
  
  int palette_sz=0;

  // store palette
  {
    LICE_pixel* palette=wr->last_palette;

    palette_sz = LICE_ExtractOctreePalette(octree, palette);

    int i;
    for (i = 0; i < palette_sz; ++i)
    {
      cmap->Colors[i].Red = LICE_GETR(palette[i]);
      cmap->Colors[i].Green = LICE_GETG(palette[i]);
      cmap->Colors[i].Blue = LICE_GETB(palette[i]);
    }
    for (i = palette_sz; i < numcolors; ++i)
    {
      cmap->Colors[i].Red = cmap->Colors[i].Green = cmap->Colors[i].Blue = 0;   
    }
  }

  wr->has_from15to8bit = false;
  wr->has_global_cmap=true;

  return palette_sz;
}

static void generate15to8(void *ww, void *octree)
{
  liceGifWriteRec  *wr = (liceGifWriteRec *)ww;
  if (!octree||!ww) return;

  // map palette to 16 bit
  unsigned char r,g,b;
  for(r=0;r<32;r++)  
  {
    unsigned char cr = r<<3;
    for (g=0;g<32;g++)
    {
      unsigned char cg = g<<3;
      for (b=0;b<32;b++)
      {
        unsigned char cb = b<<3;
        LICE_pixel col = LICE_RGBA(cr,cg,cb,0);
        wr->from15to8bit[r][g][b] = LICE_FindInOctree(octree, col);
      }
    }
  }
  wr->has_from15to8bit=true;
}

int LICE_SetGIFColorMapFromOctree(void *ww, void *octree, int numcolors)
{
  const int rv = generate_palette_from_octree(ww,octree,numcolors);
  generate15to8(ww,octree);
  return rv;
}

unsigned int LICE_WriteGIFGetSize(void *handle)
{
  if (handle)
  {
    liceGifWriteRec *wr = (liceGifWriteRec*)handle;
    if (wr->fh) return (unsigned int) ((WDL_FileWrite *)wr->fh)->GetPosition();
  }
  return 0;
}

bool LICE_WriteGIFFrame(void *handle, LICE_IBitmap *frame, int xpos, int ypos, bool perImageColorMap, int frame_delay, int nreps)
{
  liceGifWriteRec *wr = (liceGifWriteRec*)handle;
  if (!wr) return false;

  bool isFirst=false;
  if (!wr->has_had_frame)
  {
    wr->has_had_frame=true;
    isFirst=true;

    if (!perImageColorMap && !wr->has_global_cmap)
    {
      const int ccnt = 256 - (wr->transalpha?1:0);
      void* octree = wr->last_octree;
      if (!octree) wr->last_octree = octree = LICE_CreateOctree(ccnt);
      else LICE_ResetOctree(octree,ccnt);

      if (octree) 
      {
        LICE_BuildOctree(octree, frame);
        // sets has_global_cmap
        int pcnt = generate_palette_from_octree(wr, octree, ccnt);

        if (pcnt < 256 && wr->transalpha) pcnt++;
        int nb = 1;
        while (nb < 8 && (1<<nb) < pcnt) nb++;
        wr->cmap->ColorCount = 1<<nb;
        wr->cmap->BitsPerPixel=nb;
      }
    }

    if (!wr->append) EGifPutScreenDesc(wr->f,wr->w,wr->h,8,0,wr->has_global_cmap ? wr->cmap : 0);

  }

  int usew=frame->getWidth(), useh=frame->getHeight();
  if (xpos+usew > wr->w) usew = wr->w-xpos;
  if (ypos+useh > wr->h) useh = wr->h-ypos;
  if (usew<1||useh<1) return false;

  int pixcnt=usew*useh;

  const int trans_chan_mask = wr->transalpha&0xff; // -1 means 0xff by default, user can change this accordingly
  const LICE_pixel trans_mask = LICE_RGBA(trans_chan_mask,trans_chan_mask,trans_chan_mask,0);
  const bool advanced_trans_stats = !!(wr->transalpha&0x100);

  if (perImageColorMap && !wr->has_global_cmap)
  {
    const int ccnt = 256 - (wr->transalpha?1:0);
    void* octree = wr->last_octree;
    if (!octree) wr->last_octree = octree = LICE_CreateOctree(ccnt);
    else LICE_ResetOctree(octree,ccnt);
    if (octree) 
    {
      if ((!isFirst || frame_delay) && wr->transalpha<0 && wr->prevframe)
      {
        LICE_SubBitmap tmpprev(wr->prevframe, xpos, ypos, usew, useh);
        int pc=LICE_BuildOctreeForDiff(octree,frame,&tmpprev,trans_mask);
        if (!advanced_trans_stats) pixcnt = pc;
      }
      else if (wr->transalpha>0)
        pixcnt=LICE_BuildOctreeForAlpha(octree, frame,wr->transalpha&0xff);
      else
        LICE_BuildOctree(octree, frame);

        // sets has_global_cmap (clear below)
      int pcnt = generate_palette_from_octree(wr, octree, ccnt);

      wr->has_global_cmap=false;
      if (pcnt < 256 && wr->transalpha) pcnt++;
      int nb = 1;
      while (nb < 8 && (1<<nb) < pcnt) nb++;
      wr->cmap->ColorCount = 1<<nb;
      wr->cmap->BitsPerPixel=nb;
    }
  }

  if (!wr->has_from15to8bit && pixcnt > 40000 && wr->last_octree)
  {
    generate15to8(wr,wr->last_octree);
  }

  const unsigned char transparent_pix = wr->cmap->ColorCount-1;
  unsigned char gce[4] = { 0, };
  if (wr->transalpha)
  {
    gce[0] |= 1;
    gce[3] = transparent_pix;
  }

  int a = frame_delay/10;
  if(a<1&&frame_delay)a=1;
  else if (a>60000) a=60000;
  gce[1]=(a)&255;
  gce[2]=(a)>>8;

  if (isFirst && frame_delay && nreps!=1 && !wr->append)
  {
    int nr = nreps > 1 && nreps <= 65536 ? nreps-1 : 0;
    unsigned char ext[]={0xB, 'N','E','T','S','C','A','P','E','2','.','0',3,1,(unsigned char) (nr&0xff), (unsigned char) ((nr>>8)&0xff)};
    EGifPutExtension(wr->f,0xFF, sizeof(ext),ext);
  }

  if (gce[0]||gce[1]||gce[2])
    EGifPutExtension(wr->f, 0xF9, sizeof(gce), gce);


  EGifPutImageDesc(wr->f, xpos, ypos, usew,useh, 0, wr->has_global_cmap ? NULL : wr->cmap); 

  GifPixelType *linebuf = wr->linebuf;
  int y;

  void *use_octree = wr->has_from15to8bit ? NULL : wr->last_octree;

  if ((!isFirst || frame_delay) && wr->transalpha<0)
  {
    bool ignFr=false;
    if (!wr->prevframe)
    {
      ignFr=true;
      wr->prevframe = new WDL_NEW LICE_MemBitmap(wr->w,wr->h);
      LICE_Clear(wr->prevframe,0);
    }

    LICE_SubBitmap tmp(wr->prevframe,xpos,ypos,usew,useh);

    LICE_pixel last_pixel_rgb=0;
    GifPixelType last_pixel_idx=transparent_pix;

    int pix_stats[256];
    if (advanced_trans_stats) memset(pix_stats,0,sizeof(pix_stats));
    pix_stats[transparent_pix] = -8;

    for(y=0;y<useh;y++)
    {
      int rdy=y,rdy2=y;
      if (frame->isFlipped()) rdy = frame->getHeight()-1-y;
      if (tmp.isFlipped()) rdy2 = tmp.getHeight()-1-y;
      const LICE_pixel *in = frame->getBits() + rdy*frame->getRowSpan();
      const LICE_pixel *in2 = tmp.getBits() + rdy2*tmp.getRowSpan();
      int x;

      if (advanced_trans_stats)
      {
        if (use_octree) for(x=0;x<usew;x++)
        {
          const LICE_pixel p = in[x]&trans_mask;
          if (last_pixel_idx == transparent_pix || last_pixel_rgb!=p)
          {
            if (ignFr || p != (in2[x]&trans_mask)) last_pixel_idx = LICE_FindInOctree(use_octree,p);
            else 
            {
              const GifPixelType np = LICE_FindInOctree(use_octree,p);
              if (p != (wr->last_palette[np]&trans_mask) || pix_stats[transparent_pix] > pix_stats[np])
                last_pixel_idx = transparent_pix;
              else 
                last_pixel_idx = np;
            }
          }
          linebuf[x] = last_pixel_idx;
          pix_stats[last_pixel_idx]++;
          last_pixel_rgb = p;
        }
        else for(x=0;x<usew;x++)
        {
          const LICE_pixel p = in[x]&trans_mask;
          if (last_pixel_idx == transparent_pix || last_pixel_rgb!=p)
          {
            if (ignFr || p != (in2[x]&trans_mask)) last_pixel_idx = QuantPixel(p,wr);
            else 
            {
              const GifPixelType np = QuantPixel(p,wr);

              if (p != (wr->last_palette[np]&trans_mask) || pix_stats[transparent_pix] > pix_stats[np])
                last_pixel_idx = transparent_pix;
              else 
                last_pixel_idx = np;
            }
          }
          linebuf[x] = last_pixel_idx;
          pix_stats[last_pixel_idx]++;
          last_pixel_rgb = p;
        }
      }
      else
      {
        // optimize solids by reusing the same value if previous rgb was the same, also avoid switching between
        // from color to transparent if the color hasn't changed
        if (use_octree) for(x=0;x<usew;x++)
        {
          const LICE_pixel p = in[x]&trans_mask;
          if (last_pixel_idx == transparent_pix || last_pixel_rgb!=p)
          {
            if (ignFr || p != (in2[x]&trans_mask)) last_pixel_idx = LICE_FindInOctree(use_octree,last_pixel_rgb = p);
            else last_pixel_idx = transparent_pix;
          }
          linebuf[x] = last_pixel_idx;
        }
        else for(x=0;x<usew;x++)
        {
          const LICE_pixel p = in[x]&trans_mask;
          if (last_pixel_idx == transparent_pix || last_pixel_rgb!=p)
          {
            if (ignFr || p != (in2[x]&trans_mask)) last_pixel_idx = QuantPixel(last_pixel_rgb = p,wr);
            else last_pixel_idx = transparent_pix;
          }
          linebuf[x] = last_pixel_idx;
        }
      }


      EGifPutLine(wr->f, linebuf, usew);
    }

    LICE_Blit(&tmp,frame,0,0,0,0,usew,useh,1.0f,LICE_BLIT_MODE_COPY);
    
  }
  else if (wr->transalpha>0)
  {
    const unsigned int al = wr->transalpha&0xff;
    for(y=0;y<useh;y++)
    {
      int rdy=y;
      if (frame->isFlipped()) rdy = frame->getHeight()-1-y;
      const LICE_pixel *in = frame->getBits() + rdy*frame->getRowSpan();
      int x;
      if (use_octree) for(x=0;x<usew;x++)
      {
        const LICE_pixel p = in[x];
        if (LICE_GETA(p)<al) linebuf[x]=transparent_pix;
        else linebuf[x] = LICE_FindInOctree(use_octree,p);
      }
      else for(x=0;x<usew;x++)
      {
        const LICE_pixel p = in[x];
        if (LICE_GETA(p)<al) linebuf[x]=transparent_pix;
        else linebuf[x] = QuantPixel(p,wr);
      }
      EGifPutLine(wr->f, linebuf, usew);
    }
  }
  else for(y=0;y<useh;y++)
  {
    int rdy=y;
    if (frame->isFlipped()) rdy = frame->getHeight()-1-y;
    const LICE_pixel *in = frame->getBits() + rdy*frame->getRowSpan();
    int x;
    if (use_octree) for(x=0;x<usew;x++) linebuf[x] = LICE_FindInOctree(use_octree,in[x]);
    else for(x=0;x<usew;x++) linebuf[x] = QuantPixel(in[x],wr);
    EGifPutLine(wr->f, linebuf, usew);
  }

  return true;
}

static int writefunc_fh(GifFileType *fh, const GifByteType *buf, int sz) 
{  
  return ((WDL_FileWrite *)fh->UserData)->Write(buf,sz);
}

void *LICE_WriteGIFBeginNoFrame(const char *filename, int w, int h, int transparent_alpha, bool dither, bool is_append)
{
  WDL_FileWrite *fp = new WDL_FileWrite(filename,1,65536,16,16,is_append);
  if (!fp->IsOpen()) 
  {
    delete fp;
    return NULL;
  }


  EGifSetGifVersion("89a");

  
  GifFileType *f = EGifOpen(fp,writefunc_fh);
  if (!f) 
  {
    delete fp;
    return NULL;
  }

  liceGifWriteRec *wr = (liceGifWriteRec*)calloc(sizeof(liceGifWriteRec),1);
  wr->f = f;
  wr->fh = fp;
  wr->append = is_append;
  wr->dither = dither;
  wr->w=w;
  wr->h=h;
  wr->cmap = (ColorMapObject*)calloc(sizeof(ColorMapObject)+256*sizeof(GifColorType),1);
  wr->cmap->Colors = (GifColorType*)(wr->cmap+1);
  wr->cmap->ColorCount=256;
  wr->cmap->BitsPerPixel=8;
  wr->has_had_frame=false;
  wr->has_global_cmap=false;
  wr->has_from15to8bit=false;
  wr->last_octree=NULL;

  wr->linebuf = (GifPixelType*)malloc(wr->w*sizeof(GifPixelType));
  wr->transalpha = transparent_alpha;

  return wr;
}
void *LICE_WriteGIFBegin(const char *filename, LICE_IBitmap *firstframe, int transparent_alpha, int frame_delay, bool dither, int nreps)
{
  if (!firstframe) return NULL;

  void *wr=LICE_WriteGIFBeginNoFrame(filename,firstframe->getWidth(),firstframe->getHeight(),transparent_alpha,dither);
  if (wr) LICE_WriteGIFFrame(wr,firstframe,0,0,true,frame_delay,nreps);

  return wr;
}



bool LICE_WriteGIFEnd(void *handle)
{
  liceGifWriteRec *wr = (liceGifWriteRec*)handle;
  if (!wr) return false;

  int ret = EGifCloseFile(wr->f);

  free(wr->linebuf);
  free(wr->cmap);
  if (wr->last_octree) LICE_DestroyOctree(wr->last_octree);

  delete wr->prevframe;
  delete wr->fh;

  free(wr);

  return ret!=GIF_ERROR;
}


bool LICE_WriteGIF(const char *filename, LICE_IBitmap *bmp, int transparent_alpha, bool dither)
{
  // todo: alpha?
  if (!bmp) return false;

  int has_transparent = 0;
  if (transparent_alpha>0)
  {
    int y=bmp->getHeight();
    LICE_pixel *p = bmp->getBits();
    int w = bmp->getWidth();
    while (y--&&!has_transparent)
    {
      int x=w;
      while(x--)
      {
        if (LICE_GETA(*p) < (unsigned int)transparent_alpha)
        {
          has_transparent=1;
          break;
        }
        p++;
      }
      p+=bmp->getRowSpan()-w;
    }
  }


  void *wr=LICE_WriteGIFBeginNoFrame(filename,bmp->getWidth(),bmp->getHeight(),has_transparent?transparent_alpha:0,dither);
  if (!wr)  return false;
  
  LICE_WriteGIFFrame(wr,bmp,0,0,false,0);

  return LICE_WriteGIFEnd(wr);
}

typedef struct WebPMux WebPMux;

typedef enum WebPMuxError {
  WEBP_MUX_OK = 1,
  WEBP_MUX_NOT_FOUND = 0,
  WEBP_MUX_INVALID_ARGUMENT = -1,
  WEBP_MUX_BAD_DATA = -2,
  WEBP_MUX_MEMORY_ERROR = -3,
  WEBP_MUX_NOT_ENOUGH_DATA = -4
} WebPMuxError;

typedef enum WebPChunkId {
  WEBP_CHUNK_VP8X,
  WEBP_CHUNK_ICCP,
  WEBP_CHUNK_ANIM,
  WEBP_CHUNK_ANMF,
  WEBP_CHUNK_DEPRECATED,
  WEBP_CHUNK_ALPHA,
  WEBP_CHUNK_IMAGE,
  WEBP_CHUNK_EXIF,
  WEBP_CHUNK_XMP,
  WEBP_CHUNK_UNKNOWN,
  WEBP_CHUNK_NIL
} WebPChunkId;

typedef enum WebPMuxAnimDispose {
  WEBP_MUX_DISPOSE_NONE,
  WEBP_MUX_DISPOSE_BACKGROUND
} WebPMuxAnimDispose;

typedef enum WebPMuxAnimBlend {
  WEBP_MUX_BLEND,
  WEBP_MUX_NO_BLEND
} WebPMuxAnimBlend;

struct WebPData {
  const unsigned char *bytes;
  size_t size;
};

struct WebPMuxAnimParams {
  unsigned int bgcolor;
  int loop_count;
};

struct WebPMuxFrameInfo {
  WebPData bitstream;
  int x_offset;
  int y_offset;
  int duration;
  WebPChunkId id;
  WebPMuxAnimDispose dispose_method;
  WebPMuxAnimBlend blend_method;
  unsigned int pad[1];
};

#define LICE_WEBP_MUX_ABI_VERSION 0x0109

struct liceWebPLib
{
  bool tried;
  bool ok;
#ifdef _WIN32
  HMODULE webp;
  HMODULE mux;
#else
  void *webp;
  void *mux;
#endif

  size_t (*WebPEncodeBGRA)(const unsigned char *, int, int, int, float, unsigned char **);
  size_t (*WebPEncodeLosslessBGRA)(const unsigned char *, int, int, int, unsigned char **);
  void (*WebPFree)(void *);
  WebPMux *(*WebPNewInternal)(int);
  void (*WebPMuxDelete)(WebPMux *);
  WebPMuxError (*WebPMuxSetAnimationParams)(WebPMux *, const WebPMuxAnimParams *);
  WebPMuxError (*WebPMuxSetCanvasSize)(WebPMux *, int, int);
  WebPMuxError (*WebPMuxPushFrame)(WebPMux *, const WebPMuxFrameInfo *, int);
  WebPMuxError (*WebPMuxAssemble)(WebPMux *, WebPData *);
};

static liceWebPLib g_webp_lib;

static void *lice_webp_load_library(const char **names)
{
  int x;
  for (x=0; names[x]; ++x)
  {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(names[x]);
#else
    void *h = dlopen(names[x], RTLD_LAZY);
#endif
    if (h) return h;
  }
  return NULL;
}

static void *lice_webp_get_proc(void *lib, const char *name)
{
  if (!lib) return NULL;
#ifdef _WIN32
  return (void *)GetProcAddress((HMODULE)lib, name);
#else
  return dlsym(lib, name);
#endif
}

static bool lice_webp_load()
{
  if (g_webp_lib.tried) return g_webp_lib.ok;
  g_webp_lib.tried = true;

#ifdef _WIN32
  const char *webp_names[] = { "libwebp.dll", "webp.dll", "libwebp-7.dll", NULL };
  const char *mux_names[] = { "libwebpmux.dll", "webpmux.dll", "libwebpmux-3.dll", NULL };
#else
#ifdef __APPLE__
  const char *webp_names[] = { "libwebp.dylib", "libwebp.7.dylib", NULL };
  const char *mux_names[] = { "libwebpmux.dylib", "libwebpmux.3.dylib", NULL };
#else
  const char *webp_names[] = { "libwebp.so", "libwebp.so.7", NULL };
  const char *mux_names[] = { "libwebpmux.so", "libwebpmux.so.3", NULL };
#endif
#endif

#ifdef _WIN32
  g_webp_lib.webp = (HMODULE)lice_webp_load_library(webp_names);
  g_webp_lib.mux = (HMODULE)lice_webp_load_library(mux_names);
#else
  g_webp_lib.webp = lice_webp_load_library(webp_names);
  g_webp_lib.mux = lice_webp_load_library(mux_names);
#endif
  if (!g_webp_lib.webp || !g_webp_lib.mux) return false;

  *(void **)&g_webp_lib.WebPEncodeBGRA = lice_webp_get_proc(g_webp_lib.webp, "WebPEncodeBGRA");
  *(void **)&g_webp_lib.WebPEncodeLosslessBGRA = lice_webp_get_proc(g_webp_lib.webp, "WebPEncodeLosslessBGRA");
  *(void **)&g_webp_lib.WebPFree = lice_webp_get_proc(g_webp_lib.webp, "WebPFree");
  *(void **)&g_webp_lib.WebPNewInternal = lice_webp_get_proc(g_webp_lib.mux, "WebPNewInternal");
  *(void **)&g_webp_lib.WebPMuxDelete = lice_webp_get_proc(g_webp_lib.mux, "WebPMuxDelete");
  *(void **)&g_webp_lib.WebPMuxSetAnimationParams = lice_webp_get_proc(g_webp_lib.mux, "WebPMuxSetAnimationParams");
  *(void **)&g_webp_lib.WebPMuxSetCanvasSize = lice_webp_get_proc(g_webp_lib.mux, "WebPMuxSetCanvasSize");
  *(void **)&g_webp_lib.WebPMuxPushFrame = lice_webp_get_proc(g_webp_lib.mux, "WebPMuxPushFrame");
  *(void **)&g_webp_lib.WebPMuxAssemble = lice_webp_get_proc(g_webp_lib.mux, "WebPMuxAssemble");

  g_webp_lib.ok = g_webp_lib.WebPEncodeBGRA && g_webp_lib.WebPEncodeLosslessBGRA &&
                  g_webp_lib.WebPFree && g_webp_lib.WebPNewInternal &&
                  g_webp_lib.WebPMuxDelete && g_webp_lib.WebPMuxSetAnimationParams &&
                  g_webp_lib.WebPMuxSetCanvasSize && g_webp_lib.WebPMuxPushFrame &&
                  g_webp_lib.WebPMuxAssemble;
  return g_webp_lib.ok;
}

static unsigned char *lice_webp_encode_bitmap(LICE_IBitmap *bmp, float quality, bool lossless, size_t *sizeOut)
{
  if (sizeOut) *sizeOut = 0;
  if (!bmp || !sizeOut || !lice_webp_load()) return NULL;

  const int w = bmp->getWidth();
  const int h = bmp->getHeight();
  if (w < 1 || h < 1 || w > 16383 || h > 16383) return NULL;

  const int stride = w*4;
  unsigned char *bgra = (unsigned char *)malloc(stride*h);
  if (!bgra) return NULL;

  int y;
  for (y=0; y<h; ++y)
  {
    const int sy = bmp->isFlipped() ? h-1-y : y;
    const LICE_pixel *src = bmp->getBits() + sy*bmp->getRowSpan();
    unsigned char *dst = bgra + y*stride;
    int x;
    for (x=0; x<w; ++x)
    {
      const LICE_pixel p = src[x];
      dst[x*4+0] = (unsigned char)LICE_GETB(p);
      dst[x*4+1] = (unsigned char)LICE_GETG(p);
      dst[x*4+2] = (unsigned char)LICE_GETR(p);
      dst[x*4+3] = (unsigned char)(LICE_GETA(p) ? LICE_GETA(p) : 255);
    }
  }

  unsigned char *out = NULL;
  const size_t outsz = lossless ?
    g_webp_lib.WebPEncodeLosslessBGRA(bgra, w, h, stride, &out) :
    g_webp_lib.WebPEncodeBGRA(bgra, w, h, stride, quality, &out);

  free(bgra);
  if (!out || !outsz) return NULL;

  *sizeOut = outsz;
  return out;
}

struct liceWebPWriteRec
{
  char *filename;
  WebPMux *mux;
  int w;
  int h;
  int frame_count;
  float quality;
  bool lossless;
  unsigned int output_size;
};

void *LICE_WriteWebPBeginNoFrame(const char *filename, int w, int h, int nreps, float quality, bool lossless)
{
  if (!filename || !*filename || w < 1 || h < 1 || w > 16383 || h > 16383 || !lice_webp_load()) return NULL;

  liceWebPWriteRec *wr = (liceWebPWriteRec *)calloc(sizeof(liceWebPWriteRec),1);
  if (!wr) return NULL;

  wr->filename = (char *)malloc(strlen(filename)+1);
  if (!wr->filename)
  {
    free(wr);
    return NULL;
  }
  strcpy(wr->filename, filename);

  wr->mux = g_webp_lib.WebPNewInternal(LICE_WEBP_MUX_ABI_VERSION);
  if (!wr->mux)
  {
    free(wr->filename);
    free(wr);
    return NULL;
  }

  wr->w = w;
  wr->h = h;
  wr->quality = quality < 0.0f ? 0.0f : (quality > 100.0f ? 100.0f : quality);
  wr->lossless = lossless;

  WebPMuxAnimParams params;
  params.bgcolor = 0x000000ff;
  params.loop_count = nreps;

  if (g_webp_lib.WebPMuxSetCanvasSize(wr->mux, w, h) != WEBP_MUX_OK ||
      g_webp_lib.WebPMuxSetAnimationParams(wr->mux, &params) != WEBP_MUX_OK)
  {
    LICE_WriteWebPEnd(wr);
    return NULL;
  }

  return wr;
}

void *LICE_WriteWebPBegin(const char *filename, LICE_IBitmap *firstframe, int frame_delay, int nreps, float quality, bool lossless)
{
  if (!firstframe) return NULL;
  void *wr = LICE_WriteWebPBeginNoFrame(filename, firstframe->getWidth(), firstframe->getHeight(), nreps, quality, lossless);
  if (wr && !LICE_WriteWebPFrame(wr, firstframe, frame_delay))
  {
    LICE_WriteWebPEnd(wr);
    return NULL;
  }
  return wr;
}

bool LICE_WriteWebPFrame(void *handle, LICE_IBitmap *frame, int frame_delay)
{
  liceWebPWriteRec *wr = (liceWebPWriteRec *)handle;
  if (!wr || !wr->mux || !frame) return false;
  if (frame->getWidth() != wr->w || frame->getHeight() != wr->h) return false;

  size_t bitstream_size = 0;
  unsigned char *bitstream = lice_webp_encode_bitmap(frame, wr->quality, wr->lossless, &bitstream_size);
  if (!bitstream) return false;

  WebPMuxFrameInfo info;
  memset(&info, 0, sizeof(info));
  info.bitstream.bytes = bitstream;
  info.bitstream.size = bitstream_size;
  info.duration = frame_delay < 1 ? 1 : frame_delay;
  info.id = WEBP_CHUNK_ANMF;
  info.dispose_method = WEBP_MUX_DISPOSE_NONE;
  info.blend_method = WEBP_MUX_NO_BLEND;

  const bool ok = g_webp_lib.WebPMuxPushFrame(wr->mux, &info, 1) == WEBP_MUX_OK;
  g_webp_lib.WebPFree(bitstream);

  if (ok) ++wr->frame_count;
  return ok;
}

unsigned int LICE_WriteWebPGetSize(void *handle)
{
  liceWebPWriteRec *wr = (liceWebPWriteRec *)handle;
  return wr ? wr->output_size : 0;
}

bool LICE_WriteWebPEnd(void *handle)
{
  liceWebPWriteRec *wr = (liceWebPWriteRec *)handle;
  if (!wr) return false;

  bool ok = false;
  if (wr->mux && wr->frame_count > 0)
  {
    WebPData data;
    data.bytes = NULL;
    data.size = 0;

    if (g_webp_lib.WebPMuxAssemble(wr->mux, &data) == WEBP_MUX_OK && data.bytes && data.size > 0)
    {
      WDL_FileWrite fp(wr->filename, 1, 65536, 16, 16, false);
      if (fp.IsOpen())
      {
        size_t written = 0;
        while (written < data.size)
        {
          int amt = data.size - written > 0x40000000 ? 0x40000000 : (int)(data.size - written);
          int rv = fp.Write(data.bytes + written, amt);
          if (rv <= 0) break;
          written += rv;
        }
        ok = written == data.size;
        if (ok) wr->output_size = (unsigned int)(data.size > 0xffffffffU ? 0xffffffffU : data.size);
      }
    }

    if (data.bytes) g_webp_lib.WebPFree((void *)data.bytes);
  }

  if (wr->mux) g_webp_lib.WebPMuxDelete(wr->mux);
  free(wr->filename);
  free(wr);

  return ok;
}

bool LICE_WriteWebP(const char *filename, LICE_IBitmap *bmp, float quality, bool lossless)
{
  if (!filename || !*filename || !bmp) return false;

  size_t bitstream_size = 0;
  unsigned char *bitstream = lice_webp_encode_bitmap(bmp, quality, lossless, &bitstream_size);
  if (!bitstream) return false;

  bool ok = false;
  WDL_FileWrite fp(filename, 1, 65536, 16, 16, false);
  if (fp.IsOpen())
  {
    size_t written = 0;
    while (written < bitstream_size)
    {
      int amt = bitstream_size - written > 0x40000000 ? 0x40000000 : (int)(bitstream_size - written);
      int rv = fp.Write(bitstream + written, amt);
      if (rv <= 0) break;
      written += rv;
    }
    ok = written == bitstream_size;
  }

  g_webp_lib.WebPFree(bitstream);
  return ok;
}
