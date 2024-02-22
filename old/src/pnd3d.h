#ifndef PND3D_H
#define PND3D_H
#pragma pack(push,1)

//--------------------------------------------- Structs ---------------------------------------------
#define LOCT_MAXLS 4
#define OCT_MAXLS (1<<LOCT_MAXLS) //should be >= loctsid

typedef struct {   float x, y;    }  point2d;
typedef struct {  double x, y;    } dpoint2d;
typedef struct {     int x, y;    } ipoint2d;
typedef struct {   float x, y, z; }  point3d;
typedef struct {  double x, y, z; } dpoint3d;
typedef struct {     int x, y, z; } ipoint3d;

typedef struct { unsigned char chi, sol, mrk, mrk2; int ind; } octv_t;
#define MARCHCUBE 0
#if (MARCHCUBE == 0)
#define SURF_SIZ 8
typedef struct { unsigned char b, g, r, a; signed char norm[3], tex; } surf_t;
#else
#define SURF_SIZ 16
typedef struct { unsigned char b, g, r, a; signed char norm[3], tex; signed char cornval[8]; } surf_t;
#endif

	//buf: cast to: octv_t* or surf_t*
	//bit: 1 bit per sizeof(buf[0]); 0=free, 1=occupied
typedef struct { void *buf; unsigned int mal, *bit, ind, num, siz; } bitmal_t;
typedef struct oct_t //internal structure; most fields not useful to caller
{
	int head, lsid, sid, nsid;
	bitmal_t nod, sur;

		//assume neighboring voxels at boundary for oct_mod()
	char edgeiswrap; //bits 5-0: 0=air/sol, 1=wrap
	char edgeissol;  //bits 5-0: if (!edgeiswrap) { 0=air; 1=sol; } else reserved;

		//pnd3d private (don't modify these vars)
	unsigned int octid, tilid;  //gl texture indices
	unsigned int bufid;         //gl buffer indices
	int glxsid, glysid, gxsid, gysid;
	surf_t *gsurf;

	void (*recvoctfunc)(oct_t *ooct, oct_t *noct);
	int flags; //optional vars
} oct_t;

typedef struct
{
	oct_t *oct;
	dpoint3d p, v, ax;   //xlat&inst.ang vel
	double ori[9];       //unit orthonormal rotation matrix, [br,bd,bf]*ori = [r,d,f] body->world space
	dpoint3d br, bd, bf; //pgram, body (moi) space
	dpoint3d r, d, f;    //pgram, world space
	dpoint3d cen;
	double mas, rmas, moi[9], rmoi[9];

	float mixval;
	int imulcol;
	int cnt;
	double tim;
} spr_t;

//---------------------------------------------- Vars  ----------------------------------------------
extern float oct_fogdist; //use world coords
extern int oct_fogcol;
extern int oct_numcpu;    //Override # CPU threads to use (oct_initonce() autosets if 0)
extern int oct_usegpu;    //0=CPU, 1=GPU(ARBASM/GLSL); if (0) PIXMETH MUST be:{0,1}
extern int oct_useglsl;   //0=ARB ASM (usefilter 2 invalid), 1=GLSL (usefilter 3 is invalid)
extern int oct_usefilter; //0=Nearest, 1=Bilinear, 2=MipMap, 3=MipMap w/2Bilins (for ARB ASM)
extern int oct_sideshade[6];
extern INT_PTR gtilid, gskyid;

//--------------------------------------------- General ---------------------------------------------
int  oct_initonce   (float zfar); //use ~256.0 for 1 world unit = sprite; use ~65536.0 for 1 world unit = 1 voxel
void oct_uninitonce (void);
void oct_new    (oct_t *oct, int loctsid, INT_PTR tilid, int startnodes, int startsurfs, int hax4mark2spr); //ex: loctsid = 8 for 256^3
int  oct_load   (oct_t *oct, char *filnam,   point3d *p,  point3d *r,  point3d *d,  point3d *f); //supports:KVO,KV6,KVX,VOX,VXL,PNG
int  oct_load   (oct_t *oct, char *filnam,  dpoint3d *p, dpoint3d *r, dpoint3d *d, dpoint3d *f);
void oct_save   (oct_t *oct, char *filnam,   point3d *p,  point3d *r,  point3d *d,  point3d *f); //supports:KVO,KV6
void oct_save   (oct_t *oct, char *filnam,  dpoint3d *p, dpoint3d *r, dpoint3d *d, dpoint3d *f);
void oct_dup    (oct_t *oct, oct_t *newoct);
void oct_free   (oct_t *oct);

//--------------------------------------------- Render  ---------------------------------------------
	//All rendering must go between start&stop per frame
int  oct_startdraw   ( point3d *p,  point3d *r,  point3d *d,  point3d *f,  float hx,  float hy,  float hz);
int  oct_startdraw   (dpoint3d *p, dpoint3d *r, dpoint3d *d, dpoint3d *f, double hx, double hy, double hz);
void oct_stopdraw    (void);

	//Simple 2D stuff
void oct_drawpix     (int x, int y, int col);
void oct_drawline    (float x0, float y0, float x1, float y1, int col);
void oct_drawtext6x8 (int x0, int y0, int fcol, int bcol, const char *fmt, ...);

	//Sphere/Cone (useful for visualization/editing)
void oct_drawsph     (double x, double y, double z, double rad, int col, double shadfac);
void oct_drawcone    (double px0, double py0, double pz0, double pr0, double px1, double py1, double pz1, double pr1, int col, double shadefac, int flags);

	//Texture-mapped poly
enum { KGL_BGRA32=0, KGL_RGBA32/*faster xfer than KGL_BGRA32!*/, KGL_CHAR, KGL_SHORT, KGL_FLOAT, KGL_VEC4, KGL_NUM};
enum { KGL_LINEAR = (0<<4), KGL_NEAREST = (1<<4), KGL_MIPMAP = (2<<4),
		 KGL_MIPMAP3 = (2<<4), KGL_MIPMAP2 = (3<<4), KGL_MIPMAP1 = (4<<4), KGL_MIPMAP0 = (5<<4)};
enum { KGL_REPEAT = (0<<8), KGL_MIRRORED_REPEAT = (1<<8), KGL_CLAMP = (2<<8), KGL_CLAMP_TO_EDGE = (3<<8)};
INT_PTR oct_loadtex  (char *filnam, int flags);                    //use returned handle as input to oct_drawpol()
INT_PTR oct_loadtex  (INT_PTR ptr, int xsiz, int ysiz, int flags); //bitmap is copied & may be destroyed after call
INT_PTR oct_loadtiles(char *filnam);                               //same as oct_loadtex() but with hacks for tile buffer
void    oct_freetex  (INT_PTR ptr);
typedef struct { float x, y, z, u, v; } glvert_t;
enum {PROJ_SCREEN /*z ignored;no depth test*/, PROJ_WORLD, PROJ_CAM};
void oct_drawpol     (INT_PTR rptr, glvert_t *qv, int n, float rmul, float gmul, float bmul, float amul, int proj);

	//Octree
void oct_drawoct     (oct_t *oct,  point3d *p,  point3d *r,  point3d *d,  point3d *f, float mixval, int imulcol);
void oct_drawoct     (oct_t *oct, dpoint3d *p, dpoint3d *r, dpoint3d *d, dpoint3d *f, float mixval, int imulcol);

//---------------------------------------------- Brush ----------------------------------------------
	//isins() must return: 0:no int, 1:partial int, 2:total int
	//getsurf() must write surf structure
	//flags bit0:1=override normals at voxel boundaries
#define BRUSH_HEADER \
	int  (*isins  )(brush_t *brush, int x0, int y0, int z0, int log2sid);\
	void (*getsurf)(brush_t *brush, int x0, int y0, int z0, surf_t *surf);\
	int mx0, my0, mz0, mx1, my1, mz1, flags;
typedef struct brush_t { BRUSH_HEADER; /*add user data here*/ } brush_t; //generic brush used by mod functions

typedef struct { BRUSH_HEADER; int x, y, z; surf_t surf; } brush_vox_t;
void brush_vox_init (brush_vox_t *vox, int x, int y, int z, surf_t *surf);

#if (MARCHCUBE == 0)

typedef struct { BRUSH_HEADER; int x, y, z, r2, col; surf_t *surf; } brush_sph_t;
void brush_sph_init (brush_sph_t *sph, int x, int y, int z, int r, int issol);

typedef struct { BRUSH_HEADER; int x0, y0, z0, x1, y1, z1, col; } brush_box_t;
void brush_box_init (brush_box_t *box, int x0, int y0, int z0, int x1, int y1, int z1);

#else

typedef struct { BRUSH_HEADER; double x, y, z, r, r2, cornsc; int col, issol; surf_t *surf; } brush_sph_t;
void brush_sph_init (brush_sph_t *sph, double x, double y, double z, double r, int issol);

typedef struct { BRUSH_HEADER; double x0, y0, z0, x1, y1, z1, nx0, ny0, nz0, nx1, ny1, nz1, cornrad; int col, issol; surf_t *surf; } brush_box_t;
void brush_box_init (brush_box_t *box, double x0, double y0, double z0, double x1, double y1, double z1);

#endif

typedef struct { BRUSH_HEADER; float x0, y0, z0, r0, x1, y1, z1, r1, cx, cy, cz, cr, dx, dy, dz, dr, r02, r12, k0, k1, hak; int col; } brush_cone_t;
void brush_cone_init (brush_cone_t *cone, float x0, float y0, float z0, float r0, float x1, float y1, float z1, float r1);

typedef struct
{
	BRUSH_HEADER; unsigned short *boxsum; int xs, ys, zs, iux, iuy, iuz, ivx, ivy, ivz, iwx, iwy, iwz;
	int iox0[OCT_MAXLS], ioy0[OCT_MAXLS], ioz0[OCT_MAXLS], iox1[OCT_MAXLS], ioy1[OCT_MAXLS], ioz1[OCT_MAXLS];
} brush_bmp_t;
void brush_bmp_init (brush_bmp_t *bmp, unsigned short *boxsum, int xs, int ys, int zs, point3d *pp, point3d *pr, point3d *pd, point3d *pf);

//----------------------------------------------- Mod -----------------------------------------------
#define SURFPTR(oct,ind) (&((surf_t *)oct->sur.buf)[ind])

	//mode info:
	//(mode&1)==0:brush is air, !=0:brush is solid
	//(mode&2)!=0:do oct_updatesurfs() here (nice because bounding box calculated inside)
	//(mode&4)!=0:hover check
void oct_mod           (oct_t *oct, brush_t *brush, int mode); //mods existing oct; faster than oct_setvox() for large volumes
void oct_paint         (oct_t *oct, brush_t *brush); //like oct_mod but modify surfaces only
	//mode==0: newoct =  oct & brush          //most useful - extracts piece without requiring full copy
	//mode==1: newoct = (oct & brush)|~brush  //not useful - very unusual bool op
	//mode==2: newoct =  oct &~brush          //useful, but copy&oct_mod() also works
	//mode==3: newoct =  oct | brush          //useful, but copy&oct_mod() also works
void oct_modnew        (oct_t *oct, brush_t *brush, int mode); //doesn't touch existing oct; uses oct->recvoctfunc to generate new oct_t
void oct_updatesurfs   (oct_t *oct, int x0, int y0, int z0, int x1, int y1, int z1, brush_t *brush);
int  oct_getvox        (oct_t *oct, int x, int y, int z, surf_t **surf); //returns: 0:air, 1:surface (surf written), 2:interior
int  oct_getsol        (oct_t *oct, int x, int y, int z); //faster than oct_getvox() if only sol vs. air needed; returns: 0:air, 1:solid
int  oct_getsurf       (oct_t *oct, int x, int y, int z); //returns: -1:invalid, {0..sur.mal-1}:surface index
void oct_setvox        (oct_t *oct, int x, int y, int z, surf_t  *surf, int mode); //equivalent to using brush_vox_t with oct_mod()
void oct_writesurf     (oct_t *oct, int ind, surf_t *psurf); //proper way to write a surface - this function updates GPU memory
void oct_copysurfs     (oct_t *oct); //copy all surfs from CPU to GPU memory
int  oct_findsurfdowny (oct_t *oct, int x, int y, int z, surf_t **surf); //searches on increasing y, returns y and surf, or oct->sid if hit nothing
void oct_hover_check   (oct_t *oct, int x0, int y0, int z0, int x1, int y1, int z1, void (*recvoctfunc)(oct_t *ooct, oct_t *noct));
int  oct_rebox         (oct_t *oct, int x0, int y0, int z0, int x1, int y1, int z1, int *dx, int *dy, int *dz); //change octree size in-place
void oct_swizzle       (oct_t *oct, int ax0, int ax1, int ax2); //re-orient axes; x=1, y=2, z=3, -x=-1, -y=-2, -z=-3

typedef struct { int stkind[OCT_MAXLS], minls, mins, ox, oy, oz; } oct_getvox_hint_t;
void oct_getvox_hint_init (oct_t *oct, oct_getvox_hint_t *och);
int  oct_getsol_hint      (oct_t *oct, int x, int y, int z, oct_getvox_hint_t *och); //WARNING:assumes x,y,z inside grid
int  oct_getsurf_hint     (oct_t *oct, int x, int y, int z, oct_getvox_hint_t *och); //returns: -1:invalid, {0..sur.mal-1}:surface index

//--------------------------------------------- Physics ---------------------------------------------
int     oct_hitscan     (oct_t *oct,  point3d *p,  point3d *padd, ipoint3d *hit, int *rhitdir, float  *fracwent);
int     oct_hitscan     (oct_t *oct, dpoint3d *p, dpoint3d *padd, ipoint3d *hit, int *rhitdir, double *fracwent);
surf_t *oct_sphtrace    (oct_t *oct,  point3d *p,  point3d *padd, float  rad, ipoint3d *hit,  point3d *pgoal,  point3d *hitnorm);
surf_t *oct_sphtrace    (oct_t *oct, dpoint3d *p, dpoint3d *padd, double rad, ipoint3d *hit, dpoint3d *pgoal, dpoint3d *hitnorm);
double  oct_balloonrad  (oct_t *oct,  point3d *p, double cr, ipoint3d *hit, surf_t **hitsurf);
double  oct_balloonrad  (oct_t *oct, dpoint3d *p, double cr, ipoint3d *hit, surf_t **hitsurf);
int     oct_slidemove   (oct_t *oct,  point3d *p,  point3d *padd, double fat,  point3d *pgoal);
int     oct_slidemove   (oct_t *oct, dpoint3d *p, dpoint3d *padd, double fat, dpoint3d *pgoal);
#define USENEWCAC 0
#if (USENEWCAC == 0)
typedef struct { ipoint3d pt; unsigned int buf[32*32]; } oct_bitcac_t; //cache structure for repeated use of estnorm
#else
#define CACHASHN 32
#define CACN 32
typedef struct { ipoint3d pt[CACN]; int cur, ptn[CACN], n, hashead[CACHASHN]; unsigned int buf[CACN][32*32]; } oct_bitcac_t; //cache structure for repeated use of estnorm
#endif
void    oct_bitcac_reset(oct_bitcac_t *);
void    oct_estnorm     (oct_t *oct, int x, int y, int z, int r,  point3d *norm, oct_bitcac_t *bitcac);
void    oct_estnorm     (oct_t *oct, int x, int y, int z, int r, dpoint3d *norm, oct_bitcac_t *bitcac);
void    oct_refreshnorms(oct_t *oct, int dist, int x0, int y0, int z0, int x1, int y1, int z1);
void    oct_getmoi      (oct_t *oct,  float *mas,  float *cx,  float *cy,  float *cz,  float *ixx,  float *iyy,  float *izz,  float *ixy,  float *ixz,  float *iyz);
void    oct_getmoi      (oct_t *oct, double *mas, double *cx, double *cy, double *cz, double *ixx, double *iyy, double *izz, double *ixy, double *ixz, double *iyz);
int     oct_getboxstate (oct_t *oct, int bx0, int by0, int bz0, int bx1, int by1, int bz1); //returns: 0:pure air, 1:some air&sol, 2:pure sol
int     oct_getsolbbox  (oct_t *oct, int *x0, int *y0, int *z0, int *x1, int *y1, int *z1); //find bounding box of solid inside user bbox (params are in&out)
void    oct_sol2bit     (oct_t *oct, unsigned int *bitvis, int bx0, int by0, int bz0, int dx, int dy, int dz, int lsdax);
int     oct_touch_brush (oct_t *oct, brush_t *brush);
typedef struct { int x, y, z, ls; } oct_hit_t; //octree node hit (ls is log(2) of node side;0=1x1x1, 1=2x2x2, 2=4x4x4, ..)
int     oct_touch_oct   (oct_t *oct0,  point3d *p0,  point3d *r0,  point3d *d0,  point3d *f0,
								 oct_t *oct1,  point3d *p1,  point3d *r1,  point3d *d1,  point3d *f1, oct_hit_t *hit); //hit = 1st intersecting node per octree (array of 2!)
int     oct_touch_oct   (oct_t *oct0, dpoint3d *p0, dpoint3d *r0, dpoint3d *d0, dpoint3d *f0,
								 oct_t *oct1, dpoint3d *p1, dpoint3d *r1, dpoint3d *d1, dpoint3d *f1, oct_hit_t *hit);

//-------------------------------------------- Transform --------------------------------------------
void oct_rotvex       ( float ang,  point3d *a,  point3d *b); //Rotate vectors a & b around their common plane, by ang
void oct_rotvex       (double ang, dpoint3d *a, dpoint3d *b); //Rotate vectors a & b around their common plane, by ang
void oct_vox2worldpos (spr_t *sp,  point3d *pvox  ,  point3d *pworld);
void oct_vox2worldpos (spr_t *sp, dpoint3d *pvox  , dpoint3d *pworld);
void oct_vox2worlddir (spr_t *sp,  point3d *pvox  ,  point3d *pworld); //ignore origin
void oct_vox2worlddir (spr_t *sp, dpoint3d *pvox  , dpoint3d *pworld); //ignore origin
void oct_world2voxpos (spr_t *sp,  point3d *pworld,  point3d *pvox  );
void oct_world2voxpos (spr_t *sp, dpoint3d *pworld, dpoint3d *pvox  );
void oct_world2voxdir (spr_t *sp,  point3d *pworld,  point3d *pvox  ); //ignore origin
void oct_world2voxdir (spr_t *sp, dpoint3d *pworld, dpoint3d *pvox  ); //ignore origin

//---------------------------------------------------------------------------------------------------
#pragma pack(pop)
#endif
