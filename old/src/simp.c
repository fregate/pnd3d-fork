#if 0 //To compile, type: nmake simp.c
objs=simp.obj pnd3d.obj drawcone.obj kplib.obj winmain.obj
libs=ddraw.lib ole32.lib dinput.lib dxguid.lib user32.lib gdi32.lib comdlg32.lib
opts=/c /TP /Ox /Ob2 /G6Fy /Gs /MD /QIfist #VC6
#opts=/c /TP /Ox /Ob2 /GFy /Gs /MT #>VC6
simp.exe: $(objs) simp.c; link $(objs) $(libs) /opt:nowin98 /nologo
	del simp.obj
simp.obj:     simp.c pnd3d.h sysmain.h ; cl simp.c      $(opts)
pnd3d.obj:    pnd3d.c pnd3d.h sysmain.h; cl pnd3d.c     $(opts)
drawcone.obj: drawcone.c               ; cl drawcone.c  $(opts)
kplib.obj:    kplib.c                  ; cl kplib.c     $(opts)
winmain.obj:  winmain.cpp sysmain.h    ; cl winmain.cpp $(opts) /DUSEKENSOUND=0 /DOFFSCREENHACK=1
!if 0
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include "pnd3d.h"
#include "sysmain.h"
#define PI 3.14159265358979323

static dpoint3d ipos, irig, idow, ifor, ihaf;

#define OCTMAX 256
static oct_t oct[OCTMAX];
static int octnum = 0;

#define SPRMAX 256
static spr_t spr[SPRMAX];
static int sprnum = 0;

static INT_PTR htex = 0;

void uninitapp ()
{
	int i;
	if (htex) { oct_freetex(htex); htex = 0; }
	for(i=octnum-1;i>=0;i--) oct_free(&oct[i]);
	oct_uninitonce();
}

int initapp2_cb (void)
{
	dpoint3d sp, sr, sd, sf;
	double d;
	int i;

	oct_usegpu = 1;

	oct_initonce(256.0);
	gtilid = oct_loadtiles("klabtiles.png");
	gskyid = oct_loadtex("kensky.jpg",KGL_BGRA32+KGL_LINEAR);
	oct_fogdist = 1e32; oct_fogcol = 0xff808080; //sky
	//oct_fogdist = 1.0; oct_fogcol = 0x80808080; //fog

#if 1
	htex = oct_loadtex("klabtiles.png",KGL_BGRA32+KGL_LINEAR+KGL_CLAMP_TO_EDGE);
	//htex = oct_loadtex("../build2/tex/0640_plates.jpg",KGL_BGRA32+KGL_LINEAR+KGL_CLAMP_TO_EDGE);
#else
	INT_PTR ptr; int x, y, xsiz, ysiz;
	xsiz = 64; ysiz = 64;
	ptr = (INT_PTR)malloc(xsiz*ysiz*4);
	for(y=0;y<ysiz;y++)
		for(x=0;x<xsiz;x++)
			*(int *)((xsiz*y + x)*4 + ptr) = ((x^y)*0x40400 + ((x+y)<<17)) | 0xff000000;
	htex = oct_loadtex(ptr,xsiz,ysiz,KGL_BGRA32+KGL_LINEAR+KGL_CLAMP_TO_EDGE);
#endif

	ipos.x = 0.0; ipos.y = 0.0; ipos.z =-1.0;
	irig.x = 1.0; irig.y = 0.0; irig.z = 0.0;
	idow.x = 0.0; idow.y = 1.0; idow.z = 0.0;
	ifor.x = 0.0; ifor.y = 0.0; ifor.z = 1.0;
	ihaf.x = xres*.5; ihaf.y = yres*.5; ihaf.z = ihaf.x;

	sprnum = 0; octnum = 0;
	//-----------------------------------------------------------------------------------------------
	oct_load(&oct[octnum],"caco.kvo",&sp,&sr,&sd,&sf);
	oct[octnum].tilid = gtilid;
	spr[sprnum].mixval = 0.5; spr[sprnum].imulcol = 0xff404040;
	spr[sprnum].oct = &oct[octnum]; d = 1.0/(double)oct[octnum].sid;
	spr[sprnum].p.x =-.5; spr[sprnum].p.y =-.5; spr[sprnum].p.z =-.5;
	spr[sprnum].r.x =  d; spr[sprnum].r.y =  0; spr[sprnum].r.z =  0;
	spr[sprnum].d.x =  0; spr[sprnum].d.y =  d; spr[sprnum].d.z =  0;
	spr[sprnum].f.x =  0; spr[sprnum].f.y =  0; spr[sprnum].f.z =  d;
	sprnum++; octnum++;
	//-----------------------------------------------------------------------------------------------

		//Hack with voxel textures
	for(i=0;i<oct[0].sur.mal;i++) SURFPTR(spr[0].oct,i)->tex = (rand()%5)+0;
	oct_copysurfs(&oct[0]);

	return(0);
}
long initapp (long argc, char **argv)
{
	xres = 1024; yres = 768; fullscreen = 0; colbits = 32; prognam = "SIMP by Ken Silverman";
	initapp2 = initapp2_cb;
	return(0);
}

void doframe (void)
{
	static double tim = 0, otim, dtim, avgdtim = 0.0;
	static int bstatus = 0, obstatus = 0;
	static surf_t cursurf;
	glvert_t qv[4];
	dpoint3d fp, vec;
	ipoint3d hit;
	double d;
	float f, fmousx, fmousy, fmousz;
	int i, x, y, z, isurf;

		//Read timer/mouse/keyboard
	otim = tim; readklock(&tim); dtim = tim-otim;
	obstatus = bstatus; readmouse(&fmousx,&fmousy,&fmousz,(long *)&bstatus);
	readkeyboard();

		//Handle rotation
	if (!(bstatus&2)) { oct_rotvex(fmousx*.01,&ifor,&irig); f = irig.y*.1; } else f = fmousx*-.01;
	oct_rotvex(fmousy*.01,&ifor,&idow);
	oct_rotvex(f         ,&idow,&irig);

		//Handle movement (calculate movement vector from arrows&shifts)
	f = dtim;
	if (keystatus[0x2a]) f *= 1.0/16.0; //LShift
	if (keystatus[0x36]) f *= 16.0/1.0; //RShift
	fp.x = (keystatus[0xcd]-keystatus[0xcb])*f; //Right-Left
	fp.y = (keystatus[0x52]-keystatus[0x9d])*f; //KP0-RCtrl
	fp.z = (keystatus[0xc8]-keystatus[0xd0])*f; //Up-Down
	vec.x = irig.x*fp.x + idow.x*fp.y + ifor.x*fp.z;
	vec.y = irig.y*fp.x + idow.y*fp.y + ifor.y*fp.z;
	vec.z = irig.z*fp.x + idow.z*fp.y + ifor.z*fp.z;

		//Collision detection
	oct_world2voxpos(&spr[0],&ipos,&ipos);
	oct_world2voxdir(&spr[0],&vec,&vec);
	oct_slidemove(spr[0].oct,&ipos,&vec,0.5,&ipos);
	oct_vox2worldpos(&spr[0],&ipos,&ipos);

	while (i = keyread())
	{
		switch((i>>8)&255)
		{
			case 0x01: quitloop(); break; //ESC
			case 0x0f: //Tab (grab surf)
				oct_world2voxpos(&spr[0],&ipos,&fp);
				oct_world2voxdir(&spr[0],&ifor,&vec); f = 4096.0; vec.x *= f; vec.y *= f; vec.z *= f;
				isurf = oct_hitscan(spr[0].oct,&fp,&vec,&hit,&i,0);
				if (isurf != -1) memcpy(&cursurf,SURFPTR(spr[0].oct,isurf),sizeof(surf_t));
				break;
			case 0x39: //Space (paint surf)
				oct_world2voxpos(&spr[0],&ipos,&fp);
				oct_world2voxdir(&spr[0],&ifor,&vec); f = 4096.0; vec.x *= f; vec.y *= f; vec.z *= f;
				isurf = oct_hitscan(spr[0].oct,&fp,&vec,&hit,&i,0);
				if (isurf != -1) oct_writesurf(spr[0].oct,isurf,&cursurf);
				break;
			case 0x13: //R (paint surf randomly)
				oct_world2voxpos(&spr[0],&ipos,&fp);
				oct_world2voxdir(&spr[0],&ifor,&vec); f = 4096.0; vec.x *= f; vec.y *= f; vec.z *= f;
				isurf = oct_hitscan(spr[0].oct,&fp,&vec,&hit,&i,0);
				if (isurf != -1)
				{
					cursurf.b = rand()&255;
					cursurf.g = rand()&255;
					cursurf.r = rand()&255;
					cursurf.tex = rand()%108;
					oct_writesurf(spr[0].oct,isurf,&cursurf);
				}
				break;
			case 0xd2: //Ins (insert voxels)
				oct_world2voxpos(&spr[0],&ipos,&fp);
				oct_world2voxdir(&spr[0],&ifor,&vec); f = 4096.0; vec.x *= f; vec.y *= f; vec.z *= f;
				isurf = oct_hitscan(spr[0].oct,&fp,&vec,&hit,&i,0);
				if (isurf != -1)
				{
					((int *)&hit.x)[i>>1] += (i&1)*2-1;
					if (oct_rebox(spr[0].oct,hit.x,hit.y,hit.z,hit.x+1,hit.y+1,hit.z+1,&x,&y,&z)) //grow octree if necessary
					{
						hit.x += x; hit.y += y; hit.z += z;
						spr[0].p.x -= (spr[0].r.x*x + spr[0].d.x*y + spr[0].f.x*z);
						spr[0].p.y -= (spr[0].r.y*x + spr[0].d.y*y + spr[0].f.y*z);
						spr[0].p.z -= (spr[0].r.z*x + spr[0].d.z*y + spr[0].f.z*z);
					}
					oct_setvox(spr[0].oct,hit.x,hit.y,hit.z,SURFPTR(spr[0].oct,isurf),1+2);
				}
				break;
			case 0xd3: //Del (delete voxels)
				oct_world2voxpos(&spr[0],&ipos,&fp);
				oct_world2voxdir(&spr[0],&ifor,&vec); f = 4096.0; vec.x *= f; vec.y *= f; vec.z *= f;
				isurf = oct_hitscan(spr[0].oct,&fp,&vec,&hit,&i,0);
				if (isurf != -1) oct_setvox(spr[0].oct,hit.x,hit.y,hit.z,SURFPTR(spr[0].oct,isurf),2);
				break;
		}
	}

		//Set window for drawing (handles both DirectDraw/OpenGL)
	if (oct_startdraw(&ipos,&irig,&idow,&ifor,ihaf.x,ihaf.y,ihaf.z) >= 0)
	{
			//Draw sprites
		for(i=0;i<sprnum;i++)
			oct_drawoct(spr[i].oct,&spr[i].p,&spr[i].r,&spr[i].d,&spr[i].f,spr[i].mixval,spr[i].imulcol);

			//Draw bonus graphics
		oct_drawcone(-cos(tim),-.3,-sin(tim),.05,+cos(tim),-.3,+sin(tim),.05,0xffc0b0a0,38.4,0);
		oct_drawcone(+sin(tim),-.3,-cos(tim),.05,-sin(tim),-.3,+cos(tim),.05,0xffc0b0a0,38.4,0);

#if 0
		for(i=4-1;i>=0;i--)
		{
			qv[i].u = ((i==1)||(i==2)); qv[i].x = ((float)qv[i].u-.5)*512.0 + xres/2;
			qv[i].v = ((i==2)||(i==3)); qv[i].y = ((float)qv[i].v-.5)*512.0 + yres/2;
		}
		oct_drawpol(htex,qv,4,1.f,1.f,1.f,1.f,PROJ_SCREEN);
#elif 1
		for(i=4-1;i>=0;i--)
		{
			qv[i].u = ((i==1)||(i==2)); qv[i].x = ((float)qv[i].u-.5)*1.0;
			qv[i].v = ((i==2)||(i==3)); qv[i].y = ((float)qv[i].v-.5)*1.0;
												 qv[i].z = 1.0/256.0;
		}
		oct_drawpol(htex,qv,4,1.f,1.f,1.f,1.f,PROJ_WORLD);
#elif 0
		for(i=4-1;i>=0;i--)
		{
			qv[i].u = ((i==1)||(i==2)); qv[i].x = ((float)qv[i].u-.5)*1.0;
			qv[i].v = ((i==2)||(i==3)); qv[i].y = ((float)qv[i].v-.5)*1.0;
												 qv[i].z = 1.0;
		}
		oct_drawpol(htex,qv,4,1.f,1.f,1.f,1.f,PROJ_CAM);
#endif

		oct_drawtext6x8((xres>>1)-3,(yres>>1)-4,0xffffffff,0,"o");
		oct_drawline(xres-68, 0,xres-68,10,0xffc0c0c0);
		oct_drawline(xres-68,10,xres   ,10,0xffc0c0c0);
		avgdtim += (dtim-avgdtim)*.01;
		oct_drawtext6x8(xres-64,0,0xffffffff,0,"%6.2f fps",1.0/avgdtim);

		oct_stopdraw();
	}
}

#if 0
!endif
#endif
