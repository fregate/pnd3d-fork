#if 0
!if "$(AS)" == "ml"
kdisasm.exe: kdisasm.c; cl kdisasm.c /O1 /MD /nologo /DTESTKDISASM /link /nologo /opt:nowin98
!elseif "$(AS)" == "ml64"
kdisasm.exe: kdisasm_c.obj kdisasm_a.obj
	link      kdisasm_c.obj kdisasm_a.obj /out:kdisasm.exe
kdisasm_c.obj: kdisasm.c  ; cl   /Fokdisasm_c.obj /c kdisasm.c   /O1 /MT /nologo /DTESTKDISASM
kdisasm_a.obj: kdisasm.asm; ml64 /Fokdisasm_a.obj /c kdisasm.asm
!endif
!if 0
#endif

#ifdef TESTKDISASM
#include <stdlib.h>
#endif

//----------------------------------------- KDISASM begins -----------------------------------------
//Useful resources:
// * http://www.sandpile.org
// * 25366615.pdf / 25366715.pdf

#include <stdio.h>
#include <string.h>

#if !defined(max)
#define max(a,b)  (((a) > (b)) ? (a) : (b))
#endif
#if !defined(min)
#define min(a,b)  (((a) < (b)) ? (a) : (b))
#endif

	//Unfinished cases:
	//   GKAp,                //unable to generate ?
	//   GKRdCR8D,GKCR8DRd,   //unable to generate ?
	//   GKRdTd,GKTdRd,       //unable to generate ?
	//   GKVoWd,              //>=SSE3
	//   GKVoWw,              //>=SSE3
	//   GKEdVoIb,            //>=SSE3
	//   GKMdVoIb,            //>=SSE3
	//   GKMwVoIb,            //>=SSE3
	//   GKVoEdIb,            //>=SSE3
	//   GKVoMbIb,            //>=SSE3
	//   GKVoMdIb,            //>=SSE3
	//   GKVoWdIb,            //>=SSE3
	//   GKVoWqIb,            //>=SSE3
	//   GKGvMv,GKMvGv,       //SSE4? (movbe)
	//   GKMdVd,              //SSE4A (movntss)
	//   GKVoVo,              //SSE4A (extrq/insertq)
	//   GKVoIbIb,            //SSE4A (extrq)
	//   GKVoVoIbIb,          //SSE4A (insertq)
	//   GKVoWoV0,            //SSE4.1 (pblend/blend)
	//   GKVoMo,              //SSE4.1 (movntdqa), SSE3 (lddqu)
	//   GKMbVoIb,            //SSE4.1
	//   GKGdEb,GKGdEw,       //SSE4.2 (crc32)
	//   GKEdGd,GKGdEd,       //? (vmread/vmwrite)
	//
	//wait vs. [prefix]

enum
{
	GK=0,
	GKeAX,GKeCX,GKeDX,GKeBX,GKeSP,GKeBP,GKeSI,GKeDI,
	GKEAX,GKECX,GKEDX,GKEBX,GKESP,GKEBP,GKESI,GKEDI,
	GKrAX,GKrCX,GKrDX,GKrBX,GKrSP,GKrBP,GKrSI,GKrDI,
	GKrCXrAX,GKrDXrAX,GKrBXrAX,GKrSPrAX,GKrBPrAX,GKrSIrAX,GKrDIrAX,
	GKeAXIb,GKIbeAX,GKIbAL,GKALDX,GKDXAL,GKeAXDX,GKDXeAX,
	GKrAXIv,GKrCXIv,GKrDXIv,GKrBXIv,GKrSPIv,GKrBPIv,GKrSIIv,GKrDIIv,
	GKALIb ,GKCLIb ,GKDLIb ,GKBLIb ,GKAHIb ,GKCHIb ,GKDHIb ,GKBHIb,
	GKEb,GKEb1,GKEbCL,GKEw,GKEv,GKEv1,GKEvCL,GKEvIz,GKSTRB,GKSTRV,
	GKIbIv,GKALOb,GKObAL,GKOvrAX,GKrAXOv,GKrAXIz,GKIb,GKIz,
	GKAp,GKFv,GKGvM,GKJb,GKJz,
	GKnearIw,GKnear,GKfarIw,GKfar,GKCS,GKDS,GKES,GKGS,GKSS,
	GKMw,GKMd,GKMq,GKM,GKMs,GKMp,GKM512,GKM14M28,GKM94M108,
	GKGdVRo,GKGdPRq,GKVoPRq,GKVoVRo,GKPqVRq,GKPqPRq,
	GKPRqIb,GKVRoIb,GKGvEvIz,GKGdPRqIb,GKGdVRoIb,GKVoWoV0,GKVoVoIbIb,
	GKCdRd,GKRdCd,GKDdRd,GKRdDd,GKRdCR8D,GKCR8DRd,
	GKEdPd,GKEdVd,GKEbGb,GKEvGv,GKEbIb,GKEvIb,GKGbEb,GKGdWd,GKGdWq,GKGvEv,
	GKMoVo,GKMqPq,GKMqVq,GKPqEd,GKPqQd,GKPqQq,GKPqWo,GKPqWq,GKQqPq,GKVdEd,
	GKVdWd,GKVoEd,GKVoWo,GKVoMq,GKVoWq,GKVqEd,GKVqMq,GKVqWq,GKWdVd,GKWoVo,
	GKWqVq,GKEvGvIb,GKEvGvCL,GKPqMwIb,GKPqQqIb,
	GKEdGd,GKGdEd,GKEwGw,GKGdEb,GKGdEw,GKGvEb,GKGvEw,GKGvMa,GKGvMv,
	GKGzMp,GKIwIb,GKMdGd,GKMdVd,GKMvGv,GKMwSw,GKSwMw,GKRdTd,GKTdRd,
	GKVdWq,GKVoMo,GKVoVo,GKVoWd,GKVoWw,GKVqWd,
	GKEdVoIb,GKGvEvIb,GKMbVoIb,GKMdVoIb,GKMwVoIb,GKVoEdIb,GKVoIbIb,GKVoMbIb,
	GKVoMdIb,GKVoMwIb,GKVoWdIb,GKVoWqIb,
	GKVdWdIb,GKVqWqIb,GKVoWoIb,
	GKint16,GKint32,GKfloat,GKint64,GKdouble,GKpBCD,GKtbyte,
	GKAX,
	GKSTi,GKSTSTi,GKSTiST,
	GKST0  ,GKST1  ,GKST2  ,GKST3  ,GKST4  ,GKST5  ,GKST6  ,GKST7  ,
	GKSTST0,GKSTST1,GKSTST2,GKSTST3,GKSTST4,GKSTST5,GKSTST6,GKSTST7,
	GKST0ST,GKST1ST,GKST2ST,GKST3ST,GKST4ST,GKST5ST,GKST6ST,GKST7ST,
	GKNUM //=223 (<256:)
};

#pragma pack(push,1)
typedef struct { unsigned char op, parm[8]; char *st[8]; } x86_t; //each line is 41 bytes (+ string space allocated by C compiler)
#pragma pack(pop)

//--------------------------------------------------------------------------------------------------
	//1-byte opcodes:
static const x86_t op_xx[] =
{
	0x00, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GKES    ,GKES    , "add"   ,"add"  ,"add"   ,"add" ,"add"  ,"add" ,"push"   ,"pop"    ,
	0x08, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GKCS    ,GK      , "or"    ,"or"   ,"or"    ,"or"  ,"or"   ,"or"  ,"push"   ,""       ,
	0x10, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GKSS    ,GKSS    , "adc"   ,"adc"  ,"adc"   ,"adc" ,"adc"  ,"adc" ,"push"   ,"pop"    ,
	0x18, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GKDS    ,GKDS    , "sbb"   ,"sbb"  ,"sbb"   ,"sbb" ,"sbb"  ,"sbb" ,"push"   ,"pop"    ,
	0x20, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GK      ,GK      , "and"   ,"and"  ,"and"   ,"and" ,"and"  ,"and" ,"es:"    ,"daa"    ,
	0x28, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GK      ,GK      , "sub"   ,"sub"  ,"sub"   ,"sub" ,"sub"  ,"sub" ,"cs:"    ,"das"    ,
	0x30, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GK      ,GK      , "xor"   ,"xor"  ,"xor"   ,"xor" ,"xor"  ,"xor" ,"ss:"    ,"aaa"    ,
	0x38, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKALIb  ,GKrAXIz ,GK      ,GK      , "cmp"   ,"cmp"  ,"cmp"   ,"cmp" ,"cmp"  ,"cmp" ,"ds:"    ,"aas"    ,
	0x40, GKeAX  ,GKeCX   ,GKeDX   ,GKeBX   ,GKeSP   ,GKeBP   ,GKeSI   ,GKeDI   , "inc"   ,"inc"  ,"inc"   ,"inc" ,"inc"  ,"inc" ,"inc"    ,"inc"    ,
	0x48, GKeAX  ,GKeCX   ,GKeDX   ,GKeBX   ,GKeSP   ,GKeBP   ,GKeSI   ,GKeDI   , "dec"   ,"dec"  ,"dec"   ,"dec" ,"dec"  ,"dec" ,"dec"    ,"dec"    ,
	0x50, GKrAX  ,GKrCX   ,GKrDX   ,GKrBX   ,GKrSP   ,GKrBP   ,GKrSI   ,GKrDI   , "push"  ,"push" ,"push"  ,"push","push" ,"push","push"   ,"push"   ,
	0x58, GKrAX  ,GKrCX   ,GKrDX   ,GKrBX   ,GKrSP   ,GKrBP   ,GKrSI   ,GKrDI   , "pop"   ,"pop"  ,"pop"   ,"pop" ,"pop"  ,"pop" ,"pop"    ,"pop"    ,
	0x60, GK     ,GK      ,GKGvMa  ,GKEwGw  ,GK      ,GK      ,GK      ,GK      , "pusha" ,"popa" ,"bound" ,"arpl","fs:"  ,"gs:" ,""       ,""       ,
	0x68, GKIz   ,GKGvEvIz,GKIb    ,GKGvEvIb,GKSTRB  ,GKSTRV  ,GKSTRB  ,GKSTRV  , "push"  ,"imul" ,"push"  ,"imul","ins"  ,"ins" ,"outs"   ,"outs"   ,
	0x70, GKJb   ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    , "jo"    ,"jno"  ,"jb"    ,"jae" ,"jz"   ,"jnz" ,"jbe"    ,"ja"     ,
	0x78, GKJb   ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    ,GKJb    , "js"    ,"jns"  ,"jp"    ,"jnp" ,"jl"   ,"jge" ,"jle"    ,"jg"     ,
	0x80, GKEbIb ,GKEvIz  ,GKEbIb  ,GKEvIb  ,GKEbGb  ,GKEvGv  ,GKEbGb  ,GKEvGv  , "gr1"   ,"gr1"  ,"gr1"   ,"gr1" ,"test" ,"test","xchg"   ,"xchg"   ,
	0x88, GKEbGb ,GKEvGv  ,GKGbEb  ,GKGvEv  ,GKMwSw  ,GKGvM   ,GKSwMw  ,GK      , "mov"   ,"mov"  ,"mov"   ,"mov" ,"mov"  ,"lea" ,"mov"    ,"gr10"   ,
	0x90, GK     ,GKrCXrAX,GKrDXrAX,GKrBXrAX,GKrSPrAX,GKrBPrAX,GKrSIrAX,GKrDIrAX, "nop"   ,"xchg" ,"xchg"  ,"xchg","xchg" ,"xchg","xchg"   ,"xchg"   ,
	0x98, GK     ,GK      ,GKAp    ,GK      ,GKFv    ,GKFv    ,GK      ,GK      , "cbw"   ,"cwd"  ,"call"  ,"wait","pushf","popf","sahf"   ,"lahf"   ,
	0xa0, GKALOb ,GKrAXOv ,GKObAL  ,GKOvrAX ,GKSTRB  ,GKSTRV  ,GKSTRB  ,GKSTRV  , "mov"   ,"mov"  ,"mov"   ,"mov" ,"movs" ,"movs","cmps"   ,"cmps"   ,
	0xa8, GKALIb ,GKrAXIz ,GKSTRB  ,GKSTRV  ,GKSTRB  ,GKSTRV  ,GKSTRB  ,GKSTRV  , "test"  ,"test" ,"stos"  ,"stos","lods" ,"lods","scas"   ,"scas"   ,
	0xb0, GKALIb ,GKCLIb  ,GKDLIb  ,GKBLIb  ,GKAHIb  ,GKCHIb  ,GKDHIb  ,GKBHIb  , "mov"   ,"mov"  ,"mov"   ,"mov" ,"mov"  ,"mov" ,"mov"    ,"mov"    ,
	0xb8, GKrAXIv,GKrCXIv ,GKrDXIv ,GKrBXIv ,GKrSPIv ,GKrBPIv ,GKrSIIv ,GKrDIIv , "mov"   ,"mov"  ,"mov"   ,"mov" ,"mov"  ,"mov" ,"mov"    ,"mov"    ,
	0xc0, GKEbIb ,GKEvIb  ,GKnearIw,GKnear  ,GKGzMp  ,GKGzMp  ,GKEbIb  ,GKEvIz  , "gr2"   ,"gr2"  ,"ret"   ,"ret" ,"les"  ,"lds" ,"gr12"   ,"gr12"   ,
	0xc8, GKIwIb ,GK      ,GKfarIw ,GKfar   ,GK      ,GKIb    ,GK      ,GKFv    , "enter" ,"leave","ret"   ,"ret" ,"int 3","int" ,"into"   ,"iret"   ,
	0xd0, GKEb1  ,GKEv1   ,GKEbCL  ,GKEvCL  ,GKIb    ,GKIb    ,GK      ,GK      , "gr2"   ,"gr2"  ,"gr2"   ,"gr2" ,"aam"  ,"aad" ,"salc"   ,"xlat"   ,
	0xd8, GK     ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      , ""      ,""     ,""      ,""    ,""     ,""    ,""       ,""       ,
	0xe0, GKJb   ,GKJb    ,GKJb    ,GKJb    ,GKALIb  ,GKeAXIb ,GKIbAL  ,GKIbeAX , "loopne","loope","loop"  ,"jcxz","in"   ,"in"  ,"out"    ,"out"    ,
	0xe8, GKJz   ,GKJz    ,GKAp    ,GKJb    ,GKALDX  ,GKeAXDX ,GKDXAL  ,GKDXeAX , "call"  ,"jmp"  ,"jmp"   ,"jmp" ,"in"   ,"in"  ,"out"    ,"out"    ,
	0xf0, GK     ,GK      ,GK      ,GK      ,GK      ,GK      ,GKEb    ,GKEv    , "lock:" ,"int1" ,"repne:","rep:","hlt"  ,"cmc" ,"gr3"    ,"gr3"    ,
	0xf8, GK     ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      , "clc"   ,"stc"  ,"cli"   ,"sti" ,"cld"  ,"std" ,"gr4"    ,"gr5"    ,
};
//--------------------------------------------------------------------------------------------------
	//2-byte opcodes:
static const x86_t op_0f_xx[] =
{
	0x00, GK      ,GK      ,GKGvEw  ,GKGvEw,GK      ,GK      ,GK      ,GK      ,"gr6"      ,"gr7"        ,"lar"        ,"lsl"        ,""          ,"syscall"   ,"clts"     ,"sysret"    ,
	0x08, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"invd"     ,"wbinvd"     ,""           ,"ud2"        ,""          ,""          ,""         ,""          ,
	0x10, GKVoWo  ,GKWoVo  ,GKVqMq  ,GKMqVq,GKVoWo  ,GKVoWo  ,GKVqMq  ,GKMqVq  ,"movups"   ,"movups"     ,"movlps"     ,"movlps"     ,"unpcklps"  ,"unpckhps"  ,"movhps"   ,"movhps"    ,
	0x18, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"gr17"     ,"gr18"       ,"gr18"       ,"gr18"       ,"gr18"      ,"gr18"      ,"gr18"     ,"gr18"      ,
	0x20, GKRdCd  ,GKRdDd  ,GKCdRd  ,GKDdRd,GKRdTd  ,GK      ,GKTdRd  ,GK      ,"mov"      ,"mov"        ,"mov"        ,"mov"        ,"mov"       ,""          ,"mov"      ,""          ,
	0x28, GKVoWo  ,GKWoVo  ,GKVqMq  ,GKMoVo,GKPqWq  ,GKPqWq  ,GKVdWd  ,GKVdWd  ,"movaps"   ,"movaps"     ,"cvtpi2ps"   ,"movntps"    ,"cvttps2pi" ,"cvtps2pi"  ,"ucomiss"  ,"comiss"    ,
	0x30, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"wrmsr"    ,"rdtsc"      ,"rdmsr"      ,"rdpmc"      ,"sysenter"  ,"sysexit"   ,""         ,"getsec"    ,
 //0x38, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,""         ,""           ,""           ,""           ,""          ,""          ,""         ,""          ,
	0x40, GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv,GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv  ,"cmovo"    ,"cmovno"     ,"cmovb"      ,"cmovae"     ,"cmovz"     ,"cmovnz"    ,"cmovbe"   ,"cmova"     ,
	0x48, GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv,GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv  ,"cmovs"    ,"cmovns"     ,"cmovp"      ,"cmovnp"     ,"cmovl"     ,"cmovge"    ,"cmovle"   ,"cmovg"     ,
	0x50, GKGdVRo ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"movmskps" ,"sqrtps"     ,"rsqrtps"    ,"rcpps"      ,"andps"     ,"andnps"    ,"orps"     ,"xorps"     ,
	0x58, GKVoWo  ,GKVoWo  ,GKVoWq  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"addps"    ,"mulps"      ,"cvtps2pd"   ,"cvtdq2ps"   ,"subps"     ,"minps"     ,"divps"    ,"maxps"     ,
	0x60, GKPqQd  ,GKPqQd  ,GKPqQd  ,GKPqQq,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,"punpcklbw","punpcklwd"  ,"punpckldq"  ,"packsswb"   ,"pcmpgtb"   ,"pcmpgtw"   ,"pcmpgtd"  ,"packuswb"  ,
	0x68, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GK      ,GK      ,GKPqEd  ,GKPqQq  ,"punpckhbw","punpckhwd"  ,"punpckhdq"  ,"packssdw"   ,""          ,""          ,"movd"     ,"movq"      ,
	0x70, GKPqQqIb,GK      ,GK      ,GK    ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GK      ,"pshufw"   ,"gr13"       ,"gr14"       ,"gr15"       ,"pcmpeqb"   ,"pcmpeqw"   ,"pcmpeqd"  ,"emms"      ,
	0x78, GKEdGd  ,GKGdEd  ,GK      ,GK    ,GK      ,GK      ,GKEdPd  ,GKQqPq  ,"vmread"   ,"vmwrite"    ,""           ,""           ,""          ,""          ,"movd"     ,"movq"      ,
	0x80, GKJz    ,GKJz    ,GKJz    ,GKJz  ,GKJz    ,GKJz    ,GKJz    ,GKJz    ,"jo"       ,"jno"        ,"jb"         ,"jae"        ,"jz"        ,"jnz"       ,"jbe"      ,"ja"        ,
	0x88, GKJz    ,GKJz    ,GKJz    ,GKJz  ,GKJz    ,GKJz    ,GKJz    ,GKJz    ,"js"       ,"jns"        ,"jp"         ,"jnp"        ,"jl"        ,"jge"       ,"jle"      ,"jg"        ,
	0x90, GKEb    ,GKEb    ,GKEb    ,GKEb  ,GKEb    ,GKEb    ,GKEb    ,GKEb    ,"seto"     ,"setno"      ,"setb"       ,"setae"      ,"setz"      ,"setnz"     ,"setbe"    ,"seta"      ,
	0x98, GKEb    ,GKEb    ,GKEb    ,GKEb  ,GKEb    ,GKEb    ,GKEb    ,GKEb    ,"sets"     ,"setns"      ,"setp"       ,"setnp"      ,"setl"      ,"setge"     ,"setle"    ,"setg"      ,
	0xa0, GK      ,GK      ,GK      ,GKEvGv,GKEvGvIb,GKEvGvCL,GK      ,GK      ,"push fs"  ,"pop fs"     ,"cpuid"      ,"bt"         ,"shld"      ,"shld"      ,""         ,""          ,
	0xa8, GKGS    ,GKGS    ,GK      ,GKEvGv,GKEvGvIb,GKEvGvCL,GK      ,GKGvEv  ,"push"     ,"pop"        ,"rsm"        ,"bts"        ,"shrd"      ,"shrd"      ,"gr16"     ,"imul"      ,
	0xb0, GKEbGb  ,GKEvGv  ,GKGzMp  ,GKEvGv,GKGzMp  ,GKGzMp  ,GKGvEb  ,GKGvEw  ,"cmpxchg"  ,"cmpxchg"    ,"lss"        ,"btr"        ,"lfs"       ,"lgs"       ,"movzx"    ,"movzx"     ,
	0xb8, GKJz    ,GK      ,GKEvIb  ,GKEvGv,GKGvEv  ,GKGvEv  ,GKGvEb  ,GKGvEw  ,"jmpe"     ,"gr11"       ,"gr8"        ,"btc"        ,"bsf"       ,"bsr"       ,"movsx"    ,"movsx"     ,
	0xc0, GKEbGb  ,GKEvGv  ,GKVoWoIb,GKMdGd,GKPqMwIb,GKGdPRqIb,GKVoWoIb,GK     ,"xadd"     ,"xadd"       ,"cmpccps"    ,"movnti"     ,"pinsrw"    ,"pextrw"    ,"shufps"   ,"gr9"       ,
	0xc8, GKEAX   ,GKECX   ,GKEDX   ,GKEBX ,GKESP   ,GKEBP   ,GKESI   ,GKEDI   ,"bswap"    ,"bswap"      ,"bswap"      ,"bswap"      ,"bswap"     ,"bswap"     ,"bswap"    ,"bswap"     ,
	0xd0, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GK      ,GKGdPRq ,""         ,"psrlw"      ,"psrld"      ,"psrlq"      ,"paddq"     ,"pmullw"    ,""         ,"pmovmskb"  ,
	0xd8, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,"psubusb"  ,"psubusw"    ,"pminub"     ,"pand"       ,"paddusb"   ,"paddusw"   ,"pmaxub"   ,"pandn"     ,
	0xe0, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GK      ,GKMqPq  ,"pavgb"    ,"psraw"      ,"psrad"      ,"pavgw"      ,"pmulhuw"   ,"pmulhw"    ,""         ,"movntq"    ,
	0xe8, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,"psubsb"   ,"psubsw"     ,"pminsw"     ,"por"        ,"paddsb"    ,"paddsw"    ,"pmaxsw"   ,"pxor"      ,
	0xf0, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqPRq ,""         ,"psllw"      ,"pslld"      ,"psllq"      ,"pmuludq"   ,"pmaddwd"   ,"psadbw"   ,"maskmovq"  ,
	0xf8, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,"psubb"    ,"psubw"      ,"psubd"      ,"psubq"      ,"paddb"     ,"paddw"     ,"paddd"    ,""          ,
};

static const x86_t op_f3_0f_xx[] =
{
	0x10, GKVdWd  ,GKWdVd  ,GKVoWo  ,GK    ,GK      ,GK      ,GKVoWo  ,GK      ,"movss"    ,"movss"      ,"movsldup"   ,""           ,""          ,""          ,"movshdup" ,""          ,
	0x28, GK      ,GK      ,GKVdEd  ,GKMdVd,GKGdWd  ,GKGdWd  ,GK      ,GK      ,""         ,""           ,"cvtsi2ss"   ,"movntss"    ,"cvttss2si" ,"cvtss2si"  ,""         ,""          ,
	0x50, GK      ,GKVdWd  ,GKVdWd  ,GKVdWd,GK      ,GK      ,GK      ,GK      ,""         ,"sqrtss"     ,"rsqrtss"    ,"rcpss"      ,""          ,""          ,""         ,""          ,
	0x58, GKVdWd  ,GKVdWd  ,GKVqWd  ,GKVoWo,GKVdWd  ,GKVdWd  ,GKVdWd  ,GKVdWd  ,"addss"    ,"mulss"      ,"cvtss2sd"   ,"cvttps2dq"  ,"subss"     ,"minss"     ,"divss"    ,"maxss"     ,
	0x68, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GKVoWo  ,""         ,""           ,""           ,""           ,""          ,""          ,""         ,"movdqu"    ,
	0x70, GKVoWoIb,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"pshufhw"  ,""           ,""           ,""           ,""          ,""          ,""         ,""          ,
	0x78, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GKVoWo  ,GKWoVo  ,""         ,""           ,""           ,""           ,""          ,""          ,"movq"     ,"movdqu"    ,
	0xb8, GKGvEv  ,GK      ,GK      ,GK    ,GK      ,GKGvEv  ,GK      ,GK      ,"popcnt"   ,""           ,""           ,""           ,""          ,"lzcnt"     ,""         ,""          ,
	0xc0, GKEbGb  ,GKEvGv  ,GKVdWdIb,GK    ,GK      ,GK      ,GK      ,GK      ,"xadd"     ,"xadd"       ,"cmpccss"    ,""           ,""          ,""          ,""         ,"gr9"       ,
	0xd0, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GKVoPRq ,GK      ,""         ,""           ,""           ,""           ,""          ,""          ,"movq2dq"  ,""          ,
	0xe0, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GKVoWq  ,GK      ,""         ,""           ,""           ,""           ,""          ,""          ,"cvtdq2pd" ,""          ,
};

static const x86_t op_66_0f_xx[] =
{
	0x00, GK      ,GK      ,GKGvEw  ,GKGvEw,GK      ,GK      ,GK      ,GK      ,""         ,""           ,"lar"        ,"lsl"        ,""          ,""          ,""         ,""          ,
	0x10, GKVoWo  ,GKWoVo  ,GKVqMq  ,GKMqVq,GKVoWo  ,GKVoWo  ,GKVqMq  ,GKMqVq  ,"movupd"   ,"movupd"     ,"movlpd"     ,"movlpd"     ,"unpcklpd"  ,"unpckhpd"  ,"movhpd"   ,"movhpd"    ,
	0x28, GKVoWo  ,GKWoVo  ,GKVoMq  ,GKMoVo,GKPqWo  ,GKPqWo  ,GKVqWq  ,GKVqWq  ,"movapd"   ,"movapd"     ,"cvtpi2pd"   ,"movntpd"    ,"cvttpd2pi" ,"cvtpd2pi"  ,"ucomisd"  ,"comisd"    ,
	0x40, GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv,GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv  ,"cmovo"    ,"cmovno"     ,"cmovb"      ,"cmovnb"     ,"cmovz"     ,"cmovnz"    ,"cmovbe"   ,"cmovnbe"   ,
	0x48, GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv,GKGvEv  ,GKGvEv  ,GKGvEv  ,GKGvEv  ,"cmovs"    ,"cmovns"     ,"cmovp"      ,"cmovnp"     ,"cmovl"     ,"cmovnl"    ,"cmovle"   ,"cmovnle"   ,
	0x50, GKGdVRo ,GKVoWo  ,GK      ,GK    ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"movmskpd" ,"sqrtpd"     ,""           ,""           ,"andpd"     ,"andnpd"    ,"orpd"     ,"xorpd"     ,
	0x58, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"addpd"    ,"mulpd"      ,"cvtpd2ps"   ,"cvtps2dq"   ,"subpd"     ,"minpd"     ,"divpd"    ,"maxpd"     ,
	0x60, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"punpcklbw","punpcklwd"  ,"punpckldq"  ,"packsswb"   ,"pcmpgtb"   ,"pcmpgtw"   ,"pcmpgtd"  ,"packuswb"  ,
	0x68, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWq  ,GKVoWo  ,GKVoEd  ,GKVoWo  ,"punpckhbw","punpckhwd"  ,"punpckhdq"  ,"packssdw"   ,"punpcklqdq","punpckhqdq","movd"     ,"movdqa"    ,
	0x70, GKVoWoIb,GK      ,GK      ,GK    ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GK      ,"pshufd"   ,"gr13"       ,"gr14"       ,"gr15"       ,"pcmpeqb"   ,"pcmpeqw"   ,"pcmpeqd"  ,""          ,
	0x78, GKVoIbIb,GKVoVo  ,GK      ,GK    ,GKVoWo  ,GKVoWo  ,GKEdVd  ,GKWoVo  ,"extrq"    ,"extrq"      ,""           ,""           ,"haddpd"    ,"hsubpd"    ,"movd"     ,"movdqa"    ,
	0xa0, GK      ,GK      ,GK      ,GKEvGv,GKEvGvIb,GKEvGvCL,GK      ,GK      ,""         ,""           ,""           ,"bt"         ,"shld"      ,"shld"      ,""         ,""          ,
	0xa8, GK      ,GK      ,GK      ,GKEvGv,GKEvGvIb,GKEvGvCL,GK      ,GK      ,""         ,""           ,""           ,"bts"        ,"shrd"      ,"shrd"      ,""         ,""          ,
	0xb0, GK      ,GKEvGv  ,GKGzMp  ,GKEvGv,GKGzMp  ,GKGzMp  ,GKGvEb  ,GKGvEw  ,""         ,"cmpxchg"    ,"lss"        ,"btr"        ,"lfs"       ,"lgs"       ,"movzx"    ,"movzx"     ,
	0xb8, GK      ,GK      ,GKEvIb  ,GKEvGv,GKGvEv  ,GKGvEv  ,GKGvEb  ,GKGvEw  ,""         ,"gr11"       ,"gr8"        ,"btc"        ,"bsf"       ,"bsr"       ,"movsx"    ,"movsx"     ,
	0xc0, GKEbGb  ,GKEvGv  ,GKVoWoIb,GK    ,GKVoMwIb,GKGdVRoIb,GKVoWoIb,GK     ,"xadd"     ,"xadd"       ,"cmpccpd"    ,""           ,"pinsrw"    ,"pextrw"    ,"shufpd"   ,"gr9"       ,
	0xd0, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKMqVq  ,GKGdVRo ,"addsubpd" ,"psrlw"      ,"psrld"      ,"psrlq"      ,"paddq"     ,"pmullw"    ,"movq"     ,"pmovmskb"  ,
	0xd8, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"psubusb"  ,"psubusw"    ,"pminub"     ,"pand"       ,"paddusb"   ,"paddusw"   ,"pmaxub"   ,"pandn"     ,
	0xe0, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKMoVo  ,"pavgb"    ,"psraw"      ,"psrad"      ,"pavgw"      ,"pmulhuw"   ,"pmulhw"    ,"cvttpd2dq","movntdq"   ,
	0xe8, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"psubsb"   ,"psubsw"     ,"pminsw"     ,"por"        ,"paddsb"    ,"paddsw"    ,"pmaxsw"   ,"pxor"      ,
	0xf0, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoVRo ,""         ,"psllw"      ,"pslld"      ,"psllq"      ,"pmuludq"   ,"pmaddwd"   ,"psadbw"   ,"maskmovdqu",
	0xf8, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,"psubb"    ,"psubw"      ,"psubd"      ,"psubq"      ,"paddb"     ,"paddw"     ,"paddd"    ,""          ,
};

static const x86_t op_f2_0f_xx[] =
{
	0x10, GKVqWq  ,GKWqVq  ,GKVoWq  ,GK    ,GK      ,GK      ,GK      ,GK      ,"movsd"    ,"movsd"      ,"movddup"    ,""           ,""          ,""          ,""         ,""          ,
	0x28, GK      ,GK      ,GKVqEd  ,GKMqVq,GKGdWq  ,GKGdWq  ,GK      ,GK      ,""         ,""           ,"cvtsi2sd"   ,"movntsd"    ,"cvttsd2si" ,"cvtsd2si"  ,""         ,""          ,
	0x50, GK      ,GKVqWq  ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,""         ,"sqrtsd"     ,""           ,""           ,""          ,""          ,""         ,""          ,
	0x58, GKVqWq  ,GKVqWq  ,GKVdWq  ,GK    ,GKVqWq  ,GKVqWq  ,GKVqWq  ,GKVqWq  ,"addsd"    ,"mulsd"      ,"cvtsd2ss"   ,""           ,"subsd"     ,"minsd"     ,"divsd"    ,"maxsd"     ,
	0x70, GKVoWoIb,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"pshuflw"  ,""           ,""           ,""           ,""          ,""          ,""         ,""          ,
	0x78, GKVoVoIbIb,GKVoVo,GK      ,GK    ,GKVoWo  ,GKVoWo  ,GK      ,GK      ,"insertq"  ,"insertq"    ,""           ,""           ,"haddps"    ,"hsubps"    ,""         ,""          ,
	0xc0, GKEbGb  ,GKEvGv  ,GKVqWqIb,GK    ,GK      ,GK      ,GK      ,GK      ,"xadd"     ,"xadd"       ,"cmpccsd"    ,""           ,""          ,""          ,""         ,"gr9"       ,
	0xd0, GKVoWo  ,GK      ,GK      ,GK    ,GK      ,GK      ,GKPqVRq ,GK      ,"addsubps" ,""           ,""           ,""           ,""          ,""          ,"movdq2q"  ,""          ,
	0xe0, GK      ,GK      ,GK      ,GK    ,GK      ,GK      ,GKVoWo  ,GK      ,""         ,""           ,""           ,""           ,""          ,""          ,"cvtpd2dq" ,""          ,
	0xf0, GKVoMo  ,GK      ,GK      ,GK    ,GK      ,GK      ,GK      ,GK      ,"lddqu"    ,""           ,""           ,""           ,""          ,""          ,""         ,""          ,
};

static const x86_t op_f0_0f_xx[] =
{
	0x20, GKRdCR8D,GK      ,GKCR8DRd,GK    ,GK      ,GK      ,GK      ,GK      ,"mov"      ,""           ,"mov"        ,""           ,""          ,""          ,""         ,""          ,
};
//--------------------------------------------------------------------------------------------------
	//FPU:
static const x86_t op_d8_xx[] =
{
	0x00, GKfloat ,GKfloat ,GKfloat ,GKfloat ,GKfloat  ,GKfloat ,GKfloat  ,GKfloat  ,"fadd"    ,"fmul"    ,"fcom"    ,"fcomp"   ,"fsub"    ,"fsubr"   ,"fdiv"    ,"fdivr"   ,
	0xc0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,
	0xc8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,
	0xd0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,
	0xd8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,
	0xe0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,
	0xe8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,
	0xf0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,
	0xf8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,
};
static const x86_t op_d9_xx[] =
{
	0x00, GKfloat ,GK      ,GKfloat ,GKfloat ,GKM14M28 ,GKMw    ,GKM14M28 ,GKMw     ,"fld"     ,""        ,"fst"     ,"fstp"    ,"fldenv"  ,"fldcw"   ,"fstenv"  ,"fstcw"   ,
	0xc0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fld"     ,"fld"     ,"fld"     ,"fld"     ,"fld"     ,"fld"     ,"fld"     ,"fld"     ,
	0xc8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,
	0xd0, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"fnop"    ,""        ,""        ,""        ,""        ,""        ,""        ,""        ,
	0xd8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,
	0xe0, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"fchs"    ,"fabs"    ,""        ,""        ,"ftst"    ,"fxam"    ,""        ,""        ,
	0xe8, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"fld1"    ,"fldl2t"  ,"fldl2e"  ,"fldpi"   ,"fldlg2"  ,"fldln2"  ,"fldz"    ,""        ,
	0xf0, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"f2xm1"   ,"fyl2x"   ,"fptan"   ,"fpatan"  ,"fxtract" ,"fprem1"  ,"fdecstp" ,"fincstp" ,
	0xf8, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"fprem"   ,"fyl2xp1" ,"fsqrt"   ,"fsincos" ,"frndint" ,"fscale"  ,"fsin"    ,"fcos"    ,
};
static const x86_t op_da_xx[] =
{
	0x00, GKint32 ,GKint32 ,GKint32 ,GKint32 ,GKint32  ,GKint32 ,GKint32  ,GKint32  ,"fiadd"   ,"fimul"   ,"ficom"   ,"ficomp"  ,"fisub"   ,"fisubr"  ,"fidiv"   ,"fidivr"  ,
	0xc0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,"fcmovb"  ,
	0xc8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,"fcmove"  ,
	0xd0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,"fcmovbe" ,
	0xd8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,"fcmovu"  ,
	0xe8, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,""        ,"fucompp" ,""        ,""        ,""        ,""        ,""        ,""        ,
};
static const x86_t op_db_xx[] =
{
	0x00, GKint32 ,GKint32 ,GKint32 ,GKint32 ,GK       ,GKtbyte ,GK       ,GKtbyte  ,"fild"    ,"fisttp"  ,"fist"    ,"fistp"   ,""        ,"fld"     ,""        ,"fstp"    ,
	0xc0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,"fcmovnb" ,
	0xc8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovne" ,"fcmovne" ,"fcmovne" ,"fcmovne" ,"fcmovne" ,"fcmovne" ,"fcmovne" ,"fcmovne" ,
	0xd0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovnbe","fcmovnbe","fcmovnbe","fcmovnbe","fcmovnbe","fcmovnbe","fcmovnbe","fcmovnbe",
	0xd8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,"fcmovnu" ,
	0xe0, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,"feni"    ,"fdisi"   ,"fclex"   ,"finit"   ,"fsetpm"  ,"frstpm"  ,""        ,""        ,
	0xe8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,"fucomi"  ,
	0xf0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcomi"   ,"fcomi"   ,"fcomi"   ,"fcomi"   ,"fcomi"   ,"fcomi"   ,"fcomi"   ,"fcomi"   ,
};
static const x86_t op_dc_xx[] =
{
	0x00, GKdouble,GKdouble,GKdouble,GKdouble,GKdouble ,GKdouble,GKdouble ,GKdouble ,"fadd"    ,"fmul"    ,"fcom"    ,"fcomp"   ,"fsub"    ,"fsubr"   ,"fdiv"    ,"fdivr"   ,
	0xc0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,"fadd"    ,
	0xc8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,"fmul"    ,
	0xd0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,"fcom"    ,
	0xd8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,
	0xe0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,"fsubr"   ,
	0xe8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,"fsub"    ,
	0xf0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,"fdivr"   ,
	0xf8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,"fdiv"    ,
};
static const x86_t op_dd_xx[] =
{
	0x00, GKdouble,GKint64 ,GKdouble,GKdouble,GKM94M108,GK      ,GKM94M108,GKMw     ,"fld"     ,"fisttp"  ,"fst"     ,"fstp"    ,"frstor"  ,""        ,"fsave"   ,"fstsw"   ,
	0xc0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"ffree"   ,"ffree"   ,"ffree"   ,"ffree"   ,"ffree"   ,"ffree"   ,"ffree"   ,"ffree"   ,
	0xc8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,
	0xd0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fst"     ,"fst"     ,"fst"     ,"fst"     ,"fst"     ,"fst"     ,"fst"     ,"fst"     ,
	0xd8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,
	0xe0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fucom"   ,"fucom"   ,"fucom"   ,"fucom"   ,"fucom"   ,"fucom"   ,"fucom"   ,"fucom"   ,
	0xe8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fucomp"  ,"fucomp"  ,"fucomp"  ,"fucomp"  ,"fucomp"  ,"fucomp"  ,"fucomp"  ,"fucomp"  ,
};
static const x86_t op_de_xx[] =
{
	0x00, GKint16 ,GKint16 ,GKint16 ,GKint16 ,GKint16  ,GKint16 ,GKint16  ,GKint16  ,"fiadd"   ,"fimul"   ,"ficom"   ,"ficomp"  ,"fisub"   ,"fisubr"  ,"fidiv"   ,"fidivr"  ,
	0xc0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"faddp"   ,"faddp"   ,"faddp"   ,"faddp"   ,"faddp"   ,"faddp"   ,"faddp"   ,"faddp"   ,
	0xc8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fmulp"   ,"fmulp"   ,"fmulp"   ,"fmulp"   ,"fmulp"   ,"fmulp"   ,"fmulp"   ,"fmulp"   ,
	0xd0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,"fcomp"   ,
	0xd8, GK      ,GK      ,GK      ,GK      ,GK       ,GK      ,GK       ,GK       ,""        ,"fcompp"  ,""        ,""        ,""        ,""        ,""        ,""        ,
	0xe0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,"fsubrp"  ,
	0xe8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fsubp"   ,"fsubp"   ,"fsubp"   ,"fsubp"   ,"fsubp"   ,"fsubp"   ,"fsubp"   ,"fsubp"   ,
	0xf0, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,"fdivrp"  ,
	0xf8, GKST0ST ,GKST1ST ,GKST2ST ,GKST3ST ,GKST4ST  ,GKST5ST ,GKST6ST  ,GKST7ST  ,"fdivp"   ,"fdivp"   ,"fdivp"   ,"fdivp"   ,"fdivp"   ,"fdivp"   ,"fdivp"   ,"fdivp"   ,
};
static const x86_t op_df_xx[] =
{
	0x00, GKint16 ,GKint16 ,GKint16 ,GKint16 ,GKpBCD   ,GKint64 ,GKpBCD   ,GKint64  ,"fild"    ,"fisttp"  ,"fist"    ,"fistp"   ,"fbld"    ,"fild"    ,"fbstp"   ,"fistp"   ,
	0xc0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"ffreep"  ,"ffreep"  ,"ffreep"  ,"ffreep"  ,"ffreep"  ,"ffreep"  ,"ffreep"  ,"ffreep"  ,
	0xc8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,"fxch"    ,
	0xd0, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,
	0xd8, GKST0   ,GKST1   ,GKST2   ,GKST3   ,GKST4    ,GKST5   ,GKST6    ,GKST7    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,"fstp"    ,
	0xe0, GKAX    ,GKAX    ,GKAX    ,GK      ,GK       ,GK      ,GK       ,GK       ,"fstsw"   ,"fstdw"   ,"fstsg"   ,""        ,""        ,""        ,""        ,""        ,
	0xe8, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fucomip" ,"fucomip" ,"fucomip" ,"fucomip" ,"fucomip" ,"fucomip" ,"fucomip" ,"fucomip" ,
	0xf0, GKSTST0 ,GKSTST1 ,GKSTST2 ,GKSTST3 ,GKSTST4  ,GKSTST5 ,GKSTST6  ,GKSTST7  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,"fcomip"  ,
};
//--------------------------------------------------------------------------------------------------
	//3-byte opcodes: (>= SSSE3 only)
static const x86_t op_0f_38_xx[] =
{
	0x00, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  , "pshufb"   ,"phaddw"    ,"phaddd"   ,"phaddsw"  ,"pmaddubsw","phsubw"    ,"phsubd" ,"phsubsw"         ,
	0x08, GKPqQq  ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GK      ,GK      ,GK      ,GK      , "psignb"   ,"psignw"    ,"psignd"   ,"pmulhrsw" ,""         ,""          ,""       ,""                ,
	0x18, GK      ,GK      ,GK      ,GK      ,GKPqQq  ,GKPqQq  ,GKPqQq  ,GK      , ""         ,""          ,""         ,""         ,"pabsb"    ,"pabsw"     ,"pabsd"  ,""                ,
	0xf0, GKGvMv  ,GKMvGv  ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      , "movbe"    ,"movbe"     ,""         ,""         ,""         ,""          ,""       ,""                ,
};
static const x86_t op_66_0f_38_xx[] =
{
	0x00, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  , "pshufb"   ,"phaddw"    ,"phaddd"   ,"phaddsw"  ,"pmaddubsw","phsubw"    ,"phsubd" ,"phsubsw"         ,
	0x08, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GK      ,GK      ,GK      ,GK      , "psignb"   ,"psignw"    ,"psignd"   ,"pmulhrsw" ,""         ,""          ,""       ,""                ,
	0x10, GKVoWoV0,GK      ,GK      ,GK      ,GKVoWoV0,GKVoWoV0,GK      ,GKVoWo  , "pblendvb" ,""          ,""         ,""         ,"blendvps" ,"blendvpd"  ,""       ,"ptest"           ,
	0x18, GK      ,GK      ,GK      ,GK      ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GK      , ""         ,""          ,""         ,""         ,"pabsb"    ,"pabsw"     ,"pabsd"  ,""                ,
	0x20, GKVoWq  ,GKVoWd  ,GKVoWw  ,GKVoWq  ,GKVoWd  ,GKVoWq  ,GK      ,GK      , "pmovsxbw" ,"pmovsxbd"  ,"pmovsxbq" ,"pmovsxwd" ,"pmovsxwq" ,"pmovsxdq"  ,""       ,""                ,
	0x28, GKVoWo  ,GKVoWo  ,GKVoMo  ,GKVoWo  ,GK      ,GK      ,GK      ,GK      , "pmuldq"   ,"pcmpeqq"   ,"movntdqa" ,"packusdw" ,""         ,""          ,""       ,""                ,
	0x30, GKVoWq  ,GKVoWd  ,GKVoWw  ,GKVoWq  ,GKVoWd  ,GKVoWq  ,GK      ,GKVoWo  , "pmovzxbw" ,"pmovzxbd"  ,"pmovzxbq" ,"pmovzxwd" ,"pmovzxwq" ,"pmovzxdq"  ,""       ,"pcmpgtq"         ,
	0x38, GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  , "pminsb"   ,"pminsd"    ,"pminuw"   ,"pminud"   ,"pmaxsb"   ,"pmaxsd"    ,"pmaxuw" ,"pmaxud"          ,
	0x40, GKVoWo  ,GKVoWo  ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      , "pmulld"   ,"phminposuw",""         ,""         ,""         ,""          ,""       ,""                ,
	0xd8, GK      ,GK      ,GK      ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  ,GKVoWo  , ""         ,""          ,""         ,"aesimc"   ,"aesenc"   ,"aesenclast","aesdec" ,"aesdeclast"      ,
};
static const x86_t op_f2_0f_38_xx[] =
{
	0xf0, GKGdEb  ,GKGdEw  ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      , "crc32"    ,"crc32"     ,""         ,""         ,""         ,""          ,""       ,""                ,
};
static const x86_t op_0f_3a_xx[] =
{
	0x08, GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GKPqQqIb, ""         ,""          ,""         ,""         ,""         ,""          ,""       ,"palignr"         ,
};
static const x86_t op_66_0f_3a_xx[] =
{
	0x08, GKVoWoIb,GKVoWoIb,GKVoWdIb,GKVoWqIb,GKVoWoIb,GKVoWoIb,GKVoWoIb,GKVoWoIb, "roundps"  ,"roundpd"   ,"roundss"  ,"roundsd"  ,"blendps"  ,"blendpd"   ,"pblendw","palignr"         ,
	0x10, GK      ,GK      ,GK      ,GK      ,GKMbVoIb,GKMwVoIb,GKEdVoIb,GKMdVoIb, ""         ,""          ,""         ,""         ,"pextrb"   ,"pextrw"    ,"pextrd" ,"extractps"       ,
	0x20, GKVoMbIb,GKVoMdIb,GKVoEdIb,GK      ,GK      ,GK      ,GK      ,GK      , "pinsrb"   ,"insertps"  ,"pinsrd"   ,""         ,""         ,""          ,""       ,""                ,
	0x40, GKVoWoIb,GKVoWoIb,GKVoWoIb,GK      ,GKVoWoIb,GK      ,GK      ,GK      , "dpps"     ,"dppd"      ,"mpsadbw"  ,""         ,"pclmulqdq",""          ,""       ,""                ,
	0x60, GKVoWoIb,GKVoWoIb,GKVoWoIb,GK      ,GK      ,GK      ,GK      ,GK      , "pcmpestrm","pcmpestri" ,"pcmpistrm","pcmpistri",""         ,""          ,""       ,""                ,
	0xd8, GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GK      ,GKVoWoIb, ""         ,""          ,""         ,""         ,""         ,""          ,""       ,"aeskeygen-assist",
};
//--------------------------------------------------------------------------------------------------
static const x86_t op_group[] =
{
		//Groups: (--xxx--- of modrm byte)
	 1, GK    ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "add"        ,"or"        ,"adc"       ,"sbb"       ,"and"     ,"sub"     ,"xor"     ,"cmp"     ,
	 2, GK    ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "rol"        ,"ror"       ,"rcl"       ,"rcr"       ,"shl"     ,"shr"     ,"sal"     ,"sar"     ,
	 3, GKIbIv,GKIbIv,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "test"       ,"test"      ,"not"       ,"neg"       ,"mul"     ,"imul"    ,"div"     ,"idiv"    ,
	 4, GKEb  ,GKEb  ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "inc"        ,"dec"       ,""          ,""          ,""        ,""        ,""        ,""        ,
	 5, GKEv  ,GKEv  ,GKEv   ,GKMp   ,GKEv   ,GKMp   ,GKEv   ,GK     , "inc"        ,"dec"       ,"call"      ,"call"      ,"jmp"     ,"jmp"     ,"push"    ,""        ,
	 6, GKMw  ,GKMw  ,GKMw   ,GKMw   ,GKEw   ,GKEw   ,GKEv   ,GK     , "sldt"       ,"str"       ,"lldt"      ,"ltr"       ,"verr"    ,"verw"    ,"jmpe"    ,""        ,
	 7, GKMs  ,GKMs  ,GKMs   ,GKMs   ,GKMw   ,GK     ,GKMw   ,GKM    , "sgdt"       ,"sidt"      ,"lgdt"      ,"lidt"      ,"smsw"    ,""        ,"lmsw"    ,"swapgs"  ,
	 8, GK    ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , ""           ,""          ,""          ,""          ,"bt"      ,"bts"     ,"btr"     ,"btc"     ,
	 9, GK    ,GKMq  ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , ""           ,"cmpxchg8b" ,""          ,""          ,""        ,""        ,""        ,""        ,
	10, GKEv  ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "pop"        ,""          ,""          ,""          ,""        ,""        ,""        ,""        ,
	11, GK    ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "ud2"        ,"ud2"       ,"ud2"       ,"ud2"       ,"ud2"     ,"ud2"     ,"ud2"     ,"ud2"     ,
	12, GK    ,GK    ,GK     ,GK     ,GK     ,GK     ,GK     ,GK     , "mov"        ,""          ,""          ,""          ,""        ,""        ,""        ,""        ,
	13, GK    ,GK    ,GKPRqIb,GK     ,GKPRqIb,GK     ,GKPRqIb,GK     , ""           ,""          ,"psrlw"     ,""          ,"psraw"   ,""        ,"psllw"   ,""        ,
	14, GK    ,GK    ,GKPRqIb,GK     ,GKPRqIb,GK     ,GKPRqIb,GK     , ""           ,""          ,"psrld"     ,""          ,"psrad"   ,""        ,"pslld"   ,""        ,
	15, GK    ,GK    ,GKPRqIb,GKVRoIb,GK     ,GK     ,GKPRqIb,GKVRoIb, ""           ,""          ,"psrlq"     ,"psrldq"    ,""        ,""        ,"psllq"   ,"pslldq"  ,
	16, GKM512,GKM512,GKMd   ,GKMd   ,GKM    ,GKM    ,GKM    ,GKM    , "fxsave"     ,"fxrstor"   ,"ldmxcsr"   ,"stmxcsr"   ,"xsave"   ,"lfence"  ,"mfence"  ,"sfence"  ,
	17, GKM   ,GKM   ,GKM    ,GKM    ,GKEv   ,GKEv   ,GKEv   ,GKEv   , "prefetchnta","prefetcht0","prefetcht1","prefetcht2","hint_nop","hint_nop","hint_nop","hint_nop",
	18, GKEv  ,GKEv  ,GKEv   ,GKEv   ,GKEv   ,GKEv   ,GKEv   ,GKEv   , "hint_nop"   ,"hint_nop"  ,"hint_nop"  ,"hint_nop"  ,"hint_nop","hint_nop","hint_nop","hint_nop", //Fake group to reduce code space
};
//--------------------------------------------------------------------------------------------------

static const char *reg8nam [16] = {"al" ,"cl" ,"dl" ,"bl" ,"ah" ,"ch" ,"dh" ,"bh" ,"r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"}; //WARNING: must be pointer to support regnam
static const char *reg8namb[16] = {"al" ,"cl" ,"dl" ,"bl" ,"spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"}; //WARNING: must be pointer to support regnam
static const char *reg16nam[16] = {"ax" ,"cx" ,"dx" ,"bx" ,"sp" ,"bp" ,"si" ,"di" ,"r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"}; //WARNING: must be pointer to support regnam
static const char *reg32nam[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"}; //WARNING: must be pointer to support regnam
static const char *reg64nam[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8" ,"r9" ,"r10" ,"r11" ,"r12" ,"r13" ,"r14" ,"r15" }; //WARNING: must be pointer to support regnam
static const char *regmmnam[16] = {"mm0","mm1","mm2","mm3","mm4","mm5","mm6","mm7","mm8","mm9","mm10","mm11","mm12","mm13","mm14","mm15"}; //mm8-mm15 don't exist, but xmm8-xmm15 do!
static const char repref[3][7] = {"","repne ","rep "};
static const char cmpnam[8][6] = {"eq","lt","le","unord","neq","nlt","nle","ord"};
static const char segnam[8][3] = {"es","cs","ss","ds","fs","gs","",""};
static const char wordptrst[9][2] = {"","","","","d","","","","q"}; //2=()word ptr, 4=(d)word ptr, 8=(q)word ptr
enum
{
	GOT0F=(1<<0), GOT26=(1<<1), GOT2E=(1<<2), GOT36=(1<<3), GOT38=(1<<4), GOT3A=(1<<5), GOT3E=(1<<6),
	GOT64=(1<<7), GOT65=(1<<8), GOT66=(1<<9), GOT67=(1<<10), GOT9B=(1<<11),
	GOTFPU=(1<<12),
	GOTF0=(1<<13),GOTF2=(1<<14),GOTF3=(1<<15),
	GOTREX=(1<<16),GOTREXB=(1<<17),GOTREXX=(1<<18),GOTREXR=(1<<19),GOTREXW=(1<<20),
	BITS64=(1<<21),
};

static int getadst (unsigned char *ist, char *ost, int siz, int prefix)
{
	const char **regnam;
	int i, j, r0, r1;

	if ((unsigned char)ist[0] >= 0xc0)
	{
		i = (ist[0]&7) + (((prefix&GOTREXB)!=0)<<3);
		switch(siz)
		{
			case 1: if (prefix&GOTREX) sprintf(ost,"%s",reg8namb[i]);
										 else sprintf(ost,"%s",reg8nam[i]);
					  return(1);
			case 2: sprintf(ost,"%s",reg16nam[i]); return(1);
			case 4: sprintf(ost,"%s",reg32nam[i]); return(1);
			case 8: sprintf(ost,"%s",reg64nam[i]); return(1);
			case -8: sprintf(ost,"mm%d",i); return(1);
			case -16: sprintf(ost,"xmm%d",i); return(1);
		}
		return(0);
	}
	if (!(prefix&BITS64)) { regnam = reg32nam; }
	else { if (!(prefix&GOT67)) regnam = reg64nam; else regnam = reg32nam; }
	if ((((unsigned char)ist[0])&0xc7) == 5)
	{
		if (!(prefix&BITS64)) sprintf(ost,"[0x%x]",*(int *)&ist[1]);
							  else sprintf(ost,"[rip%+d]",*(int *)&ist[1]);
		return(5);
	}
	if ((ist[0]&7) == 4) //Has SIB
	{
		r0 = ( ist[1]    &7)+(((prefix&GOTREXB)!=0)<<3);
		r1 = ((ist[1]>>3)&7)+(((prefix&GOTREXX)!=0)<<3);
		j = 2;
		if ((r0 == 5) && ((((unsigned char)ist[0])&0xc0) == 0x00))
			  { strcpy(ost,"["); i = 1; }
		else { sprintf(ost,"[%s",regnam[r0]); i = strlen(ost); }
		if (r1 != 4)
		{
			if (i > 1) { ost[i++] = '+'; }
			sprintf(&ost[i],"%s",regnam[r1]); i += strlen(&ost[i]);
			if (((unsigned char)ist[1])&0xc0) { sprintf(&ost[i],"*%d",1<<(((unsigned char)ist[1])>>6)); i += 2; }
		}
	}
	else
		{ sprintf(ost,"[%s",regnam[(ist[0]&7)+(((prefix&GOTREXB)!=0)<<3)]); i = strlen(ost); j = 1; }
	switch(((unsigned char)ist[0])&0xc0)
	{
		case 0x00:
			if (((ist[1]&7) == 5) && ((((unsigned char)ist[0])&0xc7) == 0x04))
				{ sprintf(&ost[i],"%+d]",*(int *)&ist[j]); return(j+4); }
			strcpy(&ost[i],"]"); return(j);
		case 0x40: sprintf(&ost[i],"%+d]",(signed char)ist[j]); return(j+1);
		case 0x80: sprintf(&ost[i],"%+d]",*(int *)&ist[j]); return(j+4);
		default: break;
	}
	return(0);
}

	// ibuf(in ): machine language bytes to decode
	// obuf(out): generated asm instruction string (max length around 50 bytes; 64 should be safe)
	//      bits: 32:x86, 64:x86-64
	//returns: # bytes in ibuf for the instruction decoded (guaranteed to be > 0)
int kdisasm (unsigned char *ibuf, char *obuf, int bits)
{
	int i, j, x, y, prefix, regind, repind, xnum, opsize, opsizedef, mode, mode2, oi;
	const char **regnam, **reg8, **regnamdef, **regnammx;
	char *cptr;
	const x86_t *xptr;

			//1-byte opcodes:          xx
			//2-byte opcodes:       0f xx
			//                   f3 0f xx
			//             66 (REX) 0f xx
			//                   f2 0f xx
			//                   f0 0f xx
			//FPU:                  d8 xx
			//                      d9 xx
			//                      da xx
			//                      db xx
			//                      dc xx
			//                      dd xx
			//                      de xx
			//                      df xx
			//3-byte opcodes:    0f 38 xx
			//                66 0f 38 xx
			//                f2 0f 38 xx
			//                   0f 3a xx
			//                66 0f 3a xx

	opsize = 4; repind = 0; regnam = reg32nam; reg8 = reg8nam;
	if (bits < 64) { prefix =      0; regnamdef = reg32nam; regnammx = reg32nam; opsizedef = 4; }
				 else { prefix = BITS64; regnamdef = reg64nam; regnammx = reg32nam; opsizedef = 8; }
	for(i=0;i<4;i++) //x86 has up to 4 prefixes.
	{
		switch(ibuf[i])
		{
				//Prefix group 1:
			case 0xf0: prefix |= GOTF0; strcpy(obuf,"lock "); obuf += 5; break; //LOCK:
			case 0xf2: prefix |= GOTF2; repind = 1; break; //REPNE:
			case 0xf3: prefix |= GOTF3; repind = 2; break; //REP:

				//Prefix group 2:
			case 0x26: prefix |= GOT26; break; //ES:
			case 0x2e: prefix |= GOT2E; break; //CS:
			case 0x36: prefix |= GOT36; break; //SS:
			case 0x3e: prefix |= GOT3E; break; //DS:
			case 0x64: prefix |= GOT64; break; //FS:
			case 0x65: prefix |= GOT65; break; //GS:

				//Prefix group 3:
			case 0x66: prefix |= GOT66; opsize = 2; regnam = reg16nam; regnamdef = reg16nam; opsizedef = 2; break; //OPSIZE:

				//Prefix group 4:
			case 0x67: prefix |= GOT67; break; //ADSIZE:

			case 0x9b: prefix |= GOT9B; break; //WAIT: (prefix for fstsw)

			case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
			case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
				if (bits < 64) goto nomoreprefixes;
				prefix |= GOTREX; reg8 = reg8namb;
				if (ibuf[i]&1) prefix |= GOTREXB; //REX.B (R8-R15,XMM8-XMM15 in r/m; R8-R15 in SIB.base)
				if (ibuf[i]&2) prefix |= GOTREXX; //REX.X (R8-R15 in SIB.index)
				if (ibuf[i]&4) prefix |= GOTREXR; //REX.R (R8-R15,XMM8-XMM15 in reg)
				if (ibuf[i]&8) { prefix |= GOTREXW; opsize = 8; regnam = reg64nam; regnammx = reg64nam; } //REX.W (64-bit operand size)
				break;

			default: goto nomoreprefixes;
		}
	}
nomoreprefixes:;

	xptr = op_xx; xnum = sizeof(op_xx)/sizeof(x86_t); //defaults
	if (ibuf[i] == 0x0f) //Handle 2/3 byte opcode escapes/extensions
	{
		i++; prefix |= GOT0F;
		if (ibuf[i] == 0x38)
		{
			i++; prefix |= GOT38;
				  if (prefix&GOT66) { xptr = op_66_0f_38_xx; xnum = sizeof(op_66_0f_38_xx)/sizeof(x86_t); }
			else if (prefix&GOTF2) { xptr = op_f2_0f_38_xx; xnum = sizeof(op_f2_0f_38_xx)/sizeof(x86_t); }
			else                   { xptr =    op_0f_38_xx; xnum = sizeof(   op_0f_38_xx)/sizeof(x86_t); }
		}
		else if (ibuf[i] == 0x3a)
		{
			i++; prefix |= GOT3A;
				  if (prefix&GOT66) { xptr = op_66_0f_3a_xx; xnum = sizeof(op_66_0f_3a_xx)/sizeof(x86_t); }
			else                   { xptr =    op_0f_3a_xx; xnum = sizeof(   op_0f_3a_xx)/sizeof(x86_t); }
		}
		else
		{
				  if (prefix&GOTF3) { xptr =    op_f3_0f_xx; xnum = sizeof(   op_f3_0f_xx)/sizeof(x86_t); }
			else if (prefix&GOT66) { xptr =    op_66_0f_xx; xnum = sizeof(   op_66_0f_xx)/sizeof(x86_t); }
			else if (prefix&GOTF2) { xptr =    op_f2_0f_xx; xnum = sizeof(   op_f2_0f_xx)/sizeof(x86_t); }
			else if ((prefix&GOTF0) && (ibuf[i-1] == 0xf0)) //0xf0 must immediately precede 0x0f to be 2-byte inst (otherwise lock prefix)
										  { xptr =    op_f0_0f_xx; xnum = sizeof(   op_f0_0f_xx)/sizeof(x86_t); }
			else                   { xptr =       op_0f_xx; xnum = sizeof(      op_0f_xx)/sizeof(x86_t); }
		}
	}
	else if ((ibuf[i]&0xf8) == 0xd8) //FPU
	{
		switch(ibuf[i]&7)
		{
			case 0: xptr = op_d8_xx; xnum = sizeof(op_d8_xx)/sizeof(x86_t); break;
			case 1: xptr = op_d9_xx; xnum = sizeof(op_d9_xx)/sizeof(x86_t); break;
			case 2: xptr = op_da_xx; xnum = sizeof(op_da_xx)/sizeof(x86_t); break;
			case 3: xptr = op_db_xx; xnum = sizeof(op_db_xx)/sizeof(x86_t); break;
			case 4: xptr = op_dc_xx; xnum = sizeof(op_dc_xx)/sizeof(x86_t); break;
			case 5: xptr = op_dd_xx; xnum = sizeof(op_dd_xx)/sizeof(x86_t); break;
			case 6: xptr = op_de_xx; xnum = sizeof(op_de_xx)/sizeof(x86_t); break;
			case 7: xptr = op_df_xx; xnum = sizeof(op_df_xx)/sizeof(x86_t); break;
		}
		i++; prefix |= GOTFPU;
	}

	for(x=xnum-1;x>=0;x--)
	{ //FUK:optimize using binsrch
		y = ibuf[i]-xptr[x].op; if (y < 0) continue;
		y &= 7;
		xptr = &xptr[x]; i++;

		oi = i;

		if ((prefix&GOTFPU) && (ibuf[i-1] < 0xc0)) y = (ibuf[i-1]>>3)&7;
		mode = xptr->parm[y];
		if ((xptr->st[y][0] == 'g') && (xptr->st[y][1] == 'r')) //group remapping
		{
			cptr = &xptr->st[y][2]; y = cptr[0]-'0'; if (cptr[1]) y = y*10+cptr[1]-'0';
			xptr = &op_group[y-1];
			y = (ibuf[i]>>3)&7; mode2 = xptr->parm[y];
		} else mode2 = 0;
		cptr = &obuf[sprintf(obuf,"%s ",xptr->st[y])];
doagain:;
		regind = ((ibuf[i]>>3)&7)+(((prefix&GOTREXR)!=0)<<3);
		switch(mode)
		{
			case GK:
				if ((opsize == 4) && (!memcmp(obuf,"cwd"  ,3))) strcpy(obuf,"cdq"); //Evil hack!
				if ((opsize == 4) && (!memcmp(obuf,"cbw"  ,3))) strcpy(obuf,"cwde"); //Evil hack!
				if ((opsize == 4) && (!memcmp(obuf,"pusha",5))) strcpy(obuf,"pushad"); //Evil hack!
				if ((opsize == 4) && (!memcmp(obuf,"popa" ,4))) strcpy(obuf,"popad"); //Evil hack!
				if ((opsize == 8) && (!memcmp(obuf,"cwd"  ,3))) strcpy(obuf,"cqo"); //Evil hack!
				if ((opsize == 8) && (!memcmp(obuf,"cbw"  ,3))) strcpy(obuf,"cdqe"); //Evil hack!
				if ((ibuf[0] == 0xf3) && (ibuf[1] == 0x90)) strcpy(obuf,"pause"); //Evil hack!
				if ((!(prefix&GOT9B)) && (!memcmp(obuf,"finit",5))) { strcpy(obuf,"fninit " ); cptr++; } //Evil hack!
				if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fclex",5))) { strcpy(obuf,"fnclex " ); cptr++; } //Evil hack!
				break;
			case GKeAX: case GKeCX: case GKeDX: case GKeBX: case GKeSP: case GKeBP: case GKeSI: case GKeDI: sprintf(cptr,"%s",regnam  [xptr->parm[y]-GKeAX+(((prefix&GOTREXB)!=0)<<3)]); break;
			case GKEAX: case GKECX: case GKEDX: case GKEBX: case GKESP: case GKEBP: case GKESI: case GKEDI: sprintf(cptr,"%s",regnam[xptr->parm[y]-GKEAX+(((prefix&GOTREXB)!=0)<<3)]); break;
			case GKrAX: case GKrCX: case GKrDX: case GKrBX: case GKrSP: case GKrBP: case GKrSI: case GKrDI: sprintf(cptr,"%s",regnamdef[xptr->parm[y]-GKrAX+(((prefix&GOTREXB)!=0)<<3)]); break;
			case GKrCXrAX: case GKrDXrAX: case GKrBXrAX: case GKrSPrAX: case GKrBPrAX: case GKrSIrAX: case GKrDIrAX:
				sprintf(cptr,"%s, %s",regnam[(xptr->parm[y]-GKrCXrAX+1)+(((prefix&GOTREXB)!=0)<<3)],regnam[0]);
				break;

			case GKeAXIb: sprintf(cptr,"%sax, %d",(prefix&GOT66)?"":"e",(signed)ibuf[i]); i++; break;
			case GKIbeAX: sprintf(cptr,"%d, %sax",(signed)ibuf[i],(prefix&GOT66)?"":"e"); i++; break;
			case GKIbAL:  sprintf(cptr,"%d, al",(signed)ibuf[i]); i++; break;
			case GKALDX:  sprintf(cptr,"al, dx"); break;
			case GKDXAL:  sprintf(cptr,"dx, al"); break;
			case GKeAXDX: sprintf(cptr,"%sax, dx",(prefix&GOT66)?"":"e"); break;
			case GKDXeAX: sprintf(cptr,"dx, %sax",(prefix&GOT66)?"":"e"); break;

			case GKrAXIv: case GKrCXIv: case GKrDXIv: case GKrBXIv: case GKrSPIv: case GKrBPIv: case GKrSIIv: case GKrDIIv:
					  if (opsize == 2) sprintf(cptr,"%s, %d",regnam[xptr->parm[y]-GKrAXIv+(((prefix&GOTREXB)!=0)<<3)],*(short *)&ibuf[i]);
				else if (opsize == 4) sprintf(cptr,"%s, %d",regnam[xptr->parm[y]-GKrAXIv+(((prefix&GOTREXB)!=0)<<3)],*(int *)&ibuf[i]);
				else          /*== 8*/sprintf(cptr,"%s, %I64d",reg64nam[xptr->parm[y]-GKrAXIv+(((prefix&GOTREXB)!=0)<<3)],*(__int64 *)&ibuf[i]);
				i += opsize; break;
			case GKALIb: case GKCLIb: case GKDLIb: case GKBLIb: case GKAHIb: case GKCHIb: case GKDHIb: case GKBHIb:
				sprintf(cptr,"%s, %d",reg8[xptr->parm[y]-GKALIb+(((prefix&GOTREXB)!=0)<<3)],(signed)ibuf[i]); i++; break;

			case GKEb:   if (ibuf[i] < 192) cptr += sprintf(cptr,"byte ptr "); i += getadst(&ibuf[i],cptr,1,prefix); break;
			case GKEb1:  if (ibuf[i] < 192) cptr += sprintf(cptr,"byte ptr "); i += getadst(&ibuf[i],cptr,1,prefix); sprintf(&cptr[strlen(cptr)],", 1"); break;
			case GKEbCL: if (ibuf[i] < 192) cptr += sprintf(cptr,"byte ptr "); i += getadst(&ibuf[i],cptr,1,prefix); sprintf(&cptr[strlen(cptr)],", cl"); break;
			case GKEw:   if (ibuf[i] < 192) cptr += sprintf(cptr,"word ptr "); i += getadst(&ibuf[i],cptr,2,prefix); break;
			case GKEv:   if (ibuf[i] < 192) cptr += sprintf(cptr,"%sword ptr ",wordptrst[opsize]); i += getadst(&ibuf[i],cptr,opsize,prefix); break;
			case GKEv1:  if (ibuf[i] < 192) cptr += sprintf(cptr,"%sword ptr ",wordptrst[opsize]); i += getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", 1"); break;
			case GKEvCL: if (ibuf[i] < 192) cptr += sprintf(cptr,"%sword ptr ",wordptrst[opsize]); i += getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", cl"); break;
			case GKEvIz: if (ibuf[i] < 192) cptr += sprintf(cptr,"%sword ptr ",wordptrst[opsize]); i += getadst(&ibuf[i],cptr,opsize,prefix);
							 if (opsize == 2) { sprintf(&cptr[strlen(cptr)],", %d",*(short *)&ibuf[i]); }
											 else { sprintf(&cptr[strlen(cptr)],", %d",*(int *)&ibuf[i]); }
							 i += min(opsize,4); break;
			case GKSTRB: sprintf(obuf,"%s%sb",repref[repind],xptr->st[y]); break;
			case GKSTRV: sprintf(obuf,"%s%s%s",repref[repind],xptr->st[y],prefix&GOT66?"w":wordptrst[opsize]); break;

			case GKIbIv: if (!(ibuf[oi-1]&1)) { sprintf(cptr,"%d",(signed)ibuf[i]); i++; }
							else if (opsize == 2) { sprintf(cptr,"%d",*(short *)&ibuf[i]); i += 2; }
							else                  { sprintf(cptr,"%d",*(int *)&ibuf[i]); i += 4; }
							break;
			case GKALOb: sprintf(cptr,"al, [0x%x]",*(int *)&ibuf[i]); i += 4; break;
			case GKObAL: sprintf(cptr,"[0x%x], al",*(int *)&ibuf[i]); i += 4; break;
			case GKOvrAX: sprintf(cptr,"[0x%x], %s",*(int *)&ibuf[i],regnam[0]); i += 4; break;
			case GKrAXOv: sprintf(cptr,"%s, [0x%x]",regnam[0],*(int *)&ibuf[i]); i += 4; break;
			case GKrAXIz: cptr += sprintf(cptr,"%s, ",regnam[0]);
							  if (opsize == 2) sprintf(cptr,"%d",*(short *)&ibuf[i]);
											  else sprintf(cptr,"%d",*(int *)&ibuf[i]);
							  i += min(opsize,4); break;

			case GKIb: sprintf(cptr,"%d",(signed)ibuf[i]); i++; break;
			case GKIz: if (opsize == 2) sprintf(cptr,"%d",*(short *)&ibuf[i]);
										  else sprintf(cptr,"%d",*(int *)&ibuf[i]);
						  i += min(opsize,4);
						  break;

			case GKAp: break;
			case GKFv: if ((opsize == 4) && (bits == 64)) opsize = 8; strcpy(&cptr[-1],wordptrst[opsize]); break; //FUKFUK
			case GKGvM: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); break;
			case GKJb: sprintf(cptr,"%+d",(signed char)ibuf[i]); i++; break;
			case GKJz: sprintf(cptr,"%+d",*(int *)&ibuf[i]); i += 4; break;
			case GKnearIw: sprintf(cptr,"%d",*(short *)&ibuf[i]); i += 2; break;
			case GKnear: break;
			case GKfarIw: sprintf(cptr,"far %d",*(short *)&ibuf[i]); i += 2; break;
			case GKfar: sprintf(cptr,"far"); break;
			case GKCS: strcpy(cptr,"cs"); break;
			case GKDS: strcpy(cptr,"ds"); break;
			case GKES: strcpy(cptr,"es"); break;
			case GKGS: strcpy(cptr,"gs"); break;
			case GKSS: strcpy(cptr,"ss"); break;

			case GKMs: i += getadst(&ibuf[i],cptr,2,prefix); break;
			case GKMq: case GKMd: case GKM: case GKM512:
				if ((prefix&GOTREXW) && (!memcmp(obuf,"cmpxchg8b",9))) { strcpy(&obuf[7],"16b "); cptr++; } //Evil hack!
				i += getadst(&ibuf[i],cptr,opsize,prefix);
				break;
			case GKMw:     if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fstsw",5)))  { strcpy(obuf,"fnstsw " ); cptr++; } //no break intentional
								if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fstcw",5)))  { strcpy(obuf,"fnstcw " ); cptr++; } //no break intentional
			case GKM14M28: if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fstenv",6))) { strcpy(obuf,"fnstenv "); cptr++; } //no break intentional
			case GKM94M108:if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fsave",5)))  { strcpy(obuf,"fnsave " ); cptr++; } //no break intentional
			case GKMp:      sprintf(cptr,"[0x%x]",*(int *)&ibuf[i]); i += 4; break;

			case GKGdVRo: if (regnam == reg16nam) regnam = reg32nam;/*HACK!*/ cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKGdPRq: if (regnam == reg16nam) regnam = reg32nam;/*HACK!*/ cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKVoPRq: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKVoVRo: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKPqVRq: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKPqPRq: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKPRqIb: if (prefix&GOT66) *cptr++ = 'x'; i += getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); i++; break;
			case GKVRoIb: i += getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); i++; break;
			case GKGvEvIz: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %d",*(int *)&ibuf[i]); i += 4; break;
			case GKGdPRqIb: cptr += sprintf(cptr,"%s, ",regnammx[regind]); j = getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i+j]); i += j+1; break;
			case GKGdVRoIb: cptr += sprintf(cptr,"%s, ",regnammx[regind]); j = getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i+j]); i += j+1; break;
			case GKVoWoV0: break;
			case GKVoVoIbIb: break;

			case GKCdRd: cptr += sprintf(cptr,"cr%d, %s",regind,regnamdef[ibuf[i]&7]); i++; break;
			case GKRdCd: cptr += sprintf(cptr,"%s, cr%d",regnamdef[ibuf[i]&7],regind); i++; break;
			case GKDdRd: cptr += sprintf(cptr,"dr%d, %s",regind,regnamdef[ibuf[i]&7]); i++; break;
			case GKRdDd: cptr += sprintf(cptr,"%s, dr%d",regnamdef[ibuf[i]&7],regind); i++; break;
			case GKRdCR8D: break;
			case GKCR8DRd: break;

			case GKEdGd: break;
			case GKGdEd: break;
			case GKEdPd: j = getadst(&ibuf[i],cptr,max(opsize,4),prefix); sprintf(&cptr[strlen(cptr)],", %s",regmmnam[regind]);  i += j; break;
			case GKEdVd: j = getadst(&ibuf[i],cptr,max(opsize,4),prefix); sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKEbGb: j = getadst(&ibuf[i],cptr,            1,prefix); sprintf(&cptr[strlen(cptr)],", %s",reg8[regind]);      i += j; break;
			case GKEvGv: j = getadst(&ibuf[i],cptr,       opsize,prefix); sprintf(&cptr[strlen(cptr)],", %s",regnam[regind]);    i += j; break;
			case GKEbIb: if (ibuf[i] < 192) cptr += sprintf(cptr,"byte ptr "); i += getadst(&ibuf[i],cptr,1,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed char)ibuf[i]); i++; break;
			case GKEvIb: if (ibuf[i] < 192) cptr += sprintf(cptr,"%sword ptr ",wordptrst[opsizedef]); i += getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed char)ibuf[i]); i++; break;
			case GKEwGw:
				if (bits < 64) { cptr += sprintf(cptr,"%s, ",reg16nam[regind]); i += getadst(&ibuf[i],cptr,2,prefix); break; }
				strcpy(&cptr[-5],"movsxd "); cptr += 2;
				cptr += sprintf(cptr,"%s, ",regnam[regind]); if (ibuf[i] < 192) cptr += sprintf(cptr,"dword ptr "); i += getadst(&ibuf[i],cptr,opsize,prefix);
				break;
			case GKGbEb: cptr += sprintf(cptr,"%s, ",reg8[regind]);     i += getadst(&ibuf[i],cptr,1,prefix); break;
			case GKGdEb: break;
			case GKGdEw: break;
			case GKGdWd: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKGdWq: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKGvEv: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); break;
			case GKGvEb: cptr += sprintf(cptr,"%s, ",regnam[regind]); if (ibuf[i] < 192) cptr += sprintf(cptr,"byte ptr "); i += getadst(&ibuf[i],cptr,1,prefix); break;
			case GKGvEw: cptr += sprintf(cptr,"%s, ",regnam[regind]); if (ibuf[i] < 192) cptr += sprintf(cptr,"word ptr "); i += getadst(&ibuf[i],cptr,2,prefix); break;
			case GKGvMa: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,2,prefix); break;
			case GKGvMv: break;
			case GKGzMp: cptr += sprintf(cptr,"%s, ",regnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); break;
			case GKIwIb: cptr += sprintf(cptr,"%d, %d",*(short *)&ibuf[i],(signed)ibuf[i+2]); i += 3; break;
			case GKMdGd: j = getadst(&ibuf[i],cptr,opsize,prefix); cptr += sprintf(&cptr[strlen(cptr)],", %s",regnam[regind]); i += j; break;
			case GKMdVd: break;
			case GKMoVo: j = getadst(&ibuf[i],cptr,-16,prefix); cptr += sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKMqPq: j = getadst(&ibuf[i],cptr,-8,prefix); cptr += sprintf(&cptr[strlen(cptr)],", %s",regmmnam[regind]); i += j; break;
			case GKMqVq: cptr += sprintf(cptr,"qword ptr "); j = getadst(&ibuf[i],cptr,-16,prefix); cptr += sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKMvGv: break;
			case GKMwSw: j = getadst(&ibuf[i],cptr,opsize,prefix); cptr += sprintf(&cptr[strlen(cptr)],", %s",segnam[(ibuf[((prefix&GOT66)!=0)+2]>>3)&7]); i += j; break;
			case GKSwMw: cptr += sprintf(cptr,"%s, ",segnam[(ibuf[((prefix&GOT66)!=0)+2]>>3)&7]); i += getadst(&ibuf[i],&cptr[strlen(cptr)],opsize,prefix); break;
			case GKRdTd: break;
			case GKTdRd: break;
			case GKPqEd: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); break;
			case GKPqQd: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKPqQq: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKPqWo: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKPqWq: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKQqPq: j = getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", %s",regmmnam[regind]); i += j; break;
			case GKVdEd: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVdWd: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVdWq: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,max(opsize,4),prefix); break;
			case GKVoEd: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,max(opsize,4),prefix); break;
			case GKVoMo: break;
			case GKVoWo: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVoMq: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr, -8,prefix); break;
			case GKVoVo: break;
			case GKVoWd: break;
			case GKVoWq: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVoWw: break;
			case GKVqEd: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVqMq:      if ((!stricmp(xptr->st[y],"movlps")) && (ibuf[i] >= 192)) { strcpy(&cptr[-4],"hlps "); cptr++; } //Evil hack!
							 else if ((!stricmp(xptr->st[y],"movhps")) && (ibuf[i] >= 192)) { strcpy(&cptr[-4],"lhps "); cptr++; } //Evil hack!
							 cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); break;
			case GKVqWd: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKVqWq: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); break;
			case GKWdVd: j = getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKWoVo: j = getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKWqVq: j = getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", x%s",regmmnam[regind]); i += j; break;
			case GKEdVoIb: break;
			case GKEvGvIb: j = getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %s, %d",regnam[regind],(signed)ibuf[i+j]); i += j+1; break;
			case GKEvGvCL: j = getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %s, cl",regnam[regind]); i += j; break;
			case GKGvEvIb: cptr += sprintf(cptr,"%s, ",regnam[regind]); j = getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i+j]); i += j+1; break;
			case GKMbVoIb: break;
			case GKMdVoIb: break;
			case GKMwVoIb: break;
			case GKPqMwIb: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,opsize,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); i++; break;
			case GKPqQqIb: cptr += sprintf(cptr,"%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-8,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); i++; break;
			case GKVoEdIb: break;
			case GKVoIbIb: break;
			case GKVoMbIb: break;
			case GKVoMdIb: break;
			case GKVoMwIb: cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,max(opsize,4),prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); i++; break;
			case GKVoWdIb: break;
			case GKVoWqIb: break;

			case GKVdWdIb: j = getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[-5],"%sss x%s, ",cmpnam[ibuf[i+j]&7],regmmnam[regind]); i += getadst(&ibuf[i],&cptr[strlen(cptr)],-16,prefix); i++; break;
			case GKVqWqIb: j = getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[-5],"%ssd x%s, ",cmpnam[ibuf[i+j]&7],regmmnam[regind]); i += getadst(&ibuf[i],&cptr[strlen(cptr)],-16,prefix); i++; break;
			case GKVoWoIb:
				if (!memcmp(obuf,"cmpcc",5))
					  { j = getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[-5],"%sp%c x%s, ",cmpnam[ibuf[i+j]&7],cptr[-2],regmmnam[regind]); i += getadst(&ibuf[i],&cptr[strlen(cptr)],-16,prefix); }
				else { cptr += sprintf(cptr,"x%s, ",regmmnam[regind]); i += getadst(&ibuf[i],cptr,-16,prefix); sprintf(&cptr[strlen(cptr)],", %d",(signed)ibuf[i]); }
				i++; break;

			case GKint16:                cptr += sprintf(cptr, "word ptr "); i += getadst(&ibuf[i-1],cptr,opsize,prefix)-1; break;
			case GKint32: case GKfloat:  cptr += sprintf(cptr,"dword ptr "); i += getadst(&ibuf[i-1],cptr,opsize,prefix)-1; break;
			case GKint64: case GKdouble: cptr += sprintf(cptr,"qword ptr "); i += getadst(&ibuf[i-1],cptr,opsize,prefix)-1; break;
			case GKpBCD:                 cptr += sprintf(cptr,"pkbcd ptr "); i += getadst(&ibuf[i-1],cptr,opsize,prefix)-1; break;
			case GKtbyte:                cptr += sprintf(cptr,"tbyte ptr "); i += getadst(&ibuf[i-1],cptr,opsize,prefix)-1; break;
			case GKAX:
				if ((!(prefix&GOT9B)) && (!memcmp(obuf,"fstsw",5))) { strcpy(obuf,"fnstsw "); cptr++; } //no break intentional
				strcpy(cptr,"ax"); break;
			case GKSTi:   cptr += sprintf(cptr,"st(%d)"    ,ibuf[i-1]&7); break;
			case GKSTSTi: cptr += sprintf(cptr,"st, st(%d)",ibuf[i-1]&7); break;
			case GKSTiST: cptr += sprintf(cptr,"st(%d), st",ibuf[i-1]&7); break;
			case GKST0:   case GKST1:   case GKST2:   case GKST3:   case GKST4:   case GKST5:   case GKST6:   case GKST7:   sprintf(cptr,"st(%d)"    ,xptr->parm[y]-GKST0  ); break;
			case GKSTST0: case GKSTST1: case GKSTST2: case GKSTST3: case GKSTST4: case GKSTST5: case GKSTST6: case GKSTST7: sprintf(cptr,"st, st(%d)",xptr->parm[y]-GKSTST0); break;
			case GKST0ST: case GKST1ST: case GKST2ST: case GKST3ST: case GKST4ST: case GKST5ST: case GKST6ST: case GKST7ST: sprintf(cptr,"st(%d), st",xptr->parm[y]-GKST0ST); break;
		}
		if (mode2) { if (mode) sprintf(&cptr[strlen(cptr)],", "); cptr = &cptr[strlen(cptr)]; mode = mode2; mode2 = 0; goto doagain; }
		break;
	}
	if (x < 0) { strcpy(obuf,"Decoding error!"); return(1); }
	return(i);
}

//------------------------------------------ KDISASM ends ------------------------------------------

#ifdef TESTKDISASM
#pragma warning(disable:4730) //Advice for VC6:STFU

#include <conio.h>

#ifndef _WIN64
float posone = 1.0;
__declspec (naked) void __stdcall myfunc ()
{
	_asm
	{
		test dword ptr [esi+ecx*2-2147483648], -2147483648 ;strlen=50 bytes
		imul edx, [ebx+esi*8-2147483648], -2147483648 ;strlen=45 bytes
		punpcklqdq xmm0, [ecx+edx*8-2147483648] ;strlen=39 bytes
		pinsrw xmm2, word ptr [ecx+edx*8-2147483648], 7 ;strlen=38 bytes
		movdq2q mm0, xmm1
		push [ebp+8]
		movdqa xmm3, xmm5
		ldmxcsr [ecx+eax*8+35]
		fld tbyte ptr [ecx]
		fyl2xp1
		pop gs
		out dx, al
		mov eax, [esi*4]
		l1:_emit 0xd6
		rcr bx, cl
		jg short l1
		shufps xmm3, [esi+56], 6
		setnp ch
		test bl, 16
		mul bh
		emms
		rep stosw
	}
	_asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop //DO NOT REMOVE THIS!
	_asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop //DO NOT REMOVE THIS!
	_asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop _asm nop //DO NOT REMOVE THIS!
}

#else
void myfunc (void);
#endif

void main (int argc, char **argv)
{
	int i, lincnt, leng;
	unsigned char *cptr;
	char buf[256];

	cptr = (unsigned char *)myfunc;

	lincnt = 0;
	while (memcmp(cptr,"\x90\x90\x90\x90\x90\x90\x90\x90",8))
	{
		leng = kdisasm(cptr,buf,sizeof(void*)<<3); if (!leng) break;
		printf("%Ix:",cptr); //%Ix:right-justified, %p:full address (8 or 16 hex digits)

		for(i=0;i<leng;i++) printf("%02x ",cptr[i]);
		if (i < 9) printf("%*c",27-i*3,32);

		printf("%s\n",buf);
		cptr += leng;

		lincnt++; if (lincnt >= 23) { lincnt = 0; if (getch() == 27) exit(0); }
	}

	getch();
}
#endif

#if 0
!endif
#endif
