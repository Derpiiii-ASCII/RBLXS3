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
#define GCM_FLIP_HSYNC 2
#define VIDEO_RESOLUTION_720 4
#define VIDEO_RESOLUTION_1080 5
#define VIDEO_ASPECT_AUTO 0
typedef struct { u32 status[7]; } padInfo;
typedef struct { u16 ANA_L_H, ANA_L_V, ANA_R_H, ANA_R_V; u8 BTN_CROSS; u16 BTN_L2; u8 BTN_CIRCLE, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_L1, BTN_R1; } padData;

typedef struct { int a, b; char k; } StubEv;
static StubEv stub_evs[256]; static int stub_nev=-1, stub_exit=300; static int stub_dump[64], stub_ndump=0; static void stub_parse(void);
static double g_pend; static gcmContextData g_ctx; static u32 *g_fb[2]; static u32 g_w=1280,g_h=720,g_pitch; static int g_frame=0; static sysutil_cb_t g_cb;
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
static void gcmSetFlip(gcmContextData*c,u8 id){(void)c;g_frame++;g_pend+=800.0/256.0;char n[64];if(stub_nev<0)stub_parse();int dd=0;for(int q=0;q<stub_ndump;q++)if(g_frame==stub_dump[q])dd=1;if(dd){snprintf(n,64,"frame%d.ppm",g_frame);dump(id,n);}}
static void rsxFlushBuffer(gcmContextData*c){(void)c;}
static void gcmSetWaitFlip(gcmContextData*c){(void)c;}
static int ioPadInit(int n){(void)n;return 0;}
static int ioPadGetInfo(padInfo*i){memset(i,0,sizeof(*i));i->status[0]=1;return 0;}
static void stub_parse(void){ stub_nev=0; const char*e=getenv("STUB_EVENTS"); if(e){ while(*e&&stub_nev<256){ int a=0,b=-1; char k=0;
  while(*e>='0'&&*e<='9'){a=a*10+*e-'0';e++;} if(*e=='-'){e++;b=0;while(*e>='0'&&*e<='9'){b=b*10+*e-'0';e++;}} else b=a+1;
  if(*e==':')e++; k=*e?*e++:0; if(*e==',')e++; stub_evs[stub_nev].a=a;stub_evs[stub_nev].b=b;stub_evs[stub_nev].k=k;stub_nev++; } }
  e=getenv("STUB_DUMP"); if(e){ while(*e&&stub_ndump<64){ int v=0; while(*e>='0'&&*e<='9'){v=v*10+*e-'0';e++;} stub_dump[stub_ndump++]=v; if(*e==',')e++; } }
  e=getenv("STUB_EXIT"); if(e) stub_exit=atoi(e); }
static int ioPadGetData(int p,padData*d){(void)p;memset(d,0,sizeof(*d)); if(stub_nev<0) stub_parse(); int f=g_frame;
d->ANA_L_H=128;d->ANA_L_V=128;d->ANA_R_H=128;d->ANA_R_V=128;
for(int i=0;i<stub_nev;i++){ if(f>=stub_evs[i].a&&f<stub_evs[i].b){ switch(stub_evs[i].k){
 case 'U':d->BTN_UP=1;break; case 'D':d->BTN_DOWN=1;break; case 'L':d->BTN_LEFT=1;break; case 'R':d->BTN_RIGHT=1;break;
 case 'X':d->BTN_CROSS=1;break; case 'O':d->BTN_CIRCLE=1;break; case 'S':d->BTN_START=1;break;
 case 'W':d->ANA_L_V=0;d->BTN_L2=255;break; case 'w':d->ANA_L_V=0;break; default:break; } } }
return 0;}
static int sysUtilRegisterCallback(int s,sysutil_cb_t cb,void*u){(void)s;(void)u;g_cb=cb;return 0;}
static int sysUtilCheckCallback(void){if(g_frame>=stub_exit&&g_cb)g_cb(SYSUTIL_EXIT_GAME,0,NULL);return 0;}

/* ---- audio + event queue stand-ins (PC preview only) ---- */
typedef int sys_event_queue_t; typedef int sys_ipc_key_t; typedef struct { int d; } sys_event_t;
typedef struct { u64 numChannels, numBlocks, attrib; float level; } audioPortParam;
typedef struct { u32 readIndex, status; u64 channelCount, numBlocks; u32 portSize; u64 audioDataStart; } audioPortConfig;
#define AUDIO_BLOCK_SAMPLES 256
#define AUDIO_PORT_2CH 2
#define AUDIO_BLOCK_16 16
static float *g_ab; static u64 g_ablocks;
static int audioInit(void){return 0;} static int audioQuit(void){return 0;}
static int audioPortOpen(audioPortParam*p,u32*n){*n=1;g_ablocks=p->numBlocks;return 0;}
static int audioGetPortConfig(u32 n,audioPortConfig*c){(void)n;g_ab=(float*)calloc(g_ablocks*2*256,sizeof(float));c->numBlocks=g_ablocks;c->channelCount=2;c->audioDataStart=(u64)(uintptr_t)g_ab;return 0;}
static int audioCreateNotifyEventQueue(sys_event_queue_t*q,sys_ipc_key_t*k){*q=1;*k=1;return 0;}
static int audioSetNotifyEventQueue(sys_ipc_key_t k){(void)k;return 0;}
static int audioRemoveNotifyEventQueue(sys_ipc_key_t k){(void)k;return 0;}
static int audioPortStart(u32 n){(void)n;return 0;} static int audioPortStop(u32 n){(void)n;return 0;} static int audioPortClose(u32 n){(void)n;return 0;}
static int sysEventQueueDrain(sys_event_queue_t q){(void)q;g_pend=0;return 0;}
static int sysEventQueueDestroy(sys_event_queue_t q,int m){(void)q;(void)m;return 0;}
static int sysEventQueueReceive(sys_event_queue_t q,sys_event_t*e,u64 t){(void)q;(void)e;(void)t;if(g_pend>=1.0){g_pend-=1.0;return 0;}return 1;}
