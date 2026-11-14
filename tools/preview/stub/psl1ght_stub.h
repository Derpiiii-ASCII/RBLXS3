#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef struct { int dummy; } gcmContextData;
#define SYSUTIL_EXIT_GAME 0x0101
typedef void (*sysutil_cb_t)(u64,u64,void*);
typedef struct { u32 resolution; u32 aspect; } videoDisplayMode;
typedef struct { videoDisplayMode displayMode; } videoState;
typedef struct { u32 width, height; } videoResolution;
typedef struct { u32 resolution, format, pitch, aspect; } videoConfiguration;
#define VIDEO_BUFFER_FORMAT_XRGB 1
#define GCM_FLIP_VSYNC 1
#define VIDEO_RESOLUTION_720 4
#define VIDEO_RESOLUTION_1080 5
#define VIDEO_ASPECT_AUTO 0
typedef struct { u32 status[7]; } padInfo;
typedef struct { u16 ANA_L_H, ANA_L_V, ANA_R_H, ANA_R_V; u8 BTN_CROSS; u16 BTN_L2; u8 BTN_CIRCLE, BTN_START, BTN_UP, BTN_DOWN; } padData;

static gcmContextData g_ctx; static u32 *g_fb[2]; static u32 g_w=1280,g_h=720,g_pitch; static int g_frame=0; static sysutil_cb_t g_cb;
static int rsxInit(gcmContextData **c,u32 a,u32 b,void*h){(void)a;(void)b;(void)h;*c=&g_ctx;return 0;}
static int videoGetState(int a,int b,videoState*s){(void)a;(void)b;s->displayMode.resolution=0;s->displayMode.aspect=0;return 0;}
static int videoGetResolution(u32 r,videoResolution*o){(void)r;o->width=g_w;o->height=g_h;return 0;}
static int videoConfigure(int a,videoConfiguration*c,void*d,int e){(void)a;(void)d;(void)e;g_pitch=c->pitch;return 0;}
static int videoGetResolutionAvailability(u32 a,u32 b,u32 c,u32 d){(void)a;(void)b;(void)c;(void)d;return 1;}
static void gcmSetFlipMode(int m){(void)m;}
static void* rsxMemalign(int al,u32 sz){static int n=0;(void)al;g_fb[n]=calloc(1,sz);return g_fb[n++];}
static int rsxAddressToOffset(void*p,u32*o){(void)p;*o=0;return 0;}
static void gcmSetDisplayBuffer(int i,u32 o,u32 p,u32 w,u32 h){(void)i;(void)o;(void)p;(void)w;(void)h;}
static void gcmResetFlipStatus(void){}
static int gcmGetFlipStatus(void){return 0;}
static void dump(int id,const char*name){FILE*f=fopen(name,"wb");fprintf(f,"P6\n%u %u\n255\n",g_w,g_h);for(u32 y=0;y<g_h;y++)for(u32 x=0;x<g_w;x++){u32 p=g_fb[id][y*(g_pitch/4)+x];fputc((p>>16)&255,f);fputc((p>>8)&255,f);fputc(p&255,f);}fclose(f);}
static void gcmSetFlip(gcmContextData*c,u8 id){(void)c;g_frame++;char n[64];if(g_frame==1||g_frame==90||g_frame==115||g_frame==176||g_frame==250){snprintf(n,64,"frame%d.ppm",g_frame);dump(id,n);}}
static void rsxFlushBuffer(gcmContextData*c){(void)c;}
static void gcmSetWaitFlip(gcmContextData*c){(void)c;}
static int ioPadInit(int n){(void)n;return 0;}
static int ioPadGetInfo(padInfo*i){memset(i,0,sizeof(*i));i->status[0]=1;return 0;}
static int ioPadGetData(int p,padData*d){(void)p;memset(d,0,sizeof(*d));int f=g_frame;
d->ANA_L_H=128;d->ANA_L_V=(f>=60&&f<165)?0:128;d->ANA_R_H=128;d->ANA_R_V=128;
d->BTN_CROSS=(f>=100&&f<110)||(f>=200&&f<203);d->BTN_L2=(f>=20&&f<60)?255:0;
d->BTN_START=(f>=170&&f<173);
d->BTN_DOWN=(f>=180&&f<182)||(f>=190&&f<192)||(f>=215&&f<217)||(f>=225&&f<227)||(f>=235&&f<237);
return 0;}
static int sysUtilRegisterCallback(int s,sysutil_cb_t cb,void*u){(void)s;(void)u;g_cb=cb;return 0;}
static int sysUtilCheckCallback(void){if(g_frame>=262&&g_cb)g_cb(SYSUTIL_EXIT_GAME,0,NULL);return 0;}
