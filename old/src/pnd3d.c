#if 0 //To compile, type: nmake pnd3d.c (GPU=1)

objs=pnd3d.obj drawcone.obj kplib.obj kdisasm.obj winmain.obj
libs=ddraw.lib ole32.lib dinput.lib dxguid.lib user32.lib gdi32.lib comdlg32.lib
opts=/c /J /TP /Ox /Ob2 /GFy /Gs
defs=

!if 1
opts=$(opts) /G6 /MD /QIfist  #if compiling with VC6
libs=$(libs) /opt:nowin98     #if compiling with VC6
!else
opts=$(opts) /MT              #if compiling with VC version later than 6
!endif

pnd3d.exe: $(objs) pnd3d.c; link $(objs) $(libs) /nologo
	del pnd3d.obj
pnd3d.obj: pnd3d.c sysmain.h         ; cl pnd3d.c     $(opts) $(defs) /DSTANDALONE=1
drawcone.obj: drawcone.c             ; cl drawcone.c  $(opts)
kplib.obj:    kplib.c                ; cl kplib.c     $(opts)
kdisasm.obj:  kdisasm.c              ; cl kdisasm.c   $(opts)
winmain.obj:  winmain.cpp            ; cl winmain.cpp $(opts) /DUSEKENSOUND=0 /DOFFSCREENHACK=1

!if 0
#endif

#include "sysmain.h"
#include "pnd3d.h"

	//Marching Cubes porting plan:
	//Got the big one, March 20, 2013  11:17pm
	//
	//   //mod:
	////oct_updatesurfs_recur(): optimize roct->chi generation (get rid of brute force oct_issurf() calls!)
	//
	//oct_ftob2_dorect(): optimize!
	//-------------------
	//cpu_shader_texmap_mc(): write
	//
	//   //kvo:
	//brush_loadkvo_getsurf()
	//oct_savekvo()
	//oct_loadkvo()

	//PND3D current octree format:
	// ls: sol chi|
	//+-----------+-------
	//|>0   0   0 |pure air
	//|>0   0   1 |mix of air&sol ; some surfs ; child on octv_t
	//|>0   1   0 |pure solid     ; no surfs
	//|>0   1   1 |pure solid     ; some surfs ; child on octv_t
	//+-----------+-------
	//| 0   0   0 |air voxel
	//| 0   0   1 |(N/A)
	//| 0   1   0 |interior voxel
	//| 0   1   1 |surface voxel               ; child on surf_t (col/norm/etc.)
	//+-----------+-------

#if (MARCHCUBE == 0)
int oct_usegpu    = 1; //0=CPU, 1=GPU(ARBASM/GLSL); if (0) PIXMETH MUST be:{0,1}
#else
int oct_usegpu    = 0; //0=CPU, 1=GPU(ARBASM/GLSL); if (0) PIXMETH MUST be:{0,1} //FIXFIXFIXFIX:MARCHCUBE doesn't support GPU for now
#endif
int oct_useglsl   = 1; //0=ARB ASM (usefilter 2 invalid), 1=GLSL (usefilter 3 is invalid)
int oct_usefilter = 2; //0=Nearest, 1=Bilinear, 2=MipMap, 3=MipMap w/2Bilins (for ARB ASM)

int oct_usegpu_cmdline = 0;


#if (MARCHCUBE != 0)
#define USEASM 0  //FIXFIXFIXFIX:Want USEASM 1!!!
#else
#define USEASM 1  //1  0:C (prototype)                                       I7-950(KJS):     |I7-920(HMA):
						//   1:ASM(fast/default)                             default   |untitled.vxl|default/old|
						//                                     CPU_SHAD_PM0:   !327    |   !229     |   !209?   | col_only
						//                                     CPU_SHAD_PM1:    189    |     65     |    142??  | lsid<=12
#endif
#define GPUSEBO 1 //1  0:TexSubImage2D, 1:MapBuffer                   BO=0  BO=1 | BO=0  BO=1 | BO=0  BO=1|
#define PIXMETH 1 //1  0: 32bit: psurf:32                             !476  !596 | !264  !292 | !164  !151| col_only
						//   1: 64bit: x:12, y:12, z:12, psurf:28            287   542 |  181   259 |   85    78| lsid<=12
						//   2:128bit: x:32, y:32, z:32, psurf:32            183   349 |  123   179 |   64    54|

#define DRAWOCT_CPU_FILT 0 //0:near, 1:bilin, 2:mip (NOTE:ASM supports only near right now :/)
#define DRAWSKY_CPU_FILT 0 //0:near, 1:bilin
#define DRAWPOL_CPU_FILT 0 //0:near, 1:bilin

#include <gl/gl.h>
#pragma comment(lib,"opengl32.lib")
#include <process.h>
#include <mmsystem.h>
#include <emmintrin.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#define MAXYDIM 2048
#define PI 3.14159265358979323

	//Targetted apps: VCD, SLAB6, PND3D, AOS, GAME_TD
	//
	//TODO:
	// ! drawcone(),drawsph(),drawpol() should be affected by oct_fogdist in /cpu mode
	// ! FIZEX:small objects that validly get stuck start daffy ducking (once gravity incs vel) - need verlet
	// ! FIZEX:hovering objects like to get stuck.
	// * oct_modnew(): should call recvoctfunc() multiple times for disconnected regions
	// * FIZEX:use early out ohit 1v1.
	// * optimization: oct_touch_oct(): change cr.x = 2.0 to 1.0, etc.. so MAT0ISIDENTITY can be 1
	// * support mip-map for /ils=? mode
	// * bad rounding for rasterization: think borders are visible around cubes :/
	// * GAME_TD: write animation editor
	// * support highly sheared (non-orthogonal) axes
	// * get ARB ASM running on at least 1 old GPU that doesn't already support GLSL.
	// * tiles instancing editor.
	// * marching cubes.
	// * edgeiswrap/edgeissol: support!
	// * oct_rendslice (oct_t *loct, point3d *pp, point3d *pr, point3d *pd, float mixval, int imulcol);
	// . optimization: oct_modnew(): use consecutive allocation instead of bitalloc() for newoct
	// . optimization: oct_modnew(): support simultaneous chop off original oct as mode option
	// . optimization: oct_mod(): special case for empty tree (may speed up loading)
	// . oct_sol2bit(): make it respect loct->edgeiswrap/edgeissol
	// . write oct_oct_sweep() - translational sweep collision of octree.
	// . Alt+H slow with memcpy because writing directly to GPU texture?
	// . slice editor
	// . symmetry copy
	// . double/halve dimension tool
	// . hollow fill?
	// . new PIXMETH idea: store x/y/z/chi at head of 2x2x2 surf list; ~25% more GRAM, but can do larger octree and maybe faster (incompatible w/instancing :/).
	// . oct_rendclosecubes(): can't use cornmin[]; must render all cubes within manhattan dist; new code fails for high FOV :/
	// . bstatus doesn't work on first click (likely winmain dinput acquire/gbstatus problem)
	// x research voxel tearing (STRESSGRID.KC)
	// x 2 voxels on diagonal disappear when pos exactly on integer coord (must be in *_dorect); solved with temp hack
	// x optimize: drawcone brush

	//08/21/2010: FPS comparison; tests use: vxl/untitled.vxl /1024x768, no effects (Ken's C2Q@2.66Ghz)
	//                        MT:1  MT:4
	//   voxlap6:DrawPaint     19     -  (cheat:limited scan dist)
	//   voxlap6:DrawPoint     25     -  (cheat:limited scan dist)
	//   voxlap6:Drawcast6D    37    64  (cheat:limited scan dist)
	//   voxed (voxlap5)       40     -  (cheat:limited scan dist)
	//   pnd3d                 65    96  <-bbox suspected?
	//   pnd3d                 71   107  (cheats:using rcpps & z-buffer disabled)
	//  (voxlap6:Drawcast4D)  (52) (135) (cheats:limited scan dist & 4DOF!)
	//   game (voxlap5)        60     -  (cheat:mip-mapping)
	//
	//05/29/2011: Same settings as ^ but on KJSI7:
	//                        MT:1  MT:4  MT:8
	//   voxlap6:DrawPaint     21     -     -  (cheat:limited scan dist)
	//   voxlap6:DrawPoint     32     -     -  (cheat:limited scan dist)
	//   voxlap6:Drawcast6D    47    94    94  (cheat:limited scan dist)
	//   voxed (voxlap5)       59     -     -
	//   voxed (voxlap5)       64     -     -  (cheat:limited scan dist)
	//   pnd3d                 35   100   150
	//   pnd3d                 47   130   177  (cheats:using bbox)
	//   pnd3d                 47   137   178  (cheats:using bbox, rcpps, no zbuf)
	//  (voxlap6:Drawcast4D)  (74) (233) (330) (cheats:limited scan dist & 4DOF!)
	//   game (voxlap5)       116     -     -  (cheat:mip-mapping)

	//08/18/2011: After enabling early gcbit check for ls==0:
	//                opnd3d_bak3:    pnd3d:    |09/04/2011:
	//                 MT:1  MT:8   MT:1  MT:8  |MT8,ASM,PBO,GLSL,PIXMETH=1 (texmap!):
	//   CANYON.VXL   23.63 135.7  25.08 141.9  | 222
	//   coreef.vxl   23.68 123.9  24.63 127.5  | 228
	//   lung.vxl     17.64  79.6  20.42 106.5  | 168
	//   test.vxl     20.52  65.0  24.52 105.0  | 160
	//   tomland.png  20.54 104.6  21.63 106.8  | 329
	//   untitled.vxl 28.62 159.6  28.71 160.0  | 254

	//02/10/2012: mod speed, untitled.vxl:
	//                oldmod_0: newmod_2:
	//   load time:      1325ms     931ms
	//   #getsurf:    5,512,197 5,512,197
	//   #isins_ls0: 52,828,714   982,008
	//   #isins_ls1: 20,134,389   380,296
	//   #isins_ls2:  4,824,145   106,584
	//   #isins_ls3:  1,136,307    25,712
	//   #isins_ls4:    262,982     5,808
	//   #isins_ls5:     59,472     1,368
	//   #isins_ls6:     12,033       296
	//   #isins_ls7:      2,298       512
	//   #isins_ls8:         64        64
	//   #isins_ls9:          8         8

	//03/02/2012: testing both mod & rend (big GPUSEBO=1 speedup today!)
	//CPU pure:     | GPU,GPUSEBO=0 | GPU,GPUSEBO=1
	//mod:   rend:  | mod:   rend:  | mod:   rend:
	//143ms  134fps | 140ms  146fps | 147ms  499fps
	//143ms  128fps | 548ms  142fps | 149ms  433fps
	//              |               |
	//..            | ..            | ..
	//7-14ms 106fps | 762ms  153fps | 8-13ms 546fps
	// :)     :/    |  :(     :/    |  :)     :)

	//03/14/2012: test load speed&stats    uncomp: kzip: loadms: nod.num: sur.num:
	//pnd3d ../voxlap/vxl/tomland.png      2088348          1189  2212518  6042719
	//pnd3d ../aos/vxl/tomland.vxl         2101280           250   333153   936306
	//pnd3d ../aos/vxl/grandcan.vxl        2497156           282   382338  1073619
	//pnd3d ../aos/vxl/castlesandshit.vxl  2673600           283   392134  1087500
	//pnd3d ../aos/vxl/world12.vxl         3253900           291   401378  1137110
	//pnd3d ../aos/vxl/metropolis.vxl      6532084           372   556221  1670494
	//pnd3d ../voxlap/vxl/untitled.vxl    12425384  3288588 1142  1862816  5512198
	//pnd3d ../voxlap/vxl/untitled.vxl    12425384  3288588  712   650132  1852926 (edgeissol=61)
	//pnd3d ../voxlap/vxl/untitled.kvo     5788180  2265217  501   650132  1852926 (edgeissol=61)
	//pnd3d ../cavedemo/data/seed12t1.vxl 13501288          1600  2242535  5826251
	//pnd3d ../voxlap/vxl/canyon.vxl      14321980          1642  2343131  6200467
	//pnd3d ../cavedemo/data/seed7t3.vxl  20994016          2029  2720109  6936963
	//pnd3d ../cavedemo/data/seed5t1.vxl  21624788          1938  2697394  7471136
	//pnd3d ../cavedemo/data/seed9t1.vxl  22572000          2241  2914854  7228081
	//pnd3d ../cavedemo/data/seed7t2.vxl  23240136          2285  2963912  7343796
	//pnd3d ../cavedemo/data/seed7t1.vxl  23667988          2316  2980566  7395794
	//pnd3d ../voxlap/vxl/coreef.vxl      25765080          2339  3175828  8157288
	//pnd3d ../voxlap/vxl/test.vxl        28534696          2636  3426678  8557623
	//pnd3d ../voxlap/vxl/lung.vxl        33680036          2898  3560093  9243838
	//pnd3d ../voxlap/vxl/voxelstein.vxl  56591876          3488  4498673 13687906
	//pnd3d ../voxlap/vxl/voxelstein.kvo  33458913
	//
	//rule of thumb: sur.num = nod.num*~{2.5-3}

	//03/15/2012: idea for new PIXMETH?
	//   //pix buffer:
	//psurf:29 (points to start of 2x2x2 surf list)
	//popcnt:3 (psurf offset)
	//
	//   //surf format:
	//short x, y, z; char chi, filler;
	//unsigned char b, g, r, a; signed char norm[3], tex;
	//unsigned char b, g, r, a; signed char norm[3], tex;
	//unsigned char b, g, r, a; signed char norm[3], tex;
	//..
	//
	//pros:
	//* 32bit transfer is nice
	//* supports up to 16-bit lsid
	//* generation of pix val in dorect may be negligibly faster
	//
	//cons:
	//* requires ~25% more memory on GPU/surf list texture ://
	//* surf list limited to 4GBy, but still 2x better than other pixmeth

	//--------------------------------------------------
	//   clearscreen(oct_fogcol);
	//   zbuf: set all to 0x7f7f7f7f
	//   {
	//      if (first sprite) { gcbit:set cubearea to 1 }
	//                   else { gcbit:set cubearea to 1 if zbuf>front }
	//      *_dorect: if (gcbit[?]) gcbit[?] = 0; if (z<zbuf[?]) .. )
	//   }
	//   render other stuff here
	//--------------------------------------------------

#ifdef _MSC_VER
#define LL(l) l##i64
#define PRINTF64 "I64"
#else
#define __forceinline __inline__
#define LL(l) l##ll
#define PRINTF64 "ll"
#endif

#ifndef _InterlockedExchangeAdd
#define _InterlockedExchangeAdd InterlockedExchangeAdd
#endif

#ifndef _WIN64
#define KMOD32(a) (a)
#else
#define KMOD32(a) ((a)&31)
#endif

	//KPLIB.H:
	//High-level (easy) picture loading function:
void kpzload (const char *filnam, INT_PTR *pic, int *bpl, int *xsiz, int *ysiz);
	//Low-level PNG/JPG functions:
extern int kpgetdim (const char *, int, int *, int *);
extern int kprender (const char *, int, INT_PTR, int, int, int, int, int);
	//Ken's ZIP functions:
extern int kzaddstack (const char *);
extern void kzuninit (void);
extern void kzsetfil (FILE *);
extern int kzopen (const char *);
extern void kzfindfilestart (const char *);
extern int kzfindfile (char *);
extern int kzread (void *, int);
extern int kzfilelength (void);
extern int kzseek (int, int);
extern int kztell (void);
extern int kzgetc (void);
extern int kzeof (void);
extern void kzclose (void);
extern void kzfindfilestart (const char *); //pass wildcard string
extern int kzfindfile (char *); //you alloc buf, returns 1:found,0:~found

int oct_numcpu = 0;
static int maxcpu, gdrawclose = 1;

typedef struct { INT_PTR f; int p, x, y; } tiletype;
static tiletype gdd;

static int gzbufoff, gddz;

static float ghx, ghy, ghz;
static int ighx, ighy, ighz;
static point3d girig, gidow, gifor, gipos;

float oct_fogdist = 16.0; //1e32;
int oct_fogcol = 0x605040;

#if (PIXMETH == 0)
#define PIXBUFBYPP 4
#elif (PIXMETH == 1)
#define PIXBUFBYPP 8
#elif (PIXMETH == 2)
#define PIXBUFBYPP 16
#endif

static unsigned int gpixtexid;             //glBindTexture   id's (see also oct_t.octid, oct_t.tilid)
INT_PTR gskyid = 0, gtilid = 0;            //INT_PTR to support CPU as pointer & GPU as index for drawpol()
static unsigned int gfont6x8id, gtextbufid;
static unsigned int gpixbufid;             //glBufferTexture id's (see also oct_t.gbufid)

static point3d fgxmul, fgymul, fgzmul, fgnadd, gxmul, gymul, gzmul, gnadd;
static point3d gorig;
static ipoint3d glorig;

static unsigned int *gcbit, gcbitmal = 0, gcbitmax = 0, gcbpl, gcbitpl, gcbitplo5, gcbitplo3;
static unsigned int *gtbit, gtbitmal = 0;

static char *gpixbuf = 0;
static int gpixbufmal = 0, gpixxdim = 2048, gpixydim = 2048;

static void (*shaderfunc)(int,void*) = 0;
static int gshadermode = -1; //-1=uninited, 0=drawtext6x8,etc.., 1=drawoct()
static int swapinterval = 1; //0=max, 1=60fps, 2=30fps, ..
static double gznear = 1.0/32768.0, gzfar = 256.0;

static int *zbuf = 0, zbufmal = 0, zbufoff;
static int gimixval, gimulcol; //temp for cpu shader only
static int gtiloff[16];
__declspec(align(16)) static int dqtiloff[256][4];

static int gusemix = 1;

static tiletype gxd; //intermediate data between 1st pass & shader

typedef struct { INT_PTR f; int p, x, y, ltilesid; } tiles_t;
static tiles_t tiles[16]; //mips

	//for debug only
static double testim = 0.0;

//--------------------------------------------------------------------------------------------------
extern int kdisasm (unsigned char *ibuf, char *obuf, int bits);

static EXCEPTION_RECORD gexception_record;
static CONTEXT gexception_context;

static int exception_getaddr (LPEXCEPTION_POINTERS pxi)
{
	memcpy(&gexception_record,pxi->ExceptionRecord,sizeof(EXCEPTION_RECORD));
	memcpy(&gexception_context,pxi->ContextRecord,sizeof(CONTEXT));
	return(EXCEPTION_EXECUTE_HANDLER);
}

static void showexception (void)
{
		//EXCEPTION_RECORD:
		//   DWORD ExceptionCode, ExceptionFlags;
		//   EXCEPTION_RECORD *ExceptionRecord;
		//   PVOID ExceptionAddress;
		//   DWORD NumberParameters;
		//   ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
		//EXCEPTION_CONTEXT:
		//   DWORD ContextFlags;
		//   DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;                  //CONTEXT_DEBUG_REGISTERS (not in CONTEXT_FULL)
		//   FLOATING_SAVE_AREA FloatSave;                        //CONTEXT_FLOATING_POINT
		//   DWORD SegGs, SegFs, SegEs, SegDs;                    //CONTEXT_SEGMENTS
		//   DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;                  //CONTEXT_INTEGER
		//   DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;           //CONTEXT_CONTROL
		//   BYTE ExtendedRegisters[MAXIMUM_SUPPORTED_EXTENSION]; //CONTEXT_EXTENDED_REGISTERS
	int i, j, k, i0, i1;
	unsigned char *cptr, tbuf[1024];
	static char buf[65536], *optr; //WARNING: assumes n->bufalloc has enough bytes!
	optr = buf;

	optr += sprintf(optr,"Exception %08x",gexception_record.ExceptionCode);
	if (gexception_record.ExceptionCode)
	{
		optr += sprintf(optr,"; code @ %08x tried to ",gexception_record.ExceptionAddress);
		if (gexception_record.NumberParameters >= 2)
		{
			switch(gexception_record.ExceptionInformation[0])
			{
				case 0: optr += sprintf(optr,"read"); break;
				case 1: optr += sprintf(optr,"write"); break;
				case 8: optr += sprintf(optr,"execute"); break;
				default: break;
			}
			optr += sprintf(optr," %08x",gexception_record.ExceptionInformation[1]);
		}
		else optr += sprintf(optr,"do something bad:");
	}
	optr += sprintf(optr,"\r\n\r\n");

	optr += sprintf(optr,"   EAX: %08x     ESI: %08x\r\n",gexception_context.Eax,gexception_context.Esi);
	optr += sprintf(optr,"   EBX: %08x     EDI: %08x\r\n",gexception_context.Ebx,gexception_context.Edi);
	optr += sprintf(optr,"   ECX: %08x     EBP: %08x\r\n",gexception_context.Ecx,gexception_context.Ebp);
	optr += sprintf(optr,"   EDX: %08x     ESP: %08x\r\n",gexception_context.Edx,gexception_context.Esp);
	optr += sprintf(optr,"   EIP: %08x  EFLAGS: %08x\r\n\r\n",gexception_context.Eip,gexception_context.EFlags);
	optr += sprintf(optr," FP_CW: %08x  FP_SW: %08x  FP_TW: %08x\r\n",gexception_context.FloatSave.ControlWord,gexception_context.FloatSave.StatusWord,gexception_context.FloatSave.TagWord);
	for(i=0;i<8;i++)
	{
		optr += sprintf(optr,"  FP_%d: ",i);
		for(j=0;j<10;j++) optr += sprintf(optr,"%02x ",gexception_context.FloatSave.RegisterArea[i*10+j]);
		optr += sprintf(optr,"\r\n");
	}
	optr += sprintf(optr,"\r\n");
	for(i=0;i<8;i++)
	{
		optr += sprintf(optr,"  XMM%d: ",i);
		for(j=0;j<16;j++)
		{
			optr += sprintf(optr,"%02x ",gexception_context.ExtendedRegisters[i*16+j+160]);
			if ((j&3) == 3) optr += sprintf(optr," ");
		}
		optr += sprintf(optr,"\r\n");
	}
	optr += sprintf(optr,"\r\n");
	for(i=0;i<8;i++)
	{
		optr += sprintf(optr,"  XMM%d: ",i);
		for(j=0;j<16;j+=4) optr += sprintf(optr,"%12g ",*(float *)(((int)gexception_context.ExtendedRegisters) + i*16 + j + 160));
		optr += sprintf(optr,"\r\n");
	}
	optr += sprintf(optr,"\r\n");
	for(i=0;i<8;i++)
	{
		optr += sprintf(optr,"  XMM%d: ",i);
		for(j=0;j<16;j+=8) optr += sprintf(optr,"%25g ",*(double *)(((int)gexception_context.ExtendedRegisters) + i*16 + j + 160));
		optr += sprintf(optr,"\r\n");
	}

#if 1
	#define DISASMRANGE 64
	__try { i = 0; memcpy(tbuf,(void *)(((int)gexception_context.Eip)-DISASMRANGE*2),DISASMRANGE*3+16); }
	__except(EXCEPTION_EXECUTE_HANDLER) { i = -1; }
	if (!i)
	{
		optr += sprintf(optr,"...\r\n");
		for(i=0;i<DISASMRANGE*3;i+=j)
		{
			char tbuf2[64];
			j = kdisasm((unsigned char *)&tbuf[i],tbuf2,32);
			if (i < DISASMRANGE) continue;

			if (i == DISASMRANGE*2) { i0 = optr-buf; optr += sprintf(optr,"\r\n"); }
			optr += sprintf(optr,"%x:",((int)gexception_context.Eip)-DISASMRANGE*2+i);
			for(k=0;k<j;k++) optr += sprintf(optr,"%02x ",tbuf[i+k]);
			if (j < 9) optr += sprintf(optr,"%*c",27-j*3,32);
			optr += sprintf(optr,"%s\r\n",tbuf2);
			if (i == DISASMRANGE*2) { i1 = optr-buf; optr += sprintf(optr,"\r\n"); }
		}
		optr += sprintf(optr,"...\r\n");
	}
#endif
	HWND hwnd;
	HFONT hfont;
	MSG msg;
	hwnd = CreateWindowEx(WS_EX_CLIENTEDGE,"edit","Exception! (ESC:quit)",WS_VISIBLE|WS_CAPTION|WS_POPUP|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,50,50,640,1024,ghwnd,0,ghinst,0);
	hfont = CreateFont(-10,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"Courier");
	SendMessage(hwnd,WM_SETFONT,(WPARAM)hfont,0);
	SendMessage(hwnd,EM_SETSEL,0,-1);
	SendMessage(hwnd,EM_REPLACESEL,0,(LPARAM)buf);
	i = SendMessage(hwnd,EM_LINEINDEX,SendMessage(hwnd,EM_LINEFROMCHAR,i0,0)-32,0);
	SendMessage(hwnd,EM_SETSEL,i,i);
	SendMessage(hwnd,EM_SCROLLCARET,0,0);
	SendMessage(hwnd,EM_SETSEL,i0,i1);
	while (1)
	{
		if (PeekMessage(&msg,0,0,0,PM_REMOVE))
		{
			if ((msg.message == WM_CHAR) && ((msg.wParam&255) == 27)) ExitProcess(0);
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(15);
	}
}

#define CATCHBEG __try
#define CATCHEND __except(exception_getaddr(GetExceptionInformation())){showexception();ExitProcess(0);}
//--------------------------------------------------------------------------------------------------
	//Ken Silverman's multithread library (http://advsys.net/ken)

	//Simple library that takes advantage of multi-threading. For 2 CPU HT, simply do this:
	//Change: for(i=v0;i<v1;i++) myfunc(i);
	//    To: htrun(myfunc,0,v0,v1,2);

#define MAXCPU 64
static HANDLE gthand[MAXCPU-1];
static HANDLE ghevent[2][MAXCPU-1]; //WARNING: WaitForMultipleObjects has a limit of 64 threads
static int gnthreadcnt = 0, glincnt[2];
//static __forceinline int getnextindexsynced (void) { _asm mov eax, 1 _asm lock xadd glincnt, eax }
#define getnextindexsynced() _InterlockedExchangeAdd((long *)&glincnt[0],1)
static void (*ghtcallfunc)(int, void *);
static void *ghtdata;
static void htuninit (void)
{
	int i, j;
	for(i=gnthreadcnt-1;i>=0;i--)
	{
		TerminateThread(gthand[i],0); //Dangerous if using system resources in thread, but we are not
		for(j=1;j>=0;j--)
			{ if (ghevent[j][i] != (HANDLE)-1) { CloseHandle(ghevent[j][i]); ghevent[j][i] = (HANDLE)-1; } }
	}
}
static unsigned int __stdcall ghtfunc (void *_)
{
	void (*lcallfunc)(int, void *);
	void *luserdata;
	int i, thnum = (int)_, endcnt;

	while (1)
	{
		WaitForSingleObject(ghevent[0][thnum],INFINITE);
		lcallfunc = ghtcallfunc; luserdata = ghtdata; endcnt = glincnt[1];
		while ((i = getnextindexsynced()) < endcnt) lcallfunc(i,luserdata);
		SetEvent(ghevent[1][thnum]);
	}
}
static void htrun (void (*lcallfunc)(int, void *), void *luserdata, int v0, int v1, int lnumcpu)
{
	int i, threaduse; unsigned win98requiresme;

		//1 CPU requested; execute here and quit.
	if (lnumcpu <= 1) { for(i=v0;i<v1;i++) lcallfunc(i,luserdata); return; }

		//Initialize new threads if necessary
	threaduse = min(lnumcpu,MAXCPU)-1;
	while (gnthreadcnt < threaduse)
	{
		if (!gnthreadcnt) { SetThreadAffinityMask(GetCurrentThread(),1<<(maxcpu-1)); atexit(htuninit); }
		for(i=0;i<2;i++) ghevent[i][gnthreadcnt] = CreateEvent(0,0,0,0);
		gthand[gnthreadcnt] = (HANDLE)_beginthreadex(0,0,ghtfunc,(void *)gnthreadcnt,0,&win98requiresme);
		SetThreadAffinityMask(gthand[gnthreadcnt],1<<gnthreadcnt);
		gnthreadcnt++;
	}

		//Start other threads
	ghtcallfunc = lcallfunc; ghtdata = luserdata; glincnt[0] = v0; glincnt[1] = v1;
	for(i=threaduse-1;i>=0;i--) SetEvent(ghevent[0][i]);

		//Do some processing in this thread too :)
	while ((i = getnextindexsynced()) < v1) lcallfunc(i,luserdata);

		//Wait for all other threads to finish (for safety reasons)
	WaitForMultipleObjects(threaduse,&ghevent[1][0],1,INFINITE);
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------

#if 0
#define SELFMODVAL(dalab,daval) \
{  void *_daddr; unsigned int _oprot; \
	_asm { mov _daddr, offset dalab } \
	VirtualProtect(_daddr,sizeof(daval),PAGE_EXECUTE_READWRITE,&_oprot); \
	switch(sizeof(daval)) \
	{  case 1: *(char *)_daddr = daval; break; \
		case 2: *(short *)_daddr = daval; break; \
		case 4: *(int *)_daddr = daval; break; \
		case 8: *(__int64 *)_daddr = daval; break; \
	} \
	VirtualProtect(_daddr,sizeof(daval),PAGE_EXECUTE,&_oprot); \
	/*FlushInstructionCache(GetCurrentProcess(),_daddr,sizeof(daval));*/ \
}
#else
#define SELFMODVAL(init,dalab,daval) \
{  void *_daddr; unsigned long _oprot; \
	_asm { mov _daddr, offset dalab } \
	if (init) VirtualProtect(_daddr,sizeof(daval),PAGE_EXECUTE_READWRITE,&_oprot); \
	switch(sizeof(daval)) \
	{  case 1: *(char *)_daddr = daval; break; \
		case 2: *(short *)_daddr = daval; break; \
		case 4: *(int *)_daddr = daval; break; \
		case 8: *(__int64 *)_daddr = daval; break; \
	} \
}
#endif

//--------------------------------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(_WIN64)
#if 1
	//Compatible with memset except:
	//   1. All 32 bits of v are used and expected to be filled
	//   2. Writes max((n+3)&~3,4) bytes
	//   3. Assumes d is aligned on 4 byte boundary
__declspec(naked) static void memset4 (void *d, int v, int n)
{
	_asm
	{
		mov edx, [esp+4] ;d
		mov eax, [esp+8] ;v
		mov ecx, [esp+12] ;n

		sub ecx, 4
		jz short end1
		jl short end2

		test edx, 4
		jz short beg1
		movnti dword ptr [edx], eax
		add edx, 4
		sub ecx, 4
		jl short end2
		jz short end1

beg1: movd mm0, eax
		punpckldq mm0, mm0
beg2: movntq qword ptr [edx], mm0
		add edx, 8
		sub ecx, 8
		jg short beg2
		emms

		jl short end2
end1: movnti dword ptr [edx], eax
end2: ret
	}
}
#else
	//NOTE:This was slower than the above on Ken's C2Q 2.66 :/
	//Compatible with memset except:
	//   1. All 32 bits of v are used and expected to be filled
	//   2. Writes max((n+3)&~3,4) bytes
	//   3. Assumes d is aligned on 4 byte boundary
__declspec(naked) static void memset4 (void *d, int v, int n)
{
	_asm
	{
		mov edx, [esp+4] ;d
		mov eax, [esp+8] ;v
		mov ecx, [esp+12] ;n
		test edx, 15
		jz short skp1
beg1:    test ecx, ecx
			jle short enda
			movnti dword ptr [edx], eax
			add edx, 4
			sub ecx, 4
			test edx, 15
			jnz short beg1
skp1: sub ecx, 16
		jl short skp4
			movd xmm0, eax
			pshufd xmm0, xmm0, 0
beg4:       movntdq [edx], xmm0
				add edx, 16
				sub ecx, 16
				jge short beg4
skp4: add ecx, 16
		jle short enda
end1:    movnti dword ptr [edx], eax
			add edx, 4
			sub ecx, 4
			jg short end1
enda: ret
	}
}
#endif
#elif _MSC_VER
static void memset4 (void *d, unsigned int v, __int64 n)
{
	if ((((int)d)&4) && (n)) { *(int *)d = v; d = (void *)(((INT_PTR)d)+4); n -= 4; }
	__stosq((unsigned __int64 *)d,(((__int64)v)<<32) | ((__int64)v),(__int64)n>>3);
	if (n&4) { *(int *)((n-4)+(INT_PTR)d) = v; }
}
#else
static void memset4 (void *d, unsigned int v, __int64 n)
{
	__int64 i;
	int *iptr = (int *)d; n >>= 2;
	for(i=n-1;i>=0;i--) iptr[i] = v;
}
#endif

	//Compatible with memset except:
	//   1. All 32 bits of v are used and expected to be filled
	//   2. Writes max((n+15)&~15,16) bytes
	//   3. Assumes d is aligned on 16 byte boundary
__declspec(naked) static void memset16_safe (void *d, int v, int n)
{
	_asm
	{
		mov edx, [esp+4] ;d
		mov ecx, [esp+12] ;n
		movd xmm0, [esp+8] ;v
		pshufd xmm0, xmm0, 0
		add edx, ecx
		neg ecx
		test edx, 15
		jz short memset16aligned
memset16unaligned:
		movdqu [edx+ecx], xmm0
		add ecx, 16
		jl short memset16unaligned
		ret
memset16aligned:
		movaps [edx+ecx], xmm0
		add ecx, 16
		jl short memset16aligned
		ret
	}
}

	//Compatible with memset except:
	//   1. All 32 bits of v are used and expected to be filled
	//   2. Writes max((n+15)&~15,16) bytes
	//   3. Assumes d is aligned on 16 byte boundary
__declspec(naked) static void memset16 (void *d, int v, int n)
{
	_asm
	{
		mov edx, [esp+4] ;d
		mov ecx, [esp+12] ;n
		movd xmm0, [esp+8] ;v
		pshufd xmm0, xmm0, 0
		add edx, ecx
		neg ecx
memset16aligned:
		movaps [edx+ecx], xmm0
		add ecx, 16
		jl short memset16aligned
		ret
	}
}

__forceinline static void memand16 (void *d, void *s, int n)
{
	_asm
	{
		mov edx, d
		mov eax, s
		mov ecx, n
memand16beg:
		movaps xmm0, [edx+ecx-16]
		pand   xmm0, [eax+ecx-16]
		movaps [edx+ecx-16], xmm0
		sub ecx, 16
		jg short memand16beg
	}
}

//--------------------------------------------------------------------------------------------------

static const int pow2[32] =
{
	0x00000001,0x00000002,0x00000004,0x00000008,0x00000010,0x00000020,0x00000040,0x00000080,
	0x00000100,0x00000200,0x00000400,0x00000800,0x00001000,0x00002000,0x00004000,0x00008000,
	0x00010000,0x00020000,0x00040000,0x00080000,0x00100000,0x00200000,0x00400000,0x00800000,
	0x01000000,0x02000000,0x04000000,0x08000000,0x10000000,0x20000000,0x40000000,0x80000000,
};
static const int pow2m1[32] =
{
	0x00000000,0x00000001,0x00000003,0x00000007,0x0000000f,0x0000001f,0x0000003f,0x0000007f,
	0x000000ff,0x000001ff,0x000003ff,0x000007ff,0x00000fff,0x00001fff,0x00003fff,0x00007fff,
	0x0000ffff,0x0001ffff,0x0003ffff,0x0007ffff,0x000fffff,0x001fffff,0x003fffff,0x007fffff,
	0x00ffffff,0x01ffffff,0x03ffffff,0x07ffffff,0x0fffffff,0x1fffffff,0x3fffffff,0x7fffffff,
};
static const int npow2[32] =
{
	0xffffffff,0xfffffffe,0xfffffffc,0xfffffff8,0xfffffff0,0xffffffe0,0xffffffc0,0xffffff80,
	0xffffff00,0xfffffe00,0xfffffc00,0xfffff800,0xfffff000,0xffffe000,0xffffc000,0xffff8000,
	0xffff0000,0xfffe0000,0xfffc0000,0xfff80000,0xfff00000,0xffe00000,0xffc00000,0xff800000,
	0xff000000,0xfe000000,0xfc000000,0xf8000000,0xf0000000,0xe0000000,0xc0000000,0x80000000,
};
static const int popcount[256] =
{ //http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetTable
#define B2(n)    n ,   n+1 ,   n+1 ,   n+2
#define B4(n) B2(n),B2(n+1),B2(n+1),B2(n+2)
#define B6(n) B4(n),B4(n+1),B4(n+1),B4(n+2)
	B6(0),B6(1),B6(1),B6(2)
};

#define cvtss2si(f) _mm_cvt_ss2si(_mm_set_ss(f))
#define cvttss2si(f) _mm_cvtt_ss2si(_mm_set_ss(f))
#define atomicadd(p,v) _InterlockedExchangeAdd((long *)p,v)

#ifdef _MSC_VER
#ifndef _WIN64
static __forceinline unsigned int bsf (unsigned int a) { _asm bsf eax, a }
static __forceinline unsigned int bsr (unsigned int a) { _asm bsr eax, a }
#else
static __forceinline unsigned int bsf (unsigned int a) { unsigned long r; _BitScanForward(&r,a); return(r); }
static __forceinline unsigned int bsr (unsigned int a) { unsigned long r; _BitScanReverse(&r,a); return(r); }
static __forceinline unsigned int bsf64 (unsigned __int64 a) { unsigned long r; _BitScanForward64(&r,a); return(r); }
static __forceinline unsigned int bsr64 (unsigned __int64 a) { unsigned long r; _BitScanReverse64(&r,a); return(r); }
#endif
#else
//#define bsf(x) __builtin_clz(x) //Count Leading  Zeros; compiler generates function call!
//#define bsr(x) __builtin_ctz(x) //Count Trailing Zeros; compiler generates function call! GCC bug: same as "_clz!
//#define bsf64(x) __builtin_clzl(x) //Count Leading  Zeros 64-bit (untested)
//#define bsr64(x) __builtin_ctzl(x) //Count Trailing Zeros 64-bit (untested)
#define bsf(r)   ({ long      __r=(r); __asm__ __volatile__ ("bsf %0, %0;": "=a" (__r): "a" (__r): "cc"); __r; })
#define bsr(r)   ({ long      __r=(r); __asm__ __volatile__ ("bsr %0, %0;": "=a" (__r): "a" (__r): "cc"); __r; })
#define bsf64(r) ({ long long __r=(r); __asm__ __volatile__ ("bsf %0, %0;": "=a" (__r): "a" (__r): "cc"); __r; })
#define bsr64(r) ({ long long __r=(r); __asm__ __volatile__ ("bsr %0, %0;": "=a" (__r): "a" (__r): "cc"); __r; })
#endif

static __forceinline int dntil0 (unsigned int *lptr, int z, int zsiz)
{
	//   //This line does the same thing (but slow & brute force)
	//while ((z < zsiz) && (lptr[z>>5]&(1<<KMOD32(z)))) z++; return(z);
	int i;
		//WARNING: zsiz must be multiple of 32!
	i = (lptr[z>>5]|((1<<KMOD32(z))-1)); z &= ~31;
	while (i == 0xffffffff)
	{
		z += 32; if (z >= zsiz) return(zsiz);
		i = lptr[z>>5];
	}
	return(bsf(~i)+z);
}

static __forceinline int uptil0 (unsigned int *lptr, int z)
{
	//   //This line does the same thing (but slow & brute force)
	//while ((z > 0) && (lptr[(z-1)>>5]&(1<<KMOD32(z-1)))) z--; return(z);
	int i;
	if (!z) return(0); //Prevent possible crash
	i = (lptr[(z-1)>>5]|(-1<<KMOD32(z))); z &= ~31;
	while (i == 0xffffffff)
	{
		z -= 32; if (z < 0) return(0);
		i = lptr[z>>5];
	}
	return(bsr(~i)+z+1);
}

static __forceinline int dntil1 (unsigned int *lptr, int z, int zsiz)
{
	//   //This line does the same thing (but slow & brute force)
	//while ((z < zsiz) && (!(lptr[z>>5]&(1<<KMOD32(z))))) z++; return(z);
	int i;
		//WARNING: zsiz must be multiple of 32!
	i = (lptr[z>>5]&(-1<<KMOD32(z))); z &= ~31;
	while (!i)
	{
		z += 32; if (z >= zsiz) return(zsiz);
		i = lptr[z>>5];
	}
	return(bsf(i)+z);
}

static __forceinline int uptil1 (unsigned int *lptr, int z)
{
	//   //This line does the same thing (but slow & brute force)
	//while ((z > 0) && (!(lptr[(z-1)>>5]&(1<<KMOD32(z-1))))) z--; return(z);
	int i;
	if (!z) return(0); //Prevent possible crash
	i = (lptr[(z-1)>>5]&((1<<KMOD32(z))-1)); z &= ~31;
	while (!i)
	{
		z -= 32; if (z < 0) return(0);
		i = lptr[z>>5];
	}
	return(bsr(i)+z+1);
}

	//Swap bits in range {i0<=?<i0+n}; n must be <= 25
static __forceinline void xorzrangesmall (void *iptr, int i0, int n)
	{ *(int *)((i0>>3)+(INT_PTR)iptr) ^= (pow2m1[n]<<(i0&7)); }

	//Set all bits in vbit from (x,y,z0) to (x,y,z1-1) to 0's
#ifndef _WIN64

static __forceinline void setzrange0 (void *vptr, int z0, int z1)
{
	int z, ze, *iptr = (int *)vptr;
	if (!((z0^z1)&~31)) { iptr[z0>>5] &= ((~(-1<<z0))|(-1<<z1)); return; }
	z = (z0>>5); ze = (z1>>5);
	iptr[z] &=~(-1<<z0); for(z++;z<ze;z++) iptr[z] = 0;
	iptr[z] &= (-1<<z1);
}

	//Set all bits in vbit from (x,y,z0) to (x,y,z1-1) to 1's
static __forceinline void setzrange1 (void *vptr, int z0, int z1)
{
	int z, ze, *iptr = (int *)vptr;
	if (!((z0^z1)&~31)) { iptr[z0>>5] |= ((~(-1<<z1))&(-1<<z0)); return; }
	z = (z0>>5); ze = (z1>>5);
	iptr[z] |= (-1<<z0); for(z++;z<ze;z++) iptr[z] = -1;
	iptr[z] |=~(-1<<z1);
}

#else

static __forceinline void setzrange0 (void *vptr, __int64 z0, __int64 z1)
{
	unsigned __int64 z, ze, *iptr = (unsigned __int64 *)vptr;
	if (!((z0^z1)&~63)) { iptr[z0>>6] &= ((~(LL(-1)<<z0))|(LL(-1)<<z1)); return; }
	z = (z0>>6); ze = (z1>>6);
	iptr[z] &=~(LL(-1)<<z0); for(z++;z<ze;z++) iptr[z] = LL(0);
	iptr[z] &= (LL(-1)<<z1);
}

	//Set all bits in vbit from (x,y,z0) to (x,y,z1-1) to 1's
static __forceinline void setzrange1 (void *vptr, __int64 z0, __int64 z1)
{
	unsigned __int64 z, ze, *iptr = (unsigned __int64 *)vptr;
	if (!((z0^z1)&~63)) { iptr[z0>>6] |= ((~(LL(-1)<<z1))&(LL(-1)<<z0)); return; }
	z = (z0>>6); ze = (z1>>6);
	iptr[z] |= (LL(-1)<<z0); for(z++;z<ze;z++) iptr[z] = LL(-1);
	iptr[z] |=~(LL(-1)<<z1);
}

#endif

	//Returns 1 if any bits in vbit from (x,y,z0) to (x,y,z1-1) are 0
static __forceinline int anyzrange0 (unsigned int *lptr, int z0, int z1)
{
	int z, ze;
	if (!((z0^z1)&~31)) { return((lptr[z0>>5] | ((~(-1<<KMOD32(z0)))|(-1<<KMOD32(z1)))) != 0xffffffff); }
	z = (z0>>5); ze = (z1>>5);
	if ((lptr[z] |~(-1<<KMOD32(z0))) != 0xffffffff) return(1); for(z++;z<ze;z++) if (lptr[z] != 0xffffffff) return(1);
	if ((lptr[z] | (-1<<KMOD32(z1))) != 0xffffffff) return(1);
	return(0);
}

	//returns ARGB; 0x404040 in i or j is no change
static __forceinline int mulcol (int i, int j)
{
	_asm
	{
		pxor xmm7, xmm7
		movd xmm0, i
		punpcklbw xmm0, xmm7
		movd xmm1, j
		punpcklbw xmm1, xmm7
		pmullw xmm0, xmm1
		psrlw xmm0, 6
		packuswb xmm0, xmm0
		movd eax, xmm0
	}
}

	//i:ARGB, j:scale (256 is no change), returns ARGB
static __forceinline int mulsc (int i, int j)
{
	_asm
	{
		movd xmm0, i
		punpcklbw xmm0, xmm0
		movd xmm1, j
		pshuflw xmm1, xmm1, 0
		pmulhuw xmm0, xmm1
		packuswb xmm0, xmm0
		movd eax, xmm0
	}
}

static __forceinline int addcol (int i, int j)
{
	_asm
	{
		movd xmm0, i
		movd xmm1, j
		paddusb xmm0, xmm1
		movd eax, xmm0
	}
}

static __forceinline int subcol (int i, int j)
{
	_asm
	{
		movd xmm0, i
		movd xmm1, j
		psubusb xmm0, xmm1
		movd eax, xmm0
	}
}

#if 0
static int lerp (int i0, int i1, int fx)
{
	int r0, g0, b0, r1, g1, b1;
	r0 = ((i0>>16)&255); g0 = ((i0>>8)&255); b0 = (i0&255);
	r1 = ((i1>>16)&255); g1 = ((i1>>8)&255); b1 = (i1&255);
	r0 += (((r1-r0)*fx)>>15); r0 = min(max(r0,0),255);
	g0 += (((g1-g0)*fx)>>15); g0 = min(max(g0,0),255);
	b0 += (((b1-b0)*fx)>>15); b0 = min(max(b0,0),255);
	return((r0<<16)+(g0<<8)+b0);
}
#elif 1
static __forceinline int lerp (int i0, int i1, int fx)
{
	_asm
	{
		pxor xmm7, xmm7

		movd xmm0, i0
		punpcklbw xmm0, xmm7
		movd xmm1, i1
		punpcklbw xmm1, xmm7

		movd xmm2, fx
		pshuflw xmm2, xmm2, 0

		psubw xmm1, xmm0
		psllw xmm1, 1
		pmulhw xmm1, xmm2
		paddw xmm0, xmm1

		packuswb xmm0, xmm0
		movd eax, xmm0
	}
}
#else
	//untested..
static __forceinline int lerp (int i0, int i1, int fx)
{
	__declspec(align(16)) static const int dq0000[4] = {0,0,0,0}, dq1100[4] = {0,0,-1,-1};

	_asm
	{
		movd xmm0, i0
		movd xmm1, i1
		punpckldq xmm0, xmm1
		punpcklbw xmm0, dq0000

		movd xmm1, fx
		pshuflw xmm1, xmm1, 0
		movlhps xmm1, xmm1
		xorps xmm1, dq1100
		pmulhw xmm0, xmm1
		movhlps xmm1, xmm0
		paddw xmm0, xmm1

		packuswb xmm0, xmm0
		movd eax, xmm0
	}
}
#endif

#if 0
static int bilinfilt (int *i01, int *i23, int fx, int fy)
{
	int i0, i1, i2, i3, r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
	i0 = i01[0]; i1 = i01[1]; fx &= 0xffff;
	i2 = i23[0]; i3 = i23[1]; fy &= 0xffff;
	r0 = ((i0>>16)&255); g0 = ((i0>>8)&255); b0 = (i0&255);
	r1 = ((i1>>16)&255); g1 = ((i1>>8)&255); b1 = (i1&255);
	r2 = ((i2>>16)&255); g2 = ((i2>>8)&255); b2 = (i2&255);
	r3 = ((i3>>16)&255); g3 = ((i3>>8)&255); b3 = (i3&255);
	r0 += (((r1-r0)*fx)>>16); r2 += (((r3-r2)*fx)>>16);
	g0 += (((g1-g0)*fx)>>16); g2 += (((g3-g2)*fx)>>16);
	b0 += (((b1-b0)*fx)>>16); b2 += (((b3-b2)*fx)>>16);
	r0 += (((r2-r0)*fy)>>16); r0 = min(max(r0,0),255);
	g0 += (((g2-g0)*fy)>>16); g0 = min(max(g0,0),255);
	b0 += (((b2-b0)*fy)>>16); b0 = min(max(b0,0),255);
	return((r0<<16)+(g0<<8)+b0);
}
#elif 0
//__declspec(naked) static int bilinfilt (int *i01, int *i23, int fx, int fy)
__forceinline static int bilinfilt (int *i01, int *i23, int fx, int fy)
{
	_asm
	{
		pxor xmm7, xmm7

		mov eax, i01
		movd xmm0, [eax] ;[esp+4]
		punpcklbw xmm0, xmm7
		movd xmm1, [eax+4]
		punpcklbw xmm1, xmm7
		mov eax, i23
		movd xmm2, [eax] ;[esp+8]
		punpcklbw xmm2, xmm7
		movd xmm3, [eax+4]
		punpcklbw xmm3, xmm7

		movd xmm4, fx ;[esp+12]
		movd xmm5, fy ;[esp+16]
		psrlw xmm4, 1
		psrlw xmm5, 1
		pshuflw xmm4, xmm4, 0
		pshuflw xmm5, xmm5, 0

		psubw xmm1, xmm0
		psubw xmm3, xmm2
		psllw xmm1, 1
		psllw xmm3, 1
		pmulhw xmm1, xmm4
		pmulhw xmm3, xmm4
		paddw xmm0, xmm1
		paddw xmm2, xmm3

		psubw xmm2, xmm0
		psllw xmm2, 1
		pmulhw xmm2, xmm5
		paddw xmm0, xmm2

		packuswb xmm0, xmm0
		movd eax, xmm0
		;ret
	}
}
#else
//__declspec(naked) static int bilinfilt (int *i01, int *i23, int fx, int fy)
__forceinline static int bilinfilt (int *i01, int *i23, int fx, int fy)
{
	__declspec(align(16)) static const int dq0000[4] = {0,0,0,0}, dq1111[4] = {-1,-1,-1,-1};

		//algo:
		//+ i0*(1-fx)*(1-fy) + i1*(fx)*(1-fy)
		//+ i2*(1-fx)*(  fy) + i3*(fx)*(  fy)
	_asm
	{
		mov eax, i01
		movups xmm0, [eax]
		mov eax, i23
		movups xmm1, [eax]
		punpcklbw xmm0, dq0000   ;xmm0:[a1 r1 g1 b1  a0 r0 g0 b0]
		punpcklbw xmm1, dq0000   ;xmm1:[a3 r3 g3 b3  a2 r2 g2 b2]

		movd xmm2, fx            ;xmm2:[  0   0   0   0   0   0   0  fx]
		movd xmm3, fy            ;xmm3:[  0   0   0   0   0   0   0  fy]
		pshuflw xmm2, xmm2, 0x00 ;xmm2:[  0   0   0   0  fx  fx  fx  fx]
		pshuflw xmm3, xmm3, 0x00 ;xmm3:[  0   0   0   0  fy  fy  fy  fy]
		movlhps xmm3, xmm3       ;xmm3:[ fy  fy  fy  fy  fy  fy  fy  fy]

		pmulhuw xmm1, xmm3
		xorps xmm3, dq1111       ;xmm3:[~fy ~fy ~fy ~fy ~fy ~fy ~fy ~fy]
		pmulhuw xmm0, xmm3
		paddw xmm0, xmm1
		movhlps xmm1, xmm0

		pmulhuw xmm1, xmm2
		xorps xmm2, dq1111       ;xmm2:[ -1  -1  -1  -1 ~fx ~fx ~fx ~fx]
		pmulhuw xmm0, xmm2
		paddw xmm0, xmm1

		packuswb xmm0, xmm0
		movd eax, xmm0
	}
}
#endif

static __forceinline float klog (float f)
{
		//Super fast natural log, error = +/-0.02983002966476566
	return(((float)((*(int *)&f)-0x3f7a7dcf))*8.262958294867817e-8);

		//Fast natural log, error = +/-0.003423966993
	//float g = ((float)(((*(int *)&f)&8388607)-4074142))*5.828231702537851e-8;
	//return(((float)(*(int *)&f))*8.262958294867817e-8 - g*g - 87.96988524938206);
}
#define klog2(f) (klog(f)*1.442695040888963)
#define klog10(f) (klog(f)*0.4342944819032518)

static __forceinline float klog2up7 (float f) //returns: klog2(f)*128.0
{
		//Super fast natural log, error = +/-0.02983002966476566
	return(((float)((*(int *)&f)-0x3f7a7dcf))*(8.262958294867817e-8 * 1.442695040888963 * 128.0));
}

static void invert3x3 (dpoint3d *r, dpoint3d *d, dpoint3d *f, double *mat)
{
	double rdet;

	mat[0] = d->y*f->z - d->z*f->y;
	mat[1] = d->z*f->x - d->x*f->z;
	mat[2] = d->x*f->y - d->y*f->x;
	rdet = 1.0/(mat[0]*r->x + mat[1]*r->y + mat[2]*r->z);
	mat[0] *= rdet;
	mat[1] *= rdet;
	mat[2] *= rdet;
	mat[3] = (f->y*r->z - f->z*r->y)*rdet;
	mat[4] = (f->z*r->x - f->x*r->z)*rdet;
	mat[5] = (f->x*r->y - f->y*r->x)*rdet;
	mat[6] = (r->y*d->z - r->z*d->y)*rdet;
	mat[7] = (r->z*d->x - r->x*d->z)*rdet;
	mat[8] = (r->x*d->y - r->y*d->x)*rdet;
}
static void invert3x3 (point3d *r, point3d *d, point3d *f, float *mat)
{
	float rdet;

	mat[0] = d->y*f->z - d->z*f->y;
	mat[1] = d->z*f->x - d->x*f->z;
	mat[2] = d->x*f->y - d->y*f->x;
	rdet = 1.f/(mat[0]*r->x + mat[1]*r->y + mat[2]*r->z);
	mat[0] *= rdet;
	mat[1] *= rdet;
	mat[2] *= rdet;
	mat[3] = (f->y*r->z - f->z*r->y)*rdet;
	mat[4] = (f->z*r->x - f->x*r->z)*rdet;
	mat[5] = (f->x*r->y - f->y*r->x)*rdet;
	mat[6] = (r->y*d->z - r->z*d->y)*rdet;
	mat[7] = (r->z*d->x - r->x*d->z)*rdet;
	mat[8] = (r->x*d->y - r->y*d->x)*rdet;
}

	//optimization for (r,d,f) symmetrix matrix
static void invert3x3sym (dpoint3d *r, dpoint3d *d, dpoint3d *f, double *mat)
{
	double rdet;

	mat[0] = d->y*f->z - d->z*d->z;
	mat[1] = r->z*d->z - r->y*f->z;
	mat[2] = r->y*d->z - r->z*d->y;
	rdet = 1.0/(mat[0]*r->x + mat[1]*r->y + mat[2]*r->z);
	mat[0] *= rdet;
	mat[1] *= rdet;
	mat[2] *= rdet;
	mat[5] = (r->y*r->z - r->x*d->z)*rdet;
	mat[4] = (r->x*f->z - r->z*r->z)*rdet;
	mat[8] = (r->x*d->y - r->y*r->y)*rdet;
	mat[3] = mat[1];
	mat[6] = mat[2];
	mat[7] = mat[5];
}
static void invert3x3sym (point3d *r, point3d *d, point3d *f, float *mat)
{
	float rdet;

	mat[0] = d->y*f->z - d->z*d->z;
	mat[1] = r->z*d->z - r->y*f->z;
	mat[2] = r->y*d->z - r->z*d->y;
	rdet = 1.f/(mat[0]*r->x + mat[1]*r->y + mat[2]*r->z);
	mat[0] *= rdet;
	mat[1] *= rdet;
	mat[2] *= rdet;
	mat[5] = (r->y*r->z - r->x*d->z)*rdet;
	mat[4] = (r->x*f->z - r->z*r->z)*rdet;
	mat[8] = (r->x*d->y - r->y*r->y)*rdet;
	mat[3] = mat[1];
	mat[6] = mat[2];
	mat[7] = mat[5];
}

//--------------------------------------------------------------------------------------------------

static void tiles_genmip (tiles_t *rtile, tiles_t *wtile)
{
	unsigned char *wptr, *rptr, *rptr2;
	int i, x, y;

	if (wtile->x <= 0) return;
	rptr = (unsigned char *)rtile->f;
	wptr = (unsigned char *)wtile->f;
	for(y=0;y<wtile->y;y++,wptr+=wtile->p,rptr+=rtile->p*2)
	{
#if 0
		for(x=0;x<wtile->x*4;x++)
		{
			rptr2 = &rptr[(x&~3)+x];
			wptr[x] = (((int)rptr2[0] + (int)rptr2[4] + (int)rptr2[rtile->p] + (int)rptr2[rtile->p+4] + 2)>>2); //linear crap interpolation (artifact:gets darker in distance)
			//wptr[x] = (int)sqrt(((int)rptr2[         0]*(int)rptr2[         0] + //Gamma=2.0 interpolation (better approx)
			//                     (int)rptr2[         4]*(int)rptr2[         4] +
			//                     (int)rptr2[rtile->p  ]*(int)rptr2[rtile->p  ] +
			//                     (int)rptr2[rtile->p+4]*(int)rptr2[rtile->p+4])*.25f);
		}
#else
		_asm
		{
			push edi

			mov ecx, rptr
			mov edx, rtile
			mov edx, [edx + tiles_t.p]
			add edx, ecx
			mov edi, wptr

			mov eax, wtile
			mov eax, [eax + tiles_t.x]
			lea ecx, [ecx+eax*8]
			lea edx, [edx+eax*8]
			lea edi, [edi+eax*4]
			neg eax
	  top:
				;[edi+eax*4+0] = (([eax*8+0 + ecx] + [eax*8+4 + ecx] + [eax*8+0 + edx] + [eax*8+4 + edx] + 2)>>2)
				;[edi+eax*4+1] = (([eax*8+1 + ecx] + [eax*8+5 + ecx] + [eax*8+1 + edx] + [eax*8+5 + edx] + 2)>>2)
				;[edi+eax*4+2] = (([eax*8+2 + ecx] + [eax*8+6 + ecx] + [eax*8+2 + edx] + [eax*8+6 + edx] + 2)>>2)
				;[edi+eax*4+3] = (([eax*8+3 + ecx] + [eax*8+7 + ecx] + [eax*8+3 + edx] + [eax*8+7 + edx] + 2)>>2)
			movups xmm0, [eax*8+ecx]  ;xmm0: [A5 R5 G5 B5 A4 R4 G4 B4 A1 R1 G1 B1 A0 R0 G0 B0]
			movups xmm1, [eax*8+edx]  ;xmm1: [A7 R7 G7 B7 A6 R6 G6 B6 A3 R3 G3 B3 A2 R2 G2 B2]
			pavgb xmm0, xmm1          ;xmm0: [avg1b avg1a avg0b avg0a]
			pshufd xmm1, xmm0, 0x08   ;xmm1: [  x     x   avg1a avg0a]
			pshufd xmm0, xmm0, 0x0d   ;xmm0: [  x     x   avg1b avg0b]
			pavgb xmm0, xmm1          ;xmm0: [  x     x   avg1  avg0 ]
			movsd [edi+eax*4], xmm0
			add eax, 2
			jl short top
	  bot:

			pop edi
		}
#endif
	}
}

static HDC glhDC;
static HGLRC glhRC;

#define GLAPIENTRY APIENTRY
#define GL_MIRRORED_REPEAT          0x8370
#define GL_TEXTURE_WRAP_R           0x8072
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_TEXTURE_CUBE_MAP         0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_TEXTURE_3D               0x806F
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_VERTEX_PROGRAM_ARB       0x8620
#define GL_FRAGMENT_PROGRAM_ARB     0x8804
#define GL_PROGRAM_ERROR_STRING_ARB 0x8874
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_TEXTURE0                 0x84c0
#define GL_FRAMEBUFFER_EXT          0x8D40
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_TEXTURE_RECTANGLE_ARB    0x84F5
#define GL_RGBA32F_ARB              0x8814
#define GL_LUMINANCE32F_ARB         0x8818

#define GL_COLOR_ATTACHMENT0_EXT    0x8CE0
#define GL_PIXEL_UNPACK_BUFFER      0x88EC
#define GL_STREAM_DRAW              0x88E0
#define GL_STREAM_READ              0x88E1
#define GL_STREAM_COPY              0x88E2
#define GL_STATIC_DRAW              0x88E4
#define GL_STATIC_READ              0x88E5
#define GL_STATIC_COPY              0x88E6
#define GL_DYNAMIC_DRAW             0x88E8
#define GL_DYNAMIC_READ             0x88E9
#define GL_DYNAMIC_COPY             0x88EA
#define GL_WRITE_ONLY               0x88B9

#define GL_TEXTURE_BUFFER_EXT             0x8C2A
#define GL_MAX_TEXTURE_BUFFER_SIZE_EXT    0x8C2B
#define GL_TEXTURE_BINDING_BUFFER_EXT     0x8C2C
#define GL_TEXTURE_BUFFER_DATA_STORE_BINDING_EXT 0x8C2D
#define GL_TEXTURE_BUFFER_FORMAT_EXT      0x8C2E

typedef int GLsizei;
typedef char GLchar;
typedef char GLcharARB;
typedef unsigned int GLhandleARB;
#include <stddef.h>
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef GLuint (GLAPIENTRY *PFNGLCREATESHADERPROC      )(GLenum type);
typedef GLuint (GLAPIENTRY *PFNGLCREATEPROGRAMPROC     )(void);
typedef void   (GLAPIENTRY *PFNGLSHADERSOURCEPROC      )(GLuint shader, GLsizei count, const GLchar **strings, const GLint *lengths);
typedef void   (GLAPIENTRY *PFNGLCOMPILESHADERPROC     )(GLuint shader);
typedef void   (GLAPIENTRY *PFNGLATTACHSHADERPROC      )(GLuint program, GLuint shader);
typedef void   (GLAPIENTRY *PFNGLLINKPROGRAMPROC       )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLUSEPROGRAMPROC        )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLGETPROGRAMIVPROC      )(GLuint program, GLenum pname, GLint *param);
typedef void   (GLAPIENTRY *PFNGLGETSHADERIVPROC       )(GLuint shader, GLenum pname, GLint *param);
typedef void   (GLAPIENTRY *PFNGLGETINFOLOGARBPROC     )(GLhandleARB obj, GLsizei maxLength, GLsizei *length, GLcharARB *infoLog);
typedef void   (GLAPIENTRY *PFNGLDETACHSHADERPROC      )(GLuint program, GLuint shader);
typedef void   (GLAPIENTRY *PFNGLDELETEPROGRAMPROC     )(GLuint program);
typedef void   (GLAPIENTRY *PFNGLDELETESHADERPROC      )(GLuint shader);
typedef GLint  (GLAPIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar *name);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1FPROC         )(GLint location, GLfloat v0);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2FPROC         )(GLint location, GLfloat v0, GLfloat v1);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3FPROC         )(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4FPROC         )(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void   (GLAPIENTRY *PFNGLUNIFORM1IPROC         )(GLint location, GLint v0);
typedef void   (GLAPIENTRY *PFNGLUNIFORM2IPROC         )(GLint location, GLint v0, GLint v1);
typedef void   (GLAPIENTRY *PFNGLUNIFORM3IPROC         )(GLint location, GLint v0, GLint v1, GLint v2);
typedef void   (GLAPIENTRY *PFNGLUNIFORM4IPROC         )(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
typedef void   (GLAPIENTRY *PFNGLACTIVETEXTUREPROC     )(GLuint texture);
typedef void   (GLAPIENTRY *PFNGLTEXIMAGE3DPROC        )(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
typedef void   (GLAPIENTRY *PFNGLTEXSUBIMAGE3DPROC     )(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
typedef GLint  (GLAPIENTRY *PFNWGLSWAPINTERVALEXTPROC  )(GLint interval);
typedef void   (GLAPIENTRY *PFNGLGENPROGRAMSARBPROC            )(GLsizei n, GLuint *programs);
typedef void   (GLAPIENTRY *PFNGLBINDPROGRAMARBPROC            )(GLenum target, GLuint program);
typedef void   (GLAPIENTRY *PFNGLGETPROGRAMSTRINGARBPROC       )(GLenum target, GLenum pname, GLvoid *string);
typedef void   (GLAPIENTRY *PFNGLPROGRAMSTRINGARBPROC          )(GLenum target, GLenum format, GLsizei len, const GLvoid *string);
typedef void   (GLAPIENTRY *PFNGLPROGRAMLOCALPARAMETER4FARBPROC)(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void   (GLAPIENTRY *PFNGLPROGRAMENVPARAMETER4FARBPROC  )(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void   (GLAPIENTRY *PFNGLDELETEPROGRAMSARBPROC         )(GLsizei n, const GLuint *programs);
typedef void   (GLAPIENTRY *PFNGLBINDBUFFER             )(GLenum, GLuint);
typedef void   (GLAPIENTRY *PFNGLDELETEBUFFERS          )(GLsizei, const GLuint *);
typedef void   (GLAPIENTRY *PFNGLGENBUFFERS             )(GLsizei, GLuint *);
typedef void   (GLAPIENTRY *PFNGLBUFFERDATA             )(GLenum, GLsizeiptr, const GLvoid *, GLenum);
typedef void   (GLAPIENTRY *PFNGLBUFFERSUBDATA          )(GLenum, GLintptr, GLsizeiptr, const GLvoid *);
typedef GLvoid*(GLAPIENTRY *PFNGLMAPBUFFER              )(GLenum, GLenum);
typedef GLboolean (GLAPIENTRY *PFNGLUNMAPBUFFER         )(GLenum);
typedef void   (GLAPIENTRY *PFNGLBINDFRAMEBUFFEREXT     )(GLenum, GLuint);
typedef void   (GLAPIENTRY *PFNGLDELETEFRAMEBUFFERSEXT  )(GLsizei, const GLuint *);
typedef void   (GLAPIENTRY *PFNGLGENFRAMEBUFFERSEXT     )(GLsizei, GLuint *);
typedef void   (GLAPIENTRY *PFNGLFRAMEBUFFERTEXTURE2DEXT)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (GLAPIENTRY *PFNGLTEXBUFFEREXT           )(GLenum target, GLenum internalformat, GLuint buffer);

static int useoldglfuncs = 0;
const static char *glnames[] =
{
	"glGenProgramsARB","glBindProgramARB",                      //ARB ASM...
	"glGetProgramStringARB","glProgramStringARB",
	"glProgramLocalParameter4fARB",
	"glProgramEnvParameter4fARB",
	"glDeleteProgramsARB",

	"glActiveTexture","glTexImage3D","glTexSubImage3D",         //multi/extended texture
	"wglSwapIntervalEXT",                                       //limit refresh/sleep

	"glCreateShader","glCreateProgram",                         //compile
	"glShaderSource","glCompileShader",                         //
	"glAttachShader","glLinkProgram","glUseProgram",            //link
	"glGetShaderiv","glGetProgramiv","glGetInfoLogARB",         //get info
	"glDetachShader","glDeleteProgram","glDeleteShader",        //decompile
	"glGetUniformLocation",                                     //host->shader
	"glUniform1f" ,"glUniform2f" ,"glUniform3f" ,"glUniform4f",
	"glUniform1i" ,"glUniform2i" ,"glUniform3i" ,"glUniform4i",

	"glBindBuffer", "glDeleteBuffers", "glGenBuffers",          //stuff for virtual mapping
	"glBufferData", "glBufferSubData",
	"glMapBuffer", "glUnmapBuffer",
	"glBindFramebufferEXT", "glDeleteFramebuffersEXT",
	"glGenFramebuffersEXT", "glFramebufferTexture2DEXT",

	"glTexBufferEXT",
};
const static char *glnames_old[] =
{
	"glGenProgramsARB","glBindProgramARB",                          //ARB ASM...
	"glGetProgramStringARB","glProgramStringARB",
	"glProgramLocalParameter4fARB",
	"glProgramEnvParameter4fARB",
	"glDeleteProgramsARB",

	"glActiveTexture","glTexImage3D","glTexSubImage3D",             //texture unit
	"wglSwapIntervalEXT",                                           //limit refresh/sleep

	"glCreateShaderObjectARB","glCreateProgramObjectARB",           //compile
	"glShaderSourceARB","glCompileShaderARB",                       //
	"glAttachObjectARB","glLinkProgramARB","glUseProgramObjectARB", //link
	"glGetObjectParameterivARB","glGetObjectParameterivARB","glGetInfoLogARB", //get info
	"glDetachObjectARB","glDeleteObjectARB","glDeleteObjectARB",    //decompile
	"glGetUniformLocationARB",                                      //host->shader
	"glUniform1fARB" ,"glUniform2fARB" ,"glUniform3fARB" ,"glUniform4fARB",
	"glUniform1iARB" ,"glUniform2iARB" ,"glUniform3iARB" ,"glUniform4iARB",

	"glBindBuffer", "glDeleteBuffers", "glGenBuffers",              //stuff for virtual mapping
	"glBufferData", "glBufferSubData",
	"glMapBuffer", "glUnmapBuffer",
	"glBindFramebufferEXT", "glDeleteFramebuffersEXT",
	"glGenFramebuffersEXT", "glFramebufferTexture2DEXT",

	"glTexBufferEXT",
};
enum
{
	glGenProgramsARB,glBindProgramARB,             //ARB ASM...
	glGetProgramStringARB,glProgramStringARB,
	glProgramLocalParameter4fARB,
	glProgramEnvParameter4fARB,
	glDeleteProgramsARB,

	glActiveTexture,glTexImage3D,glTexSubImage3D,  //multi/extended texture
	wglSwapIntervalEXT,                            //limit refesh/sleep

	glCreateShader,glCreateProgram,                //compile
	glShaderSource,glCompileShader,                //
	glAttachShader,glLinkProgram,glUseProgram,     //link
	glGetShaderiv,glGetProgramiv,glGetInfoLogARB,  //get info
	glDetachShader,glDeleteProgram,glDeleteShader, //decompile
	glGetUniformLocation,                          //host->shader
	glUniform1f, glUniform2f, glUniform3f, glUniform4f,
	glUniform1i, glUniform2i, glUniform3i, glUniform4i,

	glBindBuffer, glDeleteBuffers, glGenBuffers,   //stuff for virtual mapping
	glBufferData, glBufferSubData,
	glMapBuffer, glUnmapBuffer,
	glBindFramebufferEXT, glDeleteFramebuffersEXT,
	glGenFramebuffersEXT, glFramebufferTexture2DEXT,

	glTexBufferEXT,

	NUMGLFUNC
};
typedef void (*glfp_t)(void);
static glfp_t glfp[NUMGLFUNC] = {0};

static unsigned int shadcur = 0;
#define SHADNUM 5
	//[0]=oct_*, [1]=drawcone, [2]=drawsky, [3]=drawtext, [4]=drawpol(need shader to brighten :/)
static unsigned int shadprog[SHADNUM], shadvert[SHADNUM], shadfrag[SHADNUM];

	//GLU replacements..
static int gluBuild2DMipmaps (GLenum target, GLint components, GLint xs, GLint ys, GLenum format, GLenum type, const void *data)
{
	tiles_t rtile, wtile;
	int i;

	for(i=1;(xs|ys)>1;i++)
	{
		rtile.f = (INT_PTR)data; rtile.p = (xs<<2); rtile.x = xs; rtile.y = ys; xs = max(xs>>1,1); ys = max(ys>>1,1); //from GL_ARB_texture_non_power_of_two spec
		wtile.f = (INT_PTR)data; wtile.p = (xs<<2); wtile.x = xs; wtile.y = ys;
		tiles_genmip(&rtile,&wtile);
		glTexImage2D   (target,i,4  ,xs,ys,0,format,type,data); //loading 1st time
	 //glTexSubImage2D(target,i,0,0,xs,ys  ,format,type,data); //overwrite old texture
	}
	return(0);
}

#if (GPUSEBO != 0) //-------------------------------------------------------------------------------
	//Pixel Buffer Object (PBO); see: http://www.mathematik.uni-dortmund.de/~goeddeke/gpgpu/tutorial3.html
static void bo_uninit (void) { ((PFNGLDELETEBUFFERS)glfp[glDeleteBuffers])(1,&gpixbufid); }
static unsigned int bo_init (int nbytes)
{
	unsigned int bufid;
	((PFNGLGENBUFFERS)glfp[glGenBuffers])(1,&bufid);
	((PFNGLBINDBUFFER)glfp[glBindBuffer])(GL_PIXEL_UNPACK_BUFFER,bufid);
	((PFNGLBUFFERDATA)glfp[glBufferData])(GL_PIXEL_UNPACK_BUFFER,nbytes,NULL,GL_DYNAMIC_COPY);
	return(bufid);
}
static void *bo_begin (int bufid, int nbytes)
{
	void *v;
	((PFNGLBINDBUFFER)glfp[glBindBuffer])(GL_PIXEL_UNPACK_BUFFER,bufid);
	if (nbytes)
	{     //NOTE:GL_STREAM_READ slightly faster than GL_STREAM_DRAW, but seems strange?
		((PFNGLBUFFERDATA)glfp[glBufferData])(GL_PIXEL_UNPACK_BUFFER,nbytes,NULL,GL_STREAM_READ);
	}
	v = ((PFNGLMAPBUFFER)glfp[glMapBuffer])(GL_PIXEL_UNPACK_BUFFER,GL_WRITE_ONLY);
	if (!v) { char buf[256]; sprintf(buf,"glMapBuffer() error 0x%08x",glGetError()); MessageBox(ghwnd,buf,prognam,MB_OK); }
	return(v);
}
static void bo_end (int bufid, int xoffs, int yoffs, int xsiz, int ysiz, int k0, int k1, int offs)
{
	((PFNGLBINDBUFFER)glfp[glBindBuffer])(GL_PIXEL_UNPACK_BUFFER,bufid);
	((PFNGLUNMAPBUFFER)glfp[glUnmapBuffer])(GL_PIXEL_UNPACK_BUFFER);
	if (xsiz) glTexSubImage2D(GL_TEXTURE_2D,0,xoffs,yoffs,xsiz,ysiz,k0,k1,((char *)NULL+(offs))); //fast - pure GPU copy
	((PFNGLBINDBUFFER)glfp[glBindBuffer])(GL_PIXEL_UNPACK_BUFFER,0); //MUST unbind buffer object (failure to do so causes future unrelated glTexSubImage2D() to be corrupt!)
}
#endif //-------------------------------------------------------------------------------------------

static const int cubemapconst[6] =
{
	GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
	GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
	GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
};
static const int cubemapindex[6] = {1,3,4,5,0,2};
static void kglalloctex (int itex, void *p, int xs, int ys, int zs, int icoltype)
{
	int i, targ, format, type, internalFormat;

	targ = GL_TEXTURE_3D;
	if (zs == 1)
	{
		targ = GL_TEXTURE_2D;
		if (xs*6 == ys)
		{
			targ = GL_TEXTURE_CUBE_MAP;
			icoltype = (icoltype&~0xf00)|KGL_CLAMP_TO_EDGE;
			if ((icoltype&0xf0) >= KGL_MIPMAP) icoltype = (icoltype&~0xf0)|KGL_LINEAR;
		}
		else if (ys == 1) { targ = GL_TEXTURE_1D; }
	}

#if (GPUSEBO != 0)
	if (!glfp[glBindBuffer]) { MessageBox(ghwnd,"glBindBuffer() not supported .. enjoy the impending crash.",prognam,MB_OK); }
	((PFNGLBINDBUFFER)glfp[glBindBuffer])(GL_PIXEL_UNPACK_BUFFER,0); //MUST unbind buffer object here, before usage of kglalloctex!
#endif

	//glEnable(targ);
	glBindTexture(targ,itex);
	switch (icoltype&0xf0)
	{
		case KGL_LINEAR: default:
			glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
			glTexParameteri(targ,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
			break;
		case KGL_NEAREST:
			glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
			glTexParameteri(targ,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
			break;
		case KGL_MIPMAP0: case KGL_MIPMAP1: case KGL_MIPMAP2: case KGL_MIPMAP3:
			switch(icoltype&0xf0)
			{
				case KGL_MIPMAP0: glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST); break;
				case KGL_MIPMAP1: glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR); break;
				case KGL_MIPMAP2: glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST); break;
				case KGL_MIPMAP3: glTexParameteri(targ,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR); break;
			}
			glTexParameteri(targ,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
			break;
	}
	switch(icoltype&0xf00)
	{
		case KGL_REPEAT: default:
			glTexParameteri(targ,GL_TEXTURE_WRAP_S,GL_REPEAT);
			glTexParameteri(targ,GL_TEXTURE_WRAP_T,GL_REPEAT);
			glTexParameteri(targ,GL_TEXTURE_WRAP_R,GL_REPEAT);
			break;
		case KGL_MIRRORED_REPEAT:
			glTexParameteri(targ,GL_TEXTURE_WRAP_S,GL_MIRRORED_REPEAT);
			glTexParameteri(targ,GL_TEXTURE_WRAP_T,GL_MIRRORED_REPEAT);
			glTexParameteri(targ,GL_TEXTURE_WRAP_R,GL_MIRRORED_REPEAT);
			break;
		case KGL_CLAMP:
			glTexParameteri(targ,GL_TEXTURE_WRAP_S,GL_CLAMP);
			glTexParameteri(targ,GL_TEXTURE_WRAP_T,GL_CLAMP);
			glTexParameteri(targ,GL_TEXTURE_WRAP_R,GL_CLAMP);
			break;
		case KGL_CLAMP_TO_EDGE:
			glTexParameteri(targ,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
			glTexParameteri(targ,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
			glTexParameteri(targ,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
			break;
	}
	//glTexParameterf(targ,GL_TEXTURE_MAX_ANISOTROPY_EXT,1.f); //disable anisotropy for CPU raycast/GPU shader algo! (values > 1.f cause horrible 2x2 edge artifacts!)
	switch(icoltype&15)
	{
		case KGL_BGRA32: internalFormat =                   4; format =  GL_BGRA_EXT; type = GL_UNSIGNED_BYTE; break;
		case KGL_RGBA32: internalFormat =                   4; format =  GL_RGBA    ; type = GL_UNSIGNED_BYTE; break;
		case KGL_CHAR:   internalFormat =       GL_LUMINANCE8; format = GL_LUMINANCE; type = GL_UNSIGNED_BYTE; break;
		case KGL_SHORT:  internalFormat =      GL_LUMINANCE16; format = GL_LUMINANCE; type =GL_UNSIGNED_SHORT; break;
		case KGL_FLOAT:  internalFormat = GL_LUMINANCE32F_ARB; format = GL_LUMINANCE; type =         GL_FLOAT; break;
		case KGL_VEC4:   internalFormat =      GL_RGBA32F_ARB; format =      GL_RGBA; type =         GL_FLOAT; break;
	}
	switch(targ)
	{
		case GL_TEXTURE_1D: glTexImage1D(targ,0,internalFormat,xs,   0,format,type,p); break;
		case GL_TEXTURE_2D: glTexImage2D(targ,0,internalFormat,xs,ys,0,format,type,p); break;
		case GL_TEXTURE_3D: ((PFNGLTEXIMAGE3DPROC)glfp[glTexImage3D])(targ,0,internalFormat,xs,ys,zs,0,format,type,p); break;
		case GL_TEXTURE_CUBE_MAP:
			for(i=0;i<6;i++) glTexImage2D(cubemapconst[i],0,internalFormat,xs,xs,0,format,type,(void *)((INT_PTR)p + xs*xs*4*cubemapindex[i]));
			//for(i=0;i<6;i++) glTexSubImage2D(cubemapconst[i],0,0,0,xs,xs,format,type,(void *)(((INT_PTR)p)+xs*xs*4*cubemapindex[i]));
			break;
	}
}

void oct_freetex (INT_PTR ptr)
{
	if (oct_usegpu)
	{
		glDeleteTextures(1,(unsigned int *)&ptr);
	}
	else
	{
		free((void *)((tiles_t *)ptr)->f);
		free((void *)ptr);
	}
}
INT_PTR oct_loadtex (char *filnam, int flags)
{
	unsigned int hand;
	tiles_t tt, *ptt;

	char *buf;
	int i, x, y, leng;

	tt.f = 0;
	if (!kzopen(filnam)) goto badfile;
	leng = kzfilelength();
	buf = (char *)malloc(leng); if (!buf) { kzclose(); goto badfile; }
	kzread(buf,leng);
	kzclose();

	kpgetdim(buf,leng,&tt.x,&tt.y);
	i = ((!oct_usegpu) && (!(flags&KGL_NEAREST)));
	tt.p = ((tt.x+i)<<2);
	tt.f = (INT_PTR)malloc((tt.y+i)*tt.p + i*16/*for movups in bilinear hack*/); if (!tt.f) { free(buf); goto badfile; }
	if (kprender(buf,leng,tt.f,tt.p,tt.x,tt.y,0,0) < 0) { free(buf); free((void *)tt.f); tt.f = 0; goto badfile; }
	free(buf);

	if (!tt.f) //<-should be if (0), but would compiler eliminate the code?
	{
badfile:;
		tt.x = 64; tt.y = 64; tt.p = (tt.x+i)*sizeof(int);
		tt.f = (INT_PTR)malloc((tt.y+1)*tt.p + i*16/*for movups in bilinear hack*/);
		for(y=0;y<tt.y;y++)
			for(x=0;x<tt.x;x++)
			{     //Generated null sign (copied from BUILD2.C)
				float f, g;
				f = fabs(sqrt((double)((x-tt.x/2)*(x-tt.x/2) + (y-tt.y/2)*(y-tt.y/2)))-tt.x*18.0/64.0)*sqrt(2.0)/2.0;
				if (labs(x-y) < tt.x*25.0/64.0) f = min(f,fabs((double)(x+y-tt.x))*.5);
				if (f < 4) *(int *)(tt.p*y+(x<<2)+tt.f) = ((long)((4-f)*63))*0x010000 + 0xff000000;
						else *(int *)(tt.p*y+(x<<2)+tt.f) = 0xff000000;
			}
	}

	if (i)
	{
		if (!(flags&(KGL_CLAMP|KGL_CLAMP_TO_EDGE)))
		{
			for(y=0;y<tt.y;y++) *(int *)(tt.p*y + (tt.x<<2) + tt.f) = *(int *)(tt.p*y + tt.f);
			memcpy((void *)(tt.p*tt.y + tt.f),(void *)tt.f,(tt.x+1)<<2);
		}
		else
		{
			for(y=0;y<tt.y;y++) *(int *)(tt.p*y + (tt.x<<2) + tt.f) = *(int *)(tt.p*y + ((tt.x-1)<<2) + tt.f);
			memcpy((void *)(tt.p*tt.y + tt.f),(void *)(tt.p*(tt.y-1) + tt.f),(tt.x+1)<<2);
		}
	}

	if (oct_usegpu)
	{
		glGenTextures(1,(unsigned int *)&hand);
		kglalloctex(hand,(void *)tt.f,tt.x,tt.y,1,flags);
		free((void *)tt.f);
		return((INT_PTR)hand);
	}
	else
	{
		ptt = (tiles_t *)malloc(sizeof(tiles_t));
		memcpy(ptt,&tt,sizeof(tiles_t));
		for(ptt->ltilesid=1;(1<<ptt->ltilesid)<ptt->x;ptt->ltilesid<<=1);
		return((INT_PTR)ptt);
	}
}
INT_PTR oct_loadtex (INT_PTR ptr, int xsiz, int ysiz, int flags)
{
	unsigned int hand;
	tiles_t *ptt;
	int i, y;

	if (oct_usegpu)
	{
		glGenTextures(1,(unsigned int *)&hand);
		kglalloctex(hand,(void *)ptr,xsiz,ysiz,1,flags);
		return((INT_PTR)hand);
	}
	else
	{
		ptt = (tiles_t *)malloc(sizeof(tiles_t));
		ptt->x = xsiz; ptt->y = ysiz;

		i = (!(flags&KGL_NEAREST));
		ptt->p = (ptt->x+i)*sizeof(int); ptt->f = (INT_PTR)malloc((ptt->y+i)*ptt->p + i*16/*for movups in bilinear hack*/);
		for(y=0;y<ptt->y;y++) memcpy((void *)(ptt->p*y + ptt->f),(void *)(y*xsiz*4 + ptr),xsiz*sizeof(int));

		if (!(flags&(KGL_CLAMP|KGL_CLAMP_TO_EDGE)))
		{
			for(y=0;y<ptt->y;y++) *(int *)(ptt->p*y + (ptt->x<<2) + ptt->f) = *(int *)(ptt->p*y + ptt->f);
			memcpy((void *)(ptt->p*ptt->y + ptt->f),(void *)ptt->f,(ptt->x+1)<<2);
		}
		else
		{
			for(y=0;y<ptt->y;y++) *(int *)(ptt->p*y + (ptt->x<<2) + ptt->f) = *(int *)(ptt->p*y + ((ptt->x-1)<<2) + ptt->f);
			memcpy((void *)(ptt->p*ptt->y + ptt->f),(void *)(ptt->p*(ptt->y-1) + ptt->f),(ptt->x+1)<<2);
		}

		for(ptt->ltilesid=1;(1<<ptt->ltilesid)<ptt->x;ptt->ltilesid<<=1);
		return((INT_PTR)ptt);
	}
}

static void kglActiveTexture (int texunit) { if (glfp[glActiveTexture]) ((PFNGLACTIVETEXTUREPROC)glfp[glActiveTexture])((texunit&3)+GL_TEXTURE0); }

static void kglProgramLocalParam (unsigned ind, float a, float b, float c, float d)
{
	((PFNGLPROGRAMLOCALPARAMETER4FARBPROC)glfp[glProgramLocalParameter4fARB])(GL_VERTEX_PROGRAM_ARB,ind,a,b,c,d);
	((PFNGLPROGRAMLOCALPARAMETER4FARBPROC)glfp[glProgramLocalParameter4fARB])(GL_FRAGMENT_PROGRAM_ARB,ind,a,b,c,d);
}
static void kglProgramEnvParam (unsigned ind, float a, float b, float c, float d)
{
	((PFNGLPROGRAMENVPARAMETER4FARBPROC)glfp[glProgramEnvParameter4fARB])(GL_VERTEX_PROGRAM_ARB,ind,a,b,c,d);
	((PFNGLPROGRAMENVPARAMETER4FARBPROC)glfp[glProgramEnvParameter4fARB])(GL_FRAGMENT_PROGRAM_ARB,ind,a,b,c,d);
}

static int kglGetUniformLoc (char *shadvarnam) { return(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shadcur],shadvarnam)); }
static void kglUniform1f (unsigned sh, float v0)                               { ((PFNGLUNIFORM1FPROC)glfp[glUniform1f])(sh,v0);          }
static void kglUniform2f (unsigned sh, float v0, float v1)                     { ((PFNGLUNIFORM2FPROC)glfp[glUniform2f])(sh,v0,v1);       }
static void kglUniform3f (unsigned sh, float v0, float v1, float v2)           { ((PFNGLUNIFORM3FPROC)glfp[glUniform3f])(sh,v0,v1,v2);    }
static void kglUniform4f (unsigned sh, float v0, float v1, float v2, float v3) { ((PFNGLUNIFORM4FPROC)glfp[glUniform4f])(sh,v0,v1,v2,v3); }
static void kglUniform1i (unsigned sh, int v0)                                 { ((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(sh,v0);          }
static void kglUniform2i (unsigned sh, int v0, int v1)                         { ((PFNGLUNIFORM2IPROC)glfp[glUniform2i])(sh,v0,v1);       }
static void kglUniform3i (unsigned sh, int v0, int v1, int v2)                 { ((PFNGLUNIFORM3IPROC)glfp[glUniform3i])(sh,v0,v1,v2);    }
static void kglUniform4i (unsigned sh, int v0, int v1, int v2, int v3)         { ((PFNGLUNIFORM4IPROC)glfp[glUniform4i])(sh,v0,v1,v2,v3); }

static void gsetshadermodefor2d (void)
{
	if (!gshadermode) return;
	gshadermode = 0;

	if (oct_useglsl)
	{
		((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(0);
	}
	else
	{
		glDisable(GL_VERTEX_PROGRAM_ARB);
		glDisable(GL_FRAGMENT_PROGRAM_ARB);
	}
	kglActiveTexture(0);

	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,xres,yres,0,-1,1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
}

//--------------------------------------------------------------------------------------------------
//DRAWCONE_GL.C:

#define DRAWCONE_NOCAP0 1
#define DRAWCONE_NOCAP1 2
#define DRAWCONE_FLAT0 4
#define DRAWCONE_FLAT1 8
#define DRAWCONE_CENT 16
#define DRAWCONE_NOPHONG 32

	//DRAWCONE.H:
extern void drawcone_setup (int, int, tiletype *, int,  point3d *,  point3d *,  point3d *,  point3d *, double, double, double);
extern void drawcone_setup (int, int, tiletype *, int, dpoint3d *, dpoint3d *, dpoint3d *, dpoint3d *, double, double, double);
extern void drawsph (double, double, double, double, int, double);
extern void drawcone (double, double, double, double, double, double, double, double, int, double, int);

static void getperpvec (double nx, double ny, double nz, double *ax, double *ay, double *az, double *bx, double *by, double *bz)
{
	double f;

	if (fabs(nx) < fabs(ny))
		  { f = 1.0/sqrt(ny*ny+nz*nz); (*ax) =  0.0; (*ay) = nz*f; (*az) =-ny*f; }
	else { f = 1.0/sqrt(nx*nx+nz*nz); (*ax) =-nz*f; (*ay) =  0.0; (*az) = nx*f; }
	(*bx) = ny*(*az) - nz*(*ay);
	(*by) = nz*(*ax) - nx*(*az);
	(*bz) = nx*(*ay) - ny*(*ax);
	f = 1.0/sqrt((*bx)*(*bx) + (*by)*(*by) + (*bz)*(*bz)); (*bx) *= f; (*by) *= f; (*bz) *= f;
}

typedef struct { dpoint3d p, r, d, f, h; } drawcone_t;
static drawcone_t drawconedat;
static void oct_drawcone_setup (int cputype, int lnumcpu, tiletype *dd, INT_PTR zbufoff, dpoint3d *ipos, dpoint3d *irig, dpoint3d *idow, dpoint3d *ifor, double hx, double hy, double hz)
{
	if (!oct_usegpu) { drawcone_setup(cputype,lnumcpu,dd,zbufoff,ipos,irig,idow,ifor,hx,hy,hz); return; }
	drawconedat.p = (*ipos); drawconedat.r = (*irig); drawconedat.d = (*idow); drawconedat.f = (*ifor);
	drawconedat.h.x = hx; drawconedat.h.y = hy; drawconedat.h.z = hz;
}
static void oct_drawcone_setup (int cputype, int lnumcpu, tiletype *dd, INT_PTR zbufoff, point3d *ipos, point3d *irig, point3d *idow, point3d *ifor, double hx, double hy, double hz)
{
	dpoint3d dpos, drig, ddow, dfor;
	if (!oct_usegpu) { drawcone_setup(cputype,lnumcpu,dd,zbufoff,ipos,irig,idow,ifor,hx,hy,hz); return; }
	dpos.x = ipos->x; dpos.y = ipos->y; dpos.z = ipos->z;
	drig.x = irig->x; drig.y = irig->y; drig.z = irig->z;
	ddow.x = idow->x; ddow.y = idow->y; ddow.z = idow->z;
	dfor.x = ifor->x; dfor.y = ifor->y; dfor.z = ifor->z;
	drawcone_setup(cputype,lnumcpu,dd,zbufoff,&dpos,&drig,&ddow,&dfor,hx,hy,hz);
}

static char vshad_drawcone[] = //See DRAWCONE2.PSS
	"varying vec4 v, c;\n"
	"uniform float zoom;\n"
	"void main () { gl_Position = ftransform(); v = gl_Vertex*vec4(zoom,zoom,1.0,0.0); c = gl_Color*vec4(.8,.8,.8,1.0); }\n";
static char fshad_drawcone[] = //See DRAWCONE2.PSS
	"varying vec4 v, c;\n"
	"uniform vec4 fogcol;\n"
	"uniform vec3 p0, p1, dnm, nm, np0, np1, nnm, kw, kxw, ligdir, fp, fnm;\n"
	"uniform vec2 kyw, zscale;\n"
	"uniform float tanang, nr0, nr1, k0, k1, rr[2], kzz, k, Zc0, Zc1, fr2, fogdist;\n"
	"uniform int mode;\n"
	"void main ()\n"
	"{\n"
	"   vec4 vv, cc;\n"
	"   vec3 norm, tp, u, r, nhit;\n"
	"   float d, f, Za, Zb, Zc, insqr, fmin;\n"
	"\n"
	"      //raytrace cone..\n"
	"   Za = v.x*dot(v.xyz,kxw) + v.y*dot(v.yz,kyw) + v.z*v.z*kzz; Zb = dot(-v.xyz,kw); Zc = k;\n"
	"   insqr = Zb*Zb - 4.0*Za*Zc; f = (sqrt(insqr) - Zb)/(Za*2.0); d = (dot(-v.xyz,nnm)*f - k1)*k0;\n"
	"   if ((insqr >= 0.0) && (d > 0.0) && (d < 1.0))\n"
	"   {\n"
	"      fmin = f; norm = -v.xyz*f - (nnm*d + np0);\n"
	"      norm = normalize(length(norm)*nm*tanang + norm);\n"
	"   }\n"
	"   else\n"
	"   {\n"
	"      fmin = 1e32;\n"
	"\n"
	"      Za = dot(-v.xyz,v.xyz); d = 1.0/Za;\n"
	"\n"
	"         //raytrace sphere (p0=0,Zc0=-1 to disable)\n"
	"      Zb = dot(-v.xyz,p0); insqr = Zb*Zb - Za*Zc0;\n"
	"      f = (sqrt(insqr) - Zb)*d; nhit = -v.xyz*f;\n"
	"      if ((insqr >= 0.0) && (f < fmin) && (dot(nhit-np1,dnm) <= 0.0))\n"
	"         { fmin = f; norm = (nhit-p0)*rr[0]; }\n"
	"\n"
	"         //raytrace sphere (p1=0,Zc1=-1 to disable)\n"
	"      Zb = dot(-v.xyz,p1); insqr = Zb*Zb - Za*Zc1;\n"
	"      f = (sqrt(insqr) - Zb)*d; nhit = -v.xyz*f;\n"
	"      if ((insqr >= 0.0) && (f < fmin) && (dot(nhit-np0,dnm) >= 0.0))\n"
	"         { fmin = f; norm = (nhit-p1)*rr[1]; }\n"
	"\n"
	"         //raytrace plane (fr2=0 to disable)\n"
	"      f = dot(-fp,fnm)/dot(v.xyz,fnm); tp = -v.xyz*f-fp;\n"
	"      if (dot(tp,tp) < fr2) { fmin = f; norm = fnm; }\n"
	"\n"
	"      if (fmin >= 1e32) discard;\n"
	"   }\n"
	"//----------------------------------------\n"
	"      //given hit&norm, apply lighting\n"
	"   f = dot(norm,ligdir)*-.5+.5;\n"
	"   u = normalize(-v.xyz); //u = normalized view direction\n"
	"   d = max(dot(dot(norm,ligdir)*norm*2.0-ligdir,u),0.0); //Standard reflection\n"
	"   d *= d; d *= d; d *= d; f += d;\n"
	"\n"
	"   //hit = -v.xyz*fmin; //(FYI)\n"
	"   cc = c*vec4(f,f,f,1); vv = v; vv.z *= -fmin;\n"
	"   cc = mix(cc,fogcol,min(vv.z*fogdist,1.0));\n"
	"   gl_FragDepth = zscale.x/vv.z+zscale.y;\n"
	"   gl_FragColor = cc;\n"
	"}\n";

static char vshadasm_drawcone[] = //See DRAWCONE2_ASM.PSS
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MUL result.color, vertex.color, {+0.1,+0.1,+0.1,1.0};\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"MOV result.texcoord[1], vertex.position;\n"
	"END\n";
static char fshadasm_drawcone[] = //See DRAWCONE2_ASM.PSS
	"!!ARBfp1.0\n"
	"ATTRIB v = fragment.texcoord[1];\n"
	"ATTRIB c = fragment.color;\n"
	"ATTRIB t = fragment.texcoord[0];\n"
	"PARAM ligmode = program.local[ 0]; #ligdir.x,ligdir.y,ligdir.z,mode\n"
	"PARAM p0      = program.local[ 1]; #px0,py0,pz0,Zc0\n"
	"PARAM p1      = program.local[ 2]; #px1,py1,pz1,Zc1\n"
	"PARAM n0      = program.local[ 3]; #nx0,ny0,nz0,nr0\n"
	"PARAM n1      = program.local[ 4]; #nx1,ny1,nz1,nr1\n"
	"PARAM dnm     = program.local[ 5]; #nmx*?,nmy*?,nmz*?,0\n"
	"PARAM nm      = program.local[ 6]; #nmx,nmy,nmz,sinang/cosang\n"
	"PARAM nnm     = program.local[ 7]; #nnx,nny,nnz,0\n"
	"PARAM k       = program.local[ 8]; #k  ,k0 ,k1 ,kzz\n"
	"PARAM kxw     = program.local[ 9]; #kxx,kxy,kxz,(-gznear*gzfar/(gzfar-gznear))\n"
	"PARAM kyw     = program.local[10]; #kyy,kyz,_0_,gzfar/(gzfar-gznear)\n"
	"PARAM kw      = program.local[11]; #kx ,ky ,kz ,0\n"
	"PARAM pr      = program.local[12]; #1/pr0,1/pr1,0,0\n"
	"PARAM fnm     = program.local[13]; #?nmx,?nmy,?nmz,0\n"
	"PARAM fp      = program.local[14]; #nx?,ny?,nz?,nr?*isflat?\n"
	"PARAM fogcol  = program.local[15]; #fogcol.r,fogcol.g,fogcol.b,fogcol.a\n"
	"PARAM fogdist = program.local[16]; #1/oct_fogdist,0,0,0\n"
	"\n"
	"TEMP norm, f, Za, Zb, insqr, fmin, ofmin, t0, t1, t2, nhit, nnorm;\n"
	"\n"
	"   #ARB ASM min guaranteed limits:\n"
	"   #INSTS     95/72 !!! :/\n"
	"   #ATTRIBS    3/10 :)\n"
	"   #PARAMS    15/24 :)\n"
	"   #TEMPS     12/16 :)\n"
	"   #ALU_INSTS 95/48 !!! :/\n"
	"   #TEX_INSTS  0/24 :)\n"
	"   #TEX_IND    0/ 4 :)\n"
	"\n"
	"DP3 t0.x, v, kxw;         #Za = v.x*dot(v.xyz,kxw) + v.y*dot(v.yz,kyw) + v.z*v.z*kzz;\n"
	"DP3 t0.y, v.yzyz, kyw;\n"
	"MUL t0.z, v.z, k.w;\n"
	"DP3 Za, t0, v;\n"
	"DP3 Zb, -v, kw;           #Zb = dot(-v.xyz,kw);\n"
	"MUL t0, Zb, Zb;           #insqr = Zb*Zb - 4.0*Za*Zc;\n"
	"MUL t1, Za, k.x;\n"
	"MAD insqr, t1, {-4.0}, t0;\n"
	"\n"
	"RSQ t0, insqr.x;          #f = (sqrt(insqr) - Zb)/(Za*2.0);\n"
	"MAD t0, t0, insqr, -Zb;\n"
	"ADD t1, Za, Za;\n"
	"RCP t1, t1.x;\n"
	"MUL f, t0.x, t1;\n"
	"\n"
	"DP3 t0, -v, nnm;          #t2 = (dot(-v.xyz,nnm)*fm - k1)*k0;\n"
	"MAD t0, t0, f, -k.z;\n"
	"MUL t2, t0.x, k.y;\n"
	"\n"
	"MAD t0, nnm, t2, n0;      #norm = -v.xyz*f - (nnm*t2 + np0);\n"
	"MAD norm, -v, f, -t0;\n"
	"DP3 t0, norm, norm;       #norm = normalize(length(norm)*nm*tanang + norm);\n"
	"RSQ t1, t0.x;\n"
	"MUL t1, t1, t0;\n"
	"MUL t0, t1, nm;\n"
	"MAD t0, t0, nm.w, norm;\n"
	"DP3 t1, t0, t0;\n"
	"RSQ t1, t1.x;\n"
	"MUL norm, t0, t1;\n"
	"\n"
	"SLT t0, t2, {1.0};        #t0 = ((t2 >= 0.0) && (t2 < 1.0) && (insqr >= 0.0));\n"
	"CMP t0, t2, {0.0}, t0;\n"
	"CMP t0, insqr.x, {0.0}, t0;\n"
	"CMP fmin, -t0.x, f, {1e32}; #if (t0) { fmin = f; }\n"
	"\n"
	"MOV ofmin, fmin;\n"
	"\n"
	"DP3 Za, v, -v;            #Za = -dot(v.xyz,v.xyz);\n"
	"RCP t2, Za.x;\n"
	"#------------------\n"
	"   #Raytrace sphere 0\n"
	"DP3 Zb, -v, p0;           #Zb = dot(-v.xyz,p0);\n"
	"MUL t0, Zb, Zb;           #insqr = Zb*Zb - Za*Zc0;\n"
	"MAD insqr, Za, -p0.w, t0;\n"
	"RSQ t0, insqr.x;          #f = (sqrt(insqr) - Zb)/Za;\n"
	"MAD t0, t0, insqr.x, -Zb;\n"
	"MUL f, t0.x, t2;\n"
	"MUL nhit, -v, f;          #nhit = -v.xyz*f;\n"
	"SUB nnorm, nhit, p0;      #nnorm = (nhit-p0)*rr[0];\n"
	"MUL nnorm, nnorm, pr.x;\n"
	"SGE t0, ofmin, {1e32};    #t0 = ((ofmin == 1e32) && (f < fmin) && (insqr > 0.0) && (dot(nhit-np1,dnm) <= 0.0);\n"
	"SUB t1, nhit, n1;         #t1 = dot(nhit-np1,dnm);\n"
	"DP3 t1, t1, dnm;\n"
	"CMP t0, -t1.x, {0.0}, t0;\n"
	"#SLT t1, f, fmin;\n"
	"CMP t0, insqr, {0.0}, t0;\n"
	"#MUL t0, t0, t1;\n"
	"CMP fmin, -t0.x, f, fmin; #if (t0) { fmin = f; norm = nnorm; }\n"
	"CMP norm, -t0.x, nnorm, norm;\n"
	"#------------------\n"
	"   #Raytrace sphere 1\n"
	"DP3 Zb, -v, p1;           #Zb = dot(-v.xyz,p1);\n"
	"MUL t0, Zb, Zb;           #insqr = Zb*Zb - Za*Zc1;\n"
	"MAD insqr, Za, -p1.w, t0;\n"
	"RSQ t0, insqr.x;          #f = (sqrt(insqr) - Zb)/Za;\n"
	"MAD t0, t0, insqr.x, -Zb;\n"
	"MUL f, t0.x, t2;\n"
	"MUL nhit, -v, f;          #nhit = -v.xyz*f;\n"
	"SUB nnorm, nhit, p1;      #nnorm = (nhit-p1)*rr[1];\n"
	"MUL nnorm, nnorm, pr.y;\n"
	"SGE t0, ofmin, {1e32};    #t0 = ((ofmin == 1e32) && (f < fmin) && (insqr > 0.0) && (dot(nhit-np0,dnm) >= 0.0);\n"
	"SUB t1, nhit, n0;         #t1 = dot(nhit-np0,dnm);\n"
	"DP3 t1, -t1, dnm;\n"
	"CMP t0, -t1.x, {0.0}, t0;\n"
	"SLT t1, f, fmin;\n"
	"CMP t0, insqr, {0.0}, t0;\n"
	"MUL t0, t0, t1;\n"
	"CMP fmin, -t0.x, f, fmin; #if (t0) { fmin = f; norm = nnorm; }\n"
	"CMP norm, -t0.x, nnorm, norm;\n"
	"#------------------\n"
	"   #Raytrace plane 0 or 1 (fr=0 to disable)\n"
	"DP3 t1, fp, fnm;          #f = -dot(fp,fnm)/dot(v.xyz,fnm);\n"
	"DP3 t0, v, fnm;\n"
	"RCP t2, t0.x;\n"
	"MUL f, t1, -t2;\n"
	"MAD t2, -v, f, -fp;       #t2 = -v.xyz*f-fp;\n"
	"DP3 t1, t2, t2;           #if (dot(t2,t2) < fr*fr)\n"
	"SLT t0, t1, fp.w;\n"
	"CMP fmin, -t0.x, f, fmin; #   { fmin = f; norm = fnm; }\n"
	"CMP norm, -t0.x, fnm, norm;\n"
	"#------------------\n"
	"\n"
	//"#SGE t0, fmin, {1e32};     #if ((fmin <= 0.0) || (fmin >= 1e32)) discard;\n"
	//"#CMP t0, fmin, t0, {0.0};\n"
	//"#KIL -t0.x;\n"
	"\n"
	"DP3 f, norm, ligmode;     #f = dot(norm,ligdir)*-.5+.5;\n"
	"MAD f, f, {-.5}, {+.5};\n"
	"\n"
	"   #Standard reflection\n"
	"DP3 t1, -v, v;            #t1 = normalize(-v.xyz); //t1 = normalized view direction\n"
	"RSQ t0, t1.x;\n"
	"MUL t1, -v, t0;\n"
	"DP3 t0, norm, ligmode;    #t2 = max(dot(dot(norm,ligdir)*norm*2.0-ligdir,t1),0.0);\n"
	"MUL t0, t0, norm;\n"
	"MAD t0, t0, {2.0,2.0,2.0}, -ligmode;\n"
	"DP3_SAT t2, t0, t1;\n"
	"MUL t2, t2, t2;           #t2 *= t2; t2 *= t2; t2 *= t2; f += t2;\n"
	"MUL t2, t2, t2;\n"
	"CMP t2, -ligmode.w, {0.0}, t2; #if (mode) t2 = 0.0; //disable phong\n"
	"MAD f, t2, t2, f;\n"
	"\n"
	"MUL t1, -v, fmin.x;       #hit = -v*f;\n"
	"RCP t0, t1.z;             #gl_FragDepth = zmul/hit.z + zadd;\n"
	"MAD result.depth, t0, kxw.w, kyw.w;\n"
	"\n"
	"MUL f, f.x, c;            #gl_FragColor = vec4(.8,.8,.8,1.0)*c*f;\n"
	"MOV f.w, c.w;\n"
	"MUL f, f, {8.0,8.0,8.0,1.0};\n"
	"\n"
	"MUL_SAT t1, t1.z, fogdist.x; #f = mix(f,fogcol,min(v.z*fogdist,1.0))\n"
	"LRP result.color, t1, fogcol, f;\n"
	"\n"
	"END\n";

	//See:polydraw\ken\drawcone.kc:drawcone3() for derivation
void oct_drawcone (double px0, double py0, double pz0, double pr0, double px1, double py1, double pz1, double pr1, int col, double shadefac, int flags)
{
	#define SCISDIST 5e-4
	#define PMAX 64
	double px[PMAX], py[PMAX], pz[PMAX], px2[PMAX], py2[PMAX];
	double ix[4], iy[4], iz[4];
	double kxx, kyy, kzz, kxy, kxz, kyz, kx, ky, kz, k, k0, k1, nmx, nmy, nmz, nnx, nny, nnz;
	double nx0, ny0, nz0, nr0, nx1, ny1, nz1, nr1, cosang2, cosang, sinang;
	double f, g, a, b, c, s, da, dr, d, d2, a0, a1, ax, ay, az, bx, by, bz, cx, cy, cz, dx, dy, dz, zdep;
	int i, j, m, n, pn, pn2, isflat0, isflat1;

	if (!oct_usegpu) { drawcone(px0,py0,pz0,pr0,px1,py1,pz1,pr1,col,shadefac,flags); return; }
	cx = px0-drawconedat.p.x; cy = py0-drawconedat.p.y; cz = pz0-drawconedat.p.z;
	px0 = cx*drawconedat.r.x + cy*drawconedat.r.y + cz*drawconedat.r.z;
	py0 = cx*drawconedat.d.x + cy*drawconedat.d.y + cz*drawconedat.d.z;
	pz0 = cx*drawconedat.f.x + cy*drawconedat.f.y + cz*drawconedat.f.z;
	cx = px1-drawconedat.p.x; cy = py1-drawconedat.p.y; cz = pz1-drawconedat.p.z;
	px1 = cx*drawconedat.r.x + cy*drawconedat.r.y + cz*drawconedat.r.z;
	py1 = cx*drawconedat.d.x + cy*drawconedat.d.y + cz*drawconedat.d.z;
	pz1 = cx*drawconedat.f.x + cy*drawconedat.f.y + cz*drawconedat.f.z;

	px0 = -px0; px1 = -px1; //WTF?
	isflat0 = (pr0 < 0.0); pr0 = max(fabs(pr0),1e-6);
	isflat1 = (pr1 < 0.0); pr1 = max(fabs(pr1),1e-6);
	if (flags&DRAWCONE_FLAT0) isflat0 = 1;
	if (flags&DRAWCONE_FLAT1) isflat1 = 1;
	if (flags&DRAWCONE_CENT)
	{
		dx = px1-px0; dy = py1-py0; dz = pz1-pz0; dr = pr1-pr0;
		f = dr/(dx*dx + dy*dy + dz*dz); g = sqrt(f*dr + 1.0);
		d = pr0*f; px0 += dx*d; py0 += dy*d; pz0 += dz*d; pr0 *= g;
		d = pr1*f; px1 += dx*d; py1 += dy*d; pz1 += dz*d; pr1 *= g;
	}

	if (shadcur != 1)
	{
		shadcur = 1;
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-gznear,gznear,-(float)yres/(float)xres*gznear,(float)yres/(float)xres*gznear,gznear,gzfar);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	}
	if (oct_useglsl)
	{
		((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
		kglUniform1f(kglGetUniformLoc("zoom"),drawconedat.h.x/drawconedat.h.z);
	}
	else
	{
		//kglProgramLocalParam(15?,drawconedat.h.x/drawconedat.h.z,0,0,0); //zoom
		glEnable(GL_VERTEX_PROGRAM_ARB);
		glEnable(GL_FRAGMENT_PROGRAM_ARB);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shadvert[shadcur]);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shadcur]);
	}

//----------------------------------------
		//raytrace cone (find hit&norm)
	cosang2 = max(1.0 - (pr1-pr0)*(pr1-pr0) / ((px1-px0)*(px1-px0) + (py1-py0)*(py1-py0) + (pz1-pz0)*(pz1-pz0)),0.0);
	cosang = sqrt(cosang2); sinang = sqrt(1.0-cosang2); if (pr0 < pr1) sinang = -sinang;

	nmx = px1-px0;
	nmy = py1-py0;
	nmz = pz1-pz0;
	f = nmx*nmx + nmy*nmy + nmz*nmz; if (f > 0.0) f = 1.0/sqrt(f);
	nmx *= f; nmy *= f; nmz *= f;
	nx0 = px0 + nmx*sinang*pr0; nx1 = px1 + nmx*sinang*pr1;
	ny0 = py0 + nmy*sinang*pr0; ny1 = py1 + nmy*sinang*pr1;
	nz0 = pz0 + nmz*sinang*pr0; nz1 = pz1 + nmz*sinang*pr1;
	nr0 =           cosang*pr0; nr1  =          cosang*pr1;

	nnx = nx1-nx0;
	nny = ny1-ny0;
	nnz = nz1-nz0;
	k0 = nnx*nnx + nny*nny + nnz*nnz;
	k1 = nx0*nnx + ny0*nny + nz0*nnz;
	if (k0 != 0.0) { k0 = 1.0/k0; } else { k0 = 1.0; k1 = 1.0; }

		//h = v*f
		//dot(h-c,n)^2 = dot(h-c,h-c)*cosang2
		//kcalc "((hx-cx)*nx+(hy-cy)*ny+(hz-cz)*nz)~2-((hx-cx)~2+(hy-cy)~2+(hz-cz)~2)*cosang2"

	f = (pr1-pr0)*(pr1-pr0) - ((px1-px0)*(px1-px0) + (py1-py0)*(py1-py0) + (pz1-pz0)*(pz1-pz0));
	kxx = (px1-px0)*(px1-px0) + f;
	kyy = (py1-py0)*(py1-py0) + f;
	kzz = (pz1-pz0)*(pz1-pz0) + f;
	kxy = (px1-px0)*(py1-py0)*2.0;
	kxz = (px1-px0)*(pz1-pz0)*2.0;
	kyz = (py1-py0)*(pz1-pz0)*2.0;

	f = ((px1-px0)*px1 + (py1-py0)*py1 + (pz1-pz0)*pz1 + (pr0-pr1)*pr1)*2.0;
	g = ((px1-px0)*px0 + (py1-py0)*py0 + (pz1-pz0)*pz0 + (pr0-pr1)*pr0)*2.0;
	kx = f*px0 - g*px1;
	ky = f*py0 - g*py1;
	kz = f*pz0 - g*pz1;

	k = + (px0*pr1 - px1*pr0)*(px0*pr1 - px1*pr0) - (px0*py1 - px1*py0)*(px0*py1 - px1*py0)
		 + (py0*pr1 - py1*pr0)*(py0*pr1 - py1*pr0) - (py0*pz1 - py1*pz0)*(py0*pz1 - py1*pz0)
		 + (pz0*pr1 - pz1*pr0)*(pz0*pr1 - pz1*pr0) - (pz0*px1 - pz1*px0)*(pz0*px1 - pz1*px0);

	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(((float)((col>>16)&255))/128.0,((float)((col>>8)&255))/128.0,((float)(col&255))/128.0,(float)((col>>24)&255)/255.0);
	if (oct_useglsl)
	{
		kglUniform3f(kglGetUniformLoc("ligdir"),-1.0/sqrt(3.0),1.0/sqrt(3.0),1.0/sqrt(3.0)); //FIX:use shadefac (glUniform,etc..)
		if (isflat0 == 0)
		{
			kglUniform3f(kglGetUniformLoc("p0" ),px0,py0,pz0);
			kglUniform1f(kglGetUniformLoc("Zc0"),pr0*pr0 - (px0*px0 + py0*py0 + pz0*pz0));
		} else { kglUniform3f(kglGetUniformLoc("p0"),0,0,0); kglUniform1f(kglGetUniformLoc("Zc0"),-1); }
		if (isflat1 == 0)
		{
			kglUniform3f(kglGetUniformLoc("p1" ),px1,py1,pz1);
			kglUniform1f(kglGetUniformLoc("Zc1"),pr1*pr1 - (px1*px1 + py1*py1 + pz1*pz1));
		} else { kglUniform3f(kglGetUniformLoc("p1"),0,0,0); kglUniform1f(kglGetUniformLoc("Zc1"),-1); }
		kglUniform3f(kglGetUniformLoc("np0"),nx0,ny0,nz0); kglUniform1f(kglGetUniformLoc("nr0"),nr0);
		kglUniform3f(kglGetUniformLoc("np1"),nx1,ny1,nz1); kglUniform1f(kglGetUniformLoc("nr1"),nr1);
		kglUniform3f(kglGetUniformLoc("dnm"),nmx,nmy,nmz);
		kglUniform3f(kglGetUniformLoc("nm" ),nmx,nmy,nmz);
		kglUniform3f(kglGetUniformLoc("nnm"),nnx,nny,nnz);
		kglUniform1f(kglGetUniformLoc("tanang"),sinang/cosang);
		kglUniform1f(kglGetUniformLoc("k0" ),k0);
		kglUniform1f(kglGetUniformLoc("k1" ),k1);
		kglUniform3f(kglGetUniformLoc("kxw"),kxx,kxy,kxz);
		kglUniform2f(kglGetUniformLoc("kyw"),kyy,kyz);
		kglUniform1f(kglGetUniformLoc("kzz"),kzz);
		kglUniform3f(kglGetUniformLoc("kw" ),kx,ky,kz);
		kglUniform1f(kglGetUniformLoc("k"  ),k);
		kglUniform1f(kglGetUniformLoc("rr")+0,1.0/pr0);
		kglUniform1f(kglGetUniformLoc("rr")+1,1.0/pr1);
		if (nx0*nmx + ny0*nmy + nz0*nmz > 0.0)
		{
			kglUniform3f(kglGetUniformLoc("fnm"),-nmx,-nmy,-nmz);
			kglUniform3f(kglGetUniformLoc("fp"),nx0,ny0,nz0);
			kglUniform1f(kglGetUniformLoc("fr2"),nr0*nr0*(float)isflat0);
		}
		else
		{
			kglUniform3f(kglGetUniformLoc("fnm"),+nmx,+nmy,+nmz);
			kglUniform3f(kglGetUniformLoc("fp"),nx1,ny1,nz1);
			kglUniform1f(kglGetUniformLoc("fr2"),nr1*nr1*(float)isflat1);
		}
		kglUniform2f(kglGetUniformLoc("zscale"),-gznear*gzfar/(gzfar-gznear),gzfar/(gzfar-gznear));
		kglUniform4f(kglGetUniformLoc("fogcol"),(float)((oct_fogcol>>16)&255)/255.0,(float)((oct_fogcol>>8)&255)/255.0,(float)(oct_fogcol&255)/255.0,(float)(((unsigned)oct_fogcol)>>24)/255.0);
		kglUniform1f(kglGetUniformLoc("fogdist"),1.0/oct_fogdist);
	}
	else
	{
		kglProgramLocalParam( 0,-1.0/sqrt(3.0),1.0/sqrt(3.0),1.0/sqrt(3.0),0.0); //ligmode
		if (isflat0 == 0) kglProgramLocalParam( 1,px0,py0,pz0,pr0*pr0 - (px0*px0 + py0*py0 + pz0*pz0)); //p0,Zc0
						 else kglProgramLocalParam( 1,0.0,0.0,0.0,-1.0);
		if (isflat1 == 0) kglProgramLocalParam( 2,px1,py1,pz1,pr1*pr1 - (px1*px1 + py1*py1 + pz1*pz1)); //p1,Zc1
						 else kglProgramLocalParam( 2,0.0,0.0,0.0,-1.0);
		kglProgramLocalParam( 3,nx0,ny0,nz0,nr0); //n0
		kglProgramLocalParam( 4,nx1,ny1,nz1,nr1); //n1
		kglProgramLocalParam( 5,nmx,nmy,nmz,0.0); //dnm
		kglProgramLocalParam( 6,nmx,nmy,nmz,sinang/cosang); //nm,tanang
		kglProgramLocalParam( 7,nnx,nny,nnz,0.0); //nnm
		kglProgramLocalParam( 8,k,k0,k1,kzz); //k
		kglProgramLocalParam( 9,kxx,kxy,kxz,-gznear*gzfar/(gzfar-gznear)); //kxw
		kglProgramLocalParam(10,kyy,kyz,0.0,gzfar/(gzfar-gznear)); //kyw
		kglProgramLocalParam(11,kx,ky,kz,0.0); //kw
		kglProgramLocalParam(12,1.0/pr0,1.0/pr1,0.0,0.0); //pr
		if (nx0*nmx + ny0*nmy + nz0*nmz > 0.0)
		{
			kglProgramLocalParam(13,-nmx,-nmy,-nmz,0.0); //fnm
			kglProgramLocalParam(14,nx0,ny0,nz0,nr0*nr0*(float)isflat0); //fp
		}
		else
		{
			kglProgramLocalParam(13,+nmx,+nmy,+nmz,0.0); //fnm
			kglProgramLocalParam(14,nx1,ny1,nz1,nr1*nr1*(float)isflat1); //fp
		}
		kglProgramLocalParam(15,(float)((oct_fogcol>>16)&255)/255.0,(float)((oct_fogcol>>8)&255)/255.0,(float)(oct_fogcol&255)/255.0,(float)(((unsigned)oct_fogcol)>>24)/255.0); //fogcol
		kglProgramLocalParam(16,1.0/oct_fogdist,0.0,0.0,0.0); //fogdist
	}
//----------------------------------------
		//Pick front-most depth to ensure it passes to fragment shader
	zdep = -max(min(pz0-pr0,pz1-pr1),gznear+1e-6);

	pn = 0;

	dx = px1-px0; dy = py1-py0; dz = pz1-pz0; dr = pr1-pr0;
	if (dx*dx + dy*dy + dz*dz <= dr*dr) //gulped/can render single sphere
	{
		if ((isflat1 < isflat0) && (dr < 0.0)) return;
		if ((isflat1 > isflat0) && (dr > 0.0)) return;
		if (oct_useglsl) kglUniform3f(kglGetUniformLoc("dnm"),0.0,0.0,0.0);
						else kglProgramLocalParam( 5,0,0,0,0); //dnm
		if (pr1 > pr0) { px0 = px1; py0 = py1; pz0 = pz1; pr0 = pr1; }
		f = 1.0/pr0; px0 *= f; py0 *= f; pz0 *= f;
singsph:;
		d2 = px0*px0 + py0*py0 + pz0*pz0; f = 1.0-1.0/d2; cx = px0*f; cy = py0*f; cz = pz0*f;

		getperpvec(cx,cy,cz,&ax,&ay,&az,&bx,&by,&bz);
		n = min(max((int)((PI*2.0)*3.0/sqrt(sqrt(d2))+.5),3),PMAX);
		da = (PI*2.0)/n; f = sqrt(f); f *= 1.0/cos(da/2.0)/*circumscribe circle*/;
		for(i=n;i>0;i--)
		{
			a = (i-.5)*da; c = cos(a)*f; s = sin(a)*f;
			px[pn] = cx+ax*c+bx*s;
			py[pn] = cy+ay*c+by*s;
			pz[pn] = cz+az*c+bz*s; pn++;
		}
		goto skipcone;
	}

		//Transform cone to cylinder by normalizing endpoint radii
	f = 1.0/pr0; px0 *= f; py0 *= f; pz0 *= f;
	f = 1.0/pr1; px1 *= f; py1 *= f; pz1 *= f;

	dx = px1-px0; dy = py1-py0; dz = pz1-pz0;
		//ix = dx*t + px0
		//iy = dy*t + py0
		//iz = dz*t + pz0
		//dx*ix + dy*iy + dz*iz = 0
	d2 = dx*px0 + dy*py0 + dz*pz0;
	d2 = (px0*px0 + py0*py0 + pz0*pz0) - d2*d2/(dx*dx + dy*dy + dz*dz);
	if (d2 <= 1.0)
	{
		if (pz0 > pz1) { px0 = px1; py0 = py1; pz0 = pz1; }
		goto singsph;
	}
	a = 1.0/sqrt(d2); b = sqrt(1.0-a*a);

	bx = dy*pz0 - dz*py0;
	by = dz*px0 - dx*pz0;
	bz = dx*py0 - dy*px0;
	f = 1.0/sqrt(bx*bx + by*by + bz*bz); bx *= f; by *= f; bz *= f;
	ax = dy*bz - dz*by;
	ay = dz*bx - dx*bz;
	az = dx*by - dy*bx;
	f = 1.0/sqrt(ax*ax + ay*ay + az*az); ax *= f; ay *= f; az *= f;

	ix[0] = px0+ax*a-bx*b; iy[0] = py0+ay*a-by*b; iz[0] = pz0+az*a-bz*b;
	ix[1] = px0+ax*a+bx*b; iy[1] = py0+ay*a+by*b; iz[1] = pz0+az*a+bz*b;
	ix[2] = px1+ax*a+bx*b; iy[2] = py1+ay*a+by*b; iz[2] = pz1+az*a+bz*b;
	ix[3] = px1+ax*a-bx*b; iy[3] = py1+ay*a-by*b; iz[3] = pz1+az*a-bz*b;

	for(m=0;m<4;m+=2)
	{
		if (!m) { d2 = px0*px0 + py0*py0 + pz0*pz0; f = 1.0-1.0/d2; cx = px0*f; cy = py0*f; cz = pz0*f; }
			else { d2 = px1*px1 + py1*py1 + pz1*pz1; f = 1.0-1.0/d2; cx = px1*f; cy = py1*f; cz = pz1*f; }
		getperpvec(cx,cy,cz,&ax,&ay,&az,&bx,&by,&bz);
		a0 = atan2((ix[m+0]-cx)*bx + (iy[m+0]-cy)*by + (iz[m+0]-cz)*bz,
					  (ix[m+0]-cx)*ax + (iy[m+0]-cy)*ay + (iz[m+0]-cz)*az);
		a1 = atan2((ix[m+1]-cx)*bx + (iy[m+1]-cy)*by + (iz[m+1]-cz)*bz,
					  (ix[m+1]-cx)*ax + (iy[m+1]-cy)*ay + (iz[m+1]-cz)*az);
		if (a0 > a1) a1 += PI*2;
		n = min(max((int)((a1-a0)*3.0/sqrt(sqrt(d2))+.5),2),PMAX>>1);
		if (a1-a0 > PI) n = max(n,3); //prevent degenerate large triangles
		da = (a1-a0)/n; f = sqrt(f); f *= 1.0/cos(da/2.0)/*circumscribe circle*/;
		for(i=n;i>0;i--)
		{
			a = (i-.5)*da+a0; c = cos(a)*f; s = sin(a)*f;
			px[pn] = cx+ax*c+bx*s;
			py[pn] = cy+ay*c+by*s;
			pz[pn] = cz+az*c+bz*s; pn++;
		}
	}
skipcone:;

	pn2 = 0;
	for(i=pn-1,j=0;j<pn;i=j,j++)
	{
		if ((pz[i] >= SCISDIST) != (pz[j] >= SCISDIST))
		{
			g = (SCISDIST-pz[j])/(pz[i]-pz[j]);
			f = drawconedat.h.z / ((pz[i]-pz[j])*g + pz[j]);
			px2[pn2] = ((px[i]-px[j])*g + px[j])*f + drawconedat.h.x;
			py2[pn2] = ((py[i]-py[j])*g + py[j])*f + drawconedat.h.y;
			pn2++;
		}
		if (pz[j] >= SCISDIST)
		{
			f = drawconedat.h.z/pz[j];
			px2[pn2] = px[j]*f + drawconedat.h.x;
			py2[pn2] = py[j]*f + drawconedat.h.y;
			pn2++;
		}
	}

	f = zdep/drawconedat.h.x;
	glBegin(GL_TRIANGLE_FAN);
	for(i=0;i<pn2;i++) glVertex3f((px2[i]-drawconedat.h.x)*f,(py2[i]-drawconedat.h.y)*f,zdep);
	glEnd();

	shadcur = 0;
	if (oct_useglsl) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
}
void oct_drawsph (double x, double y, double z, double rad, int col, double shadfac)
{
	if (!oct_usegpu) { drawsph(x,y,z,rad,col,shadfac); return; }
	oct_drawcone(x,y,z,rad,x,y,z,rad,col,shadfac,0);
}
//--------------------------------------------------------------------------------------------------

static char vshad_drawsky[] =
	"varying vec4 t;\n"
	"void main () { gl_Position = ftransform(); t = gl_MultiTexCoord0; }\n";

static char fshad_drawsky[] =
	"varying vec4 t;\n"
	"uniform samplerCube tex0;\n"
	"uniform vec4 fogcol;\n"
	"void main () { gl_FragColor = textureCube(tex0,t.xyz)*fogcol; }\n";

static char vshadasm_drawsky[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"END\n";
static char fshadasm_drawsky[] =
	"!!ARBfp1.0\n"
	"TEMP t;\n"
	"TEX t, fragment.texcoord[0], texture[0], CUBE;\n"
	"MUL result.color, t, program.local[0];\n"
	"END\n";

static void sky_draw_mt (int sy, void *_)
{
	INT_PTR wptr, rptr, rptr2, picf;
	float f, fx0, fy0, fz0, fx, fy, fz, fx2, fy2, fz2, fxi, fyi, fzi, fhpic;
	int i, j, sx, sx0, sx1, sxe2, sxe, iu, iv, iu2, iv2, iui, ivi, yoff, picsizm1, icross[6], icrossn, fac, hpic, col, nfogcol;
	#define LINTERPSIZ 4

	tiles_t *skypic = (tiles_t *)gskyid;

	hpic = (skypic->x<<(16-1)); fhpic = (float)hpic;
	picsizm1 = ((skypic->x-DRAWSKY_CPU_FILT)<<16)-1;

	wptr = gdd.p*sy + gdd.f;
	nfogcol = ((oct_fogcol&0xfefefe)>>1);

	fx0 = (0-ghx)*girig.x + (sy-ghy)*gidow.x + ghz*gifor.x;
	fy0 = (0-ghx)*girig.y + (sy-ghy)*gidow.y + ghz*gifor.y;
	fz0 = (0-ghx)*girig.z + (sy-ghy)*gidow.z + ghz*gifor.z;

	icrossn = 0;
	f =  (fx0-fy0) / (girig.y - girig.x); if (fabs(fz0+girig.z*f) < fabs(fx0+girig.x*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	f =  (fx0-fz0) / (girig.z - girig.x); if (fabs(fy0+girig.y*f) < fabs(fx0+girig.x*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	f =  (fy0-fz0) / (girig.z - girig.y); if (fabs(fx0+girig.x*f) < fabs(fy0+girig.y*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	f = -(fx0+fy0) / (girig.y + girig.x); if (fabs(fz0+girig.z*f) < fabs(fx0+girig.x*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	f = -(fx0+fz0) / (girig.z + girig.x); if (fabs(fy0+girig.y*f) < fabs(fx0+girig.x*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	f = -(fy0+fz0) / (girig.z + girig.y); if (fabs(fx0+girig.x*f) < fabs(fy0+girig.y*f)) { icross[icrossn] = cvttss2si(f)+1; if ((unsigned)icross[icrossn] < (unsigned)gdd.x) icrossn++; }
	for(i=1;i<icrossn;i++)
		for(j=0;j<i;j++)
			if (icross[i] < icross[j]) { sx = icross[i]; icross[i] = icross[j]; icross[j] = sx; }

	sx0 = 0; sx1 = gdd.x;

	while ((icross > 0) && (icross[icrossn-1] >= sx1)) icrossn--;
	sx = sx1-1;
	do
	{
		sxe = sx0;
		if ((icross > 0) && (icross[icrossn-1] > sx0)) { icrossn--; sxe = icross[icrossn]; }

		f = (float)(sx+sxe)*.5;
		fx = girig.x*f + fx0; fx2 = (sx-1)*girig.x + fx0;
		fy = girig.y*f + fy0; fy2 = (sx-1)*girig.y + fy0;
		fz = girig.z*f + fz0; fz2 = (sx-1)*girig.z + fz0;
		if (fabs(fz) > max(fabs(fx),fabs(fy))) fac = (*(int *)&fz<0)*2+0;
		else if (fabs(fy) > fabs(fx))          fac = (*(int *)&fy>0)+4  ;
		else                                   fac = (*(int *)&fx<0)*2+1;
		picf = skypic->x*fac*skypic->p + skypic->f;

		switch(fac)
		{
			case 0: fx = fx2; fy = fy2; fz = fz2; fxi = girig.x; fyi = girig.y; fzi = girig.z; break;
			case 1: fx =-fz2; fy = fy2; fz = fx2; fxi =-girig.z; fyi = girig.y; fzi = girig.x; break;
			case 2: fx = fx2; fy =-fy2; fz = fz2; fxi = girig.x; fyi =-girig.y; fzi = girig.z; break;
			case 3: fx =-fz2; fy =-fy2; fz = fx2; fxi =-girig.z; fyi =-girig.y; fzi = girig.x; break;
			case 4: fx =-fx2; fy =-fz2; fz = fy2; fxi =-girig.x; fyi =-girig.z; fzi = girig.y; break;
			case 5: fx = fx2; fy =-fz2; fz = fy2; fxi = girig.x; fyi =-girig.z; fzi = girig.y; break;
		}

		f = fhpic/fz;
		iu = cvttss2si(fx*f) + hpic; iu = min(max(iu,0),picsizm1);
		iv = cvttss2si(fy*f) + hpic; iv = min(max(iv,0),picsizm1);
		while (sx-sxe >= (1<<LINTERPSIZ))
		{
			fx -= fxi*(1<<LINTERPSIZ); fy -= fyi*(1<<LINTERPSIZ); fz -= fzi*(1<<LINTERPSIZ);
			f = fhpic/fz;
			iu2 = cvttss2si(fx*f) + hpic; iu2 = min(max(iu2,0),picsizm1); iui = ((iu2-iu)>>LINTERPSIZ);
			iv2 = cvttss2si(fy*f) + hpic; iv2 = min(max(iv2,0),picsizm1); ivi = ((iv2-iv)>>LINTERPSIZ);
			for(sxe2=max(sx-(1<<LINTERPSIZ),sxe);sx>sxe2;sx--,iu+=iui,iv+=ivi)
			{
				rptr2 = (iv>>16)*skypic->p + (iu>>16)*4 + picf;
#if (DRAWSKY_CPU_FILT == 0)
				col = *(int *)rptr2; //nearest
#else
				col = bilinfilt((int *)rptr2,(int *)(rptr2+skypic->p),iu,iv); //bilinear
#endif
				*(int *)((sx<<2)+wptr) = mulcol(col,nfogcol);
			}
		}
		for(;sx>=sxe;sx--,fx-=fxi,fy-=fyi,fz-=fzi)
		{
			f = fhpic/fz;
			iu = cvttss2si(fx*f) + hpic; iu = min(max(iu,0),picsizm1);
			iv = cvttss2si(fy*f) + hpic; iv = min(max(iv,0),picsizm1);
			rptr2 = (iv>>16)*skypic->p + (iu>>16)*4 + picf;
#if (DRAWSKY_CPU_FILT == 0)
			col = *(int *)rptr2; //nearest
#else
			col = bilinfilt((int *)rptr2,(int *)(rptr2+skypic->p),iu,iv); //bilinear
#endif
			*(int *)((sx<<2)+wptr) = mulcol(col,nfogcol);
		}
	} while (sx >= sx0);
}

static void drawsky (void)
{
	if (oct_usegpu)
	{
		float f, g;
		int i;

		shadcur = 2;
		if (oct_useglsl)
		{
			((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);

			kglUniform4f(kglGetUniformLoc("fogcol"),(float)((oct_fogcol>>16)&255)/128.0,(float)((oct_fogcol>>8)&255)/128.0,(float)(oct_fogcol&255)/128.0,(float)(((unsigned)oct_fogcol)>>24)/128.0);
		}
		else
		{
			glEnable(GL_VERTEX_PROGRAM_ARB);
			glEnable(GL_FRAGMENT_PROGRAM_ARB);
			((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shadvert[shadcur]);
			((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shadcur]);

			kglProgramLocalParam(0,(float)((oct_fogcol>>16)&255)/128.0,(float)((oct_fogcol>>8)&255)/128.0,(float)(oct_fogcol&255)/128.0,(float)(((unsigned)oct_fogcol)>>24)/128.0); //fogcol
		}

		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-gznear,gznear,-(float)yres/(float)xres*gznear,(float)yres/(float)xres*gznear,gznear,gzfar);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
		glEnable(GL_DEPTH_TEST);

		kglActiveTexture(0);
		glBlendFunc(GL_ONE,GL_NONE);
		glBindTexture(GL_TEXTURE_CUBE_MAP,gskyid);

		g = ghz/ghx; f = gzfar*(220.999992/256.0)/*highest distance without falling behind Z .. but why 221/256?*/;
		glColor4ub(255,255,255,255);
		glBegin(GL_QUADS);
		glTexCoord3f(- girig.x - gidow.x + gifor.x*g, + girig.y + gidow.y - gifor.y*g, - girig.z - gidow.z + gifor.z*g); glVertex3f(-f,+f,-f);
		glTexCoord3f(+ girig.x - gidow.x + gifor.x*g, - girig.y + gidow.y - gifor.y*g, + girig.z - gidow.z + gifor.z*g); glVertex3f(+f,+f,-f);
		glTexCoord3f(+ girig.x + gidow.x + gifor.x*g, - girig.y - gidow.y - gifor.y*g, + girig.z + gidow.z + gifor.z*g); glVertex3f(+f,-f,-f);
		glTexCoord3f(- girig.x + gidow.x + gifor.x*g, + girig.y - gidow.y - gifor.y*g, - girig.z + gidow.z + gifor.z*g); glVertex3f(-f,-f,-f);
		glEnd();

		shadcur = 0;
		if (oct_useglsl) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
	}
	else
	{
		htrun(sky_draw_mt,0,0,yres,oct_numcpu);
	}
}

//--------------------------------------------------------------------------------------------------

#if (PIXMETH == 0)
//  0: 32bit: psurf:32                           :)    :)   :)   col_only

static char vshad_drawoct[] =
	"varying vec4 tv; void main () { gl_Position = ftransform(); tv = gl_MultiTexCoord0; }";
static char fshad_drawoct[] =
	"varying vec4 tv;\n"
	"uniform sampler2D tex0, tex1, tex2, tex3;\n"
	"uniform vec4 pos, vmulx, vmuly, vadd, mulcol, fogcol;\n"
	"uniform float rxsid, rysid, mipoffs, maxmip, mixval, gsideshade[6];\n"
	"uniform int axsidm1/*WTF:shader fails if renamed to xsidm1!*/, lxsidm1, usemix;\n"
	"uniform float depthmul, depthadd, fogdist;\n"
	"void main ()\n"
	"{\n"
	"   vec4 c, p;\n"
	"   ivec4 o1;\n"
	"   vec2 v2;\n"
	"   int i;\n"
	"\n"
	//"   o1 = ivec4(texture2D(tex1,tv.xy)*255.5); i = (o1.r + o1.g*256 + o1.b*65536 + o1.a*16777216); if (i == 0) discard;\n"
	"   i = packUnorm4x8(texture2D(tex1,tv.xy)); if (i == 0) discard;\n"
	"\n"
	"   v2 = vec2(float(i&axsidm1)*2.0*rxsid + 0.5*rxsid,float(i>>lxsidm1)*rysid + 0.5*rysid);\n"
	"   c = texture2D(tex2,v2                ).bgra; c.a = 1.0;\n"
	"   p = texture2D(tex2,v2+vec2(rxsid,0.0));\n"
	"   c *= dot(p.xyz-vec3(lessThan(vec4(.5),p)),vec3(1.0))+1.0;\n"
	"   gl_FragDepth = depthadd;\n"
	"   gl_FragColor = mix(c*mulcol,fogcol,min(fogdist,1.0));\n"
	//"   gl_FragColor = c*mulcol;\n" //NVidia shader compiler bug? :_
	"}\n";

static char vshadasm_drawoct[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.color, vertex.color;\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"END\n";
static char fshadasm_drawoct[] =
	"!!ARBfp1.0\n"
	"ATTRIB tv = fragment.texcoord[0];\n"
	"PARAM sidmul0 = program.local[0]; #{ 1/xsid, 2/xsid,0,0}\n"
	"PARAM sidmul1 = program.local[1]; #{ 2     , 1/ysid,0,0}\n"
	"PARAM sidadd1 = program.local[2]; #{.5/xsid,.5/ysid,0,0}\n"
	"PARAM depth   = program.local[3]; #{depthmul,depthadd,0,0}\n"
	"PARAM vmulx   = program.local[4]; #{vmulx.x,vmulx.y,vmulx.z,0}\n"
	"PARAM vmuly   = program.local[5]; #{vmuly.x,vmuly.y,vmuly.z,0}\n"
	"PARAM vadd    = program.local[6]; #{vadd.x ,vadd.y ,vadd.z ,0}\n"
	"PARAM pos     = program.local[7]; #{pos.x  ,pos.y  ,pos.z  ,0}\n"
	"PARAM mixnfog = program.local[8]; #{0,1/oct_fogdist,mixval}\n"
	"PARAM mulcol  = program.local[9]; #mulcol\n"
	"PARAM fogcol  = program.local[10]; #fogcol\n"
	"PARAM gsidsh0 = program.local[11]; #{gsideshade[0],gsideshade[1],gsideshade[2],0}\n"
	"PARAM gsidsh1 = program.local[12]; #{gsideshade[3],gsideshade[4],gsideshade[5],0}\n"
	"TEMP t, u, i, c, p, q;\n"
	"\n"
	"TEX u, tv, texture[1], 2D;\n"                //u = floor(texture2D(tex1,t.xy)*255.5);
	"MUL u, u, {255.5,255.5,255.5,255.5};\n"
	"FLR u, u;\n"
	"DP4 i, u, {1.0,256.0,65536.0,16777216.0};\n" //i = t.r + t.g*256.0 + t.b*65536.0 + t.a*16777216.0;
	"SGE t, -i, {0.0};\n"                         //if (i == 0.0) discard;
	"KIL -t.x;\n"
	"MUL t.y, i.x, sidmul0;\n"                    //t = vec2(frac(f*rxsid)*2.0 + 0.5*rxsid,floor(f*rxsid*2)*rysid + 0.5*rysid);
	"DP4 i, u, {1.0,256.0,0.0,0.0};\n"            //Trick required for 32-bit precision: limits i to 16-bit precision to avoid float truncation
	"MUL t.x, i.x, sidmul0.x;\n"
	"FLR t.y, t.y;\n"
	"MAD t, t, sidmul1, sidadd1;\n"
	"ADD q, t, sidmul0.xwww;\n"
	"TEX c, t, texture[2], 2D;\n"                 //c = texture2D(tex2,t).bgra;
	"TEX p, q, texture[2], 2D;\n"                 //p = texture2D(tex2,t+vec2(rxsid,0.0));
	"SLT t, {.5,.5,.5,0.0}, p;\n"                 //c *= dot(p.xyz-vec3(lessThan(vec4(.5),p)),vec3(1.0))+1.0;
	"SUB t, p, t;\n"
	"DPH t, t, {1.0,1.0,1.0,1.0};\n"
	"MUL c, t, c.bgra;\n"
	"MOV c.a, {1.0};\n"
	//"MOV result.depth, depth.y;\n"                //gl_FragDepth = depthadd;
	"MOV result.depth.z, {0.0,0.0,0.0,0.0};\n"    //gl_FragDepth = depthadd;
	"MUL result.color, c, mulcol;\n"              //gl_FragColor = c*mulcol;
	"END\n";

#elif (PIXMETH == 1)
//  1: 64bit: x:12, y:12, z:12, psurf:28         :)    :)?  :)   lsid<=12

static char vshad_drawoct[] =
	"varying vec2 tv;\n"
	"varying vec4 v;\n"
	"uniform vec4 vmulx, vmuly, vadd;\n"
	"void main ()\n"
	"{\n"
	"   gl_Position = ftransform();\n"
	"   tv = gl_MultiTexCoord0.xy-vec2(1.0/8192.0,0.0);\n"
	"   v = gl_MultiTexCoord0.x*vmulx + gl_MultiTexCoord0.y*vmuly + vadd;\n"
	"}\n";
static char fshad_drawoct[] =
	"varying vec2 tv;\n"
	"varying vec4 v;\n"
	"uniform sampler2D tex0, tex1, tex2, tex3;\n"
	"uniform vec4 pos, vmulx, vmuly, vadd, mulcol, fogcol;\n"
	"uniform float rxsid, rysid, mipoffs, maxmip, mixval, gsideshade[6];\n"
	"uniform int axsidm1, lxsidm1, usemix;\n"
	"uniform float depthmul, depthadd, fogdist;\n"
	"void main ()\n"
	"{\n"
	"   vec4 c, n, p, w;\n"
	"   ivec4 o0, o1, ip;\n"
	"   vec2 v2;\n"
	"   float f, g, h, gotf;\n"
	"   int i;\n"
	"\n"
	"   o0 = ivec4(texture2D(tex1,tv                     )*255.5);\n"
	"   o1 = ivec4(texture2D(tex1,tv+vec2(1.0/4096.0,0.0))*255.5);\n"
	"   i = (o1.r>>4) + (o1.g<<4) + (o1.b<<12) + (o1.a<<20); if (i == 0) discard;\n"
	"   ip.x = o0.r + ((o0.g&15)<<8);\n"
	"   ip.y = (o0.g>>4) + (o0.b<<4);\n"
	"   ip.z = o0.a + ((o1.r&15)<<8);\n"
	"\n"
	"   p = (vec4(lessThan(v,vec4(0.0)))+ip-pos)/v;\n"
	"   n = vec4(lessThan(max(p.yxxw,p.zzyw),p));\n"
	"   gotf = dot(p.xyz,n.xyz);\n"
	"   w = v*gotf+pos-vec4(ip); w.xy = mix(w.xy,w.zz,n.xy);\n"
	//"   if (max(abs(w.x-.5),abs(w.y-.5)) > 0.5) discard;\n"
	"\n"
	"   v2 = vec2(float(i&axsidm1)*2.0*rxsid + 0.5*rxsid,float(i>>lxsidm1)*rysid + 0.5*rysid);\n"
	"   c = texture2D(tex2,v2                ).bgra; c.a = 1.0;\n"
	"   p = texture2D(tex2,v2+vec2(rxsid,0.0));\n"
	"   g = p.w*255.0; c *= dot(p.xyz-vec3(lessThan(vec4(.5),p)),vec3(1.0))+1.0;\n"
#if 1
	"#ifdef GL_ARB_shader_texture_lod\n"
	"   if (usemix != 0)\n"
	"   c = mix(c,texture2DLod(tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0),min(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,maxmip)),mixval);\n"
	"   else\n"
	"   c *=      texture2DLod(tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0),min(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,maxmip));\n "
	"#else\n"
	"   c = mix(c,texture2D   (tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0)),mixval);\n"
	"#endif\n"
#else
			 //Test code for (oct_usefilter == 3); not needed for newer GLSL..
	"   float lod = clamp(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,0.0,maxmip);\n"
	"   vec2 tx = vec2(clamp(w.x,0.0,1.0)/64.0 +  mod(g,16.0)/32.0 + 65.0/128.0,\n"
	"                  clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 +  1.0/ 64.0) * pow(0.5,floor(lod));\n"
	"   w = mix(texture2D(tex0,tx),texture2D(tex0,tx*.5),mod(lod,1.0));\n"
	"   if (usemix != 0) c = mix(c,w,mixval); else c *= w;\n"
#endif
	"   c *= mulcol;\n"
	"   c.rgb -= vec3(gsideshade[int(dot(vec3(lessThan(v.xyz,vec3(0.0)))+vec3(0.0,2.0,4.0),n.xyz))]);\n"
	"   c = mix(c,fogcol,min(gotf*fogdist,1.0));\n"
	"   gl_FragDepth = depthmul/gotf + depthadd;\n"
	"   gl_FragColor = c;\n"
	"}\n";

static char vshadasm_drawoct[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"PARAM vmulx   = program.local[4]; #{vmulx.x,vmulx.y,vmulx.z,0}\n"
	"PARAM vmuly   = program.local[5]; #{vmuly.x,vmuly.y,vmuly.z,0}\n"
	"PARAM vadd    = program.local[6]; #{vadd.x ,vadd.y ,vadd.z ,0}\n"
	"PARAM k1 = {0.0001220703125,0.000244140625,0.0      ,-17.0    }; #-17 = unallocated\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.color, vertex.color;\n"
	"ADD result.texcoord[0], vertex.texcoord[0], -k1.xzzz;\n"
	"ADD result.texcoord[1], vertex.texcoord[0], +k1.xzzz;\n"
	"MAD t, vertex.texcoord[0].x, vmulx, vadd;\n"                 //v = tv.x*vmulx + tv.y*vmuly + vadd;
	"MAD result.texcoord[2], vertex.texcoord[0].y, vmuly, t;\n"
	"END\n";
		//ARB ASM min guaranteed limits:
		//INSTS     71/72 !!!
		//ATTRIBS    1/10 :)
		//PARAMS    21/24 :)
		//TEMPS     13/16 :)
		//ALU_INSTS 64/48 !!! (in practice, limit is 64)
		//TEX_INSTS  7/24 :)
		//TEX_IND    ?/ 4 :)
static char fshadasm_drawoct[] =
	"!!ARBfp1.0\n"
	"ATTRIB t0 = fragment.texcoord[0];\n"
	"ATTRIB t1 = fragment.texcoord[1];\n"
	"ATTRIB v  = fragment.texcoord[2];\n"
	"PARAM sidmul0 = program.local[0]; #{ 1/xsid, 2/xsid,0,0}\n"
	"PARAM sidmul1 = program.local[1]; #{ 2     , 1/ysid,0,0}\n"
	"PARAM sidadd1 = program.local[2]; #{.5/xsid,.5/ysid,0,0}\n"
	"PARAM depth   = program.local[3]; #{depthmul,depthadd,0,0}\n"
	"PARAM vmulx   = program.local[4]; #{vmulx.x,vmulx.y,vmulx.z,0}\n"
	"PARAM vmuly   = program.local[5]; #{vmuly.x,vmuly.y,vmuly.z,0}\n"
	"PARAM vadd    = program.local[6]; #{vadd.x ,vadd.y ,vadd.z ,0}\n"
	"PARAM pos     = program.local[7]; #{pos.x  ,pos.y  ,pos.z  ,0}\n"
	"PARAM mixnfog = program.local[8]; #{usemix,1/oct_fogdist,mixval,0}\n"
	"PARAM mulcol  = program.local[9]; #mulcol\n"
	"PARAM fogcol  = program.local[10]; #fogcol\n"
	"PARAM gsid024 = program.local[11]; #{gsideshade[0],gsideshade[2],gsideshade[4],0}\n"
	"PARAM gsid135 = program.local[12]; #{gsideshade[1],gsideshade[3],gsideshade[5],0}\n"
	"PARAM mipdat  = program.local[13]; #{mipoffs,maxmip,0,0}\n"
	"PARAM k0 = {1.0            ,255.0         ,255.5    ,2.0      };\n"
	"PARAM k1 = {0.0001220703125,0.000244140625,0.0      ,-17.0    }; #-17 = unallocated\n"
	"PARAM k2 = {0.5            ,0.0625        ,256.0    ,0.0      };\n"
	"PARAM k3 = {0.015625       ,0.03125       ,0.5078125,0.0      };\n"
	"PARAM k4 = {0.0625         ,16.0          ,4096.0   ,0.0      };\n"
	"PARAM k5 = {0.0625         ,16.0          ,4096.0   ,1048576.0};\n"
	"PARAM k6 = {0.0            ,0.0625        ,0.5      ,1.0      };\n"
	"TEMP t, o0, o1, i, ip, n, c, p, w, g, gotf, lod, t2, j, q;\n"
	"\n" //---------------------------------------------
	//"ADD t, tv, -k1.xzzz;\n"                      //(1/8192 = 0.0001220703125)
	"TEX o0, t0, texture[1], 2D;\n"               //o0 = floor(texture2D(tex1,t.xy-vec2(1.0/8192.0,0.0))*255.5);
	"MUL o0, o0, k0.zzzz;\n"
	"FLR o0, o0;\n"
	//"ADD t, tv, k1.xzzz;\n"
	"TEX o1, t1, texture[1], 2D;\n"               //o1 = floor(texture2D(tex1,t.xy+vec2(1.0/8192.0,0.0))*255.5);
	"MUL o1, o1, k0.zzzz;\n"
	"FLR o1, o1;\n"
	"\n" //---------------------------------------------
	"DP4 i, o1, k5;\n"                            //i = (o1.r>>4) + (o1.g<<4) + (o1.b<<12) + (o1.a<<20);
	"SLT t, i, k0.x;\n"                           //if (i < 1.0) discard;
	"KIL -t.x;\n"
	"MUL t.y, i.x, sidmul0;\n"                    //t = vec2(frac(f*rxsid)*2.0 + 0.5*rxsid,floor(f*rxsid*2)*rysid + 0.5*rysid);
	"DP4 i, o1, k4;\n"                            //(this 2nd DP4 is required to handle large addresses (check side walls of a VXL map!; limit i to 16-bit precision to avoid float truncation on y)
	"MUL t.x, i.x, sidmul0.x;\n"
	"FLR t.y, t.y;\n"
	"MAD t, t, sidmul1, sidadd1;\n"
	"\n" //---------------------------------------------
	"MAD ip, o0.gbaa, k2.zzww, o0.rgaa;\n"        //ip.x = ((o0.g*256 + o0.r)   )&4095;
	"MAD ip.z, o1.r, k2.wwzw, ip;\n"              //ip.y = ((o0.b*256 + o0.g)>>4)&4095;
	"MUL ip.y, ip.y, k4.wxww;\n"                  //ip.z = ((o1.r*256 + o0.a)   )&4095;
	"FLR ip, ip;\n"
	"MUL ip, ip, k1.yyyz;\n"                      //(1/4096 = 0.000244140625)
	"FRC ip, ip;\n"
	"MAD ip, ip, k4.zzzw, -pos;\n"                //ip -= pos;
	"\n" //---------------------------------------------
	"SLT p, v, k4.wwww;\n"                        //p = (vec4(lessThan(v,vec4(0.0)))+(ip-pos))/v;
	"ADD p, p, ip;\n"
	"RCP i.x, v.x;\n"
	"RCP i.y, v.y;\n"
	"RCP i.z, v.z;\n"
	"MUL p, p, i;\n"
	"MAX i, p.yxxw, p.zzyw;\n"                    //n = vec4(lessThan(max(p.yxxw,p.zzyw),p));
	"SLT n, i, p;\n"
	"DP3 gotf, p, n;\n"                           //gotf = dot(p.xyz,n.xyz);
	"MAD w, v, gotf, -ip;\n"                      //w = v*gotf-(ip-pos);
	"LRP_SAT w, n, w.z, w;\n"                     //w.xy = mix(w.xy,w.zz,n.xy);
	"\n" //---------------------------------------------
	"ADD j, t, sidmul0.xwww;\n"
	"TEX c, t, texture[2], 2D;\n"                 //c = texture2D(tex2,t).bgra;
	"TEX q, j, texture[2], 2D;\n"                 //q = texture2D(tex2,t+vec2(rxsid,0.0));
	"MOV c.a, k0.x;\n"                            //c.a = 1.0;
	"SLT t, k2.xxxw, q;\n"                        //c *= dot(q.xyz-vec3(lessThan(vec4(.5),q)),vec3(1.0))+1.0;
	"SUB t, q, t;\n"
	"DPH t, t, k0.xxxx;\n"
	"MUL c, t, c.bgra;\n"
	"MUL g, q.w, k0.y;\n"                         //g = q.w*255.0;
#if 0
	"MAD t, w, k3.yyww, k3.xxww;\n"               //t.x = clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0;
	"MUL g, g.x, k4.xxww;\n"                      //t.y = clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0;
	"FRC g.x, g.x;\n"
	"FLR g.y, g.y;\n"
	"MAD t, g, k6.wyxx, t;\n"
	"TEX t, t, texture[0], 2D;\n"                 //t = texture2D(tex0,t);
#else
		//Use for (oct_usefilter == 3)
	"MAD t, w, k3.xyww, k3.zxww;\n"               //t.x = clamp(w.x,0.0,1.0)/64.0 +  mod(g,16.0)/32.0 + 65.0/128.0;
	"MUL g, g.x, k4.xxww;\n"                      //t.y = clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0;
	"FRC g.x, g.x;\n"
	"FLR g.y, g.y;\n"
	"MAD t, g, k2.xyww, t;\n"
	"DP3 lod, v, n;\n"                            //lod = clamp(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,0.0,maxmip);
	"ABS lod, lod;\n"
	"RCP lod, lod.x;\n"
	"MUL lod, lod, gotf;\n"
	"LG2 lod, lod.x;\n"
	"ADD lod, lod, mipdat.x;\n"                   //(mipoffs)   (NOTE: this ADD,MAX,MIN could be optimized to: MAD_SAT&MUL)
	"MAX lod, lod, k4.w;\n"
	"MIN lod, lod, mipdat.y;\n"                   //(maxmip)
	"FRC w, lod;\n"
	"SUB g, lod, w;\n"                            //vec2 tx = t/pow(2.0,floor(lod));
#if 0
	"EX2 g, g.x;\n"
	"RCP g, g.x;\n"
	"MUL t, t, g;\n"
	"TEX g, t, texture[0], 2D;\n"                 //t = mix(texture2D(tex0,tx),texture2D(tex0,tx*.5),mod(lod,1.0));
	"MUL t, t, k6.zzxw;\n"
	"TEX t, t, texture[0], 2D;\n"
#else //unsure if TXP is fast or reliable on old GPUs?
	"EX2 t.w, g.x;\n"
	"MUL j, t, k0.xxxw;\n"
	"TXP g, t, texture[0], 2D;\n"                 //t = mix(texture2D(tex0,tx),texture2D(tex0,tx*.5),mod(lod,1.0));
	"TXP t2, j, texture[0], 2D;\n"
#endif
	"LRP t, w, t2, g;\n"
#endif
	"MUL i, t, c;\n"                              //if (usemix) c = mix(c,t,mixval); else c *= t;
	"LRP c, mixnfog.z, t, c;\n"
	"CMP c, -mixnfog.x, c, i;\n"
	"MUL c, c, mulcol;\n"                         //c *= mulcol;
	"CMP i, v, gsid135, gsid024;\n"               //c -= vec4(gsideshade[int(dot(vec3(lessThan(v.xyz,vec3(0.0)))+vec3(0.0,2.0,4.0),n.xyz))]);
	"DP3 i, i, n;\n"
	"SUB_SAT c.rgb, c, i;\n"
	"MUL_SAT i, gotf, mixnfog.y;\n"               //gl_FragColor = mix(c,fogcol,min(gotf*fogdist,1.0));
	"LRP result.color, i, fogcol, c;\n"
	"RCP i, gotf.x;\n"                            //gl_FragDepth = depthmul/gotf + depthadd;
	"MAD result.depth, i, depth.x, depth.y;\n"
	"END\n";

#elif (PIXMETH == 2)
//  2:128bit: x:32, y:32, z:32, psurf:32         :)   slow? :)

static char vshad_drawoct[] =
	"varying vec2 tv;\n"
	"varying vec4 v;\n"
	"uniform vec4 vmulx, vmuly, vadd;\n"
	"void main ()\n"
	"{\n"
	"   gl_Position = ftransform();\n"
	"   tv = gl_MultiTexCoord0.xy;\n"
	"   v = gl_MultiTexCoord0.x*vmulx + gl_MultiTexCoord0.y*vmuly + vadd;\n"
	"}\n";
static char fshad_drawoct[] =
	"varying vec2 tv;\n"
	"varying vec4 v;\n"
	"uniform sampler2D tex0, tex1, tex2, tex3;\n"
	"uniform vec4 pos, vmulx, vmuly, vadd, mulcol, fogcol;\n"
	"uniform float rxsid, rysid, mipoffs, maxmip, mixval, gsideshade[6];\n"
	"uniform int axsidm1, lxsidm1, usemix;\n"
	"uniform float depthmul, depthadd, fogdist;\n"
	"void main ()\n"
	"{\n"
	"   vec4 c, n, p, w;\n"
	"   uvec4 o0, o1, o2, o3, ip;\n"
	"   vec2 v2;\n"
	"   float f, g, gotf;\n"
	"   int i;\n"
	"\n"
	"   o0 = uvec4(texture2D(tex1,tv-vec2(1.50/8192.0))*255.5);\n"
	"   o1 = uvec4(texture2D(tex1,tv-vec2(0.50/8192.0))*255.5);\n"
	"   o2 = uvec4(texture2D(tex1,tv+vec2(0.50/8192.0))*255.5);\n"
	"   o3 = uvec4(texture2D(tex1,tv+vec2(1.50/8192.0))*255.5);\n"
	"   i = o3.r + (o3.g<<8) + (o3.b<<16) + (o3.a<<24); if (i == 0) discard;\n"
	"   ip.x = o0.r + (o0.g<<8);\n"
	"   ip.y = o1.r + (o1.g<<8);\n"
	"   ip.z = o2.r + (o2.g<<8);\n"
	"\n"
	"   p = (uvec4(lessThan(v,vec4(0.0)))+ip-pos)/v;\n"
	"   n = vec4(lessThan(max(p.yxxw,p.zzyw),p));\n"
	"   gotf = dot(p.xyz,n.xyz);\n"
	"   w = v*gotf+pos-vec4(ip); w.xy = mix(w.xy,w.zz,n.xy);\n"
	"\n"
	"   v2 = vec2(float(i&axsidm1)*2.0*rxsid + 0.5*rxsid,float(i>>lxsidm1)*rysid + 0.5*rysid);\n"
	"   c = texture2D(tex2,v2                ).bgra; c.a = 1.0;\n"
	"   p = texture2D(tex2,v2+vec2(rxsid,0.0));\n"
	"   g = p.w*255.0; c *= dot(p.xyz-vec3(lessThan(vec4(.5),p)),vec3(1.0))+1.0;\n"
	"#ifdef GL_ARB_shader_texture_lod\n"
	"   if (usemix != 0)\n"
	"   c = mix(c,texture2DLod(tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0),min(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,maxmip)),mixval);\n"
	"   else\n"
	"   c *=      texture2DLod(tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0),min(log2(gotf/abs(dot(v.xyz,n.xyz)))+mipoffs,maxmip));\n "
	"#else\n"
	"   c = mix(c,   texture2D(tex0,vec2(clamp(w.x,0.0,1.0)/32.0 +  mod(g,16.0)/16.0 + 1.0/64.0,\n"
	"                                    clamp(w.y,0.0,1.0)/32.0 +floor(g/16.0)/16.0 + 1.0/64.0)),mixval);\n"
	"#endif\n"
	"   c *= mulcol;\n"
	"   c.rgb -= vec3(gsideshade[int(dot(vec3(lessThan(v.xyz,vec3(0.0)))+vec3(0.0,2.0,4.0),n.xyz))]);\n
	"   c = mix(c,fogcol,min(gotf*fogdist,1.0));\n"
	"   gl_FragDepth = depthmul/gotf + depthadd;\n"
	"   gl_FragColor = c;\n"
	"}\n";

static char vshadasm_drawoct[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.color, vertex.color;\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"END\n";
static char fshadasm_drawoct[] =
	"!!ARBfp1.0\n"
	"MOV result.color, {0.8,0.6,0.4,1.0};\n"
	"END\n";

#endif

static void compileshader (int shad, const char *vshadst, const char *fshadst, const char *shadnam)
{
	if ((unsigned)shad >= (unsigned)SHADNUM) { MessageBox(ghwnd,"compileshader():invalid index",shadnam,MB_OK); ExitProcess(0); }

	if (oct_useglsl == 0)
	{
		if (glfp[glUseProgram]) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(0);
		glGetError(); //flush errors

		glEnable(GL_VERTEX_PROGRAM_ARB);
		((PFNGLGENPROGRAMSARBPROC)glfp[glGenProgramsARB])(1,&shadvert[shad]);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB,shadvert[shad]);
		((PFNGLPROGRAMSTRINGARBPROC)glfp[glProgramStringARB])(GL_VERTEX_PROGRAM_ARB,GL_PROGRAM_FORMAT_ASCII_ARB,strlen(vshadst),vshadst);
		if (glGetError() != GL_NO_ERROR) { MessageBox(ghwnd,(const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB),shadnam,MB_OK); return; }

		glEnable(GL_FRAGMENT_PROGRAM_ARB);
		((PFNGLGENPROGRAMSARBPROC)glfp[glGenProgramsARB])(1,&shadfrag[shad]);
		((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shad]);
		((PFNGLPROGRAMSTRINGARBPROC)glfp[glProgramStringARB])(GL_FRAGMENT_PROGRAM_ARB,GL_PROGRAM_FORMAT_ASCII_ARB,strlen(fshadst),fshadst);
		if (glGetError() != GL_NO_ERROR) { MessageBox(ghwnd,(const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB),shadnam,MB_OK); return; }
	}
	else
	{
		int compiled;
		const char *cptr;
		char tbuf[4096];

		glDisable(GL_VERTEX_PROGRAM_ARB);
		glDisable(GL_FRAGMENT_PROGRAM_ARB);

		shadvert[shad] = ((PFNGLCREATESHADERPROC)(glfp[glCreateShader]))(GL_VERTEX_SHADER);
		cptr = vshadst; ((PFNGLSHADERSOURCEPROC)glfp[glShaderSource])(shadvert[shad],1,(const char **)&cptr,0);
		((PFNGLCOMPILESHADERPROC)glfp[glCompileShader])(shadvert[shad]);
		((PFNGLGETSHADERIVPROC)glfp[glGetShaderiv])(shadvert[shad],GL_COMPILE_STATUS,&compiled);
		if (!compiled)
		{
			((PFNGLGETINFOLOGARBPROC)glfp[glGetInfoLogARB])(shadvert[shad],sizeof(tbuf),0,tbuf);
			MessageBox(ghwnd,tbuf,shadnam,MB_OK); return;
		}

		shadfrag[shad] = ((PFNGLCREATESHADERPROC)(glfp[glCreateShader]))(GL_FRAGMENT_SHADER);
		cptr = fshadst; ((PFNGLSHADERSOURCEPROC)glfp[glShaderSource])(shadfrag[shad],1,(const char **)&cptr,0);
		((PFNGLCOMPILESHADERPROC)glfp[glCompileShader])(shadfrag[shad]);
		((PFNGLGETSHADERIVPROC)glfp[glGetShaderiv])(shadfrag[shad],GL_COMPILE_STATUS,&compiled);
		if (!compiled)
		{
			((PFNGLGETINFOLOGARBPROC)glfp[glGetInfoLogARB])(shadfrag[shad],sizeof(tbuf),0,tbuf);
			MessageBox(ghwnd,tbuf,shadnam,MB_OK); return;
		}

		shadprog[shad] = ((PFNGLCREATEPROGRAMPROC)glfp[glCreateProgram])();
		((PFNGLATTACHSHADERPROC)glfp[glAttachShader])(shadprog[shad],shadvert[shad]);
		((PFNGLATTACHSHADERPROC)glfp[glAttachShader])(shadprog[shad],shadfrag[shad]);

		((PFNGLLINKPROGRAMPROC)glfp[glLinkProgram])(shadprog[shad]);
		((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shad]);

			//Note: Get*Uniform*() must be called after glUseProgram() to work properly
		((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shad],"tex0"),0);
		((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shad],"tex1"),1);
		((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shad],"tex2"),2);
		((PFNGLUNIFORM1IPROC)glfp[glUniform1i])(((PFNGLGETUNIFORMLOCATIONPROC)glfp[glGetUniformLocation])(shadprog[shad],"tex3"),3);
	}
}

static int glcheckext (char *extnam)
{
	const char *st = (char *)glGetString(GL_EXTENSIONS);
	st = strstr(st,extnam); if (!st) return(0);
	return(st[strlen(extnam)] <= 32);
}

static void kgluninit (HWND hWnd, HDC hDC, HGLRC hRC)
{
	wglMakeCurrent(0,0);
	wglDeleteContext(hRC);
	ReleaseDC(hWnd,hDC);
}
static int kglinit (HDC *hDC, HGLRC *hRC)
{
	PIXELFORMATDESCRIPTOR pfd;
	RECT r, rw;
	int i, x, y, format;
	char tbuf[256];

	if (fullscreen) ShowWindow(ghwnd,SW_HIDE);

	*hDC = GetDC(ghwnd);

	ZeroMemory(&pfd,sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.cDepthBits = 32;
	pfd.iLayerType = PFD_MAIN_PLANE;
	format = ChoosePixelFormat(*hDC,&pfd);
	SetPixelFormat(*hDC,format,&pfd);

	*hRC = wglCreateContext(*hDC);
	wglMakeCurrent(*hDC,*hRC);

	if (!wglGetProcAddress("glCreateShaderObjectARB"))
	{
		//if ((GPU totally unsupported..) && (!oct_usegpu_cmdline)) { kgluninit(ghwnd,glhDC,glhRC); return(-1); }
		oct_useglsl = 0; oct_usefilter = 3;
	}
	else
	{
		if (!oct_usegpu_cmdline)
		{
			const unsigned char *cptr = glGetString(0x8B8C /*GL_SHADING_LANGUAGE_VERSION*/);
			if (cptr[0] < '3') { kgluninit(ghwnd,glhDC,glhRC); return(-1); }
		}
	}

	if (fullscreen) ShowWindow(ghwnd,SW_SHOW);

	for(i=0;i<NUMGLFUNC;i++)
	{
		if (!useoldglfuncs)
		{
			glfp[i] = (glfp_t)wglGetProcAddress(glnames[i]); if (glfp[i]) continue;
			useoldglfuncs = 1;
		}
		glfp[i] = (glfp_t)wglGetProcAddress(glnames_old[i]); if (glfp[i]) continue;
		sprintf(tbuf,"%s() / %s() not supported. :/",glnames[i],glnames_old[i]);
		if (i < glCreateShader) { MessageBox(ghwnd,tbuf,prognam,MB_OK); ExitProcess(0); }
	}

	if (glfp[wglSwapIntervalEXT]) ((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])(swapinterval);
	return(0);
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------

	//tree node vs. sphere
#if (MARCHCUBE == 0)

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_sph_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_sph_t *sph;
	int x, y, z, sm1, dx, dy, dz, nx, ny, nz;

	sph = (brush_sph_t *)brush;
	sm1 = (1<<ls)-1;
	dx = sph->x-x0; nx = sm1-dx;
	dy = sph->y-y0; ny = sm1-dy;
	dz = sph->z-z0; nz = sm1-dz;
	x = max(    dx   , nx); y = max(    dy   , ny); z = max(    dz   , nz); if (x*x + y*y + z*z <  sph->r2) return(2); //farthest point inside?
	x = max(min(dx,0),-nx); y = max(min(dy,0),-ny); z = max(min(dz,0),-nz); if (x*x + y*y + z*z >= sph->r2) return(0); //nearest point outside?
	return(1);
}

static void brush_sph_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_sph_t *sph;
	float f;
	int i, dx, dy, dz, rr, gg, bb;

	//fixcnt_getsurf++;
	sph = (brush_sph_t *)brush; i = (x0+13)*(y0+17)*(z0+19)*0x25d3eb;
	surf->b = min(max(( sph->col     &255) + ((i>> 8)&15)-8,0),255);
	surf->g = min(max(((sph->col>> 8)&255) + ((i>>12)&15)-8,0),255);
	surf->r = min(max(((sph->col>>16)&255) + ((i>>16)&15)-8,0),255);
	surf->a = 255;
	surf->tex = ((x0^y0^z0)&63);
	dx = x0-sph->x;
	dy = y0-sph->y;
	dz = z0-sph->z;
	i = dx*dx + dy*dy + dz*dz;
#if 0
	i = (int)(-64.0*65536.0/sqrt((double)i));
#else
	static const float kmul = -64.0*65536.0;
	_asm
	{
		cvtsi2ss xmm0, i
		rsqrtss xmm0, xmm0
		mulss xmm0, kmul
		cvtss2si eax, xmm0
		mov i, eax
	}
#endif
	surf->norm[0] = (signed char)((dx*i)>>16);
	surf->norm[1] = (signed char)((dy*i)>>16);
	surf->norm[2] = (signed char)((dz*i)>>16);
}

void brush_sph_init (brush_sph_t *sph, int x, int y, int z, int r, int issol)
{
	sph->isins = brush_sph_isins;
	sph->getsurf = brush_sph_getsurf;
	sph->flags = 1;
	sph->x = x; sph->y = y; sph->z = z; sph->r2 = r*r;
	sph->col = (((x>>2)&255)<<16) +
				  (((y>>2)&255)<< 8) +
				  (((z>>2)&255)<< 0) + 0x404040;
}

//--------------------------------------------------------------------------------------------------
	//tree node vs. box

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_box_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_box_t *box;
	int s;

	box = (brush_box_t *)brush;

	s = (1<<ls);

	if ((x0 >= box->x0) && (x0+s <= box->x1) &&
		 (y0 >= box->y0) && (y0+s <= box->y1) &&
		 (z0 >= box->z0) && (z0+s <= box->z1)) return(2);
	if ((x0+s <= box->x0) || (x0 >= box->x1) ||
		 (y0+s <= box->y0) || (y0 >= box->y1) ||
		 (z0+s <= box->z0) || (z0 >= box->z1)) return(0);
	return(1);
}

static void brush_box_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_box_t *box;
	int i, dx, dy, dz;

	box = (brush_box_t *)brush; i = (x0+13)*(y0+17)*(z0+19)*0x25d3eb;
	surf->b = min(max(( box->col     &255) + ((i>> 8)&15)-8,0),255);
	surf->g = min(max(((box->col>> 8)&255) + ((i>>12)&15)-8,0),255);
	surf->r = min(max(((box->col>>16)&255) + ((i>>16)&15)-8,0),255);
	surf->a = 255;
	surf->tex = ((x0^y0^z0)&63);

	dx = 0; dy = 0; dz = 0;
	if (x0 == box->x0) dx--; else if (x0 == box->x1-1) dx++;
	if (y0 == box->y0) dy--; else if (y0 == box->y1-1) dy++;
	if (z0 == box->z0) dz--; else if (z0 == box->z1-1) dz++;
	i = dx*dx + dy*dy + dz*dz;
#if 0
	i = (int)(-64.0*65536.0/sqrt((double)i));
#else
	static const float kmul = -64.0*65536.0;
	_asm
	{
		cvtsi2ss xmm0, i
		rsqrtss xmm0, xmm0
		mulss xmm0, kmul
		cvtss2si eax, xmm0
		mov i, eax
	}
#endif
	surf->norm[0] = (signed char)((dx*i)>>16);
	surf->norm[1] = (signed char)((dy*i)>>16);
	surf->norm[2] = (signed char)((dz*i)>>16);
}

void brush_box_init (brush_box_t *box, double x0, double y0, double z0, double x1, double y1, double z1, int issol)
{
	box->isins = brush_box_isins;
	box->getsurf = brush_box_getsurf;
	box->flags = 1;
	box->x0 = x0; box->y0 = y0; box->z0 = z0;
	box->x1 = x1; box->y1 = y1; box->z1 = z1;
	box->col = ((((int)(x0+x1)>>3)&255)<<16) +
				  ((((int)(y0+y1)>>3)&255)<< 8) +
				  ((((int)(z0+z1)>>3)&255)<< 0) + 0x404040;
}

//--------------------------------------------------------------------------------------------------

#elif (MARCHCUBE != 0)

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_sph_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	double x, y, z, dx, dy, dz, dr, nx, ny, nz;
	brush_sph_t *sph = (brush_sph_t *)brush;

	dr = (double)(1<<ls);
	dx = sph->x-(double)x0; nx = dr-dx;
	dy = sph->y-(double)y0; ny = dr-dy;
	dz = sph->z-(double)z0; nz = dr-dz;
	x =     max( dx     ,nx); y =     max( dy     ,ny); z =     max( dz     ,nz); if (x*x + y*y + z*z <  sph->r2) return(2); //farthest point inside?
	x = min(max(-dx,0.0),nx); y = min(max(-dy,0.0),ny); z = min(max(-dz,0.0),nz); if (x*x + y*y + z*z >= sph->r2) return(0); //nearest point outside?
	return(1);
}
static void brush_sph_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	dpoint3d tri[15];
	double d, dx, dy, dz;
	int i, x, y, z;
	brush_sph_t *sph = (brush_sph_t *)brush;

#if 0
	surf->b = ( sph->col     &255);
	surf->g = ((sph->col>> 8)&255);
	surf->r = ((sph->col>>16)&255);
#else
	i = (x0+13)*(y0+17)*(z0+19)*0x25d3eb;
	surf->b = min(max(( sph->col     &255) + ((i>> 8)&15)-8,0),255);
	surf->g = min(max(((sph->col>> 8)&255) + ((i>>12)&15)-8,0),255);
	surf->r = min(max(((sph->col>>16)&255) + ((i>>16)&15)-8,0),255);
#endif
	surf->a = 255;
	surf->tex = ((x0^y0^z0)&63);
	for(z=2-1;z>=0;z--)
		for(y=2-1;y>=0;y--)
			for(x=2-1;x>=0;x--)
			{
				dx = x0+x-sph->x; dy = y0+y-sph->y; dz = z0+z-sph->z;
				i = cvttss2si((sqrt(dx*dx + dy*dy + dz*dz) - sph->r)*sph->cornsc);
				surf->cornval[z*4+y*2+x] = (signed char)min(max(i,-128),127);
			}
	//if (marchcube_gen(surf,tri))
	//{
	//   dx = x0-sph->x; dy = y0-sph->y; dz = z0-sph->z;
	//   for(i=2-1;i>=0;i--)
	//      surf->u[i] = (char)(atan2(tri[?].?)*sph->texusc);
	//}

#if 0
	surf->norm[0] = 0;
	surf->norm[1] = 0;
	surf->norm[2] = 0;
#else
	dx = x0-sph->x;
	dy = y0-sph->y;
	dz = z0-sph->z;
	d = -64.0/sqrt(dx*dx + dy*dy + dz*dz);
	surf->norm[0] = (signed char)(dx*d);
	surf->norm[1] = (signed char)(dy*d);
	surf->norm[2] = (signed char)(dz*d);
#endif
}
void brush_sph_init (brush_sph_t *sph, double x, double y, double z, double r, int issol)
{
	sph->isins = brush_sph_isins;
	sph->getsurf = brush_sph_getsurf;
	sph->flags = 1;
	sph->x = x; sph->y = y; sph->z = z; sph->r = r; sph->r2 = r*r; sph->issol = issol;
	sph->col = ((((int)(x*.25))&255)<<16) +
				  ((((int)(y*.25))&255)<< 8) +
				  ((((int)(z*.25))&255)<< 0) + 0x404040;
	if (!issol) sph->cornsc = 128.0/sqrt(3.0);
			 else sph->cornsc =-128.0/sqrt(3.0);
}

//--------------------------------------------------------------------------------------------------
	//tree node vs. box

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_box_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_box_t *box;
	double hs, nx0, ny0, nz0, nx, ny, nz, rx0, ry0, rz0;
	int s;

	box = (brush_box_t *)brush;

	s = (1<<ls);

	hs = (double)s*.5;
	nx0 = (double)x0+hs; nx0 = min(nx0,box->x0+box->x1-nx0); rx0 = box->x0+box->cornrad;
	ny0 = (double)y0+hs; ny0 = min(ny0,box->y0+box->y1-ny0); ry0 = box->y0+box->cornrad;
	nz0 = (double)z0+hs; nz0 = min(nz0,box->z0+box->z1-nz0); rz0 = box->z0+box->cornrad;
	nx = max(nx0,rx0-hs)-nx0;
	ny = max(ny0,ry0-hs)-ny0;
	nz = max(nz0,rz0-hs)-nz0; if (nx*nx + ny*ny + nz*nz > box->cornrad*box->cornrad) return(0);
	nx = max(nx0,rx0+hs)-nx0;
	ny = max(ny0,ry0+hs)-ny0;
	nz = max(nz0,rz0+hs)-nz0; return((nx*nx + ny*ny + nz*nz < box->cornrad*box->cornrad)+1);
}

static void brush_box_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_box_t *box;
	dpoint2d lin[4];
	double fx, fy, fz;
	int i, j, x, y, z, v0, v1, dx, dy, dz;

	box = (brush_box_t *)brush; i = (x0+13)*(y0+17)*(z0+19)*0x25d3eb;
	surf->b = min(max(( box->col     &255) + ((i>> 8)&15)-8,0),255);
	surf->g = min(max(((box->col>> 8)&255) + ((i>>12)&15)-8,0),255);
	surf->r = min(max(((box->col>>16)&255) + ((i>>16)&15)-8,0),255);
	surf->a = 255;
	surf->tex = ((x0^y0^z0)&63);

	for(i=8-1;i>=0;i--)
	{
		x = ( i    &1)+x0; fx = min(max(x,box->x0+box->cornrad),box->x1-box->cornrad)-x;
		y = ((i>>1)&1)+y0; fy = min(max(y,box->y0+box->cornrad),box->y1-box->cornrad)-y;
		z = ((i>>2)&1)+z0; fz = min(max(z,box->z0+box->cornrad),box->z1-box->cornrad)-z;
		surf->cornval[i] = min(max((sqrt(fx*fx + fy*fy + fz*fz)-box->cornrad)*128.0,-128),127);
	}
	if (box->issol) { for(i=8-1;i>=0;i--) surf->cornval[i] ^= 0xff; }

		//FIX:Horrible hack leaves tiny gaps!
	v0 = 0x7fffffff; v1 = 0x80000000;
	for(i=8-1;i>=0;i--)
	{
		v0 = min(v0,surf->cornval[i]);
		v1 = max(v1,surf->cornval[i]);
	}
	if (v0 >= 0) { v0 = ~v0; for(i=8-1;i>=0;i--) surf->cornval[i] = max(surf->cornval[i]+v0,-128); }
	if (v1 <  0) { v1 = -v1; for(i=8-1;i>=0;i--) surf->cornval[i] = min(surf->cornval[i]+v1,+127); }

	dx = x0-box->x0;
	dy = y0-box->y0;
	dz = z0-box->z0;
	i = dx*dx + dy*dy + dz*dz;
#if 0
	i = (int)(-64.0*65536.0/sqrt((double)i));
#else
	static const float kmul = -64.0*65536.0;
	_asm
	{
		cvtsi2ss xmm0, i
		rsqrtss xmm0, xmm0
		mulss xmm0, kmul
		cvtss2si eax, xmm0
		mov i, eax
	}
#endif
	surf->norm[0] = (signed char)((dx*i)>>16);
	surf->norm[1] = (signed char)((dy*i)>>16);
	surf->norm[2] = (signed char)((dz*i)>>16);
}

void brush_box_init (brush_box_t *box, int x0, int y0, int z0, int x1, int y1, int z1, int issol)
{
	box->isins = brush_box_isins;
	box->getsurf = brush_box_getsurf;
	box->flags = 1;
	box->x0 = x0; box->y0 = y0; box->z0 = z0;
	box->x1 = x1; box->y1 = y1; box->z1 = z1;
	box->col = ((((x0+x1)>>3)&255)<<16) +
				  ((((y0+y1)>>3)&255)<< 8) +
				  ((((z0+z1)>>3)&255)<< 0) + 0x404040;

	box->issol = issol;
	box->cornrad = 4.0;
	//box->cornrad = min(box->cornrad,min(min(x1-x0,y1-y0),z1-z0)*.5);
	box->nx0 = x0+box->cornrad; box->nx1 = x1-box->cornrad;
	box->ny0 = y0+box->cornrad; box->ny1 = y1-box->cornrad;
	box->nz0 = z0+box->cornrad; box->nz1 = z1-box->cornrad;
}

#endif
//--------------------------------------------------------------------------------------------------
	//tree node vs. sphere, solid surf - for oct_paint()
static void brush_sph_sol_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_sph_t *sph;
	sph = (brush_sph_t *)brush;
	memcpy(surf,sph->surf,sizeof(surf_t));
}
void brush_sph_sol_init (brush_sph_t *sph, int x, int y, int z, int r, surf_t *surf)
{
	sph->isins = brush_sph_isins;
	sph->getsurf = brush_sph_sol_getsurf;
	sph->flags = 1;
	sph->x = x; sph->y = y; sph->z = z; sph->r2 = r*r;
	sph->surf = surf;
}
//--------------------------------------------------------------------------------------------------
//FIXFIXFIX

	//tree node vs. cone

	//See CONE_INTERSECT.KC for derivation
static void indrawcone3d_init (brush_cone_t *ic)
{
	float x0, y0, z0, r0, x1, y1, z1, r1;
	float t, dx, dy, dz, dr, d2, f, k, r;

	x0 = ic->x0; y0 = ic->y0; z0 = ic->z0; r0 = ic->r0;
	x1 = ic->x1; y1 = ic->y1; z1 = ic->z1; r1 = ic->r1;
	if (fabs(r0) > fabs(r1)) //avoid bad case of 1 fully inside 0
	{
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		t = z0; z0 = z1; z1 = t;
		t = r0; r0 = r1; r1 = t;
	}
	ic->x0 = x0; ic->y0 = y0; ic->z0 = z0; ic->r02 = max(r0,0.f); ic->r02 *= ic->r02; r0 = fabs(r0);
	ic->x1 = x1; ic->y1 = y1; ic->z1 = z1; ic->r12 = max(r1,0.f); ic->r12 *= ic->r12; r1 = fabs(r1);
	dx = x1-x0; dy = y1-y0; dz = z1-z0; dr = r1-r0; d2 = dx*dx + dy*dy + dz*dz - dr*dr; f = 1/sqrt(d2);
	if (fabs(dr) < r0*1e-6) { ic->hak = r0*r0; ic->dr = 0.f;  ic->cr = r0;  k =      f; r =   0.f; r0 = 0.f; r1 = d2; }
							 else { ic->hak = 0.f;   ic->dr = dr*f; ic->cr = 0.f; k = 1.f/dr; r = -r0*k; k *= f*d2; }
	ic->cx = dx*r + x0; ic->dx = dx*f; ic->k0 = r0*k;
	ic->cy = dy*r + y0; ic->dy = dy*f; ic->k1 = r1*k;
	ic->cz = dz*r + z0; ic->dz = dz*f;
}

static int indrawcone3d (brush_cone_t *ic, float x, float y, float z)
{
	float nx, ny, nz, d;
	nx = x-ic->cx; ny = y-ic->cy; nz = z-ic->cz; d = nx*ic->dx + ny*ic->dy + nz*ic->dz;
	if (d <= ic->k0) return((x-ic->x0)*(x-ic->x0) + (y-ic->y0)*(y-ic->y0) + (z-ic->z0)*(z-ic->z0) < ic->r02);
	if (d >= ic->k1) return((x-ic->x1)*(x-ic->x1) + (y-ic->y1)*(y-ic->y1) + (z-ic->z1)*(z-ic->z1) < ic->r12);
	return(nx*nx + ny*ny + nz*nz < d*d + ic->hak);
}

	//find point on line segment that minimizes cone angle
	//Derivation:
	//   x = (x1-x0)*t + x0
	//   y = (y1-y0)*t + y0
	//   z = (z1-z0)*t + z0
	//   ((x-cx)*dx + (y-cy)*dy + (z-cz)*dz) = sqrt((x-cx)^2 + (y-cy)^2 + (z-cz)^2)*sqrt(dx^2 + dy^2 + dz^2)*cos(ang)
	//To solve, plug in x/y/z, solve derivate w.r.t. t (quotient rule), then solve numerator=0 (quadratic equation)
static float nearestptline2cone (float dx, float dy, float dz, float dr, float cx, float cy, float cz, float cr, float x0, float y0, float z0, float x1, float y1, float z1)
{
	float k0, k1, k2, k3, k4, k5, k6, k7, Za, Zb, Zc, insqr, t, s;
	k0 = (x1-x0)*dx + (y1-y0)*dy + (z1-z0)*dz;
	k1 = (x0-cx)*dx + (y0-cy)*dy + (z0-cz)*dz;
	k2 = (x1-x0)*(x1-x0) + (y1-y0)*(y1-y0) + (z1-z0)*(z1-z0);
	k3 =((x1-x0)*(x0-cx) + (y1-y0)*(y0-cy) + (z1-z0)*(z0-cz))*2;
	k4 = (x0-cx)*(x0-cx) + (y0-cy)*(y0-cy) + (z0-cz)*(z0-cz);
	k5 = k0*k0;
	k6 = k0*k1*2;
	k7 = k1*k1;
	Za = k3*k5 - k2*k6;
	Zb =(k4*k5 - k2*k7)*2;
	Zc = k4*k6 - k3*k7;
	insqr = Zb*Zb - 4.f*Za*Zc; if (insqr < 0.f) return(-1);
	for(s=-1.f;s<=1.f;s+=2.f)
	{
		t = (-Zb + s*sqrt(insqr))/(Za+Za);
		if ((t > 0.f) && (t < 1.f)) return(t);
	}
	return(-1.f);
}

static int box_sph_isins (float bx, float by, float bz, float bs, float cx, float cy, float cz, float cr)
{
	float dx, dy, dz, nx, ny, nz, x, y, z;
	dx = cx-bx; nx = bs-dx;
	dy = cy-by; ny = bs-dy;
	dz = cz-bz; nz = bs-dz;
	x = max(    dx     , nx); y = max(    dy     , ny); z = max(    dz,      nz); if (x*x + y*y + z*z <  cr*cr) return(2); //far  pt in?
	x = max(min(dx,0.f),-nx); y = max(min(dy,0.f),-ny); z = max(min(dz,0.f),-nz); if (x*x + y*y + z*z >= cr*cr) return(0); //near pt out?
	return(1);
}

static int indrawcone3d_intbox (brush_cone_t *ic, float bx, float by, float bz, float bs)
{
	static const signed char edge[12][6] =
	{
		0,0,0,1,0,0, 0,1,0,1,1,0, 0,0,1,1,0,1, 0,1,1,1,1,1,
		0,0,0,0,1,0, 1,0,0,1,1,0, 0,0,1,0,1,1, 1,0,1,1,1,1,
		0,0,0,0,0,1, 1,0,0,1,0,1, 0,1,0,0,1,1, 1,1,0,1,1,1,
	};
	float t, x, y, z, r, nx, ny, nz, nx0, ny0, nz0, nx1, ny1, nz1;
	int i;

	for(i=8-1;i>=0;i--)
	{
		nx = bx+((i   )&1)*bs;
		ny = by+((i>>1)&1)*bs;
		nz = bz+((i>>2)&1)*bs;
		if (!indrawcone3d(ic,nx,ny,nz)) break;
	}
	if (i < 0) return(2);
	for(i=0;i<12;i++)
	{
		nx0 = bx + (float)edge[i][0]*bs; ny0 = by + (float)edge[i][1]*bs; nz0 = bz + (float)edge[i][2]*bs;
		nx1 = bx + (float)edge[i][3]*bs; ny1 = by + (float)edge[i][4]*bs; nz1 = bz + (float)edge[i][5]*bs;
		t = nearestptline2cone(ic->dx,ic->dy,ic->dz,ic->dr, ic->cx,ic->cy,ic->cz,ic->cr, nx0,ny0,nz0, nx1,ny1,nz1);
		if (t < 0.f) continue;

		nx = (nx1-nx0)*t + nx0;
		ny = (ny1-ny0)*t + ny0;
		nz = (nz1-nz0)*t + nz0;
		t = min(max((nx-ic->cx)*ic->dx + (ny-ic->cy)*ic->dy + (nz-ic->cz)*ic->dz,ic->k0),ic->k1);
		x = t*ic->dx + ic->cx;
		y = t*ic->dy + ic->cy;
		z = t*ic->dz + ic->cz;
		r = t*ic->dr + ic->cr;
		if (box_sph_isins(bx,by,bz,bs,x,y,z,r)) return(1);
	}
	for(i=0;i<8;i++)
	{
		nx = bx+((i   )&1)*bs;
		ny = by+((i>>1)&1)*bs;
		nz = bz+((i>>2)&1)*bs;
		t = min(max((nx-ic->cx)*ic->dx + (ny-ic->cy)*ic->dy + (nz-ic->cz)*ic->dz,ic->k0),ic->k1);
		x = t*ic->dx + ic->cx;
		y = t*ic->dy + ic->cy;
		z = t*ic->dz + ic->cz;
		r = t*ic->dr + ic->cr;
		if (box_sph_isins(bx,by,bz,bs,x,y,z,r)) return(1);
	}
	return(0);
}

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_cone_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_cone_t *c;
	float d, dx, dy, dz, dx2, dy2, dz2;
	int i, x, y, z, x1, y1, z1;

	c = (brush_cone_t *)brush;
	//x1 = x0 + (1<<ls);
	//y1 = y0 + (1<<ls);
	//z1 = z0 + (1<<ls);

		//cube: (x0,y0,z0) - (x1,y1,z1)
		//   vs.
		//cone: (c->x0,c->y0,c->z0,c->r0) - (c->x1,c->y1,c->z1,c->r1)

	return(indrawcone3d_intbox(c,x0,y0,z0,1<<ls));

#if 0
		//if all 8 points of cube are inside cone, return(2);
	for(i=8-1;i>=0;i--)
	{
		if (i&1) x = x0; else x = x1;
		if (i&2) y = y0; else y = y1;
		if (i&4) z = z0; else z = z1;

		dx = ((float)x)-c->x1;
		dy = ((float)y)-c->y1;
		dz = ((float)z)-c->z1;
		if (dx*dx + dy*dy + dz*dz >= c->r1*c->r1) break;

		dx = ((float)x)-c->x0;
		dy = ((float)y)-c->y0;
		dz = ((float)z)-c->z0;
		if (dx*dx + dy*dy + dz*dz >= c->r0*c->r0) break;

		dx2 = c->x1-c->x0;
		dy2 = c->y1-c->y0;
		dz2 = c->z1-c->z0;
		d = (dx*dx2 + dy*dy2 + dz*dz2) / (dx2*dx2 + dy2*dy2 + dz2*dz2);
		????//FIXFIXFIX
	}
	if (i < 0) return(2);


	//if (x*x + y*y + z*z >= c->r1) return(0); //nearest point outside?
#endif

	return(0);
}

static void brush_cone_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_cone_t *cone;
	float f;
	int i, dx, dy, dz;

	//fixcnt_getsurf++;
	cone = (brush_cone_t *)brush;
	*(int *)&surf->b = cone->col;
	surf->tex = ((x0^y0^z0)&63);
	dx = x0-(cone->x0+cone->x1)*.5;
	dy = y0-(cone->y0+cone->y1)*.5;
	dz = z0-(cone->z0+cone->z1)*.5;
	i = dx*dx + dy*dy + dz*dz;
#if 0
	i = (int)(-64.0*65536.0/sqrt((double)i));
#else
	static const float kmul = -64.0*65536.0;
	_asm
	{
		cvtsi2ss xmm0, i
		rsqrtss xmm0, xmm0
		mulss xmm0, kmul
		cvtss2si eax, xmm0
		mov i, eax
	}
#endif
	surf->norm[0] = (signed char)((dx*i)>>16);
	surf->norm[1] = (signed char)((dy*i)>>16);
	surf->norm[2] = (signed char)((dz*i)>>16);
}

void brush_cone_init (brush_cone_t *cone, float x0, float y0, float z0, float r0, float x1, float y1, float z1, float r1)
{
	cone->isins = brush_cone_isins;
	cone->getsurf = brush_cone_getsurf;
	cone->flags = 1;
	cone->x0 = x0; cone->y0 = y0; cone->z0 = z0; cone->r0 = r0;
	cone->x1 = x1; cone->y1 = y1; cone->z1 = z1; cone->r1 = r1;
	indrawcone3d_init(cone);
	cone->col = ((((int)x0>>2)&255)<<16) +
					((((int)y0>>2)&255)<< 8) +
					((((int)z0>>2)&255)<< 0) + 0x404040;
}
//--------------------------------------------------------------------------------------------------
	//tree node vs. single voxel

	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_vox_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_vox_t *vox;
	int s;

	vox = (brush_vox_t *)brush;
	s = (1<<ls);
	if ((vox->x < x0) || (vox->x >= x0+s)) return(0);
	if ((vox->y < y0) || (vox->y >= y0+s)) return(0);
	if ((vox->z < z0) || (vox->z >= z0+s)) return(0);
	return((!ls)+1);
}
static void brush_vox_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_vox_t *vox = (brush_vox_t *)brush;
	//fixcnt_getsurf++;
	(*surf) = vox->surf;
}
void brush_vox_init (brush_vox_t *vox, int x, int y, int z, surf_t *surf)
{
	vox->isins = brush_vox_isins;
	vox->getsurf = brush_vox_getsurf;
	vox->flags = 1;
	vox->x = x; vox->y = y; vox->z = z; vox->surf = (*surf);
}

static void oct_debugprintftree (oct_t *loct)
{
	int i, ls, ind, stkind[OCT_MAXLS], stknum[OCT_MAXLS];

	ls = loct->lsid-1; stkind[ls] = loct->head; stknum[ls] = 1;
	while (1)
	{
		ind = stkind[ls]; stkind[ls]++; stknum[ls]--; //2sibly
		if (ls >= 0) //2child
		{
			octv_t *ptr = (octv_t *)loct->nod.buf;
			printf("nod[%5d]: ls=%d, chi:%02x, sol:%02x, ind:%d\n",ind,ls,ptr[ind].chi,ptr[ind].sol,ptr[ind].ind);
			i = popcount[ptr[ind].chi];
			ls--; stkind[ls] = ptr[ind].ind; stknum[ls] = i;
		} else { printf("sur[%5d]\n",ind); } //surf
		while (stknum[ls] <= 0) { ls++; if (ls >= loct->lsid) return; } //2parent
	}
}

//--------------------------------------------------------------------------------------------------
	//      n: # structs to alloc
	//returns: bit index or crash if realloc fails
static int bitalloc (oct_t *loct, bitmal_t *bm, int n)
{
	int i, oi, ie, i0, i1, cnt;
#ifndef _WIN64
	int j, k;
	unsigned int *bitbuf = bm->bit;
#else
	__int64 j, k;
	unsigned __int64 *bitbuf = (unsigned __int64 *)bm->bit;
#endif

	i = bm->ind; oi = i; ie = bm->mal-n;
	for(cnt=1;1;cnt--)
	{
		switch(n)
		{
#ifndef _WIN64
			case 1: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i];                                                                              if (j != -1) goto found; } break;
			case 2: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1);                           k = bitbuf[i+1]; j |= (      k <<31); if (j != -1) goto found; } break;
			case 3: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1)|(j>>2);                    k = bitbuf[i+1]; j |= (((-k)|k)<<30); if (j != -1) goto found; } break;
			case 4: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2);              k = bitbuf[i+1]; j |= (((-k)|k)<<29); if (j != -1) goto found; } break;
			case 5: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2)|(j>>3);       k = bitbuf[i+1]; j |= (((-k)|k)<<28); if (j != -1) goto found; } break;
			case 6: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2)|(j>>4);       k = bitbuf[i+1]; j |= (((-k)|k)<<27); if (j != -1) goto found; } break;
			case 7: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2); j |= (j>>3); k = bitbuf[i+1]; j |= (((-k)|k)<<26); if (j != -1) goto found; } break;
			case 8: ie >>= 5; for(i>>=5;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2); j |= (j>>4); k = bitbuf[i+1]; j |= (((-k)|k)<<25); if (j != -1) goto found; } break;
#else
				//NOTE:changing to 64-bit had absolutely no effect on initboard() speed :/
			case 1: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i];                                                                              if (j != LL(-1)) goto found; } break;
			case 2: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1);                           k = bitbuf[i+1]; j |= (      k <<63); if (j != LL(-1)) goto found; } break;
			case 3: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1)|(j>>2);                    k = bitbuf[i+1]; j |= (((-k)|k)<<62); if (j != LL(-1)) goto found; } break;
			case 4: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2);              k = bitbuf[i+1]; j |= (((-k)|k)<<61); if (j != LL(-1)) goto found; } break;
			case 5: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2)|(j>>3);       k = bitbuf[i+1]; j |= (((-k)|k)<<60); if (j != LL(-1)) goto found; } break;
			case 6: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2)|(j>>4);       k = bitbuf[i+1]; j |= (((-k)|k)<<59); if (j != LL(-1)) goto found; } break;
			case 7: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2); j |= (j>>3); k = bitbuf[i+1]; j |= (((-k)|k)<<58); if (j != LL(-1)) goto found; } break;
			case 8: ie >>= 6; for(i>>=6;i<ie;i++) { j = bitbuf[i]; j |= (j>>1); j |= (j>>2); j |= (j>>4); k = bitbuf[i+1]; j |= (((-k)|k)<<57); if (j != LL(-1)) goto found; } break;
#endif
			default:
				for(;i<ie;i=i1+1) //NOTE:this seems to be faster than the above cases
				{
					i0 = dntil0((unsigned int *)bitbuf,i   ,ie  ); if (i0 >= ie) break;
					i1 = dntil1((unsigned int *)bitbuf,i0+1,i0+n); if (i1-i0 < n) continue;
					setzrange1(bitbuf,i0,i0+n); bm->ind = i0+n; return(i0);
				}
				break;
		}
		cnt--; if (cnt < 0) break;
		i = 0; ie = min(oi,bm->mal-n);
	}

	i = bm->mal;

	if ((oct_usegpu) && (&loct->sur == bm)) //Only surf needs to do GPU stuff :P
	{
			//grow space by 100%
#if (GPUSEBO != 0)
		if (loct->gsurf)
		{
			loct->gsurf = 0;
			glBindTexture(GL_TEXTURE_2D,loct->octid);
			bo_end(loct->bufid,0,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
		}
		((PFNGLDELETEBUFFERS)glfp[glDeleteBuffers])(1,&loct->bufid);
#endif
		if (loct->glxsid < loct->glysid) loct->glxsid++; else loct->glysid++;
		loct->gxsid = (1<<loct->glxsid);
		loct->gysid = (1<<loct->glysid);
		bm->mal = loct->gxsid*loct->gysid/(sizeof(surf_t)>>2);
		kglalloctex(loct->octid,0,loct->gxsid,loct->gysid,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here!
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(i*2)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)bm->buf);
#else
		loct->bufid = bo_init(loct->gxsid*loct->gysid*4);
		loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
		memcpy(loct->gsurf,bm->buf,i*bm->siz);
#endif
	}
	else { bm->mal = max(((1<<bsr(bm->mal))>>2) + bm->mal,bm->mal+n); } //grow space by ~25%

	bm->buf = (octv_t       *)realloc(bm->buf,   (__int64)bm->mal*bm->siz);        if (!bm->buf) { char tbuf[256]; sprintf(tbuf,"realloc(bm->buf,%I64d) failed",   (__int64)bm->mal*bm->siz);        MessageBox(ghwnd,tbuf,prognam,MB_OK); }
	bm->bit = (unsigned int *)realloc(bm->bit,((((__int64)bm->mal+63)>>5)<<2)+16); if (!bm->bit) { char tbuf[256]; sprintf(tbuf,"realloc(bm->bit,%I64d) failed",((((__int64)bm->mal+63)>>5)<<2)+16); MessageBox(ghwnd,tbuf,prognam,MB_OK); }

	setzrange1(bm->bit,i  ,i+n);
	setzrange0(bm->bit,i+n,bm->mal);
	bm->ind = i+n;
	return(i);

found:
#ifndef _WIN64
	i = (i<<5)+bsf(~j);
#else
	i = (i<<6)+bsf64(~j);
#endif
	xorzrangesmall(bitbuf,i,n); bm->ind = i; return(i);
}

void oct_free (oct_t *loct)
{
#if (GPUSEBO != 0)
	if (oct_usegpu)
	{
		if (loct->gsurf)
		{
			loct->gsurf = 0;
			glBindTexture(GL_TEXTURE_2D,loct->octid);
			bo_end(loct->bufid,0,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
		}
		((PFNGLDELETEBUFFERS)glfp[glDeleteBuffers])(1,&loct->bufid);
	}
#endif
	if (loct->sur.bit) free(loct->sur.bit);
	if (loct->sur.buf) free(loct->sur.buf);
	if (loct->nod.bit) free(loct->nod.bit);
	if (loct->nod.buf) free(loct->nod.buf);
	memset(loct,0,sizeof(oct_t));
}

void oct_dup (oct_t *ooct, oct_t *noct)
{
	int i;

	memcpy(noct,ooct,sizeof(oct_t));

	i = noct->nod.mal*noct->nod.siz;
	noct->nod.buf = (octv_t *)malloc(i);
	memcpy(noct->nod.buf,ooct->nod.buf,i);

	i = (((noct->nod.mal+63)>>5)<<2)+16;
	noct->nod.bit = (unsigned int *)malloc(i);
	memcpy(noct->nod.bit,ooct->nod.bit,i);


	i = noct->sur.mal*noct->sur.siz;
	noct->sur.buf = (octv_t *)malloc(i);
	memcpy(noct->sur.buf,ooct->sur.buf,i);

	i = (((noct->sur.mal+63)>>5)<<2)+16;
	noct->sur.bit = (unsigned int *)malloc(i);
	memcpy(noct->sur.bit,ooct->sur.bit,i);

	if (oct_usegpu)
	{
		glGenTextures(1,&noct->octid);
		kglalloctex(noct->octid,0,noct->gxsid,noct->gysid,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here!
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,noct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,noct->gxsid,(noct->sur.mal*2)>>noct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)noct->sur.buf);
#else
		noct->bufid = bo_init(noct->gxsid*noct->gysid*4);
		noct->gsurf = (surf_t *)bo_begin(noct->bufid,0);
		memcpy(noct->gsurf,noct->sur.buf,noct->sur.mal*noct->sur.siz);
#endif
	}
}

void oct_new (oct_t *loct, int los, INT_PTR tilid, int startnodes, int startsurfs, int hax4mark2spr)
{
	int i;
	octv_t *ptr;

	loct->head = 0;
	loct->lsid = los; loct->sid = (1<<los); loct->nsid = -loct->sid;
	loct->flags = 0;
	loct->edgeiswrap = 0;
	loct->edgeissol = 0;

	loct->recvoctfunc = 0;

		// octvn,64rndsph: model:
		// 1:      9       8*4^1  32
		// 2:     65       8*4^2  128
		// 3:    361       8*4^3  512
		// 4:    172       8*4^4  2048
		// 5:   3434       8*4^5  8192
		// 6:  20131       8*4^6  32768
		// 7: 103732       8*4^7  131072
		// 8: 474610       8*4^8  524288
		// 9:1841536       8*4^9  2097152
		//10:7417321       8*4^10 8388608

	if (startnodes) loct->nod.mal = startnodes; else if (los <= 10) loct->nod.mal = (8<<(los<<1)); else loct->nod.mal = 16777216;
	loct->nod.siz = sizeof(octv_t);
	loct->nod.buf = malloc(loct->nod.mal*loct->nod.siz);
	ptr = (octv_t *)loct->nod.buf;
	ptr[0].chi = 0;
	ptr[0].sol = 0;
	ptr[0].mrk = 0;
	ptr[0].mrk2= 0;
	ptr[0].ind =-1;
	loct->nod.num = 1; loct->nod.ind = 1;
	if (!hax4mark2spr)
	{
		i = (((loct->nod.mal+63)>>5)<<2)+16;
		loct->nod.bit = (unsigned int *)malloc(i);
		memset4(loct->nod.bit,0,i); loct->nod.bit[0] = 1;
	}

	if (startsurfs) loct->sur.mal = startsurfs; else if (los <= 10) loct->sur.mal = (8<<(los<<1)); else loct->sur.mal = 16777216;
	loct->sur.siz = sizeof(surf_t);
	if (oct_usegpu)
	{
		i = bsr(loct->sur.mal-1)+1;
		i += bsr((loct->sur.siz>>2)-1)+1;
		loct->glxsid = ((i+0)>>1);
		loct->glysid = ((i+1)>>1);
		loct->gxsid = (1<<loct->glxsid);
		loct->gysid = (1<<loct->glysid);
		loct->sur.mal = loct->gxsid*loct->gysid/(loct->sur.siz>>2);

		loct->tilid = tilid;
		glGenTextures(1,&loct->octid);
		loct->gsurf = 0;
		if (!hax4mark2spr)
		{
			kglalloctex(loct->octid,0,loct->gxsid,loct->gysid,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here!
#if (GPUSEBO != 0)
			loct->bufid = bo_init(loct->gxsid*loct->gysid*4);
#endif
		}
	}
	loct->sur.buf = (octv_t *)malloc(loct->sur.mal*loct->sur.siz);
	loct->sur.num = 1; loct->sur.ind = 1;
	if (!hax4mark2spr)
	{
		i = (((loct->sur.mal+63)>>5)<<2)+16;
		loct->sur.bit = (unsigned int *)malloc(i);
		memset4(loct->sur.bit,0,i); loct->sur.bit[0] = 1; //Can't use index 0 since it is used as background color in GLSL shaders.
	}
}

	//returns: 0:air, 1:surf (c valid), 2:interior
int oct_getvox (oct_t *loct, int x, int y, int z, surf_t **surf)
{
	octv_t *ptr;
	int i, s;

	if ((x|y|z)&loct->nsid) return(0);
	for(i=loct->head,s=(loct->sid>>1);1;s>>=1)
	{
		ptr = &((octv_t *)loct->nod.buf)[i]; //2child
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (!(ptr->chi&i)) { if (ptr->sol&i) return(2); return(0); }
		i = popcount[ptr->chi&(i-1)] + ptr->ind; //2child
		if (s <= 1) { if (surf) (*surf) = &((surf_t *)loct->sur.buf)[i]; return(1); }
	}
}

	//faster than oct_getvox() if only sol vs. air is needed (doesn't find surface pointer)
	//returns: 0:air, 1:sol (surface or interior)
int oct_getsol (oct_t *loct, int x, int y, int z)
{
	octv_t *ptr;
	int i, s;

	if ((x|y|z)&loct->nsid) return(0);
	for(i=loct->head,s=(loct->sid>>1);1;s>>=1)
	{
		ptr = &((octv_t *)loct->nod.buf)[i]; //2child
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (ptr->sol&i) return(1);
		if ((!(ptr->chi&i)) || (s <= 1)) return(0);
		i = popcount[ptr->chi&(i-1)] + ptr->ind; //2child
	}
}

	//returns: -1:invalid, {0..sur.mal-1}:surface index
int oct_getsurf (oct_t *loct, int x, int y, int z)
{
	octv_t *ptr;
	int i, s;

	if ((x|y|z)&loct->nsid) return(-1);
	for(i=loct->head,s=(loct->sid>>1);1;s>>=1)
	{
		ptr = &((octv_t *)loct->nod.buf)[i]; //2child
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (!(ptr->chi&i)) return(-1);
		i = popcount[ptr->chi&(i-1)] + ptr->ind; //2child
		if (s <= 1) return(i);
	}
}

void oct_getvox_hint_init (oct_t *loct, oct_getvox_hint_t *och)
	{ och->stkind[loct->lsid-1] = loct->head; och->minls = loct->lsid-1; och->mins = (loct->sid>>1); och->ox = 0; och->oy = 0; och->oz = 0; }
int oct_getsol_hint (oct_t *loct, int x, int y, int z, oct_getvox_hint_t *och) //WARNING:assumes x,y,z inside grid
{
	octv_t *ptr;
	int i, ls, s;

	ls = bsr((och->ox^x)|(och->oy^y)|(och->oz^z)|och->mins); s = pow2[ls];
	for(;1;s>>=1,ls--)
	{
		ptr = &((octv_t *)loct->nod.buf)[och->stkind[ls]];
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (ptr->sol&i) break;
		if ((!(ptr->chi&i)) || (!ls)) { i = 0; break; }
		och->stkind[ls-1] = popcount[ptr->chi&(i-1)] + ptr->ind; //2child
	}
	och->minls = ls; och->mins = s; och->ox = x; och->oy = y; och->oz = z; return(i);
}

	//returns sol mask of 2x2x2 cell (optimization for smallest node)
static __forceinline int oct_getsol_hint_2x2x2 (oct_t *loct, int x, int y, int z, oct_getvox_hint_t *och) //WARNING:assumes x,y,z even #'s
{
	octv_t *ptr;
	int i, ls, s;

	ls = (och->ox^x)|(och->oy^y)|(och->oz^z); if (ls&loct->nsid) return(0);
	ls = bsr(ls|och->mins); s = pow2[ls];
	for(;1;s>>=1,ls--)
	{
		ptr = &((octv_t *)loct->nod.buf)[och->stkind[ls]];
		if (!ls) { i = ptr->sol; break; }
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (ptr->sol&i) { i = (1<<8)-1; break; }
		if (!(ptr->chi&i)) { i = 0; break; }
		och->stkind[ls-1] = popcount[ptr->chi&(i-1)] + ptr->ind; //2child
	}
	och->minls = ls; och->mins = s; och->ox = x; och->oy = y; och->oz = z; return(i);
}

	//returns: -1:invalid, {0..sur.mal-1}:surface index
int oct_getsurf_hint (oct_t *loct, int x, int y, int z, oct_getvox_hint_t *och)
{
	octv_t *ptr;
	int i, ls, s;

	if ((x|y|z)&loct->nsid) return(-1);
	ls = bsr((och->ox^x)|(och->oy^y)|(och->oz^z)|och->mins); s = pow2[ls];
	for(;1;s>>=1,ls--)
	{
		ptr = &((octv_t *)loct->nod.buf)[och->stkind[ls]];
		i = 1;
		if (x&s) i <<= 1;
		if (y&s) i <<= 2;
		if (z&s) i <<= 4;
		if (!(ptr->chi&i)) { i = -1; break; }
		i = popcount[ptr->chi&(i-1)] + ptr->ind;
		if (!ls) break;
		och->stkind[ls-1] = i; //2child
	}
	och->minls = ls; och->mins = s; och->ox = x; och->oy = y; och->oz = z; return(i);
}

	//02/10/2012: Adds round values to x&y as if they form a morton code; tracks carry bits using tricks to the extreme! :)
	//* s is side length of square (area) to add (must be pow2)
	//* assumes: s is pow2, (x%s) == 0, (y%s) == 0
	//
	//Algo: find next 0 bit, starting from s, and using morton order on mx&my
	//   Performs ripple carry manually. x/y with 1st 0 is summed with s; other gets bits cleared up to that point.
	//
	//Ex#1:                                  | Ex#2:
	//   000000000100 s                      |    000000000100 s
	//   011010111100 x  ;clear bits 5-0     |    0110101o1100 x  ;add s
	//   001111o11100 y  ;add s              |    001111011100 y  ;clear bits 3-0
	//                                       |
	//   011011000000 x+s                    |    011010110000 x+s
	//   001111100000 y+s                    |    001111100000 y+s
	//   100101000000 -(x+s)                 |    100101010000 -(x+s)
	//   110000100000 -(y+s)                 |    110000100000 -(y+s)
	//
	//   111111000000 (-(x+s))|(x+s)         |    111111110000 (-(x+s))|(x+s)
	//   111111000000 (-(y+s))^(y+s)         |    111111000000 (-(y+s))^(y+s)
	//
#define mort2add(s,mx,my)\
{\
	int _x, _y;\
	(mx) += (s); _x = (-(mx))|(mx);\
	(my) += (s); _y = (-(my))^(my);\
	if (_y >= _x) (mx) += _y; else (my) += _x;\
}

static int oct_isboxanyair (oct_t *loct, int x0, int y0, int z0, int s, octv_t *ptr, int bx, int by, int bz, int bs)
{
	octv_t *octv;
	int i, x, y, z, v;

	v = ptr->sol^((1<<8)-1);
	x = x0+s-bx; if (x <= 0) v &=~0x55; else if (x >= bs) v &=~0xaa;
	y = y0+s-by; if (y <= 0) v &=~0x33; else if (y >= bs) v &=~0xcc;
	z = z0+s-bz; if (z <= 0) v &=~0x0f; else if (z >= bs) v &=~0xf0;
	if (v&~ptr->chi) return(1);
	if (s == 1) return(0);
	octv = (octv_t *)loct->nod.buf;
	if ((v&(1<<0)) && (oct_isboxanyair(loct,x0  ,y0  ,z0  ,s>>1,&octv[popcount[ptr->chi&((1<<0)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<1)) && (oct_isboxanyair(loct,x0+s,y0  ,z0  ,s>>1,&octv[popcount[ptr->chi&((1<<1)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<2)) && (oct_isboxanyair(loct,x0  ,y0+s,z0  ,s>>1,&octv[popcount[ptr->chi&((1<<2)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<3)) && (oct_isboxanyair(loct,x0+s,y0+s,z0  ,s>>1,&octv[popcount[ptr->chi&((1<<3)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<4)) && (oct_isboxanyair(loct,x0  ,y0  ,z0+s,s>>1,&octv[popcount[ptr->chi&((1<<4)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<5)) && (oct_isboxanyair(loct,x0+s,y0  ,z0+s,s>>1,&octv[popcount[ptr->chi&((1<<5)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<6)) && (oct_isboxanyair(loct,x0  ,y0+s,z0+s,s>>1,&octv[popcount[ptr->chi&((1<<6)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	if ((v&(1<<7)) && (oct_isboxanyair(loct,x0+s,y0+s,z0+s,s>>1,&octv[popcount[ptr->chi&((1<<7)-1)]+ptr->ind],bx,by,bz,bs))) return(1);
	return(0);
}

#if (MARCHCUBE == 0)

	//Function assumes (x,y,z,s) already known to not be pure air
	//returns 1 if any voxels in cube (x,y,z,s) or its 1 voxel neighbors are air
static int oct_issurf (oct_t *loct, int x, int y, int z, int ls, oct_getvox_hint_t *och)
{
	int xx, yy, zz, s, e;

	s = pow2[ls]; e = loct->sid-s;

	if ((!(loct->edgeissol& 1)) && (x == 0)) return(1);
	if ((!(loct->edgeissol& 2)) && (y == 0)) return(1);
	if ((!(loct->edgeissol& 4)) && (z == 0)) return(1);
	if ((!(loct->edgeissol& 8)) && (x == e)) return(1);
	if ((!(loct->edgeissol&16)) && (y == e)) return(1);
	if ((!(loct->edgeissol&32)) && (z == e)) return(1);

		//test cube; if interior's not pure solid, it must contain surfs
	if ((!oct_getsol_hint(loct,x,y,z,och)) || (och->minls < ls)) return(1); //NOTE:och->mins must be compared AFTER oct_getsol_hint() call!

		//Fast&elegant algo! :)
	if (x != 0) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x- 1,y+xx,z+yy,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }
	if (x != e) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x+ s,y+xx,z+yy,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }
	if (y != 0) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x+xx,y- 1,z+yy,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }
	if (y != e) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x+xx,y+ s,z+yy,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }
	if (z != 0) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x+xx,y+yy,z- 1,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }
	if (z != e) { xx = 0; yy = 0; do { if (!oct_getsol_hint(loct,x+xx,y+yy,z+ s,och)) return(1); mort2add(och->mins,xx,yy); } while (xx < s); }

	return(0);
}

#else

	//Function assumes (x,y,z,s) already known to not be pure air
	//returns 1 if any voxels in cube (x,y,z,s) or its 1 voxel neighbors are air
static int oct_issurf (oct_t *loct, int x, int y, int z, int ls, oct_getvox_hint_t *och)
{
	int s, e;

	s = pow2[ls]; e = loct->sid-s;

	if ((!(loct->edgeissol& 1)) && (x == 0)) return(1);
	if ((!(loct->edgeissol& 2)) && (y == 0)) return(1);
	if ((!(loct->edgeissol& 4)) && (z == 0)) return(1);
	if ((!(loct->edgeissol& 8)) && (x == e)) return(1);
	if ((!(loct->edgeissol&16)) && (y == e)) return(1);
	if ((!(loct->edgeissol&32)) && (z == e)) return(1);

	x--; y--; z--; s += 2;
	ls = bsr(((x+s-1)^x)|((y+s-1)^y)|((z+s-1)^z)|och->mins);
	return(oct_isboxanyair(loct,0,0,0,pow2[ls],&((octv_t *)loct->nod.buf)[och->stkind[ls]],x,y,z,s));
	//return(!oct_getboxstate(loct,x,y,z,x+s,y+s,z+s));
}

#endif

int oct_findsurfdowny (oct_t *loct, int ox, int oy, int oz, surf_t **surf)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, ls, x, y, z, nx, ny, nz;

	ls = loct->lsid-1; ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 0;
	while (1)
	{
		i = (1<<j); if (!(ptr->chi&i)) goto tosibly;

		nx = (( j    &1)<<ls)+x; if ((nx+(1<<ls)-1 < ox) || (nx > ox)) goto tosibly;
		ny = (((j>>1)&1)<<ls)+y; if (ny+(1<<ls)-1 <= oy) goto tosibly;
		nz = (((j>>2)&1)<<ls)+z; if ((nz+(1<<ls)-1 < oz) || (nz > oz)) goto tosibly;

		if (ls <= 0) { (*surf) = &((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; return(ny); }

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 0;
		continue;

tosibly:;
		j++; if (j < 8) continue;
		do { ls++; if (ls >= loct->lsid) return(loct->sid); j = stk[ls].j+1; } while (j >= 8); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}
}

void oct_setvox (oct_t *loct, int x, int y, int z, surf_t *surf, int mode)
{
	brush_vox_t vox;
	brush_vox_init(&vox,x,y,z,surf); oct_mod(loct,(brush_t *)&vox,mode);
}

	//Generates bit cube with bx0,by0,bz0 as top-left-front
	//lsdax: least significant dimension axis (-=mirrored): 1=x, 2=y, 3=z, -1=-x, -2=-y, -3=-z
	//NOTE! It is assumed that bitvis allocation x size is rounded up to next multiple of 32
void oct_sol2bit (oct_t *loct, unsigned int *bitvis, int bx0, int by0, int bz0, int dx, int dy, int dz, int lsdax)
{
	typedef struct { octv_t *ptr; int j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, ls, s, x, y, z, nx, ny, nz, x0, y0, z0, x1, y1, z1, xx, yy, zz, pit, isneg;

		//obtain bit array to calculate visibility info quickly
	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head];
	x = -bx0; y = -by0; z = -bz0; j = 8-1;
	if (lsdax < 0) { isneg = 1; lsdax = -lsdax; } else isneg = 0;
	switch(lsdax)
	{
		case 1: pit = ((dx+31)>>5); memset(bitvis,0,pit*dy*dz*sizeof(bitvis[0])); break;
		case 2: pit = ((dy+31)>>5); memset(bitvis,0,dx*pit*dz*sizeof(bitvis[0])); break;
		case 3: pit = ((dz+31)>>5); memset(bitvis,0,dx*dy*pit*sizeof(bitvis[0])); break;
	}
	while (1)
	{
		nx = x; if (j&1) nx += s; if ((nx >= dx) || (nx+s <= 0)) goto tosibly;
		ny = y; if (j&2) ny += s; if ((ny >= dy) || (ny+s <= 0)) goto tosibly;
		nz = z; if (j&4) nz += s; if ((nz >= dz) || (nz+s <= 0)) goto tosibly;

		i = (1<<j);
		if (ptr->sol&i)
		{
			switch (lsdax)
			{
				case 1:
					if (isneg) nx = dx-s-nx;
					if (dx <= 32)
					{
						if (!ls) bitvis[nz*dy+ny] += (1<<nx);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx); x0 = (-1<<x0)&~(-2<<(x1-1)); //NOTE:..&~(-1<<x1) gives wrong result when x1==32
							y0 = max(ny,0); y1 = min(ny+s,dy);
							z0 = max(nz,0); z1 = min(nz+s,dz);
							for(zz=z0;zz<z1;zz++) for(yy=y0;yy<y1;yy++) bitvis[zz*dy+yy] += x0;
						}
					}
					else
					{
						if (!ls) bitvis[(nz*dy+ny)*pit + (nx>>5)] += (1<<nx);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx);
							y0 = max(ny,0); y1 = min(ny+s,dy);
							z0 = max(nz,0); z1 = min(nz+s,dz);
							for(zz=z0;zz<z1;zz++) for(yy=y0;yy<y1;yy++) setzrange1(&bitvis[(zz*dy+yy)*pit],x0,x1);
						}
					}
					break;
				case 2:
					if (isneg) ny = dy-s-ny;
					if (dy <= 32)
					{
						if (!ls) bitvis[nz*dx+nx] += (1<<ny);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx);
							y0 = max(ny,0); y1 = min(ny+s,dy); y0 = (-1<<y0)&~(-2<<(y1-1)); //NOTE:..&~(-1<<y1) gives wrong result when y1==32
							z0 = max(nz,0); z1 = min(nz+s,dz);
							for(zz=z0;zz<z1;zz++) for(xx=x0;xx<x1;xx++) bitvis[zz*dx+xx] += y0;
						}
					}
					else
					{
						if (!ls) bitvis[(nz*dx+nx)*pit + (ny>>5)] += (1<<ny);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx);
							y0 = max(ny,0); y1 = min(ny+s,dy);
							z0 = max(nz,0); z1 = min(nz+s,dz);
							for(zz=z0;zz<z1;zz++) for(xx=x0;xx<x1;xx++) setzrange1(&bitvis[(zz*dx+xx)*pit],y0,y1);
						}
					}
					break;
				case 3:
					if (isneg) nz = dz-s-nz;
					if (dz <= 32)
					{
						if (!ls) bitvis[ny*dx+nx] += (1<<nz);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx);
							y0 = max(ny,0); y1 = min(ny+s,dy);
							z0 = max(nz,0); z1 = min(nz+s,dz); z0 = (-1<<z0)&~(-2<<(z1-1)); //NOTE:..&~(-1<<z1) gives wrong result when z1==32
							for(yy=y0;yy<y1;yy++) for(xx=x0;xx<x1;xx++) bitvis[yy*dx+xx] += z0;
						}
					}
					else
					{
						if (!ls) bitvis[(ny*dx+nx)*pit + (nz>>5)] += (1<<nz);
						else
						{
							x0 = max(nx,0); x1 = min(nx+s,dx);
							y0 = max(ny,0); y1 = min(ny+s,dy);
							z0 = max(nz,0); z1 = min(nz+s,dz);
							for(yy=y0;yy<y1;yy++) for(xx=x0;xx<x1;xx++) setzrange1(&bitvis[(yy*dx+xx)*pit],z0,z1);
						}
					}
					break;
			}
			goto tosibly;
		}

		if (ptr->chi&i) //Recurse only if mixture of air&sol
		{
			stk[ls].ptr = ptr; stk[ls].j = j; ls--; s >>= 1; //2child
			ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
			continue;
		}

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) return; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; i = s*-2;
		x = ((x+bx0)&i)-bx0;
		y = ((y+by0)&i)-by0;
		z = ((z+bz0)&i)-bz0;
	}
}

static void oct_surf2bit (oct_t *loct, unsigned int *bitsurf, int bx0, int by0, int bz0, int dx, int dy, int dz)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, ls, s, x, y, z, nx, ny, nz, x0, y0, z0, x1, y1, z1, xx, yy, zz, pit;

		//obtain bit array to calculate visibility info quickly
	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head];
	x = -bx0; y = -by0; z = -bz0; j = 8-1;
	pit = ((dx+31)>>5); memset(bitsurf,0,pit*dy*dz*sizeof(bitsurf[0]));
	while (1)
	{
		nx = (( j    &1)<<ls)+x; if ((nx >= dx) || (nx+s <= 0)) goto tosibly;
		ny = (((j>>1)&1)<<ls)+y; if ((ny >= dy) || (ny+s <= 0)) goto tosibly;
		nz = (((j>>2)&1)<<ls)+z; if ((nz >= dz) || (nz+s <= 0)) goto tosibly;

		i = (1<<j);

		if (ptr->chi&i) //Recurse only if surface
		{
			if (ls)
			{
				stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; s >>= 1; //2child
				ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
				continue;
			}
			bitsurf[(nz*dy + ny)*pit + (nx>>5)] |= (1<<nx);
		}

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) return; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}
}

static int oct_touch_brush_rec (oct_t *loct, octv_t *inode, int x0, int y0, int z0, int ls, brush_t *brush)
{
	int i, j, x, y, z, s, ind;

	ls--; s = (1<<ls); ind = inode->ind;
	for(i=1,z=0;z<=s;z+=s)
		for(y=0;y<=s;y+=s)
			for(x=0;x<=s;x+=s,i<<=1)
			{
				if (!((inode->chi|inode->sol)&i)) continue;
				j = brush->isins(brush,x+x0,y+y0,z+z0,ls);
				if (inode->chi&i)       //FIX:this works because at least 1 node approaches small size.
				//if (!(inode->sol&i))  //FIX:this fails due to approximation of bbox vs. bbox. (see quadtre3.c:see oct_touch_oct_recur() for potential solution)
				{
					if (j == 2) return(1);
					if (j == 1)
					{
						if (s <= 1) return(1); //NOTE:for safety only - a properly written brush->isins function should never return 1 for ls==0
						if (oct_touch_brush_rec(loct,&((octv_t *)loct->nod.buf)[ind],x+x0,y+y0,z+z0,ls,brush)) return(1);
					}
					ind++;
				}
				else if (j) return(1);
			}
	return(0);
}
int oct_touch_brush (oct_t *loct, brush_t *brush)
	{ return(oct_touch_brush_rec(loct,&((octv_t *)loct->nod.buf)[loct->head],0,0,0,loct->lsid,brush)); }

static __forceinline int popcount32 (int i)
	{ i -= ((i>>1)&0x55555555); i = ((i>>2)&0x33333333)+(i&0x33333333); return(((((i>>4)+i)&0xf0f0f0f)*0x01010101)>>24); }

	//Requires SSE2 :/
static __forceinline int popcount128 (void *buf)
{
	__declspec(align(16)) static const int dqzeros[4] = {0,0,0,0};
	__declspec(align(16)) static const int dq5555[4] = {0x55555555,0x55555555,0x55555555,0x55555555};
	__declspec(align(16)) static const int dq3333[4] = {0x33333333,0x33333333,0x33333333,0x33333333};
	__declspec(align(16)) static const int dq0f0f[4] = {0x0f0f0f0f,0x0f0f0f0f,0x0f0f0f0f,0x0f0f0f0f};

	_asm
	{
			;input: xmm0
		mov eax, buf
		movdqu xmm0, [eax]
		movaps xmm1, xmm0
		psrld xmm0, 1
		pand xmm0, dq5555
		psubd xmm1, xmm0
		movaps xmm0, xmm1
		psrld xmm1, 2
		pand xmm0, dq3333
		pand xmm1, dq3333
		paddd xmm0, xmm1
		movaps xmm1, xmm0
		psrld xmm0, 4
		paddd xmm0, xmm1
		pand xmm0, dq0f0f
		psadbw xmm0, dqzeros
			;output: xmm0

		movhlps xmm1, xmm0
		paddd xmm0, xmm1
		movd eax, xmm0
	}
}

#define COMPACT_LBLKSIZ 5 //# longs per block (4 r's: (3:1.43ms, 4:1.63ms, 5:1.94ms, 6:2.51ms, 7:3.68ms, 8:6.15ms, 9:10.69ms, 10:20.04ms))
static void oct_compact (oct_t *loct, int issur) //issur==0:nod, issur==1:sur
{
	bitmal_t *bm;
	int i, j, k, ls, ptr, optr, num0, *popcountlut, stkptr[OCT_MAXLS], stknum[OCT_MAXLS];

	if (!((octv_t *)loct->nod.buf)[loct->head].chi) return; //don't compact empty oct_t (needed to prevent crash in loop later)
	if (!issur) bm = &loct->nod; else bm = &loct->sur;

	popcountlut = (int *)malloc(((bm->mal>>(COMPACT_LBLKSIZ+5))+1)*sizeof(int));
	for(i=0,k=0;i<(bm->mal>>5);i+=(1<<COMPACT_LBLKSIZ))
	{
		popcountlut[i>>COMPACT_LBLKSIZ] = k;
		//for(j=(1<<COMPACT_LBLKSIZ)-1;j>=0;j--) k += popcount32(bm->bit[i+j]);
		for(j=(1<<COMPACT_LBLKSIZ)-4;j>=0;j-=4) k += popcount128(&bm->bit[i+j]);
	}

		//Subtract appropriate amount (# holes preceding address) from all ptr's
	ls = loct->lsid-1; stkptr[ls] = loct->head; stknum[ls] = 1;
	while (1)
	{
		ptr = stkptr[ls]; stkptr[ls]++; stknum[ls]--; //2sibly
		if (ls >= 0)
		{
			optr = ((octv_t *)loct->nod.buf)[ptr].ind;
			if ((!issur) == (ls != 0))
			{
					//popcount; uses LUT value every (1<<COMPACT_LBLKSIZ) longs
				i = optr>>5; j = i&(-1<<COMPACT_LBLKSIZ); num0 = popcount32(bm->bit[i]|(-1<<optr)) + popcountlut[j>>COMPACT_LBLKSIZ];
				while (i&3) { i -= 1; num0 += popcount32 ( bm->bit[i]); } //Even faster; requires SSE2
				while (i>j) { i -= 4; num0 += popcount128(&bm->bit[i]); }
				num0 = ((optr&~31)+32) - num0;

				((octv_t *)loct->nod.buf)[ptr].ind = optr-num0;
			}
			if (ls > 0) { ls--; stkptr[ls] = optr; stknum[ls] = popcount[((octv_t *)loct->nod.buf)[ptr].chi]; } //2child
		}
		while (stknum[ls] <= 0) { ls++; if (ls >= loct->lsid) goto break2; } //2parent
	}
break2:;

		//shift all nodes towards index 0
	if (!issur)
	{
		for(k=0,i=dntil1(bm->bit,0,bm->mal);i<bm->mal;k+=j-i,i=dntil1(bm->bit,j+1,bm->mal)) //faster
			{ j = dntil0(bm->bit,i+1,bm->mal); memmove(&((octv_t *)bm->buf)[k],&((octv_t *)bm->buf)[i],(j-i)*bm->siz); }
	}
	else
	{
		for(k=0,i=dntil1(bm->bit,0,bm->mal);i<bm->mal;k+=j-i,i=dntil1(bm->bit,j+1,bm->mal)) //faster
			{ j = dntil0(bm->bit,i+1,bm->mal); memmove(&((surf_t *)bm->buf)[k],&((surf_t *)bm->buf)[i],(j-i)*bm->siz); }
	}

	setzrange1(bm->bit,      0,bm->num);
	setzrange0(bm->bit,bm->num,bm->mal);
	bm->ind = bm->num;

	free(popcountlut);

	if (oct_usegpu)
	{
		if (issur)
		{
#if (GPUSEBO == 0)
			glBindTexture(GL_TEXTURE_2D,loct->octid);
			glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.num*2+loct->gxsid-1)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
#else
			if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
			memcpy(loct->gsurf,loct->sur.buf,loct->sur.num*loct->sur.siz);
#endif
		}
	}
}
static void oct_checkreducesizes (oct_t *loct)
{
	int i;

	if ((loct->nod.num < (loct->nod.mal>>2)) && (loct->nod.mal > 256))
	{
		oct_compact(loct,0);
		loct->nod.mal >>= 1; //halve space
		loct->nod.buf = (octv_t       *)realloc(loct->nod.buf,   (__int64)loct->nod.mal*loct->nod.siz);
		loct->nod.bit = (unsigned int *)realloc(loct->nod.bit,((((__int64)loct->nod.mal+63)>>5)<<2)+16);
	}
	if ((loct->sur.num < (loct->sur.mal>>2)) && (loct->sur.mal > 256))
	{
		oct_compact(loct,1);
		loct->sur.mal >>= 1; //halve space
		loct->sur.buf = (octv_t       *)realloc(loct->sur.buf,   (__int64)loct->sur.mal*loct->sur.siz);
		loct->sur.bit = (unsigned int *)realloc(loct->sur.bit,((((__int64)loct->sur.mal+63)>>5)<<2)+16);
		if (oct_usegpu)
		{
#if (GPUSEBO != 0)
			if (loct->gsurf)
			{
				loct->gsurf = 0;
				glBindTexture(GL_TEXTURE_2D,loct->octid);
				bo_end(loct->bufid,0,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
			}
			((PFNGLDELETEBUFFERS)glfp[glDeleteBuffers])(1,&loct->bufid);
#endif
			if (loct->glxsid > loct->glysid) loct->glxsid--; else loct->glysid--;
			loct->gxsid = (1<<loct->glxsid);
			loct->gysid = (1<<loct->glysid);
			loct->sur.mal = loct->gxsid*loct->gysid/(sizeof(surf_t)>>2);
			kglalloctex(loct->octid,0,loct->gxsid,loct->gysid,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here!
#if (GPUSEBO == 0)
			glBindTexture(GL_TEXTURE_2D,loct->octid);
			glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.num*2+loct->gxsid-1)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
#else
			loct->bufid = bo_init(loct->gxsid*loct->gysid*4);
			loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
			memcpy(loct->gsurf,loct->sur.buf,loct->sur.num*loct->sur.siz);
#endif
		}
	}
}

	//Handle case at edges of defined octree space
static int isins_func (oct_t *loct, brush_t *brush, int x0, int y0, int z0, int ls)
{
	if ((x0|y0|z0)&loct->nsid) return(0);
	return(brush->isins(brush,x0,y0,z0,ls));
}

static void getsurf_func (oct_t *loct, brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush->getsurf(brush,x0,y0,z0,surf);
	if (!(brush->flags&1)) return;
	if (x0 ==           0) { surf->norm[0] =+64; surf->norm[1] =  0; surf->norm[2] =  0; }
	if (y0 ==           0) { surf->norm[0] =  0; surf->norm[1] =+64; surf->norm[2] =  0; }
	if (z0 ==           0) { surf->norm[0] =  0; surf->norm[1] =  0; surf->norm[2] =+64; }
	if (x0 == loct->sid-1) { surf->norm[0] =-64; surf->norm[1] =  0; surf->norm[2] =  0; }
	if (y0 == loct->sid-1) { surf->norm[0] =  0; surf->norm[1] =-64; surf->norm[2] =  0; }
	if (z0 == loct->sid-1) { surf->norm[0] =  0; surf->norm[1] =  0; surf->norm[2] =-64; }
}

static void oct_dealloctree (oct_t *loct, int ls, int ind, int chi)
{
	int nbits;
	nbits = popcount[chi];
	if (ls <= 0) { xorzrangesmall(loct->sur.bit,ind,nbits); loct->sur.num -= nbits; return; }
	xorzrangesmall(loct->nod.bit,ind,nbits); loct->nod.num -= nbits;
	for(nbits--;nbits>=0;nbits--,ind++) oct_dealloctree(loct,ls-1,((octv_t *)loct->nod.buf)[ind].ind,((octv_t *)loct->nod.buf)[ind].chi);
}
	//Input: o=n(old), n={0..8}, ind=octree index, noct[0..n-1]
static int oct_refreshnode (oct_t *loct, int ind, int o, int n, octv_t *noct)
{
	bitmal_t *bm;
	bm = &loct->nod;

	if (n < o)
	{
		xorzrangesmall(bm->bit,ind+n,o-n); //shorten node
		if (!n) ind = -1;
		bm->num += n-o;
	}
	else if (n > o)
	{
		if (o) xorzrangesmall(bm->bit,ind,o); //delete node (if(o) prevents possible read fault if !o)
		ind = bitalloc(loct,bm,n); //alloc node
		bm->num += n-o;
	}
	if (n)
	{
		memcpy(&((octv_t *)bm->buf)[ind],noct,n*bm->siz);
	}
	return(ind);
}
	//Input: o=n(old), n={0..8}, ind=octree index, surf[0..n-1]
static int oct_refreshsurf (oct_t *loct, int ind, int o, int n, surf_t *surf)
{
	bitmal_t *bm;
	bm = &loct->sur;

	if (n < o)
	{
		xorzrangesmall(bm->bit,ind+n,o-n); //shorten node
		if (!n) ind = -1;
		bm->num += n-o;
	}
	else if (n > o)
	{
		if (o) xorzrangesmall(bm->bit,ind,o); //delete node (if(o) prevents possible read fault if !o)
		ind = bitalloc(loct,bm,n); //alloc node
		bm->num += n-o;
	}
	if (n)
	{
		//memcpy(&((octv_t *)bm->buf)[ind],surf,n*bm->siz);
		memcpy(&((surf_t *)bm->buf)[ind],surf,n*bm->siz); //FIXFIXFIXFIX::? ????
#if (GPUSEBO != 0)
		if (oct_usegpu) memcpy(&loct->gsurf[ind],surf,n*bm->siz);
#endif
	}
	return(ind);
}

static void oct_mod_recur (oct_t *loct, int inode, int x0, int y0, int z0, int ls, octv_t *roct, brush_t *brush, int issol)
{
	surf_t surf[8];
	octv_t ooct, noct[8];
	int i, iup, n, o, s, x, y, z, xsol;

	if (inode >= 0) { ooct = ((octv_t *)loct->nod.buf)[inode]; xsol = ooct.sol^issol; }
				  else { xsol = (1<<8)-1; ooct.chi = 0; ooct.sol = issol^xsol; ooct.ind = -1; ooct.mrk = 0; ooct.mrk2 = 0; }

	if (!ls)
	{
		roct->sol = ooct.sol; roct->mrk = 0; roct->mrk2 = 0;
#if (MARCHCUBE == 0)
#if 1
		if ((xsol&(1<<0)) && (isins_func(loct,brush,x0  ,y0  ,z0  ,0))) { roct->sol ^= (1<<0); } //roct->mrk ^= (1<<0); }
		if ((xsol&(1<<1)) && (isins_func(loct,brush,x0+1,y0  ,z0  ,0))) { roct->sol ^= (1<<1); } //roct->mrk ^= (1<<1); }
		if ((xsol&(1<<2)) && (isins_func(loct,brush,x0  ,y0+1,z0  ,0))) { roct->sol ^= (1<<2); } //roct->mrk ^= (1<<2); }
		if ((xsol&(1<<3)) && (isins_func(loct,brush,x0+1,y0+1,z0  ,0))) { roct->sol ^= (1<<3); } //roct->mrk ^= (1<<3); }
		if ((xsol&(1<<4)) && (isins_func(loct,brush,x0  ,y0  ,z0+1,0))) { roct->sol ^= (1<<4); } //roct->mrk ^= (1<<4); }
		if ((xsol&(1<<5)) && (isins_func(loct,brush,x0+1,y0  ,z0+1,0))) { roct->sol ^= (1<<5); } //roct->mrk ^= (1<<5); }
		if ((xsol&(1<<6)) && (isins_func(loct,brush,x0  ,y0+1,z0+1,0))) { roct->sol ^= (1<<6); } //roct->mrk ^= (1<<6); }
		if ((xsol&(1<<7)) && (isins_func(loct,brush,x0+1,y0+1,z0+1,0))) { roct->sol ^= (1<<7); } //roct->mrk ^= (1<<7); }
#else
		for(;xsol;xsol^=iup)
		{
			iup = (-xsol)&xsol;
			if (isins_func(loct,brush,(((iup&0xaa)+0xff)>>8)+x0,(((iup&0xcc)+0xff)>>8)+y0,(((iup&0xf0)+0xff)>>8)+z0,0)) { roct->sol ^= iup; } //roct->mrk ^= iup; }
		}
#endif
#else
		if (xsol&(1<<0)) { i = isins_func(loct,brush,x0  ,y0  ,z0  ,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<0); } }
		if (xsol&(1<<1)) { i = isins_func(loct,brush,x0+1,y0  ,z0  ,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<1); } }
		if (xsol&(1<<2)) { i = isins_func(loct,brush,x0  ,y0+1,z0  ,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<2); } }
		if (xsol&(1<<3)) { i = isins_func(loct,brush,x0+1,y0+1,z0  ,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<3); } }
		if (xsol&(1<<4)) { i = isins_func(loct,brush,x0  ,y0  ,z0+1,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<4); } }
		if (xsol&(1<<5)) { i = isins_func(loct,brush,x0+1,y0  ,z0+1,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<5); } }
		if (xsol&(1<<6)) { i = isins_func(loct,brush,x0  ,y0+1,z0+1,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<6); } }
		if (xsol&(1<<7)) { i = isins_func(loct,brush,x0+1,y0+1,z0+1,0); if ((i == 2) || ((i == 1) && issol)) { roct->sol ^= (1<<7); } }
#endif

		if (ooct.chi&~roct->sol)
		{
			roct->chi = ooct.chi&roct->sol;
			for(n=0,xsol=roct->chi;xsol;xsol^=iup,n++)
			{
				iup = (-xsol)&xsol;
				memcpy(&surf[n],&((surf_t *)loct->sur.buf)[popcount[(iup-1)&ooct.chi]+ooct.ind],loct->sur.siz); //copy existing surf
			}
			roct->ind = oct_refreshsurf(loct,ooct.ind,popcount[ooct.chi],n,surf);
		} else { roct->chi = ooct.chi; roct->ind = ooct.ind; }

		brush->mx0 = min(brush->mx0,x0); brush->mx1 = max(brush->mx1,x0+2);
		brush->my0 = min(brush->my0,y0); brush->my1 = max(brush->my1,y0+2);
		brush->mz0 = min(brush->mz0,z0); brush->mz1 = max(brush->mz1,z0+2);
		return;
	}

	xsol |= ooct.chi; roct->chi = 0; roct->sol = ~xsol&issol; roct->mrk = 0; roct->mrk2 = 0; //copy solid if already brush color
	o = ooct.ind; n = 0; s = pow2[ls];
	for(;xsol;xsol^=iup) //visit only nodes that may differ from brush color
	{
		iup = (-xsol)&xsol;
		x = x0; if (iup&0xaa) x += s;
		y = y0; if (iup&0xcc) y += s;
		z = z0; if (iup&0xf0) z += s;
		switch(isins_func(loct,brush,x,y,z,ls))
		{
			case 0: //octree node doesn't intersect brush:copy old tree
				roct->sol += (ooct.sol&iup);
				if (ooct.chi&iup) { roct->chi += iup; noct[n] = ((octv_t *)loct->nod.buf)[o]; o++; n++; }
				break;
			case 1:
				if (ooct.chi&iup) { i = o; o++; } //octree node intersects brush partially:must recurse
								 else { i =-1;      } //leaf; split pure node
				oct_mod_recur(loct,i,x,y,z,ls-1,&noct[n],brush,issol);
				//if (noct[n].mrk) roct->mrk |= iup;
				if (noct[n].sol == (1<<8)-1) roct->sol += iup;
				if (noct[n].chi || ((unsigned)(noct[n].sol-1) < (unsigned)((1<<8)-2))) { roct->chi += iup; n++; }
				break;
			case 2: //brush fully covers octree node:all brush
				roct->sol += (issol&iup);
				if (ooct.chi&iup) { oct_dealloctree(loct,ls-1,((octv_t *)loct->nod.buf)[o].ind,((octv_t *)loct->nod.buf)[o].chi); o++; }
				brush->mx0 = min(brush->mx0,x); brush->mx1 = max(brush->mx1,x+s);
				brush->my0 = min(brush->my0,y); brush->my1 = max(brush->my1,y+s);
				brush->mz0 = min(brush->mz0,z); brush->mz1 = max(brush->mz1,z+s);
				//roct->mrk |= iup;
				break;
			default: __assume(0); //tells MSVC default can't be reached
		}
	}
	roct->ind = oct_refreshnode(loct,ooct.ind,o-ooct.ind,n,noct);
}

#if (MARCHCUBE != 0)

static ipoint3d gcheck[1024];
static int gcheckn;
static int gcheckxyz (oct_t *loct, int x, int y, int z)
{
	static const int dirx[6] = {-1,+1,0,0,0,0}, diry[6] = {0,0,-1,+1,0,0}, dirz[6] = {0,0,0,0,-1,+1};
	static const int cornnum[6][4][2] =
	{
		0,1, 2,3, 4,5, 6,7,
		1,0, 3,2, 5,4, 7,6,
		0,2, 1,3, 4,6, 5,7,
		2,0, 3,1, 6,4, 7,5,
		0,4, 1,5, 2,6, 3,7,
		4,0, 5,1, 6,2, 7,3,
	};
	surf_t *vo, *vn;
	int i, j, k, d, v0, v1, got = 0;

	j = oct_getsurf(loct,x,y,z); if (j < 0) return(0);
	vo = &((surf_t *)loct->sur.buf)[j];

#if 0
	if ((vo->cornval[0]&vo->cornval[1]&vo->cornval[2]&vo->cornval[3]&vo->cornval[4]&vo->cornval[5]&vo->cornval[6]&vo->cornval[7]) < 0)
	{
		vo->r = 255; vo->g = 0; vo->b = 255;
		brush_box_t box;
		brush_box_init(&box,loct,x-1,y-1,z-1,x+1,y+1,z+1,loct->sid/256.0,0,0xff00ff,0);
		oct_mod(loct,(brush_t *)&box,2);
		return(1);
	}
#endif
	for(d=6-1;d>=0;d--)
	{
		j = oct_getsurf(loct,x+dirx[d],y+diry[d],z+dirz[d]);
		if (j >= 0) //if neighbor is surface, check that cornvals match..
		{
				//    +--+
				//    |v2|
				// +--0--1--+
				// |v0|vo|v1|
				// +--2--3--+
				//    |v3|
				//    +--+
			vn = &((surf_t *)loct->sur.buf)[j];
			for(j=4-1;j>=0;j--)
			{
				v0 = (int)vo->cornval[cornnum[d][j][0]];
				v1 = (int)vn->cornval[cornnum[d][j][1]]; if (v0 == v1) continue;
				k = (v0+v1+1)>>1;
				vo->cornval[cornnum[d][j][0]] = k;
				vn->cornval[cornnum[d][j][1]] = k;
				got = 1;
			}
		}
		else //if neighbor is interior/air, check if cornval matches..
		{
			j = oct_getsol(loct,x+dirx[d],y+diry[d],z+dirz[d]);
			for(k=4-1;k>=0;k--)
			{
				i = cornnum[d][k][0];
				if (j) { if (vo->cornval[i] < +1) { vo->cornval[i] = +1; got = 1; } }
				  else { if (vo->cornval[i] > -2) { vo->cornval[i] = -2; got = 1; } }
			}
		}
	}
	return(got);
}
#endif

typedef struct { oct_t *loct; int mode, mx0, my0, mz0, mx1, my1, mz1, bitmask; oct_getvox_hint_t och; } oct_updatesurfs_t;
static void oct_updatesurfs_recur (oct_updatesurfs_t *ous, int inode, int x0, int y0, int z0, int ls, octv_t *roct, brush_t *brush)
{
	surf_t surf[8], *psurf;
	octv_t ooct, noct[8];
	int i, iup, n, o, s, x, y, z, xsol;

	if (inode >= 0) { ooct = ((octv_t *)ous->loct->nod.buf)[inode]; }
				  else { ooct.chi = 0; ooct.sol = (1<<8)-1; ooct.ind = -1; ooct.mrk = 0; ooct.mrk2 = 0; }
	roct->sol = ooct.sol;
	roct->mrk = 0; roct->mrk2 = 0;

	if (!ls)
	{
 #if (MARCHCUBE == 0)
		if (0)
		{
			  //Slow&brute:
			roct->chi = 0;
			for(xsol=ooct.sol;xsol;xsol^=iup)
			{
				iup = (-xsol)&xsol;
				x = (((iup&0xaa)+0xff)>>8) + x0;
				y = (((iup&0xcc)+0xff)>>8) + y0;
				z = (((iup&0xf0)+0xff)>>8) + z0;
				if (oct_issurf(ous->loct,x,y,z,0,&ous->och)) roct->chi += iup;
			}
		}
		else
		{
				//above:      top:       bot:     below:
				//           18 19      22 23
				//+-----+    +---+      +---+    +-----+
				//|28 29|   9|0 1|8   13|4 5|12  |24 25|
				//|30 31|  11|2 3|10  15|6 7|14  |26 27|
				//+-----+    +---+      +---+    +-----+
				//           16 17      20 21
			i = ooct.sol; s = ous->loct->sid-2;
			if (x0 == 0) i += 0x0000aa00; else if (x0 == s) i += 0x00005500;
			if (y0 == 0) i += 0x00cc0000; else if (y0 == s) i += 0x00330000;
			if (z0 == 0) i += 0xf0000000; else if (z0 == s) i += 0x0f000000;
			i &= ous->bitmask;

			if (((i|~((1<<0)+(1<<1)+(1<<2)+(1<<4)                )) == -1) ||
				 ((i|~((1<<2)+(1<<0)+(1<<3)+(1<<6)                )) == -1) ||
				 ((i|~((1<<4)+(1<<0)+(1<<5)+(1<<6)                )) == -1) ||
				 ((i|~((1<<6)+(1<<2)+(1<<4)+(1<<7)                )) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0-2,y0  ,z0  ,&ous->och)&0xaa)<< 8);
			if (((i|~((1<<1)+(1<<0)+(1<<3)+(1<<5)                )) == -1) ||
				 ((i|~((1<<3)+(1<<1)+(1<<2)+(1<<7)                )) == -1) ||
				 ((i|~((1<<5)+(1<<1)+(1<<4)+(1<<7)                )) == -1) ||
				 ((i|~((1<<7)+(1<<3)+(1<<5)+(1<<6)                )) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0+2,y0  ,z0  ,&ous->och)&0x55)<< 8);
			if (((i|~((1<<0)+(1<<1)+(1<<2)+(1<<4)+(1<< 9)        )) == -1) ||
				 ((i|~((1<<1)+(1<<0)+(1<<3)+(1<<5)+(1<< 8)        )) == -1) ||
				 ((i|~((1<<4)+(1<<0)+(1<<5)+(1<<6)+(1<<13)        )) == -1) ||
				 ((i|~((1<<5)+(1<<1)+(1<<4)+(1<<7)+(1<<12)        )) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0  ,y0-2,z0  ,&ous->och)&0xcc)<<16);
			if (((i|~((1<<2)+(1<<0)+(1<<3)+(1<<6)+(1<<11)        )) == -1) ||
				 ((i|~((1<<3)+(1<<1)+(1<<2)+(1<<7)+(1<<10)        )) == -1) ||
				 ((i|~((1<<6)+(1<<2)+(1<<4)+(1<<7)+(1<<15)        )) == -1) ||
				 ((i|~((1<<7)+(1<<3)+(1<<5)+(1<<6)+(1<<14)        )) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0  ,y0+2,z0  ,&ous->och)&0x33)<<16);
			if (((i|~((1<<0)+(1<<1)+(1<<2)+(1<<4)+(1<< 9)+(1<<18))) == -1) ||
				 ((i|~((1<<1)+(1<<0)+(1<<3)+(1<<5)+(1<< 8)+(1<<19))) == -1) ||
				 ((i|~((1<<2)+(1<<0)+(1<<3)+(1<<6)+(1<<11)+(1<<16))) == -1) ||
				 ((i|~((1<<3)+(1<<1)+(1<<2)+(1<<7)+(1<<10)+(1<<17))) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0  ,y0  ,z0-2,&ous->och)&0xf0)<<24);
			if (((i|~((1<<4)+(1<<0)+(1<<5)+(1<<6)+(1<<13)+(1<<22))) == -1) ||
				 ((i|~((1<<5)+(1<<1)+(1<<4)+(1<<7)+(1<<12)+(1<<23))) == -1) ||
				 ((i|~((1<<6)+(1<<2)+(1<<4)+(1<<7)+(1<<15)+(1<<20))) == -1) ||
				 ((i|~((1<<7)+(1<<3)+(1<<5)+(1<<6)+(1<<14)+(1<<21))) == -1)) i += ((oct_getsol_hint_2x2x2(ous->loct,x0  ,y0  ,z0+2,&ous->och)&0x0f)<<24);

#if 0
			roct->chi = i; i = ~i;
			if (!(i&((1<<1)+(1<< 9) + (1<<2)+(1<<18) + (1<<4)+(1<<28)))) roct->chi &=~(1<<0);
			if (!(i&((1<<0)+(1<< 8) + (1<<3)+(1<<19) + (1<<5)+(1<<29)))) roct->chi &=~(1<<1);
			if (!(i&((1<<3)+(1<<11) + (1<<0)+(1<<16) + (1<<6)+(1<<30)))) roct->chi &=~(1<<2);
			if (!(i&((1<<2)+(1<<10) + (1<<1)+(1<<17) + (1<<7)+(1<<31)))) roct->chi &=~(1<<3);
			if (!(i&((1<<5)+(1<<13) + (1<<6)+(1<<22) + (1<<0)+(1<<24)))) roct->chi &=~(1<<4);
			if (!(i&((1<<4)+(1<<12) + (1<<7)+(1<<23) + (1<<1)+(1<<25)))) roct->chi &=~(1<<5);
			if (!(i&((1<<7)+(1<<15) + (1<<4)+(1<<20) + (1<<2)+(1<<26)))) roct->chi &=~(1<<6);
			if (!(i&((1<<6)+(1<<14) + (1<<5)+(1<<21) + (1<<3)+(1<<27)))) roct->chi &=~(1<<7);
#else
			o = (i>> 8)&i; s  = ((o&0x55)<<1) + ((o&0xaa)>>1);
			o = (i>>16)&i; s &= ((o&0x33)<<2) + ((o&0xcc)>>2);
			o = (i>>24)&i; s &= ((o&0x0f)<<4) + ((o&0xf0)>>4);
			roct->chi = (~s)&i;
#endif
		}
#elif (MARCHCUBE != 0)
		  //FIXFIXFIXFIX: Slow&brute:
		roct->chi = 0;
		for(xsol=ooct.sol;xsol;xsol^=iup)
		{
			iup = (-xsol)&xsol;
			x = (((iup&0xaa)+0xff)>>8) + x0;
			y = (((iup&0xcc)+0xff)>>8) + y0;
			z = (((iup&0xf0)+0xff)>>8) + z0;
			if (oct_issurf(ous->loct,x,y,z,0,&ous->och)) roct->chi += iup;
		}
#endif
		for(n=0,xsol=roct->chi;xsol;xsol^=iup,n++)
		{
			iup = (-xsol)&xsol;
#if (MARCHCUBE == 0)
			if (ooct.chi&iup) { memcpy(&surf[n],&((surf_t *)ous->loct->sur.buf)[popcount[(iup-1)&ooct.chi]+ooct.ind],ous->loct->sur.siz); continue; } //copy existing surf
#endif
			x = (((iup&0xaa)+0xff)>>8) + x0;
			y = (((iup&0xcc)+0xff)>>8) + y0;
			z = (((iup&0xf0)+0xff)>>8) + z0;
			getsurf_func(ous->loct,brush,x,y,z,&surf[n]);
#if (MARCHCUBE != 0)
				//keep edges solid (FIX:use ous->loct->edgeissol!)
			if (x ==                0) { surf[n].cornval[0] = -128; surf[n].cornval[2] = -128; surf[n].cornval[4] = -128; surf[n].cornval[6] = -128; }
			if (y ==                0) { surf[n].cornval[0] = -128; surf[n].cornval[1] = -128; surf[n].cornval[4] = -128; surf[n].cornval[5] = -128; }
			if (z ==                0) { surf[n].cornval[0] = -128; surf[n].cornval[1] = -128; surf[n].cornval[2] = -128; surf[n].cornval[3] = -128; }
			if (x == ous->loct->sid-1) { surf[n].cornval[1] = -128; surf[n].cornval[3] = -128; surf[n].cornval[5] = -128; surf[n].cornval[7] = -128; }
			if (y == ous->loct->sid-1) { surf[n].cornval[2] = -128; surf[n].cornval[3] = -128; surf[n].cornval[6] = -128; surf[n].cornval[7] = -128; }
			if (z == ous->loct->sid-1) { surf[n].cornval[4] = -128; surf[n].cornval[5] = -128; surf[n].cornval[6] = -128; surf[n].cornval[7] = -128; }
			if (ooct.chi&iup) //copy existing surf
			{
				int sc = 0;
				psurf = &((surf_t *)ous->loct->sur.buf)[popcount[(iup-1)&ooct.chi]+ooct.ind];
				for(i=8-1;i>=0;i--)
				{
					o = (int)psurf->cornval[i]-(int)surf[n].cornval[i]; sc += o;
					if ((o > 0) == (ous->mode&1)) surf[n].cornval[i] += o;
				}
				if ((sc > 0) == (ous->mode&1)) //resultant cornvals are closer to psurf (old)
					{ *(int *)&surf[n].b = *(int *)&psurf->b; surf[n].tex = psurf->tex; *(int *)&surf[n].norm = *(int *)&psurf->norm; }

				if (gcheckn < sizeof(gcheck)/sizeof(gcheck[0]))
					{ gcheck[gcheckn].x = x; gcheck[gcheckn].y = y; gcheck[gcheckn].z = z; gcheckn++; }
			}
#endif
		}
		roct->ind = oct_refreshsurf(ous->loct,ooct.ind,popcount[ooct.chi],n,surf);
		//NOTE:don't need to update ind&chi of parent here because it is surface (not geometry) info
		return;
	}

	roct->chi = ooct.chi; xsol = ooct.sol|ooct.chi; //skip pure air
	o = ooct.ind; n = 0; s = pow2[ls];
	for(;xsol;xsol^=iup) //visit nodes that are not pure air
	{
		iup = (-xsol)&xsol;
		x = x0; if (iup&0xaa) x += s;
		y = y0; if (iup&0xcc) y += s;
		z = z0; if (iup&0xf0) z += s;

		//if (!(ooct.mrk&iup)) //FIX:can't use here
		if ((x+s < ous->mx0) || (x > ous->mx1) || (y+s < ous->my0) || (y > ous->my1) || (z+s < ous->mz0) || (z > ous->mz1)) //outside update region: copy tree
		{
			if (ooct.chi&iup) { noct[n] = ((octv_t *)ous->loct->nod.buf)[o]; o++; n++; }
			continue;
		}

		if (!oct_issurf(ous->loct,x,y,z,ls,&ous->och)) //no surfs inside: remove tree
		{
			if (ooct.chi&iup)
			{
				oct_dealloctree(ous->loct,ls-1,((octv_t *)ous->loct->nod.buf)[o].ind,((octv_t *)ous->loct->nod.buf)[o].chi); o++;
				roct->chi ^= iup;
			}
			roct->sol |= iup;
			continue;
		}

		if (ooct.chi&iup) { i = o; o++; } //octree node intersects brush partially:must recurse
						 else { i =-1;      } //crack solid.. insert 1 node at all lower levels

		oct_updatesurfs_recur(ous,i,x,y,z,ls-1,&noct[n],brush); //recurse (crack solid if o<0)
		roct->chi |= iup;
		n++;
	}
	roct->ind = oct_refreshnode(ous->loct,ooct.ind,o-ooct.ind,n,noct);
	if (inode >= 0) ((octv_t *)ous->loct->nod.buf)[inode] = *roct; //NOTE! must keep tree&hint cache valid because of oct_getsol..() during mod
}
void oct_updatesurfs (oct_t *loct, int mx0, int my0, int mz0, int mx1, int my1, int mz1, brush_t *brush, int mode)
{
	oct_updatesurfs_t ous;
	octv_t nnode;
	int i, j;

#if (GPUSEBO != 0)
	if ((oct_usegpu) && (!loct->gsurf)) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
#endif

	ous.loct = loct; ous.mode = mode;
	ous.mx0 = max(mx0,0); ous.mx1 = min(mx1,loct->sid);
	ous.my0 = max(my0,0); ous.my1 = min(my1,loct->sid);
	ous.mz0 = max(mz0,0); ous.mz1 = min(mz1,loct->sid);

	ous.bitmask = 0x000000ff;
	if (loct->edgeissol& 1) ous.bitmask += 0x0000aa00;
	if (loct->edgeissol& 2) ous.bitmask += 0x00cc0000;
	if (loct->edgeissol& 4) ous.bitmask += 0xf0000000;
	if (loct->edgeissol& 8) ous.bitmask += 0x00005500;
	if (loct->edgeissol&16) ous.bitmask += 0x00330000;
	if (loct->edgeissol&32) ous.bitmask += 0x0f000000;

#if (MARCHCUBE != 0)
	gcheckn = 0;
#endif
	oct_getvox_hint_init(loct,&ous.och);
	oct_updatesurfs_recur(&ous,loct->head,0,0,0,loct->lsid-1,&nnode,brush);
	((octv_t *)loct->nod.buf)[loct->head] = nnode;

#if (MARCHCUBE != 0)
	for(j=8;(gcheckn > 0) && (j > 0);j--)
		for(i=gcheckn-1;i>=0;i--)
			if (!gcheckxyz(loct,gcheck[i].x,gcheck[i].y,gcheck[i].z))
				gcheck[i] = gcheck[--gcheckn];
#endif

	oct_checkreducesizes(loct);
}

//--------------------------------------------------------------------------------------------------
	//Test intersection between 2 parallelepipeds.
	//      bi: internal structure to be filled
	//r0,d0,f0: axes of base parallelepiped (vectors specify side lengths)
	//r1,d1,f1: axes of test parallelepiped (vectors specify side lengths)
typedef struct { double m0[9], m1[9], n[9], m[9], k[9]; dpoint3d r0, d0, f0, r1, d1, f1; } pgram3d_t;
#define MAT0ISIDENTITY 0
static void pgram3d_init (pgram3d_t *bi, dpoint3d *r0, dpoint3d *d0, dpoint3d *f0,
													  dpoint3d *r1, dpoint3d *d1, dpoint3d *f1)
{
	int i, j0, j1, j2;

	bi->r0 = (*r0); bi->d0 = (*d0); bi->f0 = (*f0);
	bi->r1 = (*r1); bi->d1 = (*d1); bi->f1 = (*f1);

	invert3x3(r1,d1,f1,bi->m1);
#if (MAT0ISIDENTITY != 0)
	bi->m[0] = fabs(bi->m1[0]) + fabs(bi->m1[1]) + fabs(bi->m1[2]) + 1.0;
	bi->m[1] = fabs(bi->m1[3]) + fabs(bi->m1[4]) + fabs(bi->m1[5]) + 1.0;
	bi->m[2] = fabs(bi->m1[6]) + fabs(bi->m1[7]) + fabs(bi->m1[8]) + 1.0;

	bi->n[0] = r1->x; bi->n[1] = r1->y; bi->n[2] = r1->z;
	bi->n[3] = d1->x; bi->n[4] = d1->y; bi->n[5] = d1->z;
	bi->n[6] = f1->x; bi->n[7] = f1->y; bi->n[8] = f1->z;
#else
	bi->m[0] = fabs(r0->x*bi->m1[0] + r0->y*bi->m1[1] + r0->z*bi->m1[2]) +
				  fabs(d0->x*bi->m1[0] + d0->y*bi->m1[1] + d0->z*bi->m1[2]) +
				  fabs(f0->x*bi->m1[0] + f0->y*bi->m1[1] + f0->z*bi->m1[2]) + 1.0;
	bi->m[1] = fabs(r0->x*bi->m1[3] + r0->y*bi->m1[4] + r0->z*bi->m1[5]) +
				  fabs(d0->x*bi->m1[3] + d0->y*bi->m1[4] + d0->z*bi->m1[5]) +
				  fabs(f0->x*bi->m1[3] + f0->y*bi->m1[4] + f0->z*bi->m1[5]) + 1.0;
	bi->m[2] = fabs(r0->x*bi->m1[6] + r0->y*bi->m1[7] + r0->z*bi->m1[8]) +
				  fabs(d0->x*bi->m1[6] + d0->y*bi->m1[7] + d0->z*bi->m1[8]) +
				  fabs(f0->x*bi->m1[6] + f0->y*bi->m1[7] + f0->z*bi->m1[8]) + 1.0;

	invert3x3(r0,d0,f0,bi->m0);
	bi->n[0] = r1->x*bi->m0[0] + r1->y*bi->m0[1] + r1->z*bi->m0[2];
	bi->n[1] = r1->x*bi->m0[3] + r1->y*bi->m0[4] + r1->z*bi->m0[5];
	bi->n[2] = r1->x*bi->m0[6] + r1->y*bi->m0[7] + r1->z*bi->m0[8];
	bi->n[3] = d1->x*bi->m0[0] + d1->y*bi->m0[1] + d1->z*bi->m0[2];
	bi->n[4] = d1->x*bi->m0[3] + d1->y*bi->m0[4] + d1->z*bi->m0[5];
	bi->n[5] = d1->x*bi->m0[6] + d1->y*bi->m0[7] + d1->z*bi->m0[8];
	bi->n[6] = f1->x*bi->m0[0] + f1->y*bi->m0[1] + f1->z*bi->m0[2];
	bi->n[7] = f1->x*bi->m0[3] + f1->y*bi->m0[4] + f1->z*bi->m0[5];
	bi->n[8] = f1->x*bi->m0[6] + f1->y*bi->m0[7] + f1->z*bi->m0[8];
#endif

	for(i=0;i<3;i++) { bi->m[i+3] = fabs(bi->n[i]) + fabs(bi->n[i+3]) + fabs(bi->n[i+6]) + 1.0; bi->m[i+6] = 2.0 - bi->m[i+3]; }
	j0 = 2; j1 = 0;
	do
	{     //NOTE:VC6 gave internal compiler error using: for(i0=..,i1=..,i2..)
		bi->k[j0*3+0] = fabs(bi->n[0+j0]*bi->n[3+j1] - bi->n[0+j1]*bi->n[3+j0])
						  + fabs(bi->n[0+j0]*bi->n[6+j1] - bi->n[0+j1]*bi->n[6+j0])
						  + fabs(bi->n[0+j0])       + fabs(bi->n[0+j1]);
		bi->k[j0*3+1] = fabs(bi->n[3+j0]*bi->n[6+j1] - bi->n[3+j1]*bi->n[6+j0])
						  + fabs(bi->n[3+j0]*bi->n[0+j1] - bi->n[3+j1]*bi->n[0+j0])
						  + fabs(bi->n[3+j0])       + fabs(bi->n[3+j1]);
		bi->k[j0*3+2] = fabs(bi->n[6+j0]*bi->n[0+j1] - bi->n[6+j1]*bi->n[0+j0])
						  + fabs(bi->n[6+j0]*bi->n[3+j1] - bi->n[6+j1]*bi->n[3+j0])
						  + fabs(bi->n[6+j0])       + fabs(bi->n[6+j1]);
		j0 = j1; j1++;
	} while (j1 < 3);
}
static int pgram3d_isint (pgram3d_t *bi, double dx, double dy, double dz)
{
	double x, y, z, ax, ay, az;

	if (fabs(dx*bi->m1[0] + dy*bi->m1[1] + dz*bi->m1[2]) >= bi->m[0]) return(0); //blue outside red face?
	if (fabs(dx*bi->m1[3] + dy*bi->m1[4] + dz*bi->m1[5]) >= bi->m[1]) return(0);
	if (fabs(dx*bi->m1[6] + dy*bi->m1[7] + dz*bi->m1[8]) >= bi->m[2]) return(0);
#if (MAT0ISIDENTITY != 0)
	x = dx; ax = fabs(dx); if (ax >= bi->m[3]) return(0); //red outside a blue face?
	y = dy; ay = fabs(dy); if (ay >= bi->m[4]) return(0);
	z = dz; az = fabs(dz); if (az >= bi->m[5]) return(0);
#else
	x = dx*bi->m0[0] + dy*bi->m0[1] + dz*bi->m0[2]; ax = fabs(x); if (ax >= bi->m[3]) return(0); //red outside blue face?
	y = dx*bi->m0[3] + dy*bi->m0[4] + dz*bi->m0[5]; ay = fabs(y); if (ay >= bi->m[4]) return(0);
	z = dx*bi->m0[6] + dy*bi->m0[7] + dz*bi->m0[8]; az = fabs(z); if (az >= bi->m[5]) return(0);
#endif
	if ((ax <= bi->m[6]) && (ay <= bi->m[7]) && (az <= bi->m[8])) return(2); //blue inside red?
	if (fabs(x*bi->n[1] - y*bi->n[0]) > bi->k[0]) return(0); //blue outside red, 2D xy plane
	if (fabs(x*bi->n[4] - y*bi->n[3]) > bi->k[1]) return(0);
	if (fabs(x*bi->n[7] - y*bi->n[6]) > bi->k[2]) return(0);
	if (fabs(y*bi->n[2] - z*bi->n[1]) > bi->k[3]) return(0); //blue outside red, 2D yz plane
	if (fabs(y*bi->n[5] - z*bi->n[4]) > bi->k[4]) return(0);
	if (fabs(y*bi->n[8] - z*bi->n[7]) > bi->k[5]) return(0);
	if (fabs(z*bi->n[0] - x*bi->n[2]) > bi->k[6]) return(0); //blue outside red, 2D zx plane
	if (fabs(z*bi->n[3] - x*bi->n[5]) > bi->k[7]) return(0);
	if (fabs(z*bi->n[6] - x*bi->n[8]) > bi->k[8]) return(0);
	return(1);
}
static int pgram3d_gethit (pgram3d_t *bi, dpoint3d *c0, dpoint3d *c1, dpoint3d *dpos, dpoint3d *hit, dpoint3d *norm)
{
	dpoint3d e0, ev, f0, fv;
	double fk, f, s, t, x, y, z, dx, dy, dz, bx, by, bz, cx, cy, cz, bt, d2, d2min, rb;
	int i, j, besti, k0, k1, k2, k3;

	dx = c1->x-c0->x;
	dy = c1->y-c0->y;
	dz = c1->z-c0->z; d2min = 1e32;
	for(i=0;i<15;i++)
	{
			  if (i < 3) { j =  i   *3; x = bi->m1[j]; y = bi->m1[j+1]; z = bi->m1[j+2]; fk = bi->m[i]; }
		else if (i < 6) { j = (i-3)*3; x = bi->m0[j]; y = bi->m0[j+1]; z = bi->m0[j+2]; fk = bi->m[i]; }
		else
		{
			j = i-6;
			k0 = (j/3)*3; k1 = (k0+3)%9; k2 = (((j/3)+1)%3) + (j%3)*3; k3 = (j/3)+(j%3)*3;
			x = bi->m0[k0  ]*bi->n[k2] - bi->m0[k1  ]*bi->n[k3];
			y = bi->m0[k0+1]*bi->n[k2] - bi->m0[k1+1]*bi->n[k3];
			z = bi->m0[k0+2]*bi->n[k2] - bi->m0[k1+2]*bi->n[k3];
			fk = bi->k[j];
		}
		f = x*x + y*y + z*z; t = dx*x + dy*y + dz*z; if (fabs(t) < 1e-16) continue;
		if (t >= 0) s = fk; else s = -fk;
		t = (s-t) / f;
		d2 = t*t*f; if (d2 < d2min) { d2min = d2; bx = x; by = y; bz = z; bt = t; besti = i; }
	}
	if (d2min >= 1e32) return(0);

	bx *= bt; dpos->x = bx;
	by *= bt; dpos->y = by;
	bz *= bt; dpos->z = bz;

	if (besti < 3)
	{
		cx = c0->x; cy = c0->y; cz = c0->z;
		s = bx*bi->r0.x + by*bi->r0.y + bz*bi->r0.z; s = (s>=0.0)*2-1.0; cx += bi->r0.x*s; cy += bi->r0.y*s; cz += bi->r0.z*s;
		s = bx*bi->d0.x + by*bi->d0.y + bz*bi->d0.z; s = (s>=0.0)*2-1.0; cx += bi->d0.x*s; cy += bi->d0.y*s; cz += bi->d0.z*s;
		s = bx*bi->f0.x + by*bi->f0.y + bz*bi->f0.z; s = (s>=0.0)*2-1.0; cx += bi->f0.x*s; cy += bi->f0.y*s; cz += bi->f0.z*s;
		bx *= -1; by *= -1; bz *= -1;

			//Make sure (cx,cy,cz) is inside both paralellepipeds..
		x = (cx-c1->x)*bi->m1[0] + (cy-c1->y)*bi->m1[1] + (cz-c1->z)*bi->m1[2]; x = min(max(x,-1.0),1.0);
		y = (cx-c1->x)*bi->m1[3] + (cy-c1->y)*bi->m1[4] + (cz-c1->z)*bi->m1[5]; y = min(max(y,-1.0),1.0);
		z = (cx-c1->x)*bi->m1[6] + (cy-c1->y)*bi->m1[7] + (cz-c1->z)*bi->m1[8]; z = min(max(z,-1.0),1.0);
		cx = bi->r1.x*x + bi->d1.x*y + bi->f1.x*z + c1->x;
		cy = bi->r1.y*x + bi->d1.y*y + bi->f1.y*z + c1->y;
		cz = bi->r1.z*x + bi->d1.z*y + bi->f1.z*z + c1->z;
	}
	else if (besti < 6)
	{
		cx = c1->x; cy = c1->y; cz = c1->z;
		s = bx*bi->r1.x + by*bi->r1.y + bz*bi->r1.z; s = (s>=0.0)*2-1.0; cx -= bi->r1.x*s; cy -= bi->r1.y*s; cz -= bi->r1.z*s;
		s = bx*bi->d1.x + by*bi->d1.y + bz*bi->d1.z; s = (s>=0.0)*2-1.0; cx -= bi->d1.x*s; cy -= bi->d1.y*s; cz -= bi->d1.z*s;
		s = bx*bi->f1.x + by*bi->f1.y + bz*bi->f1.z; s = (s>=0.0)*2-1.0; cx -= bi->f1.x*s; cy -= bi->f1.y*s; cz -= bi->f1.z*s;

			//Make sure (cx,cy,cz) is inside both paralellepipeds..
		x = (cx-c0->x)*bi->m0[0] + (cy-c0->y)*bi->m0[1] + (cz-c0->z)*bi->m0[2]; x = min(max(x,-1.0),1.0);
		y = (cx-c0->x)*bi->m0[3] + (cy-c0->y)*bi->m0[4] + (cz-c0->z)*bi->m0[5]; y = min(max(y,-1.0),1.0);
		z = (cx-c0->x)*bi->m0[6] + (cy-c0->y)*bi->m0[7] + (cz-c0->z)*bi->m0[8]; z = min(max(z,-1.0),1.0);
		cx = bi->r0.x*x + bi->d0.x*y + bi->f0.x*z + c0->x;
		cy = bi->r0.y*x + bi->d0.y*y + bi->f0.y*z + c0->y;
		cz = bi->r0.z*x + bi->d0.z*y + bi->f0.z*z + c0->z;
	}
	else
	{
		i = (besti%3); e0 = (*c1);
		if (i == 0) ev = bi->r1; else { s = bx*bi->r1.x + by*bi->r1.y + bz*bi->r1.z; s = (s>=0.0)*2-1.0; e0.x -= bi->r1.x*s; e0.y -= bi->r1.y*s; e0.z -= bi->r1.z*s; }
		if (i == 1) ev = bi->d1; else { s = bx*bi->d1.x + by*bi->d1.y + bz*bi->d1.z; s = (s>=0.0)*2-1.0; e0.x -= bi->d1.x*s; e0.y -= bi->d1.y*s; e0.z -= bi->d1.z*s; }
		if (i == 2) ev = bi->f1; else { s = bx*bi->f1.x + by*bi->f1.y + bz*bi->f1.z; s = (s>=0.0)*2-1.0; e0.x -= bi->f1.x*s; e0.y -= bi->f1.y*s; e0.z -= bi->f1.z*s; }
		i = (((besti-6)/3)+2)%3; f0 = (*c0);
		if (i == 0) fv = bi->r0; else { s = bx*bi->r0.x + by*bi->r0.y + bz*bi->r0.z; s = (s>=0.0)*2-1.0; f0.x += bi->r0.x*s; f0.y += bi->r0.y*s; f0.z += bi->r0.z*s; }
		if (i == 1) fv = bi->d0; else { s = bx*bi->d0.x + by*bi->d0.y + bz*bi->d0.z; s = (s>=0.0)*2-1.0; f0.x += bi->d0.x*s; f0.y += bi->d0.y*s; f0.z += bi->d0.z*s; }
		if (i == 2) fv = bi->f0; else { s = bx*bi->f0.x + by*bi->f0.y + bz*bi->f0.z; s = (s>=0.0)*2-1.0; f0.x += bi->f0.x*s; f0.y += bi->f0.y*s; f0.z += bi->f0.z*s; }
		dx = by*ev.z - bz*ev.y; //f0.x + fv.x*i + bx*b = e0.x + ev.x*e  |  (fv.x)*i + (-ev.x)*e + (bx)*b = (e0.x-f0.x)
		dy = bz*ev.x - bx*ev.z; //f0.y + fv.y*i + by*b = e0.y + ev.y*e  |  (fv.y)*i + (-ev.y)*e + (by)*b = (e0.y-f0.y)
		dz = bx*ev.y - by*ev.x; //f0.z + fv.z*i + bz*b = e0.z + ev.z*e  |  (fv.z)*i + (-ev.z)*e + (bz)*b = (e0.z-f0.z)
		f = ((e0.x-f0.x)*dx + (e0.y-f0.y)*dy + (e0.z-f0.z)*dz) / (fv.x*dx + fv.y*dy + fv.z*dz);
		cx = fv.x*f + f0.x; bx *= -1.0;
		cy = fv.y*f + f0.y; by *= -1.0;
		cz = fv.z*f + f0.z; bz *= -1.0;
	}

	hit->x = bx*.5 + cx;
	hit->y = by*.5 + cy;
	hit->z = bz*.5 + cz;
	rb = bx*bx + by*by + bz*bz;
	if (rb == 0.0) { norm->x = 0.0; norm->y = 1.0; norm->z = 0.0; return(1); } //FIXFIXFIXFIX:wrong hack!
	rb = 1.0/sqrt(rb);
	norm->x = bx*rb;
	norm->y = by*rb;
	norm->z = bz*rb;
	return(1);
}

//--------------------------------------------------------------------------------------------------
static void vecadd   (dpoint3d *c, dpoint3d *a, dpoint3d *b) { c->x = a->x+b->x; c->y = a->y+b->y; c->z = a->z+b->z; }
static void vecsub   (dpoint3d *c, dpoint3d *a, dpoint3d *b) { c->x = a->x-b->x; c->y = a->y-b->y; c->z = a->z-b->z; }
static void vecscale (dpoint3d *c, dpoint3d *a, double sc)   { c->x = a->x*sc; c->y = a->y*sc; c->z = a->z*sc; }
static double vecdot (dpoint3d *a, dpoint3d *b)              { return(a->x*b->x + a->y*b->y + a->z*b->z); }
static void veccross (dpoint3d *c, dpoint3d *a, dpoint3d *b) //C = A x B
{
	dpoint3d nc;
	nc.x = a->y*b->z - a->z*b->y;
	nc.y = a->z*b->x - a->x*b->z;
	nc.z = a->x*b->y - a->y*b->x;
	(*c) = nc;
}
static void matvecmul (dpoint3d *c, double *a, dpoint3d *b)
{
	dpoint3d nc;
	nc.x = a[0]*b->x + a[1]*b->y + a[2]*b->z;
	nc.y = a[3]*b->x + a[4]*b->y + a[5]*b->z;
	nc.z = a[6]*b->x + a[7]*b->y + a[8]*b->z;
	(*c) = nc;
}
static void vecmatmul (dpoint3d *c, dpoint3d *a, double *b)
{
	dpoint3d nc;
	nc.x = a->x*b[0] + a->y*b[3] + a->z*b[6];
	nc.y = a->x*b[1] + a->y*b[4] + a->z*b[7];
	nc.z = a->x*b[2] + a->y*b[5] + a->z*b[8];
	(*c) = nc;
}
static void simxform (double *b, double *r, double *a) //B = R*A*R^T
{
	double t[9];

		//[t0 t1 t2]   [r0 r1 r2][a0 a1 a2]
		//[t3 t4 t5] = [r3 r4 r5][a3 a4 a5]
		//[t6 t7 t8]   [r6 r7 r8][a6 a7 a8]
	t[0] = r[0]*a[0] + r[1]*a[3] + r[2]*a[6];
	t[1] = r[0]*a[1] + r[1]*a[4] + r[2]*a[7];
	t[2] = r[0]*a[2] + r[1]*a[5] + r[2]*a[8];
	t[3] = r[3]*a[0] + r[4]*a[3] + r[5]*a[6];
	t[4] = r[3]*a[1] + r[4]*a[4] + r[5]*a[7];
	t[5] = r[3]*a[2] + r[4]*a[5] + r[5]*a[8];
	t[6] = r[6]*a[0] + r[7]*a[3] + r[8]*a[6];
	t[7] = r[6]*a[1] + r[7]*a[4] + r[8]*a[7];
	t[8] = r[6]*a[2] + r[7]*a[5] + r[8]*a[8];

		//[b0 b1 b2]   [t0 t1 t2][r0 r3 r6]
		//[b3 b4 b5] = [t3 t4 t5][r1 r4 r7]
		//[b6 b7 b8]   [t6 t7 t8][r2 r5 r8]
	b[0] = t[0]*r[0] + t[1]*r[1] + t[2]*r[2];
	b[1] = t[0]*r[3] + t[1]*r[4] + t[2]*r[5];
	b[2] = t[0]*r[6] + t[1]*r[7] + t[2]*r[8];
	b[3] = t[3]*r[0] + t[4]*r[1] + t[5]*r[2];
	b[4] = t[3]*r[3] + t[4]*r[4] + t[5]*r[5];
	b[5] = t[3]*r[6] + t[4]*r[7] + t[5]*r[8];
	b[6] = t[6]*r[0] + t[7]*r[1] + t[8]*r[2];
	b[7] = t[6]*r[3] + t[7]*r[4] + t[8]*r[5];
	b[8] = t[6]*r[6] + t[7]*r[7] + t[8]*r[8];
}

	//see rigid1.pdf&rigid2.pdf for derivation
	//---------------------------------------------------------
	//e      r   Coefficient of restitution: 0=plastic, 1=elastic
	//cp,cn  r   Contact point and unit vector nomal (provided by caller)
	//?pos   r   Centroid of body, world space
	//?ori   r   3x3 rotation matrix <rig,dow,for>, body to world space
	//?vel   rw  Translational velocity, world space
	//?rax   rw  Rotation axis scaled by angular velocity (rev/s), world space
	//?rmas  r   Reciprocal of mass
	//?rmoi  r   Inverse moment of inertia 3x3 matrix, ->BODY<- space.
void doimpulse3d (double e, dpoint3d *cp, dpoint3d *cn,
	dpoint3d *apos, double *aori, dpoint3d *avel, dpoint3d *arax, double armas, double *armoi,
	dpoint3d *bpos, double *bori, dpoint3d *bvel, dpoint3d *brax, double brmas, double *brmoi)
{
	dpoint3d fp, padot, pbdot, ra, rb, ta, tb;
	double fj, vrel, num, den, namoi[9], nbmoi[9];

		//FIXFIXFIXFIX:remove!
	printf("e:%g cp:<%g %g %g> cn:<%g %g %g>\n"
			 "apos:<%g %g %g> aori:<%g %g %g %g %g %g %g %g %g> armas:%g armoi:<%g %g %g %g %g %g %g %g %g>\n"
			 "bpos:<%g %g %g> bori:<%g %g %g %g %g %g %g %g %g> brmas:%g brmoi:<%g %g %g %g %g %g %g %g %g>\n",
		e,cp->x,cp->y,cp->z,cn->x,cn->y,cn->z,
		apos->x,apos->y,apos->z,aori[0],aori[1],aori[2],aori[3],aori[4],aori[5],aori[6],aori[7],aori[8],armas,armoi[0],armoi[1],armoi[2],armoi[3],armoi[4],armoi[5],armoi[6],armoi[7],armoi[8],
		bpos->x,bpos->y,bpos->z,bori[0],bori[1],bori[2],bori[3],bori[4],bori[5],bori[6],bori[7],bori[8],brmas,brmoi[0],brmoi[1],brmoi[2],brmoi[3],brmoi[4],brmoi[5],brmoi[6],brmoi[7],brmoi[8]);
	printf("avel:<%g %g %g> arax:<%g %g %g> bvel:<%g %g %g> brax:<%g %g %g>\n",avel->x,avel->y,avel->z,arax->x,arax->y,arax->z,bvel->x,bvel->y,bvel->z,brax->x,brax->y,brax->z);

		//Calc moi in world coords
		//I_world^-1 = R * I_body^-1 * R^T
	simxform(namoi,aori,armoi);
	simxform(nbmoi,bori,brmoi);

		//Calc impulse magnitude (j)
		//j = (-e-1) * (((Va + Wa x Rap) - (Vb + Wb x Rbp)) dot N) /
		//   ( 1/ma + N dot ((Ia^-1 * (Rap cross N)) cross Rap)
		//   + 1/mb + N dot ((Ib^-1 * (Rbp cross N)) cross Rbp))
		//
		//j = num/(armas + brmas + cn . ((a->Iinv * (ra x cn)) x ra)
		//                         cn . ((b->Iinv * (rb x cn)) x rb));
		//Calc relative velocity (vrel), parallel to contact normal (cn) at contact point (cp)
	vecsub(&ra,cp,apos); //ra = cp - a->x;
	vecsub(&rb,cp,bpos); //rb = cp - b->x;
	veccross(&fp,arax,&ra); vecadd(&padot,avel,&fp); //padot = a->v + (a->w x ra)
	veccross(&fp,brax,&rb); vecadd(&pbdot,bvel,&fp); //pbdot = b->v + (b->w x rb)
	vecsub(&fp,&padot,&pbdot); vrel = vecdot(cn,&fp); //vrel = cn . (padot - pbdot);
	num = (-e-1.0)*vrel;
	den = armas+brmas;
	veccross(&fp,&ra,cn); matvecmul(&ta,namoi,&fp); veccross(&fp,&ta,&ra); den += vecdot(cn,&fp);
	veccross(&fp,&rb,cn); matvecmul(&tb,nbmoi,&fp); veccross(&fp,&tb,&rb); den += vecdot(cn,&fp);
	fj = num/den;

		//Do impulse:translation
	vecscale(&fp,cn,fj*armas); vecadd(avel,avel,&fp); //nVa = Va + (j*N)/ma;
	vecscale(&fp,cn,fj*brmas); vecsub(bvel,bvel,&fp); //nVb = Vb - (j*N)/mb;

		//Do impulse:rotation
	vecscale(&fp,&ta,fj); vecadd(arax,arax,&fp);       //nWa = Wa + Ia^-1 * (Rap cross (j*N));
	vecscale(&fp,&tb,fj); vecsub(brax,brax,&fp);       //nWb = Wb - Ib^-1 * (Rbp cross (j*N));

		//FIXFIXFIXFIX:remove!
	printf("avel:<%g %g %g> arax:<%g %g %g> bvel:<%g %g %g> brax:<%g %g %g>\n",avel->x,avel->y,avel->z,arax->x,arax->y,arax->z,bvel->x,bvel->y,bvel->z,brax->x,brax->y,brax->z);
}

//--------------------------------------------------------------------------------------------------
	//tree node vs. other octree
	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
typedef struct
{
	BRUSH_HEADER;
	oct_t *oct;
	point3d p[OCT_MAXLS], r[OCT_MAXLS], d[OCT_MAXLS], f[OCT_MAXLS];
	pgram3d_t bi[OCT_MAXLS*2];
} brush_oct_t;
static int brush_oct_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	oct_t *loct;
	octv_t *ptr;
	float dx, dy, dz, ex, ey, ez;
	int i, j, k, s, ls2, x, y, z, x1, y1, z1, nx, ny, nz;
	brush_oct_t *boct = (brush_oct_t *)brush;
	loct = boct->oct;

	dx = x0*boct->r[1].x + y0*boct->d[1].x + z0*boct->f[1].x + boct->p[ls].x;
	dy = x0*boct->r[1].y + y0*boct->d[1].y + z0*boct->f[1].y + boct->p[ls].y;
	dz = x0*boct->r[1].z + y0*boct->d[1].z + z0*boct->f[1].z + boct->p[ls].z;

		//Bounded box init:
	ex = fabs(boct->r[ls].x) + fabs(boct->d[ls].x) + fabs(boct->f[ls].x);
	ey = fabs(boct->r[ls].y) + fabs(boct->d[ls].y) + fabs(boct->f[ls].y);
	ez = fabs(boct->r[ls].z) + fabs(boct->d[ls].z) + fabs(boct->f[ls].z);
	x0 = max(cvttss2si(dx-ex),0); x1 = min(cvttss2si(dx+ex),loct->sid);
	y0 = max(cvttss2si(dy-ey),0); y1 = min(cvttss2si(dy+ey),loct->sid);
	z0 = max(cvttss2si(dz-ez),0); z1 = min(cvttss2si(dz+ez),loct->sid);

	k = 0;
	i = pgram3d_isint(&boct->bi[OCT_MAXLS-loct->lsid+ls],loct->sid-dx*2,loct->sid-dy*2,loct->sid-dz*2);
	if (i == 0) return(0);
	if (i == 1) k = 1;

	ls2 = loct->lsid-1; s = (1<<ls2); ptr = &((octv_t *)loct->nod.buf)[loct->head];
	x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		nx = (( j    &1)<<ls2)+x; if ((nx > x1) || (nx+s <= x0)) goto tosibly;
		ny = (((j>>1)&1)<<ls2)+y; if ((ny > y1) || (ny+s <= y0)) goto tosibly;
		nz = (((j>>2)&1)<<ls2)+z; if ((nz > z1) || (nz+s <= z0)) goto tosibly;

		i = (1<<j);
		if (ptr->chi&(~ptr->sol)&i) //mixed air&sol
		{
			stk[ls2].ptr = ptr; stk[ls2].x = x; stk[ls2].y = y; stk[ls2].z = z; stk[ls2].j = j; ls2--; s >>= 1; //2child
			ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
			continue;
		}

			//Do slow exact check for leaf (pure air/sol) nodes
		if (!pgram3d_isint(&boct->bi[OCT_MAXLS-ls2+ls],(nx-dx)*2+s,(ny-dy)*2+s,(nz-dz)*2+s)) goto tosibly;

		if (ptr->sol&i) k |= 2; else k |= 1;
		if (k == 3) return(1);

tosibly:;
		j--; if (j >= 0) continue;
		do { ls2++; s <<= 1; if (ls2 >= loct->lsid) return(k&2); j = stk[ls2].j-1; } while (j < 0); //2parent
		ptr = stk[ls2].ptr; x = stk[ls2].x; y = stk[ls2].y; z = stk[ls2].z;
	}
}
static void brush_oct_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	static const int dirx[27] = {0, -1,+1, 0, 0, 0, 0, -1,+1,-1,+1, 0, 0, 0, 0,-1,-1,+1,+1, -1,+1,-1,+1,-1,+1,-1,+1};
	static const int diry[27] = {0,  0, 0,-1,+1, 0, 0, -1,-1,+1,+1,-1,+1,-1,+1, 0, 0, 0, 0, -1,-1,+1,+1,-1,-1,+1,+1};
	static const int dirz[27] = {0,  0, 0, 0, 0,-1,+1,  0, 0, 0, 0,-1,-1,+1,+1,-1,+1,-1,+1, -1,-1,-1,-1,+1,+1,+1,+1};
	int i, j, x, y, z;
	brush_oct_t *boct = (brush_oct_t *)brush;

	x = cvttss2si(x0*boct->r[1].x + y0*boct->d[1].x + z0*boct->f[1].x + boct->p[0].x);
	y = cvttss2si(x0*boct->r[1].y + y0*boct->d[1].y + z0*boct->f[1].y + boct->p[0].y);
	z = cvttss2si(x0*boct->r[1].z + y0*boct->d[1].z + z0*boct->f[1].z + boct->p[0].z);

	for(i=0;i<27;i++)
	{
		j = oct_getsurf(boct->oct,dirx[i]+x,diry[i]+y,dirz[i]+z); if (j == -1) continue;
		(*surf) = ((surf_t *)boct->oct->sur.buf)[j]; return;
	}
	  //:/
	surf->b = 255; surf->g = 0; surf->r = 255; surf->a = 255;
	surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0; surf->tex = 0;
}
static void brush_oct_init (brush_oct_t *boct, oct_t *loct0, dpoint3d *p0, dpoint3d *r0, dpoint3d *d0, dpoint3d *f0,
															  oct_t *loct1, dpoint3d *p1, dpoint3d *r1, dpoint3d *d1, dpoint3d *f1)
{
	dpoint3d cr, cd, cf, dr, dd, df;
	point3d ap, ar, ad, af;
	double d, mat[9];
	int i;

	boct->isins   = brush_oct_isins;
	boct->getsurf = brush_oct_getsurf;
	boct->oct = loct1;

		//FIXFIXFIXFIX:can remove this inverse now that pgram3d handles it!
		//transform loct0 to make loct1 identity (p1=<0,0,0>, r1=<1,0,0>, d1=<0,1,0>, f1=<0,0,1>)
		//[r,d,f,p] = loct1^-1 * loct0
	invert3x3(r1,d1,f1,mat);
	ar.x = r0->x*mat[0] + r0->y*mat[1] + r0->z*mat[2];
	ar.y = r0->x*mat[3] + r0->y*mat[4] + r0->z*mat[5];
	ar.z = r0->x*mat[6] + r0->y*mat[7] + r0->z*mat[8];
	ad.x = d0->x*mat[0] + d0->y*mat[1] + d0->z*mat[2];
	ad.y = d0->x*mat[3] + d0->y*mat[4] + d0->z*mat[5];
	ad.z = d0->x*mat[6] + d0->y*mat[7] + d0->z*mat[8];
	af.x = f0->x*mat[0] + f0->y*mat[1] + f0->z*mat[2];
	af.y = f0->x*mat[3] + f0->y*mat[4] + f0->z*mat[5];
	af.z = f0->x*mat[6] + f0->y*mat[7] + f0->z*mat[8];
	dr.x = p0->x-p1->x; dr.y = p0->y-p1->y; dr.z = p0->z-p1->z;
	ap.x = dr.x*mat[0] + dr.y*mat[1] + dr.z*mat[2];
	ap.y = dr.x*mat[3] + dr.y*mat[4] + dr.z*mat[5];
	ap.z = dr.x*mat[6] + dr.y*mat[7] + dr.z*mat[8];

	for(i=loct0->lsid-1;i>=0;i--)
	{
		d = pow(2.0,(double)i)*0.5;
		boct->r[i].x = ar.x*d; boct->d[i].x = ad.x*d; boct->f[i].x = af.x*d; boct->p[i].x = boct->r[i].x + boct->d[i].x + boct->f[i].x + ap.x;
		boct->r[i].y = ar.y*d; boct->d[i].y = ad.y*d; boct->f[i].y = af.y*d; boct->p[i].y = boct->r[i].y + boct->d[i].y + boct->f[i].y + ap.y;
		boct->r[i].z = ar.z*d; boct->d[i].z = ad.z*d; boct->f[i].z = af.z*d; boct->p[i].z = boct->r[i].z + boct->d[i].z + boct->f[i].z + ap.z;
	}

	cr.x = 1.0; cr.y = 0.0; cr.z = 0.0;
	cd.x = 0.0; cd.y = 1.0; cd.z = 0.0;
	cf.x = 0.0; cf.y = 0.0; cf.z = 1.0;
	for(i=-loct1->lsid;i<loct0->lsid;i++)
	{
		d = (double)(1<<(i+17));
		dr.x = ar.x*d; dr.y = ar.y*d; dr.z = ar.z*d;
		dd.x = ad.x*d; dd.y = ad.y*d; dd.z = ad.z*d;
		df.x = af.x*d; df.y = af.y*d; df.z = af.z*d;
		pgram3d_init(&boct->bi[i+OCT_MAXLS],&cr,&cd,&cf,&dr,&dd,&df);
	}
}

//--------------------------------------------------------------------------------------------------
	//NOTE:volume of solid must be < 65536 for brush_bmp() to work! (example: 40^3 is always safe)
static void brush_bmp_calcsum (unsigned short *boxsum, char *bmp, int valeq, int xs, int ys, int zs)
{
	unsigned short *nboxsum;
	int i, j, x, y, z, xmul, ymul, zmul;

	zmul = 1; ymul = xs+1; xmul = (ys+1)*ymul;
	for(i=0;i<=xs;i++) boxsum[i*xmul] = 0;
	for(i=0;i<=ys;i++) boxsum[i*ymul] = 0;
	for(i=0;i<=zs;i++) boxsum[i*zmul] = 0;

	for(i=0,z=0;z<zs;z++)
		for(y=0;y<ys;y++)
		{
			nboxsum = &boxsum[y*ymul + z*zmul];
			for(j=0,x=0;x<xs;x++,i++,nboxsum+=xmul)
			{
				j += (bmp[i] == valeq);
				nboxsum[xmul+ymul+zmul] = nboxsum[xmul+ymul] + nboxsum[xmul+zmul] - nboxsum[xmul] + j;
			}
		}
}

#define RUBBLE_NUMREG 4
static unsigned short rubble_boxsum[RUBBLE_NUMREG][32+1][32+1][32+1];
static int rubble_inited = 0;
static float rubble_getsc (ipoint3d *pt, int numpt, int x, int y, int z, int *bestj)
{
	float f, bestf, dx, dy, dz;
	int i, j;

	bestf = 1e32; (*bestj) = 0;
	for(j=0;j<RUBBLE_NUMREG;j++)
	{
		f = 0.f;
		for(i=0;i<numpt;i++)
		{
			dx = (float)(pt[j*numpt+i].x-x)+.5;
			dy = (float)(pt[j*numpt+i].y-y)+.5;
			dz = (float)(pt[j*numpt+i].z-z)+.5;
			f += 1.f/(dx*dx + dy*dy + dz*dz);
		}
		if (f < bestf) { bestf = f; (*bestj) = j; }
	}
	return(f);
}
static void rubble_genboxsums (void) //Voxlap rubble.. generates 4 boxsums
{
	#define RUBBLE_NUMPT 7
	char bmp[32][32][32];
	ipoint3d pt[RUBBLE_NUMREG*RUBBLE_NUMPT];
	float f, maxf;
	int i, j, k, x, y, z, xi;

		//for each region, generate n points within 11 of the center
	srand(1);
	for(j=0;j<RUBBLE_NUMREG;j++)
		for(i=0;i<RUBBLE_NUMPT;i++)
		{
			k = j*RUBBLE_NUMPT+i;
			do
			{
				pt[k].x = (rand()&31)-16;
				pt[k].y = (rand()&31)-16;
				pt[k].z = (rand()&31)-16;
			} while (pt[k].x*pt[k].x + pt[k].y*pt[k].y + pt[k].z*pt[k].z >= 11*11);
			pt[k].x += 16; pt[k].y += 16; pt[k].z += 16;
		}

		//find highest score along edges of cube
	maxf = 0.f;
	for(z=32-1;z>=0;z--)
		for(y=32-1;y>=0;y--)
		{
			if ((y == 0) || (z == 0) || (y == 32-1) || (z == 32-1)) xi = 1; else xi = 32-1;
			for(x=32-1;x>=0;x-=xi) maxf = max(maxf,rubble_getsc(pt,RUBBLE_NUMPT,x,y,z,&j));
		}

		//write region buffer; 0 for blank, 1-NUMREG for object
	memset(bmp,255,sizeof(bmp));
	for(z=32-1;z>=0;z--)
		for(y=32-1;y>=0;y--)
			for(x=32-1;x>=0;x--)
				{ f = rubble_getsc(pt,RUBBLE_NUMPT,x,y,z,&j); if (f > maxf) bmp[z][y][x] = j; }

	for(k=0;k<RUBBLE_NUMREG;k++) brush_bmp_calcsum(&rubble_boxsum[k][0][0][0],&bmp[0][0][0],k,32,32,32);
}

	//tree node vs. bitmap
	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_bmp_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	int i, ox0, oy0, oz0, ox1, oy1, oz1, nx0, ny0, nz0, nx1, ny1, nz1;
	brush_bmp_t *bmp = (brush_bmp_t *)brush;

	i = bmp->iux*x0 + bmp->ivx*y0 + bmp->iwx*z0; ox0 = ((bmp->iox0[ls]+i)>>16); ox1 = ((bmp->iox1[ls]+i)>>16); nx0 = max(ox0,0); nx1 = min(ox1,bmp->xs); if (nx0 >= nx1) return(0);
	i = bmp->iuy*x0 + bmp->ivy*y0 + bmp->iwy*z0; oy0 = ((bmp->ioy0[ls]+i)>>16); oy1 = ((bmp->ioy1[ls]+i)>>16); ny0 = max(oy0,0); ny1 = min(oy1,bmp->ys); if (ny0 >= ny1) return(0);
	i = bmp->iuz*x0 + bmp->ivz*y0 + bmp->iwz*z0; oz0 = ((bmp->ioz0[ls]+i)>>16); oz1 = ((bmp->ioz1[ls]+i)>>16); nz0 = max(oz0,0); nz1 = min(oz1,bmp->zs); if (nz0 >= nz1) return(0);

	i =  (bmp->xs+1); ny0 *= i; ny1 *= i;
	i *= (bmp->ys+1); nz0 *= i; nz1 *= i;
	i  = (bmp->boxsum[nz1+ny0+nx0] - bmp->boxsum[nz1+ny1+nx0] - bmp->boxsum[nz1+ny0+nx1] + bmp->boxsum[nz1+ny1+nx1]);
	i -= (bmp->boxsum[nz0+ny0+nx0] - bmp->boxsum[nz0+ny1+nx0] - bmp->boxsum[nz0+ny0+nx1] + bmp->boxsum[nz0+ny1+nx1]);
	if (i ==                             0) return(0);
	if (i == (ox1-ox0)*(oy1-oy0)*(oz1-oz0)) return(2);
	return(1);
}
static void brush_bmp_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	//int x, y, z;
	brush_bmp_t *bmp = (brush_bmp_t *)brush;

	//x = ((bmp->iux*x0 + bmp->ivx*y0 + bmp->iwx*z0 + bmp->iox0[0])>>16);
	//y = ((bmp->iuy*x0 + bmp->ivy*y0 + bmp->iwy*z0 + bmp->ioy0[0])>>16);
	//z = ((bmp->iuz*x0 + bmp->ivz*y0 + bmp->iwz*z0 + bmp->ioz0[0])>>16);

	surf->b = 128;
	surf->g = 128;
	surf->r = 192;
	surf->a = 0;
	surf->norm[0] = 0;
	surf->norm[1] = 0;
	surf->norm[2] = 0;
	surf->tex = 0;
}

	//pp,pr,pd,pf is bmp's pos&ori in oct's coords
void brush_bmp_init (brush_bmp_t *bmp, unsigned short *boxsum, int xs, int ys, int zs, point3d *pp, point3d *pr, point3d *pd, point3d *pf)
{
	float f, mat[9];
	int i, ls, iux, iuy, iuz, ivx, ivy, ivz, iwx, iwy, iwz, iox, ioy, ioz;

	bmp->isins   = brush_bmp_isins;
	bmp->getsurf = brush_bmp_getsurf;
	bmp->boxsum = boxsum; bmp->xs = xs; bmp->ys = ys; bmp->zs = zs;

		//convert bmp to loct coords using:
		//x2 = x*pr->x + y*pd->x + z*pf->x + pp->x;
		//y2 = x*pr->y + y*pd->y + z*pf->y + pp->y;
		//z2 = x*pr->z + y*pd->z + z*pf->z + pp->z;
		//
		//convert loct to bmp coords using:
		//x = (x2-pp->x)*mat[0] + (y2-pp->y)*mat[1] + (z2-pp->z)*mat[2]
		//y = (x2-pp->x)*mat[3] + (y2-pp->y)*mat[4] + (z2-pp->z)*mat[5]
		//z = (x2-pp->x)*mat[6] + (y2-pp->y)*mat[7] + (z2-pp->z)*mat[8]
	invert3x3(pr,pd,pf,mat);
	bmp->iux = cvttss2si(mat[0]*65536.0); bmp->ivx = cvttss2si(mat[1]*65536.0); bmp->iwx = cvttss2si(mat[2]*65536.0);
	bmp->iuy = cvttss2si(mat[3]*65536.0); bmp->ivy = cvttss2si(mat[4]*65536.0); bmp->iwy = cvttss2si(mat[5]*65536.0);
	bmp->iuz = cvttss2si(mat[6]*65536.0); bmp->ivz = cvttss2si(mat[7]*65536.0); bmp->iwz = cvttss2si(mat[8]*65536.0);
	iox = cvttss2si((pp->x*mat[0] + pp->y*mat[1] + pp->z*mat[2])*-65536);
	ioy = cvttss2si((pp->x*mat[3] + pp->y*mat[4] + pp->z*mat[5])*-65536);
	ioz = cvttss2si((pp->x*mat[6] + pp->y*mat[7] + pp->z*mat[8])*-65536);
	for(ls=0;ls<OCT_MAXLS;ls++)
	{
		i = (1<<ls)-1;
		iux = bmp->iux*i; ivx = bmp->ivx*i; iwx = bmp->iwx*i;
		iuy = bmp->iuy*i; ivy = bmp->ivy*i; iwy = bmp->iwy*i;
		iuz = bmp->iuz*i; ivz = bmp->ivz*i; iwz = bmp->iwz*i;
		bmp->iox0[ls] = min(iux,0) + min(ivx,0) + min(iwx,0) + iox;
		bmp->iox1[ls] = max(iux,0) + max(ivx,0) + max(iwx,0) + iox + 65536;
		bmp->ioy0[ls] = min(iuy,0) + min(ivy,0) + min(iwy,0) + ioy;
		bmp->ioy1[ls] = max(iuy,0) + max(ivy,0) + max(iwy,0) + ioy + 65536;
		bmp->ioz0[ls] = min(iuz,0) + min(ivz,0) + min(iwz,0) + ioz;
		bmp->ioz1[ls] = max(iuz,0) + max(ivz,0) + max(iwz,0) + ioz + 65536;
	}
}

//--------------------------------------------------------------------------------------------------

	//+-------+-------+
	//|SOL    |    CHI|
	//|       |       |
	//|       |/^\    |
	//|      /|air \  |
	//+----/^\+----/^\+
	//|  / chi|\ /     \
	//|  \    |/^\ chi|/
	//|    \ /|sol \ /|
	//|CHI   \|    AIR|
	//+-------+\-/----+
typedef struct
{
	oct_t *loct[2];
	pgram3d_t bi[OCT_MAXLS*2+1];
	point3d p, r, d, f;
	oct_hit_t hit[2];
} oto_t;
static float gpow2[OCT_MAXLS*2+1] = {0.0};
static int oct_touch_oct_recur (oto_t *oto, octv_t *inode0, int x0, int y0, int z0, int ls0,
														  octv_t *inode1, int x1, int y1, int z1, int ls1, int splitmsk)
{
	double fx, fy, fz;
	int i, j, k, v, x, y, z, s0, s1, ind;

	i = (ls0 > 0) && (splitmsk&1);
	j = (ls1 > 0) && (splitmsk&2);

	if ((i) && (j) && (ls0 < ls1)) i = 0; //split larger box when there's choice (absolutely necessary optimization to reduce recursion count!)

	if (i) //split 0
	{
		ls0--; s0 = (1<<ls0); s1 = (1<<ls1); ind = inode0->ind;
		fx = s0*.5 - (x1+s1*.5)*oto->r.x - (y1+s1*.5)*oto->d.x - (z1+s1*.5)*oto->f.x - oto->p.x;
		fy = s0*.5 - (x1+s1*.5)*oto->r.y - (y1+s1*.5)*oto->d.y - (z1+s1*.5)*oto->f.y - oto->p.y;
		fz = s0*.5 - (x1+s1*.5)*oto->r.z - (y1+s1*.5)*oto->d.z - (z1+s1*.5)*oto->f.z - oto->p.z;
		for(v=(inode0->sol|inode0->chi);v;v^=i)
		{
			i = (-v)&v;
			x = x0; if (i&0xaa) x += s0;
			y = y0; if (i&0xcc) y += s0;
			z = z0; if (i&0xf0) z += s0;

			j = pgram3d_isint(&oto->bi[ls1-ls0+OCT_MAXLS],(x+fx)*gpow2[OCT_MAXLS-ls0]*4.0,
																		 (y+fy)*gpow2[OCT_MAXLS-ls0]*4.0,
																		 (z+fz)*gpow2[OCT_MAXLS-ls0]*4.0);
			if (!j) continue;

			k = splitmsk; if (inode0->sol&i) k &= ~1;
			if (oct_touch_oct_recur(oto,&((octv_t *)oto->loct[0]->nod.buf)[popcount[inode0->chi&(i-1)]+ind],x,y,z,ls0,inode1,x1,y1,z1,ls1,k)) return(1);
		}
		return(0);
	}

	if (j) //split 1
	{
		ls1--; s0 = (1<<ls0); s1 = (1<<ls1); ind = inode1->ind;
		fx = x0+s0*.5 - (s1*.5)*oto->r.x - (s1*.5)*oto->d.x - (s1*.5)*oto->f.x - oto->p.x;
		fy = y0+s0*.5 - (s1*.5)*oto->r.y - (s1*.5)*oto->d.y - (s1*.5)*oto->f.y - oto->p.y;
		fz = z0+s0*.5 - (s1*.5)*oto->r.z - (s1*.5)*oto->d.z - (s1*.5)*oto->f.z - oto->p.z;
		for(v=(inode1->sol|inode1->chi);v;v^=i)
		{
			i = (-v)&v;
			x = x1; if (i&0xaa) x += s1;
			y = y1; if (i&0xcc) y += s1;
			z = z1; if (i&0xf0) z += s1;

			j = pgram3d_isint(&oto->bi[ls1-ls0+OCT_MAXLS],(fx - x*oto->r.x - y*oto->d.x - z*oto->f.x)*gpow2[OCT_MAXLS-ls0]*4.0,
																		 (fy - x*oto->r.y - y*oto->d.y - z*oto->f.y)*gpow2[OCT_MAXLS-ls0]*4.0,
																		 (fz - x*oto->r.z - y*oto->d.z - z*oto->f.z)*gpow2[OCT_MAXLS-ls0]*4.0);
			if (!j) continue;

			k = splitmsk; if (inode1->sol&i) k &= ~2;
			if (oct_touch_oct_recur(oto,inode0,x0,y0,z0,ls0,&((octv_t *)oto->loct[1]->nod.buf)[popcount[inode1->chi&(i-1)]+ind],x,y,z,ls1,k)) return(1);
		}
		return(0);
	}

	oto->hit[0].x = x0; oto->hit[0].y = y0; oto->hit[0].z = z0; oto->hit[0].ls = ls0;
	oto->hit[1].x = x1; oto->hit[1].y = y1; oto->hit[1].z = z1; oto->hit[1].ls = ls1;
	return(1);
}
int oct_touch_oct (oct_t *bloct0, dpoint3d *bp0, dpoint3d *br0, dpoint3d *bd0, dpoint3d *bf0,
						 oct_t *bloct1, dpoint3d *bp1, dpoint3d *br1, dpoint3d *bd1, dpoint3d *bf1, oct_hit_t *hit)
{
	oto_t oto;
	octv_t *inode;
	oct_t *loct0, *loct1;
	dpoint3d p0, r0, d0, f0, p1, r1, d1, f1;
	dpoint3d cr, cd, cf, dr, dd, df;
	double f, hx, hy, hz, hx2, hy2, hz2, nx, ny, nz, nx2, ny2, nz2, nx3, ny3, nz3, mat[9];
	int i, s, v, x, y, z, ind, i0, i1, ls0, ls1;

	if (gpow2[0] == 0.0) //FIX:should init elsewhere
	{
		gpow2[OCT_MAXLS] = 1.0;
		for(i=OCT_MAXLS+1;i<=OCT_MAXLS*2;i++) gpow2[i] = gpow2[i-1]*2.0;
		for(i=OCT_MAXLS-1;i>=          0;i--) gpow2[i] = gpow2[i+1]*0.5;
	}

		//Make sure that oct_touch_oct_recur() chooses to split 0 on 1st call
	if (bloct0->lsid >= bloct1->lsid)
	{
		loct0 = bloct0; p0 = (*bp0); r0 = (*br0); d0 = (*bd0); f0 = (*bf0);
		loct1 = bloct1; p1 = (*bp1); r1 = (*br1); d1 = (*bd1); f1 = (*bf1);
	}
	else
	{
		loct0 = bloct1; p0 = (*bp1); r0 = (*br1); d0 = (*bd1); f0 = (*bf1);
		loct1 = bloct0; p1 = (*bp0); r1 = (*br0); d1 = (*bd0); f1 = (*bf0);
	}

		//FIXFIXFIXFIX:can remove this inverse now that pgram3d handles it!
		//transform loct1 to make loct0 identity (p0=<0,0,0>, r0=<1,0,0>, d0=<0,1,0>, f0=<0,0,1>)
		//oto = loct0^-1 * loct1
	invert3x3(&r0,&d0,&f0,mat);
	oto.r.x = r1.x*mat[0] + r1.y*mat[1] + r1.z*mat[2];
	oto.r.y = r1.x*mat[3] + r1.y*mat[4] + r1.z*mat[5];
	oto.r.z = r1.x*mat[6] + r1.y*mat[7] + r1.z*mat[8];
	oto.d.x = d1.x*mat[0] + d1.y*mat[1] + d1.z*mat[2];
	oto.d.y = d1.x*mat[3] + d1.y*mat[4] + d1.z*mat[5];
	oto.d.z = d1.x*mat[6] + d1.y*mat[7] + d1.z*mat[8];
	oto.f.x = f1.x*mat[0] + f1.y*mat[1] + f1.z*mat[2];
	oto.f.y = f1.x*mat[3] + f1.y*mat[4] + f1.z*mat[5];
	oto.f.z = f1.x*mat[6] + f1.y*mat[7] + f1.z*mat[8];
	nx = p1.x-p0.x; ny = p1.y-p0.y; nz = p1.z-p0.z;
	oto.p.x = nx*mat[0] + ny*mat[1] + nz*mat[2];
	oto.p.y = nx*mat[3] + ny*mat[4] + nz*mat[5];
	oto.p.z = nx*mat[6] + ny*mat[7] + nz*mat[8];

	oto.loct[0] = loct0;
	oto.loct[1] = loct1;

		//FIX:optimize by precalcing only for: -max(loct0->lsid,loct1->lsid) .. +max(loct0->lsid,loct1->lsid)
	ls0 = loct0->lsid; ls1 = loct1->lsid;
	cr.x = 2.0; cr.y = 0.0; cr.z = 0.0;
	cd.x = 0.0; cd.y = 2.0; cd.z = 0.0;
	cf.x = 0.0; cf.y = 0.0; cf.z = 2.0;
	for(i=OCT_MAXLS-ls0;i<=OCT_MAXLS+ls1;i++)
	{
		f = gpow2[i]*2.0;
		dr.x = oto.r.x*f; dr.y = oto.r.y*f; dr.z = oto.r.z*f;
		dd.x = oto.d.x*f; dd.y = oto.d.y*f; dd.z = oto.d.z*f;
		df.x = oto.f.x*f; df.y = oto.f.y*f; df.z = oto.f.z*f;
		pgram3d_init(&oto.bi[i],&cr,&cd,&cf,&dr,&dd,&df);
	}

		//full size early out check
	if (!pgram3d_isint(&oto.bi[ls1-ls0+OCT_MAXLS],((1<<ls0)*.5 - ((1<<ls1)*.5*oto.r.x + (1<<ls1)*.5*oto.d.x + (1<<ls1)*.5*oto.f.x + oto.p.x))*gpow2[OCT_MAXLS-ls0]*4.0,
																 ((1<<ls0)*.5 - ((1<<ls1)*.5*oto.r.y + (1<<ls1)*.5*oto.d.y + (1<<ls1)*.5*oto.f.y + oto.p.y))*gpow2[OCT_MAXLS-ls0]*4.0,
																 ((1<<ls0)*.5 - ((1<<ls1)*.5*oto.r.z + (1<<ls1)*.5*oto.d.z + (1<<ls1)*.5*oto.f.z + oto.p.z))*gpow2[OCT_MAXLS-ls0]*4.0)) return(0);

	s = ((1<<ls1)>>1); inode = &((octv_t *)loct1->nod.buf)[loct1->head]; ind = inode->ind;
	for(v=(inode->sol|inode->chi);v;v^=i)
	{
		i = (-v)&v;
		x = 0; if (i&0xaa) x = s;
		y = 0; if (i&0xcc) y = s;
		z = 0; if (i&0xf0) z = s;
		if (!oct_touch_oct_recur(&oto,&((octv_t *)loct0->nod.buf)[loct0->head],0,0,0,ls0,&((octv_t *)loct1->nod.buf)[popcount[inode->chi&(i-1)]+ind],x,y,z,ls1-1,3)) continue;
		if (hit)
		{
			if (bloct0->lsid >= bloct1->lsid) { hit[0] = oto.hit[0]; hit[1] = oto.hit[1]; } //must preserve order for return value!
												  else { hit[0] = oto.hit[1]; hit[1] = oto.hit[0]; }
		}
		return(1);
	}
	return(0);
}
int oct_touch_oct (oct_t *bloct0, point3d *bp0, point3d *br0, point3d *bd0, point3d *bf0,
						 oct_t *bloct1, point3d *bp1, point3d *br1, point3d *bd1, point3d *bf1, oct_hit_t *hit)
{
	dpoint3d dp0, dr0, dd0, df0, dp1, dr1, dd1, df1;
	dp0.x = (double)bp0->x; dp0.y = (double)bp0->y; dp0.z = (double)bp0->z;
	dr0.x = (double)br0->x; dr0.y = (double)br0->y; dr0.z = (double)br0->z;
	dd0.x = (double)bd0->x; dd0.y = (double)bd0->y; dd0.z = (double)bd0->z;
	df0.x = (double)bf0->x; df0.y = (double)bf0->y; df0.z = (double)bf0->z;
	dp1.x = (double)bp1->x; dp1.y = (double)bp1->y; dp0.z = (double)bp0->z;
	dr1.x = (double)br1->x; dr1.y = (double)br1->y; dr0.z = (double)br0->z;
	dd1.x = (double)bd1->x; dd1.y = (double)bd1->y; dd0.z = (double)bd0->z;
	df1.x = (double)bf1->x; df1.y = (double)bf1->y; df0.z = (double)bf0->z;
	return(oct_touch_oct(bloct0,&dp0,&dr0,&dd0,&df0,bloct1,&dp1,&dr1,&dd1,&df1,hit));
}

//--------------------------------------------------------------------------------------------------
	//02/22/2012 floodfill algo:
	//for (each unmarked solid in bbox)
	//{
	//   while (floodfill)
	//   {
	//      if ((mrk2) || (hit bottom)) { oct_copymrk2mrk2(); exit_early; }
	//      if (mrk) continue;
	//      mrk = 1;
	//      write_fif(x,y,z,ls);
	//   }
	//   if (didn't exit_early)
	//   {
	//      zot or gen sprite;
	//      oct_removemrks();
	//   }
	//}
	//oct_clearmrk2s();

typedef struct { unsigned short x, y, z; char ls, skipdir; } ffif_t;
#define FFIFMAX 65536 //FIX:make dynamic!
static ffif_t ffif[FFIFMAX];

	//returns: 0:not hover (connected to ground/out of mem/invalid nuc), 1:is hover
static int oct_floodfill (oct_t *loct, int nx, int ny, int nz)
{
	oct_getvox_hint_t och;
	octv_t *ptr;
	static const int wwadd[3][3] = {2,0,1, 0,2,1, 0,1,2};
	const int *pwwadd;
	int i, c, s, ls, x, y, z, skipdir, ww[3], ffifn;
	ffif_t *pfif;

	if ((nx|ny|nz)&loct->nsid) return(0); //out of bounds
	oct_getvox_hint_init(loct,&och);
	ffifn = 0; ww[0] = 0; ww[1] = 0; s = 0; c = -1; skipdir = -1; goto in2it;
	do
	{
		ffifn--; pfif = &ffif[ffifn]; x = pfif->x; y = pfif->y; z = pfif->z; s = pow2[pfif->ls]; skipdir = pfif->skipdir;

		for(c=6-1;c>=0;c--) //visit all neighbors of octree node (x,y,s)
		{
			if (skipdir == c) continue;
			ww[0] = 0; ww[1] = 0; ww[2] = ((c&1)-1)|s; pwwadd = &wwadd[c>>1][0];
			nx = ww[pwwadd[0]]+x;
			ny = ww[pwwadd[1]]+y;
			nz = ww[pwwadd[2]]+z; if ((nx|ny|nz)&loct->nsid) continue;
			do
			{
in2it:;     i = oct_getsol_hint(loct,nx,ny,nz,&och);
				if (i) //(nx&(-och.mins),ny&(-och.mins),nz&(-och.mins),och.mins) is neighbor
				{
					ptr = &((octv_t *)loct->nod.buf)[och.stkind[och.minls]];
					if (ptr->mrk2&i) return(0); //if hit previous grounded floodfill, exit
					if (!(ptr->mrk&i))
					{
						if (ny+och.mins >= loct->sid) return(0); //hit bottom; exit early

							//add neighbor to local fif
						if (ffifn >= FFIFMAX) return(0); //out of mem :/
						pfif = &ffif[ffifn]; pfif->x = nx&(-och.mins); pfif->y = ny&(-och.mins); pfif->z = nz&(-och.mins);
						pfif->ls = och.minls; pfif->skipdir = (c^1)|((s-och.mins)>>31); ffifn++;

						ptr->mrk |= i; //mark node
						for(ls=och.minls+1;ls<loct->lsid;ls++)
						{
							ptr = &((octv_t *)loct->nod.buf)[och.stkind[ls]];
							i = 1;
							if (nx&(1<<ls)) i <<= 1;
							if (ny&(1<<ls)) i <<= 2;
							if (nz&(1<<ls)) i <<= 4;
							if (ptr->mrk&i) break; ptr->mrk |= i;
						}
					}
				}

				mort2add(och.mins,ww[0],ww[1]); if (ww[0] >= s) break;
				nx = ww[pwwadd[0]]+x;
				ny = ww[pwwadd[1]]+y;
				nz = ww[pwwadd[2]]+z;
			} while (1);
		}
	} while (ffifn > 0);
	return(1);
}

static void oct_copymrk2mrk2 (oct_t *loct, int inode, int ls) //copy mrk to mrk2
{
	octv_t *ptr;
	int iup, v;

	ptr = &((octv_t *)loct->nod.buf)[inode];
	if (ls)
	{
		for(v=ptr->chi&ptr->mrk;v;v^=iup)
		{
			iup = (-v)&v;
			oct_copymrk2mrk2(loct,popcount[ptr->chi&(iup-1)]+ptr->ind,ls-1);
		}
	}
	ptr->mrk2 |= ptr->mrk; ptr->mrk = 0;
}

static void oct_removemrks (oct_t *loct, int inode, int ls, octv_t *roct)
{
	surf_t surf[8];
	octv_t *ooct, noct[8];
	int iup, n, o, v;

	ooct = &((octv_t *)loct->nod.buf)[inode];
	roct->sol  = ooct->sol &~ooct->mrk;
	roct->chi  = ooct->chi &~ooct->mrk;
	roct->mrk  = 0;
	roct->mrk2 = ooct->mrk2&~ooct->mrk;
	o = ooct->ind; n = 0;
	if (!ls)
	{
		for(v=ooct->chi;v;v^=iup,o++) //visit only nodes that may differ from brush color
		{
			iup = (-v)&v;
			if (!(ooct->mrk&iup)) { memcpy(&surf[n],&((surf_t *)loct->sur.buf)[o],loct->sur.siz); n++; continue; } //no intersect:copy
		}
		o = popcount[ooct->chi]; roct->ind = ooct->ind;
		xorzrangesmall(loct->sur.bit,ooct->ind+n,o-n); //shorten node
		memcpy(&((surf_t *)loct->sur.buf)[ooct->ind],surf,n*loct->sur.siz);
		loct->sur.num += n-o;
	}
	else
	{
		for(v=ooct->chi;v;v^=iup,o++) //visit only nodes that may differ from brush color
		{
			iup = (-v)&v;
			if (!(ooct->mrk&iup)) { noct[n] = ((octv_t *)loct->nod.buf)[o]; n++; continue; } //no intersect:copy
			if (ooct->sol&iup) { oct_dealloctree(loct,ls-1,((octv_t *)loct->nod.buf)[o].ind,((octv_t *)loct->nod.buf)[o].chi); continue; } //clear all
			oct_removemrks(loct,o,ls-1,&noct[n]); //intersects partially:recurse
			if (noct[n].chi || ((unsigned)(noct[n].sol-1) < (unsigned)((1<<8)-2))) { roct->chi += iup; roct->mrk2 += (ooct->mrk2&iup); n++; }
		}
		o = popcount[ooct->chi]; roct->ind = ooct->ind;
		xorzrangesmall(loct->nod.bit,ooct->ind+n,o-n); //shorten node
		memcpy(&((octv_t *)loct->nod.buf)[ooct->ind],noct,n*loct->nod.siz);
		loct->nod.num += n-o;
	}
}

	//copies marked (mrk) sections of loct to newoct; newoct must be fresh from an oct_new()
static void oct_mark2spr (oct_t *loct, oct_t *newoct, int inode, int ls, octv_t *roct, int mskor)
{
	surf_t surf[8];
	octv_t *ooct, noct[8];
	int iup, n, o, v, omrk;

	ooct = &((octv_t *)loct->nod.buf)[inode]; omrk = ooct->mrk|mskor;
	roct->sol = ooct->sol&omrk; roct->chi = 0; roct->mrk = 0; roct->mrk2 = 0;
	n = 0;
	if (!ls)
	{
		for(v=(ooct->chi&omrk);v;v^=iup) //visit only nodes that may differ from brush color
		{
			iup = (-v)&v; o = popcount[ooct->chi&(iup-1)] + ooct->ind;
			memcpy(&surf[n],&((surf_t *)loct->sur.buf)[o],loct->sur.siz);
			roct->chi += iup; n++;
		}
		if (!n) { roct->ind = -1; return; }
		if (newoct->sur.num+n > newoct->sur.mal)
		{
			if (!oct_usegpu)
			{
				newoct->sur.mal = max(((1<<bsr(newoct->sur.mal))>>2) + newoct->sur.mal,newoct->sur.num+n); //grow space by ~25%
			}
			else
			{
					//grow space by 100%
				if (newoct->glxsid < newoct->glysid) { newoct->glxsid++; newoct->gxsid <<= 1; }
														  else { newoct->glysid++; newoct->gysid <<= 1; }
				newoct->sur.mal <<= 1;
			}
			newoct->sur.buf = (octv_t       *)realloc(newoct->sur.buf,   (__int64)newoct->sur.mal*newoct->sur.siz);
		}
		roct->ind = newoct->sur.num; newoct->sur.num += n; //simple allocator (don't need bitalloc since no deallocation)
		memcpy(&((surf_t *)newoct->sur.buf)[roct->ind],surf,n*newoct->sur.siz);
	}
	else
	{
		for(v=(ooct->chi&omrk);v;v^=iup) //visit only nodes that may differ from brush color
		{
			iup = (-v)&v; o = popcount[ooct->chi&(iup-1)] + ooct->ind;
			oct_mark2spr(loct,newoct,o,ls-1,&noct[n],(-(ooct->sol&iup))>>8); //intersects partially:recurse
			if (noct[n].chi || ((unsigned)(noct[n].sol-1) < (unsigned)((1<<8)-2))) { roct->chi += iup; n++; }
		}
		if (!n) { roct->ind = -1; return; }
		if (newoct->nod.num+n > newoct->nod.mal)
		{
			newoct->nod.mal = max(((1<<bsr(newoct->nod.mal))>>2) + newoct->nod.mal,newoct->nod.num+n); //grow space by ~25%
			newoct->nod.buf = (octv_t       *)realloc(newoct->nod.buf,   (__int64)newoct->nod.mal*newoct->nod.siz);
		}
		roct->ind = newoct->nod.num; newoct->nod.num += n; //simple allocator (don't need bitalloc since no deallocation)
		memcpy(&((octv_t *)newoct->nod.buf)[roct->ind],noct,n*newoct->nod.siz);
	}
}

static void oct_clearmrk2s (oct_t *loct, int inode, int ls)
{
	octv_t *ptr;
	int iup, v;

	ptr = &((octv_t *)loct->nod.buf)[inode];
	if (ls)
	{
		for(v=ptr->chi&ptr->mrk2;v;v^=iup)
		{
			iup = (-v)&v;
			oct_clearmrk2s(loct,popcount[ptr->chi&(iup-1)]+ptr->ind,ls-1);
		}
	}
	ptr->mrk2 = 0;
}

	//See MORTCMP.C for derivation
__forceinline static int mortcmp_fast (int x0, int y0, int z0, int x1, int y1, int z1)
{
#if 1 //7.83ns
	int i0, i1, i2, i3;
	i0 = x0^x1;
	i1 = y0^y1;
	i2 = z0^z1;
	i3 = i0|i1;
	return ( ((((z0-z1)>>31)&-4) - (((z1-z0)>>31)&-4)) & ((((i3^i2)&i3)-i2)>>31) ) +
			 ( ((((y0-y1)>>31)&-2) - (((y1-y0)>>31)&-2)) & ((((i0^i1)&i0)-i1)>>31) ) +
			 ( ((((x0-x1)>>31)   ) - (((x1-x0)>>31)   ))                           );
#else //5.92ns
	__declspec(align(16)) static const int dqzero[4] = {0,0,0,0};
	_asm
	{
		movd xmm0, x0
		movd xmm1, x1
		movd xmm2, y0
		movd xmm3, y1
		punpckldq xmm0, xmm2
		punpckldq xmm1, xmm3
		movd xmm2, z0
		movd xmm3, z1
		movlhps xmm0, xmm2      ;xmm0:[ 0 z0 y0 x0]
		movlhps xmm1, xmm3      ;xmm1:[ 0 z1 y1 x1]

		movaps xmm2, xmm0
		pxor xmm2, xmm1         ;xmm2:[ 0 i2 i1 i0]

		pshufd xmm3, xmm2, 0x3f ;xmm3:[i0  0  0  0]
		pshufd xmm2, xmm2, 0x64 ;xmm3:[i1 i2 i1 i0]
		por xmm2, xmm3          ;xmm2:[i3 i2 i1 i0]

		pshufd xmm3, xmm2, 0xb1 ;xmm3:[i2 i3 i0 i1]
		movaps xmm4, xmm3       ;xmm4:[i2 i3 i0 i1]
		pxor xmm3, xmm2         ;xmm3:[i2^i3 i2^i3 i0^i1 i0^i1]
		pand xmm3, xmm4         ;xmm3:[(i2^i3)&i2 (i2^i3)&i3 (i0^i1)&i0 (i0^i1)&i1]
		pcmpgtd xmm2, xmm3      ;xmm2:[((i2^i3)&i2)<i3 ((i3^i2)&i3)<i2 ((i0^i1)&i0)<i1 ((i0^i1)&i1)<i0]

		psubd xmm1, xmm0        ;xmm1:[0 z1-z0 y1-y0 x1-x0]
		pand xmm1, xmm2         ;xmm1:[0 (z1-z0)&? (y1-y0)&? (x1-x0)&?]
		movmskps eax, xmm1
		pcmpgtd xmm1, dqzero    ;xmm1:[0 (z0-z1)&? (y0-y1)&? (x0-x1)&?]
		movmskps edx, xmm1

		sub eax, edx
	}
#endif
}

void oct_hover_check (oct_t *loct, int x0, int y0, int z0, int x1, int y1, int z1, void (*recvoctfunc)(oct_t *ooct, oct_t *noct))
{
	typedef struct { int ind, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	oct_t newoct;
	octv_t nnode, *ptr;
	int i, j, ls, s, x, y, z, nx, ny, nz, didx, didy, didz, ind;

	didx = 0; didy = 0; didz = 0;
ohc_restart:;
	ls = loct->lsid-1; s = (1<<ls); ind = loct->head; ptr = &((octv_t *)loct->nod.buf)[ind]; x = 0; y = 0; z = 0; j = 0; //WARNING:j must ascend for morton comparison to work!
	while (1)
	{
		i = (1<<j);
		nx = ((-((j   )&1))&s)+x; if ((nx > x1) || (nx+s < x0)) goto tosibly;
		ny = ((-((j>>1)&1))&s)+y; if ((ny > y1) || (ny+s < y0)) goto tosibly;
		nz = ((-((j>>2)&1))&s)+z; if ((nz > z1) || (nz+s < z0)) goto tosibly;

		if (ptr->sol&i)
		{
			if ((ptr->mrk|ptr->mrk2)&i) goto tosibly;
			if (mortcmp_fast(nx,ny,nz,didx,didy,didz) < 0) goto tosibly;

			if (!oct_floodfill(loct,nx,ny,nz)) //returns:0=hit bottom/out of mem, 1=is hover
			{
				oct_copymrk2mrk2(loct,loct->head,loct->lsid-1);
			}
			else
			{
				if (recvoctfunc)
				{
					oct_new(&newoct,loct->lsid,loct->tilid,256,256,1);

					oct_mark2spr(loct,&newoct,loct->head,loct->lsid-1,&nnode,0);
					((octv_t *)newoct.nod.buf)[newoct.head] = nnode; //can't write node directly to loct->nod.buf[loct->head] because of possible realloc inside

					//if ((newoct.nod.num <= 1) && (newoct.sur.num <= 1)) MessageBox(ghwnd,"Blank sprite :/",prognam,MB_OK);

					newoct.nod.bit = (unsigned int *)malloc(((((__int64)newoct.nod.mal+63)>>5)<<2)+16);
					newoct.nod.ind = newoct.nod.num;
					setzrange1(newoct.nod.bit,             0,newoct.nod.num);
					setzrange0(newoct.nod.bit,newoct.nod.num,newoct.nod.mal);

					newoct.sur.bit = (unsigned int *)malloc(((((__int64)newoct.sur.mal+63)>>5)<<2)+16);
					newoct.sur.ind = newoct.sur.num;
					setzrange1(newoct.sur.bit,             0,newoct.sur.num);
					setzrange0(newoct.sur.bit,newoct.sur.num,newoct.sur.mal);

					if (oct_usegpu)
					{
						kglalloctex(newoct.octid,0,newoct.gxsid,newoct.gysid,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here!
#if (GPUSEBO == 0)
						glBindTexture(GL_TEXTURE_2D,newoct.octid);
						glTexSubImage2D(GL_TEXTURE_2D,0,0,0,newoct.gxsid,(newoct.sur.num*2+newoct.gxsid-1)>>newoct.glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)newoct.sur.buf);
#else
						newoct.bufid = bo_init(newoct.gxsid*newoct.gysid*4);
						newoct.gsurf = (surf_t *)bo_begin(newoct.bufid,0);
						memcpy(newoct.gsurf,newoct.sur.buf,newoct.sur.num*newoct.sur.siz);
#endif
					}
					recvoctfunc(loct,&newoct);
				}
				oct_removemrks(loct,loct->head,loct->lsid-1,&nnode);
				((octv_t *)loct->nod.buf)[loct->head] = nnode; //can't write node directly to loct->nod.buf[loct->head] because of possible realloc inside

				didx = nx; didy = ny; didz = nz;
				goto ohc_restart;
			}
			goto tosibly;
		}
		if (ptr->chi&i)
		{
			stk[ls].ind = ind; stk[ls].j = j; ls--; s >>= 1; //2child
			ind = popcount[ptr->chi&(i-1)] + ptr->ind; ptr = &((octv_t *)loct->nod.buf)[ind]; x = nx; y = ny; z = nz; j = 0;
			continue;
		}

tosibly:;
		j++; if (j < 8) continue;
		do { s <<= 1; ls++; if (ls >= loct->lsid) goto endit; j = stk[ls].j+1; } while (j >= 8); //2parent
		ind = stk[ls].ind; ptr = &((octv_t *)loct->nod.buf)[ind]; i = -(s<<1); x &= i; y &= i; z &= i;
	}
endit:;
	oct_clearmrk2s(loct,loct->head,loct->lsid-1);
	oct_checkreducesizes(loct);
}
//--------------------------------------------------------------------------------------------------
	//              CPU:  GPU:
	//pnd3d /thr=1: 290ms 312ms
	//voxed /thr=1: 192ms
	//pnd3d /thr=2: 145ms 158ms
	//pnd3d /thr=3: 133ms 143ms
	//pnd3d /thr=4:  91ms 109ms
	//pnd3d /thr=5:  87ms 105ms
	//pnd3d /thr=6:  67ms  85ms
	//pnd3d /thr=7:  65ms  xxms
	//pnd3d /thr=8:  62ms  xxms

	//new algo (surface following)
#if (USENEWCAC == 0)
void oct_bitcac_reset (oct_bitcac_t *bitcac) { bitcac->pt.x = 0x80000000; }
#else
void oct_bitcac_reset (oct_bitcac_t *bitcac) { bitcac->n = 0; memset(bitcac->hashead,-1,CACHASHN*sizeof(int)); }
#endif
void oct_estnorm (oct_t *loct, int nx, int ny, int nz, int rad, point3d *norm, oct_bitcac_t *bitcac)
{
	float f;
	int i, j, x, y, z, dx, dy, dz;

#if 1
		//Voxlap algo (faster)
	static const int bitsnum[32] =
	{
		0        ,1-(2<<16),1-(1<<16),2-(3<<16),
		1        ,2-(2<<16),2-(1<<16),3-(3<<16),
		1+(1<<16),2-(1<<16),2        ,3-(2<<16),
		2+(1<<16),3-(1<<16),3        ,4-(2<<16),
		1+(2<<16),2        ,2+(1<<16),3-(1<<16),
		2+(2<<16),3        ,3+(1<<16),4-(1<<16),
		2+(3<<16),3+(1<<16),3+(2<<16),4,
		3+(3<<16),4+(1<<16),4+(2<<16),5
	};
	unsigned int bitsol[32*32], *bitsolp, *bitsolp2;
	int xx, yy, zz, dia, b[5], hash;

	if (bitcac)
	{
		dia = 32; i = 32-rad*2;
#if (USENEWCAC == 0)
		if (((unsigned)(nx-rad-bitcac->pt.x) >= (unsigned)i) ||
			 ((unsigned)(ny-rad-bitcac->pt.y) >= (unsigned)i) ||
			 ((unsigned)(nz-rad-bitcac->pt.z) >= (unsigned)i))
		{
			bitcac->pt.x = (nx-rad-6)&-8;
			bitcac->pt.y = (ny-rad-6)&-8;
			bitcac->pt.z = (nz-rad-6)&-8;
			oct_sol2bit(loct,bitcac->buf,bitcac->pt.x,bitcac->pt.y,bitcac->pt.z,32,32,32,1);
		}
		x = nx-bitcac->pt.x; y = ny-bitcac->pt.y; z = nz-bitcac->pt.z; bitsolp = bitcac->buf;
#else
		if ((!bitcac->n) ||
			 ((unsigned)(nx-rad-bitcac->pt[bitcac->cur].x) >= (unsigned)i) ||
			 ((unsigned)(ny-rad-bitcac->pt[bitcac->cur].y) >= (unsigned)i) ||
			 ((unsigned)(nz-rad-bitcac->pt[bitcac->cur].z) >= (unsigned)i))
		{
			x = (nx-rad-6)&-8;
			y = (ny-rad-6)&-8;
			z = (nz-rad-6)&-8;

			hash = ((x>>3) + (y>>3)*3 + (z>>3)*5)&(CACHASHN-1);
			bitcac->cur = -1;
			for(i=bitcac->hashead[hash];i>=max(bitcac->n-CACN,0);i=bitcac->ptn[i&(CACN-1)])
				if ((bitcac->pt[i].x == x) && (bitcac->pt[i].y == y) && (bitcac->pt[i].z == z)) { bitcac->cur = (i&(CACN-1)); break; }
			if (bitcac->cur < 0)
			{
				bitcac->cur = (bitcac->n&(CACN-1));
				bitcac->ptn[bitcac->cur] = bitcac->hashead[hash]; bitcac->hashead[hash] = bitcac->n; bitcac->n++;
				bitcac->pt[bitcac->cur].x = x;
				bitcac->pt[bitcac->cur].y = y;
				bitcac->pt[bitcac->cur].z = z;
				oct_sol2bit(loct,bitcac->buf[bitcac->cur],x,y,z,32,32,32,1);
			}
		}
		x = nx-bitcac->pt[bitcac->cur].x;
		y = ny-bitcac->pt[bitcac->cur].y;
		z = nz-bitcac->pt[bitcac->cur].z; bitsolp = bitcac->buf[bitcac->cur];
#endif
	}
	else
	{
		dia = rad*2+1;
		oct_sol2bit(loct,(unsigned int *)bitsol,nx-rad,ny-rad,nz-rad,dia,dia,dia,1);
		x = rad; y = rad; z = rad; bitsolp = bitsol;
	}

	dx = 0; dy = 0; dz = 0;
	if (rad == 2)
	{
		bitsolp2 = &bitsolp[(z-2)*dia + y-2]; i = x-2;
		for(yy=-2;yy<=2;yy++)
		{
			b[0] = ((bitsolp2[0]>>i)&31);
			b[1] = ((bitsolp2[1]>>i)&31);
			b[2] = ((bitsolp2[2]>>i)&31);
			b[3] = ((bitsolp2[3]>>i)&31);
			b[4] = ((bitsolp2[4]>>i)&31); bitsolp2 += dia;

			dy += (popcount[b[4]]-popcount[b[0]])*2 + popcount[b[3]]-popcount[b[1]];
			j = bitsnum[b[0]] + bitsnum[b[1]] + bitsnum[b[2]] + bitsnum[b[3]] + bitsnum[b[4]];
			dx += j; dz += (*(signed short *)&j)*yy;
		}
		dx >>= 16;
	}
	else
	{
		for(xx=-2;xx<=2;xx++)
			for(yy=-2;yy<=2;yy++)
				for(zz=-2;zz<=2;zz++)
					if (bitsolp[(z+zz)*dia + (y+yy)]&(1<<(x+xx))) { dx += xx; dy += yy; dz += zz; }
	}
#else
		//New algo: search surfaces (bfs) - not faster, but better quality!
		//be careful: rad must be <= 8!
		//12 edges + 6 faces = 18
	static const int dirx[18] = { 0, 0,-1,+1, 0, 0,   0,-1,+1, 0, -1,+1,-1,+1,  0,-1,+1, 0};
	static const int diry[18] = { 0,-1, 0, 0,+1, 0,  -1, 0, 0,+1, -1,-1,+1,+1, -1, 0, 0,+1};
	static const int dirz[18] = {-1, 0, 0, 0, 0,+1,  -1,-1,-1,-1,  0, 0, 0, 0, +1,+1,+1,+1};

	#define ESFIFMAX 4096
	typedef struct { int x, y, z, d; } fif_t;
	fif_t fif[ESFIFMAX];
	int xx, yy, zz, dia, fifw, fifr;
	unsigned int bitsol[32*32], *bitsolp, *bitsolp2, bitgot[32*32];
	static const int distweight[] = //must have >= rad+1 entries defined
	{
		(int)(exp(0.0*0.0*-.1)*65536.0), //65536
		(int)(exp(1.0*1.0*-.1)*65536.0), //59299
		(int)(exp(2.0*2.0*-.1)*65536.0), //43930
		(int)(exp(3.0*3.0*-.1)*65536.0), //26644
		(int)(exp(4.0*4.0*-.1)*65536.0), //13231
		(int)(exp(5.0*5.0*-.1)*65536.0), // 5379
		(int)(exp(6.0*6.0*-.1)*65536.0), // 1790
		(int)(exp(7.0*7.0*-.1)*65536.0), //  488
		(int)(exp(8.0*8.0*-.1)*65536.0), //  108
	};

		//       face:       |       edge:      |  edge#2:
		//  pass:     fail:  |  pass:    fail:  |   fail:
		//                   |     X        .   |      X
		//   . .       . X   |   . 1 X    X 1 . |    . 1 X
		// X 0 1 X   X 0 1 . | X 0 X    . 0 X   |  X 0 .
		//   X X       . X   |   X        .     |    X

		//  6    10  1 11     14
		//7 0 8   2     3  15  5 16
		//  9    12  4 13     17
		//
		//0 1 2   9 10 11  18 19 20
		//3 4 5  12 13 14  21 22 23
		//6 7 8  15 16 17  24 25 26
	static const int neighmsk[18][3] =
	{
			//6 faces
		(1<< 4),9,(1<< 1)+(1<< 3)+(1<< 5)+(1<< 7),
		(1<<10),3,(1<< 1)+(1<< 9)+(1<<11)+(1<<19),
		(1<<12),1,(1<< 3)+(1<< 9)+(1<<15)+(1<<21),
		(1<<14),1,(1<< 4)+(1<<10)+(1<<16)+(1<<22),
		(1<<16),3,(1<< 4)+(1<<12)+(1<<14)+(1<<22),
		(1<<22),9,(1<<10)+(1<<12)+(1<<14)+(1<<16),

			//12 edges
		(1<< 1),(1<< 4),(1<<10),
		(1<< 3),(1<< 4),(1<<12),
		(1<< 5),(1<< 4),(1<<14),
		(1<< 7),(1<< 4),(1<<16),
		(1<< 9),(1<<10),(1<<12),
		(1<<11),(1<<10),(1<<14),
		(1<<15),(1<<12),(1<<16),
		(1<<17),(1<<14),(1<<16),
		(1<<19),(1<<10),(1<<22),
		(1<<21),(1<<12),(1<<22),
		(1<<23),(1<<14),(1<<22),
		(1<<25),(1<<16),(1<<22),
	};

	if (bitcac)
	{
		dia = 32;
		if ((nx-rad-1 < bitcac->pt.x) || (nx+rad+1 >= bitcac->pt.x+32) ||
			 (ny-rad-1 < bitcac->pt.y) || (ny+rad+1 >= bitcac->pt.y+32) ||
			 (nz-rad-1 < bitcac->pt.z) || (nz+rad+1 >= bitcac->pt.z+32))
		{
			bitcac->pt.x = ((nx-rad-1)-5)&-8;
			bitcac->pt.y = ((ny-rad-1)-5)&-8;
			bitcac->pt.z = ((nz-rad-1)-5)&-8;
			oct_sol2bit(loct,bitcac->buf,bitcac->pt.x,bitcac->pt.y,bitcac->pt.z,32,32,32,1);
		}

		x = (nx-rad-1)-bitcac->pt.x;
		y = (ny-rad-1)-bitcac->pt.y;
		z = (nz-rad-1)-bitcac->pt.z;

		memset(&bitgot[z*32],0,(rad*2+3)*32*sizeof(bitgot[0]));
		x += rad+1; y += rad+1; z += rad+1; bitsolp = bitcac->buf;
	}
	else
	{
		dia = rad*2+3;
		oct_sol2bit(loct,(unsigned int *)bitsol,nx-rad-1,ny-rad-1,nz-rad-1,dia,dia,dia,1);
		memset(bitgot,0,dia*dia*sizeof(bitgot[0]));
		x = rad+1; y = rad+1; z = rad+1; bitsolp = bitsol;
	}

	dx = 0; dy = 0; dz = 0;
	bitgot[z*dia+y] |= (1<<x);
	fif[0].x = x; fif[0].y = y; fif[0].z = z; fif[0].d = 0; fifw = 1;
	for(fifr=0;fifr<fifw;fifr++)
	{
		x = fif[fifr].x; y = fif[fifr].y; z = fif[fifr].z;

			//0 1 2   9 10 11  18 19 20
			//3 4 5  12 13 14  21 22 23
			//6 7 8  15 16 17  24 25 26
		bitsolp2 = &bitsolp[z*dia + y]; i = x-1;
		j = (((bitsolp2[-dia-1]>>i)&7)    ) + (((bitsolp2[-dia+0]>>i)&7)<< 3) + (((bitsolp2[-dia+1]>>i)&7)<< 6) +
			 (((bitsolp2[    -1]>>i)&7)<< 9) + (((bitsolp2[    +0]>>i)&7)<<12) + (((bitsolp2[    +1]>>i)&7)<<15) +
			 (((bitsolp2[+dia-1]>>i)&7)<<18) + (((bitsolp2[+dia+0]>>i)&7)<<21) + (((bitsolp2[+dia+1]>>i)&7)<<24);

		i = distweight[fif[fifr].d]; //give surfaces near nucleus higher weight (solves some issues in addition to looking nice)
		if (j&(1<<12)) dx -= i;
		if (j&(1<<14)) dx += i;
		if (j&(1<<10)) dy -= i;
		if (j&(1<<16)) dy += i;
		if (j&(1<< 4)) dz -= i;
		if (j&(1<<22)) dz += i;

		if (fif[fifr].d >= rad) continue;

		for(i=18-1;i>=0;i--)
		{
			if (!(j&neighmsk[i][0])) continue;
			if (i < 6) { if ((((j>>neighmsk[i][1])|j)&neighmsk[i][2]) == neighmsk[i][2]) continue; }
					else { if (!(j&neighmsk[i][1]) == !(j&neighmsk[i][2])) continue; }

			xx = dirx[i]+x; yy = diry[i]+y; zz = dirz[i]+z; if (bitgot[zz*dia+yy]&(1<<xx)) continue;
			bitgot[zz*dia+yy] |= (1<<xx);

			fif[fifw].x = xx; fif[fifw].y = yy; fif[fifw].z = zz; fif[fifw].d = fif[fifr].d+1; fifw++;
		}
	}
#endif
	if (!(dx|dy|dz)) { norm->x = 0.0; norm->y = 0.0; norm->z = 0.0; return; }

	f = (float)dx*dx + (float)dy*dy + (float)dz*dz;
#if 0
	f = 1.0/sqrt(f);
#else
	_asm
	{
		rsqrtss xmm0, f
		movss f, xmm0
	}
#endif
	norm->x = (float)dx*f;
	norm->y = (float)dy*f;
	norm->z = (float)dz*f;
}

void oct_estnorm (oct_t *loct, int nx, int ny, int nz, int rad, dpoint3d *dnorm, oct_bitcac_t *bitcac)
{
	point3d norm;
	oct_estnorm(loct,nx,ny,nz,rad,&norm,bitcac);
	dnorm->x = norm.x; dnorm->y = norm.y; dnorm->z = norm.z;
}

typedef struct { oct_t *loct; int dist, x0, y0, z0, x1, y1, z1; } oct_rn_t;
static void oct_refreshnorms_mt (int i, void *_)
{
	oct_rn_t *rn;
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	point3d norm;
	surf_t *psurf;
	octv_t *ptr;
	int j, x, y, z, nx, ny, nz, ls, s;
	oct_bitcac_t bitcac;

	rn = &((oct_rn_t *)_)[i];

	oct_bitcac_reset(&bitcac);

	ls = rn->loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)rn->loct->nod.buf)[rn->loct->head]; x = 0; y = 0; z = 0; j = 0/*Don't reverse direction - would mess up bitcac inside oct_estnorm()*/;
	while (1)
	{
		i = (1<<j); if (!(ptr->chi&i)) goto tosibly;
		nx = (((j   )&1)<<ls)+x; if ((nx >= rn->x1) || (nx+s <= rn->x0)) goto tosibly;
		ny = (((j>>1)&1)<<ls)+y; if ((ny >= rn->y1) || (ny+s <= rn->y0)) goto tosibly;
		nz = (((j>>2)&1)<<ls)+z; if ((nz >= rn->z1) || (nz+s <= rn->z0)) goto tosibly;

		i = popcount[ptr->chi&(i-1)] + ptr->ind;

		if (ls <= 0)
		{
			psurf = &((surf_t *)rn->loct->sur.buf)[i];
			oct_estnorm(rn->loct,nx,ny,nz,rn->dist,&norm,&bitcac);
			psurf->norm[0] = (signed char)(norm.x*127.0);
			psurf->norm[1] = (signed char)(norm.y*127.0);
			psurf->norm[2] = (signed char)(norm.z*127.0);
#if (GPUSEBO != 0)
			if (oct_usegpu) memcpy(&rn->loct->gsurf[i],psurf,rn->loct->sur.siz);
#endif
			goto tosibly;
		}

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; s >>= 1; //2child
		ptr = &((octv_t *)rn->loct->nod.buf)[i]; x = nx; y = ny; z = nz; j = 0;
		continue;

tosibly:
		j++; if (j < 8) continue;
		do { ls++; s <<= 1; if (ls >= rn->loct->lsid) return; j = stk[ls].j+1; } while (j >= 8); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}
}
void oct_refreshnorms (oct_t *loct, int dist, int x0, int y0, int z0, int x1, int y1, int z1)
{
	#define LXSIZ 3
	#define LYSIZ 3
	#define LZSIZ 3
	static oct_rn_t octrn[1<<(LXSIZ+LYSIZ+LZSIZ)];
	int i, x, y, z;

#if (GPUSEBO != 0)
	if ((oct_usegpu) && (!loct->gsurf)) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
#endif

	i = 0;
	for(x=(1<<LXSIZ)-1;x>=0;x--)
		for(y=(1<<LYSIZ)-1;y>=0;y--)
			for(z=(1<<LZSIZ)-1;z>=0;z--,i++)
			{
				octrn[i].loct = loct; octrn[i].dist = dist;
				octrn[i].x0 = (((x1-x0)*(x+0))>>LXSIZ) + x0;
				octrn[i].y0 = (((y1-y0)*(y+0))>>LYSIZ) + y0;
				octrn[i].z0 = (((z1-z0)*(z+0))>>LZSIZ) + z0;
				octrn[i].x1 = (((x1-x0)*(x+1))>>LXSIZ) + x0;
				octrn[i].y1 = (((y1-y0)*(y+1))>>LYSIZ) + y0;
				octrn[i].z1 = (((z1-z0)*(z+1))>>LZSIZ) + z0;
			}
	htrun(oct_refreshnorms_mt,octrn,0,1<<(LXSIZ+LYSIZ+LZSIZ),oct_numcpu);

#if (GPUSEBO == 0)
	if (oct_usegpu)
	{
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.mal*2)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
	}
#endif
}
//--------------------------------------------------------------------------------------------------

	//(mode&1)==0:air, !=0:sol
	//(mode&2)!=0:do oct_updatesurfs() here (helpful because bbox calculated inside oct_mod_recur)
	//(mode&4)!=0:do hover check, ==0:not
	//(mode&8)!=0:do normal refresh, ==0:not
void oct_mod (oct_t *loct, brush_t *brush, int mode)
{
	octv_t nnode; //temp needed in case bitalloc() realloc's nod.buf
	double d0, d1;
	if (oct_usegpu)
	{
		readklock(&d0);
#if (GPUSEBO != 0)
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
#endif
	}

	brush->mx0 = 0x7fffffff; brush->mx1 = 0x80000000;
	brush->my0 = 0x7fffffff; brush->my1 = 0x80000000;
	brush->mz0 = 0x7fffffff; brush->mz1 = 0x80000000;
	oct_mod_recur(loct,loct->head,0,0,0,loct->lsid-1,&nnode,brush,(mode&1)*((1<<8)-1));
	((octv_t *)loct->nod.buf)[loct->head] = nnode; //can't write node directly to loct->nod.buf[loct->head] because of possible realloc inside

	if (mode&2) { oct_updatesurfs(loct,brush->mx0,brush->my0,brush->mz0,brush->mx1,brush->my1,brush->mz1,brush,mode); }
	if (mode&4) { oct_hover_check(loct,brush->mx0,brush->my0,brush->mz0,brush->mx1,brush->my1,brush->mz1,loct->recvoctfunc); }
	if (mode&8) { int i = (mode&1)^1; oct_refreshnorms(loct,2,brush->mx0-i,brush->my0-i,brush->mz0-i,brush->mx1+i,brush->my1+i,brush->mz1+i); }

	oct_checkreducesizes(loct);

	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.mal*2)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
#endif
		readklock(&d1); testim = d1-d0;
	}
}

void oct_writesurf (oct_t *loct, int ind, surf_t *psurf)
{
	memcpy(&((surf_t *)loct->sur.buf)[ind],psurf,loct->sur.siz);
	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,(ind&((loct->gxsid>>1)-1))<<1,ind>>(loct->glysid-1),2,1,GL_RGBA,GL_UNSIGNED_BYTE,(void *)&((surf_t *)loct->sur.buf)[ind]);
#else
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
		memcpy(&loct->gsurf[ind],psurf,loct->sur.siz);
#endif
	}
}

void oct_copysurfs (oct_t *loct)
{
	if (oct_usegpu)
	{
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
		memcpy(loct->gsurf,loct->sur.buf,loct->sur.mal*loct->sur.siz);
	}
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------

static void oct_modnew_copy_recur (oct_t *loct, int inode, int ls, octv_t *roct, oct_t *newoct)
{
	octv_t noct[8];
	int i, n;
	(*roct) = ((octv_t *)loct->nod.buf)[inode]; n = popcount[roct->chi];
	if (!ls) { roct->ind = oct_refreshsurf(newoct,0,0,n,&((surf_t *)loct->sur.buf)[roct->ind]); return; }
	for(i=0;i<n;i++) oct_modnew_copy_recur(loct,roct->ind+i,ls-1,&noct[i],newoct);
	roct->ind = oct_refreshnode(newoct,0,0,n,noct);
}
	//(mode&1)==0:air, !=0:sol
	//(mode&2)==0:nand, 1:and
static void oct_modnew_recur (oct_t *loct, int inode, int x0, int y0, int z0, int ls, octv_t *roct, brush_t *brush, oct_t *newoct, int mode)
{
	surf_t surf[8];
	octv_t ooct, noct[8];
	int i, iup, n, o, s, x, y, z, xsol, issol;

	issol = (mode&1)*255;
	if (inode >= 0) { ooct = ((octv_t *)loct->nod.buf)[inode]; xsol = ooct.sol^issol; }
				  else { xsol = (1<<8)-1; ooct.chi = 0; ooct.sol = issol^xsol; ooct.ind = -1; ooct.mrk = 0; ooct.mrk2 = 0; }

	if (!ls)
	{
		roct->sol = ooct.sol; roct->mrk = 0; roct->mrk2 = 0;
		for(;xsol;xsol^=iup)
		{
			iup = (-xsol)&xsol;
			i = isins_func(loct,brush,(((iup&0xaa)+0xff)>>8)+x0,(((iup&0xcc)+0xff)>>8)+y0,(((iup&0xf0)+0xff)>>8)+z0,0);
			if (!(mode&2)) i = 2-i;
			if (i) { roct->sol ^= iup; }
		}

		roct->chi = ooct.chi&roct->sol;
		for(n=0,xsol=roct->chi;xsol;xsol^=iup,n++)
		{
			iup = (-xsol)&xsol;
			memcpy(&surf[n],&((surf_t *)loct->sur.buf)[popcount[(iup-1)&ooct.chi]+ooct.ind],loct->sur.siz); //copy existing surf
		}
		roct->ind = oct_refreshsurf(newoct,0,0,n,surf);

		brush->mx0 = min(brush->mx0,x0); brush->mx1 = max(brush->mx1,x0+2);
		brush->my0 = min(brush->my0,y0); brush->my1 = max(brush->my1,y0+2);
		brush->mz0 = min(brush->mz0,z0); brush->mz1 = max(brush->mz1,z0+2);
		return;
	}

	xsol |= ooct.chi; roct->chi = 0; roct->sol = ~xsol&issol; roct->mrk = 0; roct->mrk2 = 0; //copy solid if already brush color
	o = ooct.ind; n = 0; s = pow2[ls];
	for(;xsol;xsol^=iup) //visit only nodes that may differ from brush color
	{
		iup = (-xsol)&xsol;
		x = x0; if (iup&0xaa) x += s;
		y = y0; if (iup&0xcc) y += s;
		z = z0; if (iup&0xf0) z += s;
		i = isins_func(loct,brush,x,y,z,ls);
		if (!(mode&2)) i = 2-i;
		switch(i)
		{
			case 0: //octree node doesn't intersect brush:copy old tree
				roct->sol += (ooct.sol&iup);
				if (ooct.chi&iup) { roct->chi += iup; oct_modnew_copy_recur(loct,o,ls-1,&noct[n],newoct); o++; n++; }
				break;
			case 1:
				if (ooct.chi&iup) { i = o; o++; } //octree node intersects brush partially:must recurse
								 else { i =-1;      } //leaf; split pure node
				oct_modnew_recur(loct,i,x,y,z,ls-1,&noct[n],brush,newoct,mode);
				if (noct[n].sol == (1<<8)-1) roct->sol += iup;
				if (noct[n].chi || ((unsigned)(noct[n].sol-1) < (unsigned)((1<<8)-2))) { roct->chi += iup; n++; }
				break;
			case 2: //brush fully covers octree node:all brush
				roct->sol += (issol&iup);
				if (ooct.chi&iup) o++;
				brush->mx0 = min(brush->mx0,x); brush->mx1 = max(brush->mx1,x+s);
				brush->my0 = min(brush->my0,y); brush->my1 = max(brush->my1,y+s);
				brush->mz0 = min(brush->mz0,z); brush->mz1 = max(brush->mz1,z+s);
				break;
			default: __assume(0); //tells MSVC default can't be reached
		}
	}

	roct->ind = oct_refreshnode(newoct,0,0,n,noct);
}
	//mode==0: newoct =  oct & brush          //most useful - extracts piece without requiring full copy
	//mode==1: newoct = (oct & brush)|~brush  //not useful - very unusual bool op
	//mode==2: newoct =  oct &~brush          //useful, but copy&oct_mod() also works
	//mode==3: newoct =  oct | brush          //useful, but copy&oct_mod() also works
void oct_modnew (oct_t *loct, brush_t *brush, int mode)
{
	oct_t newoct;
	octv_t nnode;

	oct_new(&newoct,loct->lsid,loct->tilid,256,256,0);
	if (oct_usegpu)
	{
#if (GPUSEBO != 0)
		if (!newoct.gsurf) newoct.gsurf = (surf_t *)bo_begin(newoct.bufid,0);
#endif
	}

	brush->mx0 = 0x7fffffff; brush->mx1 = 0x80000000;
	brush->my0 = 0x7fffffff; brush->my1 = 0x80000000;
	brush->mz0 = 0x7fffffff; brush->mz1 = 0x80000000;
	oct_modnew_recur(loct,loct->head,0,0,0,loct->lsid-1,&nnode,brush,&newoct,mode);
	((octv_t *)newoct.nod.buf)[newoct.head] = nnode; //can't write node directly because of possible realloc inside

	oct_updatesurfs(&newoct,0,0,0,newoct.sid,newoct.sid,newoct.sid,brush,mode);
	oct_checkreducesizes(&newoct);

	loct->recvoctfunc(loct,&newoct);
}

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
typedef struct
{
	float x0, y0, z0, dumzero;
	float xv, yv, zv, cosang2;
} isintcs_t; //WARNING:oct_drawoct() asm depends on this exact structure!
	//cx0,cy0,cz0: cone focus
	//cxv,cyv,czv: cone center vector direction, normalized
	//       crad: cone angle (center vector to edge)
	//         sr: sphere radius
static void isint_cone_sph_init (isintcs_t *iics, float cx0, float cy0, float cz0, float cxv, float cyv, float czv, float crad, float sr)
{
	float f = sr/sin(crad);
	iics->x0 = cx0-f*cxv;
	iics->y0 = cy0-f*cyv;
	iics->z0 = cz0-f*czv;
	iics->dumzero = 0.f;
	iics->xv = cxv;
	iics->yv = cyv;
	iics->zv = czv;
	f = cos(crad); iics->cosang2 = f*f;
}
	//sx,sy,sz: sphere center
static int isint_cone_sph (isintcs_t *iics, float sx, float sy, float sz)
{
	float f;
	sx -= iics->x0; sy -= iics->y0; sz -= iics->z0;
	f = sx*iics->xv + sy*iics->yv + sz*iics->zv; if (f <= 0.f) return(0);
	return((sx*sx + sy*sy + sz*sz)*iics->cosang2 < f*f);
}
//--------------------------------------------------------------------------------------------------

#pragma warning(disable:4731) //STFU

typedef struct { int x0, y0, x1, y1; } rect_t;
typedef struct { float z, z2, x, y; } lpoint4d;
typedef struct { short x, y, z, dum; } stk2_t;
__declspec(align(16)) static rect_t grect[(4+4)*(4+4)+1024];
__declspec(align(16)) static lpoint4d gposadd[8][OCT_MAXLS];
__declspec(align(8)) static stk2_t gklslut[8][OCT_MAXLS], glandlut[OCT_MAXLS], gldirlut[OCT_MAXLS], glcenadd, glcensub, gkls0[8];
static float cornmin[OCT_MAXLS], cornmax[OCT_MAXLS], scanmax[OCT_MAXLS];
__declspec(align(16)) static isintcs_t giics[OCT_MAXLS];
static int ordlut[8][256]; //ordered list, 4 bits per cell, compacted to skip unused cells

	//  4ÄÄÄÄÄÄ5       z
	//  ³ \    ³ \      \
	//  ³  0ÄÄÄÄÄÄ1       \ÄÄÄÄx
	//  ³  ³   ³  ³       ³
	//  6ÄÄ³ÄÄÄ7  ³       ³
	//   \ ³    \ ³       y
	//     2ÄÄÄÄÄÄ3
static const char vanlut[27][3] =
{                                // 4ÄÄ5  z
	1,2,3, 3,2,1, 3,0,1, // 0- 2  // ³0ÄÄ1  \ÄÄx
	2,1,3, 3,2,2, 3,0,2, // 3- 5  // 6³Ä7³  ³
	2,1,0, 2,3,0, 0,3,2, // 6- 8  //  2ÄÄ3  y
	1,3,2, 2,2,1, 2,0,1, // 9-11
	1,3,1, 0,0,0, 0,2,0, //12-14
	3,1,0, 0,0,3, 0,2,3, //15-17
	0,3,2, 0,1,2, 2,1,0, //18-20
	0,3,1, 1,0,0, 1,2,0, //21-23
	3,0,1, 1,0,3, 1,2,3, //24-26
};

static const char vanflip[27] = {7,6,6, 5,2,6, 5,5,4, 3,4,6, 2,0,0, 5,0,0, 3,3,2, 3,0,0, 1,0,0};
int oct_sideshade[6] = {0x000000,0x030303,0x060606,0x090909,0x0c0c0c,0x0f0f0f};
__declspec(align(16)) static int dqgsideshade[6][4];
__declspec(align(16)) static float gvanx[4], gvany[4], gvanxmh[4], gvanymh[4], cornvadd[6][4], cornvadd2[27][16] = {0};
__declspec(align(16)) static int vause[27][4] = {0};

	//Render nearby cubes with classic brute force algo..
static void drawcube_close (oct_t *loct, int xx, int yy, int zz, int ipsurf, int ind, int facemask)
{                               // left      right    up       down    front     back
	static const char fac[6][4] = {4,0,2,6, 1,5,7,3, 4,5,1,0, 2,3,7,6, 0,1,3,2, 5,4,6,7};
	point3d vt[8], vt2[5];
	float f, t, fx, fxinc, sx[5], sy[5];
	INT_PTR sptr;
	__declspec(align(16)) char val[16];
	unsigned int *gcptr;
	int i, j, k, kk, i0, i1, sx0, sy0, sx1, sy1, sxe, my0, my1;
	int n, x, y, z, *iptr, isy[5], xc[2][MAXYDIM], col, dir[3];
	#define SCISDIST2 1e-6

		//Rotate&Translate
	vt[0].x = xx*gxmul.x + yy*gymul.x + zz*gzmul.x + gnadd.x;
	vt[0].y = xx*gxmul.y + yy*gymul.y + zz*gzmul.y + gnadd.y;
	vt[0].z = xx*gxmul.z + yy*gymul.z + zz*gzmul.z + gnadd.z;
							 vt[1].x = vt[0  ].x+gxmul.x; vt[1].y = vt[0  ].y+gxmul.y; vt[1].z = vt[  0].z+gxmul.z;
	for(i=2;i<4;i++) { vt[i].x = vt[i-2].x+gymul.x; vt[i].y = vt[i-2].y+gymul.y; vt[i].z = vt[i-2].z+gymul.z; }
	for(   ;i<8;i++) { vt[i].x = vt[i-4].x+gzmul.x; vt[i].y = vt[i-4].y+gzmul.y; vt[i].z = vt[i-4].z+gzmul.z; }

	dir[0] = xx-glorig.x;
	dir[1] = yy-glorig.y;
	dir[2] = zz-glorig.z;

#if (PIXMETH == 0)
	*(int *)&val[0] = ipsurf;
#elif (PIXMETH == 1)
	*(__int64 *)&val[0] =
		(((__int64)    xx)    ) +
		(((__int64)    yy)<<12) +
		(((__int64)    zz)<<24) +
		(((__int64)ipsurf)<<36);
#elif (PIXMETH == 2)
	*(int *)&val[0] = xx;
	*(int *)&val[4] = yy;
	*(int *)&val[8] = zz;
	*(int *)&val[12] = ipsurf;
#endif

	for(k=3-1;k>=0;k--)
	{
			//Back-face cull
		if (!dir[k]) continue;
		kk = (dir[k]<0)+k*2;
		if (!(facemask&(1<<kk))) continue;

			//Clip near plane
		n = 0;
		for(i=4-1,j=0;j<4;i=j,j++)
		{
			i0 = fac[kk][i]; i1 = fac[kk][j];
			if (vt[i0].z >= SCISDIST2) { vt2[n] = vt[i0]; n++; }
			if ((vt[i0].z >= SCISDIST2) != (vt[i1].z >= SCISDIST2))
			{
				t = (SCISDIST2-vt[i0].z)/(vt[i1].z-vt[i0].z);
				vt2[n].x = (vt[i1].x-vt[i0].x)*t + vt[i0].x;
				vt2[n].y = (vt[i1].y-vt[i0].y)*t + vt[i0].y;
				vt2[n].z = SCISDIST2; n++;
			}
		} if (n < 3) continue;

			//Project
		my0 = 0x7fffffff; my1 = 0x80000000;
		for(i=0;i<n;i++)
		{
			f = ghz/vt2[i].z;
			sx[i] = vt2[i].x*f + ghx;
			sy[i] = vt2[i].y*f + ghy;
			isy[i] = min(max((int)sy[i],grect[ind].y0),grect[ind].y1);
			if (isy[i] < my0) my0 = isy[i];
			if (isy[i] > my1) my1 = isy[i];
			sy[i] -= .5;
		}

			//Rasterize
		for(i=n-1,j=0;j<n;i=j,j++)
		{
			if (isy[i] == isy[j]) continue;
			if (isy[i] < isy[j]) { sy0 = isy[i]; sy1 = isy[j]; iptr = &xc[1][0]; }
								 else { sy0 = isy[j]; sy1 = isy[i]; iptr = &xc[0][0]; }
			fxinc = (sx[j]-sx[i])/(sy[j]-sy[i]); fx = ((float)sy0-sy[i])*fxinc + sx[i];
			for(y=sy0;y<sy1;y++,fx+=fxinc) iptr[y] = (int)fx;
		}

			//Draw hlines
		sptr = my0*gxd.p + gxd.f;
		gcptr = (unsigned int *)(my0*gcbitplo3 + (INT_PTR)gcbit);
		for(y=my0;y<my1;y++,sptr+=gxd.p,gcptr+=gcbitplo5)
			for(sx1=max(xc[0][y],grect[ind].x0),sxe=min(xc[1][y],grect[ind].x1);sx1<sxe;)
			{
				sx0 = dntil1(gcptr,sx1,sxe); if (sx0 >= sxe) break;
				sx1 = dntil0(gcptr,sx0,sxe); if (sx1 > sxe) sx1 = sxe;
				setzrange0(gcptr,sx0,sx1);
#if 0
				for(x=sx0;x<sx1;x++) memcpy((void *)(sptr+x*PIXBUFBYPP),val,PIXBUFBYPP); //WARNING:memcpy VERY SLOW to GPU memory?
#elif (PIXBUFBYPP == 8)
				_asm
				{
					movaps xmm0, val
					mov ecx, sptr
					mov eax, sx0
					mov edx, sx1
					lea ecx, [ecx+edx*8]
					sub eax, edx
					jge short endit
begit:            movlps [ecx+eax*8], xmm0
						add eax, 1
						jl short begit
endit:      }
#else
				sx0 *= PIXBUFBYPP; sx1 *= PIXBUFBYPP; for(x=sx0;x<sx1;x+=4) *(int *)(sptr+x) = *(int *)&val[x&(PIXBUFBYPP-1)];
#endif
			}
	}
}

#if (MARCHCUBE != 0)

static double greciplo[512];

	//11/16/2006: Code copied from: http://paulbourke.net/geometry/polygonise/
	//02/21/2013: Tables reworked for Z order: i=z*4+y*2+x. (original data is 2 loops: 0,1,3,2,4,5,7,6)
	//
	//Marching cubes algo: generates tri through isosurface
	//   NOTE:the 8 unit cube verts assumed to be Z order
	//  Input: val: 8 cube values (-=air,+=solid)
	// Output: tri: tri verts, 3 per tri (15 max)
	//Returns: # verts (tri*3) {0,3,6,9,12,15}
static int marchcube_gen (surf_t *psurf, dpoint3d *tri/*[15]*/)
{
	static const unsigned short edgelut[128] = //Using symmetry: edgelut[i] == edgelut[255-i] (Z order)
	{
		0x000,0x111,0x221,0x330,0x412,0x503,0x633,0x722,0x822,0x933,0xa03,0xb12,0xc30,0xd21,0xe11,0xf00,
		0x144,0x055,0x365,0x274,0x556,0x447,0x777,0x666,0x966,0x877,0xb47,0xa56,0xd74,0xc65,0xf55,0xe44,
		0x284,0x395,0x0a5,0x1b4,0x696,0x787,0x4b7,0x5a6,0xaa6,0xbb7,0x887,0x996,0xeb4,0xfa5,0xc95,0xd84,
		0x3c0,0x2d1,0x1e1,0x0f0,0x7d2,0x6c3,0x5f3,0x4e2,0xbe2,0xaf3,0x9c3,0x8d2,0xff0,0xee1,0xdd1,0xcc0,
		0x448,0x559,0x669,0x778,0x05a,0x14b,0x27b,0x36a,0xc6a,0xd7b,0xe4b,0xf5a,0x878,0x969,0xa59,0xb48,
		0x50c,0x41d,0x72d,0x63c,0x11e,0x00f,0x33f,0x22e,0xd2e,0xc3f,0xf0f,0xe1e,0x93c,0x82d,0xb1d,0xa0c,
		0x6cc,0x7dd,0x4ed,0x5fc,0x2de,0x3cf,0x0ff,0x1ee,0xeee,0xfff,0xccf,0xdde,0xafc,0xbed,0x8dd,0x9cc,
		0x788,0x699,0x5a9,0x4b8,0x39a,0x28b,0x1bb,0x0aa,0xfaa,0xebb,0xd8b,0xc9a,0xbb8,0xaa9,0x999,0x888,
	};
	static const signed char trilut[256][16] = //(Z order)
	{
		-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 4, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 0, 9, 5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   5, 4, 8, 9, 5, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 4, 1,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 1,10, 8, 0,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 4, 0, 9, 4, 9,10, 5, 1, 9, 1,10, 9,-1,-1,-1,-1,   5, 1,10, 5,10, 9, 9,10, 8,-1,-1,-1,-1,-1,-1,-1,
		 5,11, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   1, 4, 8, 1, 8,11, 0, 5, 8, 5,11, 8,-1,-1,-1,-1,
		 9,11, 1, 0, 9, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   1, 4, 8, 1, 8,11,11, 8, 9,-1,-1,-1,-1,-1,-1,-1,
		 4, 5,11,10, 4,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 5,11, 0,11, 8, 8,11,10,-1,-1,-1,-1,-1,-1,-1,
		 4, 0, 9, 4, 9,10,10, 9,11,-1,-1,-1,-1,-1,-1,-1,   9,11, 8,11,10, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 8, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   2, 0, 4, 6, 2, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 9, 5, 2, 5, 6, 0, 8, 5, 8, 6, 5,-1,-1,-1,-1,   2, 9, 5, 2, 5, 6, 6, 5, 4,-1,-1,-1,-1,-1,-1,-1,
		10, 6, 2,10, 2, 1, 8, 4, 2, 4, 1, 2,-1,-1,-1,-1,  10, 6, 2,10, 2, 1, 1, 2, 0,-1,-1,-1,-1,-1,-1,-1,
		 9, 6, 2, 9,10, 6, 9, 5,10, 1,10, 5, 0, 8, 4,-1,   2,10, 6, 9,10, 2, 9, 1,10, 9, 5, 1,-1,-1,-1,-1,
		 5,11, 1, 8, 6, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   4, 6,11, 4,11, 1, 6, 2,11, 5,11, 0, 2, 0,11,-1,
		 9,11, 6, 9, 6, 2,11, 1, 6, 8, 6, 0, 1, 0, 6,-1,   1, 9,11, 1, 6, 9, 1, 4, 6, 6, 2, 9,-1,-1,-1,-1,
		 4, 5, 2, 4, 2, 8, 5,11, 2, 6, 2,10,11,10, 2,-1,   5,11,10, 5,10, 2, 5, 2, 0, 6, 2,10,-1,-1,-1,-1,
		 0, 8, 4, 2, 9, 6, 9,10, 6, 9,11,10,-1,-1,-1,-1,   2,10, 6, 2, 9,10, 9,11,10,-1,-1,-1,-1,-1,-1,-1,
		 9, 2, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   8, 2, 7, 8, 7, 4, 9, 0, 7, 0, 4, 7,-1,-1,-1,-1,
		 0, 2, 7, 5, 0, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   8, 2, 7, 8, 7, 4, 4, 7, 5,-1,-1,10,-1,-1,-1,-1,
		 9, 2, 7, 1,10, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 1, 7, 0, 7, 9, 1,10, 7, 2, 7, 8,10, 8, 7,-1,
		 0, 2,10, 0,10, 4, 2, 7,10, 1,10, 5, 7, 5,10,-1,   1, 7, 5, 1, 8, 7, 1,10, 8, 2, 7, 8,-1,-1,-1,-1,
		 7,11, 1, 7, 1, 2, 5, 9, 1, 9, 2, 1,-1,-1,-1,-1,   4,11, 1, 4, 7,11, 4, 8, 7, 2, 7, 8, 0, 5, 9,-1,
		 7,11, 1, 7, 1, 2, 2, 1, 0,-1,-1,-1,-1,-1,-1,-1,   1, 7,11, 4, 7, 1, 4, 2, 7, 4, 8, 2,-1,-1,-1,-1,
		11,10, 2,11, 2, 7,10, 4, 2, 9, 2, 5, 4, 5, 2,-1,   0, 5, 9, 8, 2,11, 8,11,10,11, 2, 7,-1,-1,-1,-1,
		 7, 0, 2, 7,10, 0, 7,11,10,10, 4, 0,-1,-1,-1,-1,   7, 8, 2, 7,11, 8,11,10, 8,-1,-1,-1,-1,-1,-1,-1,
		 9, 8, 6, 7, 9, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   9, 0, 4, 9, 4, 7, 7, 4, 6,-1,-1,-1,-1,-1,-1,-1,
		 0, 8, 6, 0, 6, 5, 5, 6, 7,-1,-1,-1,-1,-1,-1,-1,   5, 4, 7, 4, 6, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 6, 7, 1, 6, 1,10, 7, 9, 1, 4, 1, 8, 9, 8, 1,-1,   9, 6, 7, 9, 1, 6, 9, 0, 1, 1,10, 6,-1,-1,-1,-1,
		 0, 8, 4, 5, 1, 6, 5, 6, 7, 6, 1,10,-1,-1,-1,-1,  10, 5, 1,10, 6, 5, 6, 7, 5,-1,-1,-1,-1,-1,-1,-1,
		 9, 8, 1, 9, 1, 5, 8, 6, 1,11, 1, 7, 6, 7, 1,-1,   9, 0, 5, 7,11, 4, 7, 4, 6, 4,11, 1,-1,-1,-1,-1,
		 8, 1, 0, 8, 7, 1, 8, 6, 7,11, 1, 7,-1,-1,-1,-1,   1, 7,11, 1, 4, 7, 4, 6, 7,-1,-1,-1,-1,-1,-1,-1,
		11, 6, 7,11,10, 6, 9, 8, 5, 8, 4, 5,-1,-1,-1,-1,   7,10, 6, 7,11,10, 5, 9, 0,-1,-1,-1,-1,-1,-1,-1,
		10, 7,11,10, 6, 7, 8, 4, 0,-1,-1,-1,-1,-1,-1,-1,  10, 7,11, 6, 7,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 6,10, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   6, 8, 0, 6, 0, 3, 4,10, 0,10, 3, 0,-1,-1,-1,-1,
		 0, 9, 5,10, 3, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   8, 9, 3, 8, 3, 6, 9, 5, 3,10, 3, 4, 5, 4, 3,-1,
		 6, 4, 1, 3, 6, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   6, 8, 0, 6, 0, 3, 3, 0, 1,-1,-1,-1,-1,-1,-1,-1,
		 1, 3, 9, 1, 9, 5, 3, 6, 9, 0, 9, 4, 6, 4, 9,-1,   5, 1, 3, 5, 3, 8, 5, 8, 9, 8, 3, 6,-1,-1,-1,-1,
		10, 1, 5,10, 5, 6,11, 3, 5, 3, 6, 5,-1,-1,-1,-1,   5, 8, 0, 5, 6, 8, 5,11, 6, 3, 6,11, 1, 4,10,-1,
		 1, 0, 6, 1, 6,10, 0, 9, 6, 3, 6,11, 9,11, 6,-1,   1, 4,10,11, 3, 8,11, 8, 9, 8, 3, 6,-1,-1,-1,-1,
		11, 3, 6,11, 6, 5, 5, 6, 4,-1,-1,-1,-1,-1,-1,-1,  11, 3, 6, 5,11, 6, 5, 6, 8, 5, 8, 0,-1,-1,-1,-1,
		 0, 6, 4, 0,11, 6, 0, 9,11, 3, 6,11,-1,-1,-1,-1,   6,11, 3, 6, 8,11, 8, 9,11,-1,-1,-1,-1,-1,-1,-1,
		 3, 2, 8,10, 3, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   4,10, 3, 4, 3, 0, 0, 3, 2,-1,-1,-1,-1,-1,-1,-1,
		 8,10, 5, 8, 5, 0,10, 3, 5, 9, 5, 2, 3, 2, 5,-1,   9, 3, 2, 9, 4, 3, 9, 5, 4,10, 3, 4,-1,-1,-1,-1,
		 8, 4, 1, 8, 1, 2, 2, 1, 3,-1,-1,-1,-1,-1,-1,-1,   0, 1, 2, 2, 1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 4, 0, 8, 5, 1, 9, 1, 2, 9, 1, 3, 2,-1,-1,-1,-1,   5, 2, 9, 5, 1, 2, 1, 3, 2,-1,-1,-1,-1,-1,-1,-1,
		 3, 2, 5, 3, 5,11, 2, 8, 5, 1, 5,10, 8,10, 5,-1,   4,10, 1, 0, 5, 3, 0, 3, 2, 3, 5,11,-1,-1,-1,-1,
		 0, 8, 1, 1, 8,10, 2, 9,11, 2,11, 3,-1,-1,-1,-1,  11, 2, 9,11, 3, 2,10, 1, 4,-1,-1,-1,-1,-1,-1,-1,
		 8, 4, 5, 8, 5, 3, 8, 3, 2, 3, 5,11,-1,-1,-1,-1,  11, 0, 5,11, 3, 0, 3, 2, 0,-1,-1,-1,-1,-1,-1,-1,
		 2,11, 3, 2, 9,11, 0, 8, 4,-1,-1,-1,-1,-1,-1,-1,  11, 2, 9, 3, 2,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 3, 7, 9, 3, 9,10, 2, 6, 9, 6,10, 9,-1,-1,-1,-1,   0, 7, 9, 0, 3, 7, 0, 4, 3,10, 3, 4, 8, 2, 6,-1,
		 7, 5,10, 7,10, 3, 5, 0,10, 6,10, 2, 0, 2,10,-1,   8, 2, 6, 4,10, 7, 4, 7, 5, 7,10, 3,-1,-1,-1,-1,
		 6, 4, 9, 6, 9, 2, 4, 1, 9, 7, 9, 3, 1, 3, 9,-1,   8, 2, 6, 9, 0, 7, 0, 3, 7, 0, 1, 3,-1,-1,-1,-1,
		 5, 1, 7, 7, 1, 3, 4, 0, 2, 4, 2, 6,-1,-1,-1,-1,   3, 5, 1, 3, 7, 5, 2, 6, 8,-1,-1,-1,-1,-1,-1,-1,
		 9, 1, 5, 9,10, 1, 9, 2,10, 6,10, 2, 7,11, 3,-1,   0, 5, 9, 2, 6, 8, 1, 4,10, 7,11, 3,-1,-1,-1,-1,
		 7,11, 3, 2, 6, 1, 2, 1, 0, 1, 6,10,-1,-1,-1,-1,   4,10, 1, 6, 8, 2,11, 3, 7,-1,-1,-1,-1,-1,-1,-1,
		11, 3, 7, 5, 9, 6, 5, 6, 4, 6, 9, 2,-1,-1,-1,-1,   5, 9, 0, 7,11, 3, 8, 2, 6,-1,-1,-1,-1,-1,-1,-1,
		 2, 4, 0, 2, 6, 4, 3, 7,11,-1,-1,-1,-1,-1,-1,-1,   7,11, 3, 2, 6, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 3, 7, 9, 3, 9,10,10, 9, 8,-1,-1,-1,-1,-1,-1,-1,   4,10, 3, 0, 4, 3, 0, 3, 7, 0, 7, 9,-1,-1,-1,-1,
		 0, 8,10, 0,10, 7, 0, 7, 5, 7,10, 3,-1,-1,-1,-1,   3, 4,10, 3, 7, 4, 7, 5, 4,-1,-1,-1,-1,-1,-1,-1,
		 7, 9, 8, 7, 8, 1, 7, 1, 3, 4, 1, 8,-1,-1,-1,-1,   9, 3, 7, 9, 0, 3, 0, 1, 3,-1,-1,-1,-1,-1,-1,-1,
		 5, 3, 7, 5, 1, 3, 4, 0, 8,-1,-1,-1,-1,-1,-1,-1,   5, 3, 7, 1, 3, 5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 7,11, 3, 5, 9, 1, 9,10, 1, 9, 8,10,-1,-1,-1,-1,   0, 5, 9, 1, 4,10, 7,11, 3,-1,-1,-1,-1,-1,-1,-1,
		10, 0, 8,10, 1, 0,11, 3, 7,-1,-1,-1,-1,-1,-1,-1,   1, 4,10,11, 3, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 5, 8, 4, 5, 9, 8, 7,11, 3,-1,-1,-1,-1,-1,-1,-1,   9, 0, 5, 7,11, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 0, 8, 4, 7,11, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  11, 3, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		11, 7, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 4, 8, 7, 3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		11, 5, 0,11, 0, 3, 9, 7, 0, 7, 3, 0,-1,-1,-1,-1,   5, 4, 3, 5, 3,11, 4, 8, 3, 7, 3, 9, 8, 9, 3,-1,
		 3,10, 4, 3, 4, 7, 1,11, 4,11, 7, 4,-1,-1,-1,-1,  10, 8, 7,10, 7, 3, 8, 0, 7,11, 7, 1, 0, 1, 7,-1,
		 0,10, 4, 0, 3,10, 0, 9, 3, 7, 3, 9, 5, 1,11,-1,   5, 1,11, 9, 7,10, 9,10, 8,10, 7, 3,-1,-1,-1,-1,
		 5, 7, 3, 1, 5, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   5, 7, 8, 5, 8, 0, 7, 3, 8, 4, 8, 1, 3, 1, 8,-1,
		 9, 7, 3, 9, 3, 0, 0, 3, 1,-1,-1,-1,-1,-1,-1,-1,   7, 8, 9, 7, 1, 8, 7, 3, 1, 4, 8, 1,-1,-1,-1,-1,
		 3,10, 4, 3, 4, 7, 7, 4, 5,-1,-1,-1,-1,-1,-1,-1,   0,10, 8, 0, 7,10, 0, 5, 7, 7, 3,10,-1,-1,-1,-1,
		 4, 3,10, 0, 3, 4, 0, 7, 3, 0, 9, 7,-1,-1,-1,-1,   3, 9, 7, 3,10, 9,10, 8, 9,-1,-1,-1,-1,-1,-1,-1,
		 6, 3,11, 6,11, 8, 7, 2,11, 2, 8,11,-1,-1,-1,-1,   2, 0,11, 2,11, 7, 0, 4,11, 3,11, 6, 4, 6,11,-1,
		 5, 3,11, 5, 6, 3, 5, 0, 6, 8, 6, 0, 9, 7, 2,-1,   9, 7, 2,11, 5, 3, 5, 6, 3, 5, 4, 6,-1,-1,-1,-1,
		 4, 2, 8, 4, 7, 2, 4, 1, 7,11, 7, 1,10, 6, 3,-1,   6, 3,10, 7, 2,11, 2, 1,11, 2, 0, 1,-1,-1,-1,-1,
		 3,10, 6, 5, 1,11, 0, 8, 4, 2, 9, 7,-1,-1,-1,-1,   9, 7, 2,11, 5, 1, 6, 3,10,-1,-1,-1,-1,-1,-1,-1,
		 3, 1, 8, 3, 8, 6, 1, 5, 8, 2, 8, 7, 5, 7, 8,-1,   4, 3, 1, 4, 6, 3, 5, 7, 0, 7, 2, 0,-1,-1,-1,-1,
		 9, 7, 2, 0, 8, 3, 0, 3, 1, 3, 8, 6,-1,-1,-1,-1,   6, 1, 4, 6, 3, 1, 7, 2, 9,-1,-1,-1,-1,-1,-1,-1,
		10, 6, 3, 8, 4, 2, 4, 7, 2, 4, 5, 7,-1,-1,-1,-1,   7, 0, 5, 7, 2, 0, 6, 3,10,-1,-1,-1,-1,-1,-1,-1,
		 0, 8, 4, 2, 9, 7,10, 6, 3,-1,-1,-1,-1,-1,-1,-1,   2, 9, 7, 6, 3,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		11, 9, 2, 3,11, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   2, 3, 4, 2, 4, 8, 3,11, 4, 0, 4, 9,11, 9, 4,-1,
		11, 5, 0,11, 0, 3, 3, 0, 2,-1,-1,-1,-1,-1,-1,-1,   8, 5, 4, 8, 3, 5, 8, 2, 3, 3,11, 5,-1,-1,-1,-1,
		11, 9, 4,11, 4, 1, 9, 2, 4,10, 4, 3, 2, 3, 4,-1,   2,10, 8, 2, 3,10, 0, 1, 9, 1,11, 9,-1,-1,-1,-1,
		 5, 1,11, 4, 0,10, 0, 3,10, 0, 2, 3,-1,-1,-1,-1,   3, 8, 2, 3,10, 8, 1,11, 5,-1,-1,-1,-1,-1,-1,-1,
		 5, 9, 2, 5, 2, 1, 1, 2, 3,-1,-1,-1,-1,-1,-1,-1,   5, 9, 0, 1, 4, 2, 1, 2, 3, 2, 4, 8,-1,-1,-1,-1,
		 0, 2, 1, 2, 3, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   8, 1, 4, 8, 2, 1, 2, 3, 1,-1,-1,-1,-1,-1,-1,-1,
		 9, 2, 3, 9, 3, 4, 9, 4, 5,10, 4, 3,-1,-1,-1,-1,   8, 3,10, 8, 2, 3, 9, 0, 5,-1,-1,-1,-1,-1,-1,-1,
		 4, 3,10, 4, 0, 3, 0, 2, 3,-1,-1,-1,-1,-1,-1,-1,   3, 8, 2,10, 8, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 6, 3,11, 6,11, 8, 8,11, 9,-1,-1,-1,-1,-1,-1,-1,   0, 4, 6, 0, 6,11, 0,11, 9, 3,11, 6,-1,-1,-1,-1,
		11, 6, 3, 5, 6,11, 5, 8, 6, 5, 0, 8,-1,-1,-1,-1,  11, 6, 3,11, 5, 6, 5, 4, 6,-1,-1,-1,-1,-1,-1,-1,
		 3,10, 6, 1,11, 4,11, 8, 4,11, 9, 8,-1,-1,-1,-1,   1, 9, 0, 1,11, 9, 3,10, 6,-1,-1,-1,-1,-1,-1,-1,
		 5, 1,11, 4, 0, 8, 3,10, 6,-1,-1,-1,-1,-1,-1,-1,  11, 5, 1, 3,10, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 5, 3, 1, 5, 8, 3, 5, 9, 8, 8, 6, 3,-1,-1,-1,-1,   1, 6, 3, 1, 4, 6, 0, 5, 9,-1,-1,-1,-1,-1,-1,-1,
		 6, 0, 8, 6, 3, 0, 3, 1, 0,-1,-1,-1,-1,-1,-1,-1,   6, 1, 4, 3, 1, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 8, 5, 9, 8, 4, 5,10, 6, 3,-1,-1,-1,-1,-1,-1,-1,   0, 5, 9,10, 6, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 4, 0, 8,10, 6, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   6, 3,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		10,11, 7, 6,10, 7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  10,11, 0,10, 0, 4,11, 7, 0, 8, 0, 6, 7, 6, 0,-1,
		 7, 6, 0, 7, 0, 9, 6,10, 0, 5, 0,11,10,11, 0,-1,   9, 7, 8, 8, 7, 6,11, 5, 4,11, 4,10,-1,-1,-1,-1,
		 1,11, 7, 1, 7, 4, 4, 7, 6,-1,-1,-1,-1,-1,-1,-1,   8, 0, 1, 8, 1, 7, 8, 7, 6,11, 7, 1,-1,-1,-1,-1,
		11, 5, 1, 9, 7, 0, 7, 4, 0, 7, 6, 4,-1,-1,-1,-1,   9, 6, 8, 9, 7, 6,11, 5, 1,-1,-1,-1,-1,-1,-1,-1,
		10, 1, 5,10, 5, 6, 6, 5, 7,-1,-1,-1,-1,-1,-1,-1,   1, 4,10, 0, 5, 8, 5, 6, 8, 5, 7, 6,-1,-1,-1,-1,
		 9, 7, 6, 9, 6, 1, 9, 1, 0, 1, 6,10,-1,-1,-1,-1,   6, 9, 7, 6, 8, 9, 4,10, 1,-1,-1,-1,-1,-1,-1,-1,
		 5, 7, 4, 4, 7, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 6, 8, 0, 5, 6, 5, 7, 6,-1,-1,-1,-1,-1,-1,-1,
		 9, 4, 0, 9, 7, 4, 7, 6, 4,-1,-1,-1,-1,-1,-1,-1,   9, 6, 8, 7, 6, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 7, 2, 8, 7, 8,11,11, 8,10,-1,-1,-1,-1,-1,-1,-1,   7, 2, 0, 7, 0,10, 7,10,11,10, 0, 4,-1,-1,-1,-1,
		 2, 9, 7, 0, 8, 5, 8,11, 5, 8,10,11,-1,-1,-1,-1,  11, 4,10,11, 5, 4, 9, 7, 2,-1,-1,-1,-1,-1,-1,-1,
		 1,11, 7, 4, 1, 7, 4, 7, 2, 4, 2, 8,-1,-1,-1,-1,   7, 1,11, 7, 2, 1, 2, 0, 1,-1,-1,-1,-1,-1,-1,-1,
		 4, 0, 8, 5, 1,11, 2, 9, 7,-1,-1,-1,-1,-1,-1,-1,   5, 1,11, 9, 7, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 1, 5, 7, 1, 7, 8, 1, 8,10, 2, 8, 7,-1,-1,-1,-1,   0, 7, 2, 0, 5, 7, 1, 4,10,-1,-1,-1,-1,-1,-1,-1,
		 0,10, 1, 0, 8,10, 2, 9, 7,-1,-1,-1,-1,-1,-1,-1,   9, 7, 2, 1, 4,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 8, 7, 2, 8, 4, 7, 4, 5, 7,-1,-1,-1,-1,-1,-1,-1,   0, 7, 2, 5, 7, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 9, 7, 2, 0, 8, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   9, 7, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 6,10, 2,10, 9, 9,10,11,-1,-1,-1,-1,-1,-1,-1,   2, 6, 8, 9, 0,10, 9,10,11,10, 0, 4,-1,-1,-1,-1,
		 5,10,11, 5, 2,10, 5, 0, 2, 6,10, 2,-1,-1,-1,-1,   4,11, 5, 4,10,11, 6, 8, 2,-1,-1,-1,-1,-1,-1,-1,
		 1,11, 9, 1, 9, 6, 1, 6, 4, 6, 9, 2,-1,-1,-1,-1,   9, 1,11, 9, 0, 1, 8, 2, 6,-1,-1,-1,-1,-1,-1,-1,
		 4, 2, 6, 4, 0, 2, 5, 1,11,-1,-1,-1,-1,-1,-1,-1,   5, 1,11, 8, 2, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 6,10, 9, 2,10, 9,10, 1, 9, 1, 5,-1,-1,-1,-1,   9, 0, 5, 8, 2, 6, 1, 4,10,-1,-1,-1,-1,-1,-1,-1,
		10, 2, 6,10, 1, 2, 1, 0, 2,-1,-1,-1,-1,-1,-1,-1,   8, 2, 6, 4,10, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 5, 9, 2, 6, 5, 6, 4, 5,-1,-1,-1,-1,-1,-1,-1,   0, 5, 9, 8, 2, 6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 2, 4, 0, 6, 4, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   2, 6, 8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 9, 8,11,11, 8,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   4, 9, 0, 4,10, 9,10,11, 9,-1,-1,-1,-1,-1,-1,-1,
		 0,11, 5, 0, 8,11, 8,10,11,-1,-1,-1,-1,-1,-1,-1,   4,11, 5,10,11, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 1, 8, 4, 1,11, 8,11, 9, 8,-1,-1,-1,-1,-1,-1,-1,   9, 1,11, 0, 1, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 0, 8, 4, 5, 1,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   5, 1,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 5,10, 1, 5, 9,10, 9, 8,10,-1,-1,-1,-1,-1,-1,-1,   5, 9, 0, 1, 4,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 0,10, 1, 8,10, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   4,10, 1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 5, 8, 4, 9, 8, 5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,   0, 5, 9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
		 0, 8, 4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	};
	dpoint3d vt[12];
	int i, v;
	const signed char *cptr;

	cptr = &psurf->cornval[0];

		//find edge index lut which tells which vertices are inside surface
#if 0
	for(i=8-1,v=0;i>=0;i--) if (cptr[i] < 0) v += (1<<i);
#else
	v = _mm_movemask_epi8(_mm_loadl_epi64((__m128i *)cptr)); //can do above line using pmovmskb :)
#endif
	if (v < 128) i = (int)edgelut[v]; else i = (int)edgelut[v^255];
	if (!i) return(0);

		//for each of 12 edges, find intersecting verts
		//vt[0]:0,1-  vt[4]:0,2|  vt[8]:0,4/
		//vt[1]:2,3-  vt[5]:1,3|  vt[9]:1,5/
		//vt[2]:4,5-  vt[6]:4,6|  vt[a]:2,6/
		//vt[3]:6,7-  vt[7]:5,7|  vt[b]:3,7/
	if (i&(1<< 0)) { vt[ 0].x = greciplo[(int)cptr[0]-(int)cptr[1]+256]*(int)cptr[0]; vt[ 0].y = 0.f; vt[ 0].z = 0.f; }
	if (i&(1<< 1)) { vt[ 1].x = greciplo[(int)cptr[2]-(int)cptr[3]+256]*(int)cptr[2]; vt[ 1].y = 1.f; vt[ 1].z = 0.f; }
	if (i&(1<< 2)) { vt[ 2].x = greciplo[(int)cptr[4]-(int)cptr[5]+256]*(int)cptr[4]; vt[ 2].y = 0.f; vt[ 2].z = 1.f; }
	if (i&(1<< 3)) { vt[ 3].x = greciplo[(int)cptr[6]-(int)cptr[7]+256]*(int)cptr[6]; vt[ 3].y = 1.f; vt[ 3].z = 1.f; }
	if (i&(1<< 4)) { vt[ 4].x = 0.f; vt[ 4].y = greciplo[(int)cptr[0]-(int)cptr[2]+256]*(int)cptr[0]; vt[ 4].z = 0.f; }
	if (i&(1<< 5)) { vt[ 5].x = 1.f; vt[ 5].y = greciplo[(int)cptr[1]-(int)cptr[3]+256]*(int)cptr[1]; vt[ 5].z = 0.f; }
	if (i&(1<< 6)) { vt[ 6].x = 0.f; vt[ 6].y = greciplo[(int)cptr[4]-(int)cptr[6]+256]*(int)cptr[4]; vt[ 6].z = 1.f; }
	if (i&(1<< 7)) { vt[ 7].x = 1.f; vt[ 7].y = greciplo[(int)cptr[5]-(int)cptr[7]+256]*(int)cptr[5]; vt[ 7].z = 1.f; }
	if (i&(1<< 8)) { vt[ 8].x = 0.f; vt[ 8].y = 0.f; vt[ 8].z = greciplo[(int)cptr[0]-(int)cptr[4]+256]*(int)cptr[0]; }
	if (i&(1<< 9)) { vt[ 9].x = 1.f; vt[ 9].y = 0.f; vt[ 9].z = greciplo[(int)cptr[1]-(int)cptr[5]+256]*(int)cptr[1]; }
	if (i&(1<<10)) { vt[10].x = 0.f; vt[10].y = 1.f; vt[10].z = greciplo[(int)cptr[2]-(int)cptr[6]+256]*(int)cptr[2]; }
	if (i&(1<<11)) { vt[11].x = 1.f; vt[11].y = 1.f; vt[11].z = greciplo[(int)cptr[3]-(int)cptr[7]+256]*(int)cptr[3]; }

	cptr = &trilut[v][0];
	for(i=0;cptr[i]>=0;i+=3)
	{
		tri[i  ] = vt[cptr[i  ]];
		tri[i+1] = vt[cptr[i+1]];
		tri[i+2] = vt[cptr[i+2]];
	}
	return(i);
}
static int marchcube_gen (surf_t *psurf, point3d *tri/*[15]*/)
{
	dpoint3d dtri[15];
	int i, n;

	n = marchcube_gen(psurf,dtri);
	for(i=n-1;i>=0;i--) { tri[i].x = (float)dtri[i].x; tri[i].y = (float)dtri[i].y; tri[i].z = (float)dtri[i].z; }
	return(n);
}

static double marchcube_hitscan (const dpoint3d *pos, const dpoint3d *dir, int x, int y, int z, surf_t *psurf, dpoint3d *norm, double *retu, double *retv)
{
	dpoint3d tri[15], vd, vu, vv, vb, nm;
	double d, u, v, bd, bu, bv, rdet;
	int i;

	vd.x = -dir->x;
	vd.y = -dir->y;
	vd.z = -dir->z;

	bd = 1e32;
	for(i=marchcube_gen(psurf,tri)-3;i>=0;i-=3)
	{
			//raycast to tri:
			//intx = pos->x + dir->x*d = (tri[i+1].x-tri[i].x)*u + (tri[i+2].x-tri[i].x)*v + tri[i].x
			//inty = pos->y + dir->y*d = (tri[i+1].y-tri[i].y)*u + (tri[i+2].y-tri[i].y)*v + tri[i].y
			//intz = pos->z + dir->z*d = (tri[i+1].z-tri[i].z)*u + (tri[i+2].z-tri[i].z)*v + tri[i].z
			//Constraints: d >= 0, u >= 0, v >= 0, u+v <= 1
		vu.x = tri[i+1].x-tri[i].x; vv.x = tri[i+2].x-tri[i].x; vb.x = pos->x-(tri[i].x+(double)x);
		vu.y = tri[i+1].y-tri[i].y; vv.y = tri[i+2].y-tri[i].y; vb.y = pos->y-(tri[i].y+(double)y);
		vu.z = tri[i+1].z-tri[i].z; vv.z = tri[i+2].z-tri[i].z; vb.z = pos->z-(tri[i].z+(double)z);
		nm.x = vu.y*vv.z - vu.z*vv.y;
		nm.y = vu.z*vv.x - vu.x*vv.z;
		nm.z = vu.x*vv.y - vu.y*vv.x;

			//vd.x*d + vu.x*u + vv.x*v = vb.x
			//vd.y*d + vu.y*u + vv.y*v = vb.y
			//vd.z*d + vu.z*u + vv.z*v = vb.z
		rdet = vd.x*nm.x + vd.y*nm.y + vd.z*nm.z; if (rdet == 0.0) continue; rdet = 1.0/rdet;
		d = (vb.x*         nm.x           + vb.y*         nm.y           + vb.z*         nm.z          )*rdet; if ((d < 0.0) || (d >= bd)) continue;
		u = (vd.x*(vb.y*vv.z - vb.z*vv.y) + vd.y*(vb.z*vv.x - vb.x*vv.z) + vd.z*(vb.x*vv.y - vb.y*vv.x))*rdet; if (u < 0.0) continue;
		v = (vd.x*(vu.y*vb.z - vu.z*vb.y) + vd.y*(vu.z*vb.x - vu.x*vb.z) + vd.z*(vu.x*vb.y - vu.y*vb.x))*rdet; if ((v < 0.0) || (u+v > 1.0)) continue;
		bd = d;
		if (norm) (*norm) = nm;
		if (retu) (*retu) = u;
		if (retv) (*retv) = v;
	}
	if (norm) { d = 1.0/sqrt(norm->x*norm->x + norm->y*norm->y + norm->z*norm->z); norm->x *= d; norm->y *= d; norm->z *= d; }
	return(bd);
}
static float marchcube_hitscan (const point3d *pos, const point3d *dir, int x, int y, int z, surf_t *psurf, point3d *norm, float *retu, float *retv)
{
	dpoint3d dpos, ddir, dnorm;
	double ret, dretu, dretv;

	dpos.x = (double)pos->x; dpos.y = (double)pos->y; dpos.z = (double)pos->z;
	ddir.x = (double)dir->x; ddir.y = (double)dir->y; ddir.z = (double)dir->z;
	ret = marchcube_hitscan(&dpos,&ddir,x,y,z,psurf,&dnorm,&dretu,&dretv);
	if (norm) { norm->x = (float)dnorm.x; norm->y = (float)dnorm.y; norm->z = (float)dnorm.z; }
	if (retu) (*retu) = dretu;
	if (retv) (*retv) = dretv;
	return((float)ret);
}

	//Render nearby cubes with classic brute force algo..
static void drawcube_close_mc (oct_t *loct, int xx, int yy, int zz, int ipsurf, int ind)
{
	point3d fp, tri[15], vt[3], vt2[6];
	float f, t, fx, fxinc, sx[6], sy[6];
	INT_PTR sptr;
	__declspec(align(16)) char val[16];
	unsigned int *gcptr;
	int i, j, k, c, n, y, sx0, sy0, sx1, sy1, sxe, my0, my1, *iptr, isy[6], xc[2][MAXYDIM];
	#define SCISDIST2 1e-6

#if (PIXMETH == 0)
	*(int *)&val[0] = ipsurf;
#elif (PIXMETH == 1)
	*(__int64 *)&val[0] =
		(((__int64)    xx)    ) +
		(((__int64)    yy)<<12) +
		(((__int64)    zz)<<24) +
		(((__int64)ipsurf)<<36);
#elif (PIXMETH == 2)
	*(int *)&val[0] = xx;
	*(int *)&val[4] = yy;
	*(int *)&val[8] = zz;
	*(int *)&val[12] = ipsurf;
#endif

	for(c=marchcube_gen(&((surf_t *)loct->sur.buf)[ipsurf],tri)-3;c>=0;c-=3)
	{
			//Rotate&Translate
		for(j=3-1;j>=0;j--)
		{
			fp.x = tri[c+j].x+(float)xx;
			fp.y = tri[c+j].y+(float)yy;
			fp.z = tri[c+j].z+(float)zz;
			vt[j].x = fp.x*gxmul.x + fp.y*gymul.x + fp.z*gzmul.x + gnadd.x;
			vt[j].y = fp.x*gxmul.y + fp.y*gymul.y + fp.z*gzmul.y + gnadd.y;
			vt[j].z = fp.x*gxmul.z + fp.y*gymul.z + fp.z*gzmul.z + gnadd.z;
		}

			//Clip near plane
		n = 0;
		for(i=3-1,j=0;j<3;i=j,j++)
		{
			if (vt[i].z >= SCISDIST2) { vt2[n] = vt[i]; n++; }
			if ((vt[i].z >= SCISDIST2) != (vt[j].z >= SCISDIST2))
			{
				t = (SCISDIST2-vt[i].z)/(vt[j].z-vt[i].z);
				vt2[n].x = (vt[j].x-vt[i].x)*t + vt[i].x;
				vt2[n].y = (vt[j].y-vt[i].y)*t + vt[i].y;
				vt2[n].z = SCISDIST2; n++;
			}
		} if (n < 3) continue;

			//Project
		my0 = 0x7fffffff; my1 = 0x80000000;
		for(i=0;i<n;i++)
		{
			f = ghz/vt2[i].z;
			sx[i] = vt2[i].x*f + ghx;
			sy[i] = vt2[i].y*f + ghy;
			isy[i] = min(max((int)sy[i],grect[ind].y0),grect[ind].y1);
			if (isy[i] < my0) my0 = isy[i];
			if (isy[i] > my1) my1 = isy[i];
			sy[i] -= .5;
		}

			//Rasterize
		for(i=n-1,j=0;j<n;i=j,j++)
		{
			if (isy[i] == isy[j]) continue;
			if (isy[i] < isy[j]) { sy0 = isy[i]; sy1 = isy[j]; iptr = &xc[1][0]; }
								 else { sy0 = isy[j]; sy1 = isy[i]; iptr = &xc[0][0]; }
			fxinc = (sx[j]-sx[i])/(sy[j]-sy[i]); fx = ((float)sy0-sy[i])*fxinc + sx[i];
			for(y=sy0;y<sy1;y++,fx+=fxinc) iptr[y] = (int)fx;
		}

			//Draw hlines
		sptr = my0*gxd.p + gxd.f;
		gcptr = (unsigned int *)(my0*gcbitplo3 + (INT_PTR)gcbit);
		for(y=my0;y<my1;y++,sptr+=gxd.p,gcptr+=gcbitplo5)
			for(sx1=max(xc[0][y],grect[ind].x0),sxe=min(xc[1][y],grect[ind].x1);sx1<sxe;)
			{
				sx0 = dntil1(gcptr,sx1,sxe); if (sx0 >= sxe) break;
				sx1 = dntil0(gcptr,sx0,sxe); if (sx1 > sxe) sx1 = sxe;
				setzrange0(gcptr,sx0,sx1);
#if 0
				for(x=sx0;x<sx1;x++) memcpy((void *)(sptr+x*PIXBUFBYPP),val,PIXBUFBYPP); //WARNING:memcpy VERY SLOW to GPU memory?
#elif (PIXBUFBYPP == 8)
				_asm
				{
					movaps xmm0, val
					mov ecx, sptr
					mov eax, sx0
					mov edx, sx1
					lea ecx, [ecx+edx*8]
					sub eax, edx
					jge short endit
begit:            movlps [ecx+eax*8], xmm0
						add eax, 1
						jl short begit
endit:      }
#else
				sx0 *= PIXBUFBYPP; sx1 *= PIXBUFBYPP; for(x=sx0;x<sx1;x+=4) *(int *)(sptr+x) = *(int *)&val[x&(PIXBUFBYPP-1)];
#endif
			}
	}
}
#endif

static void oct_rendclosecubes (oct_t *loct, ipoint3d *cent)
{
	typedef struct { octv_t *ptr; int x, y, z, j, ord; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, k, ls, s, x, y, z, nx, ny, nz, ord, vis, surftyp;
	#define BITDIST 6 //Must be <= 6
	static unsigned int bitvis[16*16], *pbitvis;

	surftyp = oct_getvox(loct,cent->x,cent->y,cent->z,0);
	if (surftyp != 1)
	{
		oct_sol2bit(loct,bitvis,cent->x-BITDIST-1,cent->y-BITDIST-1,cent->z-BITDIST-1,16,16,16,1);
	}

	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 8-1;
	ord = (z+s > cent->z)*4 + (y+s > cent->y)*2 + (x+s > cent->x);
	while (1)
	{
		k = j^ord;

		i = (1<<k); if (!(ptr->chi&i)) goto tosibly;

		nx = (( k    &1)<<ls)+x;
		ny = (((k>>1)&1)<<ls)+y;
		nz = (((k>>2)&1)<<ls)+z;

			 //skip if > DIST manhattans
		if ((nx > cent->x+BITDIST) || (nx+s <= cent->x-BITDIST)) goto tosibly;
		if ((ny > cent->y+BITDIST) || (ny+s <= cent->y-BITDIST)) goto tosibly;
		if ((nz > cent->z+BITDIST) || (nz+s <= cent->z-BITDIST)) goto tosibly;

			//skip if not in frustum pyramid
		if (!isint_cone_sph(&giics[ls],nx,ny,nz)) goto tosibly;

			//skip if not in front //FIXFIXFIXFIX:comment this out and fix do_rect to not draw manhattan!
		//FIXFIXFIXFIX
		if (nx*gxmul.z + ny*gymul.z + nz*gzmul.z + gnadd.z > cornmin[ls]) goto tosibly;

		if (ls <= 0)
		{
			if (surftyp == 1) vis = 63;
			else
			{
				vis = 0; k = (1<< (nx-(cent->x-BITDIST-1)) );
				pbitvis = &bitvis[(ny-(cent->y-BITDIST-1)) +
										(nz-(cent->z-BITDIST-1))*16];
				if (!(pbitvis[  0]&(k>>1))) vis |= (1<<0);
				if (!(pbitvis[  0]&(k<<1))) vis |= (1<<1);
				if (!(pbitvis[ -1]&(k   ))) vis |= (1<<2);
				if (!(pbitvis[ +1]&(k   ))) vis |= (1<<3);
				if (!(pbitvis[-16]&(k   ))) vis |= (1<<4);
				if (!(pbitvis[+16]&(k   ))) vis |= (1<<5);
				if (surftyp) vis ^= 63;
			}
#if (MARCHCUBE == 0)
			drawcube_close(loct,nx,ny,nz,popcount[ptr->chi&(i-1)] + ptr->ind,0,vis);
#else
			drawcube_close_mc(loct,nx,ny,nz,popcount[ptr->chi&(i-1)] + ptr->ind,0);
#endif
			goto tosibly;
		}

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; stk[ls].ord = ord; ls--; s >>= 1; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
		ord = (z+s > cent->z)*4 + (y+s > cent->y)*2 + (x+s > cent->x);
		continue;

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) return; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z; ord = stk[ls].ord;
	}
}

static void oct_ftob2_dorect (int ind, void *_)
{
	__declspec(align(16)) lpoint4d stk[OCT_MAXLS];
	__declspec(align(16)) float psmin[4], cornadd[OCT_MAXLS][8];
	__declspec(align(16)) float vxi[8], vyi[8];
	int i, x, y, z, x0, y0, z0, x1, y1, z1, ls, stkind[OCT_MAXLS];
	unsigned int stkord[OCT_MAXLS];
	oct_t *loct = (oct_t *)_;

#if (USEASM == 0)
	__declspec(align(16)) lpoint4d pt;
	__declspec(align(16)) rect_t smax;
	__declspec(align(8))  stk2_t stk2;
	__declspec(align(16)) float oval[8] = {0}, val[8] = {0};
	float f, fx, fy, k0, k1, k2, k4, k5, k6, k8, k9, ka, kc, kd, ke;
	lpoint4d *pptr;
	float *fptr;
	surf_t *surf;
	unsigned int *uptr, ord;
	int k, col, v, sx, ind27;
	//__declspec(align(8)) stk2_t col2;
	int sptr, ipsurf;
	__int64 q;

#else
	__declspec(align(16)) static const int dq0fff[4] = {-1,-1,-1, 0};
	__declspec(align(16)) static short dqpitch[8];
#if (PIXMETH == 1)
	__declspec(align(8)) static const short q0040961[4] = {1,4096,0,0};
#endif
	__declspec(align(8)) static const short cubvmul[4] = {1*16,3*16,9*16,0};
	static int gxdf = 0, gxdp = 0;
	static const float qzero = 0.f;

	if (ind < 0)
	{
		static int ognodbuf = 0, ogcbit = 0, ongzbufoff = 0, oglsid = 0, init = 1;
		if (ognodbuf != (int)loct->nod.buf) //FIX:incompatible with MT rendering of separate models
		{
			ognodbuf = (int)loct->nod.buf;
			i = (int)&((octv_t *)loct->nod.buf)->chi;
			SELFMODVAL(init,selfmod_goctchi0-4,i);
			SELFMODVAL(init,selfmod_goctchi1-4,i);
			SELFMODVAL(init,selfmod_goctchi2-4,i);
			i = (int)&((octv_t *)loct->nod.buf)->ind;
			SELFMODVAL(init,selfmod_goctind0-4,i);
			SELFMODVAL(init,selfmod_goctind1-4,i);
		}
		if (ogcbit != (int)gcbit)
		{
			ogcbit = (int)gcbit;
			SELFMODVAL(init,selfmod_gcbit0-4,ogcbit);
			SELFMODVAL(init,selfmod_gcbit1c-4,ogcbit);
			SELFMODVAL(init,selfmod_gcbit2c-4,ogcbit);
			SELFMODVAL(init,selfmod_gcbit3c-4,ogcbit);
		}
		if (oglsid != loct->lsid)
		{
			oglsid = loct->lsid;
			SELFMODVAL(init,selfmod_maxls4-1,(char)(loct->lsid*4));
		}

		dqpitch[0] = 1; dqpitch[1] = gcbitpl; dqpitch[2] = PIXBUFBYPP; dqpitch[3] = gxd.p; dqpitch[4] = 0; dqpitch[5] = 0; dqpitch[6] = 0; dqpitch[7] = 0;
		gxdf = gxd.f;
		gxdp = gxd.p;

		init = 0; return;
	}

#endif

	psmin[0] = (float)grect[ind].x0; psmin[1] = (float)grect[ind].y0; psmin[2] = (float)-grect[ind].x1; psmin[3] = (float)-grect[ind].y1;

	x = grect[ind].x0+grect[ind].x1 - ighx*2; y = grect[ind].y0+grect[ind].y1 - ighy*2; z = ighz*2;
	x0 = (gxmul.x*z >= gxmul.z*x); y0 = (gymul.x*z >= gymul.z*x); z0 = (gzmul.x*z >= gzmul.z*x);
	cornadd[0][2] = x0*fgxmul.x + y0*fgymul.x + z0*fgzmul.x; cornadd[0][0] = fgxmul.x + fgymul.x + fgzmul.x - cornadd[0][2];
	cornadd[0][6] = x0*fgxmul.z + y0*fgymul.z + z0*fgzmul.z; cornadd[0][4] = fgxmul.z + fgymul.z + fgzmul.z - cornadd[0][6];
	x1 = (gxmul.y*z >= gxmul.z*y); y1 = (gymul.y*z >= gymul.z*y); z1 = (gzmul.y*z >= gzmul.z*y);
	cornadd[0][3] = x1*fgxmul.y + y1*fgymul.y + z1*fgzmul.y; cornadd[0][1] = fgxmul.y + fgymul.y + fgzmul.y - cornadd[0][3];
	cornadd[0][7] = x1*fgxmul.z + y1*fgymul.z + z1*fgzmul.z; cornadd[0][5] = fgxmul.z + fgymul.z + fgzmul.z - cornadd[0][7];
	cornadd[0][6] = -cornadd[0][6]; cornadd[0][7] = -cornadd[0][7]; //Negation is asm trick to eliminate minps
	for(ls=1;ls<loct->lsid;ls++) for(i=0;i<8;i++) cornadd[ls][i] = cornadd[ls-1][i]*2;

	ls = loct->lsid-1;
	stk[ls].x = fgnadd.x; stk[ls].y = fgnadd.y; stk[ls].z = fgnadd.z;

#if (USEASM == 0) //--------------------------------------------------------------------------------
	stk2.x = 0; stk2.y = 0; stk2.z = 0; stkind[ls] = loct->head; goto in2it;

tochild:;
	stkord[ls] = ord; ls--;
	stk[ls].x = pt.x; stk[ls].y = pt.y; stk[ls].z = pt.z;
	stkind[ls] = popcount[((octv_t *)loct->nod.buf)[stkind[ls+1]].chi&pow2m1[k]] + ((octv_t *)loct->nod.buf)[stkind[ls+1]].ind; //2child
	stk2.x = (stk2.x&glandlut[ls+1].x)|gklslut[k][ls+1].x;
	stk2.y = (stk2.y&glandlut[ls+1].y)|gklslut[k][ls+1].y;
	stk2.z = (stk2.z&glandlut[ls+1].z)|gklslut[k][ls+1].z;
in2it:;
	ord = ordlut[(stk2.x > gldirlut[ls].x) + (stk2.y > gldirlut[ls].y)*2 + (stk2.z > gldirlut[ls].z)*4][((octv_t *)loct->nod.buf)[stkind[ls]].chi];
top:;
	k = (ord&7);

	pptr = &gposadd[k][ls];
	pt.x = stk[ls].x + pptr->x;
	pt.y = stk[ls].y + pptr->y;
	pt.z = stk[ls].z + pptr->z;

	if (pt.z <= cornmin[ls]) //WARNING:can't use int cmp: cornmin[] may be +/-
	{
		if (pt.z <= cornmax[ls]) goto tosibly; //WARNING:can't use int cmp: cornmax[] may be +/-

		if (!isint_cone_sph(&giics[ls],(stk2.x&glandlut[ls].x)|gklslut[k][ls].x,
												 (stk2.y&glandlut[ls].y)|gklslut[k][ls].y,
												 (stk2.z&glandlut[ls].z)|gklslut[k][ls].z)) goto tosibly;

		goto tochild;
	}
	if (pt.z >= scanmax[ls]) goto tosibly;

	fptr = &cornadd[ls][0];
	smax.x0 = (int)max((pt.x+fptr[0]) / (pt.z+fptr[4]),grect[ind].x0);
	smax.y0 = (int)max((pt.y+fptr[1]) / (pt.z+fptr[5]),grect[ind].y0);
	smax.x1 = (int)min((pt.x+fptr[2]) / (pt.z-fptr[6]),grect[ind].x1);
	smax.y1 = (int)min((pt.y+fptr[3]) / (pt.z-fptr[7]),grect[ind].y1);
	smax.x1 -= smax.x0; smax.y1 -= smax.y0; if ((smax.x1 <= 0) || (smax.y1 <= 0)) goto tosibly;
	i = smax.y0*gcbitpl+smax.x0;

	v = (smax.x0&7)+smax.x1; if (v >= 32) goto covskip; //always recurse for large regions (don't bother to check cover map)
	uptr = (unsigned int *)(((int)gcbit) + (i>>3)); v = npow2[smax.x0&7]&pow2m1[v];
	for(sx=smax.y1;sx>0;sx--,uptr+=gcbitplo5) if (uptr[0]&v) goto covskip;
	goto tosibly;
covskip:;
	if (ls) goto tochild;

	sptr = smax.y0*gxd.p + smax.x0*PIXBUFBYPP + gxd.f;
	ipsurf = popcount[((octv_t *)loct->nod.buf)[stkind[0]].chi&pow2m1[k]] + ((octv_t *)loct->nod.buf)[stkind[0]].ind;
#if (PIXMETH == 1)
	q = (((__int64)(stk2.x+gkls0[k].x))    ) +
		 (((__int64)(stk2.y+gkls0[k].y))<<12) +
		 (((__int64)(stk2.z+gkls0[k].z))<<24) +
		 (((__int64)(ipsurf           ))<<36);
#endif
	{
#if 1    //FIXFIXFIXFIX:should be 0!
#if (MARCHCUBE == 0)
		drawcube_close(loct,stk2.x+gkls0[k].x,
								  stk2.y+gkls0[k].y,
								  stk2.z+gkls0[k].z,ipsurf,ind,63); goto tosibly; //fun with brute force
#else
		drawcube_close_mc(loct,stk2.x+gkls0[k].x,
									  stk2.y+gkls0[k].y,
									  stk2.z+gkls0[k].z,ipsurf,ind); goto tosibly; //fun with brute force
#endif
#endif

		fx = (float)(smax.x0+smax.x1-1);
		fy = (float)(smax.y0          );

		//if ((labs(stk2.x+gkls0[k].x-glorig.x) <= 6) &&
		//    (labs(stk2.y+gkls0[k].y-glorig.y) <= 6) &&
		//    (labs(stk2.z+gkls0[k].z-glorig.z) <= 6)) goto tosibly;

		ind27 = min(max(stk2.x+gkls0[k].x-glorig.x+1,0),2)*1
				+ min(max(stk2.y+gkls0[k].y-glorig.y+1,0),2)*3
				+ min(max(stk2.z+gkls0[k].z-glorig.z+1,0),2)*9;

		k0 = pt.z*gvany[0] - pt.y; *(int *)&k0 &= vause[ind27][0]; k4 = pt.x - pt.z*gvanx[0]; *(int *)&k4 &= vause[ind27][0];
		k1 = pt.z*gvany[1] - pt.y; *(int *)&k1 &= vause[ind27][1]; k5 = pt.x - pt.z*gvanx[1]; *(int *)&k5 &= vause[ind27][1];
		k2 = pt.z*gvany[2] - pt.y; *(int *)&k2 &= vause[ind27][2]; k6 = pt.x - pt.z*gvanx[2]; *(int *)&k6 &= vause[ind27][2];
		k8 = fx-gvanxmh[0]; kc = fy-gvanymh[0];
		k9 = fx-gvanxmh[1]; kd = fy-gvanymh[1];
		ka = fx-gvanxmh[2]; ke = fy-gvanymh[2];
		vxi[0] = cornvadd2[ind27][ 0] + k0; vyi[0] = cornvadd2[ind27][ 4] + k4; oval[0] = k8*vxi[0] + kc*vyi[0] + 1e-1; //FIX:+1e-1's not yet implemented in ASM!
		vxi[1] = cornvadd2[ind27][ 1] + k1; vyi[1] = cornvadd2[ind27][ 5] + k5; oval[1] = k9*vxi[1] + kd*vyi[1] + 1e-1;
		vxi[2] = cornvadd2[ind27][ 2] + k2; vyi[2] = cornvadd2[ind27][ 6] + k6; oval[2] = ka*vxi[2] + ke*vyi[2] + 1e-1;
		vxi[4] = cornvadd2[ind27][ 8] - k0; vyi[4] = cornvadd2[ind27][12] - k4; oval[4] = k8*vxi[4] + kc*vyi[4] + 1e-1;
		vxi[5] = cornvadd2[ind27][ 9] - k1; vyi[5] = cornvadd2[ind27][13] - k5; oval[5] = k9*vxi[5] + kd*vyi[5] + 1e-1;
		vxi[6] = cornvadd2[ind27][10] - k2; vyi[6] = cornvadd2[ind27][14] - k6; oval[6] = ka*vxi[6] + ke*vyi[6] + 1e-1;
		do
		{
			val[0]  = oval[0]; val[1]  = oval[1]; val[2]  = oval[2]; val[4]  = oval[4]; val[5]  = oval[5]; val[6]  = oval[6];
			oval[0] += vyi[0]; oval[1] += vyi[1]; oval[2] += vyi[2]; oval[4] += vyi[4]; oval[5] += vyi[5]; oval[6] += vyi[6];
			sx = smax.x1;
			do
			{
				if (((*(int *)&val[0]^*(int *)&val[4]) >= 0) && ((*(int *)&val[1]^*(int *)&val[5]) >= 0) && ((*(int *)&val[2]^*(int *)&val[6]) >= 0))
				{
					v = i+sx-1;
					if (gcbit[v>>5]&(1<<v))
					{
						gcbit[v>>5] ^= (1<<v);
#if (PIXMETH == 0)
						*(int *)((sx-1)*4 + sptr) = ipsurf;
#elif (PIXMETH == 1)
						*(__int64 *)((sx-1)*8 + sptr) = q;
#elif (PIXMETH == 2)
						*(int *)((sx-1)*16 + sptr + 0) = stk2.x+gkls0[k].x;
						*(int *)((sx-1)*16 + sptr + 4) = stk2.y+gkls0[k].y;
						*(int *)((sx-1)*16 + sptr + 8) = stk2.z+gkls0[k].z;
						*(int *)((sx-1)*16 + sptr +12) = ipsurf;
#endif
					}
				}
				val[0] -= vxi[0]; val[1] -= vxi[1]; val[2] -= vxi[2]; val[4] -= vxi[4]; val[5] -= vxi[5]; val[6] -= vxi[6];
				sx--;
			} while (sx > 0);
			sptr += gxd.p;
			i += gcbitpl; smax.y1--;
		} while (smax.y1 > 0);
	}

tosibly:;
	do
	{
		ord >>= 4; if (ord) goto top;
		ls++; if (ls >= loct->lsid) return; //2parent
		ord = stkord[ls];
	} while (1);

#else //--------------------------------------------------------------------------------------------
	stk[ls].z2 = -fgnadd.z;
	stkind[loct->lsid-1] = loct->head;

	_asm
	{
			//eax:ls*4                          mm0:temp:short life/color  xmm0:[   -sy1    -sx1     sy0     sx0]
			//ebx:temp:gcbitptr                 mm1:-                      xmm1:[sy0-sy1 sx0-sx1 sy0-sy1 sx0-sx1]/temp
			//ecx:temp:many places;cl as shift  mm2:[- z y x]              xmm2:[      y       x      z2       z] (3D screen space coord)
			//edx:temp:many places              mm3:temp                   xmm3:[      0       0    zptr gcbtptr] (pmaddwd results)
			//esi:temp:many places              mm4:temp:holds gddz/gxd    xmm4:-
			//edi:ord (stkord[ls] cache)        mm5:temp                   xmm5:-
			//ebp:(stack offset)                mm6:-                      xmm6:-
			//esp:dec y/jnz                     mm7:[0 0 0 0]              xmm7:(temp)

		push ebx
		push esi
		push edi
		mov i, esp

		mov eax, ls
		shl eax, 2

		pxor mm2, mm2
		pxor mm7, mm7

		mov esi, stkind[eax]
		jmp short in2it
align 16
tochild:
		mov stkord[eax], edi          ;stkord[ls] = ord;
		sub eax, 4                    ;ls--;

		movaps stk[eax*4], xmm2       ;stk[ls].x = pt.x; stk[ls].y = pt.y; stk[ls].z = pt.z;

		and edi, 7

		mov esi, stkind[eax+4]        ;stkind[ls] = popcount[loct->nod.buf[stkind[ls+1]].chi&pow2m1[k]] + loct->nod.buf[stkind[ls+1]].ptr; //2child
		movzx ecx, byte ptr [esi*8+0x88888888] _asm selfmod_goctchi2: ;octv_t.chi
		and ecx, pow2m1[edi*4]
		mov esi, [esi*8+0x88888888] _asm selfmod_goctind0: ;octv_t.ind
		add esi, popcount[ecx*4]
		mov stkind[eax], esi

		pand mm2, glandlut[eax*2+8]   ;stk2.x &= glandlut[ls+1].x; stk2.y &= glandlut[ls+1].y; stk2.z &= glandlut[ls+1].z;
		shl edi, LOCT_MAXLS+3         ;stk2.x |= gklslut[k][ls+1].x; stk2.y |= gklslut[k][ls+1].y; stk2.z |= gklslut[k][ls+1].z;
		por mm2, gklslut[edi+eax*2+8]

in2it:movq mm0, mm2                 ;ord = ordlut[(stk2.x > gldirlut[ls].x) + (stk2.y > gldirlut[ls].y)*2 + (stk2.z > gldirlut[ls].z)*4][loct->nod.buf[stkind[ls]].chi];
		pcmpgtw mm0, gldirlut[eax*2]
		packsswb mm0, mm7
		pmovmskb edx, mm0
		movzx esi, byte ptr [esi*8+0x88888888] _asm selfmod_goctchi0: ;octv_t.chi
		shl edx, 10
		mov edi, ordlut[edx+esi*4]

top:  mov edx, edi                  ;k = (ord&7);
		and edx, 7

			;Start divide calculation early..
		movaps xmm2, stk[eax*4]
		mov ebx, edx                  ;backup for code at covskip:
		shl edx, LOCT_MAXLS+4         ;ptr = &gposadd[k][ls]; pt.* = stk[ls].* + ptr->*;
		addps xmm2, gposadd[edx+eax*4]

		pshufd xmm0, xmm2, 0xee       ;xmm0: [pt.y  pt.x  pt.y pt.x]
		pshufd xmm1, xmm2, 0x50       ;xmm1: [pt.z2 pt.z2 pt.z pt.z]
		addps xmm0, cornadd[eax*8]    ;xmm0: [pt.y +fptr[3] pt.x +fptr[2] pt.y+fptr[1] pt.x+fptr[0]]
		addps xmm1, cornadd[eax*8+16] ;xmm1: [pt.z2+fptr[7] pt.z2+fptr[6] pt.z+fptr[5] pt.z+fptr[4]]
#if 1
		divps xmm0, xmm1
#elif 0
		rcpps xmm7, xmm1 ;newton-raphson formula (doesn't seem any faster than divps :/)
		mulps xmm1, xmm7
		mulps xmm1, xmm7
		addps xmm7, xmm7
		subps xmm7, xmm1
		mulps xmm0, xmm7
#else
		rcpps xmm1, xmm1 ;noticable artifacts (random missing pixels)
		mulps xmm0, xmm1
#endif
			;Lo probability out..
		ucomiss xmm2, cornmin[eax]    ;if (pt.z <= cornmin[ls]) { //WARNING:can't use int cmp: cornmin[] may be +/-
		ja short cornskip
		ucomiss xmm2, cornmax[eax]    ;if (pt.z <= cornmax[ls]) goto tosibly; else goto tochild; //WARNING:can't use int cmp: cornmax[] may be +/-
		jbe short tosibly

			;FIXFIXFIX:convert cone algo to use xmm2/screen space 3d coord!
			;if (!isint_cone_sph(&giics[ls],(stk2.x&glandlut[ls].x)|gklslut[k][ls].x,
			;                               (stk2.y&glandlut[ls].y)|gklslut[k][ls].y,
			;                               (stk2.z&glandlut[ls].z)|gklslut[k][ls].z)) goto tosibly;
		movq mm0, mm2
		pand mm0, glandlut[eax*2]     ;stk2.x &= glandlut[ls].x; stk2.y &= glandlut[ls].y; stk2.z &= glandlut[ls].z;
		shr edx, 1                    ;edx = (ord&7)<<(LOCT_MAXLS+3)
		por mm0, gklslut[edx+eax*2]   ;stk2.x |= gklslut[k][ls].x; stk2.y |= gklslut[k][ls].y; stk2.z |= gklslut[k][ls].z;
												;sx = (float)stk2.x; sy = (float)stk2.y; sz = (float)stk2.z;
		movq2dq xmm4, mm0             ;xmm4:[0  0| 0  0| 0 sz| sy sx]
		pshufd xmm4, xmm4, 0xd8       ;xmm4:[0  0| 0 sz| 0  0| sy sx]
		pshuflw xmm4, xmm4, 0xd8      ;xmm4:[0  0| 0 sz| 0 sy|  0 sx]
		cvtdq2ps xmm4, xmm4
		subps xmm4, giics[eax*8]      ;sx -= iics->x0; sy -= iics->y0; sz -= iics->z0;
		movaps xmm5, xmm4
		mulps xmm4, giics[eax*8+16]   ;sx *= iics->xv; sy *= iics->yv; sz *= iics->zv;
		mulps xmm5, xmm5              ;sx *= sx;       sy *= sy;       sz *= sz;
		movhlps xmm6, xmm4
		movhlps xmm7, xmm5
		addps xmm4, xmm6
		addps xmm5, xmm7
		pshuflw xmm6, xmm4, 0xe
		pshuflw xmm7, xmm5, 0xe
		addss xmm4, xmm6              ;xmm4[0] = sx*iics->xv + sy*iics->yv + sz*iics->zv
		addss xmm5, xmm7              ;xmm5[0] = sx*sx + sy*sy + sz*sz
		ucomiss xmm4, qzero           ;if (xmm4[0] <= 0.f) goto sibly;
		jbe short tosibly
		mulss xmm4, xmm4              ;if (xmm4[0]*xmm4[0] <= xmm5[0]*iics->cosang2) goto tosibly;
		mulss xmm5, giics[eax*8+28]
		ucomiss xmm4, xmm5
		jbe short tosibly

		jmp short tochild
align 16
cornskip:
		ucomiss xmm2, scanmax[eax]    ;if (pt.z >= scanmax[ls]) goto tosibly;
		jae short tosibly

		maxps xmm0, psmin             ;xmm0: [max(-sy1,-ymax) max(-sy1,-xmax) max(sy0,ymin) max(sx0,xmin)]
		cvtps2dq xmm0, xmm0           ;xmm0: [-sy1 -sx1  sy0  sx0]
		pshufd xmm1, xmm0, 0x4e       ;xmm1: [ sy0  sx0 -sy1 -sx1]
		paddd xmm1, xmm0              ;xmm1: [sy0-sy1 sx0-sx1 sy0-sy1 sx0-sx1]
		movmskps edx, xmm1
		cmp edx, 15
		jne short tosibly
		pxor xmm1, dq0fff             ;xmm1: [sy0-sy1 sx1-sx0-1 sy1-sy0-1 sx1-sx0-1]

			;zptr = sy0*gdd.p + (sx0<<2); i = sy0*gcbitpl + sx0;
		pshuflw xmm3, xmm0, 0x88      ;   xmm3:[? ? ? ?   sy0 sx0     sy0 sx0]
		pmaddwd xmm3, dqpitch         ;dqpitch:[0 0 0 0 gdd.p   4 gcbitpl   1]
		movd esp, xmm3

			;NOTE:always checking gcbit is faster than only doing it for ls>0
		movd ecx, xmm0 ;sx0        ;v = (sx0&7)+sx1;
		and ecx, 7
		movd esi, xmm1 ;sx1
		add esi, ecx
		cmp esi, 31                ;if (v >= 32) goto tochild; //always recurse for large regions (don't bother to check cover map)
		jae short covskip
		shr esp, 3                 ;uptr = (unsigned int *)(((int)gcbit) + (i>>3));
		pextrw edx, xmm1, 2 ;sy1   ;v = npow2[sx0&7]&pow2m1[v];
		mov esi, pow2m1[esi*4+4]
		and esi, npow2[ecx*4]      ;for(;sy1>0;sy1--,uptr+=gcbitplo5) if (uptr[0]&v) goto tochild;
covbegy: test esi, [esp+0x88888888] _asm selfmod_gcbit0:
			jne short covskip
			add esp, gcbitplo3
			sub edx, 1
			jge short covbegy
		jmp short tosibly          ;goto tosibly;
align 16                         ;}
covskip:
		test eax, eax
		jnz short tochild

		mov edx, stkind[eax]          ;mm3: ipsurf = popcount[loct->nod.buf[stkind[ls]].chi&pow2m1[k]] + loct->nod.buf[stkind[ls]].ptr;
		movzx esi, byte ptr [edx*8+0x88888888] _asm selfmod_goctchi1: ;octv_t.chi
		and esi, pow2m1[ebx*4]
		mov edx, [edx*8+0x88888888] _asm selfmod_goctind1: ;octv_t.ind
#if (PIXMETH == 3) //not implemented yet!
		mov esi, popcount[esi*4]
		lea edx, [edx*8+esi]
#else
		add edx, popcount[esi*4]
#endif
		movd mm3, edx
;---------------------------------------------------------------------------------------------------
		movq mm0, gkls0[ebx*8]        ;mm0:[- z=stk2.z+gkls0[k].z y=stk2.y+gkls0[k].y x=stk2.x+gkls0[k].x]
		paddw mm0, mm2

			;if ((labs(stk2.x+gkls0[k].x-glorig.x) <= 6) &&
			;    (labs(stk2.y+gkls0[k].y-glorig.y) <= 6) &&
			;    (labs(stk2.z+gkls0[k].z-glorig.z) <= 6)) goto tosibly;
		;movq mm5, mm0
		;psubw mm5, ?                 ;[0 -32768+ 6-glorig.z -32768+ 6-glorig.y -32768+ 6-glorig.x]
		;pcmpgtw mm5, ?               ;[0 -32768+12-glorig.z -32768+12-glorig.y -32768+12-glorig.x]
		;pmovmskb ecx, mm5
		;test ecx, 0x3f
		;jnz short tochild

		movq mm5, mm0
		paddsw mm5, glcenadd
		psubusw mm5, glcensub         ;mm5:[0 sgn(z)+1][sgn(y)+1 sgn(x)+1]
		pmaddwd mm5, cubvmul          ;   *[0     9*16][    3*16     1*16]
		movd ecx, mm5
		pextrw esp, mm5, 2
		add ecx, esp

#if (PIXMETH == 1)
		pshufw mm5, mm0, 0xfe         ;mm5:[0000000000000000000000000000000000000000000000000000Zzzzzzzzzzzz]
												;mm0:[00000000000000000000Zzzzzzzzzzzz0000Yyyyyyyyyyyy0000Xxxxxxxxxxxx]
												;                   *0              *0           *4096              *1
		pmaddwd mm0, q0040961         ;mm0:[0000000000000000000000000000000000000000YyyyyyyyyyyyXxxxxxxxxxxx]
		psllq mm5, 24                 ;mm5:[0000000000000000000000000000Zzzzzzzzzzzz000000000000000000000000]
		psllq mm3, 36                 ;mm3:[Iiiiiiiiiiiiiiiiiiiiiiiiiiii000000000000000000000000000000000000]
		por mm3, mm5
		por mm0, mm3                  ;mm0:[IiiiiiiiiiiiiiiiiiiiiiiiiiiiZzzzzzzzzzzzYyyyyyyyyyyyXxxxxxxxxxxx]
#endif

		movd ebx, xmm3                ;ebx: gcbitptr

		pshuflw xmm3, xmm3, 0xe       ;eax: sptr = sy0*gxd.p + (sx0<<1) + gxd.f;
		movd eax, xmm3
		add eax, gxdf

;===================================================================================================
			//Cube rasterization. Register status at this point:
			//eax:gddf                    mm0:xyzsurf data  xmm0:[   -sy1    -sx1     sy0     sx0]
			//ebx:temp:gcbitptr           mm1:-             xmm1:[      -       -   sy1-1   sx1-1]
			//ecx:temp:ind27*16           mm2:[- z y x]     xmm2:[      y       x      z2       z] (3D screen space coord)
			//edx:-                       mm3:ipsurf        xmm3:-
			//esi:-                       mm4:-             xmm4:-
			//edi:ord (stkord[ls] cache)  mm5:-             xmm5:-
			//ebp:(stack offset)          mm6:-             xmm6:-
			//esp:-                       mm7:[0 0 0 0]     xmm7:-

		pshufd xmm3, xmm2, 0x00       ;xmm3: [pt.z pt.z pt.z pt.z]
		movaps xmm5, xmm3             ;xmm5: [pt.z pt.z pt.z pt.z]
		mulps xmm3, gvany[0]
		mulps xmm5, gvanx[0]
		pshufd xmm4, xmm2, 0xff       ;xmm4: [pt.y pt.y pt.y pt.y]
		subps xmm3, xmm4
		pshufd xmm4, xmm2, 0xaa       ;xmm4: [pt.x pt.x pt.x pt.x]
		subps xmm4, xmm5
		pand xmm3, vause[ecx]         ;xmm3: [   0   k2   k1   k0]
		pand xmm4, vause[ecx]         ;xmm4: [   0   k6   k5   k4]

		pshufd xmm0, xmm0, 0x6a       ;xmm0:[ y0   -x1   -x1   -x1]
		pxor xmm0, dq0fff             ;xmm0:[ y0  x1-1  x1-1  x1-1]
		cvtdq2ps xmm5, xmm0           ;xmm5:[fy0 fx1-1 fx1-1 fx1-1]
		pshufd xmm6, xmm5, 0xff       ;xmm6:[ -    fy0   fy0   fy0]
		subps xmm5, gvanxmh[0]        ;xmm5:[ -     ka    k9    k8]
		subps xmm6, gvanymh[0]        ;xmm6:[ -     ke    kd    kc]

		movaps xmm0, cornvadd2[ecx*4+32]
		movaps xmm7, cornvadd2[ecx*4+48]
		subps xmm0, xmm3
		subps xmm7, xmm4
		addps xmm3, cornvadd2[ecx*4]
		addps xmm4, cornvadd2[ecx*4+16]
		movaps vxi[ 0], xmm3
		movaps vxi[16], xmm0
		movaps vyi[ 0], xmm4
		movaps vyi[16], xmm7

		mulps xmm0, xmm5
		mulps xmm5, xmm3
		mulps xmm4, xmm6
		mulps xmm6, xmm7
		addps xmm5, xmm4              ;xmm5 = xmm5*vxi[ 0] + xmm6*vyi[ 0] ;<- oval[ 0]
		addps xmm6, xmm0              ;xmm6 = xmm5*vxi[16] + xmm6*vyi[16] ;<- oval[16]
		pand xmm5, vause[ecx]
		pand xmm6, vause[ecx]

	  ;addps xmm5, dq?               ;oval[ 0]
	  ;addps xmm6, dq?               ;oval[16]

;===================================================================================================

#if (PIXMETH == 2)
		movq2dq xmm4, mm0             ;mm0:[00000000000000000000Zzzzzzzzzzzz0000Yyyyyyyyyyyy0000Xxxxxxxxxxxx]
		pxor xmm0, xmm0
		punpcklwd xmm4, xmm0
		movq2dq xmm0, mm3             ;mm3:[00000000000000000000000000000000Iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii]
		pslldq xmm0, 12
		por xmm4, xmm0
#endif

			; val: oval: | [3] [2] [1] [0]
			;------------+----------------
			; xmm0  xmm5 |  0  v2  v1  v0
			; xmm3  xmm6 |  0  v6  v5  v4
;-----------------------------
			;eax:gxdptr, ebx:gcbitptr, ecx:temp, edx:temp, esi:xcnt, edi:(ord), ebp:(stack offset), esp:ycnt
		pextrw esp, xmm1, 2 ;sy1-1    ;sy = sy1-1;
begpixyc:movd esi, xmm1 ;sx1-1      ;sx = sx1-1;
			movaps xmm0, xmm5
			movaps xmm3, xmm6
			addps xmm5, vyi[ 0]
			addps xmm6, vyi[16]
begpixxc:
#if 0
				movmskps ecx, xmm0   ;3,1
				movmskps edx, xmm3   ;3,1
				subps xmm0, vxi[ 0]
				subps xmm3, vxi[16]
				xor ecx, edx
				jnz short skippixc
#else
				movaps xmm7, xmm0    ;1,0.33
				pxor xmm7, xmm3      ;1,0.33
				movmskps edx, xmm7   ;3,1
				subps xmm0, vxi[ 0]
				subps xmm3, vxi[16]
				test edx, edx
				jnz short skippixc
#endif
				lea ecx, [ebx+esi]      ;v = i+sx;
				mov edx, 1              ;if (gcbit[v>>5]&(1<<v)) {
				shl edx, cl
				shr ecx, 5
				test edx, [ecx*4+0x88888888] _asm selfmod_gcbit1c:
				jz short skippixc
					xor edx, [ecx*4+0x88888888] _asm selfmod_gcbit2c:            ;gcbit[v>>5] ^= (1<<v);
					mov [ecx*4+0x88888888], edx _asm selfmod_gcbit3c:

#if (PIXMETH == 0)
					movd [eax+esi*4], mm3
#elif (PIXMETH == 1)
					movq [eax+esi*8], mm0
#elif (PIXMETH == 2)
					lea edx, [esi*2]
					movntps [eax+edx*8], xmm4
#endif
skippixc:   sub esi, 1              ;sx--;
				jge short begpixxc      ;} while (sx >= 0);
			add eax, gxdp              ;sptr += gxdp;
			add ebx, gcbitpl           ;i += gcbitpl;
			sub esp, 1                 ;sy--;
			jge short begpixyc         ;while (sy >= 0);

		xor eax, eax                  ;clear eax back to 0 for use as ls*4
;---------------------------------------------------------------------------------------------------
tosibly:
		shr edi, 4                    ;ord >>= 4; if (ord) goto top;
		jnz short top
		add eax, 4                    ;ls++; //2parent
		mov edi, stkord[eax]          ;ord = stkord[ls];
		cmp eax, 0x78 _asm selfmod_maxls4:
		jb short tosibly              ;} while (ls < loct->lsid);

		mov esp, i
		pop edi
		pop esi
		pop ebx

		emms
	}
#endif //-------------------------------------------------------------------------------------------
}

static void oct_setcam (tiletype *dd, int zbufoff, point3d *p, point3d *r, point3d *d, point3d *f, float hx, float hy, float hz)
{
	static int inited = 0;

	if (!inited)
	{
		int i, ls;
		inited = 1;
		for(ls=0;ls<OCT_MAXLS;ls++)
		{
			glandlut[ls].x = ((-2)<<(ls-0));
			glandlut[ls].y = ((-2)<<(ls-0));
			glandlut[ls].z = ((-2)<<(ls-0));
			glandlut[ls].dum = 0;
		}
		for(ls=0;ls<OCT_MAXLS;ls++)
			for(i=0;i<8;i++)
			{
				gklslut[i][ls].x = (((i   )&1)<<ls);
				gklslut[i][ls].y = (((i>>1)&1)<<ls);
				gklslut[i][ls].z = (((i>>2)&1)<<ls);
				gklslut[i][ls].dum = 0;
			}
		for(i=0;i<8;i++) gkls0[i] = gklslut[i][0];

		int ord, msk, v, n;
		for(ord=0;ord<8;ord++)
			for(msk=0;msk<256;msk++)
			{
				v = 0; n = 0;
				for(i=0;i<8;i++) if (msk&(1<<(ord^i^7))) { v += ((ord^i^15)<<n); n += 4; }
				ordlut[ord][msk] = v;
			}
		if (!oct_usegpu) { gxd.f = 0; gxd.p = 0; gxd.x = 0; gxd.y = 0; }
	}
	if (!oct_usegpu)
	{
		if (dd->x*dd->y > gxd.x*gxd.y)
		{
			if (gxd.f) free((void *)gxd.f);
			gxd.f = (INT_PTR)malloc(dd->x*dd->y*PIXBUFBYPP); if (!gxd.f) { MessageBox(ghwnd,"oct_setcam():malloc failed",prognam,MB_OK); }
		}
		gxd.p = dd->x*PIXBUFBYPP; gxd.x = dd->x; gxd.y = dd->y;
	}

	if (!oct_usegpu) { gdd = (*dd); gzbufoff = zbufoff; gddz = gdd.f+zbufoff; }
#if (GPUSEBO == 0)
	else
	{
		if (gpixbufmal < gpixxdim*gpixydim*PIXBUFBYPP)
		{
			gpixbufmal = gpixxdim*gpixydim*PIXBUFBYPP;
			gpixbuf = (char *)realloc(gpixbuf,gpixbufmal);
		}
		gxd.f = (INT_PTR)gpixbuf; gxd.p = xres*PIXBUFBYPP; gxd.x = xres; gxd.y = yres;
	}
#endif
	gipos = (*p); girig = (*r); gidow = (*d); gifor = (*f);
	ghx = hx; ghy = hy; ghz = hz;
	ighx = (int)hx; ighy = (int)hy; ighz = (int)hz;
}
static void oct_setcam (tiletype *dd, int zbufoff, dpoint3d *p, dpoint3d *r, dpoint3d *d, dpoint3d *f, double hx, double hy, double hz)
{
	point3d fp, fr, fd, ff;
	fp.x = p->x; fp.y = p->y; fp.z = p->z;
	fr.x = r->x; fr.y = r->y; fr.z = r->z;
	fd.x = d->x; fd.y = d->y; fd.z = d->z;
	ff.x = f->x; ff.y = f->y; ff.z = f->z;
	oct_setcam(dd,zbufoff,&fp,&fr,&fd,&ff,(float)hx,(float)hy,(float)hz);
}

//--------------------------------------------------------------------------------------------------
static int glmost[MAXYDIM], grmost[MAXYDIM], gxmin, gymin, gxmax, gymax;
static void rastquad (point3d vt[8], const int ind[4], int *lmost, int *rmost, int *xmin, int *ymin, int *xmax, int *ymax)
{
	point3d vt2[8];
	float f, fi, fsx[8], fsy[8];
	int i, j, sy, sy0, sy1, imost[2], scry[8], n2;
	#define SCISDIST 5e-4 //NOTE:1e-6 fails on some /ils>0

	(*xmin) = 0x7fffffff; (*ymin) = 0x7fffffff;
	(*xmax) = 0x80000000; (*ymax) = 0x80000000;
	for(i=4-1,j=0,n2=0;j<4;i=j,j++)
	{
		if (vt[ind[i]].z >= SCISDIST) { vt2[n2] = vt[ind[i]]; n2++; }
		if ((vt[ind[i]].z >= SCISDIST) != (vt[ind[j]].z >= SCISDIST))
		{
			f = (SCISDIST-vt[ind[j]].z)/(vt[ind[i]].z-vt[ind[j]].z);
			vt2[n2].x = (vt[ind[i]].x-vt[ind[j]].x)*f + vt[ind[j]].x;
			vt2[n2].y = (vt[ind[i]].y-vt[ind[j]].y)*f + vt[ind[j]].y;
			vt2[n2].z = SCISDIST; n2++;
		}
	} if (n2 < 3) return;

	for(i=n2-1;i>=0;i--)
	{
		f = ghz/vt2[i].z;
		fsx[i] = vt2[i].x*f + ghx;
		fsy[i] = vt2[i].y*f + ghy;

		j = min(max(-cvttss2si(-fsx[i]),0),xres);
		if (j < (*xmin)) { (*xmin) = j; }
		if (j > (*xmax)) { (*xmax) = j; }

		scry[i] = min(max(-cvttss2si(-fsy[i]),0),yres);
		if (scry[i] < (*ymin)) { (*ymin) = scry[i]; imost[0] = i; }
		if (scry[i] > (*ymax)) { (*ymax) = scry[i]; imost[1] = i; }
	}
	i = imost[1]; sy = (*ymax);
	do
	{
		j = i+1; if (j >= n2) j = 0;
		if (sy > scry[j])
		{
			fi = (fsx[j]-fsx[i])/(fsy[j]-fsy[i]); f = (sy-fsy[j])*fi + fsx[j] + 0.5; //make CPU render more than necessary; don't trust GPU rounding
			do { f -= fi; sy--; lmost[sy] = cvttss2si(f); } while (sy > scry[j]);
		}
		i = j;
	} while (i != imost[0]);
	do
	{
		j = i+1; if (j >= n2) j = 0;
		if (sy < scry[j])
		{
			fi = (fsx[j]-fsx[i])/(fsy[j]-fsy[i]); f = (sy-fsy[i])*fi + fsx[i] + 1.5; //make CPU render more than necessary; don't trust GPU rounding
			do { rmost[sy] = cvttss2si(f); sy++; f += fi; } while (sy < scry[j]);
		}
		i = j;
	} while (i != imost[1]);
}

	//set gcbit if rasterized quad depth (front faces of current sprite) is behind value in zbuf
static void setgcbitifz (oct_t *loct, int face, int *lmost, int *rmost, int ymin, int ymax)
{
	__declspec(align(16)) static const float dq0123[4] = {0.f,1.f,2.f,3.f};
	__declspec(align(16)) static const float dq1111[4] = {1.f,1.f,1.f,1.f};
	__declspec(align(16)) static const float dq4444[4] = {4.f,4.f,4.f,4.f};
	float f, g, fx, fy, fz, topt, *fptr;
	unsigned int *gcptr;
	int sx, sy, sx0, sx1;

		//intx = vx*t = gxmul.x*a + gymul.x*b + gzmul.x*c + gnadd.x
		//inty = vy*t = gxmul.y*a + gymul.y*b + gzmul.y*c + gnadd.y
		//intz = vz*t = gxmul.z*a + gymul.z*b + gzmul.z*c + gnadd.z
		//(-vx)*t + gymul.x*b + gzmul.x*c = gxmul.x-gnadd.x
		//(-vy)*t + gymul.y*b + gzmul.y*c = gxmul.y-gnadd.y
		//(-vz)*t + gymul.z*b + gzmul.z*c = gxmul.z-gnadd.z
	topt = 0.0; if (face&1) f = 0.f; else f = (float)loct->sid;
	switch(face>>1)
	{
		case 0: fx = gzmul.y*gymul.z - gymul.y*gzmul.z; topt  = (gxmul.x*f + gnadd.x)*fx;
				  fy = gzmul.z*gymul.x - gymul.z*gzmul.x; topt += (gxmul.y*f + gnadd.y)*fy;
				  fz = gzmul.x*gymul.y - gymul.x*gzmul.y; topt += (gxmul.z*f + gnadd.z)*fz; break;
		case 1: fx = gxmul.y*gzmul.z - gzmul.y*gxmul.z; topt  = (gymul.x*f + gnadd.x)*fx;
				  fy = gxmul.z*gzmul.x - gzmul.z*gxmul.x; topt += (gymul.y*f + gnadd.y)*fy;
				  fz = gxmul.x*gzmul.y - gzmul.x*gxmul.y; topt += (gymul.z*f + gnadd.z)*fz; break;
		case 2: fx = gymul.y*gxmul.z - gxmul.y*gymul.z; topt  = (gzmul.x*f + gnadd.x)*fx;
				  fy = gymul.z*gxmul.x - gxmul.z*gymul.x; topt += (gzmul.y*f + gnadd.y)*fy;
				  fz = gymul.x*gxmul.y - gxmul.x*gymul.y; topt += (gzmul.z*f + gnadd.z)*fz; break;
	}
	f = (float)loct->sid/(topt*512.0); fx *= f; fy *= f; fz *= f;
	//z_buffer_depth = 1.0 / ((sx-ghx)*fx + (sy-ghy)*fy + ghz*fz)  (true at this line of code)

	for(sy=ymin;sy<ymax;sy++)
	{
		sx0 = max(lmost[sy],   0);
		sx1 = min(rmost[sy],xres); if (sx0 >= sx1) continue;
#if 1    //safe
		setzrange1(&gcbit[sy*gcbitplo5],sx0,sx1); //just let it overdraw - no savings on rendering 2nd+ sprite
#elif 0
			//WARNING: this block may cause a rare crash in the CPU shader, trying to load an invalid surf due to gcbit being already 0! Z-fighting?
			//Pure C version of below
		f = (sx0-ghx)*fx + (sy-ghy)*fy + ghz*fz;
		fptr = (float *)(gdd.p*sy + gddz);
		gcptr = &gcbit[sy*gcbitplo5];
		for(sx=sx0;sx<sx1;sx++,f+=fx)
		{
			g = fptr[sx]*f;
			if (*(int *)&g > 0x3f800000) //if (g > 1.f)
			{
				gcptr[sx>>5] |= (1<<sx);
				//*(int *)(gdd.f + gdd.p*sy + (sx<<2)) = (((int)f)&255)*0x10101; //Debug only!
			}
		}
#else
			//WARNING: this block may cause a rare crash in the CPU shader, trying to load an invalid surf due to gcbit being already 0! Z-fighting?
			//SSE2 version of above
		f = (sx0-ghx)*fx + (sy-ghy)*fy + ghz*fz;
		fptr = (float *)(gdd.p*sy + gddz);
		gcptr = &gcbit[sy*gcbitplo5];
		while (sx0&7) {        g = fptr[sx0]*(             f); if (*(int *)&g > 0x3f800000) gcptr[sx0>>5] |= (1<<sx0); sx0++; f += fx; if (sx0 >= sx1) goto cont; }
		while (sx1&7) { sx1--; g = fptr[sx1]*((sx1-sx0)*fx+f); if (*(int *)&g > 0x3f800000) gcptr[sx1>>5] |= (1<<sx1);                 if (sx0 >= sx1) goto cont; }
		fptr += sx0; gcptr = (unsigned int *)((sx0>>3) + (INT_PTR)gcptr); sx1 = ((sx1-sx0)>>3);
		_asm
		{
			push esi
			push edi
			movss xmm0, f         ;xmm0: [     0      0      0      f]
			movss xmm1, fx        ;xmm1: [     0      0      0     fx]
			pshufd xmm0, xmm0, 0  ;xmm0: [     f      f      f      f]
			pshufd xmm1, xmm1, 0  ;xmm1: [    fx     fx     fx     fx]
			movaps xmm2, xmm1     ;xmm2: [    fx     fx     fx     fx]
			mulps xmm1, dq4444    ;xmm1: [  fx*4   fx*4   fx*4   fx*4]
			mulps xmm2, dq0123    ;xmm2: [  fx*3   fx*2   fx*1   fx*0]
			addps xmm0, xmm2      ;xmm0: [f+fx*3 f+fx*2 f+fx*1 f+fx*0]
			movaps xmm7, dq1111   ;xmm7: [     1      1      1      1]
			mov esi, fptr
			mov edi, gcptr
			mov ecx, sx1
			add edi, ecx
			neg ecx
		 beg: movaps xmm2, [esi]
				movaps xmm3, [esi+16]
				mulps xmm2, xmm0
				addps xmm0, xmm1
				mulps xmm3, xmm0
				addps xmm0, xmm1
				pcmpgtd xmm2, xmm7
				pcmpgtd xmm3, xmm7
				movmskps eax, xmm2
				movmskps edx, xmm3
				shl edx, 4
				add eax, edx
				mov byte ptr [edi+ecx], al
				add esi, 32
				add ecx, 1
				jnz short beg
			pop edi
			pop esi
		}
cont:;
#endif
	}
}

static void setgcbit (int *lmost, int *rmost, int ymin, int ymax)
{
	int sy, sx0, sx1;
	for(sy=ymin;sy<ymax;sy++)
	{
		sx0 = max(lmost[sy],   0);
		sx1 = min(rmost[sy],xres); if (sx0 >= sx1) continue;
		setzrange1(&gcbit[sy*gcbitplo5],sx0,sx1);
	}
}

static void clearbackpixes (int sy, void *_)
{
	int sx, sxs, sx0, sx1;
	unsigned int *gcptr;
	INT_PTR rptr;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],xres); if (sx0 >= sx1) return;

	rptr = sy*gxd.p + gxd.f;
	gcptr = &gcbit[sy*gcbitplo5];

	sxs = sx1;
	while (1)
	{
		sx  = uptil1(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil0(gcptr,sx -1); sxs = max(sxs,sx0);
		memset4((void *)(rptr+sxs*PIXBUFBYPP),0x00000000,(sx-sxs)*PIXBUFBYPP);
	}
}

static point3d grxmul, grymul, grzmul;
static float gzscale, gmipoffs;
#define LTEXPREC 7
#define TEXPREC (1<<LTEXPREC)

#if (PIXMETH == 0)

	//pixmeth 0..
static void cpu_shader_solcol (int sy, void *_)
{
	int sx, sxs, sx0, sx1, *cptr, *rptr, col;
	surf_t *psurf;
	unsigned int *gcptr;
	oct_t *loct;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (int *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	gcptr = &gcbit[sy*gcbitplo5];
	loct = (oct_t *)_;

	sxs = sx1;
	while (1)
	{
		sx  = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
		for(sx--;sx>=sxs;sx--)
		{
			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]];
			cptr[sx] = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);
		}
	}
}
static void cpu_shader_znotex (int sy, void *_) { cpu_shader_solcol(sy,_); }
static void cpu_shader_texmap (int sy, void *_) { cpu_shader_solcol(sy,_); }

#elif (PIXMETH == 1)

	//(default), 1024x768:
	//           C:   ASM:
	//CPU_TEX: 217.5  282.4 (both nearest)
	//CPU_ZNT: 255.2  323.2
	//CPU_SOL: 352.5  363.4

__declspec(align(16)) static float dqgrxmul[4], dqgorig[4], dqfwmul[4];
__declspec(align(16)) static int dqiwmsk[4];
__declspec(align(16)) static short dqfogcol[8], dqimulcol[8], dqtilepitch4[16][8], dqmixval[8];
static int tilesf[16];

	//pixmeth 1..
static void cpu_shader_solcol_mc (int sy, void *_)
{
	point3d v0, v, norm;
	float d, fu, fv;
	__int64 *rptr;
	int x, y, z, sx, sxs, sx0, sx1, *cptr, col;
	surf_t *psurf;
	unsigned int *gcptr;
	oct_t *loct;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (__int64 *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	gcptr = &gcbit[sy*gcbitplo5];
	loct = (oct_t *)_;

	v.x = -(ghx-.5); v.y = sy-(ghy-.5); v.z = ghz;
	v0.x = v.x*grxmul.x + v.y*grymul.x + v.z*grzmul.x;
	v0.y = v.x*grxmul.y + v.y*grymul.y + v.z*grzmul.y;
	v0.z = v.x*grxmul.z + v.y*grymul.z + v.z*grzmul.z;

	sxs = sx1;
	while (1)
	{
		sx  = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
		for(sx--;sx>=sxs;sx--)
		{
			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
			x = (((int) rptr[sx]     )&4095);
			y = (((int)(rptr[sx]>>12))&4095);
			z = (((int)(rptr[sx]>>24))&4095);
			v.x = sx*grxmul.x + v0.x;
			v.y = sx*grxmul.y + v0.y;
			v.z = sx*grxmul.z + v0.z;
#if 0
			d = marchcube_hitscan(&gorig,&v,x,y,z,psurf,&norm,&fu,&fv);
			cptr[sx] = mulsc(*(int *)&psurf->b,(int)((norm.x+norm.y+norm.z)*128.0+256));
#else
			cptr[sx] = mulsc(*(int *)&psurf->b,(int)(psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256));
#endif
		}
	}
}

	//pixmeth 1..
static void cpu_shader_solcol (int sy, void *_)
{
	__int64 *rptr;
	int sx, sxs, sx0, sx1, *cptr, col;
	surf_t *psurf;
	unsigned int *gcptr;
	oct_t *loct;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (__int64 *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	gcptr = &gcbit[sy*gcbitplo5];
	loct = (oct_t *)_;

	sxs = sx1;
	while (1)
	{
		sx  = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
#if 0
		for(sx--;sx>=sxs;sx--)
		{
			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
			cptr[sx] = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);
		}
#else
		__declspec(align(16)) static short dq256s[8] = {256,256,256,0,0,0,0,0};
		int i = (int)loct->sur.buf;
		_asm
		{
			push ebx
			push esi
			push edi
			mov ebx, i

			mov ecx, sx
			mov esi, cptr
			jmp short in2it

beg:           ;psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
				mov edi, rptr
				mov edi, [edi+ecx*8+4]
				shr edi, 4
#if ((SURF_SIZ == 4) || (SURF_SIZ == 8))
				#define SURF_SIZMUL SURF_SIZ
#elif (SURF_SIZ == 16)
				#define SURF_SIZMUL 8
				add edi, edi
#elif (SURF_SIZ == 12)
				#define SURF_SIZMUL (SURF_SIZ/3)
				lea edi, [edi+edi*2]
#else
				#define SURF_SIZMUL 1
				imul edi, SURF_SIZ
#endif

#if 1
				movsx eax, byte ptr [edi*SURF_SIZMUL+ebx+4]
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+5]
				add eax, edx
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+6]
				lea eax, [eax+edx+256]

				movd xmm0, [edi*SURF_SIZMUL+ebx]
				punpcklbw xmm0, xmm0
				movd xmm1, eax
				pshuflw xmm1, xmm1, 0
#else
					;slower than ^ :/
				movlps xmm0, [edi*SURF_SIZMUL+ebx]
				punpcklbw xmm0, xmm0     ;[x x nz nz ny ny nx nx a a r r g g b b]
				movhlps xmm1, xmm0
				pmaddwd xmm1, dq256s     ;*= [0 0 0 0 0 256 256 256]
				pshuflw xmm2, xmm1, 0x4e ;xmm2:[1 0 3 2]
				paddd xmm1, xmm2
				paddd xmm1, dq256s       ;[0 0 0 256]
				pshuflw xmm1, xmm1, 0x55
#endif
				pmulhuw xmm0, xmm1
				packuswb xmm0, xmm0
				movd [esi+ecx*4], xmm0

in2it:      sub ecx, 1
				cmp ecx, sxs
				jge short beg

			pop edi
			pop esi
			pop ebx
		}
#endif
	}
}

	//pixmeth 1..
static void cpu_shader_znotex (int sy, void *_)
{
	surf_t *psurf;
	point3d v0, v, p;
	int ivs[3];
	float f, fsc, fz, dep, *zptr, fwmul, fogmul;
	int i, j, k, m, x, y, z, sx, sxs, sx0, sx1, *cptr, sid, col, dofog;
	unsigned int *gcptr;
	__int64 *rptr;
	oct_t *loct = (oct_t *)_;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (__int64 *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	zptr = (float *)(sy*gdd.p + gddz);

	v.x = -(ghx-.5); v.y = sy-(ghy-.5); v.z = ghz;
	v0.x = v.x*grxmul.x + v.y*grymul.x + v.z*grzmul.x;
	v0.y = v.x*grxmul.y + v.y*grymul.y + v.z*grzmul.y;
	v0.z = v.x*grxmul.z + v.y*grymul.z + v.z*grzmul.z;
	fsc = gzscale;

	psurf = 0; gcptr = &gcbit[sy*gcbitplo5];

	if (oct_fogdist >= 1e32) dofog = 0; else { fogmul = ghz*32768.0/((double)loct->sid*oct_fogdist); dofog = 1; }

#if 0
	sxs = sx1;
	while (1)
	{
		sx = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
		for(sx--;sx>=sxs;sx--)
		{
			v.x = sx*grxmul.x + v0.x;
			v.y = sx*grxmul.y + v0.y;
			v.z = sx*grxmul.z + v0.z;

			x = (((int) rptr[sx]     )&4095);
			y = (((int)(rptr[sx]>>12))&4095);
			z = (((int)(rptr[sx]>>24))&4095);

			ivs[0] = ((*(unsigned int *)&v.x)>>31);
			ivs[1] = ((*(unsigned int *)&v.y)>>31);
			ivs[2] = ((*(unsigned int *)&v.z)>>31);

			p.x = (float)(ivs[0]+x)-gorig.x;
			p.y = (float)(ivs[1]+y)-gorig.y;
			p.z = (float)(ivs[2]+z)-gorig.z;
#if 0
			f = p.x/v.x;             { fz = f; sid = 0; }
			f = p.y/v.y; if (f > fz) { fz = f; sid = 1; }
			f = p.z/v.z; if (f > fz) { fz = f; sid = 2; }
#else
			sid = (p.x*v.y < p.y*v.x) != (ivs[0]^ivs[1]);
			if ((((float *)&p)[sid]*v.z < p.z*((float *)&v)[sid]) != (ivs[sid]^ivs[2])) sid = 2;
			fz = ((float *)&p)[sid]/((float *)&v)[sid];
#endif
			dep = fz*fsc; if (*(int *)&zptr[sx] < *(int *)&dep) continue;

				//result = Cube color * Face shade

			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
			col = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);
#if 1
			col = mulcol(col,gimulcol);
#endif
			col = subcol(col,oct_sideshade[ivs[sid] + (sid<<1)]);
			if (dofog) col = lerp(col,oct_fogcol,min(cvttss2si(fz*fogmul),32767));

			cptr[sx] = col;
			zptr[sx] = dep;
		}
	}
#else
	__declspec(align(16)) static const int dq4095s[4] = {4095,4095,4095,0};
	__declspec(align(16)) static const int dqmul[4] = {0x00010001,0x00000001,0,0}, dqadd[4] = {256,0,0,0};
	static const char sidlut[8] = //(x>z)*4 + (z>y)*2 + (y>x)
	{
		0, //x<=z, z<=y, y<=x All equal:choose any.
		1, //x<=z, z<=y, y> x
		2, //x<=z, z> y, y<=x
		2, //x<=z, z> y, y> x
		0, //x> z, z<=y, y<=x
		1, //x> z, z<=y, y> x
		0, //x> z, z> y, y<=x
		0, //x> z, z> y, y> x Impossible!
	};
	static const char sidlut2[3][8] =
	{
		 0,16, 0,16, 0,16, 0,16, //sidlut2[ecx*8 + edx]
		32,32,48,48,32,32,48,48, //(((edx>>ecx)&1) + (ecx<<1))*16
		64,64,64,64,80,80,80,80,
	};
	__declspec(align(16)) float dqv0[4], dqtemp[4];

	dqv0[0] = v0.x; dqv0[1] = v0.y; dqv0[2] = v0.z; dqv0[3] = 1.f;

	i = (int)loct->sur.buf;

	sxs = sx1;
	while (1)
	{
		sx = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);

		_asm
		{
			push ebx
			push esi
			push edi
			mov ebx, i
			movd xmm3, esp
			mov eax, sx
			mov esi, zptr
			pxor xmm7, xmm7
			jmp short in2it

beg:        mov edi, rptr

					;v.x = sx*dqgrxmul[0] + dqv0[0];
					;v.y = sx*dqgrxmul[1] + dqv0[1];
					;v.z = sx*dqgrxmul[2] + dqv0[2];
				cvtsi2ss xmm6, eax
				shufps xmm6, xmm6, 0
				mulps xmm6, dqgrxmul
				addps xmm6, dqv0

					;x = (((int) rptr[sx]     )&4095);
					;y = (((int)(rptr[sx]>>12))&4095);
					;z = (((int)(rptr[sx]>>24))&4095);
				movlps xmm0, [edi+eax*8] ;xmm0:[???????? ???????? IiiiiiiZ zzYyyXxx]
				movaps xmm1, xmm0        ;xmm1:[???????? ???????? IiiiiiiZ zzYyyXxx]
				movaps xmm2, xmm0        ;xmm2:[???????? ???????? IiiiiiiZ zzYyyXxx]
				psllq xmm0, 20           ;xmm0:[???????? ???00000 iiZzzYyy Xxx00000]
				psrlq xmm1, 24           ;xmm1:[000000?? ???????? 000000Ii iiiiiZzz]
				movss xmm0, xmm2         ;xmm0:[???????? ???00000 iiZzzYyy zzYyyXxx]
				movlhps xmm0, xmm1       ;xmm0:[000000Ii iiiiiZzz iiZzzYyy zzYyyXxx]
				pand xmm0, dq4095s       ;xmm0:[00000000 00000Zzz 00000Yyy 00000Xxx]

					;ivs[0] = ((*(unsigned int *)&v.x)>>31);
					;ivs[1] = ((*(unsigned int *)&v.y)>>31);
					;ivs[2] = ((*(unsigned int *)&v.z)>>31);
				pxor xmm5, xmm5
				pcmpgtd xmm5, xmm6

					;f = ((float)(ivs[0]+x)-gorig.x)/v.x;             { fz = f; sid = 0; }
					;f = ((float)(ivs[1]+y)-gorig.y)/v.y; if (f > fz) { fz = f; sid = 1; }
					;f = ((float)(ivs[2]+z)-gorig.z)/v.z; if (f > fz) { fz = f; sid = 2; }
				psubd xmm0, xmm5
				cvtdq2ps xmm0, xmm0
				subps xmm0, dqgorig
				;divps xmm0, xmm6         ;xmm0:[dw dz dy dx]
				rcpps xmm1, xmm6         ;xmm0:[dw dz dy dx]
				mulps xmm0, xmm1

					;sid = argmax(xmm0[0],xmm0[1],xmm0[2])
				pshufd xmm1, xmm0, 0xc9  ;xmm1:[dw dx dz dy]
				pcmpgtd xmm1, xmm0       ;xmm1:[0 dx>dz dz>dy dy>dx]
				movmskps ecx, xmm1
				movzx ecx, sidlut[ecx]

					;fz = max(xmm0[0],xmm0[1],xmm0[2]) //NOTE:memory access much faster than register-only extraction
				movaps dqtemp, xmm0
				movss xmm5, dqtemp[ecx*4]

					;dep = fz*fsc; if (*(int *)&zptr[sx] < *(int *)&dep) continue;
				movss xmm2, xmm5
				mulss xmm2, fsc
				comiss xmm2, [esi+eax*4]
				jae short in2it

					;psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
				mov edi, [edi+eax*8+4]
				shr edi, 4
#if ((SURF_SIZ == 4) || (SURF_SIZ == 8))
				#define SURF_SIZMUL SURF_SIZ
#elif (SURF_SIZ == 16)
				#define SURF_SIZMUL 8
				add edi, edi
#elif (SURF_SIZ == 12)
				#define SURF_SIZMUL (SURF_SIZ/3)
				lea edi, [edi+edi*2]
#else
				#define SURF_SIZMUL 1
				imul edi, SURF_SIZ
#endif

					;col = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);
#if 1
				movsx esp, byte ptr [edi*SURF_SIZMUL+ebx+4]
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+5]
				add esp, edx
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+6]
				lea esp, [esp+edx+256]
				movd xmm0, [edi*SURF_SIZMUL+ebx]
				punpcklbw xmm0, xmm0
				movd xmm1, esp
				pshuflw xmm1, xmm1, 0
				pmulhuw xmm0, xmm1
#else
					;slower, but handles true dot w/normal (to be implemented later)
				movlps xmm0, [edi*SURF_SIZMUL+ebx] ;xmm0: [? ? ? ? ? ? ? ? ? z y x ? r g b]
				punpcklbw xmm0, xmm0        ;xmm0: [0 ? 0 z 0 y 0 x 0 ? 0 r 0 g 0 b]
				movhlps xmm1, xmm0          ;xmm1: [0 0 0 0 0 0 0 0 0 ? 0 z 0 y 0 x]
				psllw xmm1, 8               ;xmm1: [0 0 0 0 0 0 0 0 ? 0 z 0 y 0 x 0]
				psraw xmm1, 8               ;xmm1: [0 0 0 0 0 0 0 0 ? ? --z --y --x]
				pmaddwd xmm1, dqmul
				pshufd xmm5, xmm1, 1   <---WARNING! can't use xmm5 - used by fog!
				paddd xmm1, xmm5
				paddd xmm1, dqadd
				pshuflw xmm1, xmm1, 0
				pmulhuw xmm0, xmm1
#endif

					;col = subcol(col,oct_sideshade[ivs[sid] + (sid<<1)]);
				movmskps edx, xmm6
				movzx edx, byte ptr sidlut2[ecx*8 + edx] ;mov edx, (((edx>>ecx)&1) + (ecx<<1))*16
				psubusb xmm0, dqgsideshade[edx]
				;shr edx, cl
				;adc ecx, ecx
				;add ecx, ecx
				;psubusb xmm0, dqgsideshade[ecx*8]

					;col = mulcol(col,gimulcol);
				pmullw xmm0, dqimulcol
				psrlw xmm0, 6

					;if (dofog) col = lerp(col,oct_fogcol,min(cvttss2si(fz*fogmul),32767));
				cmp dofog, 0
				jz short skipfog
				mulss xmm5, fogmul
				cvttps2dq xmm5, xmm5
				packssdw xmm5, xmm5
				pshuflw xmm5, xmm5, 0
				movaps xmm1, dqfogcol
				psubw xmm1, xmm0
				paddw xmm1, xmm1
				pmulhw xmm1, xmm5
				paddw xmm0, xmm1
skipfog:
				packuswb xmm0, xmm0
				mov edx, cptr
				movd [edx+eax*4], xmm0 ;cptr[sx] = col;
				movd [esi+eax*4], xmm2 ;zptr[sx] = dep;

in2it:      sub eax, 1
				cmp eax, sxs
				jge short beg

			movd esp, xmm3
			pop edi
			pop esi
			pop ebx
		}
	}
#endif
}

static void cpu_shader_texmap (int sy, void *_)
{
	surf_t *psurf;
	point3d v0, v, p;
	int ivs[3];
	float f, fsc, fz, fu, fv, dep, *zptr, fwmul, fogmul;
	int i, j, k, m, x, y, z, iu, iv, im, sx, sxs, sx0, sx1, *cptr, sid, iwmsk, tilx5, ltilmsk, dofog;
	unsigned int *gcptr;
	__int64 *rptr;
	oct_t *loct = (oct_t *)_;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (__int64 *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	zptr = (float *)(sy*gdd.p + gddz);

	v.x = -(ghx-.5); v.y = sy-(ghy-.5); v.z = ghz;
	v0.x = v.x*grxmul.x + v.y*grymul.x + v.z*grzmul.x;
	v0.y = v.x*grxmul.y + v.y*grymul.y + v.z*grzmul.y;
	v0.z = v.x*grxmul.z + v.y*grymul.z + v.z*grzmul.z;
	fsc = gzscale;

	psurf = 0; gcptr = &gcbit[sy*gcbitplo5];

	ltilmsk = max(((tiles[0].ltilesid-1)<<LTEXPREC) - 1,0); //ltilesid-1 gives 2x2
	tilx5 = (tiles[0].x>>5); iwmsk = (tilx5<<LTEXPREC)-1; fwmul = (float)(iwmsk+1);

	if (oct_fogdist >= 1e32) dofog = 0; else { fogmul = ghz*32768.0/((double)loct->sid*oct_fogdist); dofog = 1; }

#if 0
	static const int clamplut[16] =
	{
		((64- 1)<<LTEXPREC)-1,
		((64- 2)<<LTEXPREC)-1,
		((64- 4)<<LTEXPREC)-1,
		((64- 8)<<LTEXPREC)-1,
		((64-16)<<LTEXPREC)-1,
		((64-32)<<LTEXPREC)-1,
		0,0,0,0,0,0,0,0,0,0,
	};

	sxs = sx1;
	while (1)
	{
		sx  = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
		for(sx--;sx>=sxs;sx--)
		{
			v.x = sx*grxmul.x + v0.x;
			v.y = sx*grxmul.y + v0.y;
			v.z = sx*grxmul.z + v0.z;

			x = (((int) rptr[sx]     )&4095);
			y = (((int)(rptr[sx]>>12))&4095);
			z = (((int)(rptr[sx]>>24))&4095);

			ivs[0] = ((*(unsigned int *)&v.x)>>31);
			ivs[1] = ((*(unsigned int *)&v.y)>>31);
			ivs[2] = ((*(unsigned int *)&v.z)>>31);
#if 0
			sid = 0;
			f = ((float)(ivs[0]+x)-gorig.x)/v.x;                      p.y = v.y*f+gorig.y; p.z = v.z*f+gorig.z; if ((cvttss2si(p.y) == y) && (cvttss2si(p.z) == z)) { fz = f; fu = p.z; fv = p.y; sid = 0; }
			f = ((float)(ivs[1]+y)-gorig.y)/v.y; p.x = v.x*f+gorig.x;                      p.z = v.z*f+gorig.z; if ((cvttss2si(p.x) == x) && (cvttss2si(p.z) == z)) { fz = f; fu = p.x; fv = p.z; sid = 1; }
			f = ((float)(ivs[2]+z)-gorig.z)/v.z; p.x = v.x*f+gorig.x; p.y = v.y*f+gorig.y;                      if ((cvttss2si(p.x) == x) && (cvttss2si(p.y) == y)) { fz = f; fu = p.x; fv = p.y; sid = 2; }
#else
			p.x = (float)(ivs[0]+x)-gorig.x;
			p.y = (float)(ivs[1]+y)-gorig.y;
			p.z = (float)(ivs[2]+z)-gorig.z;
			if (((p.x*v.y < p.y*v.x) == (ivs[1]^ivs[0])) && ((p.x*v.z < p.z*v.x) == (ivs[2]^ivs[0])))
				{ fz = p.x/v.x; fu = v.z*fz+gorig.z; fv = v.y*fz+gorig.y; sid = 0; }
			else if ((p.y*v.z < p.z*v.y) == (ivs[2]^ivs[1]))
				{ fz = p.y/v.y; fu = v.x*fz+gorig.x; fv = v.z*fz+gorig.z; sid = 1; }
			else
				{ fz = p.z/v.z; fu = v.x*fz+gorig.x; fv = v.y*fz+gorig.y; sid = 2; }
#endif
			dep = fz*fsc; if (*(int *)&zptr[sx] < *(int *)&dep) continue;

				//result = Cube color * Texture map * Face shade

			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
			//i = (rptr[sx]>>36); if (((unsigned)i >= (unsigned)loct->sur.mal) || (!(loct->sur.bit[i>>5]&(1<<i)))) { cptr[sx] = 0xff00ff; zptr[sx] = dep; continue; }
			i = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);

#if 1
				//shade
			im = cvttss2si(klog2up7(fz)+gmipoffs); im = min(max(im,0),ltilmsk); m = (im>>LTEXPREC);
			iu = cvttss2si(fu*fwmul)&iwmsk;
			iv = cvttss2si(fv*fwmul)&iwmsk;
			iu = min(iu,clamplut[m]); iv = min(iv,clamplut[m]); //clamp attempt..
			iu = (gtiloff[psurf->tex&15] + iu)>>m;
			iv = (gtiloff[psurf->tex>>4] + iv)>>m;

#if (DRAWOCT_CPU_FILT == 0) //nearest (GL_NEAREST_MIPMAP_NEAREST)
			j = (iv>>LTEXPREC)*tiles[m  ].p + (iu>>LTEXPREC)*4 + (INT_PTR)tiles[m  ].f; j = *(int *)j;
#elif (DRAWOCT_CPU_FILT == 1) //bilinear (GL_LINEAR_MIPMAP_NEAREST)
			j = (iv>>LTEXPREC)*tiles[m  ].p + (iu>>LTEXPREC)*4 + (INT_PTR)tiles[m  ].f; j = bilinfilt((int *)j,(int *)(j+tiles[m  ].p),iu<<(16-LTEXPREC),iv<<(16-LTEXPREC));
#else       //mipmap
			j = (iv>> LTEXPREC   )*tiles[m  ].p + (iu>> LTEXPREC   )*4 + (INT_PTR)tiles[m  ].f; j = bilinfilt((int *)j,(int *)(j+tiles[m  ].p),iu<<(16- LTEXPREC   ),iv<<(16- LTEXPREC   ));
			k = (iv>>(LTEXPREC+1))*tiles[m+1].p + (iu>>(LTEXPREC+1))*4 + (INT_PTR)tiles[m+1].f; k = bilinfilt((int *)k,(int *)(k+tiles[m+1].p),iu<<(16-(LTEXPREC+1)),iv<<(16-(LTEXPREC+1)));
			j = lerp(j,k,(im<<(16-LTEXPREC))&0xffff);
#endif

			i = lerp(i,j,gimixval); //i = mulcol(i,j);
			i = mulcol(i,gimulcol);
#endif
			i = subcol(i,oct_sideshade[ivs[sid] + (sid<<1)]);

			if (dofog) i = lerp(i,oct_fogcol,min(cvttss2si(fz*fogmul),32767));

			cptr[sx] = i;
			zptr[sx] = dep;
		}
	}
#else
	__declspec(align(16)) static const int dq4095s[4] = {4095,4095,4095,0};
	__declspec(align(16)) static const int dqmul[4] = {0x00010001,0x00000001,0,0}, dqadd[4] = {256,0,0,0};
	static const char sidlut_t4[8] = //((x>z)*4 + (z>y)*2 + (y>x))  *4 to eliminate an indexing inst
	{
		0, //x<=z, z<=y, y<=x All equal:choose any.
		4, //x<=z, z<=y, y> x
		8, //x<=z, z> y, y<=x
		8, //x<=z, z> y, y> x
		0, //x> z, z<=y, y<=x
		4, //x> z, z<=y, y> x
		0, //x> z, z> y, y<=x
		0, //x> z, z> y, y> x Impossible!
	};
	static const char sidlut2[3][8] =
	{
		 0,16, 0,16, 0,16, 0,16, //sidlut2[ecx*8 + edx]
		32,32,48,48,32,32,48,48, //(((edx>>ecx)&1) + (ecx<<1))*16
		64,64,64,64,80,80,80,80,
	};
	__declspec(align(16)) static const int dqklogsub[4] = {0x3f7a7dcf,0,0,0};
	__declspec(align(16)) static const float dqklogmul[4] = {8.262958294867817e-8 * 1.442695040888963 * 128.0,0.f,0.f,0.f};
	__declspec(align(16)) static const int dqclamplut[16][4] =
	{
		((64- 1)<<LTEXPREC)-1,((64- 1)<<LTEXPREC)-1,0,0,
		((64- 2)<<LTEXPREC)-1,((64- 2)<<LTEXPREC)-1,0,0,
		((64- 4)<<LTEXPREC)-1,((64- 4)<<LTEXPREC)-1,0,0,
		((64- 8)<<LTEXPREC)-1,((64- 8)<<LTEXPREC)-1,0,0,
		((64-16)<<LTEXPREC)-1,((64-16)<<LTEXPREC)-1,0,0,
		((64-32)<<LTEXPREC)-1,((64-32)<<LTEXPREC)-1,0,0,
		0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
		0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	};
	__declspec(align(16)) static const int dquvlut[3][4] = {0,-1,-1,0, -1,0,0,-1, -1,-1,0,0};
	__declspec(align(16)) float dqv0[4], dqtemp[4];

	dqv0[0] = v0.x; dqv0[1] = v0.y; dqv0[2] = v0.z; dqv0[3] = 1.f;

	i = (int)loct->sur.buf;

	sxs = sx1;
	while (1)
	{
		sx = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);

		_asm
		{
			push ebx
			push esi
			push edi
			mov ebx, i
			mov eax, sx
			mov esi, zptr
			pxor xmm7, xmm7
			jmp short in2it

beg:        mov edi, rptr

					;v.x = sx*dqgrxmul[0] + dqv0[0];
					;v.y = sx*dqgrxmul[1] + dqv0[1];
					;v.z = sx*dqgrxmul[2] + dqv0[2];
				cvtsi2ss xmm6, eax
				shufps xmm6, xmm6, 0
				mulps xmm6, dqgrxmul
				addps xmm6, dqv0

					;x = (((int) rptr[sx]     )&4095);
					;y = (((int)(rptr[sx]>>12))&4095);
					;z = (((int)(rptr[sx]>>24))&4095);
				movlps xmm0, [edi+eax*8] ;xmm0:[???????? ???????? IiiiiiiZ zzYyyXxx]
				movaps xmm1, xmm0        ;xmm1:[???????? ???????? IiiiiiiZ zzYyyXxx]
				movaps xmm2, xmm0        ;xmm2:[???????? ???????? IiiiiiiZ zzYyyXxx]
				psllq xmm0, 20           ;xmm0:[???????? ???00000 iiZzzYyy Xxx00000]
				psrlq xmm1, 24           ;xmm1:[000000?? ???????? 000000Ii iiiiiZzz]
				movss xmm0, xmm2         ;xmm0:[???????? ???00000 iiZzzYyy zzYyyXxx]
				movlhps xmm0, xmm1       ;xmm0:[000000Ii iiiiiZzz iiZzzYyy zzYyyXxx]
				pand xmm0, dq4095s       ;xmm0:[00000000 00000Zzz 00000Yyy 00000Xxx]

					;ivs[0] = ((*(unsigned int *)&v.x)>>31);
					;ivs[1] = ((*(unsigned int *)&v.y)>>31);
					;ivs[2] = ((*(unsigned int *)&v.z)>>31);
				pxor xmm5, xmm5
				pcmpgtd xmm5, xmm6

					;f = ((float)(ivs[0]+x)-gorig.x)/v.x;             { fz = f; sid = 0; }
					;f = ((float)(ivs[1]+y)-gorig.y)/v.y; if (f > fz) { fz = f; sid = 1; }
					;f = ((float)(ivs[2]+z)-gorig.z)/v.z; if (f > fz) { fz = f; sid = 2; }
				psubd xmm0, xmm5
				cvtdq2ps xmm0, xmm0
				subps xmm0, dqgorig
				;divps xmm0, xmm6         ;xmm0:[dw dz dy dx]
				rcpps xmm1, xmm6         ;xmm0:[dw dz dy dx]
				mulps xmm0, xmm1

					;sid = argmax(xmm0[0],xmm0[1],xmm0[2])
				pshufd xmm1, xmm0, 0xc9  ;xmm1:[dw dx dz dy]
				pcmpgtd xmm1, xmm0       ;xmm1:[0 dx>dz dz>dy dy>dx]
				movmskps ecx, xmm1
				movzx ecx, sidlut_t4[ecx]

					;fz = max(xmm0[0],xmm0[1],xmm0[2]) //NOTE:memory access much faster than register-only extraction
				movaps dqtemp, xmm0
				movss xmm5, dqtemp[ecx]

					;dep = fz*fsc; if (*(int *)&zptr[sx] < *(int *)&dep) continue;
				movss xmm2, xmm5
				mulss xmm2, fsc
				comiss xmm2, [esi+eax*4]
				jae short in2it

					;p.x = v.x*fz+gorig.x;
					;p.y = v.y*fz+gorig.y;
					;p.z = v.z*fz+gorig.z;
				pshufd xmm4, xmm5, 0
				mulps xmm4, xmm6
				addps xmm4, dqgorig
					;if (sid == 0) { fu = p.z; fv = p.y; }
					;if (sid == 1) { fu = p.x; fv = p.z; }
					;if (sid == 2) { fu = p.x; fv = p.y; }
				pshufhw xmm4, xmm4, 0x44 ;xmm4:[p.z p.z p.y p.x]
				pand xmm4, dquvlut[ecx*4]
				movhlps xmm3, xmm4
				por xmm4, xmm3

				movd xmm3, ecx ;backup sid

					;psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36];
				mov edi, [edi+eax*8+4]
				shr edi, 4
#if ((SURF_SIZ == 4) || (SURF_SIZ == 8))
				#define SURF_SIZMUL SURF_SIZ
#elif (SURF_SIZ == 16)
				#define SURF_SIZMUL 8
				add edi, edi
#elif (SURF_SIZ == 12)
				#define SURF_SIZMUL (SURF_SIZ/3)
				lea edi, [edi+edi*2]
#else
				#define SURF_SIZMUL 1
				imul edi, SURF_SIZ
#endif

					;col = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);
#if 1
				movsx ecx, byte ptr [edi*SURF_SIZMUL+ebx+4]
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+5]
				add ecx, edx
				movsx edx, byte ptr [edi*SURF_SIZMUL+ebx+6]
				lea ecx, [ecx+edx+256]
				movd xmm0, [edi*SURF_SIZMUL+ebx]
				punpcklbw xmm0, xmm0
				movd xmm1, ecx
				pshuflw xmm1, xmm1, 0
				pmulhuw xmm0, xmm1
#else
					;slower, but handles true dot w/normal (to be implemented later)
				movlps xmm0, [edi*SURF_SIZMUL+ebx] ;xmm0: [? ? ? ? ? ? ? ? ? z y x ? r g b]
				punpcklbw xmm0, xmm0        ;xmm0: [0 ? 0 z 0 y 0 x 0 ? 0 r 0 g 0 b]
				movhlps xmm1, xmm0          ;xmm1: [0 0 0 0 0 0 0 0 0 ? 0 z 0 y 0 x]
				psllw xmm1, 8               ;xmm1: [0 0 0 0 0 0 0 0 ? 0 z 0 y 0 x 0]
				psraw xmm1, 8               ;xmm1: [0 0 0 0 0 0 0 0 ? ? --z --y --x]
				pmaddwd xmm1, dqmul
				pshufd xmm5, xmm1, 1   <---WARNING! can't use xmm5 - used by tex&fog!
				paddd xmm1, xmm5
				paddd xmm1, dqadd
				pshuflw xmm1, xmm1, 0
				pmulhuw xmm0, xmm1
#endif

					;Register allocation at this point, [] for after this point
					;eax:sx                          xmm0:   -   -   - col
					;ecx:[im]                        xmm1:   -   -   -   -
					;edx:[m]                         xmm2:   -   -   - dep
					;ebx:loct->sur.buf (selfmod?)    xmm3:   -   -   - sid*4
					;esp:-                           xmm4:   -   -  fv  fu
					;ebp:-                           xmm5:   -   -   -  fz
					;esi:zptr (const per hlin)       xmm6:   - v.z v.y v.x
					;edi:loct->sur.buf index         xmm7:   0   0   0   0 (could be mem)


					;   //shade
					;im = cvttss2si(klog2up7(fz)+gmipoffs);
				movss xmm1, xmm5
				psubd xmm1, dqklogsub ;klog2up7()    //const int   dqklogsub[4] = {0x3f7a7dcf,0,0,0};
				cvtdq2ps xmm1, xmm1
				mulss xmm1, dqklogmul                //const float dqklogmul[4] = {8.262958294867817e-8 * 1.442695040888963 * 128.0,0.f,0.f,0.f};
				addss xmm1, gmipoffs
				cvttss2si ecx, xmm1
					;im = min(max(im,0),ltilmsk);
				cmp ecx, ltilmsk
				jbe skipmipclip
				not ecx
				shr ecx, 31
				and ecx, ltilmsk
skipmipclip:   ;m = (im>>LTEXPREC);
				mov edx, ecx
				shr edx, LTEXPREC
					;iu = cvttss2si(fu*fwmul)&iwmsk;
					;iv = cvttss2si(fv*fwmul)&iwmsk;
				mulps xmm4, dqfwmul
				cvttps2dq xmm4, xmm4
				pand xmm4, dqiwmsk
					;iu = min(iu,clamplut[m]); iv = min(iv,clamplut[m]); //clamp attempt..
				add edx, edx
				pminsw xmm4, dqclamplut[edx*8]
				shr edx, 1
					;iu = (gtiloff[psurf->tex&15] + iu)>>m;
					;iv = (gtiloff[psurf->tex>>4] + iv)>>m;
				movzx edi, byte ptr [edi*SURF_SIZMUL+ebx+7]
				add edi, edi
				paddd xmm4, dqtiloff[edi*8]
				movd xmm1, edx
				psrld xmm4, xmm1

						;nearest (GL_NEAREST_MIPMAP_NEAREST)
					;j = (iv>>LTEXPREC)*tiles[m  ].p + (iu>>LTEXPREC)*4 + (INT_PTR)tiles[m  ].f; j = *(int *)j;
				psrld xmm4, LTEXPREC
				packssdw xmm4, xmm4
				shl edx, 2
				pmaddwd xmm4, dqtilepitch4[edx*4]
				movd edi, xmm4
				mov edx, tilesf[edx]
				movd xmm1, [edi+edx]
				punpcklbw xmm1, xmm7

					;col = lerp(col,j,gimixval);
				psubw xmm1, xmm0
				paddw xmm1, xmm1
				pmulhw xmm1, dqmixval
				paddw xmm0, xmm1

					;col = mulcol(col,gimulcol);
				pmullw xmm0, dqimulcol
				psrlw xmm0, 6

					;col = subcol(col,oct_sideshade[ivs[sid] + (sid<<1)]);
				movmskps edx, xmm6
				movd ecx, xmm3 ;restore sid
				movzx edx, byte ptr sidlut2[ecx*2 + edx] ;mov edx, (((edx>>ecx)&1) + (ecx<<1))*16
				psubusb xmm0, dqgsideshade[edx]

					;if (dofog) col = lerp(col,oct_fogcol,min(cvttss2si(fz*fogmul),32767));
				cmp dofog, 0
				jz short skipfog
				mulss xmm5, fogmul
				cvttps2dq xmm5, xmm5
				packssdw xmm5, xmm5
				pshuflw xmm5, xmm5, 0
				movaps xmm1, dqfogcol
				psubw xmm1, xmm0
				paddw xmm1, xmm1
				pmulhw xmm1, xmm5
				paddw xmm0, xmm1
skipfog:
				packuswb xmm0, xmm0
				mov edx, cptr
				movss [edx+eax*4], xmm0 ;cptr[sx] = col;
				movss [esi+eax*4], xmm2 ;zptr[sx] = dep;

in2it:      sub eax, 1
				cmp eax, sxs
				jge short beg

			pop edi
			pop esi
			pop ebx
		}
	}

#endif
}

static void cpu_shader_texmap_mc (int sy, void *_)
{
#if (MARCHCUBE != 0)
	surf_t *psurf;
	point3d fp, p, v0, v, tri[15], vt[3];
	float f, g, fsc, tfz, tfu, tfv, fz, fu, fv, dep, *zptr, fwmul, fogmul;
	int i, j, k, c, m, x, y, z, iu, iv, im, sx, sxs, sx0, sx1, *cptr, iwmsk, tilx5, ltilmsk, dofog;
	unsigned int *gcptr;
	__int64 *rptr;
	oct_t *loct = (oct_t *)_;

	if ((sy < gymin) || (sy >= gymax)) return;
	sx0 = max(glmost[sy],0); sx1 = min(grmost[sy],gdd.x); if (sx0 >= sx1) return;

	rptr = (__int64 *)(sy*gxd.p + gxd.f);
	cptr = (int *)(sy*gdd.p + gdd.f);
	zptr = (float *)(sy*gdd.p + gddz);

	v.x = -(ghx-.5); v.y = sy-(ghy-.5); v.z = ghz;
	v0.x = v.x*grxmul.x + v.y*grymul.x + v.z*grzmul.x;
	v0.y = v.x*grxmul.y + v.y*grymul.y + v.z*grzmul.y;
	v0.z = v.x*grxmul.z + v.y*grymul.z + v.z*grzmul.z;
	fsc = gzscale;

	psurf = 0; gcptr = &gcbit[sy*gcbitplo5];

	ltilmsk = max(((tiles[0].ltilesid-1)<<LTEXPREC) - 1,0); //ltilesid-1 gives 2x2
	tilx5 = (tiles[0].x>>5); iwmsk = (tilx5<<LTEXPREC)-1; fwmul = (float)(iwmsk+1);

	if (oct_fogdist >= 1e32) dofog = 0; else { fogmul = ghz*32768.0/((double)loct->sid*oct_fogdist); dofog = 1; }

	static const int clamplut[16] =
	{
		((64- 1)<<LTEXPREC)-1,
		((64- 2)<<LTEXPREC)-1,
		((64- 4)<<LTEXPREC)-1,
		((64- 8)<<LTEXPREC)-1,
		((64-16)<<LTEXPREC)-1,
		((64-32)<<LTEXPREC)-1,
		0,0,0,0,0,0,0,0,0,0,
	};

	sxs = sx1;
	while (1)
	{
		sx  = uptil0(gcptr,sxs-1); if (sx <= sx0) return;
		sxs = uptil1(gcptr,sx -1); sxs = max(sxs,sx0);
		for(sx--;sx>=sxs;sx--)
		{
			v.x = sx*grxmul.x + v0.x;
			v.y = sx*grxmul.y + v0.y;
			v.z = sx*grxmul.z + v0.z;

			p.x = (float)(((int) rptr[sx]     )&4095) - gorig.x;
			p.y = (float)(((int)(rptr[sx]>>12))&4095) - gorig.y;
			p.z = (float)(((int)(rptr[sx]>>24))&4095) - gorig.z;
			psurf = &((surf_t *)loct->sur.buf)[rptr[sx]>>36]; fz = 1e32;
			for(c=marchcube_gen(psurf,tri)-3;c>=0;c-=3)
			{
				vt[0].x = tri[c].x+p.x; vt[1].x = tri[c+1].x-tri[c].x; vt[2].x = tri[c+2].x-tri[c].x;
				vt[0].y = tri[c].y+p.y; vt[1].y = tri[c+1].y-tri[c].y; vt[2].y = tri[c+2].y-tri[c].y;
				vt[0].z = tri[c].z+p.z; vt[1].z = tri[c+1].z-tri[c].z; vt[2].z = tri[c+2].z-tri[c].z;

					//if (v passes through vt[0],vt[1],vt[2]) { ..
					//
					//vt[1].x*u + vt[2].x*v + vt[0].x = ix = v.x*t
					//vt[1].y*u + vt[2].y*v + vt[0].y = iy = v.y*t
					//vt[1].z*u + vt[2].z*v + vt[0].z = iz = v.z*t
					//
					//(-v.x)*t + vt[1].x*u + vt[2].x*v = (-vt[0].x)
					//(-v.y)*t + vt[1].y*u + vt[2].y*v = (-vt[0].y)
					//(-v.z)*t + vt[1].z*u + vt[2].z*v = (-vt[0].z)
				fp.x = vt[1].y*vt[2].z - vt[1].z*vt[2].y;
				fp.y = vt[1].z*vt[2].x - vt[1].x*vt[2].z;
				fp.z = vt[1].x*vt[2].y - vt[1].y*vt[2].x;
				f = 1.0/(v.x*fp.x + v.y*fp.y + v.z*fp.z);
				tfz = (vt[0].x*fp.x + vt[0].y*fp.y + vt[0].z*fp.z)*f; if (*(int *)&tfz >= *(int *)&fz) continue;
				tfu = ((vt[0].z*vt[2].y - vt[0].y*vt[2].z)*v.x +
						 (vt[0].x*vt[2].z - vt[0].z*vt[2].x)*v.y +
						 (vt[0].y*vt[2].x - vt[0].x*vt[2].y)*v.z)*f; if (*(unsigned int *)&tfu > 0x3f800000) continue;
				tfv = ((vt[1].z*vt[0].y - vt[1].y*vt[0].z)*v.x +
						 (vt[1].x*vt[0].z - vt[1].z*vt[0].x)*v.y +
						 (vt[1].y*vt[0].x - vt[1].x*vt[0].y)*v.z)*f; if (*(unsigned int *)&tfv > 0x3f800000) continue;
				f = tfu+tfv; if (*(int *)&f < 0x3f800000) { fz = tfz; fu = tfu; fv = tfv; }
			}
			dep = fz*fsc; if (*(int *)&zptr[sx] < *(int *)&dep) continue;

				//result = Cube color * Texture map * Face shade

			//i = (rptr[sx]>>36); if (((unsigned)i >= (unsigned)loct->sur.mal) || (!(loct->sur.bit[i>>5]&(1<<i)))) { cptr[sx] = 0xff00ff; zptr[sx] = dep; continue; }
			i = mulsc(*(int *)&psurf->b,psurf->norm[0]+psurf->norm[1]+psurf->norm[2]+256);

#if 1
				//shade
			im = cvttss2si(klog2up7(fz)+gmipoffs); im = min(max(im,0),ltilmsk); m = (im>>LTEXPREC);
			iu = cvttss2si(fu*fwmul)&iwmsk;
			iv = cvttss2si(fv*fwmul)&iwmsk;
			iu = min(iu,clamplut[m]); iv = min(iv,clamplut[m]); //clamp attempt..
			iu = (gtiloff[psurf->tex&15] + iu)>>m;
			iv = (gtiloff[psurf->tex>>4] + iv)>>m;

#if (DRAWOCT_CPU_FILT == 0) //nearest (GL_NEAREST_MIPMAP_NEAREST)
			j = (iv>>LTEXPREC)*tiles[m  ].p + (iu>>LTEXPREC)*4 + (INT_PTR)tiles[m  ].f; j = *(int *)j;
#elif (DRAWOCT_CPU_FILT == 1) //bilinear (GL_LINEAR_MIPMAP_NEAREST)
			j = (iv>>LTEXPREC)*tiles[m  ].p + (iu>>LTEXPREC)*4 + (INT_PTR)tiles[m  ].f; j = bilinfilt((int *)j,(int *)(j+tiles[m  ].p),iu<<(16-LTEXPREC),iv<<(16-LTEXPREC));
#else       //mipmap
			j = (iv>> LTEXPREC   )*tiles[m  ].p + (iu>> LTEXPREC   )*4 + (INT_PTR)tiles[m  ].f; j = bilinfilt((int *)j,(int *)(j+tiles[m  ].p),iu<<(16- LTEXPREC   ),iv<<(16- LTEXPREC   ));
			k = (iv>>(LTEXPREC+1))*tiles[m+1].p + (iu>>(LTEXPREC+1))*4 + (INT_PTR)tiles[m+1].f; k = bilinfilt((int *)k,(int *)(k+tiles[m+1].p),iu<<(16-(LTEXPREC+1)),iv<<(16-(LTEXPREC+1)));
			j = lerp(j,k,(im<<(16-LTEXPREC))&0xffff);
#endif

			i = lerp(i,j,gimixval); //i = mulcol(i,j);
			i = mulcol(i,gimulcol);
#endif

			if (dofog) i = lerp(i,oct_fogcol,min(cvttss2si(fz*fogmul),32767));

			cptr[sx] = i;
			zptr[sx] = dep;
		}
	}
#endif
}

#endif

//--------------------------------------------------------------------------------------------------

void oct_drawoct (oct_t *loct, point3d *pp, point3d *pr, point3d *pd, point3d *pf, float mixval, int imulcol)
{
	point3d vt[8];
	float f, g, h, fx, fy, fz, mat[9], minz;
	unsigned int *ogcbit;
	int i, j, k, x, y, z, ls, split[2][4+4+1+512], splitn[2], rectn, vaneg[24], dorast;
	static int lmost[MAXYDIM], rmost[MAXYDIM], xmin, ymin, xmax, ymax;
	static const int ind[6][4] = {1,5,7,3, 4,0,2,6, 2,3,7,6, 4,5,1,0, 5,4,6,7, 0,1,3,2};

#if (GPUSEBO != 0)
	if ((oct_usegpu) && (loct->gsurf))
	{
		loct->gsurf = 0;
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		bo_end(loct->bufid,0,0,loct->gxsid,loct->gysid,GL_RGBA,GL_UNSIGNED_BYTE,0);
	}
#endif

	if (!((octv_t *)loct->nod.buf)[loct->head].chi) return;

	invert3x3(pr,pd,pf,mat);
	fx = gipos.x-pp->x; fy = gipos.y-pp->y; fz = gipos.z-pp->z;
	gorig.x = fx*mat[0] + fy*mat[1] + fz*mat[2];
	gorig.y = fx*mat[3] + fy*mat[4] + fz*mat[5];
	gorig.z = fx*mat[6] + fy*mat[7] + fz*mat[8];

		//FIXFIXFIX:ugly hack to avoid visual artifact :/
	#define HACKTHRESH 1e-4
	f = gorig.x-floor(gorig.x+.5); if (fabs(f) < HACKTHRESH) { if (f >= 0) gorig.x += HACKTHRESH-f; else gorig.x -= HACKTHRESH+f; }
	f = gorig.y-floor(gorig.y+.5); if (fabs(f) < HACKTHRESH) { if (f >= 0) gorig.y += HACKTHRESH-f; else gorig.y -= HACKTHRESH+f; }
	f = gorig.z-floor(gorig.z+.5); if (fabs(f) < HACKTHRESH) { if (f >= 0) gorig.z += HACKTHRESH-f; else gorig.z -= HACKTHRESH+f; }

	gxmul.x = girig.x*pr->x + girig.y*pr->y + girig.z*pr->z;
	gxmul.y = gidow.x*pr->x + gidow.y*pr->y + gidow.z*pr->z;
	gxmul.z = gifor.x*pr->x + gifor.y*pr->y + gifor.z*pr->z;
	gymul.x = girig.x*pd->x + girig.y*pd->y + girig.z*pd->z;
	gymul.y = gidow.x*pd->x + gidow.y*pd->y + gidow.z*pd->z;
	gymul.z = gifor.x*pd->x + gifor.y*pd->y + gifor.z*pd->z;
	gzmul.x = girig.x*pf->x + girig.y*pf->y + girig.z*pf->z;
	gzmul.y = gidow.x*pf->x + gidow.y*pf->y + gidow.z*pf->z;
	gzmul.z = gifor.x*pf->x + gifor.y*pf->y + gifor.z*pf->z;
	gnadd.x = -(gorig.x*gxmul.x + gorig.y*gymul.x + gorig.z*gzmul.x);
	gnadd.y = -(gorig.x*gxmul.y + gorig.y*gymul.y + gorig.z*gzmul.y);
	gnadd.z = -(gorig.x*gxmul.z + gorig.y*gymul.z + gorig.z*gzmul.z);

	f = 1.0/(float)loct->sid;
	grxmul.x = (girig.x*mat[0] + girig.y*mat[1] + girig.z*mat[2])*f;
	grymul.x = (gidow.x*mat[0] + gidow.y*mat[1] + gidow.z*mat[2])*f;
	grzmul.x = (gifor.x*mat[0] + gifor.y*mat[1] + gifor.z*mat[2])*f;
	grxmul.y = (girig.x*mat[3] + girig.y*mat[4] + girig.z*mat[5])*f;
	grymul.y = (gidow.x*mat[3] + gidow.y*mat[4] + gidow.z*mat[5])*f;
	grzmul.y = (gifor.x*mat[3] + gifor.y*mat[4] + gifor.z*mat[5])*f;
	grxmul.z = (girig.x*mat[6] + girig.y*mat[7] + girig.z*mat[8])*f;
	grymul.z = (gidow.x*mat[6] + gidow.y*mat[7] + gidow.z*mat[8])*f;
	grzmul.z = (gifor.x*mat[6] + gifor.y*mat[7] + gifor.z*mat[8])*f;

	f = 1.f; //> 1 here shrinks cubes: weird effect, but slower fps
	//{ double d; readklock(&d); f = 1.0/(sin(d*4.0)*.5+.5); } //Enable this line for crazy cube shrinking effect :P
	fx = ghx*f; fy = ghy*f; fz = ghz*f;
	fgxmul.x = gxmul.x*fz + gxmul.z*fx; fgxmul.y = gxmul.y*fz + gxmul.z*fy; fgxmul.z = gxmul.z*f;
	fgymul.x = gymul.x*fz + gymul.z*fx; fgymul.y = gymul.y*fz + gymul.z*fy; fgymul.z = gymul.z*f;
	fgzmul.x = gzmul.x*fz + gzmul.z*fx; fgzmul.y = gzmul.y*fz + gzmul.z*fy; fgzmul.z = gzmul.z*f;
	fgnadd.x = gnadd.x*fz + gnadd.z*fx; fgnadd.y = gnadd.y*fz + gnadd.z*fy; fgnadd.z = gnadd.z*f;

							 gposadd[  0][0].x =                        0; gposadd[  0][0].y =                        0; gposadd[  0][0].z =                        0;
							 gposadd[  1][0].x =                 fgxmul.x; gposadd[  1][0].y =                 fgxmul.y; gposadd[  1][0].z =                 fgxmul.z;
	for(j=0;j<2;j++) { gposadd[j+2][0].x = gposadd[j][0].x+fgymul.x; gposadd[j+2][0].y = gposadd[j][0].y+fgymul.y; gposadd[j+2][0].z = gposadd[j][0].z+fgymul.z; }
	for(j=0;j<4;j++) { gposadd[j+4][0].x = gposadd[j][0].x+fgzmul.x; gposadd[j+4][0].y = gposadd[j][0].y+fgzmul.y; gposadd[j+4][0].z = gposadd[j][0].z+fgzmul.z; }
	for(j=0;j<8;j++) gposadd[j][0].z2 = -gposadd[j][0].z;
	for(j=0;j<8;j++)
	{
#if 0
		for(i=1;i<loct->lsid;i++)
		{
			gposadd[j][i].z = gposadd[j][i-1].z *2;
			gposadd[j][i].z2= gposadd[j][i-1].z2*2;
			gposadd[j][i].x = gposadd[j][i-1].x *2;
			gposadd[j][i].y = gposadd[j][i-1].y *2;
		}
#else
		i = (int)&gposadd[j][           0];
		k = (int)&gposadd[j][loct->lsid-1];
		_asm
		{
			mov eax, i
			movaps xmm0, [eax]
beg:     add eax, 16
			addps xmm0, xmm0
			movaps [eax], xmm0
			cmp eax, k
			jb short beg
		}
#endif
	}

	glorig.x = (int)floor(gorig.x);
	glorig.y = (int)floor(gorig.y);
	glorig.z = (int)floor(gorig.z);
	for(ls=loct->lsid-1;ls>=0;ls--)
	{
		i = (1<<ls);
		gldirlut[ls].x = min(max(glorig.x - i,-32768),32767);
		gldirlut[ls].y = min(max(glorig.y - i,-32768),32767);
		gldirlut[ls].z = min(max(glorig.z - i,-32768),32767);
		gldirlut[ls].dum = 32767;
	}

	glcenadd.x = -min(max(glorig.x,-1),loct->sid)+32766;   //-( -1)+32766 = 32767
	glcenadd.y = -min(max(glorig.y,-1),loct->sid)+32766;   //-(256)+32766 = 32510
	glcenadd.z = -min(max(glorig.z,-1),loct->sid)+32766;
	glcenadd.dum = 32767;
	glcensub.x = 32765;
	glcensub.y = 32765;
	glcensub.z = 32765;
	glcensub.dum = 65535;

	f = 3.0*sqrt(pr->z*pr->z + pd->z*pd->z + pf->z*pf->z); g = f;
	fx = min(fgxmul.z,0) + min(fgymul.z,0) + min(fgzmul.z,0);
	fy = max(fgxmul.z,0) + max(fgymul.z,0) + max(fgzmul.z,0);
	h = oct_fogdist;
	for(i=0;i<loct->lsid;i++,fx+=fx,fy+=fy)
	{
		cornmin[i] = f - fx;
		cornmax[i] = g - fy;
		scanmax[i] = h - fx; //0x7f7fffff;
	}
	cornmax[0] = 1e32; //Prevent goto tochild when (!ls)

	if (oct_usegpu)
	{
		//FIX:worth it to read zbuf from GPU?
			//Clear only area that cube covers
								 vt[  0].x =         gnadd.x;           vt[  0].y =         gnadd.y;           vt[  0].z =         gnadd.z;
								 vt[  1].x = gnadd.x+gxmul.x*loct->sid; vt[  1].y = gnadd.y+gxmul.y*loct->sid; vt[  1].z = gnadd.z+gxmul.z*loct->sid;
		for(i=0;i<2;i++) { vt[i+2].x = vt[i].x+gymul.x*loct->sid; vt[i+2].y = vt[i].y+gymul.y*loct->sid; vt[i+2].z = vt[i].z+gymul.z*loct->sid; }
		for(i=0;i<4;i++) { vt[i+4].x = vt[i].x+gzmul.x*loct->sid; vt[i+4].y = vt[i].y+gzmul.y*loct->sid; vt[i+4].z = vt[i].z+gzmul.z*loct->sid; }

		dorast = 0;
		for(i=0;i<3;i++)
		{
			switch(i)
			{
				case 0: fx = gxmul.y*gymul.z - gxmul.z*gymul.y;
						  fy = gxmul.z*gymul.x - gxmul.x*gymul.z;
						  fz = gxmul.x*gymul.y - gxmul.y*gymul.x; break;
				case 1: fx = gymul.y*gzmul.z - gymul.z*gzmul.y;
						  fy = gymul.z*gzmul.x - gymul.x*gzmul.z;
						  fz = gymul.x*gzmul.y - gymul.y*gzmul.x; break;
				case 2: fx = gzmul.y*gxmul.z - gzmul.z*gxmul.y;
						  fy = gzmul.z*gxmul.x - gzmul.x*gxmul.z;
						  fz = gzmul.x*gxmul.y - gzmul.y*gxmul.x; break;
			}
			f = min(-(vt[0].x*fx + vt[0].y*fy + vt[0].z*fz),(vt[7].x*fx + vt[7].y*fy + vt[7].z*fz));
			if (f >= 0.0) continue;
			if (f*f <= (fx*fx + fy*fy + fz*fz)*SCISDIST*SCISDIST*16*16) continue; //SCISDIST*16:assumes not zoomed out a ridiculous amount
			dorast = 1; break;
		}
		if (dorast)
		{
			gymin = 0x7fffffff; gymax = 0x80000000;
			for(j=3-1;j>=0;j--)
			{
				i = ((int *)&glorig)[j]; if ((unsigned)i < (unsigned)loct->sid) continue;
				i = (((unsigned)i)>>31)+j*2;
				if (gymin >= gymax)
				{
					rastquad(vt,ind[i],glmost,grmost,&gxmin,&gymin,&gxmax,&gymax);
				}
				else
				{
					rastquad(vt,ind[i],lmost,rmost,&xmin,&ymin,&xmax,&ymax);
					if (ymin >= ymax) continue;

						//take union of rasts
					for(y=max(ymin,gymin),k=min(ymax,gymax);y<k;y++)
					{
						glmost[y] = min(glmost[y],lmost[y]);
						grmost[y] = max(grmost[y],rmost[y]);
					}
					while (ymin < gymin) { gymin--; glmost[gymin] = lmost[gymin]; grmost[gymin] = rmost[gymin];          }
					while (ymax > gymax) {          glmost[gymax] = lmost[gymax]; grmost[gymax] = rmost[gymax]; gymax++; }
					gxmin = min(gxmin,xmin); gxmax = max(gxmax,xmax);
				}
			}
			if (gymin >= gymax) { gymin = 0; gymax = 0; }
		}
		else
		{
			gxmin = 0; gymin = 0; gxmax = xres; gymax = yres;
			memset4(glmost,   0,yres*4);
			memset4(grmost,xres,yres*4);
		}
		setgcbit(glmost,grmost,gymin,gymax);
	}
	else
	{
		memset16((void *)gcbit,-1,gcbpl*yres); //Clear cover buffer

		gxmin = 0; gymin = 0; gxmax = xres; gymax = yres;
		memset4(glmost,   0,yres*4);
		memset4(grmost,xres,yres*4);
	}

	g = 65536.f*65536.f;
	if (fabs(gxmul.z)*g < ghz) { f = g; if (gxmul.z < 0) f = -g; } else f = ghz/gxmul.z;
	gvanx[0] = gxmul.x*f + ghx;
	gvany[0] = gxmul.y*f + ghy;
	if (fabs(gymul.z)*g < ghz) { f = g; if (gymul.z < 0) f = -g; } else f = ghz/gymul.z;
	gvanx[1] = gymul.x*f + ghx;
	gvany[1] = gymul.y*f + ghy;
	if (fabs(gzmul.z)*g < ghz) { f = g; if (gzmul.z < 0) f = -g; } else f = ghz/gzmul.z;
	gvanx[2] = gzmul.x*f + ghx;
	gvany[2] = gzmul.y*f + ghy;
	for(i=0;i<3;i++) { gvanxmh[i] = gvanx[i]-.5; gvanymh[i] = gvany[i]-.5; }

	for(i=0;i<4;i++)
	{
		fx = 0.f; fy = 0.f; fz = 0.f;
		if ((i  )&1) { fx += gxmul.x; fy += gxmul.y; fz += gxmul.z; }
		if ((i+1)&2) { fx += gymul.x; fy += gymul.y; fz += gymul.z; }
		if ((i  )&2) { fx += gzmul.x; fy += gzmul.y; fz += gzmul.z; }
		fx = fx*ghz + fz*ghx;
		fy = fy*ghz + fz*ghy;
		cornvadd[0][i] = gvanx[0]*fz - fx;
		cornvadd[1][i] = gvanx[1]*fz - fx;
		cornvadd[2][i] = gvanx[2]*fz - fx;
		cornvadd[3][i] = gvany[0]*fz - fy;
		cornvadd[4][i] = gvany[1]*fz - fy;
		cornvadd[5][i] = gvany[2]*fz - fy;
	}

	memset(vaneg,0,sizeof(vaneg));
	for(i=0;i<27;i++)
	{
		vause[i][0] = ((i != 14) && (i != 12)); //left/right
		vause[i][1] = ((i != 16) && (i != 10)); //up/down
		vause[i][2] = ((i != 22) && (i !=  4)); //front/back
		vause[i][3] = 0;

		cornvadd2[i][ 0] = cornvadd[3][vanlut[i][2]]*+vause[i][0];
		cornvadd2[i][ 1] = cornvadd[4][vanlut[i][0]]*+vause[i][1];
		cornvadd2[i][ 2] = cornvadd[5][vanlut[i][1]]*+vause[i][2];
		cornvadd2[i][ 3] = 0.f;
		cornvadd2[i][ 4] = cornvadd[0][vanlut[i][2]]*-vause[i][0];
		cornvadd2[i][ 5] = cornvadd[1][vanlut[i][0]]*-vause[i][1];
		cornvadd2[i][ 6] = cornvadd[2][vanlut[i][1]]*-vause[i][2];
		cornvadd2[i][ 7] = 0.f;
		cornvadd2[i][ 8] = cornvadd[3][vanlut[i][0]]*-vause[i][0];
		cornvadd2[i][ 9] = cornvadd[4][vanlut[i][1]]*-vause[i][1];
		cornvadd2[i][10] = cornvadd[5][vanlut[i][2]]*-vause[i][2];
		cornvadd2[i][11] = 0.f;
		cornvadd2[i][12] = cornvadd[0][vanlut[i][0]]*+vause[i][0];
		cornvadd2[i][13] = cornvadd[1][vanlut[i][1]]*+vause[i][1];
		cornvadd2[i][14] = cornvadd[2][vanlut[i][2]]*+vause[i][2];
		cornvadd2[i][15] = 0.f;

		vause[i][0] = -vause[i][0];
		vause[i][1] = -vause[i][1];
		vause[i][2] = -vause[i][2];
	}

//--------------------------------------------------------------------------------------------------
		//Collect horz&vert splits
	split[0][0] = gxmin; splitn[0] = 1;
	split[1][0] = gymin; splitn[1] = 1;

	if (gxmul.z != 0.f)
	{
		i = (int)(gxmul.x/gxmul.z*ghz + ghx); if ((i > gxmin) && (i < gxmax)) { split[0][splitn[0]] = i; splitn[0]++; }
		i = (int)(gxmul.y/gxmul.z*ghz + ghy); if ((i > gymin) && (i < gymax)) { split[1][splitn[1]] = i; splitn[1]++; }
	}
	if (gymul.z != 0.f)
	{
		i = (int)(gymul.x/gymul.z*ghz + ghx); if ((i > gxmin) && (i < gxmax)) { split[0][splitn[0]] = i; splitn[0]++; }
		i = (int)(gymul.y/gymul.z*ghz + ghy); if ((i > gymin) && (i < gymax)) { split[1][splitn[1]] = i; splitn[1]++; }
	}
	if (gzmul.z != 0.f)
	{
		i = (int)(gzmul.x/gzmul.z*ghz + ghx); if ((i > gxmin) && (i < gxmax)) { split[0][splitn[0]] = i; splitn[0]++; }
		i = (int)(gzmul.y/gzmul.z*ghz + ghy); if ((i > gymin) && (i < gymax)) { split[1][splitn[1]] = i; splitn[1]++; }
	}

	//------------------------------------------------
	if (oct_numcpu >= 2)
	{
		#define XSPLITS 0
		#define YSPLITS 16
		for(i=1;i<XSPLITS;i++) { j = (i*xres)/XSPLITS; if ((j > gxmin) && (j < gxmax)) { split[0][splitn[0]] = j; splitn[0]++; } }
		for(i=1;i<YSPLITS;i++) { j = (i*yres)/YSPLITS; if ((j > gymin) && (j < gymax)) { split[1][splitn[1]] = j; splitn[1]++; } }
	}
	//------------------------------------------------

	for(k=2-1;k>=0;k--)
	{
			//Sort x's (split[0][?]) or y's (split[1][?])
		for(i=2;i<splitn[k];i++)
			for(j=1;j<i;j++)
				if (split[k][i] < split[k][j]) { z = split[k][i]; split[k][i] = split[k][j]; split[k][j] = z; }

			//Eliminate duplicates
		for(i=1,j=1;i<splitn[k];i++) if (split[k][i] != split[k][j-1]) { split[k][j] = split[k][i]; j++; }
		splitn[k] = j;
	}
	split[0][splitn[0]] = gxmax;
	split[1][splitn[1]] = gymax;

//--------------------------------------------------------------------------------------------------

	rectn = 1;
	grect[0].x0 = 0; grect[0].y0 = 0; grect[0].x1 = gxmax; grect[0].y1 = gymax;
	for(x=0;x<splitn[0];x++)
		for(y=0;y<splitn[1];y++) //NOTE: Keep y as inner loop - reduces chances of multithread contention (and glitches with xor gcbit in inner loop)
		{
			grect[rectn].x0 = split[0][x]; grect[rectn].x1 = split[0][x+1];
			grect[rectn].y0 = split[1][y]; grect[rectn].y1 = split[1][y+1];
			rectn++;
		}

		//f = cone radius angle
	fx = max(ghx-0.f,(float)xres-ghx);
	fy = max(ghy-0.f,(float)yres-ghy);
	f = atan(sqrt(fx*fx + fy*fy)/ghz);
	g = 1.0/sqrt(gxmul.z*gxmul.z + gymul.z*gymul.z + gzmul.z*gzmul.z); fx = gxmul.z*g; fy = gymul.z*g; fz = gzmul.z*g;
	g = sqrt(3.0)*0.5; //sphere radius
	h = 0.5;
	for(i=0;i<loct->lsid;i++,g+=g,h+=h) isint_cone_sph_init(&giics[i],gorig.x-h,gorig.y-h,gorig.z-h,fx,fy,fz,f,g);

	if (oct_usegpu)
	{
		x = xres*(PIXBUFBYPP>>2);
#if (GPUSEBO != 0)
		gxd.f = (INT_PTR)bo_begin(gpixbufid,x*yres*PIXBUFBYPP); gxd.p = x*4; gxd.x = xres; gxd.y = yres;
#endif
	}

	if (gdrawclose) oct_rendclosecubes(loct,&glorig);
	if (gdrawclose < 2)
	{
#if (USEASM != 0)
		oct_ftob2_dorect(-1,loct);
#endif
		htrun(oct_ftob2_dorect,loct,1,rectn,oct_numcpu);
	}

	if (!oct_usegpu)
	{
		gimixval = min(max((int)(mixval*32768.0),0),32767); gimulcol = imulcol;
		gzscale = ghz/loct->sid;
		gmipoffs = (float)klog2up7((tiles[0].x>>5)/(sqrt(pr->x*pr->x + pd->y*pd->y + pf->z*pf->z)*loct->sid));
		static int otiles0x = 0;
		if (tiles[0].x != otiles0x)
		{
			otiles0x = tiles[0].x;
			for(i=0;i<16;i++) gtiloff[i] = i*((tiles[0].x>>4)<<LTEXPREC) + ((tiles[0].x>>6)<<LTEXPREC);
			for(i=0;i<256;i++) { dqtiloff[i][0] = gtiloff[i&15]; dqtiloff[i][1] = gtiloff[i>>4]; }
		}
		for(i=0;i<6;i++) { dqgsideshade[i][0] = (oct_sideshade[i]&0xff) + ((oct_sideshade[i]&0xff00)<<8); dqgsideshade[i][1] = ((oct_sideshade[i]&0xff0000)>>16); }

		if ((shaderfunc == cpu_shader_znotex) || (shaderfunc == cpu_shader_texmap) || (shaderfunc == cpu_shader_texmap_mc))
		{
			dqgrxmul[0] = grxmul.x; dqgrxmul[1] = grxmul.y; dqgrxmul[2] = grxmul.z; dqgrxmul[3] = 0.f;
			dqgorig[0] = gorig.x; dqgorig[1] = gorig.y; dqgorig[2] = gorig.z; dqgorig[3] = 0.f;
			dqfogcol[0] = (oct_fogcol&255); dqfogcol[1] = ((oct_fogcol>>8)&255); dqfogcol[2] = ((oct_fogcol>>16)&255);
			dqimulcol[0] = (gimulcol&255); dqimulcol[1] = ((gimulcol>>8)&255); dqimulcol[2] = ((gimulcol>>16)&255);
			if ((shaderfunc == cpu_shader_texmap) || (shaderfunc == cpu_shader_texmap_mc))
			{
				float fwmul;
				int iwmsk, tilx5, ltilmsk;

				ltilmsk = max(((tiles[0].ltilesid-1)<<LTEXPREC) - 1,0); //ltilesid-1 gives 2x2
				tilx5 = (tiles[0].x>>5); iwmsk = (tilx5<<LTEXPREC)-1; fwmul = (float)(iwmsk+1);

				dqfwmul[0] = fwmul; dqfwmul[1] = fwmul; dqfwmul[2] = 0.f; dqfwmul[3] = 0.f;
				dqiwmsk[0] = iwmsk; dqiwmsk[1] = iwmsk; dqiwmsk[2] = 0; dqiwmsk[3] = 0;

				for(i=0;i<16;i++) { dqtilepitch4[i][0] = 4; dqtilepitch4[i][1] = tiles[i].p; tilesf[i] = tiles[i].f; } //FIXFIXFIXFIX:reduce count/optimize w/cache!
				dqmixval[0] = gimixval; dqmixval[1] = gimixval; dqmixval[2] = gimixval; dqmixval[3] = gimixval;
			}
		}
		htrun(shaderfunc,loct,gymin,gymax,oct_numcpu);
	}
	else
	{
		htrun(clearbackpixes,loct,gymin,gymax,oct_numcpu);

			//memcpy xres*yres*PIXBUFBYPP bytes from CPU->GPU
		glBindTexture(GL_TEXTURE_2D,gpixtexid);
#if (GPUSEBO == 0)
		glTexSubImage2D(GL_TEXTURE_2D,0,0,gymin,x,gymax-gymin,GL_RGBA,GL_UNSIGNED_BYTE,(void *)(gymin*gxd.p + gxd.f));
#else
		bo_end(gpixbufid,0,gymin,x,gymax-gymin,GL_RGBA,GL_UNSIGNED_BYTE,gymin*gxd.p);
#endif

			//send vals to shader
			//v.x = sx*grxmul.x + sy*grymul.x + ((.5-ghx)*grxmul.x + (.5-ghy)*grymul.x + ghz*grzmul.x);
			//v.y = sx*grxmul.y + sy*grymul.y + ((.5-ghx)*grxmul.y + (.5-ghy)*grymul.y + ghz*grzmul.y);
			//v.z = sx*grxmul.z + sy*grymul.z + ((.5-ghx)*grxmul.z + (.5-ghy)*grymul.z + ghz*grzmul.z);
			//v = t.x*vmulx + t.y*vmuly + vadd; <- in shader
		if (oct_useglsl)
		{
			((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);

			kglUniform1f(kglGetUniformLoc("depthmul"),-gznear*gzfar/(gzfar-gznear)*(float)loct->sid);
			kglUniform1f(kglGetUniformLoc("depthadd"),gzfar/(gzfar-gznear));

			kglUniform4f(kglGetUniformLoc("pos"),gorig.x,gorig.y,gorig.z,0.f);
			f = 1.0/ghz;
			kglUniform4f(kglGetUniformLoc("vmulx"),grxmul.x*gpixxdim*f,
																grxmul.y*gpixxdim*f,
																grxmul.z*gpixxdim*f,0.f);
			kglUniform4f(kglGetUniformLoc("vmuly"),grymul.x*gpixydim*f,
																grymul.y*gpixydim*f,
																grymul.z*gpixydim*f,0.f);
			kglUniform4f(kglGetUniformLoc("vadd"),(-ghx*grxmul.x - ghy*grymul.x + ghz*grzmul.x)*f,
															  (-ghx*grxmul.y - ghy*grymul.y + ghz*grzmul.y)*f,
															  (-ghx*grxmul.z - ghy*grymul.z + ghz*grzmul.z)*f,0.f);
			kglUniform1f(kglGetUniformLoc("mipoffs"),-ghz/ghx + (float)tiles[0].ltilesid - loct->lsid*2 - log(pr->z*pr->z + pd->z*pd->z + pf->z*pf->z)/log(2.f) - 9.f); //-=sharp, +=blurry
			kglUniform1f(kglGetUniformLoc("maxmip"),(float)tiles[0].ltilesid);
			kglUniform1f(kglGetUniformLoc("mixval"),mixval);
			kglUniform4f(kglGetUniformLoc("mulcol"),(float)((imulcol>>16)&255)/64.0,(float)((imulcol>>8)&255)/64.0,(float)(imulcol&255)/64.0,(float)(((unsigned)imulcol)>>24)/256.0);
			kglUniform1i(kglGetUniformLoc("usemix"),gusemix);
			kglUniform4f(kglGetUniformLoc("fogcol"),(float)((oct_fogcol>>16)&255)/255.0,(float)((oct_fogcol>>8)&255)/255.0,(float)(oct_fogcol&255)/255.0,(float)(((unsigned)oct_fogcol)>>24)/255.0);
			kglUniform1f(kglGetUniformLoc("fogdist"),1.0/(oct_fogdist*(double)loct->sid));

			kglUniform1f(kglGetUniformLoc("rxsid"),1.0/(float)loct->gxsid);
			kglUniform1f(kglGetUniformLoc("rysid"),1.0/(float)loct->gysid);
			kglUniform1i(kglGetUniformLoc("axsidm1"),loct->gxsid-1);
			kglUniform1i(kglGetUniformLoc("lxsidm1"),loct->glxsid-1);

			for(i=0;i<6;i++) kglUniform1f(kglGetUniformLoc("gsideshade")+i,(float)(oct_sideshade[i]&255)/255.f);
		}
		else
		{
			//kglProgramLocalParam(15?,drawconedat.h.x/drawconedat.h.z,0,0,0); //zoom
			shadcur = 0;
			glEnable(GL_VERTEX_PROGRAM_ARB);
			glEnable(GL_FRAGMENT_PROGRAM_ARB);
			((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shadvert[shadcur]);
			((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shadcur]);

			kglProgramLocalParam( 0,1.0/((float)loct->gxsid),2.0/((float)loct->gxsid),0.0,0.0); //sidmul0
			kglProgramLocalParam( 1,2.0,1.0/((float)loct->gysid),0.0,0.0); //sidmul1
			kglProgramLocalParam( 2,0.5*(float)loct->gxsid,0.5*(float)loct->gysid,0.0,0.0); //sidadd1
			kglProgramLocalParam( 3,-gznear*gzfar/(gzfar-gznear)*(float)loct->sid,gzfar/(gzfar-gznear),0.0,0.0); //depth

			f = 1.0/ghz;
			kglProgramLocalParam( 4,grxmul.x*gpixxdim*f,grxmul.y*gpixxdim*f,grxmul.z*gpixxdim*f,0.f); //vmulx
			kglProgramLocalParam( 5,grymul.x*gpixydim*f,grymul.y*gpixydim*f,grymul.z*gpixydim*f,0.f); //vmuly
			kglProgramLocalParam( 6,(-ghx*grxmul.x - ghy*grymul.x + ghz*grzmul.x)*f,
											(-ghx*grxmul.y - ghy*grymul.y + ghz*grzmul.y)*f,
											(-ghx*grxmul.z - ghy*grymul.z + ghz*grzmul.z)*f,0.f); //vadd
			kglProgramLocalParam( 7,gorig.x,gorig.y,gorig.z,0.f); //pos

			kglProgramLocalParam( 8,(float)gusemix,1.0/(oct_fogdist*(double)loct->sid),mixval,0.0); //mixnfog
			kglProgramLocalParam( 9,(float)((imulcol>>16)&255)/64.0,(float)((imulcol>>8)&255)/64.0,(float)(imulcol&255)/64.0,(float)(((unsigned)imulcol)>>24)/256.0);    //mulcol
			kglProgramLocalParam(10,(float)((oct_fogcol>>16)&255)/255.0,(float)((oct_fogcol>>8)&255)/255.0,(float)(oct_fogcol&255)/255.0,(float)(((unsigned)oct_fogcol)>>24)/255.0); //fogcol

			kglProgramLocalParam(11,(float)(oct_sideshade[0]&255)/255.f,(float)(oct_sideshade[2]&255)/255.f,(float)(oct_sideshade[4]&255)/255.f,0.0); //gsideshade024
			kglProgramLocalParam(12,(float)(oct_sideshade[1]&255)/255.f,(float)(oct_sideshade[3]&255)/255.f,(float)(oct_sideshade[5]&255)/255.f,0.0); //gsideshade135

			kglProgramLocalParam(13,-ghz/ghx + (float)tiles[0].ltilesid - loct->lsid*2 - log(pr->z*pr->z + pd->z*pd->z + pf->z*pf->z)/log(2.f) - 9.f,
				(float)tiles[0].ltilesid,0.0,0.0); //mipdat (mipoffs,maxmip,0,0)
		}
		kglActiveTexture(0); glBindTexture(GL_TEXTURE_2D,loct->tilid);
		kglActiveTexture(1); glBindTexture(GL_TEXTURE_2D,gpixtexid   );
		kglActiveTexture(2); glBindTexture(GL_TEXTURE_2D,loct->octid);

			//render fullscreen quad using shader
		fx = (float)xres/gpixxdim; fy = (float)yres/gpixydim; g = 256.f; f = (float)yres/(float)xres*g;
		if (dorast)
		{
			for(j=3-1;j>=0;j--)
			{
				point3d vt2[8];
				float ff, gg, hh, fsx, fsy;
				int n2, m;
				#define SCISDIST 5e-4
	
				i = ((int *)&glorig)[j]; if ((unsigned)i < (unsigned)loct->sid) continue;
				i = (((unsigned)i)>>31)+j*2;
	
				for(k=4-1,m=0,n2=0;m<4;k=m,m++)
				{
					if (vt[ind[i][k]].z >= SCISDIST) { vt2[n2] = vt[ind[i][k]]; n2++; }
					if ((vt[ind[i][k]].z >= SCISDIST) != (vt[ind[i][m]].z >= SCISDIST))
					{
						ff = (SCISDIST-vt[ind[i][m]].z)/(vt[ind[i][k]].z-vt[ind[i][m]].z);
						vt2[n2].x = (vt[ind[i][k]].x-vt[ind[i][m]].x)*ff + vt[ind[i][m]].x;
						vt2[n2].y = (vt[ind[i][k]].y-vt[ind[i][m]].y)*ff + vt[ind[i][m]].y;
						vt2[n2].z = SCISDIST; n2++;
					}
				}
				if (n2 < 3) continue;
	
				gg = ghz/ghx; hh = (float)xres/(float)yres;
				glBegin(GL_TRIANGLE_FAN);
				for(i=0;i<n2;i++)
				{
					ff = gg/vt2[i].z;
					fsx = vt2[i].x*ff;
					fsy = vt2[i].y*ff*hh;
					glTexCoord2f((fsx*.5+.5)*fx,(fsy*.5+.5)*fy); glVertex3f(g*fsx,-f*fsy,-g);
				}
				glEnd();
			}
		}
		else
		{
			glBegin(GL_QUADS);
			glTexCoord2f( 0, 0); glVertex3f(-g,+f,-g);
			glTexCoord2f(fx, 0); glVertex3f(+g,+f,-g);
			glTexCoord2f(fx,fy); glVertex3f(+g,-f,-g);
			glTexCoord2f( 0,fy); glVertex3f(-g,-f,-g);
			glEnd();
		}
	}

#if 0
		//Debug only: show split lines
	static int bozocnt = 0; bozocnt++;
	for(i=0;i<splitn[1];i++) { for(x=(bozocnt&7);x<gdd.x;x+=8) *(int *)(gdd.p*split[1][i] + (x<<2) + gdd.f) = 0xc0a080; }
	for(i=0;i<splitn[0];i++) { for(y=(bozocnt&7);y<gdd.y;y+=8) *(int *)(gdd.p*y + (split[0][i]<<2) + gdd.f) = 0xc0a080; }
#endif
}
void oct_drawoct (oct_t *loct, dpoint3d *dp, dpoint3d *dr, dpoint3d *dd, dpoint3d *df, float mixval, int imulcol)
{
	point3d fp, fr, fd, ff;
	fp.x = dp->x; fp.y = dp->y; fp.z = dp->z;
	fr.x = dr->x; fr.y = dr->y; fr.z = dr->z;
	fd.x = dd->x; fd.y = dd->y; fd.z = dd->z;
	ff.x = df->x; ff.y = df->y; ff.z = df->z;
	oct_drawoct(loct,&fp,&fr,&fd,&ff,mixval,imulcol);
}

//--------------------------------------------------------------------------------------------------

	//Test intersection between infinite ray and box; returns 1 if intersect else 0
	//See INBOX.KC for test program
static int inbox3d (dpoint3d *p, dpoint3d *d, double bx, double by, double bz, double bs)
{
	double hs, x, y, z;
		//x = d->x*t + p->x;
		//y = d->y*t + p->y;
		//z = d->z*t + p->z;
	hs = bs*.5; bx += hs-p->x; by += hs-p->y; bz += hs-p->z;

	if (fabs(d->x) > 0.0)
	{
		x = fabs(d->x)*hs; y = d->x*by; z = d->x*bz;
		if (max(fabs((bx-hs)*d->y - y),fabs((bx-hs)*d->z - z)) <= x) return(1);
		if (max(fabs((bx+hs)*d->y - y),fabs((bx+hs)*d->z - z)) <= x) return(1);
	}

	if (fabs(d->y) > 0.0)
	{
		y = fabs(d->y)*hs; x = d->y*bx; z = d->y*bz;
		if (max(fabs((by-hs)*d->x - x),fabs((by-hs)*d->z - z)) <= y) return(1);
		if (max(fabs((by+hs)*d->x - x),fabs((by+hs)*d->z - z)) <= y) return(1);
	}

	if (fabs(d->z) > 0.0)
	{
		z = fabs(d->z)*hs; x = d->z*bx; y = d->z*by;
		if (max(fabs((bz-hs)*d->x - x),fabs((bz-hs)*d->y - y)) <= z) return(1);
		if (max(fabs((bz+hs)*d->x - x),fabs((bz+hs)*d->y - y)) <= z) return(1);
	}

	return(0);
}

int oct_hitscan (oct_t *loct, dpoint3d *p, dpoint3d *padd, ipoint3d *hit, int *rhitdir, double *fracwent)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	double d, f, fx, fy, fz;
	int i, j, k, ls, x, y, z, nx, ny, nz, ix, iy, iz, ox, oy, oz, ox2, oy2, oz2, ord;

	if (fabs(padd->x) < 1e-8) { fx = 0.0; ix = 0; } else { fx = padd->x; ix = (fx>0)*2-1; }
	if (fabs(padd->y) < 1e-8) { fy = 0.0; iy = 0; } else { fy = padd->y; iy = (fy>0)*2-1; }
	if (fabs(padd->z) < 1e-8) { fz = 0.0; iz = 0; } else { fz = padd->z; iz = (fz>0)*2-1; }

	ox = (int)floor(p->x); ox2 = (int)floor(p->x+fx);
	oy = (int)floor(p->y); oy2 = (int)floor(p->y+fy);
	oz = (int)floor(p->z); oz2 = (int)floor(p->z+fz);
	ord = (fz >= 0.0)*4 + (fy >= 0.0)*2 + (fx >= 0.0);

	ls = loct->lsid-1; ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		k = j^ord;

		i = (1<<k); if (!(ptr->chi&i)) goto tosibly;

		nx = (( k    &1)<<ls)+x;
		ny = (((k>>1)&1)<<ls)+y;
		nz = (((k>>2)&1)<<ls)+z;

		if (!inbox3d(p,padd,nx,ny,nz,1<<ls)) goto tosibly;
			  if (ix > 0) { if ((nx+(1<<ls) <= ox) || (nx         >  ox2)) goto tosibly; }
		else if (ix < 0) { if ((nx         >  ox) || (nx+(1<<ls) <= ox2)) goto tosibly; }
			  if (iy > 0) { if ((ny+(1<<ls) <= oy) || (ny         >  oy2)) goto tosibly; }
		else if (iy < 0) { if ((ny         >  oy) || (ny+(1<<ls) <= oy2)) goto tosibly; }
			  if (iz > 0) { if ((nz+(1<<ls) <= oz) || (nz         >  oz2)) goto tosibly; }
		else if (iz < 0) { if ((nz         >  oz) || (nz+(1<<ls) <= oz2)) goto tosibly; }

		if (ls <= 0)
		{
			if (hit) { hit->x = nx; hit->y = ny; hit->z = nz; }
			if ((rhitdir) || (fracwent))
			{
#if (MARCHCUBE == 0)
				d = 0.0;
				if (rhitdir) (*rhitdir) = 0;
				if (fx != 0.0) { f = (nx+(fx<0.0)-p->x)/fx; if (f > d) { d = f; if (rhitdir) (*rhitdir) = (fx<0.0)+0; } }
				if (fy != 0.0) { f = (ny+(fy<0.0)-p->y)/fy; if (f > d) { d = f; if (rhitdir) (*rhitdir) = (fy<0.0)+2; } }
				if (fz != 0.0) { f = (nz+(fz<0.0)-p->z)/fz; if (f > d) { d = f; if (rhitdir) (*rhitdir) = (fz<0.0)+4; } }
				if (fracwent) (*fracwent) = d;
#else
				dpoint3d dnorm;
				double d, du, dv;
				d = marchcube_hitscan(p,padd,nx,ny,nz,&((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind],&dnorm,&du,&dv);
				if (d >= 1e32) goto tosibly;
				if (fracwent) (*fracwent) = d;
#endif
			}
			return(popcount[ptr->chi&(i-1)] + ptr->ind);
		}

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
		continue;

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; if (ls >= loct->lsid) { if (fracwent) (*fracwent) = 1.0; return(-1); } j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}
}
int oct_hitscan (oct_t *loct, point3d *fp, point3d *fpadd, ipoint3d *hit, int *rhitdir, float *fracwent)
{
	dpoint3d dp, dpadd;
	double dfracwent;
	int hitind;

	dp.x = fp->x; dp.y = fp->y; dp.z = fp->z;
	dpadd.x = fpadd->x; dpadd.y = fpadd->y; dpadd.z = fpadd->z;
	hitind = oct_hitscan(loct,&dp,&dpadd,hit,rhitdir,&dfracwent);
	(*fracwent) = dfracwent;
	return(hitind);
}

void oct_paint (oct_t *loct, brush_t *brush)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, ls, s, x, y, z, nx, ny, nz, ind;

	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
#else
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
#endif
	}

	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head];
	x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		i = (1<<j); if (!(ptr->chi&i)) goto tosibly;

		nx = (( j    &1)<<ls)+x;
		ny = (((j>>1)&1)<<ls)+y;
		nz = (((j>>2)&1)<<ls)+z;
		if (!brush->isins(brush,nx,ny,nz,ls)) goto tosibly;

		if (ls > 0)
		{
			stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; s >>= 1; //2child
			ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind];
			x = nx; y = ny; z = nz; j = 8-1; continue;
		}

		ind = popcount[ptr->chi&(i-1)] + ptr->ind;

		brush->getsurf(brush,nx,ny,nz,&((surf_t *)loct->sur.buf)[ind]);
		if (oct_usegpu)
		{
#if (GPUSEBO == 0)
			glTexSubImage2D(GL_TEXTURE_2D,0,(ind&((loct->gxsid>>1)-1))<<1,ind>>(loct->glysid-1),2,1,GL_RGBA,GL_UNSIGNED_BYTE,(void *)&((surf_t *)loct->sur.buf)[ind]);
#else
			memcpy(&loct->gsurf[ind],&((surf_t *)loct->sur.buf)[ind],loct->sur.siz);
#endif
		}

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) goto break2; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}

break2:;
}

//--------------------------------------------------------------------------------------------------
	//06/06/2011:Ported from CYL_INTERSECT.KC (see .KC for derivation)
typedef struct
{
	double x0, y0, z0, dx, dy, dz, rad, rad2, rdx, rdy, rdz;
	double dyz2, dxz2, dxy2, dxyz2, rdyz2, rdxz2, rdxy2, rdxyz2;
	int sdx, sdy, sdz, dumalign;
} int_cube_cyl_3d_t;
static void int_cube_cyl_3d_init (int_cube_cyl_3d_t *ic, double x0, double y0, double z0, double x1, double y1, double z1, double rad)
{
	double dx2, dy2, dz2;
	ic->x0 = x0; ic->dx = x1-x0; if (ic->dx) ic->rdx = 1.0/ic->dx;
	ic->y0 = y0; ic->dy = y1-y0; if (ic->dy) ic->rdy = 1.0/ic->dy;
	ic->z0 = z0; ic->dz = z1-z0; if (ic->dz) ic->rdz = 1.0/ic->dz;
	if (ic->dx > 0.0) ic->sdx = 1; else if (ic->dx < 0.0) ic->sdx = -1; else ic->sdx = 0;
	if (ic->dy > 0.0) ic->sdy = 1; else if (ic->dy < 0.0) ic->sdy = -1; else ic->sdy = 0;
	if (ic->dz > 0.0) ic->sdz = 1; else if (ic->dz < 0.0) ic->sdz = -1; else ic->sdz = 0;
	dx2 = ic->dx*ic->dx; dy2 = ic->dy*ic->dy; dz2 = ic->dz*ic->dz;
	ic->dyz2 = dy2 + dz2; if (ic->dyz2) ic->rdyz2 = 1.0/ic->dyz2;
	ic->dxz2 = dx2 + dz2; if (ic->dxz2) ic->rdxz2 = 1.0/ic->dxz2;
	ic->dxy2 = dx2 + dy2; if (ic->dxy2) ic->rdxy2 = 1.0/ic->dxy2;
	ic->dxyz2 = dx2 + ic->dyz2; if (ic->dxyz2) ic->rdxyz2 = 1.0/ic->dxyz2;
	ic->rad = rad; ic->rad2 = rad*rad;
}
	//returns t, where:
	//   goalx = ic->dx*t + ic->x0
	//   goaly = ic->dy*t + ic->y0
	//   goalz = ic->dz*t + ic->z0
	//and hitx/y/z is nearest point on cube (bx,by,bz,bs) to goalx/y/z
static double int_cube_cyl_3d_gett (int_cube_cyl_3d_t *ic, double bx, double by, double bz, double bs, double tmin)
{
	double f, t, xx, yy, zz, Zb, Zc, insqr, hbs;
	int x, y, z, u, v;

	hbs = bs*.5;
	bx += hbs-ic->x0;
	by += hbs-ic->y0;
	bz += hbs-ic->z0;

		//early exit bounding box
	f = hbs+ic->rad;
	t = ic->dx*tmin; if ((bx > max(t,0.0)+f) || (bx < min(t,0.0)-f)) return(tmin);
	t = ic->dy*tmin; if ((by > max(t,0.0)+f) || (by < min(t,0.0)-f)) return(tmin);
	t = ic->dz*tmin; if ((bz > max(t,0.0)+f) || (bz < min(t,0.0)-f)) return(tmin);

		//tests rounded cube at start in case it already intersects..
	xx = fabs(bx); xx -= min(xx,hbs);
	yy = fabs(by); yy -= min(yy,hbs);
	zz = fabs(bz); zz -= min(zz,hbs);
	if (xx*xx + yy*yy + zz*zz < ic->rad2) return(0.0);

		//test 3/6 faces
	f = hbs+ic->rad;
	if (ic->sdx) { t = (bx-(double)ic->sdx*f)*ic->rdx; if ((t >= 0.0) && (t < tmin) && (max(fabs(ic->dy*t-by),fabs(ic->dz*t-bz)) < hbs)) tmin = t; }
	if (ic->sdy) { t = (by-(double)ic->sdy*f)*ic->rdy; if ((t >= 0.0) && (t < tmin) && (max(fabs(ic->dx*t-bx),fabs(ic->dz*t-bz)) < hbs)) tmin = t; }
	if (ic->sdz) { t = (bz-(double)ic->sdz*f)*ic->rdz; if ((t >= 0.0) && (t < tmin) && (max(fabs(ic->dx*t-bx),fabs(ic->dy*t-by)) < hbs)) tmin = t; }

		//test 7/8 verts
	if (ic->dxyz2)
		for(x=-1;x<=1;x+=2)
			for(y=-1;y<=1;y+=2)
				for(z=-1;z<=1;z+=2)
				{
					if ((x == ic->sdx) && (y == ic->sdy) && (z == ic->sdz)) continue;
					xx = (double)x*hbs+bx; yy = (double)y*hbs+by; zz = (double)z*hbs+bz; //(xx - ic->dx*t)^2+ "y + "z = ic->rad2)
					Zb = xx*ic->dx + yy*ic->dy + zz*ic->dz;
					Zc = xx*xx + yy*yy + zz*zz - ic->rad2;
					insqr = Zb*Zb - ic->dxyz2*Zc; if (insqr < 0.0) continue;
					t = (Zb-sqrt(insqr))*ic->rdxyz2; if ((t >= 0.0) && (t < tmin)) tmin = t;
				}

		//test 9/12 edges
	for(v=-1;v<=1;v+=2)
		for(u=-1;u<=1;u+=2)
		{
			if ((ic->dyz2) && ((u != ic->sdy) || (v != ic->sdz)))
			{
				yy = hbs*(double)u+by; zz = hbs*(double)v+bz; //(yy - ic->dy*t)^2 + "z = rad2
				Zb = yy*ic->dy + zz*ic->dz;
				Zc = yy*yy + zz*zz - ic->rad2;
				insqr = Zb*Zb - ic->dyz2*Zc;
				if (insqr >= 0.0) { t = (Zb-sqrt(insqr))*ic->rdyz2; if ((t >= 0.0) && (t < tmin) && (fabs(ic->dx*t-bx) < hbs)) tmin = t; }
			}
			if ((ic->dxz2) && ((u != ic->sdx) || (v != ic->sdz)))
			{
				xx = hbs*(double)u+bx; zz = hbs*(double)v+bz; //(xx - ic->dx*t)^2 + "z = rad2
				Zb = xx*ic->dx + zz*ic->dz;
				Zc = xx*xx + zz*zz - ic->rad2;
				insqr = Zb*Zb - ic->dxz2*Zc;
				if (insqr >= 0.0) { t = (Zb-sqrt(insqr))*ic->rdxz2; if ((t >= 0.0) && (t < tmin) && (fabs(ic->dy*t-by) < hbs)) tmin = t; }
			}
			if ((ic->dxy2) && ((u != ic->sdx) || (v != ic->sdy)))
			{
				xx = hbs*(double)u+bx; yy = hbs*(double)v+by; //(xx - ic->dx*t)^2 + "y = rad2
				Zb = xx*ic->dx + yy*ic->dy;
				Zc = xx*xx + yy*yy - ic->rad2;
				insqr = Zb*Zb - ic->dxy2*Zc;
				if (insqr >= 0.0) { t = (Zb-sqrt(insqr))*ic->rdxy2; if ((t >= 0.0) && (t < tmin) && (fabs(ic->dz*t-bz) < hbs)) tmin = t; }
			}
		}

	return(tmin);
}

static double int_tri_cyl_3d_gett (const dpoint3d *c0, double cr, const dpoint3d *cv, const dpoint3d *tri, double tmin, dpoint3d *hit)
{
	dpoint3d ntri[3];
	double f, t, u, v, cr2, ux, uy, uz, vx, vy, vz, k0, k1, k2, k3, k4, k5, k6, k7;
	double k8, k9, ka, kb, kc, kd, ke, kf, kg, kh, Za, Zb, Zc, insqr, rZa, px, py, pz, dx, dy, dz;
	int i, j, s;

	cr2 = cr*cr;
	for(i=3-1;i>=0;i--)
	{
		ntri[i].x = tri[i].x-c0->x;
		ntri[i].y = tri[i].y-c0->y;
		ntri[i].z = tri[i].z-c0->z;
	}

		//Check plane..
	ux = ntri[1].x-ntri[0].x; vx = ntri[2].x-ntri[0].x;
	uy = ntri[1].y-ntri[0].y; vy = ntri[2].y-ntri[0].y;
	uz = ntri[1].z-ntri[0].z; vz = ntri[2].z-ntri[0].z;
		//hx = ux*u + vx*v + ntri[0].x, ix = cv->x*t
		//hy = uy*u + vy*v + ntri[0].y, iy = cv->y*t
		//hz = uz*u + vz*v + ntri[0].z, iz = cv->z*t
		//(hx-ix)*ux + (hy-iy)*uy + (hz-iz)*uz = 0
		//(hx-ix)*vx + (hy-iy)*vy + (hz-iz)*vz = 0
		//(hx-ix)^2  + (hy-iy)^2  + (hz-iz)^2 = cr2
		//-----------------------------------------
		//(ux*u + vx*v - cv->x*t + ntri[0].x)*ux +
		//(uy*u + vy*v - cv->y*t + ntri[0].y)*uy +
		//(uz*u + vz*v - cv->z*t + ntri[0].z)*uz = 0
		//
		//(ux*u + vx*v - cv->x*t + ntri[0].x)*vx +
		//(uy*u + vy*v - cv->y*t + ntri[0].y)*vy +
		//(uz*u + vz*v - cv->z*t + ntri[0].z)*vz = 0
		//
		//(ux*u + vx*v - cv->x*t + ntri[0].x)^2 +
		//(uy*u + vy*v - cv->y*t + ntri[0].y)^2 +
		//(uz*u + vz*v - cv->z*t + ntri[0].z)^2 = cr2
	k0 = ux*ux + uy*uy + uz*uz;
	k1 = ux*vx + uy*vy + uz*vz;
	k5 = vx*vx + vy*vy + vz*vz;
	k2 = cv->x*ux + cv->y*uy + cv->z*uz; k3 = ntri[0].x*ux + ntri[0].y*uy + ntri[0].z*uz;
	k6 = cv->x*vx + cv->y*vy + cv->z*vz; k7 = ntri[0].x*vx + ntri[0].y*vy + ntri[0].z*vz;
		//k0*u + k1*v + -k2*t = -k3
		//k1*u + k5*v + -k6*t = -k7
	f = k0*k5 - k1*k1;
	k8 = k2*k5 - k1*k6; k9 = k1*k7 - k3*k5;
	ka = k0*k6 - k1*k2; kb = k1*k3 - k0*k7;
		//u = (t*k8 + k9)/f
		//v = (t*ka + kb)/f
		//(ux*(t*k8 + k9) + vx*(t*ka + kb) - cv->x*t*f + ntri[0].x*f)^2 +
		//(uy*(t*k8 + k9) + vy*(t*ka + kb) - cv->y*t*f + ntri[0].y*f)^2 +
		//(uz*(t*k8 + k9) + vz*(t*ka + kb) - cv->z*t*f + ntri[0].z*f)^2 = cr2*f*f
	kc = ux*k8 + vx*ka - cv->x*f; kd = ux*k9 + vx*kb + ntri[0].x*f;
	ke = uy*k8 + vy*ka - cv->y*f; kf = uy*k9 + vy*kb + ntri[0].y*f;
	kg = uz*k8 + vz*ka - cv->z*f; kh = uz*k9 + vz*kb + ntri[0].z*f;
		//(kc*t + kd)^2 +
		//(ke*t + kf)^2 +
		//(kg*t + kh)^2 = cr2*f*f
	Za = kc*kc + ke*ke + kg*kg;
	Zb = kc*kd + ke*kf + kg*kh;
	Zc = kd*kd + kf*kf + kh*kh - cr2*f*f;
	insqr = Zb*Zb - Za*Zc;
	if ((insqr >= 0.0) && (Za != 0.0))
	{
		rZa = 1.0/Za;
		for(s=-1;s<=1;s+=2)
		{
			t = (sqrt(insqr)*(double)s - Zb)*rZa; if ((t < 0.0) || (t >= tmin)) continue;
			u = t*k8 + k9; if (u < 0.0) continue;
			v = t*ka + kb; if ((v < 0.0) || (u+v > f)) continue;
			tmin = t; f = 1.0/f; u *= f; v *= f;
			hit->x = ux*u + vx*v + tri[0].x;
			hit->y = uy*u + vy*v + tri[0].y;
			hit->z = uz*u + vz*v + tri[0].z;
		}
	}

	for(j=2,i=0;i<3;j=i,i++)
	{
		px = ntri[i].x; dx = ntri[j].x-px;
		py = ntri[i].y; dy = ntri[j].y-py;
		pz = ntri[i].z; dz = ntri[j].z-pz;

			//Check 3 line segments..
			//ix = dx*u + px
			//iy = dy*u + py
			//iz = dz*u + pz
			//
			//   //if sphere hits inside line segment,
			//   //(ix,iy,iz)-(cv->x*t,cv->y*t,cv->z*t) must be
			//   //perpendicular to line ntri[i]-ntri[j]
			//(ix-cv->x*t)*dx + "y + "z = 0
			//(ix-cv->x*t)^2 + "y + "z = cr^2, 0<t<tmin
			//
			//   2eq/2unk:
			//(dx*u + px - cv->x*t)*dx + "y + "z = 0
			//(dx*u + px - cv->x*t)^2 + "y + "z = cr^2
		f = cv->x*dx + cv->y*dy + cv->z*dz;
		k0 = dx*dx + dy*dy + dz*dz;
		k1 = px*dx + py*dy + pz*dz;
			//t = (k0*u + k1)/f;
			//
			//(dx*u*f + px*f - cv->x*(k0*u + k1))^2 + "y + "z = cr^2*f*f
			//((dx*f - cv->x*k0)*u + (px*f - cv->x*k1))^2 + "y + "z = cr^2*f*f
		k2 = dx*f - cv->x*k0; k5 = px*f - cv->x*k1;
		k3 = dy*f - cv->y*k0; k6 = py*f - cv->y*k1;
		k4 = dz*f - cv->z*k0; k7 = pz*f - cv->z*k1;
		Za = k2*k2 + k3*k3 + k4*k4;
		Zb = k2*k5 + k3*k6 + k4*k7;
		Zc = k5*k5 + k6*k6 + k7*k7 - cr2*f*f;
		insqr = Zb*Zb - Za*Zc;
		if ((insqr >= 0.0) && (Za != 0.0))
		{
			rZa = 1.0/Za;
			for(s=-1;s<=1;s+=2)
			{
				u = (sqrt(insqr)*(double)s - Zb)*rZa; if ((u < 0.0) || (u > 1.0)) continue;
				t = (k0*u + k1)/f; if ((t < 0.0) || (t >= tmin)) continue;
				tmin = t;
				hit->x = dx*u + tri[i].x;
				hit->y = dy*u + tri[i].y;
				hit->z = dz*u + tri[i].z;
			}
		}

			//Check 3 endpoints..
			//(cv->x*t - px)^2 + "y + "z = cr^2
		Za = cv->x*cv->x + cv->y*cv->y + cv->z*cv->z;
		Zb = cv->x*px + cv->y*py + cv->z*pz;
		Zc = px*px + py*py + pz*pz - cr2;
		insqr = Zb*Zb - Za*Zc;
		if ((insqr >= 0.0) && (Za != 0.0))
		{
			t = (Zb - sqrt(insqr))/Za;
			if ((t >= 0.0) && (t < tmin)) { tmin = t; (*hit) = tri[i]; }
		}
	}

	return(tmin);
}

surf_t *oct_sphtrace (oct_t *loct, dpoint3d *p, dpoint3d *padd, double rad, ipoint3d *hit, dpoint3d *pgoal, dpoint3d *hitnorm)
{
	typedef struct { octv_t *ptr; int x, y, z, j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	surf_t *lsurf = 0;
	int_cube_cyl_3d_t ic;
	double d, dx, dy, dz, dnx, dny, dnz, bhx, bhy, bhz, tmin, ntmin, nntmin;
	int i, j, k, ls, x, y, z, nx, ny, nz, hnx, hny, hnz, ord;

	if (pgoal)
	{
		pgoal->x = p->x+padd->x;
		pgoal->y = p->y+padd->y;
		pgoal->z = p->z+padd->z;
	}

	int_cube_cyl_3d_init(&ic,p->x,p->y,p->z,p->x+padd->x,p->y+padd->y,p->z+padd->z,rad);
	tmin = 1.0; ord = (padd->z >= 0.0)*4 + (padd->y >= 0.0)*2 + (padd->x >= 0.0);
	ls = loct->lsid-1; ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		k = j^ord;

		i = (1<<k); if (!(ptr->chi&i)) goto tosibly;

		nx = (( k    &1)<<ls)+x;
		ny = (((k>>1)&1)<<ls)+y;
		nz = (((k>>2)&1)<<ls)+z;

		ntmin = int_cube_cyl_3d_gett(&ic,nx,ny,nz,1<<ls,tmin);
		if (ntmin >= tmin) goto tosibly;

		if (ls <= 0)
		{
#if (MARCHCUBE != 0)
			dpoint3d tri[15], hit; ntmin = tmin;
			for(k=marchcube_gen(&((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind],tri)-3;k>=0;k-=3)
			{
				tri[k].x += nx; tri[k+1].x += nx; tri[k+2].x += nx;
				tri[k].y += ny; tri[k+1].y += ny; tri[k+2].y += ny;
				tri[k].z += nz; tri[k+1].z += nz; tri[k+2].z += nz;
				nntmin = int_tri_cyl_3d_gett(p,rad,padd,&tri[k],tmin,&hit);
				if (nntmin < ntmin) { ntmin = nntmin; dnx = hit.x; dny = hit.y; dnz = hit.z; }
			}
			if (ntmin >= tmin) goto tosibly;
			bhx = dnx; bhy = dny; bhz = dnz;
#endif
			hnx = nx; hny = ny; hnz = nz;
			tmin = ntmin;
			lsurf = &((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind];
			goto tosibly;
		}

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; ls--; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1;
		continue;

tosibly:;
		j--; if (j >= 0) continue;
		do
		{
			ls++;
			if (ls >= loct->lsid)
			{
				dx = padd->x*tmin + p->x;
				dy = padd->y*tmin + p->y;
				dz = padd->z*tmin + p->z;
				if (pgoal)
				{
					pgoal->x = dx;
					pgoal->y = dy;
					pgoal->z = dz;
				}
				if (!lsurf) return(0);
				if (hit) { hit->x = hnx; hit->y = hny; hit->z = hnz; }
				if (hitnorm)
				{
						//hitx/y/z is nearest point on cube (hit->x,hit->y,hit->z,1) to pgoal
					d = 1.0/rad;
#if (MARCHCUBE == 0)
					hitnorm->x = (min(max(dx,(double)hnx),(double)(hnx+1)) - dx)*d;
					hitnorm->y = (min(max(dy,(double)hny),(double)(hny+1)) - dy)*d;
					hitnorm->z = (min(max(dz,(double)hnz),(double)(hnz+1)) - dz)*d;
#else
					hitnorm->x = (bhx - dx)*d;
					hitnorm->y = (bhy - dy)*d;
					hitnorm->z = (bhz - dz)*d;
#endif
				}
				return(lsurf);
			}
			j = stk[ls].j-1;
		} while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z;
	}
}
surf_t *oct_sphtrace (oct_t *loct, point3d *fp, point3d *fpadd, double rad, ipoint3d *hit, point3d *fpgoal, point3d *fhitnorm)
{
	dpoint3d dp, dpadd, dpgoal, dhitnorm;
	surf_t *retval;
	dp.x = fp->x; dp.y = fp->y; dp.z = fp->z;
	dpadd.x = fpadd->x; dpadd.y = fpadd->y; dpadd.z = fpadd->z;
	retval = oct_sphtrace(loct,&dp,&dpadd,rad,hit,&dpgoal,&dhitnorm);
	fpgoal->x = dpgoal.x; fpgoal->y = dpgoal.y; fpgoal->z = dpgoal.z;
	fhitnorm->x = dhitnorm.x; fhitnorm->y = dhitnorm.y; fhitnorm->z = dhitnorm.z;
	return(retval);
}
//--------------------------------------------------------------------------------------------------

	//   x,y,z: vector difference of test point to cube's top-left-etc corner
	//     dia: cube's diameter
	// returns: distance squared from point to nearest point on cube (0.0 if inside)
static double dist2cube2_nearest (double x, double y, double z, double dia)
{
	double rad = dia*.5;
	x = fabs(x+rad); x -= min(x,rad);
	y = fabs(y+rad); y -= min(y,rad);
	z = fabs(z+rad); z -= min(z,rad);
	return(x*x + y*y + z*z);
}
static double dist2cube2_farthest (double x, double y, double z, double dia)
{
	double rad = dia*.5;
	x = fabs(x+rad)+dia;
	y = fabs(y+rad)+dia;
	z = fabs(z+rad)+dia;
	return(x*x + y*y + z*z);
}
static double dist2tri2_nearest_2d (dpoint2d *p, dpoint2d *p0, dpoint2d *p1, dpoint2d *p2)
{
	double x0, y0, x1, y1, x2, y2, x10, y10, x21, y21, x02, y02, t0, t1, t2;
	int a0, a1, a2, a3, a4, a5, sid;

	x0 = p->x-p0->x; y0 = p->y-p0->y; x10 = p1->x-p0->x; y10 = p1->y-p0->y;
	x1 = p->x-p1->x; y1 = p->y-p1->y; x21 = p2->x-p1->x; y21 = p2->y-p1->y;
	x2 = p->x-p2->x; y2 = p->y-p2->y; x02 = p0->x-p2->x; y02 = p0->y-p2->y;
	a5 = (x0*x02 + y0*y02 > 0.0);
	a0 = (x0*x10 + y0*y10 > 0.0); if (a5 > a0) return(x0*x0 + y0*y0);
	a1 = (x1*x10 + y1*y10 > 0.0);
	a2 = (x1*x21 + y1*y21 > 0.0); if (a1 > a2) return(x1*x1 + y1*y1);
	a3 = (x2*x21 + y2*y21 > 0.0);
	a4 = (x2*x02 + y2*y02 > 0.0); if (a3 > a4) return(x2*x2 + y2*y2);
	sid = (x10*y21 < y10*x21);
	t0 = x0*y10 - y0*x10;
	t1 = x1*y21 - y1*x21;
	t2 = x2*y02 - y2*x02;
	if (((t0<0.0) == sid) && (a0 > a1)) return(t0*t0/(x10*x10 + y10*y10));
	if (((t1<0.0) == sid) && (a2 > a3)) return(t1*t1/(x21*x21 + y21*y21));
	if (((t2<0.0) == sid) && (a4 > a5)) return(t2*t2/(x02*x02 + y02*y02));
	return(0.0);
}
static double dist2tri2_nearest_3d (dpoint3d *p, dpoint3d *p0, dpoint3d *p1, dpoint3d *p2)
{
	dpoint3d a, b, n;
	dpoint2d q, q0, q1, q2;
	double f;

	n.x = (p1->y-p0->y)*(p2->z-p0->z) - (p1->z-p0->z)*(p2->y-p0->y);
	n.y = (p1->z-p0->z)*(p2->x-p0->x) - (p1->x-p0->x)*(p2->z-p0->z);
	n.z = (p1->x-p0->x)*(p2->y-p0->y) - (p1->y-p0->y)*(p2->x-p0->x);
	f = 1.0/sqrt(n.x*n.x + n.y*n.y + n.z*n.z); n.x *= f; n.y *= f; n.z *= f;
	a.x = (p1->y-p0->y)*n.z - (p1->z-p0->z)*n.y;
	a.y = (p1->z-p0->z)*n.x - (p1->x-p0->x)*n.z;
	a.z = (p1->x-p0->x)*n.y - (p1->y-p0->y)*n.x;
	b.x = n.y*a.z - n.z*a.y;
	b.y = n.z*a.x - n.x*a.z;
	b.z = n.x*a.y - n.y*a.x;

	f   = (p->x-p0->x)*n.x + (p->y-p0->y)*n.y + (p->z-p0->z)*n.z;
	q.x = (p->x-p0->x)*a.x + (p->y-p0->y)*a.y + (p->z-p0->z)*a.z;
	q.y = (p->x-p0->x)*b.x + (p->y-p0->y)*b.y + (p->z-p0->z)*b.z;
	q0.x = 0.0;
	q0.y = 0.0;
	q1.x = 0.0;
	q1.y = (p1->x-p0->x)*b.x + (p1->y-p0->y)*b.y + (p1->z-p0->z)*b.z;
	q2.x = (p2->x-p0->x)*a.x + (p2->y-p0->y)*a.y + (p2->z-p0->z)*a.z;
	q2.y = (p2->x-p0->x)*b.x + (p2->y-p0->y)*b.y + (p2->z-p0->z)*b.z;
	return(dist2tri2_nearest_2d(&q,&q0,&q1,&q2)/(a.x*a.x + a.y*a.y + a.z*a.z) + f*f);
}

	//find closest distance from point to any voxel (renamed from findmaxcr)
double oct_balloonrad (oct_t *loct, dpoint3d *p, double cr, ipoint3d *hit, surf_t **hitsurf)
{
	typedef struct { octv_t *ptr; int x, y, z, j, ord; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	double cr2, ncr2;
	int i, j, k, s, ls, x, y, z, nx, ny, nz, ord, ipx, ipy, ipz;

	if (cr <= 0.0) return(-1.0);

	ipx = (int)floor(p->x); ipy = (int)floor(p->y); ipz = (int)floor(p->z);

	cr2 = cr*cr; hit->x = -1;
	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head];
	x = 0; y = 0; z = 0; j = 8-1; ord = (ipz < z+s)*4 + (ipy < y+s)*2 + (ipx < x+s);
	while (1)
	{
		k = (j^ord);

		i = (1<<k); if (!(ptr->chi&i)) goto tosibly;

		nx = (( k    &1)<<ls)+x;
		ny = (((k>>1)&1)<<ls)+y;
		nz = (((k>>2)&1)<<ls)+z;

		ncr2 = dist2cube2_nearest((double)nx-p->x,(double)ny-p->y,(double)nz-p->z,(double)s);
		if (ncr2 >= cr2) goto tosibly;

		if (ls <= 0)
		{
#if (MARCHCUBE != 0)
			dpoint3d pt, tri[15]; ncr2 = 1e32;
			pt.x = p->x-nx; pt.y = p->y-ny; pt.z = p->z-nz;
			for(k=marchcube_gen(&((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind],tri)-3;k>=0;k-=3)
				ncr2 = min(ncr2,dist2tri2_nearest_3d(&pt,&tri[k],&tri[k+1],&tri[k+2]));
			if (ncr2 >= cr2) goto tosibly;
#endif

			cr2 = ncr2;
			if (hit) { hit->x = nx; hit->y = ny; hit->z = nz; }
			if (hitsurf) (*hitsurf) = &((surf_t *)loct->sur.buf)[popcount[ptr->chi&(i-1)] + ptr->ind];
			goto tosibly;
		}

		if (!(ptr->sol&i)) cr2 = min(cr2,dist2cube2_farthest((double)nx-p->x,(double)ny-p->y,(double)nz-p->z,(double)s));

		stk[ls].ptr = ptr; stk[ls].x = x; stk[ls].y = y; stk[ls].z = z; stk[ls].j = j; stk[ls].ord = ord; ls--; s >>= 1; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; x = nx; y = ny; z = nz; j = 8-1; ord = (ipz < z+s)*4 + (ipy < y+s)*2 + (ipx < x+s);
		continue;

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) return(sqrt(cr2)); j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr; x = stk[ls].x; y = stk[ls].y; z = stk[ls].z; ord = stk[ls].ord;
	}
}
double oct_balloonrad (oct_t *loct, point3d *fp, double cr, ipoint3d *hit, surf_t **hitsurf)
{
	dpoint3d dp;
	dp.x = fp->x; dp.y = fp->y; dp.z = fp->z;
	return(oct_balloonrad(loct,&dp,cr,hit,hitsurf));
}

//--------------------------------------------------------------------------------------------------

int oct_slidemove (oct_t *loct, dpoint3d *p, dpoint3d *padd, double fat, dpoint3d *pgoal)
{
	dpoint3d fp, fa, hit[3], hnorm[3];
	ipoint3d lp;
	surf_t *psurf;
	double d;
	int i;

	fp = *p; fa = *padd; fat = oct_balloonrad(loct,&fp,fat,&lp,&psurf);
	for(i=0;1;i++)
	{
		fat -= 1e-7; if (!oct_sphtrace(loct,&fp,&fa,fat,0,&hit[i],&hnorm[i])) { *pgoal = hit[i]; return(i); }
		if (i == 2) break;

		fa.x += fp.x-hit[i].x;
		fa.y += fp.y-hit[i].y;
		fa.z += fp.z-hit[i].z;
		fp = hit[i];

		if (!i)
		{
			d = fa.x*hnorm[0].x + fa.y*hnorm[0].y + fa.z*hnorm[0].z;
			fa.x -= hnorm[0].x*d;
			fa.y -= hnorm[0].y*d;
			fa.z -= hnorm[0].z*d;
		}
		else
		{
			hnorm[2].x = hnorm[0].y*hnorm[1].z - hnorm[0].z*hnorm[1].y;
			hnorm[2].y = hnorm[0].z*hnorm[1].x - hnorm[0].x*hnorm[1].z;
			hnorm[2].z = hnorm[0].x*hnorm[1].y - hnorm[0].y*hnorm[1].x;
			d = hnorm[2].x*hnorm[2].x + hnorm[2].y*hnorm[2].y + hnorm[2].z*hnorm[2].z; if (d == 0.0) return(1);
			d = (fa.x*hnorm[2].x + fa.y*hnorm[2].y + fa.z*hnorm[2].z)/d;
			fa.x = hnorm[2].x*d;
			fa.y = hnorm[2].y*d;
			fa.z = hnorm[2].z*d;
		}
	}
	*pgoal = hit[2]; return(3);
}
int oct_slidemove (oct_t *loct, point3d *fp, point3d *fpadd, double fat, point3d *fpgoal)
{
	dpoint3d dp, dpadd, dpgoal;
	int retval;
	dp.x = fp->x; dp.y = fp->y; dp.z = fp->z;
	dpadd.x = fpadd->x; dpadd.y = fpadd->y; dpadd.z = fpadd->z;
	retval = oct_slidemove(loct,&dp,&dpadd,fat,&dpgoal);
	fpgoal->x = dpgoal.x; fpgoal->y = dpgoal.y; fpgoal->z = dpgoal.z;
	return(retval);
}

//--------------------------------------------------------------------------------------------------
static int palhashead[4096];
typedef struct { int rgb, n; } paltab_t;
static paltab_t paltab[4096];
static int paltabn;
static void palreset (void) { memset(palhashead,-1,sizeof(palhashead)); paltabn = 0; }
static int palget (int rgb)
{
	int i, hash;
	hash = ((((rgb>>22)^(rgb>>11))-rgb)&(sizeof(palhashead)/sizeof(palhashead[0])-1));
	for(i=palhashead[hash];i>=0;i=paltab[i].n) if (paltab[i].rgb == rgb) return(i);
	if (paltabn >= sizeof(paltab)/sizeof(paltab[0])) return(-1);
	paltab[paltabn].rgb = rgb;
	paltab[paltabn].n = palhashead[hash]; palhashead[hash] = paltabn; paltabn++;
	return(paltabn-1);
}
//--------------------------------------------------------------------------------------------------

//EQUIVEC code begins -----------------------------------------------------

#define GOLDRAT 0.3819660112501052 //Golden Ratio: 1 - 1/((sqrt(5)+1)/2)
typedef struct
{
	float fibx[45], fiby[45];
	float azval[20], zmulk, zaddk;
	int fib[47], aztop, npoints;
	point3d *p;  //For memory version :/
	int pcur;
} equivectyp;
static equivectyp equivec;

static void equiind2vec (int i, float *x, float *y, float *z)
{
	float a, r;
	(*z) = (float)i*equivec.zmulk + equivec.zaddk; r = sqrt(1.f - (*z)*(*z));
	a = ((float)i)*(GOLDRAT*PI*2); (*x) = cos(a)*r; (*y) = sin(a)*r;
}

	//(Pass n=0 to free buffer)
static int equimemset (int n)
{
	int z;

	if (equivec.pcur == n) return(1); //Don't recalculate if same #
	if (equivec.p) { free(equivec.p); equivec.p = 0; }
	if (!n) return(1);

		//Init vector positions (equivec.p) for memory version
	if (!(equivec.p = (point3d *)malloc(((n+1)&~1)*sizeof(point3d))))
		return(0);

	equivec.pcur = n;
	equivec.zmulk = 2 / (float)n; equivec.zaddk = equivec.zmulk*.5 - 1.0;
	for(z=n-1;z>=0;z--)
		equiind2vec(z,&equivec.p[z].x,&equivec.p[z].y,&equivec.p[z].z);
	if (n&1) //Hack for when n=255 and want a <0,0,0> vector
		{ equivec.p[n].x = equivec.p[n].y = equivec.p[n].z = 0; }
	return(1);
}

	//Very fast; good quality, requires equivec.p[] :/
static int equivec2indmem (float x, float y, float z)
{
	int b, i, j, k, bestc;
	float xy, zz, md, d;

	xy = atan2(y,x); //atan2 is 150 clock cycles!
	j = ((*(int *)&z)&0x7fffffff);
	bestc = equivec.aztop;
	do
	{
		if (j < *(int *)&equivec.azval[bestc]) break;
		bestc--;
	} while (bestc);

	zz = z + 1.f;
	i = (int)(equivec.fibx[bestc]*xy + equivec.fiby[bestc]*zz - .5);
	bestc++;
	j = (int)(equivec.fibx[bestc]*xy + equivec.fiby[bestc]*zz - .5);

	k = equivec.fib[bestc+2]*i + equivec.fib[bestc+1]*j;
	if ((unsigned)k < (unsigned)equivec.npoints)
	{
		md = equivec.p[k].x*x + equivec.p[k].y*y + equivec.p[k].z*z;
		j = k;
	} else md = -2.f;
	b = bestc+3;
	do
	{
		i = equivec.fib[b] + k;
		if ((unsigned)i < (unsigned)equivec.npoints)
		{
			d = equivec.p[i].x*x + equivec.p[i].y*y + equivec.p[i].z*z;
			if (*(int *)&d > *(int *)&md) { md = d; j = i; }
		}
		b--;
	} while (b != bestc);
	return(j);
}

static void equivecinit (int n)
{
	float t0, t1;
	int z;

		//Init constants for ind2vec
	equivec.npoints = n;
	equivec.zmulk = 2 / (float)n; equivec.zaddk = equivec.zmulk*.5 - 1.0;

	equimemset(n);

		//Init Fibonacci table
	equivec.fib[0] = 0; equivec.fib[1] = 1;
	for(z=2;z<47;z++) equivec.fib[z] = equivec.fib[z-2]+equivec.fib[z-1];

		//Init fibx/y LUT
	t0 = .5 / PI; t1 = (float)n * -.5;
	for(z=0;z<45;z++)
	{
		t0 = -t0; equivec.fibx[z] = (float)equivec.fib[z+2]*t0;
		t1 = -t1; equivec.fiby[z] = ((float)equivec.fib[z+2]*GOLDRAT - (float)equivec.fib[z])*t1;
	}

	t0 = 1 / ((float)n * PI);
	for(equivec.aztop=0;equivec.aztop<20;equivec.aztop++)
	{
		t1 = 1 - (float)equivec.fib[(equivec.aztop<<1)+6]*t0; if (t1 < 0) break;
		equivec.azval[equivec.aztop+1] = sqrt(t1);
	}
}

static void equivecuninit (void) { equimemset(0); }

//EQUIVEC code ends -------------------------------------------------------

typedef struct { octv_t *octv; int bx, by, bz, bdx, bdy, bdz, stat; } ogbs_t;
static void oct_getboxstate_recur (ogbs_t *ogbs, int x0, int y0, int z0, int s, int ind)
{
	octv_t *ptr;
	int i, j, x, y, z, v;

	ptr = &ogbs->octv[ind]; v = (1<<8)-1;
	x = x0+s-ogbs->bx; if (x <= 0) v &= 0xaa; else if (x >= ogbs->bdx) v &= 0x55;
	y = y0+s-ogbs->by; if (y <= 0) v &= 0xcc; else if (y >= ogbs->bdy) v &= 0x33;
	z = z0+s-ogbs->bz; if (z <= 0) v &= 0xf0; else if (z >= ogbs->bdz) v &= 0x0f;
	if ( v&          ptr->sol       ) { ogbs->stat |= 2; if (ogbs->stat == 3) return; }
	if ((v&(ptr->chi|ptr->sol)) != v) { ogbs->stat |= 1; if (ogbs->stat == 3) return; }
	if (s == 1) return;
	v = (v&ptr->chi&(~ptr->sol));
	for(;v;v^=i)
	{
		i = (-v)&v;
		x = x0; if (i&0xaa) x += s;
		y = y0; if (i&0xcc) y += s;
		z = z0; if (i&0xf0) z += s;
		oct_getboxstate_recur(ogbs,x,y,z,s>>1,popcount[(i-1)&ptr->chi]+ptr->ind);
		if (ogbs->stat == 3) return;
	}
}
	//returns: 0:pure air, 1:some air&sol, 2:pure sol
int oct_getboxstate (oct_t *loct, int bx0, int by0, int bz0, int bx1, int by1, int bz1)
{
	ogbs_t ogbs;

	ogbs.octv = (octv_t *)loct->nod.buf;
	ogbs.bx = bx0; ogbs.bdx = bx1-bx0;
	ogbs.by = by0; ogbs.bdy = by1-by0;
	ogbs.bz = bz0; ogbs.bdz = bz1-bz0;
	ogbs.stat = 0;
	oct_getboxstate_recur(&ogbs,0,0,0,loct->sid>>1,loct->head);
	if (ogbs.stat == 3) return(1);
	return(ogbs.stat&2);
}

	//NOTE:x0,etc. must be filled with bounded box to check on input. For whole object, set x0=0; x1=loct->sid; etc..
	//NOTE:ins&outs are inclusive on x0/y0/z0 and exclusive on x1/y1/z1
int oct_getsolbbox (oct_t *loct, int *rx0, int *ry0, int *rz0, int *rx1, int *ry1, int *rz1)
{
	typedef struct { octv_t *ptr; int j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	int i, j, ls, s, x, y, z, x0, y0, z0, x1, y1, z1;

	x0 = loct->sid; x1 = -1;
	y0 = loct->sid; y1 = -1;
	z0 = loct->sid; z1 = -1;
	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		i = (1<<j); if (!((ptr->chi|ptr->sol)&i)) goto tosibly;

		x = ((-((j   )&1))&s) + (x&(-s-s));
		y = ((-((j>>1)&1))&s) + (y&(-s-s));
		z = ((-((j>>2)&1))&s) + (z&(-s-s));

		if ((x >= (*rx1)) || (x+s <= (*rx0)) || //skip if fully outside user input bbox
			 (y >= (*ry1)) || (y+s <= (*ry0)) ||
			 (z >= (*rz1)) || (z+s <= (*rz0))) goto tosibly;
		if ((x >= x0) && (x+s <= x1) && //skip if cell fully inside current extents
			 (y >= y0) && (y+s <= y1) &&
			 (z >= z0) && (z+s <= z1)) goto tosibly;

		if (ptr->sol&i)
		{
			x0 = min(x0,x); x1 = max(x1,x+s);
			y0 = min(y0,y); y1 = max(y1,y+s);
			z0 = min(z0,z); z1 = max(z1,z+s);
			goto tosibly;
		}

		stk[ls].ptr = ptr; stk[ls].j = j; s >>= 1; ls--; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; j = 8-1;
		continue;

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) goto endit; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr;
	}
endit:
	if (x0 >= x1) return(0);
	(*rx0) = max(*rx0,x0); (*rx1) = min(*rx1,x1);
	(*ry0) = max(*ry0,y0); (*ry1) = min(*ry1,y1);
	(*rz0) = max(*rz0,z0); (*rz1) = min(*rz1,z1);
	return(1);
}

typedef struct { oct_t *loct; int remap[8]; char invrebits[256]; } osr_t;
static void oct_swizzle_recur (osr_t *osr, int ind, int ls)
{
	surf_t surf[8];
	octv_t node[8], *ptr;
	int i, j, n, ochi;

	ptr = &((octv_t *)osr->loct->nod.buf)[ind];
	ochi = ptr->chi;
	ptr->chi = osr->invrebits[ptr->chi];
	ptr->sol = osr->invrebits[ptr->sol];
	n = 0;
	for(i=0;i<8;i++)
	{
		j = osr->remap[i];
		if (!(ochi&(1<<j))) continue;
		if (ls) memcpy(&node[n],&((surf_t *)osr->loct->nod.buf)[popcount[((1<<j)-1)&ochi]+ptr->ind],osr->loct->nod.siz);
			else memcpy(&surf[n],&((surf_t *)osr->loct->sur.buf)[popcount[((1<<j)-1)&ochi]+ptr->ind],osr->loct->sur.siz);
		n++;
	}

	if (ls) memcpy(&((surf_t *)osr->loct->nod.buf)[ptr->ind],node,osr->loct->nod.siz*n);
		else memcpy(&((surf_t *)osr->loct->sur.buf)[ptr->ind],surf,osr->loct->sur.siz*n);

	if (ls) for(i=0;i<n;i++) oct_swizzle_recur(osr,ptr->ind+i,ls-1);
}
	//+x=+1  +y=+2  +z=+3
	//-x=-1  -y=-2  -z=-3
void oct_swizzle (oct_t *loct, int ax0, int ax1, int ax2)
{
	osr_t osr;
	int i, j, k, m, invremap[8];

	if ((ax0 == 0) || (ax1 == 0) || (ax2 == 0)) return;
	if ((labs(ax0) > 3) || (labs(ax1) > 3) || (labs(ax2) > 3)) return;
	if ((labs(ax0) == labs(ax1)) || (labs(ax0) == labs(ax2)) || (labs(ax1) == labs(ax2))) return;
#if (GPUSEBO != 0)
	if (oct_usegpu)
	{
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
	}
#endif

	osr.loct = loct;
	switch(labs(ax0))
	{
		case 1: if (labs(ax1) == 2) j = 0x01234567; else j = 0x01452367; break;
		case 2: if (labs(ax1) == 1) j = 0x02134657; else j = 0x02461357; break;
		case 3: if (labs(ax1) == 1) j = 0x04152637; else j = 0x04261537; break;
	}
	k = ((ax0<0)<<(labs(ax0)-1)) + ((ax1<0)<<(labs(ax1)-1)) + ((ax2<0)<<(labs(ax2)-1));
	for(i=8-1;i>=0;i--) osr.remap[i] = ((j>>(28-(i<<2)))&15)^k;

	for(i=8-1;i>=0;i--) invremap[osr.remap[i]] = i;
	memset(osr.invrebits,0,sizeof(osr.invrebits));
	for(j=8-1;j>=0;j--)
	{
		k = (1<<j); m = (1<<invremap[j]);
		for(i=k;i<256;i=((i+1)|k)) osr.invrebits[i] += m;
	}

	oct_swizzle_recur(&osr,loct->head,loct->lsid-1);

	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.mal*2)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
#else
		memcpy(loct->gsurf,loct->sur.buf,loct->sur.mal*loct->sur.siz);
#endif
	}
}

	//                                      nsol:
	//chi sol   ls=0   ls>0            | ls=0  ls>0
	// 0   0    air    pure air        |   1     1
	// 1   0    N/A    hybrid          |  (0)    0
	// 0   1    sol    pure sol        |   0     0
	// 1   1    surf   pure sol w/surf |   1   &OfChildren
static void oct_swap_sol_air_recur (oct_t *loct, int ind, int ls)
{
	octv_t *ptr;
	int o, v, iup;

	ptr = &((octv_t *)loct->nod.buf)[ind];
	if (!ls)
	{
		ptr->sol = (~ptr->sol)|ptr->chi;
		return;
	}
	ptr->sol = ~(ptr->chi|ptr->sol); o = ptr->ind;
	for(v=ptr->chi;v;v^=iup)
	{
		iup = (-v)&v;
		oct_swap_sol_air_recur(loct,o,ls-1);
		if (((octv_t *)loct->nod.buf)[o].sol == (1<<8)-1) ptr->sol |= iup;
		o++;
	}
}
void oct_swap_sol_air (oct_t *loct)
{
	oct_swap_sol_air_recur(loct,loct->head,loct->lsid-1);
	loct->edgeissol ^= 63;
}

	//Ensures box is defined in octree (i.e. box is inside 0..sid-1)
	//Algo is: grow .. translate .. shrink
int oct_rebox (oct_t *loct, int bx0, int by0, int bz0, int bx1, int by1, int bz1, int *dx, int *dy, int *dz)
{
	static const char wshlut[3][8] = {-1,1,7,9,15,17,23,25, -1,6,2,10,14,22,18,26, -1,4,12,20,4,12,20,28};
	static const char wshr[3][8] = {0,-1,8,-1,16,-1,24,-1, 0,8,-1,-1,16,24,-1,-1, 0,8,16,24,-1,-1,-1,-1};
	static const int windex[3][4] = {0,2,4,6, 0,1,4,5, 0,1,2,3};
	octv_t nod0, nod1, onod[32];
	int nchi0, nsol0, nind0, nchi1, nsol1, nind1;
	int w, w0[3], w1[3];
	int i, j, k, m, n, ind, got, anygot;

	if (dx) (*dx) = 0;
	if (dy) (*dy) = 0;
	if (dz) (*dz) = 0;
	anygot = 0;

		//force bbox to be outside defined solid
	for(i=0;i<3;i++) { w0[i] = 0; w1[i] = loct->sid; }
	oct_getsolbbox(loct,&w0[0],&w0[1],&w0[2],&w1[0],&w1[1],&w1[2]);
	w0[0] = min(w0[0],bx0); w1[0] = max(w1[0],bx1);
	w0[1] = min(w0[1],by0); w1[1] = max(w1[1],by1);
	w0[2] = min(w0[2],bz0); w1[2] = max(w1[2],bz1);

		//grow (double)
	while ((loct->lsid < OCT_MAXLS) && (((w1[0]-1)|(w1[1]-1)|(w1[2]-1)|w0[0]|w0[1]|w0[2])&loct->nsid))
	{
		ind = bitalloc(loct,&loct->nod,1); loct->nod.num++;
		((octv_t *)loct->nod.buf)[ind] = ((octv_t *)loct->nod.buf)[loct->head];

		i = 1;
		if (w0[0]+w1[0] < loct->sid) i <<= 1;
		if (w0[1]+w1[1] < loct->sid) i <<= 2;
		if (w0[2]+w1[2] < loct->sid) i <<= 4;
		((octv_t *)loct->nod.buf)[loct->head].chi = i;

		if (i&0xaa) { w0[0] += loct->sid; w1[0] += loct->sid; if (dx) (*dx) += loct->sid; }
		if (i&0xcc) { w0[1] += loct->sid; w1[1] += loct->sid; if (dy) (*dy) += loct->sid; }
		if (i&0xf0) { w0[2] += loct->sid; w1[2] += loct->sid; if (dz) (*dz) += loct->sid; }
		anygot = 1;

		((octv_t *)loct->nod.buf)[loct->head].sol = 0;
		((octv_t *)loct->nod.buf)[loct->head].ind = ind;
		loct->lsid++; loct->sid <<= 1; loct->nsid = -loct->sid;
	}

		//translate (needed to allow model with tiny cluster in center to shrink)

		//algo guarantees bbox region no smaller than sid/4+1 on the largest sid
	do
	{
		got = 0;

			//translate by sid/2 if possible
		i = (loct->sid>>1); ind = loct->head;
		for(w=0;w<3;w++)
		{
			if (w0[w] < i) continue;
			w0[w] -= i; w1[w] -= i; got = 1;
			if ((w == 0) && dx) (*dx) -= i;
			if ((w == 1) && dy) (*dy) -= i;
			if ((w == 2) && dz) (*dz) -= i;

			((octv_t *)loct->nod.buf)[ind].chi >>= (1<<w); ((octv_t *)loct->nod.buf)[ind].sol >>= (1<<w);
		}

			//translate by sid/4 if possible
			//
			//   //Before:                                                       //After:
			//buf[ 0].chi = 0xff; buf[ 0].sol = 0x00; buf[ 0].ind = 10;       buf[  0].chi = 0x55; buf[  0].sol = 0x55; buf[  0].ind = 100;
			//..                                                              ..
			//buf[10].chi = 0xaa; buf[10].sol = 0xaa; buf[10].ind = 20;       buf[100].chi = 0xff; buf[100].sol = 0xff; buf[100].ind = 110;
			//buf[11].chi = 0x55; buf[11].sol = 0x55; buf[11].ind = 30;       buf[101].chi = 0xff; buf[101].sol = 0xff; buf[101].ind = 120;
			//buf[12].chi = 0xaa; buf[12].sol = 0xaa; buf[12].ind = 40;       buf[102].chi = 0xff; buf[102].sol = 0xff; buf[102].ind = 130;
			//buf[13].chi = 0x55; buf[13].sol = 0x55; buf[13].ind = 50;       buf[103].chi = 0xff; buf[103].sol = 0xff; buf[103].ind = 140;
			//buf[14].chi = 0xaa; buf[14].sol = 0xaa; buf[14].ind = 60;       ..
			//buf[15].chi = 0x55; buf[15].sol = 0x55; buf[15].ind = 70;       buf[110].chi = 0x7f; buf[110].sol = 0xff; buf[110].ind =   A;
			//buf[16].chi = 0xaa; buf[16].sol = 0xaa; buf[16].ind = 80;       buf[111].chi = 0xbf; buf[111].sol = 0xff; buf[111].ind =   E;
			//buf[17].chi = 0x55; buf[17].sol = 0x55; buf[17].ind = 90;       buf[112].chi = 0x5f; buf[112].sol = 0xff; buf[112].ind =   B;
			//..                                                              buf[113].chi = 0xaf; buf[113].sol = 0xff; buf[113].ind =   F;
			//buf[20].chi = 0x7f; buf[20].sol = 0xff; buf[20].ind =  A;       buf[114].chi = 0x77; buf[114].sol = 0xff; buf[114].ind =   C;
			//buf[21].chi = 0x5f; buf[21].sol = 0xff; buf[21].ind =  B;       buf[115].chi = 0xbb; buf[115].sol = 0xff; buf[115].ind =   G;
			//buf[22].chi = 0x77; buf[22].sol = 0xff; buf[22].ind =  C;       buf[116].chi = 0x55; buf[116].sol = 0xff; buf[116].ind =   D;
			//buf[23].chi = 0x55; buf[23].sol = 0xff; buf[23].ind =  D;       buf[117].chi = 0xaa; buf[117].sol = 0xff; buf[117].ind =   H;
			//..                                                              ..
			//buf[30].chi = 0xbf; buf[30].sol = 0xff; buf[30].ind =  E;
			//buf[31].chi = 0xaf; buf[31].sol = 0xff; buf[31].ind =  F;
			//buf[32].chi = 0xbb; buf[32].sol = 0xff; buf[32].ind =  G;
			//buf[33].chi = 0xaa; buf[33].sol = 0xff; buf[33].ind =  H;
			//..
			//
			//X: A--+--B--+--+  +--+--+--+--+  E--+--F--+--+  +--+--+--+--+       j=0, 1, 2, 3, 4, 5, 6, 7
			//   |  | 0| 1|  |  |  | 4| 5|  |  |  |16|17|  |  |  |20|21|  |   i=0      0     2     4     6
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=1   1     3     5     7
			//   |  | 2| 3|  |  |  | 6| 7|  |  |  |18|19|  |  |  |22|23|  |   i=2      8    10    12    14
			//   C--+--D--+--+  +--+--+--+--+  G--+--H--+--+  +--+--+--+--+   i=3   9    11    13    15
			//   |  | 8| 9|  |  |  |12|13|  |  |  |24|25|  |  |  |28|29|  |   i=4     16    18    20    22
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=5  17    19    21    23
			//   |  |10|11|  |  |  |14|15|  |  |  |26|27|  |  |  |30|31|  |   i=6     24    26    28    30
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=7  25    27    29    31
			//
			//Y: A--+--B--+--+  +--+--+--+--+  E--+--F--+--+  +--+--+--+--+       j=0, 1, 2, 3, 4, 5, 6, 7
			//   |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |   i=0         0  1        4  5
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=1         8  9       12 13
			//   | 0| 1| 8| 9|  | 4| 5|12|13|  |16|17|24|25|  |20|21|28|29|   i=2   2  3        6  7
			//   C--+--D--+--+  +--+--+--+--+  G--+--H--+--+  +--+--+--+--+   i=3  10 11       14 15
			//   | 2| 3|10|11|  | 6| 7|14|15|  |18|19|26|27|  |22|23|30|31|   i=4        16 17       20 21
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=5        24 25       28 29
			//   |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |   i=6  18 19       22 23
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=7  26 27       30 31
			//
			//Z: A--+--B--+--+  +--+--+--+--+  E--+--F--+--+  +--+--+--+--+       j=0, 1, 2, 3, 4, 5, 6, 7
			//   |  |  |  |  |  | 0| 1| 8| 9|  | 4| 5|12|13|  |  |  |  |  |   i=0               0  1  2  3
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=1               8  9 10 11
			//   |  |  |  |  |  | 2| 3|10|11|  | 6| 7|14|15|  |  |  |  |  |   i=2              16 17 18 19
			//   C--+--D--+--+  +--+--+--+--+  G--+--H--+--+  +--+--+--+--+   i=3              24 25 25 26
			//   |  |  |  |  |  |16|17|24|25|  |20|21|28|29|  |  |  |  |  |   i=4   4  5  6  7
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=5  12 13 14 15
			//   |  |  |  |  |  |18|19|26|27|  |22|23|30|31|  |  |  |  |  |   i=6  20 21 22 23
			//   +--+--+--+--+  +--+--+--+--+  +--+--+--+--+  +--+--+--+--+   i=7  28 29 30 31
		for(w=0;w<3;w++)
		{
			if ((w0[w] < (loct->sid>>2)) || (w1[w] >= ((loct->sid*3)>>2))) continue;
			w0[w] -= (loct->sid>>2); w1[w] -= (loct->sid>>2); got = 1;
			if ((w == 0) && dx) (*dx) -= (loct->sid>>2);
			if ((w == 1) && dy) (*dy) -= (loct->sid>>2);
			if ((w == 2) && dz) (*dz) -= (loct->sid>>2);

			nchi1 = 0; nsol1 = 0;
			nod0 = ((octv_t *)loct->nod.buf)[loct->head];
			n = popcount[nod0.chi]; xorzrangesmall(loct->nod.bit,nod0.ind,n); loct->nod.num -= n; //remove 2nd level nodes (8 max)
			for(i=0;i<8;i++)
			{
				if (!(nod0.chi&(1<<i))) continue;
				nod1 = ((octv_t *)loct->nod.buf)[popcount[nod0.chi&((1<<i)-1)] + nod0.ind];
				n = popcount[nod1.chi]; xorzrangesmall(loct->nod.bit,nod1.ind,n); loct->nod.num -= n; //remove 3rd level nodes (64 max)
				if (!i) //can't really encode shift left&right in same lut without platform incompatibilities :/
				{
					nchi1 += ((int)nod1.chi>>(1<<w));
					nsol1 += ((int)nod1.sol>>(1<<w));
				}
				else
				{
					nchi1 += ((int)nod1.chi<<wshlut[w][i]);
					nsol1 += ((int)nod1.sol<<wshlut[w][i]);
				}
				for(j=0;j<8;j++)
				{
					if (!(nod1.chi&(1<<j))) continue;
						  if (w == 0) onod[((i&6)<<2)              + (j^1)] = ((octv_t *)loct->nod.buf)[popcount[nod1.chi&((1<<j)-1)] + nod1.ind];
					else if (w == 1) onod[((i&4)<<2) + ((i&1)<<3) + (j^2)] = ((octv_t *)loct->nod.buf)[popcount[nod1.chi&((1<<j)-1)] + nod1.ind];
					else             onod[((i&3)<<3)              + (j^4)] = ((octv_t *)loct->nod.buf)[popcount[nod1.chi&((1<<j)-1)] + nod1.ind];
				}
			}
			nchi0 = 0; nsol0 = 0;
			for(i=0;i<4;i++)
			{
				nchi0 += (((nchi1& (255<<(i<<3)))!= 0)<<windex[w][i]);
				nsol0 += (((nsol1|~(255<<(i<<3)))==-1)<<windex[w][i]);
			}
			((octv_t *)loct->nod.buf)[loct->head].chi = nchi0;
			((octv_t *)loct->nod.buf)[loct->head].sol = nsol0;

			n = popcount[nchi0];
			nind0 = bitalloc(loct,&loct->nod,n); loct->nod.num += n;
			((octv_t *)loct->nod.buf)[loct->head].ind = nind0;
			for(j=0,i=0;i<8;i++)
			{
				if (!(nchi0&(1<<i))) continue;
				((octv_t *)loct->nod.buf)[nind0+j].chi = (nchi1>>wshr[w][i]);
				((octv_t *)loct->nod.buf)[nind0+j].sol = (nsol1>>wshr[w][i]);
				n = popcount[((octv_t *)loct->nod.buf)[nind0+j].chi];
				nind1 = bitalloc(loct,&loct->nod,n); loct->nod.num += n;
				((octv_t *)loct->nod.buf)[nind0+j].ind = nind1;
				for(m=0,k=0;k<8;k++)
				{
					if (!(((octv_t *)loct->nod.buf)[nind0+j].chi&(1<<k))) continue;
						  if (w == 0) { ((octv_t *)loct->nod.buf)[nind1+m] = onod[((i&6)<<2)              + k]; }
					else if (w == 1) { ((octv_t *)loct->nod.buf)[nind1+m] = onod[((i&4)<<2) + ((i&1)<<3) + k]; }
					else             { ((octv_t *)loct->nod.buf)[nind1+m] = onod[((i&3)<<3)              + k]; }
					m++;
				}
				j++;
			}
		}

			//shrink (halve) if possible
		while ((loct->lsid > 2) && (!(((w1[0]-1)|(w1[1]-1)|(w1[2]-1))&(loct->nsid>>1))))
		{
			if (popcount[((octv_t *)loct->nod.buf)[loct->head].chi] != 1) break;
			got = 1;
			i = ((octv_t *)loct->nod.buf)[loct->head].ind;
			xorzrangesmall(loct->nod.bit,i,1); loct->nod.num--; //remove node
			((octv_t *)loct->nod.buf)[loct->head] = ((octv_t *)loct->nod.buf)[i];
			loct->lsid--; loct->sid >>= 1; loct->nsid = -loct->sid;
		}
		if (got) anygot = 1;
	} while (got);
	return(anygot);
}

	//KV6 format:
	//   int sig; //0x6c78764b (Kvxl)
	//   int xsiz, ysiz, zsiz;
	//   float xpiv, ypiv, zpiv;
	//   int numvoxs;
	//   kv6data { char b, g, r, a; ushort z; char vis, dir; } [numvoxs];
	//           { Nnnnnnnn--VvvvvvZzzzzzzzzzzzzzzz--------RrrrrrrrGgggggggBbbbbbbb }
	//   int xlen[xsiz];
	//   ushort ylen[xsiz][ysiz];
static void savekv6 (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	FILE *fil;
	surf_t surf, *psurf;
	float f;
	int i, x, y, z, numvoxs, *xlen, x0, y0, z0, x1, y1, z1, vis, oy0, oz0, oy1, oz1;
	short *ylen;

	x0 = 0; y0 = 0; z0 = 0; x1 = loct->sid; y1 = loct->sid; z1 = loct->sid;
	oct_getsolbbox(loct,&x0,&y0,&z0,&x1,&y1,&z1);
	oy0 = y0; oz0 = z0; oy1 = y1; oz1 = z1;
	z0 = oy0; y0 = loct->sid-oz1;
	z1 = oy1; y1 = loct->sid-oz0;

	equivecinit(255);

	xlen = (int *)malloc((x1-x0)<<2); if (!xlen) return;
	ylen = (short *)malloc(((x1-x0)*(y1-y0))<<1); if (!ylen) { free(xlen); return; }

	fil = fopen(filnam,"wb"); if (!fil) { free(ylen); free(xlen); return; }
	i = 0x6c78764b; fwrite(&i,4,1,fil); //Kvxl
	i = x1-x0; fwrite(&i,4,1,fil);
	i = y1-y0; fwrite(&i,4,1,fil);
	i = z1-z0; fwrite(&i,4,1,fil);
	//f = (x1-x0)*.5; fwrite(&f,4,1,fil); //bbox of solid centered
	//f = (y1-y0)*.5; fwrite(&f,4,1,fil);
	//f = (z1-z0)*.5; fwrite(&f,4,1,fil);
	f = (float)loct->sid*.5 - x0; fwrite(&f,4,1,fil); //pivot preserved
	f = (float)loct->sid*.5 - y0; fwrite(&f,4,1,fil);
	f = (float)loct->sid*.5 - z0; fwrite(&f,4,1,fil);

	numvoxs = 0;
	memset(xlen,0,(x1-x0)<<2);
	memset(ylen,0,((x1-x0)*(y1-y0))<<1);

	fwrite(&numvoxs,4,1,fil); //dummy write

	for(x=x0;x<x1;x++)
		for(y=y0;y<y1;y++)
			for(z=oct_findsurfdowny(loct,x,-1,loct->sid-1-y,&psurf);z<loct->sid;z=oct_findsurfdowny(loct,x,z,loct->sid-1-y,&psurf))
			{
				surf = (*psurf);

				vis = 0;
				if (!oct_getsol(loct,x-1,z,loct->sid-1-(y))) vis |= (1<<0);
				if (!oct_getsol(loct,x+1,z,loct->sid-1-(y))) vis |= (1<<1);
				if (!oct_getsol(loct,x,z,loct->sid-1-(y-1))) vis |= (1<<2);
				if (!oct_getsol(loct,x,z,loct->sid-1-(y+1))) vis |= (1<<3);
				if (!oct_getsol(loct,x,z-1,loct->sid-1-(y))) vis |= (1<<4);
				if (!oct_getsol(loct,x,z+1,loct->sid-1-(y))) vis |= (1<<5);

				fputc(surf.b    ,fil);
				fputc(surf.g    ,fil);
				fputc(surf.r    ,fil);
				fputc(128       ,fil);
				fputc((z-z0)&255,fil);
				fputc((z-z0)>>8 ,fil);
				fputc(vis       ,fil);

				fputc(equivec2indmem(((float)surf.norm[0])*(+1.f/64.f),
											((float)surf.norm[2])*(-1.f/64.f),
											((float)surf.norm[1])*(+1.f/64.f)),fil);

				numvoxs++; xlen[x-x0]++; ylen[(x-x0)*(y1-y0)+y-y0]++;
			}

	fwrite(xlen,(x1-x0)<<2,1,fil);
	fwrite(ylen,((x1-x0)*(y1-y0))<<1,1,fil);
	fseek(fil,28,SEEK_SET); fwrite(&numvoxs,4,1,fil);
	fclose(fil);

	free(ylen); free(xlen);
}

#if 0
02/24/2012: KVO format, described in pseudo-C code. (assume: int=4 bytes; all fields Little Endian)

	fopen(..)

	int id;       //must be: 'KVOx' or 0x784f564b (Little Endian)
	char version; //must be 0x01
	char lsid;    //log(2) of octree side. sid=(1<<lsid) (Ex.: use 10 for 1024^3)

		//bit 0: pos&ori usage: 0:pivot point (for kvx/kv6 sprite), 1:camera point (for vxl world) //use edgeissol to determine!
		//bit 1: surface voxel color format:
		//    0:log2(paltabn)-bit palette (LUT max entries is 4096)
		//    1:24-bit RGB
		//bit 3-2: surface normal format (supports loadkvo only?)
		//  00:not stored
		//  01:equivec (see equivecbits for bit size; see KV6 format for derivation)
		//  10:24-bit signed char x, y, z;
		//bit 4: 1:store surface voxel alpha (8-bit) (supports loadkvo only?)
		//bit 5: 1:store surface voxel tex   (8-bit) (supports loadkvo only?)
		//bit 6: 1:use tail compression; store 32-bit ptr's (not strict bfs order) (not implemented)
		//bit 8-7: 0:store chi&~sol for ls>0, then sol for all. 1:store chi's for all, 2:store chi's for all, then sol (comp)
	short flags;
	char edgeiswrap; //bits 5-0: 0=air/sol, 1=wrap
	char edgeissol;  //bits 5-0: if (!edgeiswrap) { 0=air; 1=sol; } else reserved;
	char equivecbits; //# bits in equivec normal (if used)
	char reserved;

		//Coordinate system is: x=right, y=down, z=forward
	float px, py, pz; //pos of default view; voxel coords (i.e. 0..(1<<lsid)-1)
	float rx, ry, rz; //  right unit vector of default view
	float dx, dy, dz; //   down unit vector of default view
	float fx, fy, fz; //forward unit vector of default view
	float suggdia; //suggested diameter for rendering; usually: (float)(1<<lsid) but may differ for animations

	int octpren; //number of nodes in chi table
	int sur.num; //number of octree leaves (surface voxels); not needed but useful as guess for alloc
	int nod.num; //number of octree nodes after reconstruct; not needed but useful as guess for alloc

		//bits 8-7:description for save mode 0 here: (obsolete!)
	char chi[octpren];           //chi bytes,  BFS order, big to small (ls>0 only). 0=no child, 1=has child
	char sol[popcount(~chi[*])]; //sol bits in BFS order, big to small, 1 bit per chi=0. sol/air (surfs calced later)
	//flush any extra bits to next byte boundary, i.e.: bitptr = (bitptr+7)&~7;

	if (!(flags&2)) //Palette (if applicable)
	{
		short paltabn; unsigned char bgrcol[paltabn][3]; //blue:[][0], green:[][1], red:[][2], range:{0-255}
	}

		//surface voxel data: colors and normals (optional)
	for(i=0;i<sur.num;i++) //visit all surface voxels (solid voxels next to air) in octree order..
	{
		if (!(flags&2)) { log2(paltabn)_bits palind; } //color:Paletted
					  else { unsigned char b, g, r;     } //color:24-bit RGB
		switch((flags>>2)&7) //normal:
		{
			case 0: /*ignore normal*/ break;
			case 1: equivecbits equivec; break;
			case 2: signed char norm[3]; break;
		}
		if (flags&16) unsigned char alpha; //alpha value
		if (flags&32) unsigned char tex //texture index
	}

	fclose(..);

#endif

	//See SVO_COMP.KC for derivation
static const char hsollut[116] = //log2(n),0,..,n-1
{
	0,0,
	1,0,1, 1,0,2, 1,0,3, 1,0,4, 1,0,5, 1,0,7, 1,0,8, 1,0,10, 1,0,11, 1,0,12, 1,0,13,
	1,0,14, 1,0,16, 1,0,17, 1,0,19, 1,0,21, 1,0,23, 1,0,32, 1,0,34, 1,0,35, 1,0,42, 1,0,43,
	2,0,1,8,9, 2,0,1,32,33, 2,0,2,4,6, 2,0,2,16,18, 2,0,4,16,20, 2,0,8,32,40,
	3,0,1,8,9,32,33,40,41, 3,0,2,4,6,16,18,20,22,
};
static const char hsolind[256] =
{
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 5, 0, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0, 5, 0, 0, 8, 5, 2, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0,11, 0,11, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0,11,14,11, 0, 0, 2, 0,
	0,38, 0,38,0,38,41,38,50,107,47,88,44,83,41,38,
	0, 0, 0, 0, 0, 0, 2, 0,17,78,14,11, 8, 5, 2, 0,
	0, 0, 0, 0, 0, 0,20,20, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0,53,53,65,62,98,93, 0,56,53,53,59,56,73,53,
	0, 0, 0, 0, 0,23,20,20, 0, 5, 0, 0, 0, 5, 0, 0,
	0, 0, 0, 0,26,23,68,20, 0, 5, 0, 0, 8, 5, 2, 0,
	0, 0, 0,29, 0, 0,20,20, 0,11, 0,11, 0, 0, 0, 0,
	0, 0,32,29, 0, 0,68,20, 0,11,14,11, 0, 0, 2, 0,
	0,35, 0,29, 0,23,20,20, 0,78, 0,11, 0, 5, 0, 0,
	0, 0, 0, 0, 0, 0, 2, 0, 0, 5, 0, 0, 0, 0, 0, 0,
};

	//------------------------------------------------------------
	//07/18/2010: NOTE source models were 5-mip KVX; converted from HRP by Devan
	//
	//                   kv6 kvx1mip    kvo kv6zip kvxzip kvozip
	//               ------- ------- ------ ------ ------ ------
	//0692_knife       99288   26176  14224  26075  11292   9171
	//1158_wiaderkob 1077364  376543 169895 224431 117772  66248
	//2282            748666  279360  60311 209869 116243  35647
	//4407_robotmouse 306708  117302  44310  93035  48478  27443
	//4562_doggy      305460  116592  42911  90414  49231  25366
	//922             307012  120842  33021  93255  48269  17102
	//caco            113804   48843  11630  23404  11195   5123
	//duke_stand      669762  220385 109975 245690 120125  60076
	//enforcer_walk1  987566  389378 157127 338132 196425  93589
	//octa_move1      814876  305706 134344 311672 178442  87857
	//pigtank1975    3854356 1376587 641405 885433 525100 240750
	//pig_shot1       758656  274802 123827 257446 143023  65518
	//reconcar1960    782328  326706 119738 183278 111579  49152
	//trooper_walk1  1079648  412565 174066 405456 230402 110240
	//
	//trike                   333399  56224         70839  21422
	//rocketgun2      254862          32234  55808         16286
	//untitled(vxl)      12425384  16765996    3288588   2297266

#define SAVEKVO_PRINTF 1
typedef struct
{
	FILE *fil;
	oct_t *loct;
	int val, cnt, ls, pass;
		//pass:
		//savmode 0: always requires slow surface search at load!
		// 0 chi&~sol:ls-1..1, bytes (recurse on interesting nodes (some sol + some air)
		// 1 sol/air :ls-1..1, comp (determine air/sol of nonleaf pure nodes)
		// 1 sol     :      0, bytes (uses same code as above because all chi's are 0 at ls==0)
		//savmode 1: ~7% bigger than savmode 0; loads ~5x faster! Hollow interior :/
		// 0 chi     :ls-1..1, bytes
		// 1 chi     :      0, bytes
		//savmode 2:
		// 0 chi     :ls-1..1, bytes
		// 1 chi     :      0, bytes
		// 2 sol     :ls-1..0, comp (nicely compressed using hsolind[]/hsollut[])
	int savmode;
} gsav_t;
	//Save obsolete KVO/KVS formats only:
static void save_recur (gsav_t *sav, octv_t *inode, int ls)
{
	octv_t *onode;
	int i, iup;

	if (!sav->savmode)
	{
		i = (~inode->sol)&inode->chi; //visit child w/ >0air & >0sol; skips pure air/sol
		if (ls == sav->ls)
		{
				//Nonleaf:write all child visit bits to file
			if (!sav->pass) { fputc(i,sav->fil); return; }

				//Leaf:write only if non surface
			for(i^=((1<<8)-1);i;i^=iup) { iup = (-i)&i; if (inode->sol&iup) { sav->val += (1<<sav->cnt); } sav->cnt++; }
			if (sav->cnt >= 8) { fputc(sav->val&255,sav->fil); sav->val >>= 8; sav->cnt -= 8; }
			return;
		}
	}
	else
	{
		if (ls == sav->ls) { fputc(inode->chi,sav->fil); return; }
		i = inode->chi;
	}
	onode = &((octv_t *)sav->loct->nod.buf)[inode->ind];
	for(;i;i^=iup) { iup = (-i)&i; save_recur(sav,&onode[popcount[inode->chi&(iup-1)]],ls-1); }
}

static int visitnodes_call (oct_t *loct, int lstop, int (*myfunc)(oct_t *loct, int ptr, int par, void *mydat), void *mydat)
{
	int ls, ptr, par, stki[OCT_MAXLS], stkn[OCT_MAXLS];

	ls = loct->lsid; stki[ls] = loct->head; stkn[ls] = 1;
	while (1)
	{
		ptr = stki[ls]; stki[ls]++; stkn[ls]--; //2sibly
		if (ls == lstop) { if (myfunc(loct,ptr,par,mydat) < 0) return(-1); }
		if (ls >  lstop) { ls--; stki[ls] = ((octv_t *)loct->nod.buf)[ptr].ind; stkn[ls] = popcount[((octv_t *)loct->nod.buf)[ptr].chi]; par = ptr; } //2child
		while (stkn[ls] <= 0) { ls++; if (ls >= loct->lsid) return(0); } //2parent
	}
}

static int savegetcnt_cb (oct_t *loct, int ptr, int par, void *_) { gsav_t *sav = (gsav_t *)_; sav->cnt++; return(0); }
static int savechi_cb (oct_t *loct, int ptr, int par, void *_)
{
	octv_t *inode;
	gsav_t *sav = (gsav_t *)_;

	inode = &((octv_t *)sav->loct->nod.buf)[ptr];

#if (SAVEKVO_PRINTF != 0)
	printf("ls=%d,chi:0x%02x\n",sav->ls,inode->chi);
#endif
	fputc(inode->chi,sav->fil);
	return(0);
}

static int savesol_cb (oct_t *loct, int ptr, int par, void *_)
{
	octv_t *inode;
	int i, j, k, ln, n;
	gsav_t *sav = (gsav_t *)_;

	inode = &((octv_t *)sav->loct->nod.buf)[ptr];

	if (sav->ls)
	{
#if (SAVEKVO_PRINTF != 0)
		printf("ls=%d,sol:0x%02x\n",sav->ls,inode->sol);
#endif
		fputc(inode->sol,sav->fil); return(0);
	}

	i = hsolind[inode->chi]; ln = hsollut[i]; n = (1<<ln);
	for(k=n-1;k>=0;k--)
	{
		j = hsollut[i+k+1];
		if ((inode->sol&(~inode->chi)) ==  j     ) {                break; }
		if ((inode->sol|  inode->chi ) == (j^255)) { k ^= (n<<1)-1; break; }
	}
#if (SAVEKVO_PRINTF != 0)
	if (k < 0) { printf("Octree corrupt error: chi=0x%02x && sol =0x%02x\n",inode->chi,inode->sol); }
	printf("ls=%d,sol:0x%02x (%d/%d)\n",sav->ls,inode->sol,k,1<<(ln+1));
#endif
	sav->val += (k<<sav->cnt); sav->cnt += ln+1;
	if (sav->cnt >= 8) { fputc(sav->val&255,sav->fil); sav->val >>= 8; sav->cnt -= 8; }

	return(0);
}

	//Callback functions for visitnodes_call() for grabbing palette, then saving colors in later pass
static int savegetpal_cb (oct_t *loct, int ptr, int par, void *_) { surf_t *psurf = &((surf_t *)loct->sur.buf)[ptr]; return(palget((*(int *)&psurf->b)&0xffffff)); }
typedef struct { int colmode, palval, palcnt, palbits; FILE *fil; } palwrite_t;
static int savecol_cb (oct_t *loct, int ptr, int par, void *_)
{
	int i, col;
	surf_t *psurf = &((surf_t *)loct->sur.buf)[ptr];
	palwrite_t *pw = (palwrite_t *)_;

	col = (*(int *)&psurf->b)&0xffffff;
		//dump on overflow or changing mode
	if (!pw->colmode)
	{
		i = palget(col);
#if (SAVEKVO_PRINTF != 0)
		printf("col:%d\n",i);
#endif
		pw->palval += (i<<pw->palcnt);
		pw->palcnt += pw->palbits;
		while (pw->palcnt >= 8) { fputc(pw->palval&255,pw->fil); pw->palval >>= 8; pw->palcnt -= 8; }
	}
	else
	{
#if (SAVEKVO_PRINTF != 0)
		printf("col:0x%06x\n",col);
#endif
		fwrite(&col,3,1,pw->fil);
	}

	return(0);
}

static void savekvo (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	gsav_t sav;
	surf_t *psurf;
	palwrite_t pw;
	FILE *fil;
	float f;
	int i, octpren, edgeiswrap, edgeissol;

	fil = fopen(filnam,"wb"); if (!fil) return;
	i = 0x784f564b; fwrite(&i,4,1,fil); //KVOx

	fputc(0x01,fil); //version 1
	fputc(loct->lsid,fil);
	i = 0; fwrite(&i,2,1,fil); //flags

	fputc(loct->edgeiswrap,fil); //edgeiswrap
	fputc(loct->edgeissol,fil); //edgeissol
	i = 0; fwrite(&i,2,1,fil); //reserved

	fwrite(rpos,sizeof(point3d),1,fil);
	fwrite(rrig,sizeof(point3d),1,fil);
	fwrite(rdow,sizeof(point3d),1,fil);
	fwrite(rfor,sizeof(point3d),1,fil);

	f = (float)(1<<loct->lsid);
	fwrite(&f,4,1,fil); //suggested diameter

	fwrite(&i,4,1,fil); //dummy for octpren
	fwrite(&loct->sur.num,4,1,fil);
	fwrite(&loct->nod.num,4,1,fil);

	i = toupper(filnam[max(strlen(filnam)-1,0)]); sav.savmode = (i == 'S') + (i == 'N')*2;

	sav.fil = fil; sav.loct = loct; sav.cnt = 0; sav.val = 0;
	if (sav.savmode < 2)
	{
			//Write upper levels of octree, 1 byte at a time; octpren = # non-leaf nodes
		sav.pass = 0; i = ftell(fil);
		for(sav.ls=loct->lsid-1;sav.ls> 0;sav.ls--) save_recur(&sav,&((octv_t *)loct->nod.buf)[loct->head],loct->lsid-1); //saving breadth-first is annoying :/
		octpren = ftell(fil)-i;

		sav.pass = 1;
		if (!sav.savmode) { sav.ls = loct->lsid-1; } else { sav.ls = 0; }
		for(;sav.ls>=0;sav.ls--) save_recur(&sav,&((octv_t *)loct->nod.buf)[loct->head],loct->lsid-1); //saving breadth-first is annoying :/
	}
	else
	{
		for(sav.ls=loct->lsid-1;sav.ls> 0;sav.ls--) { visitnodes_call(loct,sav.ls+1,savegetcnt_cb,&sav); } octpren = sav.cnt; sav.cnt = 0;
		for(sav.ls=loct->lsid-1;sav.ls>=0;sav.ls--) { visitnodes_call(loct,sav.ls+1,savechi_cb   ,&sav); }
		for(sav.ls=loct->lsid-1;sav.ls>=0;sav.ls--) { visitnodes_call(loct,sav.ls+1,savesol_cb   ,&sav); }
	}
	while (sav.cnt > 0) { fputc(sav.val&255,fil); sav.val >>= 8; sav.cnt -= 8; } //flush bits to next byte boundary

		 //if (<= 4096 unique colors) pw.colmode = 0; else pw.colmode = 1;
	palreset(); pw.colmode = 0;
	if (!visitnodes_call(loct,0,savegetpal_cb,0))
	{
		pw.colmode = 0;
		fwrite(&paltabn,2,1,fil);
		for(i=0;i<paltabn;i++)
		{
#if (SAVEKVO_PRINTF != 0)
			printf("pal(%d):0x%06x\n",i,paltab[i].rgb);
#endif
			fputc((paltab[i].rgb    )&255,fil);
			fputc((paltab[i].rgb>> 8)&255,fil);
			fputc((paltab[i].rgb>>16)&255,fil);
		}
		pw.palbits = bsr(max(paltabn-1,1))+1;
	} else pw.colmode = 1;
	pw.palval = 0; pw.palcnt = 0; pw.fil = fil;
	visitnodes_call(loct,0,savecol_cb,&pw);
	if (pw.palcnt) fputc(pw.palval&255,fil); //flush bits

	i = ftell(fil);
	fseek(fil, 6,SEEK_SET); loct->flags = (loct->flags&~2)|(pw.colmode<<1)|(sav.savmode<<7); fwrite(&loct->flags,2,1,fil); //write flags
	fseek(fil,64,SEEK_SET); fwrite(&octpren,4,1,fil);
	fseek(fil, i,SEEK_SET);

	fclose(fil);
}

void oct_save (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	int i;

	i = strlen(filnam);
	if ((i >= 4) && (!stricmp(&filnam[i-4],".kvo"))) { savekvo(loct,filnam,rpos,rrig,rdow,rfor); return; }
	if ((i >= 4) && (!stricmp(&filnam[i-4],".kvs"))) { savekvo(loct,filnam,rpos,rrig,rdow,rfor); return; }
	if ((i >= 4) && (!stricmp(&filnam[i-4],".kvn"))) { savekvo(loct,filnam,rpos,rrig,rdow,rfor); return; }
	if ((i >= 4) && (!stricmp(&filnam[i-4],".kv6"))) { savekv6(loct,filnam,rpos,rrig,rdow,rfor); return; }
}
void oct_save (oct_t *loct, char *filnam, dpoint3d *rpos, dpoint3d *rrig, dpoint3d *rdow, dpoint3d *rfor)
{
	point3d fp, fr, fd, ff;
	fp.x = rpos->x; fp.y = rpos->y; fp.z = rpos->z;
	fr.x = rrig->x; fr.y = rrig->y; fr.z = rrig->z;
	fd.x = rdow->x; fd.y = rdow->y; fd.z = rdow->z;
	ff.x = rfor->x; ff.y = rfor->y; ff.z = rfor->z;
	oct_save(loct,filnam,&fp,&fr,&fd,&ff);
}

//--------------------------------------------------------------------------------------------------

static void oct_surf_normalize (surf_t *psurf)
{
	unsigned char *uptr;
	int t, bb, gg, rr, cnt;
	float dx, dy, dz;

	bb = psurf->b; gg = psurf->g; rr = psurf->r; cnt = 256; t = psurf->tex;
	do
	{
		//if (oct_usefilter != 3) uptr = (unsigned char *)((t&255)*4 + tiles[tiles[0].ltilesid+1].f);
		//                   else
		uptr = (unsigned char *)((t&15)*4 + ((t>>4)&15)*tiles[tiles[0].ltilesid+1].p + tiles[tiles[0].ltilesid+1].f);
			//surf->b(old) = cptr[0] * surf->b(new) * 2.0/256.0;
		psurf->b = min(max(bb*(256.0/2.0) / max(uptr[0],1),0),255);
		psurf->g = min(max(gg*(256.0/2.0) / max(uptr[1],1),0),255);
		psurf->r = min(max(rr*(256.0/2.0) / max(uptr[2],1),0),255);

		dx = ((float)uptr[0]) * (float)psurf->b * (2.0/256.0) - bb;
		dy = ((float)uptr[1]) * (float)psurf->g * (2.0/256.0) - gg;
		dz = ((float)uptr[2]) * (float)psurf->r * (2.0/256.0) - rr;
		if (dx*dx + dy*dy + dz*dz < 16*16) break;
		cnt--; if (cnt < 0) break;

		t = (t-1)&255;
	} while (1);
	psurf->tex = t;
}
void oct_normalizecols (oct_t *loct)
{
	typedef struct { octv_t *ptr; int j; } stk_t;
	stk_t stk[OCT_MAXLS];
	surf_t *psurf;
	octv_t *ptr;
	int i, j, ls, ind;

	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
#else
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
#endif
	}

	ls = loct->lsid-1; ptr = &((octv_t *)loct->nod.buf)[loct->head]; j = 8-1;
	while (1)
	{
		i = (1<<j); if (!(ptr->chi&i)) goto tosibly;

		if (ls <= 0)
		{
			ind = popcount[ptr->chi&(i-1)] + ptr->ind; psurf = &((surf_t *)loct->sur.buf)[ind];
			oct_surf_normalize(psurf);
			if (oct_usegpu)
			{
#if (GPUSEBO == 0)
				glTexSubImage2D(GL_TEXTURE_2D,0,(ind&((loct->gxsid>>1)-1))<<1,ind>>(loct->glysid-1),2,1,GL_RGBA,GL_UNSIGNED_BYTE,(void *)&((surf_t *)loct->sur.buf)[ind]);
#else
				memcpy(&loct->gsurf[ind],psurf,loct->sur.siz);
#endif
			}
			goto tosibly;
		}

		stk[ls].ptr = ptr; stk[ls].j = j; ls--; //2child
		ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; j = 8-1;
		continue;

tosibly:
		j--; if (j >= 0) continue;
		do { ls++; if (ls >= loct->lsid) goto break2; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr;
	}
break2:;
}

//--------------------------------------------------------------------------------------------------
	//See VOX_MOI.KC for derivation
void oct_getmoi (oct_t *loct, double *mas, double *cx, double *cy, double *cz, double *ixx, double *iyy, double *izz, double *ixy, double *ixz, double *iyz)
{
	typedef struct { octv_t *ptr; int j; } stk_t;
	stk_t stk[OCT_MAXLS];
	octv_t *ptr;
	double f;
	static const __int64 swwlut[OCT_MAXLS] = //swwlut[ls] = ((((2^ls*2 - 3)*2^ls) + 1)*2^(ls*3))/6;  NOTE:swwlut[14] >= 2^64
		{ 0,4,224,8960,317440,10665984,349569024,11319377920,364359188480,11693786660864,374750392090624,12000804344954880,384166442167173120,12295577674285318000,0,0};
	static const __int64 swlut[OCT_MAXLS] = //swlut[ls] = (2^(ls*4-1)) - (2^(ls*3-1));
		{ 0,4,96,1792,30720,507904,8257536,133169152,2139095040,34292629504,549218942976,8791798054912,140703128616960,2251524935778304,36026597995708416,576443160117379070};
	__int64 sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, szz = 0, sxy = 0, sxz = 0, syz = 0;
	__int64 qx, qy, qz, sw = 0, sww = 0, swx = 0, swy = 0, swz = 0;
	int i, j, k, ls, s, x, y, z, xx, yy, zz, sn = 0;
	#define FASTMOI 1

	ls = loct->lsid-1; s = (1<<ls); ptr = &((octv_t *)loct->nod.buf)[loct->head]; x = 0; y = 0; z = 0; j = 8-1;
	while (1)
	{
		if (!ls) //optimization: calculates all 8 leaf nodes simultaneously
		{
			k = ptr->sol; i = popcount[k]; sn += i;
			j = popcount[k&0xaa]; xx = i*x + j; sx += xx; sxx += (xx+j)*x + j;    //Be careful with j when moving these!
			j = popcount[k&0xcc]; yy = i*y + j; sy += yy; syy += (yy+j)*y + j; sxy += xx*y + j*x + popcount[k&0x88];
			j = popcount[k&0xf0]; zz = i*z + j; sz += zz; szz += (zz+j)*z + j; sxz += xx*z + j*x + popcount[k&0xa0];
																									 syz += yy*z + j*y + popcount[k&0xc0];
			j = 0; goto tosibly;
		}

		i = (1<<j); if (!((ptr->chi|ptr->sol)&i)) goto tosibly; //skip pure air
		k = -s-s;
		x &= k; if (j&1) x += s;
		y &= k; if (j&2) y += s;
		z &= k; if (j&4) z += s;

		if (!(ptr->sol&i)) //hybrid
		{
			stk[ls].ptr = ptr; stk[ls].j = j; ls--; s >>= 1; //2child
			ptr = &((octv_t *)loct->nod.buf)[popcount[ptr->chi&(i-1)] + ptr->ind]; j = 8-1;
			continue;
		}

			 //pure solid; process node (x,y,z,ls)
#if (FASTMOI != 0)
		if (ls == 1) //faster because using 32-bit temps
		{
			sw += swlut[1]; sww += swwlut[1]; sn += 8;
			xx = (x<<3); sx += xx; swx += xx; sxx += xx*x; sxy += xx*y;
			yy = (y<<3); sy += yy; swy += yy; syy += yy*y; sxz += xx*z;
			zz = (z<<3); sz += zz; swz += zz; szz += zz*z; syz += yy*z;
		}
		else //general solution for 64-bit precision
		{
			sw += swlut[ls]; sww += swwlut[ls]; k = ls*3; sn += (1<<k); i = s-1;
			qx = (((__int64)x)<<k); sx += qx; swx += qx*i; sxx += qx*x; sxy += qx*y;
			qy = (((__int64)y)<<k); sy += qy; swy += qy*i; syy += qy*y; sxz += qx*z;
			qz = (((__int64)z)<<k); sz += qz; swz += qz*i; szz += qz*z; syz += qy*z;
		}
#else
		for(zz=z;zz<z+s;zz++)
			for(yy=y;yy<y+s;yy++)
				for(xx=x;xx<x+s;xx++)
					{ sn++; sx += xx; sy += yy; sz += zz; sxx += xx*xx; syy += yy*yy; szz += zz*zz; sxy += xx*yy; sxz += xx*zz; syz += yy*zz; }
#endif

tosibly:;
		j--; if (j >= 0) continue;
		do { ls++; s <<= 1; if (ls >= loct->lsid) goto break2; j = stk[ls].j-1; } while (j < 0); //2parent
		ptr = stk[ls].ptr;
	}
break2:;
#if (FASTMOI != 0)
	sx += sw; sxx += swx+sww;
	sy += sw; syy += swy+sww;
	sz += sw; szz += swz+sww;
	sww = sww*3 - sw;
	sxy += (((swx+swy)*2 + sww)>>2);
	sxz += (((swx+swz)*2 + sww)>>2);
	syz += (((swy+swz)*2 + sww)>>2);
#endif
	(*mas) = sn; if (sn > 0.0) f = 1.0/sn; else f = 0.0; (*cx) = sx*f; (*cy) = sy*f; (*cz) = sz*f;
	(*ixx) = syy+szz - ((*cy)*(*cy) + (*cz)*(*cz))*(*mas); (*ixy) = (*cx)*(*cy)*(*mas) - sxy;
	(*iyy) = sxx+szz - ((*cx)*(*cx) + (*cz)*(*cz))*(*mas); (*ixz) = (*cx)*(*cz)*(*mas) - sxz;
	(*izz) = sxx+syy - ((*cx)*(*cx) + (*cy)*(*cy))*(*mas); (*iyz) = (*cy)*(*cz)*(*mas) - syz;
	(*cx) += .5; (*cy) += .5; (*cz) += .5;

		//Correct values for default model: 3.42269e6, 122.157, 125.582, 123.841, 3.49237e10, 3.64484e10, 3.83971e10, -3.1043e9, -3.2001e9, -6.77705e8
	//{ char tbuf[2048]; sprintf(tbuf,"%g\n%g\n%g\n%g\n%g\n%g\n%g\n%g\n%g\n%g",*mas,*cx,*cy,*cz,*ixx,*iyy,*izz,*ixy,*ixz,*iyz); MessageBox(ghwnd,tbuf,prognam,MB_OK); }
}
void oct_getmoi (oct_t *loct, float *mas, float *cx, float *cy, float *cz, float *ixx, float *iyy, float *izz, float *ixy, float *ixz, float *iyz)
{
	double dmas, dcx, dcy, dcz, dixx, diyy, dizz, dixy, dixz, diyz;
	oct_getmoi(loct,&dmas,&dcx,&dcy,&dcz,&dixx,&diyy,&dizz,&dixy,&dixz,&diyz);
	(*mas) = (float)dmas;
	(*cx ) = (float)dcx;  (*cy ) = (float)dcy;  (*cz ) = (float)dcz;
	(*ixx) = (float)dixx; (*iyy) = (float)diyy; (*izz) = (float)dizz;
	(*ixy) = (float)dixy; (*ixz) = (float)dixz; (*iyz) = (float)diyz;
}

//--------------------------------------------------------------------------------------------------

	//tree node vs. kvo surf
typedef struct
{
	int  (*isins  )(brush_t *brush, int x0, int y0, int z0, int ls);
	void (*getsurf)(brush_t *brush, int x0, int y0, int z0, surf_t *surf);
	int mx0, my0, mz0, mx1, my1, mz1;
	int flags;

	int paltabn, palbits, palmask, bitcnt, equivecbits, equivecmask;
	float equiveczmulk, equiveczaddk;
	unsigned char *filptr;
	short filags;
} brush_kvo_t;
static void brush_loadkvo_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_kvo_t *kvo = (brush_kvo_t *)brush;

		//Load color
	if (!(kvo->filags&2)) //Palette
	{
		*(int *)&surf->b = paltab[((*(int *)kvo->filptr)>>kvo->bitcnt)&kvo->palmask].rgb;
		kvo->bitcnt += kvo->palbits; kvo->filptr += (kvo->bitcnt>>3); kvo->bitcnt &= 7;
	}
	else
	{
		*(int *)&surf->b = (((*(int *)kvo->filptr)>>kvo->bitcnt)&0xffffff); kvo->filptr += 3;
	}

		//Load normal
	switch((kvo->filags>>2)&3)
	{
		case 0: surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0; break; //unused
		case 1:
		{
			#define GOLDRAT 0.3819660112501052 //Golden Ratio: 1 - 1/((sqrt(5)+1)/2)
			float fx, fy, fz, r, a;
			int i;

			i = ((*(int *)kvo->filptr)>>kvo->bitcnt)&(kvo->equivecmask);

			fz = (float)i*kvo->equiveczmulk + kvo->equiveczaddk; r = sqrt(1.f - fz*fz);
			a = ((float)i)*(GOLDRAT*PI*2); fx = cos(a)*r; fy = sin(a)*r;
			surf->norm[0] = (signed char)cvttss2si(fx*127.0);
			surf->norm[1] = (signed char)cvttss2si(fy*127.0);
			surf->norm[2] = (signed char)cvttss2si(fz*127.0);

			kvo->bitcnt += kvo->equivecbits; kvo->filptr += (kvo->bitcnt>>3); kvo->bitcnt &= 7;
			break;
		}
		case 2: *(int *)&surf->norm[0] = (((*(int *)kvo->filptr)>>kvo->bitcnt)&0xffffff); kvo->filptr += 3; break;
		case 3: surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0; break; //reserved
	}

		//Load alpha
	if (kvo->filags&16) { *(unsigned char *)&surf->a = (unsigned char)(((*(int *)kvo->filptr)>>kvo->bitcnt)&0xff); kvo->filptr++; }
						else { surf->a = 255; }

		//Load tex
	if (kvo->filags&32) { *(unsigned char *)&surf->tex = (unsigned char)(((*(int *)kvo->filptr)>>kvo->bitcnt)&0xff); kvo->filptr++; }
						else { surf->tex = 0; }
}
static int loadkvo (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	brush_kvo_t brush;
	int i, j, k, iup, nprenodes, nprenodes2, octcn, octvn, bitcnt, *iptr;
	unsigned char *filbuf;

	if (!kzopen(filnam)) return(-1);
	kzseek(0,SEEK_END); i = kztell(); kzseek(0,SEEK_SET);
	filbuf = (unsigned char *)malloc(i); if (!filbuf) { kzclose(); return(-1); }
	kzread(filbuf,i);
	kzclose();

	if (*(int *)&filbuf[0] != 0x784f564b) { free(filbuf); return(-1); } //KVOx

	if (filbuf[4] != 1) { free(filbuf); return(-1); } //version 1
	loct->lsid = (int)filbuf[5];
	brush.filags = *(short *)&filbuf[ 6];

	brush.filptr = &filbuf[12];
	memcpy(rpos,brush.filptr,sizeof(point3d)); brush.filptr += sizeof(point3d);
	memcpy(rrig,brush.filptr,sizeof(point3d)); brush.filptr += sizeof(point3d);
	memcpy(rdow,brush.filptr,sizeof(point3d)); brush.filptr += sizeof(point3d);
	memcpy(rfor,brush.filptr,sizeof(point3d)); brush.filptr += sizeof(point3d);
	brush.filptr += sizeof(float); //suggdia

	nprenodes = *(int *)brush.filptr; brush.filptr += 4;
	octcn     = *(int *)brush.filptr; brush.filptr += 4; //not needed; used only for guess at alloc
	octvn     = *(int *)brush.filptr; brush.filptr += 4; //not needed; used only for guess at alloc

	oct_new(loct,loct->lsid,loct->tilid,(octvn>>2)+octvn,(octcn>>2)+octcn,0);

	  //NOTE:must copy edgeis* AFTER oct_new()!
	loct->edgeiswrap = filbuf[ 8]; //edgeiswrap
	loct->edgeissol  = filbuf[ 9]; //edgeissol
	if (loct->edgeissol) loct->flags |= 1;

	nprenodes2 = 1;
	for(i=0;i<nprenodes;i++)
	{
		((octv_t *)loct->nod.buf)[i].chi = brush.filptr[i]; ((octv_t *)loct->nod.buf)[i].sol = 0; ((octv_t *)loct->nod.buf)[i].mrk = 0; ((octv_t *)loct->nod.buf)[i].mrk2 = 0;
		((octv_t *)loct->nod.buf)[i].ind = nprenodes2; nprenodes2 += popcount[((octv_t *)loct->nod.buf)[i].chi]; //Generate ind's from chi's
	}

	if (!(brush.filags&384)) //if (non save surface format)
	{
		memset4(&((octv_t *)loct->nod.buf)[nprenodes],0,(nprenodes2-nprenodes)*loct->nod.siz);
		brush.filptr += nprenodes;

		bitcnt = 0; iptr = (int *)brush.filptr;
		for(j=0;j<nprenodes2;j++)
		{
			i = ((octv_t *)loct->nod.buf)[j].chi^((1<<8)-1);
			for(;i;i^=iup,bitcnt++) { iup = (-i)&i; if (iptr[bitcnt>>5]&(1<<bitcnt)) ((octv_t *)loct->nod.buf)[j].sol += iup; }
		}
		//NOTE: chi's of ls==0 calculated inside oct_updatesurfs()

		brush.filptr += ((bitcnt+7)>>3);
	}
	else
	{
		j = 1; //Must skip 0 for GPU shaders which use index 0 as discard
		for(i=nprenodes;i<nprenodes2;i++)
		{
			((octv_t *)loct->nod.buf)[i].chi = brush.filptr[i]; ((octv_t *)loct->nod.buf)[i].sol = brush.filptr[i]; ((octv_t *)loct->nod.buf)[i].mrk = 0; ((octv_t *)loct->nod.buf)[i].mrk2 = 0;
			((octv_t *)loct->nod.buf)[i].ind = j; j += popcount[((octv_t *)loct->nod.buf)[i].chi]; //Generate ind's from chi's
		}
		brush.filptr += nprenodes2;

		if ((brush.filags&384) == 256)
		{
			int m, n, chi, ln;

			bitcnt = 0;
			for(m=0;m<nprenodes2;m++)
			{
				if (m < nprenodes) { ((octv_t *)loct->nod.buf)[m].sol = brush.filptr[m]; bitcnt += 8; continue; }

				chi = ((octv_t *)loct->nod.buf)[m].chi;
				i = hsolind[chi]; ln = hsollut[i]; n = (1<<ln);

				k = (((*(int *)&brush.filptr[bitcnt>>3])>>(bitcnt&7)) & ((n<<1)-1));
				if (k < n) { k = hsollut[ k            +i+1]    ; }
						else { k = hsollut[(k^((n<<1)-1))+i+1]^255; }
				((octv_t *)loct->nod.buf)[m].sol = (k|chi);

				bitcnt += ln+1;
			}

			brush.filptr += ((bitcnt+7)>>3);
		}

		nprenodes2++; //Must skip 0 for GPU shaders which use index 0 as discard
	}

	loct->nod.num = nprenodes2; loct->nod.ind = nprenodes2;

		 //loct->nod.bit: 0<=?<nprenodes2 are 1's; nprenodes2<=?<loct->nod.mal are 0's
	memset4(loct->nod.bit,-1,(nprenodes2>>5)<<2); loct->nod.bit[nprenodes2>>5] = pow2m1[nprenodes2&31];

	if (!(brush.filags&2)) //Palette
	{
		brush.paltabn = (int)(*(short *)brush.filptr); brush.filptr += 2;
		brush.palbits = bsr(max(brush.paltabn-1,1))+1; brush.palmask = (1<<brush.palbits)-1;
		for(i=0;i<brush.paltabn;i++) { paltab[i].rgb = *(int *)brush.filptr; brush.filptr += 3; }
	}
	else { brush.paltabn = 0; }
	brush.bitcnt = 0;

	brush.equivecbits = 0;
	if (brush.equivecbits)
	{
		brush.equivecmask = (1<<brush.equivecbits)-1;
		brush.equiveczmulk = 2.f / (float)(1<<brush.equivecbits); brush.equiveczaddk = brush.equiveczmulk*.5f - 1.f;
	}

	brush.mx0 = 0; brush.my0 = 0; brush.mz0 = 0; brush.mx1 = loct->sid; brush.my1 = loct->sid; brush.mz1 = loct->sid;
	brush.isins = 0;/*not used*/
	brush.getsurf = brush_loadkvo_getsurf;
	brush.flags = 0;
	if (!(brush.filags&384)) //if (non save surface format)
	{
		oct_updatesurfs(loct,brush.mx0,brush.my0,brush.mz0,brush.mx1,brush.my1,brush.mz1,(brush_t *)&brush,1);
	}
	else
	{
			//Must skip 0 for GPU shaders which use index 0 as discard
		for(i=1;i<j;i++) { brush_loadkvo_getsurf((brush_t *)&brush,0,0,0,&((surf_t *)loct->sur.buf)[i]); }

		loct->sur.num = j; loct->sur.ind = j;
		memset4(loct->sur.bit,-1,(j>>5)<<2); loct->sur.bit[j>>5] = pow2m1[j&31];
	}
	free(filbuf);

	return(0);
}

static int lightvox (int i)
{
	int sh = (((unsigned)i)>>24);
	return((min((((i>>16)&255)*sh)>>7,255)<<16)+
			 (min((((i>>8 )&255)*sh)>>7,255)<< 8)+
			 (min((((i    )&255)*sh)>>7,255)    ));
}

	//Limitations: zs must be multiple of 32
	//Based on pnd3dold.c:octgenmip()
typedef struct { int ys, zs, isor; unsigned int *rbit, *wbit; } genmip_voxbits_t;
static short unshuflut[256] = //unshuflut[i] = (i&1) + ((i&2)<<7) + ((i&4)>>1) + ((i&8)<<6) + ((i&16)>>2) + ((i&32)<<5) + ((i&64)>>3) + ((i&128)<<4); //see NEW/PACKBIT.BAS
{
	0x0000,0x0001,0x0100,0x0101,0x0002,0x0003,0x0102,0x0103,0x0200,0x0201,0x0300,0x0301,0x0202,0x0203,0x0302,0x0303,
	0x0004,0x0005,0x0104,0x0105,0x0006,0x0007,0x0106,0x0107,0x0204,0x0205,0x0304,0x0305,0x0206,0x0207,0x0306,0x0307,
	0x0400,0x0401,0x0500,0x0501,0x0402,0x0403,0x0502,0x0503,0x0600,0x0601,0x0700,0x0701,0x0602,0x0603,0x0702,0x0703,
	0x0404,0x0405,0x0504,0x0505,0x0406,0x0407,0x0506,0x0507,0x0604,0x0605,0x0704,0x0705,0x0606,0x0607,0x0706,0x0707,
	0x0008,0x0009,0x0108,0x0109,0x000a,0x000b,0x010a,0x010b,0x0208,0x0209,0x0308,0x0309,0x020a,0x020b,0x030a,0x030b,
	0x000c,0x000d,0x010c,0x010d,0x000e,0x000f,0x010e,0x010f,0x020c,0x020d,0x030c,0x030d,0x020e,0x020f,0x030e,0x030f,
	0x0408,0x0409,0x0508,0x0509,0x040a,0x040b,0x050a,0x050b,0x0608,0x0609,0x0708,0x0709,0x060a,0x060b,0x070a,0x070b,
	0x040c,0x040d,0x050c,0x050d,0x040e,0x040f,0x050e,0x050f,0x060c,0x060d,0x070c,0x070d,0x060e,0x060f,0x070e,0x070f,
	0x0800,0x0801,0x0900,0x0901,0x0802,0x0803,0x0902,0x0903,0x0a00,0x0a01,0x0b00,0x0b01,0x0a02,0x0a03,0x0b02,0x0b03,
	0x0804,0x0805,0x0904,0x0905,0x0806,0x0807,0x0906,0x0907,0x0a04,0x0a05,0x0b04,0x0b05,0x0a06,0x0a07,0x0b06,0x0b07,
	0x0c00,0x0c01,0x0d00,0x0d01,0x0c02,0x0c03,0x0d02,0x0d03,0x0e00,0x0e01,0x0f00,0x0f01,0x0e02,0x0e03,0x0f02,0x0f03,
	0x0c04,0x0c05,0x0d04,0x0d05,0x0c06,0x0c07,0x0d06,0x0d07,0x0e04,0x0e05,0x0f04,0x0f05,0x0e06,0x0e07,0x0f06,0x0f07,
	0x0808,0x0809,0x0908,0x0909,0x080a,0x080b,0x090a,0x090b,0x0a08,0x0a09,0x0b08,0x0b09,0x0a0a,0x0a0b,0x0b0a,0x0b0b,
	0x080c,0x080d,0x090c,0x090d,0x080e,0x080f,0x090e,0x090f,0x0a0c,0x0a0d,0x0b0c,0x0b0d,0x0a0e,0x0a0f,0x0b0e,0x0b0f,
	0x0c08,0x0c09,0x0d08,0x0d09,0x0c0a,0x0c0b,0x0d0a,0x0d0b,0x0e08,0x0e09,0x0f08,0x0f09,0x0e0a,0x0e0b,0x0f0a,0x0f0b,
	0x0c0c,0x0c0d,0x0d0c,0x0d0d,0x0c0e,0x0c0f,0x0d0e,0x0d0f,0x0e0c,0x0e0d,0x0f0c,0x0f0d,0x0e0e,0x0e0f,0x0f0e,0x0f0f,
};
static void genmip_voxbits (int x, void *vptr)
{
	genmip_voxbits_t *gm = (genmip_voxbits_t *)vptr;
	int i, j, i0, i1, y, z, rxpit, rypit, wxpit, wypit;

	wxpit = gm->zs;        rxpit = (wxpit<<1);
	wypit = gm->ys*gm->zs; rypit = (wypit<<2);
	if (gm->zs >= 16)
	{
		if (gm->isor)
		{
			for(y=0;y<gm->ys;y++)
			{
				i0 = (x<<1)*rxpit + (y<<1)*rypit;
				i1 =  x    *wxpit +  y    *wypit; i1 >>= 4;
				for(z=0;z<gm->zs;z+=16,i0+=32,i1++)
				{
					i = gm->rbit[(i0      )>>5] | gm->rbit[(i0+rxpit      )>>5] |
						 gm->rbit[(i0+rypit)>>5] | gm->rbit[(i0+rxpit+rypit)>>5];
					j = (((i>>1)|i)&0x55555555); j += (j>>15); j = (unshuflut[(j>>8)&255]<<4) + unshuflut[j&255]; //LUT: fastest on Ken's C2Q
					((short *)gm->wbit)[i1] = (short)j;
				}
			}
		}
		else
		{
			for(y=0;y<gm->ys;y++)
			{
				i0 = (x<<1)*rxpit + (y<<1)*rypit;
				i1 =  x    *wxpit +  y    *wypit; i1 >>= 4;
				for(z=0;z<gm->zs;z+=16,i0+=32,i1++)
				{
					i = gm->rbit[(i0      )>>5] & gm->rbit[(i0+rxpit      )>>5] &
						 gm->rbit[(i0+rypit)>>5] & gm->rbit[(i0+rxpit+rypit)>>5];
					j = (((i>>1)&i)&0x55555555); j += (j>>15); j = (unshuflut[(j>>8)&255]<<4) + unshuflut[j&255]; //LUT: fastest on Ken's C2Q
					((short *)gm->wbit)[i1] = (short)j;
				}
			}
		}
	}
	else
	{
		if (gm->isor)
		{
			for(y=0;y<gm->ys;y++)
			{
				i0 = (x<<1)*rxpit + (y<<1)*rypit;
				i1 =  x    *wxpit +  y    *wypit;
				for(z=0;z<gm->zs;z++,i0+=2,i1++)
				{
					i = 0;
					j = i0            ; if (gm->rbit[j>>5]&(3<<j)) i = 1;
					j = i0+rxpit      ; if (gm->rbit[j>>5]&(3<<j)) i = 1;
					j = i0+rypit      ; if (gm->rbit[j>>5]&(3<<j)) i = 1;
					j = i0+rxpit+rypit; if (gm->rbit[j>>5]&(3<<j)) i = 1;
					if (!i) gm->wbit[i1>>5] &=~(1<<i1);
						else gm->wbit[i1>>5] |= (1<<i1);
				}
			}
		}
		else
		{
			for(y=0;y<gm->ys;y++)
			{
				i0 = (x<<1)*rxpit + (y<<1)*rypit;
				i1 =  x    *wxpit +  y    *wypit;
				for(z=0;z<gm->zs;z++,i0+=2,i1++)
				{
					i = 1;
					j = i0            ; if (((gm->rbit[j>>5]>>j)&3) != 3) i = 0;
					j = i0+rxpit      ; if (((gm->rbit[j>>5]>>j)&3) != 3) i = 0;
					j = i0+rypit      ; if (((gm->rbit[j>>5]>>j)&3) != 3) i = 0;
					j = i0+rxpit+rypit; if (((gm->rbit[j>>5]>>j)&3) != 3) i = 0;
					if (!i) gm->wbit[i1>>5] &=~(1<<i1);
						else gm->wbit[i1>>5] |= (1<<i1);
				}
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------
	//tree node vs. vxl format
typedef struct
{
	int  (*isins  )(brush_t *, int x0, int y0, int z0, int ls);
	void (*getsurf)(brush_t *, int x0, int y0, int z0, surf_t *);
	int mx0, my0, mz0, mx1, my1, mz1;
	int flags;

	unsigned int *bitor[9], *bitand[9];
	unsigned char **vcolptr;
	int vsid, lvsid;
} brush_vxl_t;
	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_vxl_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	int i, j;
	brush_vxl_t *vxl = (brush_vxl_t *)brush;

		//0  128 256 384 512 640 768 896 1024
		//+---+---+---+---+---+---+---+---+
		//        | x x xx|xxxxxxxxxxxxxxxx
		//
		//0  128 256 384 512
		//+---+---+---+---+
		//        | x x xx|

	y0 -= 256; //arbitrary height adjustment
	if (ls >= 8) return(1);
	if (y0&(-256)) { if (y0 < 0) return(0); else return(2); }
	if (!ls) { i = (x0<<(vxl->lvsid+8)) + (z0<<8) + y0; if (!(vxl->bitor[0][i>>5]&(1<<i))) return(0); return(2); } //<-optimization only
	i = _lrotl(x0,vxl->lvsid+8-ls*3) + _lrotl(z0,8-ls*2) + (y0>>ls); j = (1<<i); i >>= 5;
	if (!(vxl->bitor [ls][i]&j)) return(0);
	if (  vxl->bitand[ls][i]&j ) return(2);
	return(1);
}
static void brush_vxl_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	int z;
	unsigned char *v;
	brush_vxl_t *vxl = (brush_vxl_t *)brush;

	//fixcnt_getsurf++;
	surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0;
	surf->tex = ((rand()*108)>>15);

	*(int *)&surf->b = 0x40404040;
	y0 -= 256; if (y0&(-256)) return; //height adjustment
	v = vxl->vcolptr[(x0<<vxl->lvsid)+z0];
	while (1)
	{
		if (y0 <= v[2]) { *(int *)&surf->b = lightvox(*(int *)&v[(y0-v[1]+1)<<2]); break; }
		if (!v[0]) break; z = v[2]-v[1]-v[0]+2; v += v[0]*4;
		if (y0 < z+v[3]) break;
		if (y0 <  v[3]) { *(int *)&surf->b = lightvox(*(int *)&v[(y0-v[3]  )<<2]); break; }
	}
}

static void loadvxl_genbitbuf (int y, void *vptr)
{
	int x, z;
	unsigned char *v;
	brush_vxl_t *vxl = (brush_vxl_t *)vptr;

	memset4(&vxl->bitor[0][y<<(vxl->lvsid+8-5)],-1,1<<(vxl->lvsid+8-3));
	for(x=0;x<vxl->vsid;x++)
	{
		v = vxl->vcolptr[(y<<vxl->lvsid)+x];
		for(z=0;1;v+=v[0]*4,z=v[3])
		{
			setzrange0(&vxl->bitor[0][((y<<vxl->lvsid)+x)<<(8-5)],z,v[1]);
			if (!v[0]) break;
		}
		v += ((((int)v[2])-((int)v[1])+2)<<2);
	}
}

static int loadvxl (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	dpoint3d dp;
	double d;
	brush_vxl_t vxl;
	genmip_voxbits_t gm;
	unsigned int *isanymal, *rptr, *wptr;
	unsigned char *v, *vbuf;
	int i, j, s, x, y, z;

	if (!kzopen(filnam)) return(-1);
	kzread(&i,4);
	if (i == 0x09072000)
	{
		kzread(&i,4);
		kzread(&j,4); if ((i != j) || (i > 16384)) { kzclose(); return(-1); }
		for(vxl.lvsid=0;(1<<vxl.lvsid)<i;vxl.lvsid++);
		vxl.vsid = (1<<vxl.lvsid); if (vxl.vsid != j) { kzclose(); return(-1); }
		kzread(&dp,24); rpos->x = dp.x; rpos->y = dp.z+256.0; rpos->z = vxl.vsid-dp.y; //camera position
		kzread(&dp,24); rrig->x = dp.x; rrig->y = dp.z; rrig->z = -dp.y; //unit right vector
		kzread(&dp,24); rdow->x = dp.x; rdow->y = dp.z; rdow->z = -dp.y; //unit down vector
		kzread(&dp,24); rfor->x = dp.x; rfor->y = dp.z; rfor->z = -dp.y; //unit forward vector
	}
	else //AoS format w/no header info :/
	{
		kzseek(0,SEEK_SET);
		vxl.lvsid = 9; vxl.vsid = (1<<vxl.lvsid); //AoS format has no header :/
		rpos->x = 256.0; rpos->y = 1024.0; rpos->z = +0.0; //camera position
		rrig->x = 1.0; rrig->y = 0.0; rrig->z = 0.0; //unit right vector
		rdow->x = 0.0; rdow->y = 0.0; rdow->z = 1.0; //unit down vector
		rfor->x = 0.0; rfor->y =-1.0; rfor->z = 0.0; //unit forward vector
	}

		//Allocate huge buffer and load rest of file into it...
	x = kztell(); kzseek(0,SEEK_END); i = kztell()-x; kzseek(x,SEEK_SET);
	vbuf = (unsigned char *)malloc(i); if (!vbuf) { kzclose(); return(-1); }
	kzread(vbuf,i);
	kzclose();

		//~41.1431MB for vsid=1024
	for(j=0,i=vxl.vsid,s=256;s>0;i>>=1,s>>=1)
	{
		if (!j) j += i*i*s; //don't need 2 buffers at full res
			else j += i*i*s*2;
	}
	j = (j>>2)+256;

	isanymal = (unsigned int *)malloc(j);
	if (!isanymal) return(-1);

		//4MB
	vxl.vcolptr = (unsigned char **)malloc(vxl.vsid*vxl.vsid*sizeof(unsigned char *));
	if (!vxl.vcolptr) { free(isanymal); return(-1); }

		//Generate vcolptr[][]
	v = vbuf;
	for(x=vxl.vsid-1;x>=0;x--) //NOTE:can't change for loop order
		for(y=0;y<vxl.vsid;y++) //NOTE:can't change for loop order
		{
			vxl.vcolptr[(y<<vxl.lvsid)+x] = v;
			for(z=0;v[0];v+=v[0]*4,z=v[3]);
			v += ((((int)v[2])-((int)v[1])+2)<<2);
		}

		//Prepare bit buffer pointers
	vxl.bitor[0] = (unsigned int *)((((int)isanymal)+15)&~15);
	htrun(loadvxl_genbitbuf,&vxl,0,vxl.vsid,oct_numcpu); //Generate bit buffer using vcolptr's

		//Generate mips (bitor & bitand)
	j = ((vxl.vsid*vxl.vsid*256)>>3);
	vxl.bitand[0] = vxl.bitor[0]; //NOTE:bitand[0] bitmap is same as bitor[0]; saves 32MB! :)
	for(i=1;i<=8;i++)
	{
		vxl.bitor[i]  = &vxl.bitand[i-1][(j+3)>>2]; j >>= 3;
		vxl.bitand[i] = &vxl.bitor [i  ][(j+3)>>2];

		gm.ys = (vxl.vsid>>i); gm.zs = (256>>i);
		gm.rbit = vxl.bitor [i-1]; gm.wbit = vxl.bitor [i]; gm.isor = 1; htrun(genmip_voxbits,&gm,0,gm.ys,oct_numcpu*(gm.zs>=16)); //NOTE:MT not safe in genmip_voxbits when gm.zs<16!
		gm.rbit = vxl.bitand[i-1]; gm.wbit = vxl.bitand[i]; gm.isor = 0; htrun(genmip_voxbits,&gm,0,gm.ys,oct_numcpu*(gm.zs>=16));
	}

	vxl.isins   = brush_vxl_isins;
	vxl.getsurf = brush_vxl_getsurf;
	vxl.flags = 1;

	oct_new(loct,vxl.lvsid,gtilid,0,0,0);
	loct->flags |= 1;
	loct->edgeissol = 61;
	oct_mod(loct,(brush_t *)&vxl,1+2);

	free(vxl.vcolptr);
	free(isanymal);

	free(vbuf);
	return(0);
}
//--------------------------------------------------------------------------------------------------
	//tree node vs. kv6 format
typedef struct { unsigned char b, g, r, a; unsigned short z; char vis, dir; } kv6vox_t;
typedef struct
{
	int  (*isins  )(brush_t *, int x0, int y0, int z0, int ls);
	void (*getsurf)(brush_t *, int x0, int y0, int z0, surf_t *);
	int mx0, my0, mz0, mx1, my1, mz1;
	int flags;

	unsigned int *bitor[OCT_MAXLS], *bitand[OCT_MAXLS], lsid, xsiz, ysiz, zsiz, xof, yof, zof;
	void **vcolptr;
	int filtyp;
	char *ppal;
} brush_kv6_t;
	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_kv6_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	int i, j;
	brush_kv6_t *kv6 = (brush_kv6_t *)brush;

	if (!ls) { i = (x0<<(kv6->lsid*2)) + (z0<<kv6->lsid) + y0; if (!(kv6->bitor[0][i>>5]&(1<<i))) return(0); return(2); } //<-optimization only
	i = _lrotl(x0,kv6->lsid*2-ls*3) + _lrotl(z0,kv6->lsid-ls*2) + (y0>>ls); j = (1<<i); i >>= 5;
	if (!(kv6->bitor [ls][i]&j)) return(0);
	if (  kv6->bitand[ls][i]&j ) return(2);
	return(1);
}
static void brush_kv6_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	kv6vox_t *v;
	float fx, fy, fz;
	int i;
	brush_kv6_t *kv6 = (brush_kv6_t *)brush;

	//fixcnt_getsurf++;
	x0 -= kv6->xof;
	y0 -= kv6->zof;
	z0 -= kv6->yof;
	switch(kv6->filtyp)
	{
		case 0: //.KV6
			i = x0*kv6->ysiz + (kv6->ysiz-1-z0);
			for(v=(kv6vox_t *)kv6->vcolptr[i];v<(kv6vox_t *)kv6->vcolptr[i+1];v++)
			{
				if (v->z < y0) continue;
				if (v->z > y0) break;

				//i = lightvox(*(int *)&v->b);
				surf->b = v->b;
				surf->g = v->g;
				surf->r = v->r;
				surf->a = v->a;
				surf->tex = 0; //((x0^y0^z0)&63);

				equiind2vec(v->dir,&fx,&fy,&fz);
				surf->norm[0] = (signed char)(fx*+64.0);
				surf->norm[1] = (signed char)(fz*+64.0);
				surf->norm[2] = (signed char)(fy*-64.0);
				return;
			}
			break;
		case 1: //.KVX
			{
			unsigned char *v0, *v1;
			v0 = (unsigned char *)kv6->vcolptr[x0*kv6->ysiz + kv6->ysiz-1-z0];
			v1 = (unsigned char *)kv6->vcolptr[x0*kv6->ysiz + kv6->ysiz-1-z0 + 1];
			for(;v0<v1;v0+=((int)v0[1])+3)
			{
				//char slabztop, slabzleng, slabbackfacecullinfo, col[slabzleng];
				if ((y0 < ((int)v0[0])) || (y0 >= ((int)v0[0])+((int)v0[1]))) continue;

				i = v0[y0-((int)v0[0])+3];
				surf->b = kv6->ppal[i*3+2]*4;
				surf->g = kv6->ppal[i*3+1]*4;
				surf->r = kv6->ppal[i*3+0]*4;
				surf->a = 255;
				surf->tex = 0; //((x0^y0^z0)&63);
				surf->norm[0] = 0;
				surf->norm[1] = 0;
				surf->norm[2] = 0;
				return;
			}
			}
			break;
		case 2: //.VOX
			i = ((unsigned char *)kv6->vcolptr[x0*kv6->ysiz + kv6->ysiz-1-z0])[y0];
			surf->b = kv6->ppal[i*3+2]*4;
			surf->g = kv6->ppal[i*3+1]*4;
			surf->r = kv6->ppal[i*3+0]*4;
			surf->a = 255;
			surf->tex = 0; //((x0^y0^z0)&63);
			surf->norm[0] = 0;
			surf->norm[1] = 0;
			surf->norm[2] = 0;
			return;
	}

	*(int *)&surf->b = 0x80808080;
	surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0;
	surf->tex = 255;
}
static int loadkv6_kvx_vox (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor, int filtyp)
{
	brush_kv6_t kv6;
	kv6vox_t *v, *vbuf;
	surf_t surf, *psurf;
	unsigned int *isanymal, *rptr, *wptr;
	unsigned short *ylen; //xsiz*ysiz*sizeof(short)
	float fx, fy, fz, xpiv, ypiv, zpiv;
	int i, j, s, x, y, z, z0, z1, col, xsiz, ysiz, zsiz, xyzsiz, numvoxs, datell;
	int numbytes;
	short *xyoffset;
	char *cptr, *voxdata, pal[768];

	if (!kzopen(filnam)) return(-1);
	switch(filtyp)
	{
		case 0: //.KV6
			kzread(&i,4); if (i != 0x6c78764b) { kzclose(); return(-1); } //Kvxl
			kzread(&xsiz,4); kzread(&ysiz,4); kzread(&zsiz,4);
			kzread(&xpiv,4); kzread(&ypiv,4); kzread(&zpiv,4);
			kzread(&numvoxs,4);
			break;
		case 1: //.KVX
			kzread(&numbytes,4);
			kzread(&xsiz,4); kzread(&ysiz,4); kzread(&zsiz,4);
			kzread(&i,4); xpiv = ((float)i)*(1.0/256.0);
			kzread(&i,4); ypiv = ((float)i)*(1.0/256.0);
			kzread(&i,4); zpiv = ((float)i)*(1.0/256.0);
			break;
		case 2: //.VOX
			kzread(&xsiz,4); kzread(&ysiz,4); kzread(&zsiz,4);
			xpiv = xsiz*.5; ypiv = ysiz*.5; zpiv = zsiz*.5;
			break;
	}

	i = max(max(xsiz,ysiz),zsiz); if (i <= 0) { kzclose(); return(-1); }
	oct_new(loct,bsr(i-1)+1,gtilid,0,0,0);

	switch(filtyp)
	{
		case 0: //.KV6
			ylen = (unsigned short *)malloc(xsiz*ysiz*sizeof(short)); if (!ylen) { kzclose(); return(-1); }
			kzseek(32+numvoxs*sizeof(kv6vox_t)+(xsiz<<2),SEEK_SET);
			kzread(ylen,xsiz*ysiz*sizeof(short));
			kzseek(32,SEEK_SET);

			equivecinit(255);

			vbuf = (kv6vox_t *)malloc(numvoxs*sizeof(kv6vox_t)); if (!vbuf) { free(ylen); kzclose(); return(-1); }
			kzread(vbuf,numvoxs*sizeof(kv6vox_t));
			break;
		case 1: //.KVX
			xyoffset = (short *)malloc(xsiz*(ysiz+1)*2); if (!xyoffset) { kzclose(); return(-1); }
			voxdata = (char *)malloc(numbytes-24-(xsiz+1)*4-xsiz*(ysiz+1)*2); if (!voxdata) { free(xyoffset); kzclose(); return(-1); }

			kzseek((xsiz+1)*4,SEEK_CUR);
			kzread(xyoffset,xsiz*(ysiz+1)*2);
			kzread(voxdata,numbytes-24-(xsiz+1)*4-xsiz*(ysiz+1)*2);

			kzseek(-768,SEEK_END);
			kzread(pal,768); //0-63
			break;
		case 2: //.VOX
			voxdata = (char *)malloc(xsiz*ysiz*zsiz); //255=transparent
			if (!voxdata) { kzclose(); return(-1); }
			kzread(voxdata,xsiz*ysiz*zsiz);
			kzread(pal,768); //0-63
			break;
	}
	kzclose();

	kv6.xof = max(min((loct->sid>>1)-(int)xpiv,loct->sid-xsiz),0);
	kv6.yof = max(min((loct->sid>>1)-(int)ypiv,loct->sid-ysiz),0);
	kv6.zof = max(min((loct->sid>>1)-(int)zpiv,loct->sid-zsiz),0);

	i = (1<<(loct->lsid*3)); for(j=loct->lsid-1;j>=0;j--) i += (2<<(j*3));
	isanymal = (unsigned int *)malloc((i>>3)+256);
	if (!isanymal) { free(ylen); return(-1); }

	kv6.vcolptr = (void **)malloc((xsiz*ysiz+1)*sizeof(void *));
	if (!kv6.vcolptr) { free(isanymal); free(ylen); return(-1); }

		//Prepare bit buffer pointers
	kv6.bitor[0] = (unsigned int *)((((int)isanymal)+15)&~15);
	j = (1<<(loct->lsid*3-3)); memset16(kv6.bitor[0],0,j);

		//Generate bit buffer & vcolptr[][]
	switch(filtyp)
	{
		case 0: //.KV6
			v = vbuf;
			for(x=0;x<xsiz;x++)
				for(y=0;y<ysiz;y++)
				{
					kv6.vcolptr[x*ysiz+y] = (void *)v;
					for(i=ylen[x*ysiz+y];i>0;i--,v++)
					{
						if (v->vis&16) z = v->z; //air above
						if (v->vis&32) //air below
						{
							j = ((x+kv6.xof)<<(loct->lsid*2)) + ((ysiz-1-y+kv6.yof)<<loct->lsid) + z+kv6.zof;
							setzrange1(&kv6.bitor[0][j>>5],j&31,(j&31)+(v->z+1)-z);
						}
					}
				}
			kv6.vcolptr[xsiz*ysiz] = (void *)v;
			free(ylen);
			break;
		case 1: //.KVX
			cptr = (char *)voxdata;
			for(x=0;x<xsiz;x++)
				for(y=0;y<ysiz;y++)
				{
					kv6.vcolptr[x*ysiz+y] = (void *)cptr; z0 = 0;
					for(i=xyoffset[x*(ysiz+1)+y+1]-xyoffset[x*(ysiz+1)+y];i>0;i-=j)
					{
						//char slabztop, slabzleng, slabbackfacecullinfo, col[slabzleng];
						z1 = ((int)cptr[0])+((int)cptr[1]);
						if (cptr[2]&16) z0 = ((int)cptr[0]);
						j = ((x+kv6.xof)<<(loct->lsid*2)) + ((ysiz-1-y+kv6.yof)<<loct->lsid) + z0+kv6.zof;
						setzrange1(&kv6.bitor[0][j>>5],j&31,(j&31)+z1-z0); //((int)cptr[0]));
						z0 = ((int)cptr[0])+((int)cptr[1]);
						j = ((int)cptr[1])+3; cptr += j;
					}
				}
			kv6.vcolptr[xsiz*ysiz] = (void *)cptr;
			free(xyoffset);
			break;
		case 2: //.VOX
			cptr = (char *)voxdata;
			for(x=0;x<xsiz;x++)
				for(y=0;y<ysiz;y++)
				{
					kv6.vcolptr[x*ysiz+y] = (void *)cptr;
					for(z=0;z<zsiz;z++)
					{
						if (cptr[z] == 255) continue;
						j = ((x+kv6.xof)<<(loct->lsid*2)) + ((ysiz-1-y+kv6.yof)<<loct->lsid) + z+kv6.zof;
						*(int *)&kv6.bitor[0][j>>5] |= (1<<j);
					}
					cptr += zsiz;
				}
			kv6.vcolptr[xsiz*ysiz] = (void *)cptr;
			break;
	}

		//Generate mips (bitor & bitand)
	kv6.bitand[0] = kv6.bitor[0]; //NOTE:bitand[0] bitmap is same as bitor[0]; saves 32MB! :)
	j = (1<<(loct->lsid*3-3));
	for(i=1;i<=loct->lsid;i++)
	{
		genmip_voxbits_t gm;

		xyzsiz = (1<<(loct->lsid-i));
		kv6.bitor[i]  = &kv6.bitand[i-1][(j+3)>>2]; j >>= 3;
		kv6.bitand[i] = &kv6.bitor [i  ][(j+3)>>2];

		gm.ys = xyzsiz; gm.zs = xyzsiz;
		gm.rbit = kv6.bitor [i-1]; gm.wbit = kv6.bitor [i]; gm.isor = 1; htrun(genmip_voxbits,&gm,0,xyzsiz,oct_numcpu*(gm.zs>=16)); //NOTE:MT not safe in genmip_voxbits when gm.zs<16!
		gm.rbit = kv6.bitand[i-1]; gm.wbit = kv6.bitand[i]; gm.isor = 0; htrun(genmip_voxbits,&gm,0,xyzsiz,oct_numcpu*(gm.zs>=16));
	}

	kv6.isins   = brush_kv6_isins;
	kv6.getsurf = brush_kv6_getsurf;
	kv6.flags   = 0;
	kv6.lsid    = loct->lsid; kv6.xsiz = xsiz; kv6.ysiz = ysiz; kv6.zsiz = zsiz;
	kv6.filtyp = filtyp; kv6.ppal = pal;

	oct_mod(loct,(brush_t *)&kv6,1+2);

	free(kv6.vcolptr);
	free(isanymal);
	if (filtyp) free(voxdata);
	return(0);
}

//--------------------------------------------------------------------------------------------------
	//tree node vs. png format
typedef struct
{
	int  (*isins  )(brush_t *, int x0, int y0, int z0, int ls);
	void (*getsurf)(brush_t *, int x0, int y0, int z0, surf_t *);
	int mx0, my0, mz0, mx1, my1, mz1;
	int flags;

	tiletype pic;
	unsigned char *umax[OCT_MAXLS], *umin[OCT_MAXLS];
	int sid, yofs;
} brush_png_t;
	//Returns: 0: node doesn't intersect brush
	//         1: node partially  inside brush
	//         2: node fully      inside brush
static int brush_png_isins (brush_t *brush, int x0, int y0, int z0, int ls)
{
	brush_png_t *png = (brush_png_t *)brush;
	int i, xs, zs;

	if (ls >= 8) return(1);
	y0 -= png->yofs;
	if (y0&(-256)) { if (y0 < 0) return(0); else return(2); }
	z0 = png->sid-1-z0;
	x0 >>= ls; xs = ((png->pic.x+(1<<ls)-1)>>ls); if (x0 >= xs) return(0);
	z0 >>= ls; zs = ((png->pic.y+(1<<ls)-1)>>ls); if (z0 >= zs) return(0);
	i = z0*xs+x0;
	if (y0         >= png->umax[ls][i]) return(2);
	if (y0+(1<<ls) <= png->umin[ls][i]) return(0);
	return(1);
}
static void brush_png_getsurf (brush_t *brush, int x0, int y0, int z0, surf_t *surf)
{
	brush_png_t *png = (brush_png_t *)brush;
	int i;

	surf->norm[0] = 0; surf->norm[1] = 0; surf->norm[2] = 0;
	surf->a = 255; surf->tex = ((y0-png->yofs)%108);

	z0 = png->sid-1-z0;
	if ((x0 < png->pic.x) && (z0 < png->pic.y))
	{
		*(int *)&surf->b = *(int *)(png->pic.p*z0 + (x0<<2) + png->pic.f);
		return;
	}
	*(int *)&surf->b = 0x404040;
}

	//05/28/2012:     ri_2048: ri_4096:
	//---------------------------------
	//kpzload            208.1    791.3
	//oct_init             1.3      1.4
	//extract alpha        4.8     19.5
	//gen min/max mips    25.7     89.9
	//oct_mod()         2785.8   8272.3
	//
static int loadpng (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	brush_png_t png;
	int i, j, ls, x, y, xx, yy, xs, ys, nxs, nys, chi = 0, sol = 0, ind = 0;
	unsigned char cmin, cmax, *rptr, *wptr, xorhei;

	if (filnam[0] == '~') { xorhei =   0; filnam++; } //255 is lo (tomland.png)
						  else { xorhei = 255;           } //255 is hi (default/most others)
	kpzload(filnam,&png.pic.f,&png.pic.p,&png.pic.x,&png.pic.y);
	if (!png.pic.f) return(-1);

	oct_new(loct,bsr(max(png.pic.x,png.pic.y)-1)+1,gtilid,0,0,0);

	loct->flags |= 1;
	loct->edgeissol = 61;

	rpos->x = loct->sid*.5; rpos->y = 64; rpos->z = loct->sid*.5;
	rrig->x = 1.0; rrig->y = 0.0; rrig->z = 0.0;
	rdow->x = 0.0; rdow->y = 1.0; rdow->z = 0.0;
	rfor->x = 0.0; rfor->y = 0.0; rfor->z = 1.0;

		//generate min/max mip tables..
	for(i=0,ls=0;ls<loct->lsid;ls++) { i += ((png.pic.x+(1<<ls)-1)>>ls) * ((png.pic.y+(1<<ls)-1)>>ls) * ((ls!=0)+1); }
	png.umin[0] = (unsigned char *)malloc(i); if (!png.umin[0]) { free((void *)png.pic.f); return(-1); }
	png.umax[0] = png.umin[0]; i = png.pic.x*png.pic.y;
	for(ls=1;ls<loct->lsid;ls++)
	{
		png.umin[ls] = png.umax[ls-1]+i; i = ((png.pic.x+(1<<ls)-1)>>ls) * ((png.pic.y+(1<<ls)-1)>>ls);
		png.umax[ls] = png.umin[ls  ]+i;
	}
	wptr = &png.umin[0][0]; rptr = (unsigned char *)(png.pic.f + 3);
	for(y=0;y<png.pic.y;y++,wptr+=png.pic.x,rptr+=png.pic.p)
		for(x=0;x<png.pic.x;x++)
			wptr[x] = rptr[x<<2]^xorhei;
	nxs = png.pic.x; nys = png.pic.y;
	for(ls=1;ls<loct->lsid;ls++)
	{
		xs = nxs; nxs = ((png.pic.x+(1<<ls)-1)>>ls);
		ys = nys; nys = ((png.pic.y+(1<<ls)-1)>>ls);
		for(y=0;y*2+1<ys;y++)
		{
			i = y*xs*2; j = y*nxs; xx = ((xs-1)>>1);
			for(x=0;x<xx;x++,i+=2)
			{
				rptr = &png.umin[ls-1][i]; png.umin[ls][j+x] = min(min(rptr[0],rptr[1]),min(rptr[xs],rptr[xs+1]));
				rptr = &png.umax[ls-1][i]; png.umax[ls][j+x] = max(max(rptr[0],rptr[1]),max(rptr[xs],rptr[xs+1]));
			}
			if (x < nxs)
			{
				rptr = &png.umin[ls-1][i]; png.umin[ls][j+x] = min(rptr[0],rptr[xs]);
				rptr = &png.umax[ls-1][i]; png.umax[ls][j+x] = max(rptr[0],rptr[xs]);
			}
		}
		if (y < nys)
		{
			for(x=0;x<nxs;x++)
			{
				cmin = 255; cmax = 0;
				for(xx=0;xx<2;xx++)
				{
					if (x*2+xx >= xs) continue;
					cmin = min(cmin,png.umin[ls-1][(y*2)*xs+(x*2+xx)]);
					cmax = max(cmax,png.umax[ls-1][(y*2)*xs+(x*2+xx)]);
				}
				png.umin[ls][y*nxs+x] = cmin;
				png.umax[ls][y*nxs+x] = cmax;
			}
		}
	}

	png.isins   = brush_png_isins;
	png.getsurf = brush_png_getsurf;
	png.flags   = 0;
	png.sid     = loct->sid;
	png.yofs    = (max(loct->sid-256,0)>>1)&-256;
	oct_mod(loct,(brush_t *)&png,1+2);

	rpos->y += png.yofs;

	free(png.umin[0]);
	free((void *)png.pic.f);

	return(0);
}

int oct_load (oct_t *loct, char *filnam, point3d *rpos, point3d *rrig, point3d *rdow, point3d *rfor)
{
	int i, ret;

	i = strlen(filnam);
	if (i >= 4)
	{
			  if (!stricmp(&filnam[i-4],".kvo")) ret = loadkvo        (loct,filnam,rpos,rrig,rdow,rfor);
		else if (!stricmp(&filnam[i-4],".kvs")) ret = loadkvo        (loct,filnam,rpos,rrig,rdow,rfor);
		else if (!stricmp(&filnam[i-4],".kvn")) ret = loadkvo        (loct,filnam,rpos,rrig,rdow,rfor);
		else if (!stricmp(&filnam[i-4],".kv6")) ret = loadkv6_kvx_vox(loct,filnam,rpos,rrig,rdow,rfor,0);
		else if (!stricmp(&filnam[i-4],".kvx")) ret = loadkv6_kvx_vox(loct,filnam,rpos,rrig,rdow,rfor,1);
		else if (!stricmp(&filnam[i-4],".vox")) ret = loadkv6_kvx_vox(loct,filnam,rpos,rrig,rdow,rfor,2);
		else if (!stricmp(&filnam[i-4],".vxl")) ret = loadvxl        (loct,filnam,rpos,rrig,rdow,rfor);
		else if (!stricmp(&filnam[i-4],".png")) ret = loadpng        (loct,filnam,rpos,rrig,rdow,rfor);
		else ret = -1;
	} else ret = -1;

	if (ret < 0)
	{
		char tbuf[1024];
		sprintf(tbuf,"Unable to load file: %s",filnam);
		MessageBox(ghwnd,tbuf,prognam,MB_OK);
		oct_new(loct,6,gtilid,0,0,0); //generate bogus model
	}

	if (oct_usegpu)
	{
#if (GPUSEBO == 0)
		glBindTexture(GL_TEXTURE_2D,loct->octid);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,0,loct->gxsid,(loct->sur.mal*2)>>loct->glxsid,GL_RGBA,GL_UNSIGNED_BYTE,(void *)loct->sur.buf);
#else
		if (!loct->gsurf) loct->gsurf = (surf_t *)bo_begin(loct->bufid,0);
		memcpy(loct->gsurf,loct->sur.buf,loct->sur.mal*loct->sur.siz);
#endif
	}

	return(0);
}
int oct_load (oct_t *loct, char *filnam, dpoint3d *rpos, dpoint3d *rrig, dpoint3d *rdow, dpoint3d *rfor)
{
	int i;
	point3d fp, fr, fd, ff;
	i = oct_load(loct,filnam,&fp,&fr,&fd,&ff);
	rpos->x = fp.x; rpos->y = fp.y; rpos->z = fp.z;
	rrig->x = fr.x; rrig->y = fr.y; rrig->z = fr.z;
	rdow->x = fd.x; rdow->y = fd.y; rdow->z = fd.z;
	rfor->x = ff.x; rfor->y = ff.y; rfor->z = ff.z;
	return(i);
}

	//NOTE: font is stored vertically first! (like .ART files)
static const __int64 font6x8[] = //256 DOS chars, from: DOSAPP.FON (tab blank)
{
	0x3E00000000000000,0x6F6B3E003E455145,0x1C3E7C3E1C003E6B,0x3000183C7E3C1800,
	0x7E5C180030367F36,0x000018180000185C,0x0000FFFFE7E7FFFF,0xDBDBC3FF00000000,
	0x0E364A483000FFC3,0x6000062979290600,0x0A7E600004023F70,0x2A1C361C2A003F35,
	0x0800081C3E7F0000,0x7F361400007F3E1C,0x005F005F00001436,0x22007F017F090600,
	0x606060002259554D,0x14B6FFB614000060,0x100004067F060400,0x3E08080010307F30,
	0x08083E1C0800081C,0x0800404040407800,0x3F3C3000083E083E,0x030F3F0F0300303C,
	0x0000000000000000,0x0003070000065F06,0x247E247E24000307,0x630000126A2B2400,
	0x5649360063640813,0x0000030700005020,0x00000000413E0000,0x1C3E080000003E41,
	0x08083E080800083E,0x0800000060E00000,0x6060000008080808,0x0204081020000000,
	0x00003E4549513E00,0x4951620000407F42,0x3649494922004649,0x2F00107F12141800,
	0x494A3C0031494949,0x0305097101003049,0x0600364949493600,0x6C6C00001E294949,
	0x00006CEC00000000,0x2400004122140800,0x2241000024242424,0x0609590102000814,
	0x7E001E555D413E00,0x49497F007E111111,0x224141413E003649,0x7F003E4141417F00,
	0x09097F0041494949,0x7A4949413E000109,0x00007F0808087F00,0x4040300000417F41,
	0x412214087F003F40,0x7F00404040407F00,0x04027F007F020402,0x3E4141413E007F08,
	0x3E00060909097F00,0x09097F005E215141,0x3249494926006619,0x3F0001017F010100,
	0x40201F003F404040,0x3F403C403F001F20,0x0700631408146300,0x4549710007087008,
	0x0041417F00000043,0x0000201008040200,0x01020400007F4141,0x8080808080800402,
	0x2000000007030000,0x44447F0078545454,0x2844444438003844,0x38007F4444443800,
	0x097E080008545454,0x7CA4A4A418000009,0x0000007804047F00,0x8480400000407D00,
	0x004428107F00007D,0x7C0000407F000000,0x04047C0078041804,0x3844444438000078,
	0x380038444444FC00,0x44784400FC444444,0x2054545408000804,0x3C000024443E0400,
	0x40201C00007C2040,0x3C6030603C001C20,0x9C00006C10106C00,0x54546400003C60A0,
	0x0041413E0800004C,0x0000000077000000,0x02010200083E4141,0x3C2623263C000001,
	0x3D001221E1A11E00,0x54543800007D2040,0x7855555520000955,0x2000785554552000,
	0x5557200078545555,0x1422E2A21C007857,0x3800085555553800,0x5555380008555455,
	0x00417C0100000854,0x0000004279020000,0x2429700000407C01,0x782F252F78007029,
	0x3400455554547C00,0x7F097E0058547C54,0x0039454538004949,0x3900003944453800,
	0x21413C0000384445,0x007C20413D00007D,0x3D00003D60A19C00,0x40413C00003D4242,
	0x002466241800003D,0x29006249493E4800,0x16097F00292A7C2A,0x02097E8840001078,
	0x0000785555542000,0x4544380000417D00,0x007D21403C000039,0x7A0000710A097A00,
	0x5555080000792211,0x004E51514E005E55,0x3C0020404D483000,0x0404040404040404,
	0x506A4C0817001C04,0x0000782A34081700,0x0014080000307D30,0x0814000814001408,
	0x55AA114411441144,0xEEBBEEBB55AA55AA,0x0000FF000000EEBB,0x0A0A0000FF080808,
	0xFF00FF080000FF0A,0x0000F808F8080000,0xFB0A0000FE0A0A0A,0xFF00FF000000FF00,
	0x0000FE02FA0A0000,0x0F0800000F080B0A,0x0F0A0A0A00000F08,0x0000F80808080000,
	0x080808080F000000,0xF808080808080F08,0x0808FF0000000808,0x0808080808080808,
	0xFF0000000808FF08,0x0808FF00FF000A0A,0xFE000A0A0B080F00,0x0B080B0A0A0AFA02,
	0x0A0AFA02FA0A0A0A,0x0A0A0A0AFB00FF00,0xFB00FB0A0A0A0A0A,0x0A0A0B0A0A0A0A0A,
	0x0A0A08080F080F08,0xF808F8080A0AFA0A,0x08080F080F000808,0x00000A0A0F000000,
	0xF808F8000A0AFE00,0x0808FF00FF080808,0x08080A0AFB0A0A0A,0xF800000000000F08,
	0xFFFFFFFFFFFF0808,0xFFFFF0F0F0F0F0F0,0xFF000000000000FF,0x0F0F0F0F0F0FFFFF,
	0xFE00241824241800,0x01017F0000344A4A,0x027E027E02000003,0x1800006349556300,
	0x2020FC00041C2424,0x000478040800001C,0x3E00085577550800,0x02724C00003E4949,
	0x0030595522004C72,0x1800182418241800,0x2A2A1C0018247E24,0x003C02023C00002A,
	0x0000002A2A2A2A00,0x4A4A510000242E24,0x00514A4A44000044,0x20000402FC000000,
	0x2A08080000003F40,0x0012241224000808,0x0000000609090600,0x0008000000001818,
	0x02023E4030000000,0x0900000E010E0100,0x3C3C3C0000000A0D,0x000000000000003C,
};

void oct_drawpix (int x, int y, int col)
{
	if (!oct_usegpu)
	{
		if (((unsigned)x >= (unsigned)gdd.x) || ((unsigned)y >= (unsigned)gdd.y)) return;
		*(int *)(gdd.p*y + (x<<2) + gdd.f) = col;
	}
	else
	{
		gsetshadermodefor2d();
		glDisable(GL_TEXTURE_2D);
		glColor3ub((col>>16)&255,(col>>8)&255,col&255);
		glBegin(GL_POINTS); glTexCoord2f(0.5f,219.f/256.f);/*middle of solid white character*/
		glVertex2f(x,y);
		glEnd();
	}
}

void oct_drawline (float x0, float y0, float x1, float y1, int col)
{
	if (oct_usegpu)
	{
		gsetshadermodefor2d();
		glDisable(GL_TEXTURE_2D);
		glColor4ub((col>>16)&255,(col>>8)&255,col&255,(col>>24)&255);
		glBegin(GL_LINES); glTexCoord2f(0.5f,219.f/256.f);/*middle of solid white character*/
		glVertex2f(x0,y0);
		glVertex2f(x1,y1);
		glEnd();
	}
	else
	{
		float f;
		int i, dx, dy, ipx[2], ipy[2];

			  if (x0 <     0) { if (x1 <     0) return; y0 = (    0-x0)*(y1-y0)/(x1-x0)+y0; x0 =     0; }
		else if (x0 > gdd.x) { if (x1 > gdd.x) return; y0 = (gdd.x-x0)*(y1-y0)/(x1-x0)+y0; x0 = gdd.x; }
			  if (y0 <     0) { if (y1 <     0) return; x0 = (    0-y0)*(x1-x0)/(y1-y0)+x0; y0 =     0; }
		else if (y0 > gdd.y) { if (y1 > gdd.y) return; x0 = (gdd.y-y0)*(x1-x0)/(y1-y0)+x0; y0 = gdd.y; }
			  if (x1 <     0) {                         y1 = (    0-x1)*(y1-y0)/(x1-x0)+y1; x1 =     0; }
		else if (x1 > gdd.x) {                         y1 = (gdd.x-x1)*(y1-y0)/(x1-x0)+y1; x1 = gdd.x; }
			  if (y1 <     0) {                         x1 = (    0-y1)*(x1-x0)/(y1-y0)+x1; y1 =     0; }
		else if (y1 > gdd.y) {                         x1 = (gdd.y-y1)*(x1-x0)/(y1-y0)+x1; y1 = gdd.y; }

		x1 -= x0; y1 -= y0;
		i = ceil(max(fabs(x1),fabs(y1))); if (!(i&0x7fffffff)) return;
		f = 65536.0/(float)i;
		ipx[0] = (int)(x0*65536.0); ipx[1] = (int)(x1*f);
		ipy[0] = (int)(y0*65536.0); ipy[1] = (int)(y1*f);
		for(;i>0;i--)
		{
			oct_drawpix(ipx[0]>>16,ipy[0]>>16,col);
			ipx[0] += ipx[1]; ipy[0] += ipy[1];
		}
	}
}

static char vshad_drawtext[] = //See TEXTMODE.PSS
	"varying vec4 t;\n"
	"void main () { gl_Position = ftransform(); t = gl_MultiTexCoord0; }\n";
static char fshad_drawtext[] = //See TEXTMODE.PSS
	"varying vec4 t;\n"
	"uniform sampler1D tex0;\n"
	"uniform sampler2D tex1;\n"
	"uniform vec2 rtxtmul0, rtxtmul1, rtxtmul2, txtmod;\n"
	"uniform vec4 fcol, bcol;\n"
	"void main ()\n"
	"{\n"
	"   float ch = texture1D(tex0,dot(floor(t.xy*rtxtmul0),rtxtmul1)).x;\n"
	"   ch *= (255.001/256.0);\n"
	"   vec4 inchar = texture2D(tex1,mod(t.xy,txtmod)*rtxtmul2 + vec2(0.0,ch));\n"
	"   gl_FragColor = mix(bcol,fcol,inchar.r);\n"
	"}\n";

static char vshadasm_drawtext[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"END\n";
static char fshadasm_drawtext[] =
	"!!ARBfp1.0\n"
	"ATTRIB t = fragment.texcoord[0];\n"
	"PARAM rtxtmul0 = program.local[0];\n"
	"PARAM rtxtmul1 = program.local[1];\n"
	"PARAM rtxtmul2 = program.local[2];\n"
	"PARAM rtxtmod  = program.local[3];\n"
	"PARAM fcol     = program.local[4];\n"
	"PARAM bcol     = program.local[5];\n"
	"TEMP a, b;\n"
	"MUL a, t, rtxtmul0;\n"
	"FLR a, a;\n"
	"DP3 a, a, rtxtmul1;\n"
	"TEX a, a, texture[0], 1D;\n"
	"MUL a, a, {0.0,0.996,0.0,0.0};\n" //~255.001/256
	"MUL b, t, rtxtmod;\n"
	"FRC b, b;\n"
	"MAD b, b, rtxtmul2, a;\n"
	"TEX b, b, texture[1], 2D;\n"
	"LRP result.color, b.r, fcol, bcol;\n"
	"END\n";

#define TXTBUFSIZ 1024 //FIX:dynamic!
	//* Caller must update (0..dx-1, 0..dy-1) of GPU tex#0 before calling oct_drawtext6x8()
	//* Full alpha is supported on GPU.
void oct_drawtext6x8 (int x0, int y0, int fcol, int bcol, const char *fmt, ...)
{
	va_list arglist;
	unsigned char st[1024];

	if (!fmt) return;
	va_start(arglist,fmt);
	if (_vsnprintf((char *)&st,sizeof(st)-1,fmt,arglist)) st[sizeof(st)-1] = 0;
	va_end(arglist);

	if (!oct_usegpu)
	{
		unsigned char *pst, *c, *v;
		int i, j, ie, x, *lp, *lpx;

		pst = st;
		do
		{
			lp = (int *)(y0*gdd.p+gdd.f);
			for(j=1;j<256;y0++,lp=(int *)(((INT_PTR)lp)+gdd.p),j+=j)
				if ((unsigned)y0 < (unsigned)gdd.y)
					for(c=pst,x=x0;c[0] && (c[0] != '\n');c++,x+=6)
					{
						v = ((int)(*c))*6 + ((unsigned char *)font6x8); lpx = &lp[x];
						for(i=max(-x,0),ie=min(gdd.x-x,6);i<ie;i++) { if (v[i]&j) lpx[i] = fcol; else if (bcol < 0) lpx[i] = bcol; }
						if ((*c) == 9) { if (bcol < 0) { for(i=max(-x,6),ie=min(gdd.x-x,18);i<ie;i++) lpx[i] = bcol; } x += 2*6; }
					}
			if (!c[0]) return;
			pst = &c[1];
		} while (1);
	}
	else
	{
		#define FXSIZ 6
		#define FYSIZ 8
		unsigned char *cptr, txtbuf[TXTBUFSIZ];
		int i, x, y, xmax, ymax;

			//Calculate rectangle area
		x = 0; y = 0; xmax = -1; ymax = -1;
		for(cptr=st;cptr[0];x++)
		{
			i = *cptr++;
			if (i == 32) continue;
			if (i == 9) { x += 2; continue; }
			if (i == '\n') { x = -1; y++; continue; }
			if (x > xmax) xmax = x;
		}
		ymax = y; if ((xmax|ymax) < 0) return;
		xmax++; ymax++; if (xmax*ymax >= TXTBUFSIZ) return; //:/

			//Render string
		x = 0; y = 0; memset(txtbuf,32,xmax*ymax);
		for(cptr=st;cptr[0];x++)
		{
			i = *cptr++;
			if (i == 32) continue;
			if (i == 9) { x += 2; continue; }
			if (i == '\n') { x = -1; y++; continue; }
			txtbuf[y*xmax+x] = (unsigned char)i;
		}

		if (shadcur != 3)
		{
			shadcur = 3;
			glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,xres,yres,0,-1,1);
			glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

			kglActiveTexture(1); glBindTexture(GL_TEXTURE_2D,gfont6x8id);
			kglActiveTexture(0); glBindTexture(GL_TEXTURE_1D,gtextbufid);

			if (oct_useglsl)
			{
				((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
			}
			else
			{
				glEnable(GL_VERTEX_PROGRAM_ARB);
				glEnable(GL_FRAGMENT_PROGRAM_ARB);
				((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shadvert[shadcur]);
				((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shadcur]);
			}
		}

		glTexSubImage1D(GL_TEXTURE_1D,0,0,xmax*ymax,GL_LUMINANCE,GL_UNSIGNED_BYTE,(void *)txtbuf);

		if (oct_useglsl)
		{
			kglUniform2f(kglGetUniformLoc("rtxtmul0"),1.0/(float)FXSIZ,1.0/(float)FYSIZ);
			kglUniform2f(kglGetUniformLoc("rtxtmul1"),1.0/(float)TXTBUFSIZ,(float)xmax/(float)TXTBUFSIZ);
			kglUniform2f(kglGetUniformLoc("rtxtmul2"),1.0/(float)8,1.0/(float)(FYSIZ*256));
			kglUniform2f(kglGetUniformLoc("txtmod"),FXSIZ,FYSIZ);
			kglUniform4f(kglGetUniformLoc("fcol"),(float)((fcol>>16)&255)*(1.0/255.0),(float)((fcol>> 8)&255)*(1.0/255.0),
															  (float)((fcol>> 0)&255)*(1.0/255.0),(float)((fcol>>24)&255)*(1.0/255.0));
			kglUniform4f(kglGetUniformLoc("bcol"),(float)((bcol>>16)&255)*(1.0/255.0),(float)((bcol>> 8)&255)*(1.0/255.0),
															  (float)((bcol>> 0)&255)*(1.0/255.0),(float)((bcol>>24)&255)*(1.0/255.0));
		}
		else
		{
			kglProgramLocalParam(0,1.0/(float)FXSIZ,1.0/(float)FYSIZ,0,0);                                   //rtxtmul0
			kglProgramLocalParam(1,1.0/(float)TXTBUFSIZ,(float)xmax/(float)TXTBUFSIZ,0,0);                   //rtxtmul1
			kglProgramLocalParam(2,FXSIZ/(float)8,1.0/256.0,0,0);                                            //rtxtmul2*txtmod
			kglProgramLocalParam(3,1.0/(float)FXSIZ,1.0/(float)FYSIZ,0,0);                                   //rtxtmod
			kglProgramLocalParam(4,(float)((fcol>>16)&255)*(1.0/255.0),(float)((fcol>> 8)&255)*(1.0/255.0),  //fcol
										  (float)((fcol>> 0)&255)*(1.0/255.0),(float)((fcol>>24)&255)*(1.0/255.0));
			kglProgramLocalParam(5,(float)((bcol>>16)&255)*(1.0/255.0),(float)((bcol>> 8)&255)*(1.0/255.0),  //bcol
										  (float)((bcol>> 0)&255)*(1.0/255.0),(float)((bcol>>24)&255)*(1.0/255.0));
		}

		xmax *= FXSIZ; ymax *= FYSIZ;
		glBegin(GL_QUADS);
		glTexCoord2f(   0,   0); glVertex2i(x0     ,y0     );
		glTexCoord2f(xmax,   0); glVertex2i(x0+xmax,y0     );
		glTexCoord2f(xmax,ymax); glVertex2i(x0+xmax,y0+ymax);
		glTexCoord2f(   0,ymax); glVertex2i(x0     ,y0+ymax);
		glEnd();

		shadcur = 0;
		if (oct_useglsl) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
	}
}

static char vshad_drawpol[] =
	"varying vec4 t;\n"
	"void main () { gl_Position = ftransform(); t = gl_MultiTexCoord0; }\n";
static char fshad_drawpol[] =
	"varying vec4 t;\n"
	"uniform sampler2D tex0;\n"
	"uniform vec4 colmul;\n"
	"void main ()\n"
	"{\n"
	"   gl_FragColor = texture2D(tex0,t.xy)*colmul;\n"
	"}\n";

static char vshadasm_drawpol[] =
	"!!ARBvp1.0\n"
	"PARAM ModelViewProj[4] = {state.matrix.mvp};\n"
	"TEMP t;\n"
	"DP4 t.x, ModelViewProj[0], vertex.position;\n"
	"DP4 t.y, ModelViewProj[1], vertex.position;\n"
	"DP4 t.z, ModelViewProj[2], vertex.position;\n"
	"DP4 t.w, ModelViewProj[3], vertex.position;\n"
	"MOV result.position, t;\n"
	"MOV result.texcoord[0], vertex.texcoord[0];\n"
	"END\n";
static char fshadasm_drawpol[] =
	"!!ARBfp1.0\n"
	"ATTRIB t = fragment.texcoord[0];\n"
	"PARAM colmul = program.local[0];\n"
	"TEMP a;\n"
	"TEX a, t, texture[0], 2D;\n"
	"MUL result.color, a, colmul;\n"
	"END\n";

	//proj==PROJ_SCREEN: qv.xy  is screen coords (no depth test)
	//proj==PROJ_WORLD : qv.xyz is world  coords
	//proj==PROJ_CAM   : qv.xyz is camera coords
void oct_drawpol (INT_PTR rptr, glvert_t *qv, int n, float rmul, float gmul, float bmul, float amul, int proj)
{
	if (oct_usegpu)
	{
		float fx, fy, fz;
		int i;

		if (shadcur != 4)
		{
			shadcur = 4;
			if (proj == PROJ_SCREEN) { glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,xres,yres,0,-1,1); }
			else
			{
				glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-gznear,gznear,-(float)yres/(float)xres*gznear,(float)yres/(float)xres*gznear,gznear,gzfar);
				glMatrixMode(GL_MODELVIEW); glLoadIdentity();
			}
			glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

			kglActiveTexture(0); glBindTexture(GL_TEXTURE_2D,(unsigned int)rptr);

			if (oct_useglsl)
			{
				((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
			}
			else
			{
				glEnable(GL_VERTEX_PROGRAM_ARB);
				glEnable(GL_FRAGMENT_PROGRAM_ARB);
				((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_VERTEX_PROGRAM_ARB  ,shadvert[shadcur]);
				((PFNGLBINDPROGRAMARBPROC)glfp[glBindProgramARB])(GL_FRAGMENT_PROGRAM_ARB,shadfrag[shadcur]);
			}
		}

		if (oct_useglsl) { kglUniform4f(kglGetUniformLoc("colmul"),rmul,gmul,bmul,amul); }
						else { kglProgramLocalParam(0,rmul,gmul,bmul,amul); }

		if (proj == PROJ_SCREEN) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);

		glBegin(GL_TRIANGLE_FAN);
		for(i=0;i<n;i++)
		{
			glTexCoord2f(qv[i].u,qv[i].v);
			switch(proj)
			{
				case PROJ_SCREEN: glVertex2f(qv[i].x,qv[i].y); break;
				case PROJ_WORLD:
					fx = qv[i].x-gipos.x;
					fy = qv[i].y-gipos.y;
					fz = qv[i].z-gipos.z;
					glVertex3f(+(fx*girig.x + fy*girig.y + fz*girig.z),
								  -(fx*gidow.x + fy*gidow.y + fz*gidow.z),
								  -(fx*gifor.x + fy*gifor.y + fz*gifor.z));
					break;
				case PROJ_CAM: glVertex3f(qv[i].x,-qv[i].y,-qv[i].z); break;
			}
		}
		glEnd();

		shadcur = 0;
		if (oct_useglsl) ((PFNGLUSEPROGRAMPROC)glfp[glUseProgram])(shadprog[shadcur]);
		return;
	}

	#define DRAWPOL_USEASM 1
	point3d fp, fp2;
	glvert_t *np, *np2, tp;
	INT_PTR rptr2;
	__declspec(align(16)) float fdi[4];
	__declspec(align(16)) int iwi[4];
	float f, t, d, u, v, d2, u2, v2, r, nx, ny, nz, ox, oy, oz, dx, dy, db, ux, uy, ub, vx, vy, vb, *zptr, fd, fd2;
	int i, j, k, l, sx, sxe, sx2, sxe2, sy, sy1, x, xi, *iptr, iu, iv, n2, poff, pit, umsk, vmsk, nfcol;
	int *isy, *pxc, xc[2][MAXYDIM], my0, my1;
	tiles_t *rtile;

	rtile = (tiles_t *)rptr;
	np  = (glvert_t *)_alloca(sizeof(glvert_t)*n  );
	np2 = (glvert_t *)_alloca(sizeof(glvert_t)*n*2);
	isy = (     int *)_alloca(sizeof(int     )*n*2);

		//Convert projection to 3D camera coords if (proj != PROJ_CAM)
	for(i=n-1;i>=0;i--)
	{
		np[i] = qv[i];
			  if (proj == PROJ_SCREEN) { np[i].x -= ghx; np[i].y -= ghy; np[i].z = ghz; }
		else if (proj == PROJ_WORLD)
		{
			fp.x = np[i].x-gipos.x; fp.y = np[i].y-gipos.y; fp.z = np[i].z-gipos.z;
			np[i].x = fp.x*girig.x + fp.y*girig.y + fp.z*girig.z;
			np[i].y = fp.x*gidow.x + fp.y*gidow.y + fp.z*gidow.z;
			np[i].z = fp.x*gifor.x + fp.y*gifor.y + fp.z*gifor.z;
		}
	}

		//Clip to SCISDIST plane
	n2 = 0;
	for(i=0;i<n;i++)
	{
		j = i+1; if (j >= n) j = 0;
		if (np[i].z >= SCISDIST) { np2[n2] = np[i]; n2++; }
		if ((np[i].z >= SCISDIST) != (np[j].z >= SCISDIST))
		{
			f = (SCISDIST-np[i].z)/(np[j].z-np[i].z);
			np2[n2].u = (np[j].u-np[i].u)*f + np[i].u;
			np2[n2].v = (np[j].v-np[i].v)*f + np[i].v;
			np2[n2].x = (np[j].x-np[i].x)*f + np[i].x;
			np2[n2].y = (np[j].y-np[i].y)*f + np[i].y;
			np2[n2].z = SCISDIST; n2++;
		}
	}
	if (n2 < 3) return;

	poff = rtile->f;
	pit  = rtile->p;
	umsk = rtile->x-1;
	vmsk = rtile->y-1;

	nfcol = (min(max((int)(bmul*64.0),0),255)<< 0) +
			  (min(max((int)(gmul*64.0),0),255)<< 8) +
			  (min(max((int)(rmul*64.0),0),255)<<16) +
			  (min(max((int)(amul*64.0),0),255)<<24);

		//3D->2D projection
	for(i=n2-1;i>=0;i--)
	{
		np2[i].z = 1.0/np2[i].z; f = np2[i].z*ghz;
		np2[i].x = np2[i].x*f + ghx;
		np2[i].y = np2[i].y*f + ghy;
	}

		//General equations:
		//pz[i] = (px[i]*gdx + py[i]*gdy + gdo)
		//pu[i] = (px[i]*gux + py[i]*guy + guo)/pz[i]
		//pv[i] = (px[i]*gvx + py[i]*gvy + gvo)/pz[i]
		//
		//px[0]*gdx + py[0]*gdy + 1*gdo = pz[0]
		//px[1]*gdx + py[1]*gdy + 1*gdo = pz[1]
		//px[2]*gdx + py[2]*gdy + 1*gdo = pz[2]
		//
		//px[0]*gux + py[0]*guy + 1*guo = pu[0]*pz[0] (pu[i] premultiplied by pz[i] above)
		//px[1]*gux + py[1]*guy + 1*guo = pu[1]*pz[1]
		//px[2]*gux + py[2]*guy + 1*guo = pu[2]*pz[2]
		//
		//px[0]*gvx + py[0]*gvy + 1*gvo = pv[0]*pz[0] (pv[i] premultiplied by pz[i] above)
		//px[1]*gvx + py[1]*gvy + 1*gvo = pv[1]*pz[1]
		//px[2]*gvx + py[2]*gvy + 1*gvo = pv[2]*pz[2]
	np2[0].u *= np2[0].z; np2[1].u *= np2[1].z; np2[2].u *= np2[2].z;
	np2[0].v *= np2[0].z; np2[1].v *= np2[1].z; np2[2].v *= np2[2].z;
	ox = np2[1].y-np2[2].y; oy = np2[2].y-np2[0].y; oz = np2[0].y-np2[1].y;
	r = 1.0 / (ox*np2[0].x + oy*np2[1].x + oz*np2[2].x);
	dx = (ox*np2[0].z + oy*np2[1].z + oz*np2[2].z)*r;
	ux = (ox*np2[0].u + oy*np2[1].u + oz*np2[2].u)*r;
	vx = (ox*np2[0].v + oy*np2[1].v + oz*np2[2].v)*r;
	ox = np2[2].x-np2[1].x; oy = np2[0].x-np2[2].x; oz = np2[1].x-np2[0].x;
	dy = (ox*np2[0].z + oy*np2[1].z + oz*np2[2].z)*r;
	uy = (ox*np2[0].u + oy*np2[1].u + oz*np2[2].u)*r;
	vy = (ox*np2[0].v + oy*np2[1].v + oz*np2[2].v)*r;
	db = np2[0].z - np2[0].x*dx - np2[0].y*dy;
	ub = np2[0].u - np2[0].x*ux - np2[0].y*uy;
	vb = np2[0].v - np2[0].x*vx - np2[0].y*vy;
	ux *= rtile->x; uy *= rtile->x; ub *= rtile->x;
	vx *= rtile->y; vy *= rtile->y; vb *= rtile->y;

		//Hack to simulate GL_CULL_NONE
	for(f=0.f,i=n2-2,j=n2-1,k=0;k<n2;i=j,j=k,k++) f += (np2[i].x-np2[k].x)*np2[j].y;
	if (*(int *)&f < 0.f) { for(i=(n2>>1)-1;i>=0;i--) { tp = np2[i]; np2[i] = np2[n2-1-i]; np2[n2-1-i] = tp; } }

		//Quantize sy
	my0 = 0x7fffffff; my1 = 0x80000000;
	for(i=n2-1;i>=0;i--)
	{
		isy[i] = min(max(cvttss2si(np2[i].y)+1,0),gdd.y);
		if (isy[i] < my0) my0 = isy[i];
		if (isy[i] > my1) my1 = isy[i];
	}

		//Rasterize
	for(i=n2-1,j=0;j<n2;i=j,j++)
	{
		if (isy[i] == isy[j]) continue;
		if (isy[i] < isy[j]) { sy = isy[i]; sy1 = isy[j]; pxc = &xc[1][0]; }
							 else { sy = isy[j]; sy1 = isy[i]; pxc = &xc[0][0]; }
		v = (np2[j].x-np2[i].x)/(np2[j].y-np2[i].y);
		u = ((float)sy-np2[i].y)*v + np2[i].x + 1.0;
		for(;sy<sy1;sy++,u+=v) pxc[sy] = cvttss2si(u);
	}

#if (DRAWPOL_USEASM != 0)
	__declspec(align(16)) static const int dqzeros[4] = {0,0,0,0}, dqnegones[4] = {-1,-1,-1,-1}, dq0011[4] = {0,0,-1,-1};
	__declspec(align(16)) short dqnfcol[8], dqmsk[8], dqpit4[8];
	dqnfcol[0] = ((nfcol    )&255); dqnfcol[1] = ((nfcol>> 8)&255); dqnfcol[2] = ((nfcol>>16)&255); dqnfcol[3] = ((nfcol>>24)&255);
	dqmsk[0] = umsk; dqmsk[1] = vmsk;
	dqpit4[0] = 4;   dqpit4[1] = pit;
#endif

	for(sy=my0;sy<my1;sy++)
	{
		sx  = max(xc[0][sy],0);
		sxe = min(xc[1][sy],gdd.x); if (sx >= sxe) continue;
		iptr = (int *)(sy*gdd.p + gdd.f); zptr = (float *)((INT_PTR)iptr + zbufoff);

		d2 = dy*(float)sy + db;
		u2 = uy*(float)sy + ub;
		v2 = vy*(float)sy + vb;

			//New code in C, with clean interpolation
		#define LPIXPERDIV 4 //FIXFIXFIXFIX:optimize for integer stuff!
		#define PIXPERDIV (1<<LPIXPERDIV)

		if (dx >= 0) sx2 = sx;
				  else sx2 = sxe - (((sxe-sx)+PIXPERDIV+1)&(-PIXPERDIV));
		d = dx*(float)sx2 + d2;
		u = ux*(float)sx2 + u2;
		v = vx*(float)sx2 + v2;
		fd = 1.f/d; f = fd*65536.f;
		iu = cvttss2si(u*f);
		iv = cvttss2si(v*f);

		if (dx < 0)
		{
			d = dx*(float)(sx2+PIXPERDIV) + d2;
			u = ux*(float)(sx2+PIXPERDIV) + u2;
			v = vx*(float)(sx2+PIXPERDIV) + v2;
			fd2 = 1.f/d; f = fd2*65536.f;
			fdi[0] = (fd2-fd)*(1.0/PIXPERDIV);
			iwi[0] = ((cvttss2si(u*f)-iu)>>LPIXPERDIV);
			iwi[1] = ((cvttss2si(v*f)-iv)>>LPIXPERDIV);

			fd += (sx-sx2)*fdi[0];
			iu += (sx-sx2)*iwi[0];
			iv += (sx-sx2)*iwi[1];
			sxe2 = min(sx2+PIXPERDIV,sxe);
			goto in2it;
		}

		if (proj == PROJ_SCREEN) { fd = 0.0; fdi[0] = 0.0; }

		while (sx < sxe)
		{
			d = dx*(float)(sx+PIXPERDIV) + d2;
			u = ux*(float)(sx+PIXPERDIV) + u2;
			v = vx*(float)(sx+PIXPERDIV) + v2;
			fd2 = 1.f/d; f = fd2*65536.f;
			if (proj != PROJ_SCREEN) fdi[0] = (fd2-fd)*(1.0/PIXPERDIV);
			iwi[0] = ((cvttss2si(u*f)-iu)>>LPIXPERDIV);
			iwi[1] = ((cvttss2si(v*f)-iv)>>LPIXPERDIV);
			sxe2 = min(sx+PIXPERDIV,sxe);
in2it:;
#if (DRAWPOL_USEASM == 0)
			for(;sx<sxe2;sx++,fd+=fdi[0],iu+=iwi[0],iv+=iwi[1])
			{
				if (*(int *)&fd >= *(int *)&zptr[sx]) continue;
				rptr2 = ((iv>>16)&vmsk)*pit + (((iu>>16)&umsk)<<2) + poff;

#if (DRAWPOL_CPU_FILT == 0)
				i = *(int *)rptr2; //nearest
#else
				i = bilinfilt((int *)rptr2,(int *)(rptr2+pit),iu,iv); //bilinear
#endif

				i = mulcol(i,nfcol); if (i >= 0) continue;
				iptr[sx] = i;
				zptr[sx] = fd;
			}
#else
			_asm
			{
				push ebx
				push esi
				push edi

				mov ecx, sx
				cmp ecx, sxe2
				jge short endit

					;xmm6:[---- ---- ---- ---- iv.h iv.l iu.h iu.l]
					;xmm7:[--------- --------- ---------     fd   ]
				movd xmm6, iu
				movd xmm0, iv
				punpckldq xmm6, xmm0
				movss xmm7, fd

				mov eax, sxe2
				mov edi, iptr
				sub ecx, eax
				lea edi, [edi+eax*4]
				mov eax, poff

				mov esi, zbufoff
				add esi, edi

				mov ebx, pit
				add ebx, eax

			begit:comiss xmm7, [esi+ecx*4]
					jae short skipit

					pshuflw xmm0, xmm6, 0xd  ;xmm0: [" " " " - -   iv   iu]
					pand xmm0, dqmsk         ;   &= [- - - - - - vmsk umsk]
					pmaddwd xmm0, dqpit4     ;   *= [- - - - - -  pit   4 ]
					movd edx, xmm0

#if (DRAWPOL_CPU_FILT == 0)
						;nearest
					movd xmm0, [edx+eax]
					punpcklbw xmm0, dqzeros
#else
						;bilinear (optimized nicely for SSE2!)
					movups xmm0, [edx+eax]
					movups xmm2, [edx+ebx]
					punpcklbw xmm0, dqzeros  ;xmm0:[a10 r10 g10 b10 a00 r00 g00 b00]
					punpcklbw xmm2, dqzeros  ;xmm2:[a11 r11 g11 b11 a01 r01 g01 b01]

					pshuflw xmm4, xmm6, 0x00 ;xmm4:[  -   -   -   -  fx  fx  fx  fx]
					pshuflw xmm5, xmm6, 0xaa ;xmm5:[  -   -   -   -  fy  fy  fy  fy]
					movlhps xmm5, xmm5       ;xmm5:[ fy  fy  fy  fy  fy  fy  fy  fy]
					pmulhuw xmm2, xmm5
					xorps xmm5, dqnegones
					pmulhuw xmm0, xmm5
					paddw xmm0, xmm2
					movhlps xmm2, xmm0

					pmulhuw xmm2, xmm4
					xorps xmm4, dqnegones
					pmulhuw xmm0, xmm4
					paddw xmm0, xmm2
#endif

					pmullw xmm0, dqnfcol
					psrlw xmm0, 6
					packuswb xmm0, xmm0

					movd edx, xmm0
					test edx, edx
					jns short skipit

					movd [edi+ecx*4], xmm0
					movd [esi+ecx*4], xmm7
		  skipit:paddd xmm6, iwi          ;+= [ -   -  iwi[1] iwi[0]]
					addss xmm7, fdi          ;+= [ -   -     -   fdi[0]]
					add ecx, 1
					jl short begit

				movd iu, xmm6
				pshuflw xmm6, xmm6, 0x4e
				movd iv, xmm6
				movss fd, xmm7

		endit:pop edi
				pop esi
				pop ebx
			}
			sx = sxe2;
#endif
		}
	}
}

INT_PTR oct_loadtiles (char *tilefil)
{
	tiletype pic;
	unsigned int tilid;
	int i, j, x, y, xx, yy, tilesid, numwhite, gotdif, useval[256], usevaln = 0;

	if (tilefil)
	{
		kpzload(tilefil,&pic.f,&pic.p,&pic.x,&pic.y);
	}
	else
	{
		pic.f = 0;
	}
	if (!pic.f)
	{
		pic.x = 256; pic.y = pic.x; pic.p = pic.x*sizeof(int); pic.f = (INT_PTR)malloc(pic.p*pic.y);
		xx = (pic.x>>4)-1; yy = 6144/pic.x;
		for(y=0;y<pic.y;y++)
			for(x=0;x<pic.x;x++) //gen crap dummy tex
			{
				i = min((xx-labs((x&xx)-(xx>>1)-1))*yy,224);
				j = min((xx-labs((y&xx)-(xx>>1)-1))*yy,224);
				i = min(i,j)*0x10101 + (((x*y)*0x2345+(x^y)*0x156d)&0x1f1f1f);
				*(int *)(pic.p*y + (x<<2) + pic.f) = ((i&0xfefefe)>>1) + 0xff000000;
			}
	}

	pic.x = min(pic.x,pic.y); pic.y = pic.x;
	tilesid = (pic.x>>4); for(tiles[0].ltilesid=0;(1<<tiles[0].ltilesid)<tilesid;tiles[0].ltilesid++);
	tiles[0].x = pic.x*2; tiles[0].y = pic.y*2; tiles[0].p = tiles[0].x*sizeof(int);
	if ((oct_usegpu) && (oct_usefilter == 3)) tiles[0].p *= 2;
	tiles[0].f = (INT_PTR)malloc(tiles[0].p*tiles[0].y); if (!tiles[0].f) { MessageBox(ghwnd,"tiles[0] malloc fail",prognam,MB_OK); exit(1); }
	if ((oct_usegpu) && (oct_usefilter == 3)) tiles[0].f += tiles[0].x*sizeof(int); //FIX:BEWARE of free() location! (not implemented yet!)
	for(y=0;y<16;y++) //Generate list of which tiles look good and mount on 2x size texture for clean mip-map borders
		for(x=0;x<16;x++)
		{
			numwhite = 0; gotdif = 0; j = *(int *)((y*tilesid)*pic.p + (x*tilesid)*4 + pic.f);
			for(yy=0;yy<tilesid;yy++)
				for(xx=0;xx<tilesid;xx++)
				{
					i = *(int *)((y*tilesid+yy)*pic.p + (x*tilesid+xx)*4 + pic.f); if (i != j) gotdif = 1;
					if ((i&0xffffff) == 0xffffff) numwhite++;
				}
			if ((gotdif != 0) && (numwhite < 24)) { useval[usevaln] = y*16+x; usevaln++; }

			for(yy=0;yy<tilesid*2;yy++)
				for(xx=0;xx<tilesid*2;xx++)
				{
					*(int *)((y*tilesid*2+yy)*tiles[0].p + (x*tilesid*2+xx)*4 + tiles[0].f) =
				 //*(int *)((y*tilesid+min(max(yy-(tilesid>>1),0),tilesid-1))*pic.p + (x*tilesid+min(max(xx-(tilesid>>1),0),tilesid-1))*4 + pic.f); //clamp
					*(int *)((y*tilesid+      ((yy-(tilesid>>1))&(tilesid-1)))*pic.p + (x*tilesid+      ((xx-(tilesid>>1))&(tilesid-1)))*4 + pic.f); //wrap
				}
		}
	free((void *)pic.f);

	if ((!oct_usegpu) || (oct_usefilter != 3))
	{
		for(i=0;tiles[i].y;i++)
		{
			tiles[i+1].x = (tiles[i].x>>1);
			tiles[i+1].y = (tiles[i].y>>1); tiles[i+1].p = tiles[i+1].x*sizeof(int);
			tiles[i+1].f = (INT_PTR)malloc(tiles[i+1].p*tiles[i+1].y); if (!tiles[i+1].f) { MessageBox(ghwnd,"tiles[] malloc fail",prognam,MB_OK); exit(1); }
			tiles_genmip(&tiles[i],&tiles[i+1]);
		}
	}
	else
	{
			//Hacked Mip-map using double bilinear lut: wastes 1/3 memory
			//   ÚÄÂÄÂÄÄÄÂÄÄÄÄÄÄ¿
			//   ³xÀÄ´   ³      ³
			//   ³xxxÀÄÄÄ´      ³
			//   ³xxxxxxx³      ³
			//   ÀÄÄÄÄÄÄÄÁÄÄÄÄÄÄÙ
		for(i=0;tiles[i].y;i++)
		{
			tiles[i+1].x = (tiles[i].x>>1);
			tiles[i+1].y = (tiles[i].y>>1); tiles[i+1].p = tiles[0].p;
			tiles[i+1].f = tiles[i].f-(tiles[i+1].x*sizeof(int));
			tiles_genmip(&tiles[i],&tiles[i+1]);
		}
	}

	if (!oct_usegpu)
	{
		if (!tiles[0].ltilesid) { tiles[1] = tiles[0]; } //Hack to prevent crash in cpu_shader_texmap() while mip-mapping; FIX:beware of freeing same pointer twice!
		return((INT_PTR)&tiles[0]);
	}

		//Load tiles texture to GPU
	glGenTextures(1,&tilid);
	switch(oct_usefilter)
	{
		case 0: kglalloctex(tilid,(void *)tiles[0].f,tiles[0].x,tiles[0].y,1,KGL_BGRA32+KGL_NEAREST+KGL_CLAMP_TO_EDGE); break;
		case 1: kglalloctex(tilid,(void *)tiles[0].f,tiles[0].x,tiles[0].y,1,KGL_BGRA32+KGL_LINEAR +KGL_CLAMP_TO_EDGE); break;
		case 2: kglalloctex(tilid,(void *)tiles[0].f,tiles[0].x,tiles[0].y,1,KGL_BGRA32+KGL_MIPMAP3+KGL_CLAMP_TO_EDGE);
				  for(i=1;tiles[i].x && tiles[i].y;i++) glTexImage2D(GL_TEXTURE_2D,i,4,tiles[i].x,tiles[i].y,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,(const void *)tiles[i].f);
				  break;
		case 3: kglalloctex(tilid,(void *)(tiles[0].f-tiles[0].x*sizeof(int)),tiles[0].x*2,tiles[0].y,1,KGL_BGRA32+KGL_LINEAR +KGL_CLAMP_TO_EDGE); break;
	}
	return((INT_PTR)tilid);
}

void oct_uninitonce (void)
{
	if (oct_usegpu) { kgluninit(ghwnd,glhDC,glhRC); DestroyWindow(ghwnd); }
}

int oct_initonce (float zfar)
{
	brush_sph_t sph;
	double d, d0, d1;
	int i, j, x, y, z, s, xsiz, ysiz;
	char *cbuf, *cptr;
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	maxcpu = min(si.dwNumberOfProcessors,MAXCPU); if (!oct_numcpu) oct_numcpu = maxcpu;

	gzfar = zfar; gznear = zfar*(1.0/8388608.0);

	if (!oct_usegpu)
	{
cpuonly:;
#if (MARCHCUBE == 0)
		shaderfunc = cpu_shader_texmap;
#else
		shaderfunc = cpu_shader_texmap_mc;
#endif
	}
	else
	{
		if (kglinit(&glhDC,&glhRC) < 0) { oct_usegpu = 0; goto cpuonly; }

		shaderfunc = 0;
		if (oct_useglsl)
		{
			compileshader(0,vshad_drawoct ,fshad_drawoct ,"drawoct");
			compileshader(1,vshad_drawcone,fshad_drawcone,"drawcone");
			compileshader(2,vshad_drawsky ,fshad_drawsky ,"drawsky");
			compileshader(3,vshad_drawtext,fshad_drawtext,"drawtext");
			compileshader(4,vshad_drawpol ,fshad_drawpol ,"drawpol");
		}
		else
		{
			compileshader(0,vshadasm_drawoct ,fshadasm_drawoct ,"drawoct");
			compileshader(1,vshadasm_drawcone,fshadasm_drawcone,"drawcone");
			compileshader(2,vshadasm_drawsky ,fshadasm_drawsky ,"drawsky");
			compileshader(3,vshadasm_drawtext,fshadasm_drawtext,"drawtext");
			compileshader(4,vshadasm_drawpol ,fshadasm_drawpol ,"drawpol");
		}
		shadcur = 0;

			//Allocate screen texture for pass 1->2 transfer
		glGenTextures(1,&gpixtexid);
		kglalloctex(gpixtexid,0,(gpixxdim*PIXBUFBYPP)>>2,gpixydim,1,KGL_RGBA32+KGL_NEAREST); //only NEAREST makes sense here! (allocate stuff here)
#if (GPUSEBO != 0)
		((PFNGLGENBUFFERS)glfp[glGenBuffers])(1,&gpixbufid);
#endif

			//Load 6x8(x256) font
		xsiz = 8; ysiz = 8*256;
		if (!(cbuf = (char *)malloc(xsiz*ysiz))) return(-1);
		memset(cbuf,0,xsiz*ysiz);
		for(y=0,cptr=cbuf;y<ysiz;y++,cptr+=xsiz)
			for(x=0;x<6;x++)
				if (((char *)font6x8)[(y>>3)*6+x]&(1<<(y&7))) cptr[x] = 255;
		glGenTextures(1,&gfont6x8id);
		kglalloctex(gfont6x8id,cbuf,xsiz,ysiz,1,KGL_CHAR+KGL_NEAREST);
		free((void *)cbuf);

			//Allocate buffer for oct_drawtext6x8()
		glGenTextures(1,&gtextbufid);
		kglalloctex(gtextbufid,0,TXTBUFSIZ,1,1,KGL_CHAR+KGL_NEAREST);
	}

#if (MARCHCUBE != 0)
	greciplo[0] = -1.0/256.0;
	for(i=256-1;i>0;i--) { d = 1.0/(double)i; greciplo[256-i] =-d; greciplo[256+i] = d; }
#endif

	return(0);
}

int oct_startdraw (dpoint3d *ipos, dpoint3d *irig, dpoint3d *idow, dpoint3d *ifor, double ghx, double ghy, double ghz)
{
	tiletype dd;
	int i, j, k;

	if (!oct_usegpu)
	{
		if (!startdirectdraw((long *)&dd.f,(long *)&dd.p,(long *)&dd.x,(long *)&dd.y)) return(-1);

		i = dd.p*dd.y;
		if (i > zbufmal) { zbufmal = i; zbuf = (int *)realloc(zbuf,zbufmal+256); }
	}
	else
	{
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-gznear,gznear,-(float)yres/(float)xres*gznear,(float)yres/(float)xres*gznear,gznear,gzfar);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();

		k = oct_fogcol;
		glClearColor(((k>>16)&255)/255.0,((k>>8)&255)/255.0,(k&255)/255.0,0.0);
		glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE,GL_NONE);
		gshadermode = 1;
	}
	i = ((xres+127)&~127);
	if ((i>>3)*yres > gcbitmax)
	{
		gcbitmax = (i>>3)*yres; gcbitpl = i; gcbitplo5 = (gcbitpl>>5); gcbitplo3 = (gcbitpl>>3); gcbpl = (gcbitpl>>3);
		gcbitmal = (unsigned int)realloc((void *)gcbitmal,gcbitmax+256); gcbit = (unsigned int *)((gcbitmal+15)&~15);
	}

	if (!oct_usegpu)
	{
			//zbuffer aligns its memory to the same pixel boundaries as the screen!
			//WARNING: Pentium 4's L2 cache has severe slowdowns when 65536-64 <= (zbufoff&65535) < 64
		zbufoff = (((((int)zbuf)-dd.f-128)+255)&~255)+128;
		for(i=0,j=dd.f+zbufoff;i<dd.y;i++,j+=dd.p) memset16_safe((void *)j,0x7f7f7f7f,dd.x<<2); //Clear z-buffer
	}
	oct_drawcone_setup(cputype,oct_numcpu,(tiletype *)&dd,zbufoff,ipos,irig,idow,ifor,ghx,ghy,ghz);
	oct_setcam(&dd,zbufoff,ipos,irig,idow,ifor,ghx,ghy,ghz);

	if ((oct_fogdist >= 1e32) && (gskyid)) drawsky();
	else if (!oct_usegpu) clearscreen(oct_fogcol);

	if (oct_usegpu) glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	return(0);
}
int oct_startdraw (point3d *ipos, point3d *irig, point3d *idow, point3d *ifor, float ghx, float ghy, float ghz)
{
	dpoint3d dpos, drig, ddow, dfor;
	dpos.x = ipos->x; dpos.y = ipos->y; dpos.z = ipos->z;
	drig.x = irig->x; drig.y = irig->y; drig.z = irig->z;
	ddow.x = idow->x; ddow.y = idow->y; ddow.z = idow->z;
	dfor.x = ifor->x; dfor.y = ifor->y; dfor.z = ifor->z;
	return(oct_startdraw(&dpos,&drig,&ddow,&dfor,ghx,ghy,ghz));
}

void oct_stopdraw (void)
{
	if (!oct_usegpu)
	{
		stopdirectdraw();
		nextpage();
	}
	else { SwapBuffers(glhDC); }
}

	//Rotate vectors a & b around their common plane, by ang
void oct_rotvex (float ang, point3d *a, point3d *b) //Rotate vectors a & b around their common plane, by ang
{
	float c, s, f;
	c = cos(ang); s = sin(ang);
	f = a->x; a->x = f*c + b->x*s; b->x = b->x*c - f*s;
	f = a->y; a->y = f*c + b->y*s; b->y = b->y*c - f*s;
	f = a->z; a->z = f*c + b->z*s; b->z = b->z*c - f*s;
}
void oct_rotvex (double ang, dpoint3d *a, dpoint3d *b) //Rotate vectors a & b around their common plane, by ang
{
	double c, s, f;
	c = cos(ang); s = sin(ang);
	f = a->x; a->x = f*c + b->x*s; b->x = b->x*c - f*s;
	f = a->y; a->y = f*c + b->y*s; b->y = b->y*c - f*s;
	f = a->z; a->z = f*c + b->z*s; b->z = b->z*c - f*s;
}

void oct_vox2worldpos (spr_t *sp, point3d *pvox, point3d *pworld) //transform pos
{
	point3d opvox = *pvox;
	pworld->x = opvox.x*sp->r.x + opvox.y*sp->d.x + opvox.z*sp->f.x + sp->p.x;
	pworld->y = opvox.x*sp->r.y + opvox.y*sp->d.y + opvox.z*sp->f.y + sp->p.y;
	pworld->z = opvox.x*sp->r.z + opvox.y*sp->d.z + opvox.z*sp->f.z + sp->p.z;
}
void oct_vox2worldpos (spr_t *sp, dpoint3d *pvox, dpoint3d *pworld) //transform pos
{
	dpoint3d opvox = *pvox;
	pworld->x = opvox.x*sp->r.x + opvox.y*sp->d.x + opvox.z*sp->f.x + sp->p.x;
	pworld->y = opvox.x*sp->r.y + opvox.y*sp->d.y + opvox.z*sp->f.y + sp->p.y;
	pworld->z = opvox.x*sp->r.z + opvox.y*sp->d.z + opvox.z*sp->f.z + sp->p.z;
}
void oct_vox2worlddir (spr_t *sp, point3d *pvox, point3d *pworld) //transform vec (ignore origin)
{
	point3d opvox = *pvox;
	pworld->x = opvox.x*sp->r.x + opvox.y*sp->d.x + opvox.z*sp->f.x;
	pworld->y = opvox.x*sp->r.y + opvox.y*sp->d.y + opvox.z*sp->f.y;
	pworld->z = opvox.x*sp->r.z + opvox.y*sp->d.z + opvox.z*sp->f.z;
}
void oct_vox2worlddir (spr_t *sp, dpoint3d *pvox, dpoint3d *pworld) //transform vec (ignore origin)
{
	dpoint3d opvox = *pvox;
	pworld->x = opvox.x*sp->r.x + opvox.y*sp->d.x + opvox.z*sp->f.x;
	pworld->y = opvox.x*sp->r.y + opvox.y*sp->d.y + opvox.z*sp->f.y;
	pworld->z = opvox.x*sp->r.z + opvox.y*sp->d.z + opvox.z*sp->f.z;
}

void oct_world2voxpos (spr_t *sp, point3d *pworld, point3d *pvox) //transform pos
{
	double fx, fy, fz, mat[9];

	invert3x3(&sp->r,&sp->d,&sp->f,mat);
	fx = pworld->x-sp->p.x; fy = pworld->y-sp->p.y; fz = pworld->z-sp->p.z;
	pvox->x = fx*mat[0] + fy*mat[1] + fz*mat[2];
	pvox->y = fx*mat[3] + fy*mat[4] + fz*mat[5];
	pvox->z = fx*mat[6] + fy*mat[7] + fz*mat[8];
}
void oct_world2voxpos (spr_t *sp, dpoint3d *pworld, dpoint3d *pvox) //transform pos
{
	double fx, fy, fz, mat[9];

	invert3x3(&sp->r,&sp->d,&sp->f,mat);
	fx = pworld->x-sp->p.x; fy = pworld->y-sp->p.y; fz = pworld->z-sp->p.z;
	pvox->x = fx*mat[0] + fy*mat[1] + fz*mat[2];
	pvox->y = fx*mat[3] + fy*mat[4] + fz*mat[5];
	pvox->z = fx*mat[6] + fy*mat[7] + fz*mat[8];
}
void oct_world2voxdir (spr_t *sp, point3d *pworld, point3d *pvox) //transform vec (ignore origin)
{
	double fx, fy, fz, mat[9];

	invert3x3(&sp->r,&sp->d,&sp->f,mat);
	fx = pworld->x; fy = pworld->y; fz = pworld->z;
	pvox->x = fx*mat[0] + fy*mat[1] + fz*mat[2];
	pvox->y = fx*mat[3] + fy*mat[4] + fz*mat[5];
	pvox->z = fx*mat[6] + fy*mat[7] + fz*mat[8];
}
void oct_world2voxdir (spr_t *sp, dpoint3d *pworld, dpoint3d *pvox) //transform vec (ignore origin)
{
	double fx, fy, fz, mat[9];

	invert3x3(&sp->r,&sp->d,&sp->f,mat);
	fx = pworld->x; fy = pworld->y; fz = pworld->z;
	pvox->x = fx*mat[0] + fy*mat[1] + fz*mat[2];
	pvox->y = fx*mat[3] + fy*mat[4] + fz*mat[5];
	pvox->z = fx*mat[6] + fy*mat[7] + fz*mat[8];
}

//==================================================================================================
//==================================================================================================
//==================================================================================================

#if (STANDALONE != 0)

static int showborders = 0, hovercheck = 0, normcheck = 0, docollide = 1, currad = 16, gravity = 1;
static double speed = 1.0, fatness = 0.5;

static INT_PTR gcolid;

static dpoint3d sipos, sirig, sidow, sifor; //starting pos
static dpoint3d ipos, irig, idow, ifor; //camera
static dpoint3d mpos, mrig, mdow, mfor; //mouse cursor

	//FPS counter
#define FPSSIZ 128
static float fpsometer[FPSSIZ];
static int fpsind[FPSSIZ], numframes = 0, showfps = 0;

#define OCTMAX 256
static oct_t oct[OCTMAX];
static int octnum = 0;

#define SPRMAX 256
static spr_t spr[SPRMAX];
static int sprnum = 0, sprord[SPRMAX], curspr = 0, ocurspr = 1;
static float sprdep[SPRMAX];

	//Draw sprites in approximate front to back order .. handles clearing of screen, and all bit cover arrays
static void drawsprites (dpoint3d *ipos, dpoint3d *irig, dpoint3d *idow, dpoint3d *ifor, int selmode)
{
	int i, j, k, n, y, gap;

	if (oct_usegpu) glBlendFunc(GL_ONE,GL_NONE);
	j = 0; k = spr[j].imulcol; if (selmode >= 0) k = ((k&0xfefefe)>>1)+(k&0xff000000);
	oct_drawoct(spr[j].oct,&spr[j].p,&spr[j].r,&spr[j].d,&spr[j].f,spr[j].mixval,k); //spr[j].imulcol);

	if (oct_usegpu) glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	n = sprnum-1;
	for(i=sprnum-1;i>0;i--)
	{
		sprord[i-1] = i;
	 //sprdep[i-1] = (spr[i].p.x-ipos->x)*ifor->x + (spr[i].p.y-ipos->y)*ifor->y + (spr[i].p.z-ipos->z)*ifor->z;
		sprdep[i-1] = (spr[i].p.x-ipos->x)*(spr[i].p.x-ipos->x) + (spr[i].p.y-ipos->y)*(spr[i].p.y-ipos->y) + (spr[i].p.z-ipos->z)*(spr[i].p.z-ipos->z);
	}

		//Draw sprites in approximate back to front order for transparency
	for(gap=(n>>1);gap;gap>>=1)
		for(i=0;i<n-gap;i++)
			for(j=i;j>=0;j-=gap)
			{
				if (sprdep[sprord[j]] <= sprdep[sprord[j+gap]]) break;
				k = sprord[j]; sprord[j] = sprord[j+gap]; sprord[j+gap] = k;
			}
	for(i=n-1;i>=0;i--)
	{
		j = sprord[i]; k = spr[j].imulcol; if (selmode >= 0) k = ((k&0xfefefe)>>1)+(k&0xff000000);
		oct_drawoct(spr[j].oct,&spr[j].p,&spr[j].r,&spr[j].d,&spr[j].f,spr[j].mixval,k); //spr[j].imulcol);
	}
}

//----------------------  WIN file select code begins ------------------------

#include <commdlg.h>

static char relpathbase[MAX_PATH];
static void relpathinit (char *st)
{
	int i;

	for(i=0;st[i];i++) relpathbase[i] = st[i];
	if ((i) && (relpathbase[i-1] != '/') && (relpathbase[i-1] != '\\'))
		relpathbase[i++] = '\\';
	relpathbase[i] = 0;
}

static char fileselectnam[MAX_PATH+1];
static int fileselect1stcall = -1; //Stupid directory hack
static char *loadfileselect (char *mess, char *spec, char *defext)
{
	int i;
	for(i=0;fileselectnam[i];i++) if (fileselectnam[i] == '/') fileselectnam[i] = '\\';
	OPENFILENAME ofn =
	{
		sizeof(OPENFILENAME),ghwnd,0,spec,0,0,1,fileselectnam,MAX_PATH,0,0,(char *)(((int)relpathbase)&fileselect1stcall),mess,
		/*OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|*/ OFN_HIDEREADONLY,0,0,defext,0,0,0
	};
	fileselect1stcall = 0; //Let windows remember directory after 1st call
	if (!GetOpenFileName(&ofn)) return(0); else return(fileselectnam);
}
static char *savefileselect (char *mess, char *spec, char *defext)
{
	int i;
	for(i=0;fileselectnam[i];i++) if (fileselectnam[i] == '/') fileselectnam[i] = '\\';
	OPENFILENAME ofn =
	{
		sizeof(OPENFILENAME),ghwnd,0,spec,0,0,1,fileselectnam,MAX_PATH,0,0,(char *)(((int)relpathbase)&fileselect1stcall),mess,
		OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT,0,0,defext,0,0,0
	};
	fileselect1stcall = 0; //Let windows remember directory after 1st call
	if (!GetSaveFileName(&ofn)) return(0); else return(fileselectnam);
}

//------------------------- file select code begins --------------------------

static int spr_getmoi (spr_t *spr)
{
	double f, g, ixx, iyy, izz, ixy, ixz, iyz;

	oct_getmoi(spr->oct,&spr->mas,&spr->cen.x,&spr->cen.y,&spr->cen.z,&ixx,&iyy,&izz,&ixy,&ixz,&iyz);

		//FIXFIXFIXFIX:valid?
		//convert scale of moi from body to world coords
	f = sqrt(spr->r.x*spr->r.x + spr->r.y*spr->r.y + spr->r.z*spr->r.z);
	g = f*f*f; spr->mas *= g;
	g *= f*f; ixx *= g; iyy *= g; izz *= g; ixy *= g; ixz *= g; iyz *= g;

	spr->rmas = 1.0/spr->mas;

	spr->moi[0] = ixx; spr->moi[1] = ixy; spr->moi[2] = ixz;
	spr->moi[3] = ixy; spr->moi[4] = iyy; spr->moi[5] = iyz;
	spr->moi[6] = ixz; spr->moi[7] = iyz; spr->moi[8] = izz;
	if (fabs(ixx*(iyy*izz-iyz*iyz)+ixy*(iyz*ixz-ixy*izz)+ixz*(ixy*iyz-iyy*ixz)) == 0.0) //when mas < 3 or solid voxels are pure collinear, rmoi cannot be calculated :/
	{
		int i;
		for(i=0;i<9;i++) spr->rmoi[i] = (!(i&3));
		return(-1);
	}
	invert3x3sym((dpoint3d *)&spr->moi[0],(dpoint3d *)&spr->moi[3],(dpoint3d *)&spr->moi[6],spr->rmoi);
	return(0);
}
static void spr_update_rdf (spr_t *spr)
{
	spr->r.x = spr->br.x*spr->ori[0] + spr->br.y*spr->ori[1] + spr->br.z*spr->ori[2];
	spr->r.y = spr->br.x*spr->ori[3] + spr->br.y*spr->ori[4] + spr->br.z*spr->ori[5];
	spr->r.z = spr->br.x*spr->ori[6] + spr->br.y*spr->ori[7] + spr->br.z*spr->ori[8];
	spr->d.x = spr->bd.x*spr->ori[0] + spr->bd.y*spr->ori[1] + spr->bd.z*spr->ori[2];
	spr->d.y = spr->bd.x*spr->ori[3] + spr->bd.y*spr->ori[4] + spr->bd.z*spr->ori[5];
	spr->d.z = spr->bd.x*spr->ori[6] + spr->bd.y*spr->ori[7] + spr->bd.z*spr->ori[8];
	spr->f.x = spr->bf.x*spr->ori[0] + spr->bf.y*spr->ori[1] + spr->bf.z*spr->ori[2];
	spr->f.y = spr->bf.x*spr->ori[3] + spr->bf.y*spr->ori[4] + spr->bf.z*spr->ori[5];
	spr->f.z = spr->bf.x*spr->ori[6] + spr->bf.y*spr->ori[7] + spr->bf.z*spr->ori[8];
}

static void sprite_del (int k)
{
	int i, j;

	sprnum--; spr[k] = spr[sprnum];
	for(i=octnum-1;i>=0;i--)
	{
		for(j=sprnum-1;j>=0;j--) if (spr[j].oct == &oct[i]) break;
		if (j >= 0) continue;
		oct_free(&oct[i]);
		octnum--; oct[i] = oct[octnum];
		for(j=sprnum-1;j>=0;j--) if (spr[j].oct == &oct[octnum]) spr[j].oct = &oct[i];
	}
}

//--------------------------------------------------------------------------------------------------

static float fps_period_est (double dtim)
{
	float f;
	int i, j, k, m;

		//FPS counter
	fpsometer[numframes&(FPSSIZ-1)] = (float)dtim; numframes++;
		//Fast sort when already sorted... otherwise slow!
	j = min(numframes,FPSSIZ)-1;
	for(k=0;k<j;k++)
		if (*(int *)&fpsometer[fpsind[k]] > *(int *)&fpsometer[fpsind[k+1]])
		{
			m = fpsind[k+1];
			for(i=k;i>=0;i--)
			{
				fpsind[i+1] = fpsind[i];
				if (*(int *)&fpsometer[fpsind[i]] <= *(int *)&fpsometer[m]) break;
			}
			fpsind[i+1] = m;
		}

		//Average of samples from 1/4 to 3/4 of array
	f = 0.f; i = j-(j>>2); m = 0;
	for(k=(j>>2);k<=i;k++) { f += fpsometer[fpsind[k]]; m++; } f /= (float)m;
	return(f);
}

static void resetfps (void)
{
	int i;
	memset(fpsometer,0x7f,sizeof(fpsometer)); for(i=0;i<FPSSIZ;i++) fpsind[i] = i; numframes = 0;
}

	//Quantizes rotation matrix to one of the 48 possible identity permutations
static void oriquant (dpoint3d *lrig, dpoint3d *ldow, dpoint3d *lfor)
{
	double d, maxd, b[9], c[9], in[9];
	int i, p, s;

	in[0] = lrig->x; in[1] = lrig->y; in[2] = lrig->z;
	in[3] = ldow->x; in[4] = ldow->y; in[5] = ldow->z;
	in[6] = lfor->x; in[7] = lfor->y; in[8] = lfor->z;

	maxd = -1e32;
	for(i=0;i<9;i++) b[i] = (double)(!(i&3));
	for(p=0;p<6;p++)
	{
		for(s=0;s<4;s++)
		{
			d = 0.0; for(i=0;i<9;i++) d += in[i]*b[i];
			if (+d > maxd) { maxd = +d; for(i=0;i<9;i++) c[i] = +b[i]; }
			if (-d > maxd) { maxd = -d; for(i=0;i<9;i++) c[i] = -b[i]; }
			for(i=(s&1)*3;i<(s&1)*3+3;i++) b[i] = -b[i];
		}
		for(i=0;i<3;i++) { d = b[i]; b[i] = b[i+3]; b[i+3] = d; }
		if (p != 2) { for(i=3;i<6;i++) { d = b[i]; b[i] = b[i+3]; b[i+3] = d; } }
	}
	lrig->x = c[0]; lrig->y = c[1]; lrig->z = c[2];
	ldow->x = c[3]; ldow->y = c[4]; ldow->z = c[5];
	lfor->x = c[6]; lfor->y = c[7]; lfor->z = c[8];
}

static void orthofit (double *m, long c)
{
	double d, nm[9];
	int i;

		//(Cheap & simplified version of the 03/18/2006 algo)
		//Note: this version assumes input matrix has positive determinant
	for(;c>0;c--)
	{
		for(i=9-1;i>=0;i--) nm[i] = m[i];
		m[0] += nm[4]*nm[8] - nm[5]*nm[7];
		m[1] += nm[5]*nm[6] - nm[3]*nm[8];
		m[2] += nm[3]*nm[7] - nm[4]*nm[6];
		m[3] += nm[7]*nm[2] - nm[8]*nm[1];
		m[4] += nm[8]*nm[0] - nm[6]*nm[2];
		m[5] += nm[6]*nm[1] - nm[7]*nm[0];
		m[6] += nm[1]*nm[5] - nm[2]*nm[4];
		m[7] += nm[2]*nm[3] - nm[0]*nm[5];
		m[8] += nm[0]*nm[4] - nm[1]*nm[3];
		d = 1.0 / sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
		for(i=9-1;i>=0;i--) m[i] *= d;
	}
}

	//10/26/2011:optimized algo :)
	//WARNING:Assumes axis is unit length!
static void axisrotate (dpoint3d *p, dpoint3d *ax, double w)
{
	dpoint3d op;
	double c, s, d;

	c = cos(w); s = sin(w);

		//P = cross(AX,P)*s + dot(AX,P)*(1-c)*AX + P*c;
	op = (*p);
	d = (op.x*ax->x + op.y*ax->y + op.z*ax->z)*(1.0-c);
	p->x = (ax->y*op.z - ax->z*op.y)*s + ax->x*d + op.x*c;
	p->y = (ax->z*op.x - ax->x*op.z)*s + ax->y*d + op.y*c;
	p->z = (ax->x*op.y - ax->y*op.x)*s + ax->z*d + op.z*c;
}

static void collide_binsearch (oct_t *o0, dpoint3d *op, dpoint3d *or, dpoint3d *od, dpoint3d *of,
														dpoint3d *np, dpoint3d *nr, dpoint3d *nd, dpoint3d *nf,
										 oct_t *o1, dpoint3d *p1, dpoint3d *r1, dpoint3d *d1, dpoint3d *f1, oct_hit_t *ohit)
{
	dpoint3d hitp, hitr, hitd, hitf;
	int k;

		//Binary search..
	for(k=8;k>0;k--)
	{
		hitp.x = (op->x + np->x)*.5;
		hitp.y = (op->y + np->y)*.5;
		hitp.z = (op->z + np->z)*.5;
		hitr = *or;
		hitd = *od;
		hitf = *of; //FIX:hack!

		if (oct_touch_oct(o0,&hitp,&hitr,&hitd,&hitf,o1,p1,r1,d1,f1,ohit))
			  { *np = hitp; *nr = hitr; *nd = hitd; *nf = hitf; }
		else { *op = hitp; *or = hitr; *od = hitd; *of = hitf; }
	}
}

	//Binary search collision..
	//  s0:     read: 1st object, starting posori, proposed delta movement
	//  s1:     read: 2nd (static) object to test collision with
	//  sfree: write: max posori not colliding
	//  shit:  write: min posori     colliding (= {0} + 1 lsd binsrch unit)
	//ohit: write: octree nodes, in order of {o0,o1}
	//Returns ratio travelled {0.0-1.0}
static double collide_binsearch (spr_t *s0, spr_t *s1, spr_t *sfree, spr_t *shit, double dtim, oct_hit_t *ohit)
{
	spr_t tspr;
	dpoint3d uax;
	double f, t, nt, tstep, mat[9], w;
	int j, k;

	w = s0->ax.x*s0->ax.x + s0->ax.y*s0->ax.y + s0->ax.z*s0->ax.z;
	if (w != 0.0)
	{
		w = sqrt(w);
		f = 1.0/w; uax.x = s0->ax.x*f; uax.y = s0->ax.y*f; uax.z = s0->ax.z*f;
	}

	t = 0.0; tstep = 1.0;
	for(k=8;k>0;k--,tstep*=.5)
	{
		nt = t+tstep;
		//---------------------------------------------------
		tspr.p.x = s0->p.x + s0->v.x*nt*dtim;
		tspr.p.y = s0->p.y + s0->v.y*nt*dtim;
		tspr.p.z = s0->p.z + s0->v.z*nt*dtim;

			//adjust pivot before rotation
		tspr.p.x += (s0->r.x*s0->cen.x + s0->d.x*s0->cen.y + s0->f.x*s0->cen.z);
		tspr.p.y += (s0->r.y*s0->cen.x + s0->d.y*s0->cen.y + s0->f.y*s0->cen.z);
		tspr.p.z += (s0->r.z*s0->cen.x + s0->d.z*s0->cen.y + s0->f.z*s0->cen.z);

		if (w != 0.0)
		{
			f = w*nt*dtim;
			for(j=0;j<9;j++) mat[j] = s0->ori[(j/3)+(j%3)*3];
			axisrotate((dpoint3d *)&mat[0],&uax,f); //screw in
			axisrotate((dpoint3d *)&mat[3],&uax,f);
			axisrotate((dpoint3d *)&mat[6],&uax,f);
			for(j=0;j<9;j++) tspr.ori[j] = mat[(j/3)+(j%3)*3];
		}
		else
		{
			memcpy(tspr.ori,s0->ori,sizeof(tspr.ori));
		}
		tspr.br = s0->br; tspr.bd = s0->bd; tspr.bf = s0->bf;
		spr_update_rdf(&tspr);

			//adjust pivot after rotation
		tspr.p.x -= (tspr.r.x*s0->cen.x + tspr.d.x*s0->cen.y + tspr.f.x*s0->cen.z);
		tspr.p.y -= (tspr.r.y*s0->cen.x + tspr.d.y*s0->cen.y + tspr.f.y*s0->cen.z);
		tspr.p.z -= (tspr.r.z*s0->cen.x + tspr.d.z*s0->cen.y + tspr.f.z*s0->cen.z);
		//---------------------------------------------------
		j = oct_touch_oct(s0->oct,&tspr.p,&tspr.r,&tspr.d,&tspr.f,s1->oct,&s1->p,&s1->r,&s1->d,&s1->f,ohit);
		if (!j) { (*sfree) = tspr; t = nt; if (t == 1.0) break; }
			else { (*shit)  = tspr; }
	}

	if (t == 0.0)
	{
		(*sfree) = (*s0);
	}
	else
	{
		orthofit(sfree->ori,1); //not required, but doesn't hurt
		spr_update_rdf(sfree);
	}
	return(t);
}

static void resetcamera (int mode)
{
	ghx = ((double)xres)*.5; ghy = ((double)yres)*.5; ghz = ghx;

	if (!mode) //reset pos&ori
	{
		ipos = sipos; irig = sirig; idow = sidow; ifor = sifor;
	}
	else //keep pos; quantize ori to axes
	{
		oriquant(&irig,&idow,&ifor);
	}

		//3D cursor
	mpos.x = 0.0; mpos.y = 0.0; mpos.z = 0.0;
	mrig.x = 1.0; mrig.y = 0.0; mrig.z = 0.0;
	mdow.x = 0.0; mdow.y = 1.0; mdow.z = 0.0;
	mfor.x = 0.0; mfor.y = 0.0; mfor.z = 1.0;
}

static unsigned short simp_boxsum[40+1][40+1][40+1];
static int simp_inited = 0;
static void simp_genboxsum (void) //simple example
{
	char bmp[40][40][40];
	int x, y, z;

	for(z=40-1;z>=0;z--)
		for(y=40-1;y>=0;y--)
			for(x=40-1;x>=0;x--)
			{
				bmp[z][y][x] = 0;
				if ((x-20)*(x-20) + (y-20)*(y-20) < 16*16) bmp[z][y][x] = 255;
				if ((x-20)*(x-20) + (z-20)*(z-20) < 16*16) bmp[z][y][x] = 255;
				if ((y-20)*(y-20) + (z-20)*(z-20) < 16*16) bmp[z][y][x] = 255;
			}
	brush_bmp_calcsum(&simp_boxsum[0][0][0],&bmp[0][0][0],0,40,40,40);
}

	//generate sprite callback function for oct_hover_check()
static void recvoctfunc (oct_t *ooct, oct_t *noct)
{
	octv_t nnode;
	int j, x, y, z;
	float f, fx, fy, fz;

	if ((sprnum >= SPRMAX) || (octnum >= OCTMAX)) { oct_free(noct); return; }

	oct[octnum] = (*noct);
	spr[sprnum] = spr[curspr];
	spr[sprnum].oct = &oct[octnum];
	spr[sprnum].oct->tilid = ooct->tilid;
	spr[sprnum].tim = -2.0;
	spr[sprnum].cnt = 0;

	oct_rebox(spr[sprnum].oct,0x7fffffff,0x7fffffff,0x7fffffff,0,0,0,&x,&y,&z);
	spr[sprnum].p.x -= (spr[sprnum].r.x*x + spr[sprnum].d.x*y + spr[sprnum].f.x*z);
	spr[sprnum].p.y -= (spr[sprnum].r.y*x + spr[sprnum].d.y*y + spr[sprnum].f.y*z);
	spr[sprnum].p.z -= (spr[sprnum].r.z*x + spr[sprnum].d.z*y + spr[sprnum].f.z*z);
	if (spr_getmoi(&spr[sprnum]) < 0) { oct_free(noct); return; } //Reject masses too small for valid rmoi (would cause evil inf/ind numbers in physics)

#if 1
	spr[sprnum].v.x = 0; spr[sprnum].v.y = 0; spr[sprnum].v.z = 0;
#else
	spr[sprnum].v.x = ((rand()&32767)-16384)/262144.0;
	spr[sprnum].v.y = ((rand()&32767)-16384)/262144.0;
	spr[sprnum].v.z = ((rand()&32767)-16384)/262144.0;
#endif

#if 1
	spr[sprnum].ax.x = 0; spr[sprnum].ax.y = 0; spr[sprnum].ax.z = 0;
#else
		//UNIFORM spherical randomization (see spherand.c)
	spr[sprnum].ax.z = ((rand()&32767)-16384)/16384.0;
	f = (rand()&32767)*(PI*2.0/32768.0); spr[sprnum].ax.x = cos(f); spr[sprnum].ax.y = sin(f);
	f = sqrt(1.0-spr[sprnum].ax.z*spr[sprnum].ax.z); spr[sprnum].ax.x *= f; spr[sprnum].ax.y *= f;
	f = ((rand()&32767)-16384)/1048576.0; spr[sprnum].ax.x *= f; spr[sprnum].ax.y *= f; spr[sprnum].ax.z *= f;
#endif

	for(j=0;j<9;j++) spr[sprnum].ori[j] = (double)(!(j&3));

	sprnum++; octnum++;
}

	//SLAB6 style molding tool
	//NOTE:rad must be <= 13
	//if (isadd) caller expected to copy surf from rand neighbor
static int voxfindspray (oct_t *loct, int hitx, int hity, int hitz, int rad, int *retx, int *rety, int *retz, int isadd)
{
	static const char msk[7][7] =
	{
		0x00,0x1c,0x3e,0x3e,0x3e,0x1c,0x00,
		0x1c,0x3e,0x7f,0x7f,0x7f,0x3e,0x1c,
		0x3e,0x7f,0x7f,0x7f,0x7f,0x7f,0x3e,
		0x3e,0x7f,0x7f,0x7f,0x7f,0x7f,0x3e,
		0x3e,0x7f,0x7f,0x7f,0x7f,0x7f,0x3e,
		0x1c,0x3e,0x7f,0x7f,0x7f,0x3e,0x1c,
		0x00,0x1c,0x3e,0x3e,0x3e,0x1c,0x00,
	};
	unsigned int bitsol[32*32], bituse[32*32];
	int i, x, y, z, xb, yb, zb, yy, zz, pit, mas, bmas, bx, by, bz;

	rad = min(max(rad,0),13);
	pit = (rad-1+3)*2+1;
	oct_sol2bit(loct,bitsol,hitx-(rad-1+3),hity-(rad-1+3),hitz-(rad-1+3),pit,pit,pit,1);

	for(z=3;z<pit-3;z++)
		for(y=3;y<pit-3;y++)
		{
			i = z*pit + y;
			if (isadd) bituse[i] = ((bitsol[i]>>1) | (bitsol[i]<<1) | bitsol[i-1] | bitsol[i+1] | bitsol[i-pit] | bitsol[i+pit]) &~bitsol[i]; //cur==air && a neigh==sol
					else bituse[i] =~((bitsol[i]>>1) & (bitsol[i]<<1) & bitsol[i-1] & bitsol[i+1] & bitsol[i-pit] & bitsol[i+pit]) & bitsol[i]; //cur==sol && a neigh==air
		}

	if (isadd) bmas = -1; else bmas = 0x7fffffff;
	for(z=1-rad;z<rad;z++)
		for(y=1-rad;y<rad;y++)
			for(x=1-rad;x<rad;x++)
			{
				if (x*x + y*y + z*z >= rad*rad) continue;
				xb = x+rad-1+3;
				yb = y+rad-1+3;
				zb = z+rad-1+3;
				i = zb*pit + yb; if (!(bituse[i]&(1<<xb))) continue;

					//mass = # voxels in radius <4 sphere
				mas = 0; i -= (pit*3+3); xb -= 3;
				for(zz=7-1;zz>=0;zz--)
					for(yy=7-1;yy>=0;yy--)
						mas += popcount[(bitsol[zz*pit+yy+i]>>xb)&msk[zz][yy]];

				if ((mas >= bmas) == isadd) { bmas = mas; bx = x; by = y; bz = z; }
			}

	(*retx) = bx+hitx; (*rety) = by+hity; (*retz) = bz+hitz;
	return((bmas<<1)>>1); //turn 0x7fffffff into -1
}

static int oct_print4debug_x, oct_print4debug_y;
static void oct_print4debug (oct_t *loct, int inode, int xx, int yy, int zz, int ls)
{
	surf_t *psurf;
	octv_t *iptr;
	int i, x, y, z, o;

	ls--;
	if (ls < 0)
	{
		psurf = &((surf_t *)loct->sur.buf)[inode];
		oct_drawtext6x8(oct_print4debug_x,oct_print4debug_y,0xffffffff,0,"%08x",(*(int *)psurf)&0xffffffff);
		oct_print4debug_x += 56;
		return;
	}

	iptr = &((octv_t *)loct->nod.buf)[inode];
	oct_print4debug_x = 0;
	oct_print4debug_y += 8; if (oct_print4debug_y >= gdd.y) return;
	oct_drawtext6x8(oct_print4debug_x,oct_print4debug_y,0xffffffff,0,"%*.s|%*.s%6d %d %d%d%d%d%d%d%d%d %d%d%d%d%d%d%d%d %d%d%d%d%d%d%d%d %d %d",
		loct->lsid-ls,"            ",ls,"            ",
		iptr-loct->nod.buf,ls,
		iptr->chi&1,(iptr->chi>>1)&1,(iptr->chi>>2)&1,(iptr->chi>>3)&1,(iptr->chi>>4)&1,(iptr->chi>>5)&1,(iptr->chi>>6)&1,(iptr->chi>>7)&1,
		iptr->sol&1,(iptr->sol>>1)&1,(iptr->sol>>2)&1,(iptr->sol>>3)&1,(iptr->sol>>4)&1,(iptr->sol>>5)&1,(iptr->sol>>6)&1,(iptr->sol>>7)&1,
		iptr->mrk&1,(iptr->mrk>>1)&1,(iptr->mrk>>2)&1,(iptr->mrk>>3)&1,(iptr->mrk>>4)&1,(iptr->mrk>>5)&1,(iptr->mrk>>6)&1,(iptr->mrk>>7)&1,
		iptr->ind,
		(loct->nod.bit[inode>>5]>>inode)&1);
	oct_print4debug_x += 320;

	o = iptr->ind; i = 1;
	for(z=0;z<2;z++)
		for(y=0;y<2;y++)
			for(x=0;x<2;x++,i<<=1)
			{
				if (!(iptr->chi&i)) continue;
				oct_print4debug(loct,o,(x<<ls)+xx,(y<<ls)+yy,(z<<ls)+zz,ls);
				o++;
			}
}

static void vecrand (point3d *a) //UNIFORM spherical randomization (see spherand.c)
{
	float f;

	a->z = (float)rand()/(float)RAND_MAX*2.0-1.0;
	f = (float)rand()/(float)RAND_MAX*(PI*2.0); a->x = cos(f); a->y = sin(f);
	f = sqrt(1.0 - a->z*a->z); a->x *= f; a->y *= f;
}
static void matrand (point3d *a, point3d *b, point3d *c)
{
	float f;

	vecrand(a);
	vecrand(c);
	b->x = a->y*c->z - a->z*c->y;
	b->y = a->z*c->x - a->x*c->z;
	b->z = a->x*c->y - a->y*c->x;
	f = 1.0 / sqrt(b->x*b->x + b->y*b->y + b->z*b->z);
	b->x*= f; b->y*= f; b->z *= f;
	c->x = a->y*b->z - a->z*b->y;
	c->y = a->z*b->x - a->x*b->z;
	c->z = a->x*b->y - a->y*b->x;
}

//--------------------------------------------------------------------------------------------------

void uninitapp (void)
{
	int i;

	for(i=octnum-1;i>=0;i--) oct_free(&oct[i]);

	oct_uninitonce();
}

	//See COLWHEEL.KC for derivation
static int colpick_xy2rgb (float x, float y, float intens, float *r, float *g, float *b)
{
	float f;

	f = x*(-1.0/sqrt(3.0));
	if (min(y,-f) > f) { (*r) = 1.0+f+f; (*g) = 1.0+f-y; (*b) = 1.0    ; }
	  else if (-y < f) { (*r) = 1.0    ; (*g) = 1.0-f-y; (*b) = 1.0-f-f; }
					  else { (*r) = 1.0+f+y; (*g) = 1.0    ; (*b) = 1.0-f+y; }
	f = intens/sqrt((*r)*(*r) + (*g)*(*g) + (*b)*(*b)); (*r) *= f; (*g) *= f; (*b) *= f;
	return(((*r) >= 0) && ((*g) >= 0) && ((*b) >= 0));
}
static void colpick_rgb2xy (float r, float g, float b, float *x, float *y, float *intens)
{
	float f;

	(*intens) = sqrt(r*r + g*g + b*b); f = -1.0/max(max(r,g),b);
	(*x) = (r-b)*f*(sqrt(3.0)*.5); (*y) = (g - (r+b)*.5)*f;
}

static int gdeflsid = 8, giloctsid = 0, gshowinfo = 0;
static char gtilefile[MAX_PATH] = "klabtiles.png", gskyfile[MAX_PATH] = "kensky.jpg";
static char ginfilnam[MAX_PATH] = {0}, goutfilnam[MAX_PATH] = {0};
int initapp2_cb (void)
{
	brush_sph_t sph;
	double d, d0, d1;
	int i, j, x, y, z, s, xsiz, ysiz;
	char *cbuf, *cptr;

	CATCHBEG {

	if (goutfilnam[0]) { oct_usegpu = 0; } //Hack to not load GPU shader system when doing file conversion

	i = oct_initonce(256.0); if (i) return(i);
	gtilid = oct_loadtiles(gtilefile);
	gskyid = oct_loadtex(gskyfile,KGL_BGRA32+KGL_LINEAR);

		//load color picker texture
	#define LCOLSIZ 7
	tiles_t colpic;
	colpic.ltilesid = LCOLSIZ; colpic.x = (1<<LCOLSIZ); colpic.y = colpic.x;
	colpic.p = colpic.x*sizeof(int); colpic.f = (INT_PTR)malloc(colpic.p*colpic.y);
	float rat = 1.0-1.0/colpic.x;
	float f = ((1<<LCOLSIZ)-1.0)/2.0;
	float ff = 1.0/(rat*f);
	for(y=0;y<colpic.y;y++)
		for(x=0;x<colpic.x;x++)
		{
			float r, g, b;
			colpick_xy2rgb((x-f)*ff,(y-f)*ff,256.0,&r,&g,&b);
			*(int *)(colpic.p*y + (x<<2) + colpic.f) = (((int)min(max(r,0),255))<<16) + (((int)min(max(g,0),255))<<8) + (int)min(max(b,0),255) + 0xff000000;
		}
	for(y=0;y<((1<<LCOLSIZ)>>3);y++) //fill top-left corner with white
		for(x=0;x<((1<<LCOLSIZ)>>3);x++)
			*(int *)(colpic.p*y + (x<<2) + colpic.f) = 0xffffffff;
	gcolid = oct_loadtex(colpic.f,colpic.x,colpic.y,KGL_BGRA32+KGL_LINEAR+KGL_CLAMP_TO_EDGE);
	free((void *)colpic.f);

	sipos.x = 0.0; sipos.y = 0.0; sipos.z =-1.4;
	sirig.x = 1.0; sirig.y = 0.0; sirig.z = 0.0;
	sidow.x = 0.0; sidow.y = 1.0; sidow.z = 0.0;
	sifor.x = 0.0; sifor.y = 0.0; sifor.z = 1.0;
	resetcamera(0);

	//-----------------------------------------------------------------------------------------------
	sprnum = 0; octnum = 0;
	spr[sprnum].p.x =-0.5; spr[sprnum].p.y =-0.5; spr[sprnum].p.z =-0.5;
	spr[sprnum].br.x = 1.0; spr[sprnum].br.y = 0.0; spr[sprnum].br.z = 0.0;
	spr[sprnum].bd.x = 0.0; spr[sprnum].bd.y = 1.0; spr[sprnum].bd.z = 0.0;
	spr[sprnum].bf.x = 0.0; spr[sprnum].bf.y = 0.0; spr[sprnum].bf.z = 1.0;
	spr[sprnum].oct = &oct[octnum];
	spr[sprnum].mixval = 0.375;
	spr[sprnum].imulcol = 0xff404040;
	oct[octnum].tilid = gtilid;

	if (ginfilnam[0])
	{
		dpoint3d sp, sr, sd, sf;
		readklock(&d0);
		oct_load(&oct[octnum],ginfilnam,&sp,&sr,&sd,&sf);
		readklock(&d1); testim = d1-d0;

		d = 1.0/(double)oct[octnum].sid;
		spr[sprnum].br.x *= d; spr[sprnum].br.y *= d; spr[sprnum].br.z *= d;
		spr[sprnum].bd.x *= d; spr[sprnum].bd.y *= d; spr[sprnum].bd.z *= d;
		spr[sprnum].bf.x *= d; spr[sprnum].bf.y *= d; spr[sprnum].bf.z *= d;

		if (spr[octnum].oct->flags&1)
		{
			for(j=0;j<9;j++) spr[sprnum].ori[j] = (double)(!(j&3));
			spr_update_rdf(&spr[sprnum]);

			oct_vox2worldpos(&spr[sprnum],&sp,&ipos);
			irig.x = sr.x; irig.y = sr.y; irig.z = sr.z;
			idow.x = sd.x; idow.y = sd.y; idow.z = sd.z;
			ifor.x = sf.x; ifor.y = sf.y; ifor.z = sf.z;

			speed = 1.0/16.0;
		}
		sipos = ipos; sirig = irig; sidow = idow; sifor = ifor;
	}
	else
	{
		oct_new(&oct[octnum],gdeflsid,gtilid,0,0,0);

		d = 1.0/(double)oct[octnum].sid;
		spr[sprnum].br.x *= d; spr[sprnum].br.y *= d; spr[sprnum].br.z *= d;
		spr[sprnum].bd.x *= d; spr[sprnum].bd.y *= d; spr[sprnum].bd.z *= d;
		spr[sprnum].bf.x *= d; spr[sprnum].bf.y *= d; spr[sprnum].bf.z *= d;

		readklock(&d0);
		for(i=(64-1)*(gdeflsid<10);i>=0;i--)
		{
			x = rand()&(oct[octnum].sid-1);
			y = rand()&(oct[octnum].sid-1);
			z = rand()&(oct[octnum].sid-1);
			s = (rand()&((oct[octnum].sid>>4)-1)) + (oct[octnum].sid>>4);
			brush_sph_init(&sph,x,y,z,s,1); oct_mod(&oct[octnum],(brush_t *)&sph,1+2);
		}
		readklock(&d1); testim = d1-d0;
	}

#if 0
	spr_getmoi(&spr[sprnum]);
#endif

	for(j=0;j<giloctsid;j++) //'instancing'
	{
		int ind, n;

		n = 8;
		ind = bitalloc(&oct[octnum],&oct[octnum].nod,n); oct[octnum].nod.num += n;
		for(i=0;i<n;i++) ((octv_t *)oct[octnum].nod.buf)[ind+i] = ((octv_t *)oct[octnum].nod.buf)[oct[octnum].head];

		((octv_t *)oct[octnum].nod.buf)[oct[octnum].head].chi = (1<<n)-1;
		((octv_t *)oct[octnum].nod.buf)[oct[octnum].head].sol = 0;
		((octv_t *)oct[octnum].nod.buf)[oct[octnum].head].ind = ind;

#if 0
			//NOTE:math is valid only if chi (above) is 255
		d = spr[sprnum].mas*oct[octnum].sid*oct[octnum].sid*4;
		spr[sprnum].mas *= 8.0;
		spr[sprnum].cen.x += oct[octnum].sid*.5;
		spr[sprnum].cen.y += oct[octnum].sid*.5;
		spr[sprnum].cen.z += oct[octnum].sid*.5;

		for(i=0;i<9;i++) spr[sprnum].moi[i] *= 8.0;
		for(i=0;i<9;i+=4) spr[sprnum].moi[i] += d; //do diagonal only
		spr[sprnum].rmas = 1/spr[sprnum].mas;
		invert3x3sym((dpoint3d *)&spr[sprnum].moi[0],(dpoint3d *)&spr[sprnum].moi[3],(dpoint3d *)&spr[sprnum].moi[6],spr[sprnum].rmoi);
#endif

		oct[octnum].lsid++; oct[octnum].sid = pow2[oct[octnum].lsid]; oct[octnum].nsid = -oct[octnum].sid;

		d = 0.5;
		spr[sprnum].br.x *= d; spr[sprnum].br.y *= d; spr[sprnum].br.z *= d;
		spr[sprnum].bd.x *= d; spr[sprnum].bd.y *= d; spr[sprnum].bd.z *= d;
		spr[sprnum].bf.x *= d; spr[sprnum].bf.y *= d; spr[sprnum].bf.z *= d;
	}

	for(j=0;j<9;j++) spr[sprnum].ori[j] = (double)(!(j&3));
	spr_update_rdf(&spr[sprnum]);
	spr[sprnum].tim = -2.0;
	spr[sprnum].cnt = 0;
	sprnum++; octnum++;
	//-----------------------------------------------------------------------------------------------

	if (gshowinfo)
	{
		MessageBox(ghwnd,"unimplemented",prognam,MB_OK);
		return(-1);
	}

	if (goutfilnam[0])
	{
		dpoint3d fp;
		s = 0; oct_world2voxpos(&spr[s],&sipos,&fp);
		oct_save(spr[s].oct,goutfilnam,&fp,&sirig,&sidow,&sifor);
		return(-1);
	}

#if 0
		//Duplicate sprite //FIXFIXFIX
	for(x=-1;x<=1;x++)
		for(y=-1;y<=1;y++)
			for(z=1;z<=1;z++)
			{
				spr[sprnum] = spr[curspr];
				spr[sprnum].p.x = x;
				spr[sprnum].p.y = y;
				spr[sprnum].p.z = z;
				spr_getmoi(&spr[sprnum]);
				sprnum++;
			}
#endif

	resetfps();

	} CATCHEND

	return(0);
}

long initapp (long argc, char **argv)
{
	void *brush;
	int i, j, k, x, y, z, showinfo = 0;
	char tbuf[MAX_PATH];

	xres = 1024; yres = 704; fullscreen = 0; colbits = 32; prognam = "PaintNDrag3D by Ken Silverman";

	GetCurrentDirectory(sizeof(tbuf),tbuf);
	relpathinit(tbuf);

	for(i=argc-1;i>0;i--)
	{
		if ((argv[i][0] != '/') && (argv[i][0] != '-')) { strcpy(ginfilnam,argv[i]); continue; }
		if (!memicmp(&argv[i][1],"zip",3)) { kzaddstack(&argv[i][5]); continue; }
		if (!stricmp(&argv[i][1],"win")) { fullscreen = 0; continue; }
		if (!stricmp(&argv[i][1],"full")) { fullscreen = 1; continue; }
		if (!stricmp(&argv[i][1],"cpu")) { oct_usegpu = 0; oct_usegpu_cmdline = 1; continue; }
		if (!stricmp(&argv[i][1],"gpu")) { oct_usegpu = 1; oct_usegpu_cmdline = 1; continue; }
		if (!memicmp(&argv[i][1],"filt",4)) { oct_usefilter = min(max(atol(&argv[i][6]),0),2); if (!oct_useglsl) oct_usefilter = 3; continue; }
		if (!stricmp(&argv[i][1],"arbasm")) { oct_useglsl = 0; oct_usefilter = 3; continue; }
		if (!stricmp(&argv[i][1],"glsl")) { oct_useglsl = 1; if (oct_usefilter == 3) oct_usefilter = 2; continue; }
		if (!memicmp(&argv[i][1],"thr",3)) { oct_numcpu = max(atol(&argv[i][5]),1); continue; }
		if (!memicmp(&argv[i][1],"info",4)) { gshowinfo = 1; continue; }
		if (!memicmp(&argv[i][1],"out",3)) { strcpy(goutfilnam,&argv[i][5]); continue; }
		if (!memicmp(&argv[i][1],"tile",4)) { strcpy(gtilefile,&argv[i][6]); continue; }
		if (!memicmp(&argv[i][1],"sky",3)) { strcpy(gskyfile,&argv[i][5]); continue; }
		if (!memicmp(&argv[i][1],"fogdist",7)) { oct_fogdist = atof(&argv[i][9]); continue; }
		if (!memicmp(&argv[i][1],"fogcol",6)) { oct_fogcol = strtol(&argv[i][8],0,0); continue; }
		if (!memicmp(&argv[i][1],"ils",3)) { giloctsid = min(max(atol(&argv[i][5]),1),OCT_MAXLS); continue; }
		if (!memicmp(&argv[i][1],"ls",2))
		{
			gdeflsid = min(max(atol(&argv[i][4]),1),14);
#if (PIXMETH == 1)
			if (oct_usegpu) gdeflsid = min(gdeflsid,12);
#endif
			continue;
		}
		if (!memicmp(&argv[i][1],"i",1)) swapinterval = atol(&argv[i][3]);

		if ((argv[i][1] >= '0') && (argv[i][1] <= '9'))
		{
			k = 0; z = 0;
			for(j=1;;j++)
			{
				if ((argv[i][j] >= '0') && (argv[i][j] <= '9'))
					{ k = (k*10+argv[i][j]-'0'); continue; }
				switch (z)
				{
					case 0: xres = k; break;
					case 1: yres = k; break;
					case 2: /*colbits = k;*/ fullscreen = 1; break;
				}
				if (!argv[i][j]) break;
				z++; if (z > 2) break;
				k = 0;
			}
		}
	}

	if (goutfilnam[0]) { initapp2_cb(); return(-1); } //Hack to not show window when doing file conversion

	initapp2 = initapp2_cb;

	return(0);
}

void doframe (void)
{
	static double tim = 0;
	static int bstatus = 0, obstatus = 0, but = 0, obut, shownorm = 0;
	static surf_t cursurf = {128,128,128,128,0,0,0,0};
	static int selmode = -1; //-1=off, 0=tex, 1=col
	static float selmul = 2.0, seladdx, seladdy, selcolintens;
	static ipoint3d selvox;
	oct_t *loct;
	surf_t surf, *psurf;
	static oct_hit_t ohit[2];
	double f, g, d0, d1, otim, dtim;
	ipoint3d lp;
	dpoint3d fp, fp2, vert[8];
	static dpoint3d hitp, hitr, hitd, hitf;
	float fmousx, fmousy, fmousz;
	static int fixhitit = 0;
	int i, j, k, m, s, x, y, z, sx, sy, isurf;
	char tbuf[1024];

	CATCHBEG {

	otim = tim; readklock(&tim); dtim = tim-otim;
	obstatus = bstatus; readmouse(&fmousx,&fmousy,&fmousz,(long *)&bstatus);
	readkeyboard();

	//if ((tim >= .25) && (otim < .25)) { setacquire(0,1); setacquire(1,1); } //FIX:Hack to capture mouse :P
	//if ((tim >= .50) && (otim < .50)) { setacquire(0,1); setacquire(1,1); } //FIX:Hack to capture mouse :P
	//if ((tim >= 1.0) && (otim < 1.0)) { setacquire(0,1); setacquire(1,1); } //FIX:Hack to capture mouse :P

		//Rotate
	if (selmode == 0) //tex
	{
		f = 1.0/(selmul*16.0);
		seladdx += fmousx*f;
		seladdy += fmousy*f;
		selmul = min(max(selmul*pow(1.002f,fmousz),0.5),16.0);
	}
	else if (selmode == 1) //col
	{
		f = 1.0/(selmul*64.0);
		seladdx += fmousx*f;
		seladdy += fmousy*f;
		selcolintens = min(max(selcolintens+fmousz*(1.0/2048.0),1.0/512.0),4.0);
		for(i=3-1;i>=0;i--)
		{
			fp.x = cos(((double)i)*(PI/3.0));
			fp.y = sin(((double)i)*(PI/3.0));
			f = seladdx*fp.x + seladdy*fp.y;
			f -= min(max(f,-sqrt(3.0)*.4999),sqrt(3.0)*.4999);
			if (f != 0.f) { seladdx -= fp.x*f; seladdy -= fp.y*f; }
		}
	}
	else
	{
		f = ghx/ghz*.008;
		oct_rotvex(fmousy*f,&ifor,&idow);
		if (!(bstatus&2))
		{
			oct_rotvex(fmousx*f,&ifor,&irig);
			f = atan2(-irig.y,idow.y);
			if (f < PI/2) f += PI;
			if (f > PI/2) f -= PI;
			oct_rotvex(f*dtim*4,&irig,&idow);
		}
		else
		{
			oct_rotvex(fmousx*.008,&irig,&idow);
		}
	}

		//Speed
	if (keystatus[0x2a]|keystatus[0x36])
	{
		if (keystatus[0x1a]) { keystatus[0x1a] = 0; speed *= 0.5; } //{
		if (keystatus[0x1b]) { keystatus[0x1b] = 0; speed *= 2.0; } //}
	}
	f = dtim;
	if (keystatus[0x2a]) f *= 1.0/16.0;
	if (keystatus[0x36]) f *= 16.0/1.0;

	if (keystatus[0x1d]) //Holding L.CTRL moves mouse pointer
	{
		if (keystatus[0xcb]) { mpos.x -= irig.x*f*speed; mpos.y -= irig.y*f*speed; mpos.z -= irig.z*f*speed; } //Left
		if (keystatus[0xcd]) { mpos.x += irig.x*f*speed; mpos.y += irig.y*f*speed; mpos.z += irig.z*f*speed; } //Right
		if (keystatus[0xc8]) { mpos.x += ifor.x*f*speed; mpos.y += ifor.y*f*speed; mpos.z += ifor.z*f*speed; } //Up
		if (keystatus[0xd0]) { mpos.x -= ifor.x*f*speed; mpos.y -= ifor.y*f*speed; mpos.z -= ifor.z*f*speed; } //Down
		if (keystatus[0x9d]) { mpos.x -= idow.x*f*speed; mpos.y -= idow.y*f*speed; mpos.z -= idow.z*f*speed; } //Rt.Ctrl
		if (keystatus[0x52]) { mpos.x += idow.x*f*speed; mpos.y += idow.y*f*speed; mpos.z += idow.z*f*speed; } //KP0
	}
	else if (keystatus[0x38]|keystatus[0xb8]) //Holding L.Alt moves sprite
	{
		dpoint3d op, or, od, of, np, nr, nd, nf;
		op = spr[curspr].p; or = spr[curspr].r; od = spr[curspr].d; of = spr[curspr].f;

		if (keystatus[0xcb]) { spr[curspr].p.x -= irig.x*f*speed; spr[curspr].p.y -= irig.y*f*speed; spr[curspr].p.z -= irig.z*f*speed; } //Left
		if (keystatus[0xcd]) { spr[curspr].p.x += irig.x*f*speed; spr[curspr].p.y += irig.y*f*speed; spr[curspr].p.z += irig.z*f*speed; } //Right
		if (keystatus[0xc8]) { spr[curspr].p.x += ifor.x*f*speed; spr[curspr].p.y += ifor.y*f*speed; spr[curspr].p.z += ifor.z*f*speed; } //Up
		if (keystatus[0xd0]) { spr[curspr].p.x -= ifor.x*f*speed; spr[curspr].p.y -= ifor.y*f*speed; spr[curspr].p.z -= ifor.z*f*speed; } //Down
		if (keystatus[0x9d]) { spr[curspr].p.x -= idow.x*f*speed; spr[curspr].p.y -= idow.y*f*speed; spr[curspr].p.z -= idow.z*f*speed; } //Rt.Ctrl
		if (keystatus[0x52]) { spr[curspr].p.x += idow.x*f*speed; spr[curspr].p.y += idow.y*f*speed; spr[curspr].p.z += idow.z*f*speed; } //KP0
		if (keystatus[0x33]|keystatus[0x34]|keystatus[0xc9]|keystatus[0xd1])
		{
				//adjust pivot before rotation
			spr[curspr].p.x += (spr[curspr].r.x*spr[curspr].cen.x + spr[curspr].d.x*spr[curspr].cen.y + spr[curspr].f.x*spr[curspr].cen.z);
			spr[curspr].p.y += (spr[curspr].r.y*spr[curspr].cen.x + spr[curspr].d.y*spr[curspr].cen.y + spr[curspr].f.y*spr[curspr].cen.z);
			spr[curspr].p.z += (spr[curspr].r.z*spr[curspr].cen.x + spr[curspr].d.z*spr[curspr].cen.y + spr[curspr].f.z*spr[curspr].cen.z);

			if (keystatus[0x33]|keystatus[0x34]) //,.
			{
				double d;
				g = (keystatus[0x34]-keystatus[0x33])*f*2;
				d = spr[curspr].ori[0]; spr[curspr].ori[0] = d*cos(g) + spr[curspr].ori[6]*sin(g); spr[curspr].ori[6] = spr[curspr].ori[6]*cos(g) - d*sin(g);
				d = spr[curspr].ori[1]; spr[curspr].ori[1] = d*cos(g) + spr[curspr].ori[7]*sin(g); spr[curspr].ori[7] = spr[curspr].ori[7]*cos(g) - d*sin(g);
				d = spr[curspr].ori[2]; spr[curspr].ori[2] = d*cos(g) + spr[curspr].ori[8]*sin(g); spr[curspr].ori[8] = spr[curspr].ori[8]*cos(g) - d*sin(g);
				spr_update_rdf(&spr[curspr]);
			}
			if (keystatus[0xc9]|keystatus[0xd1]) //PGUP,PGDN
			{
				double d;
				g = (keystatus[0xd1]-keystatus[0xc9])*f*2;
				d = spr[curspr].ori[3]; spr[curspr].ori[3] = d*cos(g) + spr[curspr].ori[6]*sin(g); spr[curspr].ori[6] = spr[curspr].ori[6]*cos(g) - d*sin(g);
				d = spr[curspr].ori[4]; spr[curspr].ori[4] = d*cos(g) + spr[curspr].ori[7]*sin(g); spr[curspr].ori[7] = spr[curspr].ori[7]*cos(g) - d*sin(g);
				d = spr[curspr].ori[5]; spr[curspr].ori[5] = d*cos(g) + spr[curspr].ori[8]*sin(g); spr[curspr].ori[8] = spr[curspr].ori[8]*cos(g) - d*sin(g);
				spr_update_rdf(&spr[curspr]);
			}

				//adjust pivot after rotation
			spr[curspr].p.x -= (spr[curspr].r.x*spr[curspr].cen.x + spr[curspr].d.x*spr[curspr].cen.y + spr[curspr].f.x*spr[curspr].cen.z);
			spr[curspr].p.y -= (spr[curspr].r.y*spr[curspr].cen.x + spr[curspr].d.y*spr[curspr].cen.y + spr[curspr].f.y*spr[curspr].cen.z);
			spr[curspr].p.z -= (spr[curspr].r.z*spr[curspr].cen.x + spr[curspr].d.z*spr[curspr].cen.y + spr[curspr].f.z*spr[curspr].cen.z);
		}

		if (docollide)
		{
			if (oct_touch_oct(spr[curspr].oct,&spr[curspr].p,&spr[curspr].r,&spr[curspr].d,&spr[curspr].f,
									spr[     0].oct,&spr[     0].p,&spr[     0].r,&spr[     0].d,&spr[     0].f,ohit))
			{
#if 0
				hitp = spr[curspr].p; spr[curspr].p = op;
				hitr = spr[curspr].r; spr[curspr].r = or;
				hitd = spr[curspr].d; spr[curspr].d = od;
				hitf = spr[curspr].f; spr[curspr].f = of;
#else
				np = spr[curspr].p;
				nr = spr[curspr].r;
				nd = spr[curspr].d;
				nf = spr[curspr].f;
				collide_binsearch(spr[curspr].oct,&op,&or,&od,&of,&np,&nr,&nd,&nf,spr[0].oct,&spr[0].p,&spr[0].r,&spr[0].d,&spr[0].f,ohit);
				hitp = np; spr[curspr].p = op;
				hitr = nr; spr[curspr].r = or;
				hitd = nd; spr[curspr].d = od;
				hitf = nf; spr[curspr].f = of;
#endif
				fixhitit = 1;
			}
		}
	}
	else
	{
		dpoint3d ivec;
		ivec.x = 0.0; ivec.y = 0.0; ivec.z = 0.0;
		if (selmode == 0)
		{
			i = 0;
			if (keystatus[0xcb]) { keystatus[0xcb] = 0; seladdx -= 1.0; i = 1; }
			if (keystatus[0xcd]) { keystatus[0xcd] = 0; seladdx += 1.0; i = 1; }
			if (keystatus[0xc8]) { keystatus[0xc8] = 0; seladdy -= 1.0; i = 1; }
			if (keystatus[0xd0]) { keystatus[0xd0] = 0; seladdy += 1.0; i = 1; }
			if (i)
			{
				seladdx = min(max(floor(seladdx)+.5,0.5),15.5);
				seladdy = min(max(floor(seladdy)+.5,0.5),15.5);
			}
		}
		else if (selmode == 1)
		{
			f = dtim;
			if (keystatus[0x2a]) f *= .25;
			if (keystatus[0x36]) f *= 4.0;
			if (keystatus[0xcb]) { seladdx -= f; }
			if (keystatus[0xcd]) { seladdx += f; }
			if (keystatus[0xc8]) { seladdy -= f; }
			if (keystatus[0xd0]) { seladdy += f; }
			for(i=3-1;i>=0;i--)
			{
				fp.x = cos(((double)i)*(PI/3.0));
				fp.y = sin(((double)i)*(PI/3.0));
				f = seladdx*fp.x + seladdy*fp.y;
				f -= min(max(f,-sqrt(3.0)*.4999),sqrt(3.0)*.4999);
				if (f != 0.f) { seladdx -= fp.x*f; seladdy -= fp.y*f; }
			}
		}
		else
		{
			if (keystatus[0xcb]) { ivec.x -= irig.x; ivec.y -= irig.y; ivec.z -= irig.z; } //Left
			if (keystatus[0xcd]) { ivec.x += irig.x; ivec.y += irig.y; ivec.z += irig.z; } //Right
			if (keystatus[0xc8]) { ivec.x += ifor.x; ivec.y += ifor.y; ivec.z += ifor.z; } //Up
			if (keystatus[0xd0]) { ivec.x -= ifor.x; ivec.y -= ifor.y; ivec.z -= ifor.z; } //Down
			if (keystatus[0x9d]) { ivec.x -= idow.x; ivec.y -= idow.y; ivec.z -= idow.z; } //Rt.Ctrl
			if (keystatus[0x52]) { ivec.x += idow.x; ivec.y += idow.y; ivec.z += idow.z; } //KP0
		}

		if ((ivec.x != 0.0) || (ivec.y != 0.0) || (ivec.z != 0.0))
		{
			ivec.x *= f*speed; ivec.y *= f*speed; ivec.z *= f*speed;
			if (!docollide) { ipos.x += ivec.x; ipos.y += ivec.y; ipos.z += ivec.z; }
			else
			{
				oct_world2voxpos(&spr[curspr],&ipos,&fp);
				oct_world2voxdir(&spr[curspr],&ivec,&fp2);
				oct_slidemove(spr[curspr].oct,&fp,&fp2,fatness,&fp);
				oct_vox2worldpos(&spr[curspr],&fp,&ipos);
			}
		}

		if (keystatus[0x33]|keystatus[0x34]) //,.
		{
			if (keystatus[0x36]) //RShift
			{
				spr[curspr].mixval += (keystatus[0x34]-keystatus[0x33])*dtim;
				spr[curspr].mixval = min(max(spr[curspr].mixval,0.0),1.0);
			}
			else if (keystatus[0x2a]) //LShift
			{
				g = (keystatus[0x34]-keystatus[0x33])*64.0;
				i = (spr[curspr].imulcol&255) + (int)(floor(tim*g)-floor(otim*g));
				spr[curspr].imulcol = min(max(i,0),255)*0x10101 + 0xff000000;
			}
			else
			{
				g = (double)(keystatus[0x34]-keystatus[0x33])*f*-2.0;
				fp = ipos; ipos.x = fp.x*cos(g) + fp.z*sin(g); ipos.z = fp.z*cos(g) - fp.x*sin(g);
				fp = irig; irig.x = fp.x*cos(g) + fp.z*sin(g); irig.z = fp.z*cos(g) - fp.x*sin(g);
				fp = idow; idow.x = fp.x*cos(g) + fp.z*sin(g); idow.z = fp.z*cos(g) - fp.x*sin(g);
				fp = ifor; ifor.x = fp.x*cos(g) + fp.z*sin(g); ifor.z = fp.z*cos(g) - fp.x*sin(g);
			}
		}
		if (keystatus[0xc9]|keystatus[0xd1]) //PGUP,PGDN
		{
			g = (double)(keystatus[0xd1]-keystatus[0xc9])*f*2.0;

			fp2.x = ipos.x*irig.x + ipos.y*irig.y + ipos.z*irig.z;
			fp2.y = ipos.x*idow.x + ipos.y*idow.y + ipos.z*idow.z;
			fp2.z = ipos.x*ifor.x + ipos.y*ifor.y + ipos.z*ifor.z;

			fp = idow;
			idow.x = idow.x*cos(g) + ifor.x*sin(g); ifor.x = ifor.x*cos(g) - fp.x*sin(g);
			idow.y = idow.y*cos(g) + ifor.y*sin(g); ifor.y = ifor.y*cos(g) - fp.y*sin(g);
			idow.z = idow.z*cos(g) + ifor.z*sin(g); ifor.z = ifor.z*cos(g) - fp.z*sin(g);

			ipos.x = fp2.x*irig.x + fp2.y*idow.x + fp2.z*ifor.x;
			ipos.y = fp2.x*irig.y + fp2.y*idow.y + fp2.z*ifor.y;
			ipos.z = fp2.x*irig.z + fp2.y*idow.z + fp2.z*ifor.z;
		}
	}

		//Zoom
	if (keystatus[0x38]|keystatus[0xb8]) //Alt
	{
		if (keystatus[0x37]^keystatus[0xb5]) //KP/-KP*
		{
			if (keystatus[0x37]) f = pow(2.0,dtim); else f = pow(0.5,dtim);
			spr[curspr].p.x += (spr[curspr].r.x + spr[curspr].d.x + spr[curspr].f.x)*spr[curspr].oct->sid*.5;
			spr[curspr].p.y += (spr[curspr].r.y + spr[curspr].d.y + spr[curspr].f.y)*spr[curspr].oct->sid*.5;
			spr[curspr].p.z += (spr[curspr].r.z + spr[curspr].d.z + spr[curspr].f.z)*spr[curspr].oct->sid*.5;
			for(i=0;i<9;i++) spr[curspr].ori[i] *= f;
			spr_update_rdf(&spr[curspr]);
			spr[curspr].p.x -= (spr[curspr].r.x + spr[curspr].d.x + spr[curspr].f.x)*spr[curspr].oct->sid*.5;
			spr[curspr].p.y -= (spr[curspr].r.y + spr[curspr].d.y + spr[curspr].f.y)*spr[curspr].oct->sid*.5;
			spr[curspr].p.z -= (spr[curspr].r.z + spr[curspr].d.z + spr[curspr].f.z)*spr[curspr].oct->sid*.5;
		}
	}
	else
	{
		if (selmode == 0) //tex
		{
			if (keystatus[0x37]) selmul = min(selmul*pow(8.0,dtim),16.0);
			if (keystatus[0xb5]) selmul = max(selmul/pow(8.0,dtim), 0.5);
		}
		else if (selmode == 1) //col
		{
			if (keystatus[0x37]|keystatus[0x4e]) selcolintens = min(selcolintens*pow(2.0,dtim)+dtim*.25,4.0);     //KP*,KP+
			if (keystatus[0xb5]|keystatus[0x4a]) selcolintens = max(selcolintens/pow(2.0,dtim)-dtim*.25,1/512.0); //KP/,KP-
		}
		else
		{
			if (keystatus[0x37]) { g = ghz; ghz = min((1.0+f*(2.0/1.0))*ghz,ghx*64.0); if ((g < ghx) && (ghz >= ghx)) { ghz = ghx; keystatus[0x37] = 0; } ighz = (int)ghz; } //KP*
			if (keystatus[0xb5]) { g = ghz; ghz = max((1.0-f*(2.0/1.0))*ghz,ghx/64.0); if ((g > ghx) && (ghz <= ghx)) { ghz = ghx; keystatus[0xb5] = 0; } ighz = (int)ghz; } //KP/
		}
	}

	while (i = keyread())
	{
		switch((i>>8)&255)
		{
			case 0x01:
				if (selmode >= 0) { selmode = -1; break; }
				quitloop(); return;
			case 0x35: resetcamera((i&0x30000) != 0); break; // /
			case 0x4c: ghz = ghx; ighz = (int)ghz; break; //KP5
			case 0x2b: ocurspr = curspr; curspr++; if (curspr >= sprnum) curspr = 0; break; //backslash
			case 0x1a: if (!(i&0x30000)) currad = max(currad-1,1); break; //[
			case 0x1b: if (!(i&0x30000)) currad++; break; //]
			case 0x27: fatness *= 0.5; break; //;
			case 0x28: fatness *= 2.0; break; //'
			case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: case 0x40: case 0x41: case 0x42: //F1..F8
				if (!(i&0x30000)) oct_numcpu = min(((i>>8)&255)-0x3b+1,maxcpu);
				resetfps();
				break;
			case 0xd2: //Ins
				if (i&0x300000) //Alt+Ins:dup sprite
				{
					if ((sprnum < SPRMAX) && (octnum < OCTMAX))
					{
						spr[sprnum] = spr[curspr];
						oct_dup(spr[curspr].oct,&oct[octnum]);
						spr[sprnum].oct = &oct[octnum];
						sprnum++; octnum++;
					}
				}
				else
				{
					oct_world2voxpos(&spr[curspr],&ipos,&fp);
					oct_world2voxdir(&spr[curspr],&ifor,&fp2);
					f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
					isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&i,0);
					if (isurf != -1)
					{
						((int *)&lp.x)[i>>1] += (i&1)*2-1;

						if (oct_rebox(spr[curspr].oct,lp.x,lp.y,lp.z,lp.x+1,lp.y+1,lp.z+1,&x,&y,&z)) //grow octree if necessary
						{
							lp.x += x; lp.y += y; lp.z += z;
							spr[curspr].p.x -= (spr[curspr].r.x*x + spr[curspr].d.x*y + spr[curspr].f.x*z);
							spr[curspr].p.y -= (spr[curspr].r.y*x + spr[curspr].d.y*y + spr[curspr].f.y*z);
							spr[curspr].p.z -= (spr[curspr].r.z*x + spr[curspr].d.z*y + spr[curspr].f.z*z);
						}

						oct_setvox(spr[curspr].oct,lp.x,lp.y,lp.z,&((surf_t *)spr[curspr].oct->sur.buf)[isurf],1+2+hovercheck*4+normcheck*8);
					}
				}
				break;
			case 0xd3: //Del
				if (i&0x300000) //Alt+Del:free sprite
				{
					if (sprnum > 1) { sprite_del(curspr); if (curspr >= sprnum) curspr = 0; }
				}
				else
				{
					oct_world2voxpos(&spr[curspr],&ipos,&fp);
					oct_world2voxdir(&spr[curspr],&ifor,&fp2);
					f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
					isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&i,0);
					if (isurf != -1) oct_setvox(spr[curspr].oct,lp.x,lp.y,lp.z,&((surf_t *)spr[curspr].oct->sur.buf)[isurf],0+2+hovercheck*4+normcheck*8);
				}
				break;
			case 0xc7: case 0xcf: //Home/End
				if (i&0x300000) break;
				oct_world2voxpos(&spr[curspr],&ipos,&fp);
				oct_world2voxdir(&spr[curspr],&ifor,&fp2);
				f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
				isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&j,0);
				if (isurf != -1)
				{
					if ((i&0xc0000) && (((i>>8)&255)==0xc7) && (oct_rebox(spr[curspr].oct,lp.x-currad,lp.y-currad,lp.z-currad,lp.x+currad,lp.y+currad,lp.z+currad,&x,&y,&z))) //grow octree if necessary
					{
						lp.x += x; lp.y += y; lp.z += z;
						spr[curspr].p.x -= (spr[curspr].r.x*x + spr[curspr].d.x*y + spr[curspr].f.x*z);
						spr[curspr].p.y -= (spr[curspr].r.y*x + spr[curspr].d.y*y + spr[curspr].f.y*z);
						spr[curspr].p.z -= (spr[curspr].r.z*x + spr[curspr].d.z*y + spr[curspr].f.z*z);
					}

					//printf("%d,%d,%d,%d,%d,%d\n",lp.x,lp.y,lp.z,currad,((i>>8)&255)==0xc7,!(i&0x30000)); //Generate list for 'F'

					if (!(i&0x30000))
						  { brush_sph_t sph; brush_sph_init(&sph,lp.x,lp.y,lp.z,currad,((i>>8)&255)==0xc7); oct_mod(spr[curspr].oct,(brush_t *)&sph,(((i>>8)&255)==0xc7)+2+hovercheck*4+normcheck*8); }
					else { brush_box_t box; brush_box_init(&box,lp.x-currad,lp.y-currad,lp.z-currad,lp.x+currad,lp.y+currad,lp.z+currad,((i>>8)&255)==0xc7); oct_mod(spr[curspr].oct,(brush_t *)&box,(((i>>8)&255)==0xc7)+2+hovercheck*4+normcheck*8); }
				}
				break;
			case 0x13: //R:add, Shift+R:rem, Ctrl+R:add rand, Ctrl+R,rem rand
				readklock(&d0);
				if (!(i&0xc0000))
				{
					brush_sph_t sph;

					j = (!(i&0x30000))+2;
					for(i=64;i>0;i--)
					{
						brush_sph_init(&sph,rand()&(spr[curspr].oct->sid-1),
												  rand()&(spr[curspr].oct->sid-1),
												  rand()&(spr[curspr].oct->sid-1),
												  (rand()&((spr[curspr].oct->sid>>4)-1))+(spr[curspr].oct->sid>>4),j);
						oct_mod(spr[curspr].oct,(brush_t *)&sph,j);
					}
				}
				else
				{
					j = (!(i&0x30000))+2;
					surf.b = (rand()&63)+96; surf.norm[0] = 0;
					surf.g = (rand()&63)+96; surf.norm[1] = 0;
					surf.r = (rand()&63)+96; surf.norm[2] = 0;
					surf.a = (rand()&255);   surf.tex = (rand()&63);

					k = spr[curspr].oct->sid-1;
					for(i=16384;i>0;i--) oct_setvox(spr[curspr].oct,rand()&k,rand()&k,rand()&k,&surf,j);
				}
				readklock(&d1); testim = d1-d0;
				break;
			case 0x21: //F
				if (!(i&0x30000)) //F
				{
					static const signed char swizlut[6*8][3] =
					{
						-1,-2,-3, +1,-2,-3, -1,+2,-3, +1,+2,-3, -1,-2,+3, +1,-2,+3, -1,+2,+3, +1,+2,+3,
						-1,-3,-2, +1,-3,-2, -1,+3,-2, +1,+3,-2, -1,-3,+2, +1,-3,+2, -1,+3,+2, +1,+3,+2,
						-2,-1,-3, +2,-1,-3, -2,+1,-3, +2,+1,-3, -2,-1,+3, +2,-1,+3, -2,+1,+3, +2,+1,+3,
						-2,-3,-1, +2,-3,-1, -2,+3,-1, +2,+3,-1, -2,-3,+1, +2,-3,+1, -2,+3,+1, +2,+3,+1,
						-3,-1,-2, +3,-1,-2, -3,+1,-2, +3,+1,-2, -3,-1,+2, +3,-1,+2, -3,+1,+2, +3,+1,+2,
						-3,-2,-1, +3,-2,-1, -3,+2,-1, +3,+2,-1, -3,-2,+1, +3,-2,+1, -3,+2,+1, +3,+2,+1,
					};
					i = rand()%48; oct_swizzle(spr[curspr].oct,swizlut[i][0],swizlut[i][1],swizlut[i][2]);
					break;
				}

					//Test oct_hover_check()/physics bugs.. (press Shift+F, then Ctrl+G)
				{
				static int bozoval[][6] =
				{
					177,207,196,16,1,1,
					177,191,195,16,1,1,
					177,175,193,16,1,1,
					177,182,177,8,1,1,
					177,181,169,8,1,1,
					177,181,161,8,1,1,
					177,181,153,8,1,1,
					177,181,145,8,1,1,
					177,181,137,8,1,1,
					177,180,129,8,1,1,
					177,180,121,8,1,1,
					169,181,123,8,1,1,
					161,180,122,8,1,1,
					153,179,122,8,1,1,
					145,178,122,8,1,1,
					137,177,122,8,1,1,
					129,176,122,8,1,1,
					121,175,122,8,1,1,
					113,174,122,8,1,1,
					113,166,120,8,1,1,
					112,158,120,8,1,1,
					110,150,120,8,1,1,
					109,142,120,8,1,1,
					108,134,120,8,1,1,
					106,126,118,8,1,1,
					105,118,118,8,1,1,
					103,110,118,8,1,1,
					102,111,125,8,1,1,
					101,111,132,8,1,1,
					101,110,139,8,1,1,
					100,110,146,8,1,1,
					100,109,153,4,1,1,
					 99,109,156,4,1,1,
					 99,109,159,4,1,1,
					 99,109,162,4,1,1,
					 99,109,165,8,1,1,
					 98,108,172,8,1,1,
					 97,108,179,8,1,1,
					 97,107,186,8,1,1,
					 96,107,193,16,1,1,
					 95,106,208,16,1,1,
					111,106,191,16,1,1,
					126,100,191,16,1,1,
					141,113,190,12,1,1,
					152,120,192,8,1,1,
					154,124,191,8,1,1,
					103,105,155,8,0,1,
				};

				hovercheck = 1; i = spr[curspr].oct->sid; oct_hover_check(spr[curspr].oct,0,0,0,i,i,i,0); spr[curspr].oct->recvoctfunc = recvoctfunc;
#if (GPUSEBO != 0)
				if (oct_usegpu)
				{
					if (!spr[curspr].oct->gsurf) spr[curspr].oct->gsurf = (surf_t *)bo_begin(spr[curspr].oct->bufid,0);
					memcpy(spr[curspr].oct->gsurf,spr[curspr].oct->sur.buf,spr[curspr].oct->sur.mal*spr[curspr].oct->sur.siz);
				}
#endif

				for(i=0;i<sizeof(bozoval)/(6*4);i++)
				{
					if (!bozoval[i][5])
					{
						brush_sph_t sph;
						brush_sph_init(&sph,bozoval[i][0],bozoval[i][1],bozoval[i][2],bozoval[i][3],bozoval[i][4]);
						oct_mod(spr[curspr].oct,(brush_t *)&sph,bozoval[i][4]+2+hovercheck*4+normcheck*8);
					}
					else
					{
						brush_box_t box;
						brush_box_init(&box,bozoval[i][0]-bozoval[i][3],bozoval[i][1]-bozoval[i][3],bozoval[i][2]-bozoval[i][3],
												  bozoval[i][0]+bozoval[i][3],bozoval[i][1]+bozoval[i][3],bozoval[i][2]+bozoval[i][3],bozoval[i][4]);
						oct_mod(spr[curspr].oct,(brush_t *)&box,bozoval[i][4]+2+hovercheck*4+normcheck*8);
					}
				}
				}
				break;
			case 0x2e: //C
				if (i&0x0c0000) { docollide ^= 1; } //Ctrl+C:toggle collision
				else if (i&0x030000) { oct_checkreducesizes(spr[curspr].oct); } //Shift+C
				else if (i&0x300000) //Alt+C:drawcone
				{
					brush_cone_t cone;

					j = (!(i&0x30000))+2;
					readklock(&d0);
					brush_cone_init(&cone,rand()&(spr[curspr].oct->sid-1),rand()&(spr[curspr].oct->sid-1),rand()&(spr[curspr].oct->sid-1),
												(rand()&((spr[curspr].oct->sid>>3)-1))+(spr[curspr].oct->sid>>5),
												 rand()&(spr[curspr].oct->sid-1),rand()&(spr[curspr].oct->sid-1),rand()&(spr[curspr].oct->sid-1),
												(rand()&((spr[curspr].oct->sid>>3)-1))+(spr[curspr].oct->sid>>5));
					oct_mod(spr[curspr].oct,(brush_t *)&cone,j);
					readklock(&d1); testim = d1-d0;
				}
				break;
			case 0x31: //N
				if (i&0xc0000) //Ctrl+N
				{
					normcheck ^= 1;
					if (normcheck)
					{
						readklock(&d0);
						i = spr[curspr].oct->sid; oct_refreshnorms(spr[curspr].oct,2,0,0,0,i,i,i);
						readklock(&d1); testim = d1-d0;
					}
				}
				else if (i&0x30000) //Shift+N
				{
					if (gusemix)
					{
						gusemix = 0;
						i = spr[curspr].imulcol;
						spr[curspr].imulcol = (i&0xff000000)
							+ (min(((i>> 0)&255)*2,255)<< 0)
							+ (min(((i>> 8)&255)*2,255)<< 8)
							+ (min(((i>>16)&255)*2,255)<<16);

						readklock(&d0);
						oct_normalizecols(spr[curspr].oct);
						readklock(&d1); testim = d1-d0;
					}
				}
				else shownorm = !shownorm;
				break;
			case 0x1c: //L.Enter
selectexit:;if (selmode == 0)
				{
					x = (int)floor(seladdx);
					y = (int)floor(seladdy); if ((x|y)&-16) { selmode = -1; break; }
					i = oct_getsurf(spr[curspr].oct,selvox.x,selvox.y,selvox.z);
					if (i >= 0)
					{
						memcpy(&surf,&((surf_t *)spr[curspr].oct->sur.buf)[i],spr[curspr].oct->sur.siz);
						surf.tex = y*16 + x; oct_writesurf(spr[curspr].oct,i,&surf);
					}
					selmode = -1;
				}
				else if (selmode == 1)
				{
					i = oct_getsurf(spr[curspr].oct,selvox.x,selvox.y,selvox.z);
					if (i >= 0)
					{
						float r, g, b;
						memcpy(&surf,&((surf_t *)spr[curspr].oct->sur.buf)[i],spr[curspr].oct->sur.siz);
						if (colpick_xy2rgb(seladdx,seladdy,selcolintens*256.0,&r,&g,&b))
						{
							surf.b = min(max((int)b,0),255);
							surf.g = min(max((int)g,0),255);
							surf.r = min(max((int)r,0),255);
							oct_writesurf(spr[curspr].oct,i,&surf);
						}
					}
					selmode = -1;
				}
				break;
			case 0x29: //`
				gdrawclose++; if (gdrawclose >= 3) gdrawclose = 0;
				break;
			case 0x9c: //KPEnter
					  if (oct_sideshade[1] == 0x030303) { for(i=6-1;i>=0;i--) oct_sideshade[i] = i*0x0e0e0e; }
				else if (oct_sideshade[1] == 0x0e0e0e) { for(i=6-1;i>=0;i--) oct_sideshade[i] = i*0x000000; }
				else                                   { for(i=6-1;i>=0;i--) oct_sideshade[i] = i*0x030303; }
				break;
			case 0x26: //L
				if (i&0xc0000) //Ctrl+L:load
				{
					dpoint3d sp, sr, sd, sf;
					char *v;

					keystatus[0x1d] = 0; keystatus[0x9d] = 0;
					if (v = (char *)loadfileselect("LOAD MODEL...","All files\0*.kvo;*.kvs;*.kvn;*.vxl;*.kv6;*.kvx;*.vox;*.png\0*.KVO\0*.kvo\0*.KVS\0*.kvs\0*.KVN\0;*.kvn\0;*.VXL\0*.vxl\0*.KV6\0*.kv6\0*.KVX\0*.kvx\0*.VOX\0*.vox\0*.PNG\0*.png\0\0","KVO"))
					{
						f = (double)spr[curspr].oct->sid;

						readklock(&d0);
						oct_free(spr[curspr].oct); spr[curspr].oct->tilid = gtilid;
						oct_load(spr[curspr].oct,v,&sp,&sr,&sd,&sf);
						readklock(&d1); testim = d1-d0;

						f /= (double)spr[curspr].oct->sid;
						spr[curspr].br.x *= f; spr[curspr].br.y *= f; spr[curspr].br.z *= f;
						spr[curspr].bd.x *= f; spr[curspr].bd.y *= f; spr[curspr].bd.z *= f;
						spr[curspr].bf.x *= f; spr[curspr].bf.y *= f; spr[curspr].bf.z *= f;

						fatness = 0.5;
						if (spr[curspr].oct->flags&1)
						{
							for(j=0;j<9;j++) spr[curspr].ori[j] = (double)(!(j&3));
							spr_update_rdf(&spr[curspr]);

							oct_vox2worldpos(&spr[curspr],&sp,&ipos);
							irig.x = sr.x; irig.y = sr.y; irig.z = sr.z;
							idow.x = sd.x; idow.y = sd.y; idow.z = sd.z;
							ifor.x = sf.x; ifor.y = sf.y; ifor.z = sf.z;

							speed = 1.0/16.0;
						}
						sipos = ipos; sirig = irig; sidow = idow; sifor = ifor;

						spr_getmoi(&spr[curspr]);
						spr_update_rdf(&spr[curspr]);

						resetfps();
					}
				}
				break;
			case 0x1f: //S
				if (i&0xc0000) //Ctrl+S:save
				{
					char *v;
					keystatus[0x1d] = 0; keystatus[0x9d] = 0;
					if (v = (char *)savefileselect("SAVE MODEL...","All files\0*.*\0*.KVO\0*.kvo\0*.KVS\0*.kvs\0*.KVN\0*.kvn\0*.KV6\0*.kv6\0\0",""))
					{
						s = 0; oct_world2voxpos(&spr[s],&sipos,&fp);
						oct_save(spr[curspr].oct,v,&fp,&sirig,&sidow,&sifor);
						MessageBeep(0);
					}
					break;
				}
				else if (i&0x300000) //Alt+S (swap solid&air)
				{
					oct_swap_sol_air(spr[curspr].oct);
					break;
				}
				if (!oct_usegpu)
				{
#if (MARCHCUBE == 0)
						  if (shaderfunc == cpu_shader_texmap   ) { shaderfunc = cpu_shader_znotex; }
					else if (shaderfunc == cpu_shader_znotex   ) { shaderfunc = cpu_shader_solcol; }
					else if (shaderfunc == cpu_shader_solcol   ) { shaderfunc = cpu_shader_texmap; if (tiles[0].ltilesid < 0) shaderfunc = cpu_shader_solcol; }
#else
						  if (shaderfunc == cpu_shader_texmap_mc) { shaderfunc = cpu_shader_solcol_mc; }
					else                                         { shaderfunc = cpu_shader_texmap_mc; if (tiles[0].ltilesid < 0) shaderfunc = cpu_shader_solcol_mc; }
#endif
				}
				break;
			case 0x17: //I
				if (oct_usegpu)
				{
					swapinterval = !swapinterval;
					if (glfp[wglSwapIntervalEXT]) ((PFNWGLSWAPINTERVALEXTPROC)glfp[wglSwapIntervalEXT])(swapinterval);
				}
				break;
			case 0x22: //G
				if (i&0xc0000) //Ctrl+G:gravity
				{
					gravity ^= 1;
				}
				else
				{
					showborders ^= 1; resetfps();
				}
				break;
			case 0x23: //H
				readklock(&d0);
				if (!(i&0xc0000))
				{
					oct_world2voxpos(&spr[curspr],&ipos,&fp);
					oct_world2voxdir(&spr[curspr],&ifor,&fp2);
					f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
					if (oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&j,0) != -1)
					{
						if (!(i&0x300000)) oct_hover_check(spr[curspr].oct,lp.x,lp.y,lp.z,lp.x+1,lp.y+1,lp.z+1,0); //H:zot
										  else oct_hover_check(spr[curspr].oct,lp.x,lp.y,lp.z,lp.x+1,lp.y+1,lp.z+1,recvoctfunc); //Alt+H:gen sprite
					}
				}
				else
				{
					hovercheck ^= 1;
					if (hovercheck) { i = spr[curspr].oct->sid; oct_hover_check(spr[curspr].oct,0,0,0,i,i,i,0); spr[curspr].oct->recvoctfunc = recvoctfunc; } //Ctrl+H
								  else { spr[curspr].oct->recvoctfunc = 0; }
				}

#if (GPUSEBO != 0)
				if (oct_usegpu)
				{
					if (!spr[curspr].oct->gsurf) spr[curspr].oct->gsurf = (surf_t *)bo_begin(spr[curspr].oct->bufid,0);
					memcpy(spr[curspr].oct->gsurf,spr[curspr].oct->sur.buf,spr[curspr].oct->sur.mal*spr[curspr].oct->sur.siz);
				}
#endif
				readklock(&d1); testim = d1-d0;
				break;
			case 0x16: //U
#if (GPUSEBO != 0)
				if (!oct_usegpu) break;
				if (!spr[curspr].oct->gsurf) spr[curspr].oct->gsurf = (surf_t *)bo_begin(spr[curspr].oct->bufid,0);
				memcpy(spr[curspr].oct->gsurf,spr[curspr].oct->sur.buf,spr[curspr].oct->sur.mal*spr[curspr].oct->sur.siz);
#endif
				break;
			case 0x30: //B
				if (i&0x300000) //Alt+B
				{
					brush_sph_t sph;
					oct_free(spr[curspr].oct);
					oct_new(spr[curspr].oct,gdeflsid,gtilid,0,0,0);
					oct_world2voxpos(&spr[curspr],&mpos,&fp);
					brush_sph_init(&sph,fp.x,fp.y,fp.z,currad,1); oct_mod(spr[curspr].oct,(brush_t *)&sph,1+2);
				}
				else
				{
					j = spr[curspr].oct->sid;
						  if ( (i&0x30000)) oct_rebox(spr[curspr].oct,j,j,j,0,0,0,&x,&y,&z);                                        //Shift+B: shrink-wrap
					else if (!(i&0xc0000)) oct_rebox(spr[curspr].oct,0,0,0,j<<1,j<<1,j<<1,&x,&y,&z);                               //      B: grow
					else                   oct_rebox(spr[curspr].oct,-(j>>1),-(j>>1),-(j>>1),(j*3)>>1,(j*3)>>1,(j*3)>>1,&x,&y,&z); // Ctrl+B: grow, centered
					spr[curspr].p.x -= (spr[curspr].r.x*x + spr[curspr].d.x*y + spr[curspr].f.x*z);
					spr[curspr].p.y -= (spr[curspr].r.y*x + spr[curspr].d.y*y + spr[curspr].f.y*z);
					spr[curspr].p.z -= (spr[curspr].r.z*x + spr[curspr].d.z*y + spr[curspr].f.z*z);
				}
				break;
			case 0x32: //M
				if (i&0xc0000) //Ctrl+M: test brush_bmp_* (rubble)
				{
					dpoint3d dp;
					point3d pp, pr, pd, pf;
					brush_bmp_t bbmp;

					if (!rubble_inited) { rubble_inited = 1; rubble_genboxsums(); }
					if (!simp_inited) { simp_inited = 1; simp_genboxsum(); }

					oct_world2voxpos(&spr[curspr],&mpos,&dp);
					pp.x = dp.x; pp.y = dp.y; pp.z = dp.z;
					matrand(&pr,&pd,&pf); f = currad*16.0*sqrt(spr[curspr].r.x*spr[curspr].r.x + spr[curspr].r.y*spr[curspr].r.y + spr[curspr].r.z*spr[curspr].r.z);
					pr.x *= f; pr.y *= f; pr.z *= f;
					pd.x *= f; pd.y *= f; pd.z *= f;
					pf.x *= f; pf.y *= f; pf.z *= f;
					pp.x -= (pr.x + pd.x + pf.x)*16.0;
					pp.y -= (pr.y + pd.y + pf.y)*16.0;
					pp.z -= (pr.z + pd.z + pf.z)*16.0;
					i = (rand()%5);
					if (i < 4) brush_bmp_init(&bbmp,&rubble_boxsum[rand()&3][0][0][0],32,32,32,&pp,&pr,&pd,&pf);
							else brush_bmp_init(&bbmp,&simp_boxsum[0][0][0],40,40,40,&pp,&pr,&pd,&pf);
					oct_mod(spr[curspr].oct,(brush_t *)&bbmp,1+2);
					break;
				}
				if ((curspr != ocurspr) && ((unsigned)ocurspr < (unsigned)sprnum) && ((unsigned)curspr < (unsigned)sprnum))
				{
					brush_oct_t boct;
					brush_oct_init(&boct,spr[ocurspr].oct,&spr[ocurspr].p,&spr[ocurspr].r,&spr[ocurspr].d,&spr[ocurspr].f,
												spr[ curspr].oct,&spr[ curspr].p,&spr[ curspr].r,&spr[ curspr].d,&spr[ curspr].f);
					oct_mod(spr[ocurspr].oct,(brush_t *)&boct,(!(i&0x30000))+2);
				}
				break;
			case 0x2f: //V
				if (selmode >= 0) { selmode = -1; break; }
				oct_world2voxpos(&spr[curspr],&ipos,&fp);
				oct_world2voxdir(&spr[curspr],&ifor,&fp2);
				f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
				isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&selvox,&i,0);
				if (isurf != -1)
				{
					psurf = &((surf_t *)spr[curspr].oct->sur.buf)[isurf];
					i = (unsigned char)psurf->tex;
					seladdx = (i&15)+.5;
					seladdy = (i>>4)+.5;
					selmode = 0;
				} else selmode = -1;
				break;
			case 0x19: //P
				if (selmode >= 0) { selmode = -1; break; }
				oct_world2voxpos(&spr[curspr],&ipos,&fp);
				oct_world2voxdir(&spr[curspr],&ifor,&fp2);
				f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
				isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&selvox,&i,0);
				if (isurf != -1)
				{
					float fx, fy, fintens;
					psurf = &((surf_t *)spr[curspr].oct->sur.buf)[isurf];
					i = ((*(int *)&psurf->b)&0xffffff);
					colpick_rgb2xy((i>>16)&255,(i>>8)&255,i&255,&seladdx,&seladdy,&fintens);
					selcolintens = fintens/256.0;
					selmode = 1;
				}
				break;
			case 0x1e: //A
				{
				brush_box_t box;
				brush_sph_t sph;
				brush_t *bru;

				oct_world2voxpos(&spr[curspr],&ipos,&fp);
				oct_world2voxdir(&spr[curspr],&ifor,&fp2);
				f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
				if (oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&j,0) != -1) break;

				spr[curspr].oct->recvoctfunc = recvoctfunc;
				if (!(keystatus[0x2a]|keystatus[0x36]))
					  { brush_sph_init(&sph,lp.x,lp.y,lp.z,currad,0); bru = (brush_t *)&sph; }
				else { brush_box_init(&box,lp.x-currad,lp.y-currad,lp.z-currad,lp.x+currad,lp.y+currad,lp.z+currad,0); bru = (brush_t *)&box; }

#if 1
					//cut piece out
				oct_modnew(spr[curspr].oct,bru,0);
				oct_mod(spr[curspr].oct,bru,2+hovercheck*4+normcheck*8);
#else
					//bool test:
				oct_modnew(spr[curspr].oct,bru,0);
				gravity = 0; spr[sprnum-1].p.x += 1.0;
#endif
				}
				break;
		}
	}

	if ((keystatus[0x0f]|keystatus[0x39]|keystatus[0x4a]|keystatus[0x4e]) && (selmode < 0)) //Tab,Space,KP-,KP+
	{
		oct_world2voxpos(&spr[curspr],&ipos,&fp);
		oct_world2voxdir(&spr[curspr],&ifor,&fp2);
		f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
		isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&i,0);
		if (isurf != -1)
		{
			psurf = &((surf_t *)spr[curspr].oct->sur.buf)[isurf];
				  if (keystatus[0x0f]) { cursurf = *psurf; }
			else if ((keystatus[0x39]) || (keystatus[0x1c] && (selmode < 0)))
			{
				if (!(keystatus[0x2a]|keystatus[0x36]))
					{ oct_writesurf(spr[curspr].oct,isurf,&cursurf); }
				else
				{
					brush_sph_t sph;
					brush_sph_sol_init(&sph,lp.x,lp.y,lp.z,currad,&cursurf);
					oct_paint(spr[curspr].oct,(brush_t *)&sph);
				}
			}
			else
			{
				i = keystatus[0x4e]-keystatus[0x4a];
				if (keystatus[0x1d]|keystatus[0x9d])
				{
					keystatus[0x4e] = 0; keystatus[0x4a] = 0;
					surf = *psurf; surf.tex = (char)min(max(((int)surf.tex)+i,0),255);
					oct_writesurf(spr[curspr].oct,isurf,&surf);
				}
				else
				{
					surf = *psurf;
					surf.b = (char)min(max(((int)surf.b)+i,0),255);
					surf.g = (char)min(max(((int)surf.g)+i,0),255);
					surf.r = (char)min(max(((int)surf.r)+i,0),255);
					oct_writesurf(spr[curspr].oct,isurf,&surf);
				}
			}
		}
	}

	if ((keystatus[0xc7]|keystatus[0xcf]) && (keystatus[0x38]|keystatus[0xb8])) //Alt+Home/End (voxel spray/suck)
	{
		oct_world2voxpos(&spr[curspr],&ipos,&fp);
		oct_world2voxdir(&spr[curspr],&ifor,&fp2);
		f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
		isurf = oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&j,0);
		if ((isurf != -1) && (voxfindspray(spr[curspr].oct,lp.x,lp.y,lp.z,4,&x,&y,&z,keystatus[0xc7]) >= 0))
			oct_setvox(spr[curspr].oct,x,y,z,&((surf_t *)spr[curspr].oct->sur.buf)[isurf],keystatus[0xc7]+2+hovercheck*4+normcheck*8);
	}


	if (keystatus[0x18]) spr[curspr].r.y += dtim*.01*(keystatus[0x2a]*2-1); //O

	if (keystatus[0x1d]|keystatus[0x9d])
	{
		if ((but&1) || (bstatus&1))  //LefBut/Ins
		{
			brush_sph_t sph;

			oct_world2voxpos(&spr[curspr],&mpos,&fp);
			brush_sph_init(&sph,fp.x,fp.y,fp.z,currad,1);
			oct_mod(spr[curspr].oct,(brush_t *)&sph,1+2+hovercheck*4+normcheck*8);
		}
		if ((but&2)) // || (bstatus&2)) //RigBut/Del
		{
			brush_sph_t sph;

			oct_world2voxpos(&spr[curspr],&mpos,&fp);
			brush_sph_init(&sph,fp.x,fp.y,fp.z,currad,0);
			oct_mod(spr[curspr].oct,(brush_t *)&sph,0+2+hovercheck*4+normcheck*8);
		}
	}
	else
	{
		if (bstatus&~obstatus&1) //select sprite w/LMB
		{
			if (selmode >= 0) goto selectexit;

			g = 1e32; i = -1;
			for(s=0;s<sprnum;s++)
			{
				oct_world2voxpos(&spr[s],&ipos,&fp);
				oct_world2voxdir(&spr[s],&ifor,&fp2);
				f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
				if (oct_hitscan(spr[s].oct,&fp,&fp2,&lp,&j,&f) != -1) { if (f < g) { g = f; i = s; } }
			}
			if (i >= 0) { if (i != curspr) ocurspr = curspr; curspr = i; }
		}
		if ((bstatus&~obstatus&2) && (selmode >= 0)) selmode = -1;
	}

	k = oct_fogcol;
	if (selmode >= 0) oct_fogcol = ((oct_fogcol&0xfefefe)>>1)+(oct_fogcol&0xff000000);
	i = oct_startdraw(&ipos,&irig,&idow,&ifor,ghx,ghy,ghz);
	oct_fogcol = k;
	if (i < 0) goto skipdd;
	drawsprites(&ipos,&irig,&idow,&ifor,selmode);

	if (showborders)
	{
		f = .005*sqrt(2.0);
		oct_drawcone(f,0.0,0.0,0.005, 0.5,0.0,0.0,0.005, 0xffc06060,48.0,0);
		oct_drawcone(0.0,f,0.0,0.005, 0.0,0.5,0.0,0.005, 0xff60c060,48.0,0);
		oct_drawcone(0.0,0.0,f,0.005, 0.0,0.0,0.5,0.005, 0xff6060c0,48.0,0);

		for(s=sprnum-1;s>=0;s--)
		{
			for(i=0,x=0;x<=spr[s].oct->sid;x+=spr[s].oct->sid)
				for(y=0;y<=spr[s].oct->sid;y+=spr[s].oct->sid)
					for(z=0;z<=spr[s].oct->sid;z+=spr[s].oct->sid,i++)
						{ fp.x = (double)x; fp.y = (double)y; fp.z = (double)z; oct_vox2worldpos(&spr[s],&fp,&vert[i]); }
			static const char wireframecubeverts[12][2] = {0,1,2,3,4,5,6,7, 0,2,1,3,4,6,5,7, 0,4,1,5,2,6,3,7};

			for(k=12-1;k>=0;k--)
			{
				i = wireframecubeverts[k][0];
				j = wireframecubeverts[k][1];
				oct_drawcone(vert[i].x,vert[i].y,vert[i].z,0.005,
								 vert[j].x,vert[j].y,vert[j].z,0.005, 0xff606060+(curspr==s)*0x404040,48.0,0);
			}
#if 0
			int x0, y0, z0, x1, y1, z1;
			x0 = 0; y0 = 0; z0 = 0; x1 = spr[s].oct->sid; y1 = spr[s].oct->sid; z1 = spr[s].oct->sid;
			oct_getsolbbox(spr[s].oct,&x0,&y0,&z0,&x1,&y1,&z1);
			for(i=0,x=x0;x<=x1;x+=x1-x0)
				for(y=y0;y<=y1;y+=y1-y0)
					for(z=z0;z<=z1;z+=z1-z0,i++)
						{ fp.x = (double)x; fp.y = (double)y; fp.z = (double)z; oct_vox2worldpos(&spr[s],&fp,&vert[i]); }
			for(k=12-1;k>=0;k--)
			{
				i = wireframecubeverts[k][0];
				j = wireframecubeverts[k][1];
				oct_drawcone(vert[i].x,vert[i].y,vert[i].z,0.001,
								 vert[j].x,vert[j].y,vert[j].z,0.001, 0xff606060+(curspr==s)*0x404040,48.0,0);
			}
#endif
		}

//--------------------------------------------------------------------------------------------------
		{  //Draw 3D mouse cursor
		brush_sph_t sph;
		oct_world2voxpos(&spr[curspr],&mpos,&fp);
		brush_sph_init(&sph,fp.x,fp.y,fp.z,currad,0);
		if (oct_touch_brush(spr[curspr].oct,(brush_t *)&sph)) i = 0x808090a0; else i = 0x80807060;
		oct_drawsph(mpos.x,mpos.y,mpos.z,currad*sqrt(spr[curspr].r.x*spr[curspr].r.x + spr[curspr].r.y*spr[curspr].r.y + spr[curspr].r.z*spr[curspr].r.z),i,48.0);
		}

#if (0) //MARCHCUBE != 0)
		static ipoint3d boxcorn = {130,122,0};
		static int boxsid = 4;
		if (!keystatus[0x1d])
		{
			if (keystatus[0x4f]) { keystatus[0x4f] = 0; boxcorn.x--; }
			if (keystatus[0x51]) { keystatus[0x51] = 0; boxcorn.x++; }
			if (keystatus[0x4b]) { keystatus[0x4b] = 0; boxcorn.y--; }
			if (keystatus[0x4d]) { keystatus[0x4d] = 0; boxcorn.y++; }
			if (keystatus[0x50]) { keystatus[0x50] = 0; boxcorn.z--; }
			if (keystatus[0x4c]) { keystatus[0x4c] = 0; boxcorn.z++; }

		}
		else
		{
			if (keystatus[0x4f]) { keystatus[0x4f] = 0; boxsid--; }
			if (keystatus[0x51]) { keystatus[0x51] = 0; boxsid++; }
		}

		for(i=8-1;i>=0;i--)
		{
			fp.x = (double)(((i   )&1)*boxsid + boxcorn.x);
			fp.y = (double)(((i>>1)&1)*boxsid + boxcorn.y);
			fp.z = (double)(((i>>2)&1)*boxsid + boxcorn.z);
			oct_vox2worldpos(&spr[curspr],&fp,&vert[i]);
		}
		static const char wireframecubeverts[12][2] = {0,1,2,3,4,5,6,7, 0,2,1,3,4,6,5,7, 0,4,1,5,2,6,3,7};
		i = keystatus[0x1d]*7; oct_drawsph(vert[i].x,vert[i].y,vert[i].z,0.002, 0xff606060,48.0);
		m = oct_isboxanyair(spr[0].oct,0,0,0,spr[0].oct->sid>>1,&((octv_t *)spr[0].oct->nod.buf)[spr[0].oct->head],boxcorn.x,boxcorn.y,boxcorn.z,boxsid);
		for(k=12-1;k>=0;k--)
		{
			i = wireframecubeverts[k][0];
			j = wireframecubeverts[k][1];
			oct_drawcone(vert[i].x,vert[i].y,vert[i].z,0.001,
							 vert[j].x,vert[j].y,vert[j].z,0.001, 0xff606060 + m*0x9f0000,48.0,0);
		}
#endif
	}

#if 0
	for(i=(spr[curspr].oct->nod.mal>>4)-1;i>=0;i--)
	{
		j = (i<<4); if (!(spr[curspr].oct->nod.bit[j>>5]&(1<<j))) k = 0x808080; else k = 0xffff80;
		oct_drawpix((i&255)+256,i>>8,k);
	}
	for(i=(spr[curspr].oct->sur.mal>>4)-1;i>=0;i--)
	{
		j = (i<<4); if (!(spr[curspr].oct->sur.bit[j>>5]&(1<<j))) k = 0x808080; else k = 0xffff80;
		oct_drawpix((i&255)+512,i>>8,k);
	}
#endif

#if 0
		//show centroids
	for(i=1;i<sprnum;i++)
	{
		fp.x = spr[i].cen.x; fp.y = spr[i].cen.y; fp.z = spr[i].cen.z;
		oct_vox2worldpos(&spr[i],&fp,&fp);
		oct_drawsph(fp.x,fp.y,fp.z,currad/(double)spr[i].oct->sid,0xffc0c060,48.0);
		oct_drawtext6x8(100,100+i*10,0xffffffff,0,"%g %g %g",spr[i].cen.x,spr[i].cen.y,spr[i].cen.z);
	}
#endif

	if (!gravity)
	{
		if ((fixhitit) && (curspr))
		{
			pgram3d_t bi;
			dpoint3d dp0, dr0, dd0, df0, dp1, dr1, dd1, df1, dpos, hit, norm;

			f = (1<<ohit[1].ls)*.5;
			dr0.x = spr[0].r.x*f; dd0.x = spr[0].d.x*f; df0.x = spr[0].f.x*f;
			dr0.y = spr[0].r.y*f; dd0.y = spr[0].d.y*f; df0.y = spr[0].f.y*f;
			dr0.z = spr[0].r.z*f; dd0.z = spr[0].d.z*f; df0.z = spr[0].f.z*f;
			fp.x = ohit[1].x+f; fp.y = ohit[1].y+f; fp.z = ohit[1].z+f; oct_vox2worldpos(&spr[     0],&fp,&dp0);
			f = (1<<ohit[0].ls)*.5;
			dr1.x = hitr.x*f; dd1.x = hitd.x*f; df1.x = hitf.x*f;
			dr1.y = hitr.y*f; dd1.y = hitd.y*f; df1.y = hitf.y*f;
			dr1.z = hitr.z*f; dd1.z = hitd.z*f; df1.z = hitf.z*f;
			fp.x = ohit[0].x+f; fp.y = ohit[0].y+f; fp.z = ohit[0].z+f; //oct_vox2worldpos(&spr[curspr],&fp,&dp1);
			dp1.x = fp.x*hitr.x + fp.y*hitd.x + fp.z*hitf.x + hitp.x;
			dp1.y = fp.x*hitr.y + fp.y*hitd.y + fp.z*hitf.y + hitp.y;
			dp1.z = fp.x*hitr.z + fp.y*hitd.z + fp.z*hitf.z + hitp.z;

			for(y=-1;y<=1;y+=2)
				for(x=-1;x<=1;x+=2)
				{
					oct_drawcone(dp0.x+dr0.x* x+dd0.x* y+df0.x*-1, dp0.y+dr0.y* x+dd0.y* y+df0.y*-1, dp0.z+dr0.z* x+dd0.z* y+df0.z*-1,.0001,
									 dp0.x+dr0.x* x+dd0.x* y+df0.x*+1, dp0.y+dr0.y* x+dd0.y* y+df0.y*+1, dp0.z+dr0.z* x+dd0.z* y+df0.z*+1,.0001, 0xffc06060,48.0,0);
					oct_drawcone(dp0.x+dr0.x*-1+dd0.x* x+df0.x* y, dp0.y+dr0.y*-1+dd0.y* x+df0.y* y, dp0.z+dr0.z*-1+dd0.z* x+df0.z* y,.0001,
									 dp0.x+dr0.x*+1+dd0.x* x+df0.x* y, dp0.y+dr0.y*+1+dd0.y* x+df0.y* y, dp0.z+dr0.z*+1+dd0.z* x+df0.z* y,.0001, 0xffc06060,48.0,0);
					oct_drawcone(dp0.x+dr0.x* y+dd0.x*-1+df0.x* x, dp0.y+dr0.y* y+dd0.y*-1+df0.y* x, dp0.z+dr0.z* y+dd0.z*-1+df0.z* x,.0001,
									 dp0.x+dr0.x* y+dd0.x*+1+df0.x* x, dp0.y+dr0.y* y+dd0.y*+1+df0.y* x, dp0.z+dr0.z* y+dd0.z*+1+df0.z* x,.0001, 0xffc06060,48.0,0);
					oct_drawcone(dp1.x+dr1.x* x+dd1.x* y+df1.x*-1, dp1.y+dr1.y* x+dd1.y* y+df1.y*-1, dp1.z+dr1.z* x+dd1.z* y+df1.z*-1,.0001,
									 dp1.x+dr1.x* x+dd1.x* y+df1.x*+1, dp1.y+dr1.y* x+dd1.y* y+df1.y*+1, dp1.z+dr1.z* x+dd1.z* y+df1.z*+1,.0001, 0xff60c060,48.0,0);
					oct_drawcone(dp1.x+dr1.x*-1+dd1.x* x+df1.x* y, dp1.y+dr1.y*-1+dd1.y* x+df1.y* y, dp1.z+dr1.z*-1+dd1.z* x+df1.z* y,.0001,
									 dp1.x+dr1.x*+1+dd1.x* x+df1.x* y, dp1.y+dr1.y*+1+dd1.y* x+df1.y* y, dp1.z+dr1.z*+1+dd1.z* x+df1.z* y,.0001, 0xff60c060,48.0,0);
					oct_drawcone(dp1.x+dr1.x* y+dd1.x*-1+df1.x* x, dp1.y+dr1.y* y+dd1.y*-1+df1.y* x, dp1.z+dr1.z* y+dd1.z*-1+df1.z* x,.0001,
									 dp1.x+dr1.x* y+dd1.x*+1+df1.x* x, dp1.y+dr1.y* y+dd1.y*+1+df1.y* x, dp1.z+dr1.z* y+dd1.z*+1+df1.z* x,.0001, 0xff60c060,48.0,0);
				}

			oct_vox2worldpos(&spr[curspr],&spr[curspr].cen,&fp);
			oct_drawsph(fp.x,fp.y,fp.z,0.001, 0xffc0c0c0,48.0);

			pgram3d_init(&bi,&dr0,&dd0,&df0,&dr1,&dd1,&df1);
			if (pgram3d_gethit(&bi,&dp0,&dp1,&dpos,&hit,&norm))
			{
				oct_drawsph(hit.x,hit.y,hit.z,0.0004, 0xff6060c0,48.0);
				oct_drawcone(hit.x-norm.x*.03,hit.y-norm.y*.03,hit.z-norm.z*.03,0.0001, hit.x+norm.x*.03,hit.y+norm.y*.03,hit.z+norm.z*.03,0.0001, 0xff6060c0,48.0,0);
				oct_drawtext6x8(xres>>1,30,0xffffffff,0,"dpos: %f %f %f\nhit:%f %f %f\nnorm:%f %f %f",dpos.x,dpos.y,dpos.z,hit.x,hit.y,hit.z,norm.x,norm.y,norm.z);
			}

			spr[curspr].imulcol = ((int)(cos(tim*16.0)*16.0)+64)*0x10101 + 0xff000000;
		}
		else spr[curspr].imulcol = 0xff404040;
	}
	else
	{
			//Physics..
		for(i=sprnum-1;i>0;i--)
		{
			dpoint3d lvec;
			double dt, bdt, mat[9];
			spr_t sfree, shit, bsfree, bshit;
			oct_hit_t bohit[2];
			int bj;

			if (spr[i].cnt < 3) spr[i].v.y += dtim*.25; //gravity

			oct_drawtext6x8(xres>>1,30+i*10,0xffffffff,0,"spr[%d].v: %f %f %f",i,spr[i].v.x,spr[i].v.y,spr[i].v.z); //FIXFIXFIXFIX:remove

			bdt = 1.0;
			for(j=sprnum-1;j>=0;j--) ////FIX:doesn't handle hitting multiple sprites simultaneously properly :/
			{
				if (i == j) continue;
				dt = collide_binsearch(&spr[i],&spr[j],&sfree,&shit,dtim,ohit);
				if (dt <= bdt) { bdt = dt; bj = j; bsfree = sfree; bshit = shit; bohit[0] = ohit[0]; bohit[1] = ohit[1]; }
			}

				//precession (AngMom = Iworld*AngVel); see FIZEX3D.KC for derivation
			simxform(mat,spr[i].ori,spr[i].moi); matvecmul(&lvec,mat,&spr[i].ax);

			spr[i].p = bsfree.p; spr[i].r = bsfree.r; spr[i].d = bsfree.d; spr[i].f = bsfree.f;
			memcpy(&spr[i].ori,&bsfree.ori,sizeof(spr[i].ori));
			spr_update_rdf(&spr[i]);

				//precession (AngVel = Iworld^-1*AngMom); see FIZEX3D.KC for derivation
			simxform(mat,spr[i].ori,spr[i].rmoi); matvecmul(&spr[i].ax,mat,&lvec);

			if (bdt < 1.0)
			{
				pgram3d_t bi;
				dpoint3d dp0, dr0, dd0, df0, dp1, dr1, dd1, df1, dpos, hit, norm;

				//if (bdt == 0.0) { spr[i].v.x = 0; spr[i].v.y = 0.0; spr[i].v.z = 0.0; } //FIXFIXFIXFIX:prevents daffy ducking, but makes more stucks :/
				if (bdt == 0.0) spr[i].cnt++; else spr[i].cnt = 0;

				f = (1<<bohit[0].ls)*.5;
				dr0.x = bshit.r.x*f; dd0.x = bshit.d.x*f; df0.x = bshit.f.x*f;
				dr0.y = bshit.r.y*f; dd0.y = bshit.d.y*f; df0.y = bshit.f.y*f;
				dr0.z = bshit.r.z*f; dd0.z = bshit.d.z*f; df0.z = bshit.f.z*f;
				fp.x = bohit[0].x+f; fp.y = bohit[0].y+f; fp.z = bohit[0].z+f; oct_vox2worldpos(&bshit,&fp,&dp0);
				f = (1<<bohit[1].ls)*.5;
				dr1.x = spr[bj].r.x*f; dd1.x = spr[bj].d.x*f; df1.x = spr[bj].f.x*f;
				dr1.y = spr[bj].r.y*f; dd1.y = spr[bj].d.y*f; df1.y = spr[bj].f.y*f;
				dr1.z = spr[bj].r.z*f; dd1.z = spr[bj].d.z*f; df1.z = spr[bj].f.z*f;
				fp.x = bohit[1].x+f; fp.y = bohit[1].y+f; fp.z = bohit[1].z+f; oct_vox2worldpos(&spr[bj],&fp,&dp1);

				pgram3d_init(&bi,&dr0,&dd0,&df0,&dr1,&dd1,&df1);
				if (pgram3d_gethit(&bi,&dp0,&dp1,&dpos,&hit,&norm))
				{
					if (!bj) { spr[0].rmas = 0.0; for(k=0;k<9;k++) spr[0].rmoi[k] = 0.0; } //Hack to prevent spr[0] from moving
					if (keystatus[0x38]) { gravity = 0; fixhitit = 1; curspr = 1; hitp = bshit.p; hitr = bshit.r; hitd = bshit.d; hitf = bshit.f; break; } //FIX:debug only!

					oct_vox2worldpos(&spr[ i],&spr[ i].cen,&dp0);
					oct_vox2worldpos(&spr[bj],&spr[bj].cen,&dp1);
					doimpulse3d(0.5,&hit,&norm,&dp0,spr[ i].ori,&spr[ i].v,&spr[ i].ax,spr[ i].rmas,spr[ i].rmoi,
														&dp1,spr[bj].ori,&spr[bj].v,&spr[bj].ax,spr[bj].rmas,spr[bj].rmoi);
				}

				spr[i].tim += dtim;
				if (spr[i].tim < 1.0)
					{ if (spr[i].tim >= 0.0) spr[i].imulcol = (spr[i].imulcol&0xffffff) + (min(max((int)(255.0-spr[i].tim*256.0),0),255)<<24); }
				else { sprite_del(i); if (curspr >= sprnum) curspr = 0; }
			}
			else
			{
				spr[i].cnt = 0;
				spr[i].tim = max(spr[i].tim-dtim*.5,-2.0);
			}
			if (spr[i].p.y > 4.0) { sprite_del(i); if (curspr >= sprnum) curspr = 0; }
		}
	}

	if (shownorm)
	{
		oct_world2voxpos(&spr[curspr],&ipos,&fp);
		oct_world2voxdir(&spr[curspr],&ifor,&fp2);
		f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
		if (oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&i,0) != -1)
		{
			dpoint3d norm;
			for(z=-4;z<=4;z++)
				for(y=-4;y<=4;y++)
					for(x=-4;x<=4;x++)
					{
						if (oct_getvox(spr[curspr].oct,lp.x+x,lp.y+y,lp.z+z,0) != 1) continue;

						oct_estnorm(spr[curspr].oct,lp.x+x,lp.y+y,lp.z+z,3,&norm,0);

						fp2.x = lp.x+x+.5; fp2.y = lp.y+y+.5; fp2.z = lp.z+z+.5;
						oct_vox2worldpos(&spr[curspr],&fp2,&fp2);
						oct_vox2worlddir(&spr[curspr],&norm,&norm);

						oct_drawcone(fp2.x,fp2.y,fp2.z,0.0015, fp2.x-norm.x*4,fp2.y-norm.y*4,fp2.z-norm.z*4,0.0004, 0xffc06060,48.0,0);
					}
		}
	}

#if 0
	oct_world2voxpos(&spr[curspr],&ipos,&fp);
	oct_world2voxdir(&spr[curspr],&ifor,&fp2);
	f = 4096.0; fp2.x *= f; fp2.y *= f; fp2.z *= f;
	if (oct_hitscan(spr[curspr].oct,&fp,&fp2,&lp,&i,&f) != -1)
	{
		fp.x += fp2.x*f; fp.y += fp2.y*f; fp.z += fp2.z*f;
		oct_vox2worldpos(&spr[curspr],&fp,&fp);
		oct_drawsph(fp.x,fp.y,fp.z,0.0015, 0xffc06060,48.0);
	}
#endif


	if (selmode == 0)
	{
		glvert_t qv[4];
		float fx, fy;
		int xx[2], yy[2];

		f = min(xres,yres)/2*selmul;
		fx = xres/2 - ((seladdx-8.0)/8.0)*f; //fx = max(min(fx,xres-f),f);
		fy = yres/2 - ((seladdy-8.0)/8.0)*f; //fy = max(min(fy,yres-f),f);
		for(y=0;y<16;y++)
			for(x=0;x<16;x++)
			{
				for(i=4-1;i>=0;i--)
				{
					qv[i].u = ((float)x+.25+((i==1)||(i==2))*.5)/16.0; qv[i].x = ((float)(x+((i==1)||(i==2)))/8.0-1.0)*f + fx;
					qv[i].v = ((float)y+.25+((i==2)||(i==3))*.5)/16.0; qv[i].y = ((float)(y+((i==2)||(i==3)))/8.0-1.0)*f + fy;
					if ((oct_usegpu) && (oct_usefilter == 3)) qv[i].u = qv[i].u*0.5+0.5;
				}
				oct_drawpol(gtilid,qv,4,1.f,1.f,1.f,1.f,PROJ_SCREEN);
			}

		x = (int)floor(seladdx);
		y = (int)floor(seladdy);
		if (!((x|y)&-16))
		{
			for(i=2-1;i>=0;i--)
			{
				xx[i] = ((float)(x+i)/8.0-1.0)*f + fx;
				yy[i] = ((float)(y+i)/8.0-1.0)*f + fy;
			}
			for(j=2;j>0;j--)
			{
				for(i=4-1;i>=0;i--) oct_drawline(xx[(i==1)||(i==2)],yy[(i==2)||(i==3)],xx[(i==0)||(i==1)],yy[(i==1)||(i==2)],(int)(sin(tim*12.0)*63+192)*0x10101+0xff000000);
				xx[0]++; yy[0]++; xx[1]--; yy[1]--;
			}
		}
	}
	if (selmode == 1)
	{
		glvert_t qv[6];
		float cx = xres*.5;
		float cy = yres*.5;
		float rad = xres*.25;
		float rat = 1.0 - 1.0/(1<<LCOLSIZ);
		float r, g, b;
		for(i=0;i<6;i++)
		{
			float c, s;
			f = (float)i+.5; c = cos(f*PI/3.0); s = sin(f*PI/3.0);
			qv[i].u = (c*rat + 1.0)*.5; qv[i].x = c*rad + cx - seladdx*rad;
			qv[i].v = (s*rat + 1.0)*.5; qv[i].y = s*rad + cy - seladdy*rad;
		}
		oct_drawpol(gcolid,qv,6,selcolintens,selcolintens,selcolintens,1.f,PROJ_SCREEN);

		if (colpick_xy2rgb(seladdx,seladdy,selcolintens,&r,&g,&b))
		{
			for(i=0;i<4;i++)
			{
				qv[i].u = .0625; qv[i].x = (((i==1)||(i==2))*2-1)*44 + xres*.5;
				qv[i].v = .0625; qv[i].y = (((i==2)||(i==3))*2-1)*12 + yres*.5-18;
			}
			oct_drawpol(gcolid,qv,4,r,g,b,1.f,PROJ_SCREEN);

			for(i=0;i<4;i++)
			{
				qv[i].u = .0625; qv[i].x = (((i==1)||(i==2))*2-1)*38 + xres*.5;
				qv[i].v = .0625; qv[i].y = (((i==2)||(i==3))*2-1)* 6 + yres*.5-18;
			}
			oct_drawpol(gcolid,qv,4,0.f,0.f,0.f,1.f,PROJ_SCREEN);

			oct_drawtext6x8(xres*.5-3*11,yres*.5-22,0xffff6060,0,"%3d",min((int)(r*256.0),255));
			oct_drawtext6x8(xres*.5-3* 3,yres*.5-22,0xff50d050,0,"%3d",min((int)(g*256.0),255));
			oct_drawtext6x8(xres*.5+3* 5,yres*.5-22,0xff60a0ff,0,"%3d",min((int)(b*256.0),255));
		}
	}

	f = fps_period_est(dtim);
	tbuf[0] = 0;
	sprintf(&tbuf[strlen(tbuf)],"%.2fms %.2ffps\n",f*1000.f,1.f/f);
	sprintf(&tbuf[strlen(tbuf)],"S1:CPU_%s\n",USEASM?"SSE2":"C");
	if (!oct_usegpu)
	{
			  if (shaderfunc == cpu_shader_solcol   ) sprintf(&tbuf[strlen(tbuf)],"S2:CPU_SOL");
		else if (shaderfunc == cpu_shader_solcol_mc) sprintf(&tbuf[strlen(tbuf)],"S2:CPU_SOL_MC");
		else if (shaderfunc == cpu_shader_znotex   ) sprintf(&tbuf[strlen(tbuf)],"S2:CPU_ZNT");
		else if (shaderfunc == cpu_shader_texmap   ) sprintf(&tbuf[strlen(tbuf)],"S2:CPU_TEX");
		else if (shaderfunc == cpu_shader_texmap_mc) sprintf(&tbuf[strlen(tbuf)],"S2:CPU_TEX_MC");
	}
	else
	{
		if (oct_useglsl) sprintf(&tbuf[strlen(tbuf)],"S2:GLSL\n");
						else sprintf(&tbuf[strlen(tbuf)],"S2:ARBASM\n");
	}
	oct_drawtext6x8(0,0,0xffffffff,0,"%s",tbuf);

	x = xres-144;
	tbuf[0] = 0;
	sprintf(&tbuf[strlen(tbuf)],"  thr: %d (F1-F8)\n",oct_numcpu);
	sprintf(&tbuf[strlen(tbuf)],"  nod: %d/%d\n"     ,spr[curspr].oct->nod.num,spr[curspr].oct->nod.mal);
	sprintf(&tbuf[strlen(tbuf)],"  sur: %d/%d\n"     ,spr[curspr].oct->sur.num,spr[curspr].oct->sur.mal);
	sprintf(&tbuf[strlen(tbuf)]," lsid: %d %s\n"     ,spr[curspr].oct->lsid,MARCHCUBE?"MC":"");
	sprintf(&tbuf[strlen(tbuf)],"close: %d `\n"      ,gdrawclose);
	sprintf(&tbuf[strlen(tbuf)],"  rad: %d [/]\n",currad);
	if (speed >= 1.0) sprintf(&tbuf[strlen(tbuf)],"speed: %g {/}\n",speed);
					 else sprintf(&tbuf[strlen(tbuf)],"speed: 1/%g {/}\n",1.0/speed);
	if (docollide)
	{
		if (fatness >= 1.0) sprintf(&tbuf[strlen(tbuf)],"  fat: %g ;/'\n",fatness);
							else sprintf(&tbuf[strlen(tbuf)],"  fat: 1/%g ;/'\n",1.0/fatness);
	}
	sprintf(&tbuf[strlen(tbuf)]," sprn: %d\n",sprnum);
	if (gravity || docollide || hovercheck)
	{
		if (gravity) sprintf(&tbuf[strlen(tbuf)]," grav ");
		if (docollide) sprintf(&tbuf[strlen(tbuf)]," coll ");
		if (hovercheck) sprintf(&tbuf[strlen(tbuf)]," hovchk ");
		sprintf(&tbuf[strlen(tbuf)],"\n");
	}
	if (selmode >= 0) sprintf(&tbuf[strlen(tbuf)],"  tex: %d,%d,%d\n",selvox.x,selvox.y,selvox.z);
	oct_drawtext6x8(x,0,0xffffffff,0,"%s",tbuf);

	oct_drawtext6x8((xres>>1)-27,0,0xffffffff,0,"%7.2fms",testim*1000.0);

#if 0
	oct_print4debug_x = 0; oct_print4debug_y = 64;
	oct_drawtext6x8(oct_print4debug_x,oct_print4debug_y,0xffffffff,0,"%*c# LS CHI      SOL      MRK     IND MAL\n"
																						  "-----------------------------------------",spr[curspr].oct->lsid+6,' '); oct_print4debug_y += 8*2;
	oct_print4debug(spr[curspr].oct,spr[curspr].oct->head,0,0,0,spr[curspr].oct->lsid);
#endif

#if 0
	for(x=0;x<BITDIST*2+3;x++)
		for(y=0;y<BITDIST*2+3;y++)
			for(z=0;z<BITDIST*2+3;z++)
				if (bitvis[x][y]&(1<<z)) *(int *)(gdd.f + (x+50)*4 + (y+50+z*16)*gdd.p) = 0xffffff;
#endif

		//Draw 2D mouse cursor
	oct_drawtext6x8((xres>>1)-3,(yres>>1)-4,0xffffffff,0,"o");

	oct_stopdraw();
skipdd:;

	} CATCHEND
}
#endif

#if 0
!endif
#endif
