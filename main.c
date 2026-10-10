/*
 * XBOX MESSAGE SCREENSAVER
 * Original Xbox: nxdk + SDL2 + lwIP
 * TCP/4242, newline-delimited messages; each gets random color, heading and spin.
 * ASCII-based 5x7 bitmap glyphs drawn with per-message 2D rotation.
 */
#include <SDL.h>
#include <hal/video.h>
#include <hal/debug.h>
#include <nxdk/net.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define W 320
#define H 240
#define TCP_PORT 4242
#define MAX_CLIENTS 6
#define MAX_MESSAGES 14
#define MAX_CHARS 46
#define Y_TOP 49
#define Y_BOTTOM 219
#define PI 3.14159265358979323846f
#define COLOR(r,g,b) (0xff000000u | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define BG COLOR(6,11,20)
#define PALE COLOR(169,189,201)
#define LIME COLOR(144,235,177)

typedef struct {
  char str[MAX_CHARS+1];
  int used, scale;
  float x,y,dx,dy,angle,spin;
  uint32_t color;
  uint32_t number;
} Message;

typedef struct {
  int fd;
  int size;
  char text[MAX_CHARS+1];
  int clipped;
} Client;

extern struct netif *g_pnetif;
static SDL_Window *win;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t frame[W*H];
static Message msgs[MAX_MESSAGES];
static Client clients[MAX_CLIENTS];
static uint32_t rngState=0xa1c3e5f7u;
static uint32_t received=0;
static int listener=-1;
static int net_result=-1;
static char ip_str[48]="0.0.0.0";
static char mask_str[48]="0.0.0.0";
static char gateway_str[48]="0.0.0.0";
static uint32_t lastIpPoll=0;
static uint32_t connected=0;
static uint32_t lastReceive=0;

static uint32_t random32(void) {
  rngState^=rngState<<13;
  rngState^=rngState>>17;
  rngState^=rngState<<5;
  return rngState;
}
static float rf(void){return (float)(random32()&65535)/65535.0f;}
static float rangef(float a,float b){return a+(b-a)*rf();}
static float clampf(float a,float low,float high){return a<low?low:(a>high?high:a);}
static uint32_t color_random(void) {
  /* saturated HSV --> RGB; avoid very dark/grey text */
  float hue=rf()*6.0f;
  float s=rangef(.72f,.98f);
  float v=rangef(.82f,1.0f);
  int sector=(int)hue;
  float f=hue-(float)sector;
  float p=v*(1-s),q=v*(1-s*f),t=v*(1-s*(1-f));
  float r,g,b;
  switch(sector%6) {
    case 0:r=v;g=t;b=p;break;
    case 1:r=q;g=v;b=p;break;
    case 2:r=p;g=v;b=t;break;
    case 3:r=p;g=q;b=v;break;
    case 4:r=t;g=p;b=v;break;
    default:r=v;g=p;b=q;break;
  }
  return COLOR((int)(r*255),(int)(g*255),(int)(b*255));
}
static uint32_t dim(uint32_t col, float factor){
 int r=(int)(((col>>16)&255)*factor);
 int g=(int)(((col>>8)&255)*factor);
 int b=(int)((col&255)*factor);
 return COLOR(r,g,b);
}
static void px(int x,int y,uint32_t c){
 if((unsigned)x<W && (unsigned)y<H) frame[y*W+x]=c;
}
static void rect(int x,int y,int w,int h,uint32_t c) {
 if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;}
 if(x+w>W)w=W-x; if(y+h>H)h=H-y;
 if(w<=0||h<=0)return;
 for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)frame[yy*W+xx]=c;
}
/* Font encoding: each byte contains one vertical column, least-significant bit at top. */
static void glyph(char ch,unsigned char *v){
 char c=(char)toupper((unsigned char)ch);
 memset(v,0,5);
 switch(c){
 case 'A':{unsigned char q[5]={0x7e,0x11,0x11,0x11,0x7e};memcpy(v,q,5);break;}
 case 'B':{unsigned char q[5]={0x7f,0x49,0x49,0x49,0x36};memcpy(v,q,5);break;}
 case 'C':{unsigned char q[5]={0x3e,0x41,0x41,0x41,0x22};memcpy(v,q,5);break;}
 case 'D':{unsigned char q[5]={0x7f,0x41,0x41,0x22,0x1c};memcpy(v,q,5);break;}
 case 'E':{unsigned char q[5]={0x7f,0x49,0x49,0x49,0x41};memcpy(v,q,5);break;}
 case 'F':{unsigned char q[5]={0x7f,0x09,0x09,0x09,0x01};memcpy(v,q,5);break;}
 case 'G':{unsigned char q[5]={0x3e,0x41,0x49,0x49,0x7a};memcpy(v,q,5);break;}
 case 'H':{unsigned char q[5]={0x7f,0x08,0x08,0x08,0x7f};memcpy(v,q,5);break;}
 case 'I':{unsigned char q[5]={0x00,0x41,0x7f,0x41,0x00};memcpy(v,q,5);break;}
 case 'J':{unsigned char q[5]={0x20,0x40,0x41,0x3f,0x01};memcpy(v,q,5);break;}
 case 'K':{unsigned char q[5]={0x7f,0x08,0x14,0x22,0x41};memcpy(v,q,5);break;}
 case 'L':{unsigned char q[5]={0x7f,0x40,0x40,0x40,0x40};memcpy(v,q,5);break;}
 case 'M':{unsigned char q[5]={0x7f,0x02,0x0c,0x02,0x7f};memcpy(v,q,5);break;}
 case 'N':{unsigned char q[5]={0x7f,0x04,0x08,0x10,0x7f};memcpy(v,q,5);break;}
 case 'O':{unsigned char q[5]={0x3e,0x41,0x41,0x41,0x3e};memcpy(v,q,5);break;}
 case 'P':{unsigned char q[5]={0x7f,0x09,0x09,0x09,0x06};memcpy(v,q,5);break;}
 case 'Q':{unsigned char q[5]={0x3e,0x41,0x51,0x21,0x5e};memcpy(v,q,5);break;}
 case 'R':{unsigned char q[5]={0x7f,0x09,0x19,0x29,0x46};memcpy(v,q,5);break;}
 case 'S':{unsigned char q[5]={0x46,0x49,0x49,0x49,0x31};memcpy(v,q,5);break;}
 case 'T':{unsigned char q[5]={0x01,0x01,0x7f,0x01,0x01};memcpy(v,q,5);break;}
 case 'U':{unsigned char q[5]={0x3f,0x40,0x40,0x40,0x3f};memcpy(v,q,5);break;}
 case 'V':{unsigned char q[5]={0x1f,0x20,0x40,0x20,0x1f};memcpy(v,q,5);break;}
 case 'W':{unsigned char q[5]={0x7f,0x20,0x18,0x20,0x7f};memcpy(v,q,5);break;}
 case 'X':{unsigned char q[5]={0x63,0x14,0x08,0x14,0x63};memcpy(v,q,5);break;}
 case 'Y':{unsigned char q[5]={0x07,0x08,0x70,0x08,0x07};memcpy(v,q,5);break;}
 case 'Z':{unsigned char q[5]={0x61,0x51,0x49,0x45,0x43};memcpy(v,q,5);break;}
 case '0':{unsigned char q[5]={0x3e,0x51,0x49,0x45,0x3e};memcpy(v,q,5);break;}
 case '1':{unsigned char q[5]={0x00,0x42,0x7f,0x40,0x00};memcpy(v,q,5);break;}
 case '2':{unsigned char q[5]={0x42,0x61,0x51,0x49,0x46};memcpy(v,q,5);break;}
 case '3':{unsigned char q[5]={0x22,0x41,0x49,0x49,0x36};memcpy(v,q,5);break;}
 case '4':{unsigned char q[5]={0x18,0x14,0x12,0x7f,0x10};memcpy(v,q,5);break;}
 case '5':{unsigned char q[5]={0x27,0x45,0x45,0x45,0x39};memcpy(v,q,5);break;}
 case '6':{unsigned char q[5]={0x3c,0x4a,0x49,0x49,0x30};memcpy(v,q,5);break;}
 case '7':{unsigned char q[5]={0x01,0x71,0x09,0x05,0x03};memcpy(v,q,5);break;}
 case '8':{unsigned char q[5]={0x36,0x49,0x49,0x49,0x36};memcpy(v,q,5);break;}
 case '9':{unsigned char q[5]={0x06,0x49,0x49,0x29,0x1e};memcpy(v,q,5);break;}
 case ':':v[2]=0x36;break;
 case '.':v[2]=0x40;break;
 case '!':v[2]=0x5f;break;
 case '?':v[0]=0x02;v[1]=0x01;v[2]=0x51;v[3]=0x09;v[4]=0x06;break;
 case '-':for(int k=0;k<5;k++)v[k]=0x08;break;
 case '_':for(int k=0;k<5;k++)v[k]=0x40;break;
 case '/':v[0]=0x60;v[1]=0x18;v[2]=0x06;v[3]=0x01;break;
 case '+':v[1]=0x08;v[2]=0x3e;v[3]=0x08;break;
 case '=':for(int k=0;k<5;k++)v[k]=0x14;break;
 case '@':v[0]=0x3e;v[1]=0x41;v[2]=0x5d;v[3]=0x55;v[4]=0x1e;break;
 case '#':v[0]=0x14;v[1]=0x7f;v[2]=0x14;v[3]=0x7f;v[4]=0x14;break;
 case '*':v[0]=0x14;v[1]=0x08;v[2]=0x3e;v[3]=0x08;v[4]=0x14;break;
 case ',':v[1]=0x40;v[2]=0x20;break;
 case '\'':v[2]=0x03;break;
 case '"':v[1]=0x03;v[3]=0x03;break;
 case '(':v[1]=0x1c;v[2]=0x22;v[3]=0x41;break;
 case ')':v[1]=0x41;v[2]=0x22;v[3]=0x1c;break;
 case '&':v[0]=0x36;v[1]=0x49;v[2]=0x55;v[3]=0x22;v[4]=0x50;break;
 case '>':v[0]=0x41;v[1]=0x22;v[2]=0x14;v[3]=0x08;break;
 case '<':v[0]=0x08;v[1]=0x14;v[2]=0x22;v[3]=0x41;break;
 case '[':v[1]=0x7f;v[2]=0x41;break;
 case ']':v[2]=0x41;v[3]=0x7f;break;
 case '%':v[0]=0x63;v[1]=0x13;v[2]=0x08;v[3]=0x64;v[4]=0x63;break;
 case '$':v[0]=0x24;v[1]=0x2a;v[2]=0x7f;v[3]=0x2a;v[4]=0x12;break;
 case ' ':break;
 default:v[0]=0x02;v[1]=0x01;v[2]=0x51;v[3]=0x09;v[4]=0x06;break;
 }
}
static void smallText(int x,int y,const char *txt,uint32_t col,int scale){
 for(int c=0;txt[c] && x<W; c++){
   unsigned char bits[5]; glyph(txt[c],bits);
   for(int xx=0;xx<5;xx++)for(int yy=0;yy<7;yy++)if(bits[xx]&(1u<<yy))
     rect(x+xx*scale,y+yy*scale,scale,scale,col);
   x+=6*scale;
 }
}
static void animatedText(const Message *m,float sx,float sy,uint32_t col){
 int len=(int)strlen(m->str),scale=m->scale;
 float w=len*6.0f*scale,h=7.0f*scale;
 float sn=sinf(m->angle),cs=cosf(m->angle);
 for(int i=0;i<len;i++){
   unsigned char b[5];glyph(m->str[i],b);
   for(int xx=0;xx<5;xx++)for(int yy=0;yy<7;yy++)if(b[xx]&(1u<<yy)) {
     for(int dy=0;dy<scale;dy++)for(int dx=0;dx<scale;dx++){
       float ox=i*6.0f*scale+xx*scale+dx-w*.5f;
       float oy=yy*scale+dy-h*.5f;
       int tx=(int)(sx+ox*cs-oy*sn);
       int ty=(int)(sy+ox*sn+oy*cs);
       px(tx,ty,col);
     }
   }
 }
}
static void spriteBounds(Message *m,float *rx,float *ry){
 float halfW=(float)strlen(m->str)*3.0f*m->scale+2.0f;
 float halfH=4.0f*m->scale+2.0f;
 float c=fabsf(cosf(m->angle)),s=fabsf(sinf(m->angle));
 *rx=halfW*c+halfH*s;
 *ry=halfW*s+halfH*c;
}
static void addMessage(const char *text){
 if(!text||!*text)return;
 int slot=-1;
 for(int i=0;i<MAX_MESSAGES;i++)if(!msgs[i].used){slot=i;break;}
 if(slot<0){
   uint32_t oldest=0xffffffffu;
   for(int i=0;i<MAX_MESSAGES;i++)if(msgs[i].number<oldest){oldest=msgs[i].number;slot=i;}
 }
 Message *m=&msgs[slot];
 memset(m,0,sizeof(*m));
 strncpy(m->str,text,MAX_CHARS);m->str[MAX_CHARS]=0;
 int len=(int)strlen(m->str);
 m->scale=len<=13 && (random32()&1u)?3:(len<=23?2:1);
 m->angle=rangef(-.36f,.36f);
 m->spin=rangef(-.24f,.24f);
 if(fabsf(m->spin)<.055f)m->spin=(m->spin<0?-.10f:.10f);
 float heading=rangef(0.22f,6.05f),speed=rangef(28.f,68.f);
 m->dx=cosf(heading)*speed;
 m->dy=sinf(heading)*speed;
 if(fabsf(m->dx)<12)m->dx=m->dx<0?-20:20;
 if(fabsf(m->dy)<12)m->dy=m->dy<0?-18:18;
 m->color=color_random();
 m->number=++received;
 float rx,ry;spriteBounds(m,&rx,&ry);
 if(rx>W/2-8){m->scale=1;spriteBounds(m,&rx,&ry);}
 if(ry>(Y_BOTTOM-Y_TOP)*.5f-3){
   m->angle=0;
   spriteBounds(m,&rx,&ry);
 }
 m->x=rangef(rx+4,W-rx-4);
 m->y=rangef(Y_TOP+ry+2,Y_BOTTOM-ry-2);
 m->used=1;
 lastReceive=SDL_GetTicks();
 debugPrint("[tcp] #%u \"%s\"\n",(unsigned)m->number,m->str);
}
static void updateMessages(float dt){
 for(int i=0;i<MAX_MESSAGES;i++){
   Message *m=&msgs[i];if(!m->used)continue;
   m->angle+=m->spin*dt;
   if(m->angle>PI)m->angle-=2*PI;
   if(m->angle<-PI)m->angle+=2*PI;
   float rx,ry;spriteBounds(m,&rx,&ry);
   /* Cap angle if current glyphs would no longer fit in the viewport. */
   if(ry>(Y_BOTTOM-Y_TOP)*.5f-3){
     m->spin=-m->spin;
     m->angle*=.92f;
     spriteBounds(m,&rx,&ry);
   }
   m->x+=m->dx*dt;m->y+=m->dy*dt;
   if(m->x<rx+3){m->x=rx+3;m->dx=fabsf(m->dx);}
   if(m->x>W-rx-3){m->x=W-rx-3;m->dx=-fabsf(m->dx);}
   if(m->y<Y_TOP+ry+2){m->y=Y_TOP+ry+2;m->dy=fabsf(m->dy);}
   if(m->y>Y_BOTTOM-ry-2){m->y=Y_BOTTOM-ry-2;m->dy=-fabsf(m->dy);}
 }
}
static void drawBackground(uint32_t now){
 uint32_t mix=(now/75u);
 for(int y=0;y<H;y++){
   int r=6+y/27,g=10+y/19,b=19+y/12;
   for(int x=0;x<W;x++)frame[y*W+x]=COLOR(r,g,b);
 }
 /* subtle grid and VHS-style phosphor markings */
 for(int x=0;x<W;x+=20)for(int y=44;y<225;y+=20)px(x,y,COLOR(19,29,38));
 for(int i=0;i<60;i++){
   uint32_t x=(i*137u+31u)%W,y=(i*71u+57u)%(Y_BOTTOM-Y_TOP)+Y_TOP;
   uint32_t glow=(mix+i*9u)%13u;
   if(glow<3)px((int)x,(int)y,COLOR(45,79,88));
 }
 rect(0,0,W,42,COLOR(9,17,28));
 rect(0,42,W,1,COLOR(40,112,114));
 rect(0,227,W,13,COLOR(9,17,28));
 rect(0,226,W,1,COLOR(40,112,114));
}
static void drawHUD(uint32_t now){
 char buf[100];
 smallText(9,5,"XBOX / TCP SCREENSAVER",LIME,1);
 smallText(9,16,"IP:",PALE,1);
 smallText(31,16,ip_str,COLOR(244,215,144),1);
 snprintf(buf,sizeof(buf),"PORT:%d",TCP_PORT);
 smallText(215,16,buf,COLOR(244,215,144),1);
 snprintf(buf,sizeof(buf),"MSG:%lu CLIENTS:%lu",(unsigned long)received,(unsigned long)connected);
 smallText(9,29,buf,PALE,1);
 if(listener<0)smallText(185,29,"TCP ERROR",COLOR(255,112,126),1);
 else if(strcmp(ip_str,"0.0.0.0")==0)smallText(178,29,"WAITING DHCP",COLOR(255,190,93),1);
 else smallText(229,29,"ONLINE",LIME,1);
 if(received==0){
   smallText(63,107,"AWAITING MESSAGES",COLOR(108,144,150),1);
   smallText(80,119,"TCP PORT 4242",COLOR(73,111,119),1);
 }
 smallText(9,231,"ONE LINE = ONE FLOATING MESSAGE",COLOR(92,122,129),1);
 if(lastReceive && now-lastReceive<950){
   smallText(264,231,"RX!",LIME,1);
 }
}
static void render(uint32_t now){
 drawBackground(now);
 for(int i=0;i<MAX_MESSAGES;i++)if(msgs[i].used){
   Message *m=&msgs[i];
   animatedText(m,m->x+2,m->y+3,COLOR(4,6,13));
   animatedText(m,m->x,m->y,dim(m->color,.34f));
   animatedText(m,m->x-1,m->y-1,m->color);
 }
 drawHUD(now);
 SDL_UpdateTexture(texture,NULL,frame,W*(int)sizeof(frame[0]));
 SDL_RenderClear(renderer);
 SDL_RenderCopy(renderer,texture,NULL,NULL);
 SDL_RenderPresent(renderer);
}
static void getIP(uint32_t now){
 if(now-lastIpPoll<800 && lastIpPoll!=0)return;
 lastIpPoll=now;
 if(!g_pnetif)return;
 const char *value=ip4addr_ntoa(netif_ip4_addr(g_pnetif));
 if(value){strncpy(ip_str,value,sizeof(ip_str)-1);ip_str[sizeof(ip_str)-1]=0;}
 value=ip4addr_ntoa(netif_ip4_netmask(g_pnetif));
 if(value){strncpy(mask_str,value,sizeof(mask_str)-1);mask_str[sizeof(mask_str)-1]=0;}
 value=ip4addr_ntoa(netif_ip4_gw(g_pnetif));
 if(value){strncpy(gateway_str,value,sizeof(gateway_str)-1);gateway_str[sizeof(gateway_str)-1]=0;}
}
static void closeClient(int index){
 if(clients[index].fd<0)return;
 lwip_close(clients[index].fd);
 clients[index].fd=-1;
 clients[index].size=0;clients[index].clipped=0;
 if(connected)connected--;
}
static void acceptChar(Client *c,unsigned char ch){
 if(ch=='\n'){
   c->text[c->size]=0;
   if(c->size) addMessage(c->text);
   c->size=0;c->clipped=0;
 }else if(ch=='\r'){
   /* Ignore CR; supports CRLF or LF */
 }else if(!c->clipped){
   if(c->size<MAX_CHARS){
     if(ch>=32 && ch<=126)c->text[c->size++]=(char)ch;
     else if(ch=='\t')c->text[c->size++]=' ';
     else if(ch>=128)c->text[c->size++]='?'; /* ASCII terminal font */
   } else c->clipped=1;
 }
}
static int setupServer(void){
 struct sockaddr_in addr;
 int yes=1;
 listener=lwip_socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
 if(listener<0){debugPrint("socket() failed\n");return -1;}
 lwip_setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
 memset(&addr,0,sizeof(addr));
 addr.sin_family=AF_INET;
 addr.sin_port=htons(TCP_PORT);
 addr.sin_addr.s_addr=htonl(INADDR_ANY);
 if(lwip_bind(listener,(struct sockaddr *)&addr,sizeof(addr))<0){
   debugPrint("bind(%d) failed\n",TCP_PORT);lwip_close(listener);listener=-1;return -1;
 }
 if(lwip_listen(listener,MAX_CLIENTS)<0){
   debugPrint("listen() failed\n");lwip_close(listener);listener=-1;return -1;
 }
 debugPrint("Listening on TCP %d\n",TCP_PORT);
 return 0;
}
static void serviceNetwork(void){
 if(listener<0)return;
 fd_set readSet;
 FD_ZERO(&readSet);
 FD_SET(listener,&readSet);
 int highest=listener;
 for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd>=0){
   FD_SET(clients[i].fd,&readSet);
   if(clients[i].fd>highest)highest=clients[i].fd;
 }
 struct timeval tv;tv.tv_sec=0;tv.tv_usec=0;
 int ready=lwip_select(highest+1,&readSet,NULL,NULL,&tv);
 if(ready<=0)return;
 if(FD_ISSET(listener,&readSet)){
   struct sockaddr_in remote;
   socklen_t size=sizeof(remote);
   int fd=lwip_accept(listener,(struct sockaddr *)&remote,&size);
   if(fd>=0){
     int slot=-1;
     for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd<0){slot=i;break;}
     if(slot<0)lwip_close(fd);
     else{
       clients[slot].fd=fd;
       clients[slot].size=0;clients[slot].clipped=0;
       connected++;
       debugPrint("Client connected; total %u\n",(unsigned)connected);
     }
   }
 }
 for(int i=0;i<MAX_CLIENTS;i++){
   int fd=clients[i].fd;
   if(fd>=0 && FD_ISSET(fd,&readSet)){
     char buf[256];
     int n=lwip_recv(fd,buf,sizeof(buf),0);
     if(n<=0){
       if(clients[i].size>0){
         clients[i].text[clients[i].size]=0;
         addMessage(clients[i].text);
       }
       closeClient(i);
     }else{
       for(int j=0;j<n;j++)acceptChar(&clients[i],(unsigned char)buf[j]);
     }
   }
 }
}
int main(int argc,char **argv){
 (void)argc;(void)argv;
 for(int i=0;i<MAX_CLIENTS;i++)clients[i].fd=-1;
 XVideoSetMode(640,480,32,REFRESH_DEFAULT);
 debugPrint("XBOX TCP SCREENSAVER - boot\n");
 net_result=nxNetInit(NULL);
 if(net_result<0)debugPrint("ERROR: nxNetInit returned %d\n",net_result);
 else{
   debugPrint("Network initialized, waiting for DHCP\n");
   setupServer();
 }
 if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK)!=0){
   debugPrint("SDL_Init: %s\n",SDL_GetError());return 1;
 }
 win=SDL_CreateWindow("XBOX TCP SCREENSAVER",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,640,480,SDL_WINDOW_SHOWN);
 if(!win){debugPrint("SDL_CreateWindow: %s\n",SDL_GetError());return 1;}
 renderer=SDL_CreateRenderer(win,-1,0);
 if(!renderer){debugPrint("SDL_CreateRenderer: %s\n",SDL_GetError());return 1;}
 texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,W,H);
 if(!texture){debugPrint("SDL_CreateTexture: %s\n",SDL_GetError());return 1;}
 uint32_t prev=SDL_GetTicks();
 getIP(prev);
 int running=1;
 while(running){
   uint32_t now=SDL_GetTicks();
   float dt=clampf((float)(now-prev)*.001f,0.f,.07f);prev=now;
   SDL_Event event;
   while(SDL_PollEvent(&event)){
     if(event.type==SDL_QUIT)running=0;
     if(event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE)running=0;
   }
   getIP(now);
   serviceNetwork();
   updateMessages(dt);
   render(now);
   SDL_Delay(12);
 }
 for(int i=0;i<MAX_CLIENTS;i++)closeClient(i);
 if(listener>=0)lwip_close(listener);
 SDL_DestroyTexture(texture);
 SDL_DestroyRenderer(renderer);
 SDL_DestroyWindow(win);
 SDL_Quit();
 if(net_result>=0)nxNetShutdown();
 return 0;
}
