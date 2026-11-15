"""Replay the real ui.c draw calls with the bundled CFNT's glyph metrics/atlas.
This checks native 400x240/320x240 layout, not hardware GPU or LCD behavior.
Run inside the build container: python3 3ds/app/preview_ui.py OUTPUT_DIRECTORY
"""
from pathlib import Path
import json,struct,subprocess,sys,zlib
root=Path(__file__).resolve().parent
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
work=Path('/tmp/native-ui-preview');work.mkdir(exist_ok=True)
font=(root/'assets/ui-font.bcfnt').read_bytes()
u16=lambda at:struct.unpack_from('<H',font,at)[0]
u32=lambda at:struct.unpack_from('<I',font,at)[0]
tglp,cwdh,cmap=struct.unpack_from('<III',font,36)
cw,ch,baseline=font[tglp:tglp+3];sheet_size=u32(tglp+4)
sheet_count,fmt,rows,lines,sw,sh=struct.unpack_from('<6H',font,tglp+8)
atlas=u32(tglp+20);assert fmt==11 # GPU_A4, native Morton tiles
mapping={}
while cmap:
    first,last,kind=struct.unpack_from('<3H',font,cmap)
    if kind==0:mapping.update({cp:u16(cmap+12)+cp-first for cp in range(first,last+1)})
    elif kind==1:mapping.update({cp:u16(cmap+12+(cp-first)*2) for cp in range(first,last+1) if u16(cmap+12+(cp-first)*2)!=0xFFFF})
    elif kind==2:
        for i in range(u16(cmap+12)):
            cp,index=struct.unpack_from('<HH',font,cmap+14+i*4);mapping[cp]=index
    else:raise AssertionError(kind)
    cmap=u32(cmap+8)
widths={}
while cwdh:
    start,end=struct.unpack_from('<HH',font,cwdh)
    for index in range(start,end+1):widths[index]=struct.unpack_from('<bBB',font,cwdh+8+(index-start)*3)
    cwdh=u32(cwdh+4)
alternate=u16(30)
metrics='static const unsigned codes[]={'+','.join(map(str,mapping))+'};\n'
metrics+='static const unsigned glyphs[]={'+','.join(str(mapping[cp]) for cp in mapping)+'};\n'
metrics+='static const float advances[]={'+','.join(str(widths[i][2]*30/ch)+'f' for i in range(max(widths)+1))+'};\n'
(work/'metrics.h').write_text(metrics)
(work/'font_data.h').write_text('static const unsigned char native_font[]={0};\n')
(work/'citro2d.h').write_text(r'''
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "metrics.h"
typedef uint32_t u32;
typedef struct {unsigned alterCharIndex;} FINF_s;
typedef void *C2D_Font;
typedef void *C2D_TextBuf;
typedef struct {char value[512];} C2D_Text;
typedef struct {unsigned id;} C3D_RenderTarget;
static C3D_RenderTarget targets[2]={{0},{1}};
static unsigned current_screen;
#define C3D_DEFAULT_CMDBUF_SIZE 0
#define C2D_DEFAULT_MAX_OBJECTS 0
#define C3D_FRAME_SYNCDRAW 0
#define GFX_TOP 0
#define GFX_BOTTOM 1
#define GFX_LEFT 0
#define GPU_NEAREST 0
#define C2D_WithColor 0
static uint64_t osGetTime(void){return 10000;}
static unsigned codepoint(const unsigned char **p) {
 unsigned c=*(*p)++;if(c<128)return c;
 unsigned n=(c&0xE0)==0xC0?1:(c&0xF0)==0xE0?2:3;c&=(1u<<(6-n))-1;
 while(n-- && (**p&0xC0)==0x80)c=c*64+(*(*p)++&63);
 return c;
}
static unsigned C2D_FontGlyphIndexFromCodePoint(C2D_Font f,unsigned cp) {
 (void)f;for(unsigned i=0;i<sizeof(codes)/sizeof(*codes);i++)if(codes[i]==cp)return glyphs[i];return 31;
}
static FINF_s *C2D_FontGetInfo(C2D_Font f){(void)f;static FINF_s info={31};return &info;}
static void quoted(const char *s){putchar('"');for(;*s;s++){if(*s=='"'||*s=='\\')putchar('\\');if((unsigned char)*s<32)printf("\\u%04x",(unsigned char)*s);else putchar(*s);}putchar('"');}
static bool C3D_Init(int x){(void)x;return true;}
static bool C2D_Init(int x){(void)x;return true;}
static void C3D_Fini(void){}
static void C2D_Fini(void){}
static void C2D_Prepare(void){}
static C2D_Font C2D_FontLoadFromMem(const void *p,size_t n){(void)p;(void)n;return (void*)1;}
static void C2D_FontFree(C2D_Font f){(void)f;}
static void C2D_FontSetFilter(C2D_Font f,int a,int b){(void)f;(void)a;(void)b;}
static C3D_RenderTarget *C2D_CreateScreenTarget(unsigned screen,unsigned eye){(void)eye;return &targets[screen];}
static C2D_TextBuf C2D_TextBufNew(unsigned n){(void)n;return (void*)1;}
static void C2D_TextBufClear(C2D_TextBuf b){(void)b;}
static void C2D_TextBufDelete(C2D_TextBuf b){(void)b;}
static void C2D_TextFontParse(C2D_Text *t,C2D_Font f,C2D_TextBuf b,const char *s){(void)f;(void)b;snprintf(t->value,sizeof(t->value),"%s",s);}
static void C2D_TextOptimize(C2D_Text *t){(void)t;}
static void C2D_TextGetDimensions(const C2D_Text *t,float sx,float sy,float *w,float *h){float n=0;for(const unsigned char *p=(const unsigned char*)t->value;*p;)n+=advances[C2D_FontGlyphIndexFromCodePoint(NULL,codepoint(&p))];if(w)*w=n*sx;if(h)*h=30*sy;}
static u32 C2D_Color32(unsigned r,unsigned g,unsigned b,unsigned a){return r|(g<<8)|(b<<16)|(a<<24);}
static void C3D_FrameBegin(int x){(void)x;}
static void C3D_FrameEnd(int x){(void)x;}
static void C2D_SceneBegin(C3D_RenderTarget *t){current_screen=t->id;}
static void C2D_TargetClear(C3D_RenderTarget *t,u32 color){printf("{\"op\":\"clear\",\"screen\":%u,\"color\":%u}\n",t->id,color);}
static void C2D_DrawRectSolid(float x,float y,float z,float w,float h,u32 color){(void)z;printf("{\"op\":\"rect\",\"screen\":%u,\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,\"color\":%u}\n",current_screen,x,y,w,h,color);}
static void C2D_DrawCircleSolid(float x,float y,float z,float r,u32 color){(void)z;printf("{\"op\":\"circle\",\"screen\":%u,\"x\":%g,\"y\":%g,\"r\":%g,\"color\":%u}\n",current_screen,x,y,r,color);}
static void C2D_DrawText(const C2D_Text *t,int flags,float x,float y,float z,float sx,float sy,u32 color){(void)flags;(void)z;float w;C2D_TextGetDimensions(t,sx,sy,&w,NULL);printf("{\"op\":\"text\",\"screen\":%u,\"x\":%g,\"y\":%g,\"scale\":%g,\"w\":%g,\"color\":%u,\"text\":",current_screen,x,y,sx,w,color);quoted(t->value);puts("}");}
''')
driver=r'''
#include "ui.h"
#include <stdio.h>
#include <string.h>
int main(void){
 if(!native_ui_init())return 1;
 for(unsigned lang=0;lang<3;lang++)for(unsigned page=0;page<12;page++){
 NativeUI ui={0};ui.language=lang;ui.show_timings=page==6;ui.cfg.jpeg_quality=50;ui.cfg.target_fps=30;ui.cfg.audio_kbps=64;
 strcpy(ui.cfg.tv_ip,"192.168.1.241");strcpy(ui.cfg.app_id,"01B7A380");ui.cfg.layout_mode=3;
 strcpy(ui.status.message,"Streaming game video directly to TV");ui.status.state=NATIVE_STREAMING;
 ui.status.fps=14;ui.status.capture_ms=2;ui.status.encode_ms=19;ui.status.send_ms=14;ui.status.audio_state=NATIVE_AUDIO_UNSUPPORTED;
 ui.tab=page<3?2:page<6?1:0;ui.selected_setting=page<3?page*4:0;ui.selected_tv=page>=3 && page<6?(page-3)*2:0;
 ui.tv_count=5;for(unsigned i=0;i<5;i++){snprintf(ui.tvs[i].name,64,"Living Room TV %u",i+1);snprintf(ui.tvs[i].ip,16,"192.168.1.%u",41+i);}
 strcpy(ui.selected_name,"Selected TV");
 if(page==7){ui.tab=0;ui.status.state=NATIVE_IDLE;ui.status.audio_state=NATIVE_AUDIO_OFF;strcpy(ui.status.message,"Streaming stopped");strcpy(ui.notice,"Settings saved");}
 if(page==8){ui.tab=1;ui.tv_count=0;}
 if(page==10){ui.tab=1;ui.tv_count=2;strcpy(ui.tvs[0].name,"Cast\xF0\x9F\x99\x82TV");strcpy(ui.tvs[1].name,"AB\xE9\x9B");}
 if(page==11){ui.tab=1;ui.tv_count=1;strcpy(ui.tvs[0].name,"\xE9\x9B\xBB\x9B");}
 if(page==9){ui.tab=2;ui.selected_setting=12;ui.cpu_core=2;ui.capture_source=1;ui.status.worker_core=2;}
 printf("{\"op\":\"case\",\"name\":\"%s-%u\"}\n",lang==2?"ja":lang?"zh":"en",page);native_ui_draw(&ui);
 }native_ui_exit();return 0;}
'''
(work/'driver.c').write_text(driver)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-I'+str(work),'-I'+str(root),'-I'+str(root.parent/'shared'),str(root/'ui.c'),str(root/'i18n.c'),str(work/'driver.c'),'-o',str(work/'preview')],check=True)
calls=[json.loads(line) for line in subprocess.check_output([str(work/'preview')],text=True).splitlines()]
cases={};current=None
for call in calls:
    if call['op']=='case':current=call['name'];cases[current]=[]
    else:cases[current].append(call)
def png(path,width,height,pixels):
    def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data)&0xFFFFFFFF)
    raw=b''.join(b'\0'+pixels[y*width*4:(y+1)*width*4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
def rgba(color):return tuple((color>>(8*i))&255 for i in range(4))
def morton(x,y):return sum(((x>>b)&1)<<(2*b) | ((y>>b)&1)<<(2*b+1) for b in range(3))
def alpha(index,x,y):
    sheet=index//(rows*lines);index%=rows*lines
    tx=(index%rows)*(cw+1)+1+x;ty=(index//rows)*(ch+1)+1+y
    pos=((ty//8)*(sw//8)+(tx//8))*64+morton(tx%8,ty%8)
    return ((font[atlas+sheet*sheet_size+pos//2]>>((pos%2)*4))&15)*17
for name,commands in cases.items():
    screens=[bytearray(400*240*4),bytearray(320*240*4)]
    def pixel(screen,x,y,color,opacity=255):
        width=400 if screen==0 else 320
        if not 0<=x<width or not 0<=y<240:return
        at=(y*width+x)*4;buf=screens[screen]
        for c in range(3):buf[at+c]=(buf[at+c]*(255-opacity)+color[c]*opacity+127)//255
        buf[at+3]=255
    for call in commands:
        op=call['op'];screen=call['screen'];width=400 if screen==0 else 320;color=rgba(call['color'])
        if op=='clear':screens[screen][:]=bytes(color)*(width*240)
        elif op in ('rect','circle'):
            if op=='rect':x0,y0,x1,y1=call['x'],call['y'],call['x']+call['w'],call['y']+call['h']
            else:x0,y0,x1,y1=call['x']-call['r'],call['y']-call['r'],call['x']+call['r'],call['y']+call['r']
            for y in range(int(y0),int(y1)+1):
                for x in range(int(x0),int(x1)+1):
                    if op=='rect' or (x-call['x'])**2+(y-call['y'])**2<=call['r']**2:pixel(screen,x,y,color)
        elif op=='text':
            assert call['scale']>=.66,(name,call)
            assert call['x']>=0 and call['x']+call['w']<=width+.1 and call['y']+30*call['scale']<=240,(name,call)
            if name.endswith(('-0','-1','-2','-9')) and screen==1:assert '...' not in call['text'],(name,call)
            x=call['x'];factor=30/ch*call['scale']
            for char in call['text']:
                index=mapping.get(ord(char),alternate);left,gw,advance=widths[index]
                for dy in range(round(ch*factor)):
                    for dx in range(round(gw*factor)):
                        a=alpha(index,min(gw-1,int(dx/factor)),min(ch-1,int(dy/factor)))
                        if a:pixel(screen,round(x+left*factor)+dx,round(call['y'])+dy,color,a)
                x+=advance*factor
    combined=bytearray(bytes((12,19,32,255))*(400*480))
    combined[:400*240*4]=screens[0]
    for y in range(240):combined[((y+240)*400+40)*4:((y+240)*400+360)*4]=screens[1][y*320*4:(y+1)*320*4]
    png(out/(name+'.png'),400,480,combined)
names=[c['text'] for k in ('en-10','en-11') for c in cases[k] if c['op']=='text']
assert 'Cast?TV' in names and 'AB?' in names and '\u96fb?' in names,names
assert not any('\U0001F642' in t for t in names)
(out/'draw-calls.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2))
print('PASS: 36 layouts (3 languages x 12 pages) rendered from real ui.c; native screen bounds, >=0.66 scale, complete settings labels/values, unknown or broken characters shown as ?')
