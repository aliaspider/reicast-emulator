#include <math.h>
#include <algorithm>
#include "gles.h"
#include "../rend.h"
#include "rend/TexCache.h"
#include "../../libretro/libretro.h"

extern struct retro_hw_render_callback hw_render;
extern bool enable_rtt;
extern bool is_dupe;

struct modvol_shader_type
{
   GLuint program;

   GLuint scale,depth_scale;
   GLuint sp_ShaderColor;
};

struct PipelineShader
{
	GLuint program;

	GLuint scale,depth_scale;
	GLuint pp_ClipTest,cp_AlphaTestValue;
	GLuint sp_FOG_COL_RAM,sp_FOG_COL_VERT,sp_FOG_DENSITY,sp_LOG_FOG_COEFS;

	u32 cp_AlphaTest; s32 pp_ClipTestMode;
	u32 pp_Texture, pp_UseAlpha, pp_IgnoreTexA, pp_ShadInstr, pp_Offset, pp_FogCtrl;
};

#define SGL_CAP_MAX 8

struct vbo_type
{
   GLuint geometry,modvols,idxs,idxs2;
#ifdef CORE
   GLuint vao;
#endif
};

gl_cached_state gl_state;
vbo_type vbo;
modvol_shader_type modvol_shader;
PipelineShader program_table[768*2];
static float fog_coefs[]={0,0};

/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target
*/

//Uncomment this to disable the stencil work around
//Seems like there's a bug either on the wrapper, or nvogl making
//stencil not work properly (requiring some double calls to get proper results)
//#define NO_STENCIL_WORKAROUND


const static u32 CullMode[]= 
{

	GL_NONE, //0    No culling          No culling
	GL_NONE, //1    Cull if Small       Cull if ( |det| < fpu_cull_val )

	GL_FRONT, //2   Cull if Negative    Cull if ( |det| < 0 ) or ( |det| < fpu_cull_val )
	GL_BACK,  //3   Cull if Positive    Cull if ( |det| > 0 ) or ( |det| < fpu_cull_val )
};
const static u32 Zfunction[]=
{
	GL_NEVER,      //GL_NEVER,              //0 Never
	GL_LESS,        //GL_LESS/*EQUAL*/,     //1 Less
	GL_EQUAL,       //GL_EQUAL,             //2 Equal
	GL_LEQUAL,      //GL_LEQUAL,            //3 Less Or Equal
	GL_GREATER,     //GL_GREATER/*EQUAL*/,  //4 Greater
	GL_NOTEQUAL,    //GL_NOTEQUAL,          //5 Not Equal
	GL_GEQUAL,      //GL_GEQUAL,            //6 Greater Or Equal
	GL_ALWAYS,      //GL_ALWAYS,            //7 Always
};

/*
0   Zero                  (0, 0, 0, 0)
1   One                   (1, 1, 1, 1)
2   Dither Color          (OR, OG, OB, OA) 
3   Inverse Dither Color  (1-OR, 1-OG, 1-OB, 1-OA)
4   SRC Alpha             (SA, SA, SA, SA)
5   Inverse SRC Alpha     (1-SA, 1-SA, 1-SA, 1-SA)
6   DST Alpha             (DA, DA, DA, DA)
7   Inverse DST Alpha     (1-DA, 1-DA, 1-DA, 1-DA)
*/

const static u32 DstBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const static u32 SrcBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

Vertex* vtx_sort_base;

struct IndexTrig
{
	u16 id[3];
	u16 pid;
	f32 z;
};


struct SortTrigDrawParam
{
	PolyParam* ppid;
	u16 first;
	u16 count;
};

static vector<SortTrigDrawParam>	pidx_sort;
PipelineShader* CurrentShader;
u32 gcflip;

static struct
{
	TSP tsp;
	//TCW tcw;
	PCW pcw;
	ISP_TSP isp;
	u32 clipmode;
	//u32 texture_enabled;
	u32 stencil_modvol_on;
	u32 program;
	GLuint texture;

	void Reset(const PolyParam* gp)
	{
		program=~0;
		texture=~0;
		tsp.full = ~gp->tsp.full;
		//tcw.full = ~gp->tcw.full;
		pcw.full = ~gp->pcw.full;
		isp.full = ~gp->isp.full;
		clipmode=0xFFFFFFFF;
//		texture_enabled=~gp->pcw.Texture;
		stencil_modvol_on=false;
	}
} cache;

s32 SetTileClip(u32 val, bool set)
{
	float csx=0,csy=0,cex=0,cey=0;
	u32 clipmode=val>>28;
	s32 clip_mode;
	if (clipmode<2)
	{
		clip_mode=0;    //always passes
	}
	else if (clipmode&1)
		clip_mode=-1;   //render stuff inside the region
	else
		clip_mode=1;    //render stuff outside the region

	csx=(float)(val&63);
	cex=(float)((val>>6)&63);
	csy=(float)((val>>12)&31);
	cey=(float)((val>>17)&31);
	csx=csx*32;
	cex=cex*32 +32;
	csy=csy*32;
	cey=cey*32 +32;

	if (csx==0 && csy==0 && cex==640 && cey==480)
		return 0;
	
	if (set)
		glUniform4f(CurrentShader->pp_ClipTest,-csx,-csy,-cex,-cey);		

	return clip_mode;
}

static void SetCull(u32 CulliMode)
{
	if (CullMode[CulliMode] == GL_NONE)
   {
		glDisable(GL_CULL_FACE);
      gl_state.cap_state[4] = 0;
   }
	else
	{
		glEnable(GL_CULL_FACE);
      gl_state.cap_state[4] = 1;
      gl_state.cullmode     = CullMode[CulliMode];
		glCullFace(gl_state.cullmode); //GL_FRONT/GL_BACK, ...
	}
}

template <u32 Type, bool SortingEnabled>
static __forceinline void SetGPState(const PolyParam* gp,u32 cflip=0)
{
   //force everything to be shadowed
   const u32 stencil=0x80;

   //has to preserve cache_tsp/cache_isp
   //can freely use cache_tcw
   CurrentShader=&program_table[GetProgramID(Type==ListType_Punch_Through
         ? 1 : 0,
         SetTileClip(gp->tileclip,false)+1,
         gp->pcw.Texture,
         gp->tsp.UseAlpha,
         gp->tsp.IgnoreTexA,
         gp->tsp.ShadInstr,
         gp->pcw.Offset,
         gp->tsp.FogCtrl)];

   if (CurrentShader->program == -1)
      CompilePipelineShader(CurrentShader);
   if (CurrentShader->program != cache.program)
   {
      cache.program    = CurrentShader->program;
      gl_state.program = CurrentShader->program;
      glUseProgram(gl_state.program);
      SetTileClip(gp->tileclip,true);
   }

#ifdef NO_STENCIL_WORKAROUND
   //This for some reason doesn't work properly
   //So, shadow bit emulation is disabled.
   //This bit normally control which pixels are affected
   //by modvols
   if (gp->pcw.Shadow==0)
      stencil = 0x0;
#endif

   if (cache.stencil_modvol_on!=stencil)
   {
      cache.stencil_modvol_on=stencil;

      glStencilFunc(GL_ALWAYS,stencil,stencil);
   }

   if (gp->texid != cache.texture)
   {
      cache.texture=gp->texid;
      if (gp->texid != -1)
         glBindTexture(GL_TEXTURE_2D, gp->texid);
   }

   if (gp->tsp.full!=cache.tsp.full)
   {
      cache.tsp=gp->tsp;

      if (Type==ListType_Translucent)
      {
         gl_state.blendfunc.sfactor = SrcBlendGL[gp->tsp.SrcInstr];
         gl_state.blendfunc.dfactor = DstBlendGL[gp->tsp.DstInstr];
         glBlendFunc(gl_state.blendfunc.sfactor, gl_state.blendfunc.dfactor);

#ifdef WEIRD_SLOWNESS
         //SGX seems to be super slow with discard enabled blended pixels
         //can't cache this -- due to opengl shader api
         bool clip_alpha_on_zero=gp->tsp.SrcInstr==4 && (gp->tsp.DstInstr==1 || gp->tsp.DstInstr==5);
         glUniform1f(CurrentShader->cp_AlphaTestValue,clip_alpha_on_zero?(1/255.f):(-2.f));
#endif
      }
   }

   //set cull mode !
   //cflip is required when exploding triangles for triangle sorting
   //gcflip is global clip flip, needed for when rendering to texture due to mirrored Y direction
   SetCull(gp->isp.CullMode ^ cflip ^ gcflip);


   if (gp->isp.full!= cache.isp.full)
   {
      cache.isp.full=gp->isp.full;

      //set Z mode, only if required
      if (!(Type==ListType_Punch_Through || (Type==ListType_Translucent && SortingEnabled)))
         glDepthFunc(Zfunction[gp->isp.DepthMode]);

#if TRIG_SORT
      if (SortingEnabled)
         glDepthMask(GL_FALSE);
      else
#endif
         glDepthMask(!gp->isp.ZWriteDis);
   }
}

template <u32 Type, bool SortingEnabled>
static void DrawList(const List<PolyParam>& gply)
{
   PolyParam* params=gply.head();
   int count=gply.used();

   if (count==0)
      return;
   //we want at least 1 PParam

   //reset the cache state
   cache.Reset(params);

   //set some 'global' modes for all primitives

   //Z funct. can be fixed on these combinations, avoid setting it all the time
   if (Type==ListType_Punch_Through || (Type==ListType_Translucent && SortingEnabled))
      glDepthFunc(Zfunction[6]);

   glEnable(GL_STENCIL_TEST);
   gl_state.cap_state[7] = 1;
   glStencilFunc(GL_ALWAYS,0,0);
   glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);

#ifndef NO_STENCIL_WORKAROUND
   //This looks like a driver bug
   glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
#endif

   while(count-->0)
   {
      if (params->count>2) //this actually happens for some games. No idea why ..
      {
         SetGPState<Type,SortingEnabled>(params);
         glDrawElements(GL_TRIANGLE_STRIP, params->count, GL_UNSIGNED_SHORT, (GLvoid*)(2*params->first));
      }

      params++;
   }
}

bool operator<(const PolyParam &left, const PolyParam &right)
{
/* put any condition you want to sort on here */
	return left.zvZ<right.zvZ;
	//return left.zMin<right.zMax;
}

//Sort based on min-z of each strip
void SortPParams(void)
{
   if (pvrrc.verts.used()==0 || pvrrc.global_param_tr.used()<=1)
      return;

   Vertex* vtx_base=pvrrc.verts.head();
   u16* idx_base=pvrrc.idx.head();

   PolyParam* pp=pvrrc.global_param_tr.head();
   PolyParam* pp_end= pp + pvrrc.global_param_tr.used();

   while(pp!=pp_end)
   {
      if (pp->count<2)
      {
         pp->zvZ=0;
      }
      else
      {
         u16* idx=idx_base+pp->first;

         Vertex* vtx=vtx_base+idx[0];
         Vertex* vtx_end=vtx_base + idx[pp->count-1]+1;

         u32 zv=0xFFFFFFFF;
         while(vtx!=vtx_end)
         {
            zv=min(zv,(u32&)vtx->z);
            vtx++;
         }

         pp->zvZ=(f32&)zv;
      }
      pp++;
   }

   std::stable_sort(pvrrc.global_param_tr.head(),pvrrc.global_param_tr.head()+pvrrc.global_param_tr.used());
}

static inline float min3(float v0,float v1,float v2)
{
	return min(min(v0,v1),v2);
}

static inline float max3(float v0,float v1,float v2)
{
	return max(max(v0,v1),v2);
}

static inline float minZ(Vertex* v,u16* mod)
{
	return min(min(v[mod[0]].z,v[mod[1]].z),v[mod[2]].z);
}

bool operator<(const IndexTrig &left, const IndexTrig &right)
{
	return left.z<right.z;
}

//are two poly params the same?
static inline bool PP_EQ(PolyParam* pp0, PolyParam* pp1)
{
	return 
      (pp0->pcw.full&PCW_DRAW_MASK)==(pp1->pcw.full&PCW_DRAW_MASK) 
      && pp0->isp.full==pp1->isp.full 
      && pp0->tcw.full==pp1->tcw.full
      && pp0->tsp.full==pp1->tsp.full
      && pp0->tileclip==pp1->tileclip;
}


static inline void fill_id(u16* d, Vertex* v0, Vertex* v1, Vertex* v2,  Vertex* vb)
{
	d[0]=v0-vb;
	d[1]=v1-vb;
	d[2]=v2-vb;
}

static void GenSorted(void)
{
	u32 tess_gen=0;

	pidx_sort.clear();

	if (pvrrc.verts.used()==0 || pvrrc.global_param_tr.used()<=1)
		return;

	Vertex* vtx_base=pvrrc.verts.head();
	u16* idx_base=pvrrc.idx.head();

	PolyParam* pp_base=pvrrc.global_param_tr.head();
	PolyParam* pp=pp_base;
	PolyParam* pp_end= pp + pvrrc.global_param_tr.used();
	
	Vertex* vtx_arr=vtx_base+idx_base[pp->first];
	vtx_sort_base=vtx_base;

	static u32 vtx_cnt;

	int vtx_count=idx_base[pp_end[-1].first+pp_end[-1].count-1]-idx_base[pp->first];
	if (vtx_count>vtx_cnt)
		vtx_cnt=vtx_count;

#if PRINT_SORT_STATS
	printf("TVTX: %d || %d\n",vtx_cnt,vtx_count);
#endif
	
	if (vtx_count<=0)
		return;

	//make lists of all triangles, with their pid and vid
	static vector<IndexTrig> lst;
	
	lst.resize(vtx_count*4);
	

	int pfsti=0;

	while(pp!=pp_end)
	{
		u32 ppid=(pp-pp_base);

		if (pp->count>2)
		{
			u16* idx=idx_base+pp->first;

			Vertex* vtx=vtx_base+idx[0];
			Vertex* vtx_end=vtx_base + idx[pp->count-1]-1;
			u32 flip=0;
			while(vtx!=vtx_end)
			{
				Vertex* v0, * v1, * v2, * v3, * v4, * v5;

				if (flip)
				{
					v0=&vtx[2];
					v1=&vtx[1];
					v2=&vtx[0];
				}
				else
				{
					v0=&vtx[0];
					v1=&vtx[1];
					v2=&vtx[2];
				}

				if (settings.pvr.subdivide_transp)
				{
					u32 tess_x=(max3(v0->x,v1->x,v2->x)-min3(v0->x,v1->x,v2->x))/32;
					u32 tess_y=(max3(v0->y,v1->y,v2->y)-min3(v0->y,v1->y,v2->y))/32;

					if (tess_x==1)
                  tess_x=0;
					if (tess_y==1)
                  tess_y=0;

					//bool tess=(maxZ(v0,v1,v2)/minZ(v0,v1,v2))>=1.2;

					if (tess_x + tess_y)
					{
						v3=pvrrc.verts.Append(3);
						v4=v3+1;
						v5=v4+1;

						//xyz
						for (int i=0;i<3;i++)
						{
							((float*)&v3->x)[i]=((float*)&v0->x)[i]*0.5f+((float*)&v2->x)[i]*0.5f;
							((float*)&v4->x)[i]=((float*)&v0->x)[i]*0.5f+((float*)&v1->x)[i]*0.5f;
							((float*)&v5->x)[i]=((float*)&v1->x)[i]*0.5f+((float*)&v2->x)[i]*0.5f;
						}

						//*TODO* Make it perspective correct

						//uv
						for (int i=0;i<2;i++)
						{
							((float*)&v3->u)[i]=((float*)&v0->u)[i]*0.5f+((float*)&v2->u)[i]*0.5f;
							((float*)&v4->u)[i]=((float*)&v0->u)[i]*0.5f+((float*)&v1->u)[i]*0.5f;
							((float*)&v5->u)[i]=((float*)&v1->u)[i]*0.5f+((float*)&v2->u)[i]*0.5f;
						}

						//color
						for (int i=0;i<4;i++)
						{
							v3->col[i]=v0->col[i]/2+v2->col[i]/2;
							v4->col[i]=v0->col[i]/2+v1->col[i]/2;
							v5->col[i]=v1->col[i]/2+v2->col[i]/2;
						}

						fill_id(lst[pfsti].id,v0,v3,v4,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v2,v3,v5,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v3,v4,v5,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						fill_id(lst[pfsti].id,v5,v4,v1,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;

						tess_gen+=3;
					}
					else
					{
						fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
						lst[pfsti].pid= ppid ;
						lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
						pfsti++;
					}
				}
				else
				{
					fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
					lst[pfsti].pid= ppid ;
					lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
					pfsti++;
				}

				flip ^= 1;
				
				vtx++;
			}
		}
		pp++;
	}

	u32 aused=pfsti;

	lst.resize(aused);

	//sort them
	std::stable_sort(lst.begin(),lst.end());

	//Merge pids/draw cmds if two different pids are actually equal
   for (u32 k=1;k<aused;k++)
   {
      if (lst[k].pid!=lst[k-1].pid)
      {
         if (PP_EQ(&pp_base[lst[k].pid],&pp_base[lst[k-1].pid]))
            lst[k].pid=lst[k-1].pid;
      }
   }

	//re-assemble them into drawing commands
	static vector<u16> vidx_sort;

	vidx_sort.resize(aused*3);

	int idx=-1;

	for (u32 i=0; i<aused; i++)
   {
      int pid=lst[i].pid;
      u16* midx=lst[i].id;

      vidx_sort[i*3 + 0]=midx[0];
      vidx_sort[i*3 + 1]=midx[1];
      vidx_sort[i*3 + 2]=midx[2];

      if (idx == pid)
         continue;

      SortTrigDrawParam stdp={pp_base + pid, (u16)(i*3), 0};

      if (idx!=-1)
      {
         SortTrigDrawParam* last=&pidx_sort[pidx_sort.size()-1];
         last->count=stdp.first-last->first;
      }

      pidx_sort.push_back(stdp);
      idx=pid;
   }

	SortTrigDrawParam* stdp=&pidx_sort[pidx_sort.size()-1];
	stdp->count=aused*3-stdp->first;

#if PRINT_SORT_STATS
	printf("Reassembled into %d from %d\n",pidx_sort.size(),pp_end-pp_base);
#endif

	//Upload to GPU if needed
	if (pidx_sort.size())
	{
		//Bind and upload sorted index buffer
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs2);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,vidx_sort.size()*2,&vidx_sort[0],GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

		if (tess_gen) printf("Generated %.2fK Triangles !\n",tess_gen/1000.0);
	}
}

static void DrawSorted(void)
{
   //if any drawing commands, draw them
   if (!pidx_sort.size())
      return;

   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs2);

   u32 count=pidx_sort.size();

   cache.Reset(pidx_sort[0].ppid);

   //set some 'global' modes for all primitives

   //Z sorting is fixed for .. sorted stuff
   glDepthFunc(Zfunction[6]);

   glEnable(GL_STENCIL_TEST);
   gl_state.cap_state[7] = 1;
   glStencilFunc(GL_ALWAYS,0,0);
   glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);

#ifndef NO_STENCIL_WORKAROUND
   //This looks like a driver bug
   glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
#endif

   for (u32 p=0; p<count; p++)
   {
      PolyParam* params = pidx_sort[p].ppid;
      if (pidx_sort[p].count>2) //this actually happens for some games. No idea why ..
      {
         SetGPState<ListType_Translucent,true>(params);
         glDrawElements(GL_TRIANGLES, pidx_sort[p].count, GL_UNSIGNED_SHORT, (GLvoid*)(2*pidx_sort[p].first));
      }
      params++;
   }
}

//All pixels are in area 0 by default.
//If inside an 'in' volume, they are in area 1
//if inside an 'out' volume, they are in area 0
/*
	Stencil bits:
		bit 7: mv affected (must be preserved)
		bit 1: current volume state
		but 0: summary result (starts off as 0)

	Lower 2 bits:

	IN volume (logical OR):
	00 -> 00
	01 -> 01
	10 -> 01
	11 -> 01

	Out volume (logical AND):
	00 -> 00
	01 -> 00
	10 -> 00
	11 -> 01
*/
static void SetMVS_Mode(u32 mv_mode,ISP_Modvol ispc)
{
	if (mv_mode==0)	//normal trigs
	{
		//set states
		glEnable(GL_DEPTH_TEST);
      gl_state.cap_state[0] = 1;
		//write only bit 1
		glStencilMask(2);
		//no stencil testing
		glStencilFunc(GL_ALWAYS,0,2);
		//count the number of pixels in front of the Z buffer (and only keep the lower bit of the count)
		glStencilOp(GL_KEEP,GL_KEEP,GL_INVERT);
#ifndef NO_STENCIL_WORKAROUND
		//this needs to be done .. twice ? looks like
		//a bug somewhere, on gles/nvgl ?
		glStencilOp(GL_KEEP,GL_KEEP,GL_INVERT);
#endif
		//Cull mode needs to be set
		SetCull(ispc.CullMode);
	}
	else
	{
		//1 (last in) or 2 (last out)
		//each triangle forms the last of a volume

		//common states

		//no depth test
		glDisable(GL_DEPTH_TEST);
      gl_state.cap_state[0] = 0;

		//write bits 1:0
		glStencilMask(3);

		if (mv_mode==1)
		{
			//res : old : final 
			//0   : 0      : 00
			//0   : 1      : 01
			//1   : 0      : 01
			//1   : 1      : 01
			

			//if (1<=st) st=1; else st=0;
			glStencilFunc(GL_LEQUAL,1,3);
			glStencilOp(GL_ZERO,GL_ZERO,GL_REPLACE);
#ifndef NO_STENCIL_WORKAROUND
			//Look @ comment above -- this looks like a driver bug
			glStencilOp(GL_ZERO,GL_ZERO,GL_REPLACE);
#endif

			/*
			//if !=0 -> set to 10
			verifyc(dev->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_LESSEQUAL));
			verifyc(dev->SetRenderState(D3DRS_STENCILREF,1));					
			verifyc(dev->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_REPLACE));
			verifyc(dev->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_ZERO));
			*/
		}
		else
		{
			/*
				this is bugged. a lot.
				I've only seen a single game use it, so i guess it doesn't matter ? (Zombie revenge)
				(actually, i think there was also another, racing game)
			*/

			//res : old : final 
			//0   : 0   : 00
			//0   : 1   : 00
			//1   : 0   : 00
			//1   : 1   : 01

			//if (2>st) st=1; else st=0;	//can't be done with a single pass
			glStencilFunc(GL_GREATER,1,3);
			glStencilOp(GL_ZERO,GL_KEEP,GL_REPLACE);
#ifndef NO_STENCIL_WORKAROUND
			//Look @ comment above -- this looks like a driver bug
			glStencilOp(GL_ZERO,GL_KEEP,GL_REPLACE);
#endif
		}
	}
}

static void SetupMainVBO(void)
{
#ifdef CORE
	glBindVertexArray(vbo.vao);
#endif

	glBindBuffer(GL_ARRAY_BUFFER, vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,spc));

	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));

}

static void SetupModvolVBO(void)
{
#ifdef CORE
	glBindVertexArray(vbo.vao);
#endif

	glBindBuffer(GL_ARRAY_BUFFER, vbo.modvols);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float)*3, (void*)0);

	glDisableVertexAttribArray(VERTEX_UV_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
}

#if 0
static void DrawModVols(void)
{
	if (pvrrc.modtrig.used()==0 /*|| GetAsyncKeyState(VK_F4)*/)
		return;

	SetupModvolVBO();

	glEnable(GL_BLEND);
   gl_state.cap_state[1] = 1;
   gl_state.blendfunc.sfactor = GL_SRC_ALPHA;
   gl_state.blendfunc.dfactor = GL_ONE_MINUS_SRC_ALPHA;
   glBlendFunc(gl_state.blendfunc.sfactor, gl_state.blendfunc.dfactor);

   gl_state.program = modvol_shader.program;
	glUseProgram(gl_state.program);
	glUniform1f(modvol_shader.sp_ShaderColor,0.5f);

	glDepthMask(GL_FALSE);
	glDepthFunc(GL_GREATER);

	if(0 /*|| GetAsyncKeyState(VK_F5)*/ )
	{
		//simply draw the volumes -- for debugging
		SetCull(0);
		glDrawArrays(GL_TRIANGLES,0,pvrrc.modtrig.used()*3);
		SetupMainVBO();
	}
	else
	{
		/*
		mode :
		normal trig : flip
		last *in*   : flip, merge*in* &clear from last merge
		last *out*  : flip, merge*out* &clear from last merge
		*/

		/*

			Do not write to color
			Do not write to depth

			read from stencil bits 1:0
			write to stencil bits 1:0
		*/

		glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
		glDepthFunc(GL_GREATER);

		if ( 0 /* || GetAsyncKeyState(VK_F6)*/ )
		{
			//simple single level stencil
			glEnable(GL_STENCIL_TEST);
         gl_state.cap_state[7] = 1;
			glStencilFunc(GL_ALWAYS,0x1,0x1);
			glStencilOp(GL_KEEP,GL_KEEP,GL_INVERT);
#ifndef NO_STENCIL_WORKAROUND
			//looks like a driver bug
			glStencilOp(GL_KEEP,GL_KEEP,GL_INVERT);
#endif
			glStencilMask(0x1);
			SetCull(0);
			glDrawArrays(GL_TRIANGLES,0,pvrrc.modtrig.used()*3);
		}
		else if (true)
		{
			//Full emulation
			//the *out* mode is buggy

			u32 mod_base=0; //cur start triangle
			u32 mod_last=0; //last merge

			u32 cmv_count=(pvrrc.global_param_mvo.used()-1);
			ISP_Modvol* params=pvrrc.global_param_mvo.head();

			//ISP_Modvol
			for (u32 cmv=0;cmv<cmv_count;cmv++)
			{

				ISP_Modvol ispc=params[cmv];
				mod_base=ispc.id;
				u32 sz=params[cmv+1].id-mod_base;

				u32 mv_mode = ispc.DepthMode;


				if (mv_mode==0)	//normal trigs
				{
					SetMVS_Mode(0,ispc);
					//Render em (counts intersections)
					//verifyc(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,sz,pvrrc.modtrig.data+mod_base,3*4));
					glDrawArrays(GL_TRIANGLES,mod_base*3,sz*3);
				}
				else if (mv_mode<3)
				{
					while(sz)
					{
						//merge and clear all the prev. stencil bits

						//Count Intersections (last poly)
						SetMVS_Mode(0,ispc);
						glDrawArrays(GL_TRIANGLES,mod_base*3,3);

						//Sum the area
						SetMVS_Mode(mv_mode,ispc);
						glDrawArrays(GL_TRIANGLES,mod_last*3,(mod_base-mod_last+1)*3);

						//update pointers
						mod_last=mod_base+1;
						sz--;
						mod_base++;
					}
				}
			}
		}
		//disable culling
		SetCull(0);
		//enable color writes
		glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);

		//black out any stencil with '1'
		glEnable(GL_BLEND);
      gl_state.cap_state[1] = 1;
      gl_state.blendfunc.sfactor = GL_SRC_ALPHA;
      gl_state.blendfunc.dfactor = GL_ONE_MINUS_SRC_ALPHA;
      glBlendFunc(gl_state.blendfunc.sfactor, gl_state.blendfunc.dfactor);
		
		glEnable(GL_STENCIL_TEST);
      gl_state.cap_state[7] = 1;
		glStencilFunc(GL_EQUAL,0x81,0x81); //only pixels that are Modvol enabled, and in area 1
		
		//clear the stencil result bit
		glStencilMask(0x3);    //write to lsb 
		glStencilOp(GL_ZERO,GL_ZERO,GL_ZERO);
#ifndef NO_STENCIL_WORKAROUND
		//looks like a driver bug ?
		glStencilOp(GL_ZERO,GL_ZERO,GL_ZERO);
#endif

		//don't do depth testing
		glDisable(GL_DEPTH_TEST);
      gl_state.cap_state[0] = 0;

		SetupMainVBO();
		glDrawArrays(GL_TRIANGLE_STRIP,0,4);

		//Draw and blend
		glDrawArrays(GL_TRIANGLES,pvrrc.modtrig.used(),2);

      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	//restore states
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
   gl_state.cap_state[1] = 0;
	glEnable(GL_DEPTH_TEST);
   gl_state.cap_state[0] = 1;
	glDisable(GL_STENCIL_TEST);
   gl_state.cap_state[7] = 0;
}
#endif

/*
GL|ES 2
Slower, smaller subset of gl2

*Optimisation notes*
Keep stuff in packed ints
Keep data as small as possible
Keep vertex programs as small as possible
The drivers more or less suck. Don't depend on dynamic allocation, or any 'complex' feature
as it is likely to be problematic/slow
Do we really want to enable striping joins?

*Design notes*
Follow same architecture as the d3d renderer for now
Render to texture, keep track of textures in GL memory
Direct flip to screen (no vlbank/fb emulation)
Do we really need a combining shader? it is needlessly expensive for openGL | ES
Render contexts
Free over time? we actually care about ram usage here?
Limit max resource size? for psp 48k verts worked just fine

FB:
Pixel clip, mapping

SPG/VO:
mapping

TA:
Tile clip

*/

#include "oslib/oslib.h"
#include "rend/rend.h"
#include "hw/pvr/Renderer_if.h"

float fb_scale_x,fb_scale_y;

#define attr "attribute"
#define vary "varying"
#define FRAGCOL "gl_FragColor"
#define TEXLOOKUP "texture2D"

#ifdef GLES
#define HIGHP "highp"
#define MEDIUMP "mediump"
#define LOWP "lowp"
#else
#define HIGHP
#define MEDIUMP
#define LOWP
#endif

//Fragment and vertex shaders code
//pretty much 1:1 copy of the d3d ones for now
const char* VertexShaderSource =
#ifndef GLES
   "#version 120 \n"
#endif
"\
/* Vertex constants*/  \n\
uniform " HIGHP " vec4      scale; \n\
uniform " HIGHP " vec4      depth_scale; \n\
uniform " HIGHP " float sp_FOG_DENSITY; \n\
/* Vertex input */ \n\
" attr " " HIGHP " vec4    in_pos; \n\
" attr " " LOWP " vec4     in_base; \n\
" attr " " LOWP " vec4     in_offs; \n\
" attr " " MEDIUMP " vec2  in_uv; \n\
/* output */ \n\
" vary " " LOWP " vec4 vtx_base; \n\
" vary " " LOWP " vec4 vtx_offs; \n\
" vary " " MEDIUMP " vec2 vtx_uv; \n\
" vary " " HIGHP " vec3 vtx_xyz; \n\
void main() \n\
{ \n\
	vtx_base=in_base; \n\
	vtx_offs=in_offs; \n\
	vtx_uv=in_uv; \n\
	vec4 vpos=in_pos; \n\
	vtx_xyz.xy = vpos.xy;  \n\
	vtx_xyz.z = vpos.z*sp_FOG_DENSITY;  \n\
	vpos.w=1.0/vpos.z;  \n\
	vpos.xy=vpos.xy*scale.xy-scale.zw;  \n\
	vpos.xy*=vpos.w;  \n\
	vpos.z=depth_scale.x+depth_scale.y*vpos.w;  \n\
	gl_Position = vpos; \n\
}";




const char* PixelPipelineShader =
#ifndef GLES
      "#version 120 \n"
#endif
"\
\
#define cp_AlphaTest %d \n\
#define pp_ClipTestMode %d.0 \n\
#define pp_UseAlpha %d \n\
#define pp_Texture %d \n\
#define pp_IgnoreTexA %d \n\
#define pp_ShadInstr %d \n\
#define pp_Offset %d \n\
#define pp_FogCtrl %d \n\
/* Shader program params*/ \n\
/* gles has no alpha test stage, so its emulated on the shader */ \n\
uniform " LOWP " float cp_AlphaTestValue; \n\
uniform " LOWP " vec4 pp_ClipTest; \n\
uniform " LOWP " vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT; \n\
uniform " HIGHP " vec2 sp_LOG_FOG_COEFS; \n\
uniform sampler2D tex,fog_table; \n\
/* Vertex input*/ \n\
" vary " " LOWP " vec4 vtx_base; \n\
" vary " " LOWP " vec4 vtx_offs; \n\
" vary " " MEDIUMP " vec2 vtx_uv; \n\
" vary " " HIGHP " vec3 vtx_xyz; \n\
" LOWP " float fog_mode2(" HIGHP " float val) \n\
{ \n\
   " HIGHP " float fog_idx=clamp(val,0.0,127.99); \n\
	return clamp(sp_LOG_FOG_COEFS.y*log2(fog_idx)+sp_LOG_FOG_COEFS.x,0.001,1.0); //the clamp is required due to yet another bug !\n\
} \n\
void main() \n\
{ \n\
   " LOWP " vec4 color=vtx_base; \n\
	#if pp_UseAlpha==0 \n\
		color.a=1.0; \n\
	#endif\n\
	#if pp_FogCtrl==3 \n\
		color=vec4(sp_FOG_COL_RAM.rgb,fog_mode2(vtx_xyz.z)); \n\
	#endif\n\
	#if pp_Texture==1 \n\
	{ \n\
      " LOWP " vec4 texcol=" TEXLOOKUP "(tex,vtx_uv); \n\
		\n\
		#if pp_IgnoreTexA==1 \n\
			texcol.a=1.0;	 \n\
		#endif\n\
		\n\
		#if pp_ShadInstr==0 \n\
		{ \n\
			color.rgb=texcol.rgb; \n\
			color.a=texcol.a; \n\
		} \n\
		#endif\n\
		#if pp_ShadInstr==1 \n\
		{ \n\
			color.rgb*=texcol.rgb; \n\
			color.a=texcol.a; \n\
		} \n\
		#endif\n\
		#if pp_ShadInstr==2 \n\
		{ \n\
			color.rgb=mix(color.rgb,texcol.rgb,texcol.a); \n\
		} \n\
		#endif\n\
		#if  pp_ShadInstr==3 \n\
		{ \n\
			color*=texcol; \n\
		} \n\
		#endif\n\
		\n\
		#if pp_Offset==1 \n\
		{ \n\
			color.rgb+=vtx_offs.rgb; \n\
			if (pp_FogCtrl==1) \n\
				color.rgb=mix(color.rgb,sp_FOG_COL_VERT.rgb,vtx_offs.a); \n\
		} \n\
		#endif\n\
	} \n\
	#endif\n\
	#if pp_FogCtrl==0 \n\
	{ \n\
		color.rgb=mix(color.rgb,sp_FOG_COL_RAM.rgb,fog_mode2(vtx_xyz.z));  \n\
	} \n\
	#endif\n\
	#if cp_AlphaTest == 1 \n\
		if (cp_AlphaTestValue>color.a) discard;\n\
	#endif  \n\
	//color.rgb=vec3(vtx_xyz.z/255.0);\n\
	" FRAGCOL "=color; \n\
}";

const char* ModifierVolumeShader =
" \
uniform " LOWP " float sp_ShaderColor; \n\
/* Vertex input*/ \n\
void main() \n\
{ \n\
	" FRAGCOL "=vec4(0.0, 0.0, 0.0, sp_ShaderColor); \n\
}";


static int gles_screen_width  = 640;
static int gles_screen_height = 480;

void egl_stealcntx(void)
{
}

struct ShaderUniforms_t
{
	float PT_ALPHA;
	float scale_coefs[4];
	float depth_coefs[4];
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	float fog_coefs[2];

	void Set(PipelineShader* s)
	{
		if (s->cp_AlphaTestValue!=-1)
			glUniform1f(s->cp_AlphaTestValue,PT_ALPHA);

		if (s->scale!=-1)
			glUniform4fv( s->scale, 1, scale_coefs);

		if (s->depth_scale!=-1)
			glUniform4fv( s->depth_scale, 1, depth_coefs);

		if (s->sp_FOG_DENSITY!=-1)
			glUniform1f( s->sp_FOG_DENSITY,fog_den_float);

		if (s->sp_FOG_COL_RAM!=-1)
			glUniform3fv( s->sp_FOG_COL_RAM, 1, ps_FOG_COL_RAM);

		if (s->sp_FOG_COL_VERT!=-1)
			glUniform3fv( s->sp_FOG_COL_VERT, 1, ps_FOG_COL_VERT);

		if (s->sp_LOG_FOG_COEFS!=-1)
			glUniform2fv(s->sp_LOG_FOG_COEFS,1, fog_coefs);
	}

} ShaderUniforms;

GLuint gl_CompileShader(const char* shader,GLuint type)
{
	GLint result;
	GLint compile_log_len;
	GLuint rv=glCreateShader(type);
	glShaderSource(rv, 1,&shader, NULL);
	glCompileShader(rv);

	//lets see if it compiled ...
	glGetShaderiv(rv, GL_COMPILE_STATUS, &result);
	glGetShaderiv(rv, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
		if (compile_log_len==0)
			compile_log_len=1;
		char* compile_log=(char*)malloc(compile_log_len);
		*compile_log=0;

		glGetShaderInfoLog(rv, compile_log_len, &compile_log_len, compile_log);
		printf("Shader: %s \n%s\n",result?"compiled!":"failed to compile",compile_log);

		free(compile_log);
	}

	return rv;
}

GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader)
{
	//create shaders
	GLuint vs=gl_CompileShader(VertexShader ,GL_VERTEX_SHADER);
	GLuint ps=gl_CompileShader(FragmentShader ,GL_FRAGMENT_SHADER);

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, ps);

	//bind vertex attribute to vbo inputs
	glBindAttribLocation(program, VERTEX_POS_ARRAY,      "in_pos");
	glBindAttribLocation(program, VERTEX_COL_BASE_ARRAY, "in_base");
	glBindAttribLocation(program, VERTEX_COL_OFFS_ARRAY, "in_offs");
	glBindAttribLocation(program, VERTEX_UV_ARRAY,       "in_uv");

#ifndef GLES
	glBindFragDataLocation(program, 0, "FragColor");
#endif

	glLinkProgram(program);

	GLint result;
	glGetProgramiv(program, GL_LINK_STATUS, &result);


	GLint compile_log_len;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
		if (compile_log_len==0)
			compile_log_len=1;
		compile_log_len+= 1024;
		char* compile_log=(char*)malloc(compile_log_len);
		*compile_log=0;

		glGetProgramInfoLog(program, compile_log_len, &compile_log_len, compile_log);
		printf("Shader linking: %s \n (%d bytes), - %s -\n",result?"linked":"failed to link", compile_log_len,compile_log);

		free(compile_log);
		die("shader compile fail\n");
	}

	glDeleteShader(vs);
	glDeleteShader(ps);

	glUseProgram(program);

	verify(glIsProgram(program));

   gl_state.program = modvol_shader.program;

	return program;
}

int GetProgramID(u32 cp_AlphaTest,
      u32 pp_ClipTestMode,
      u32 pp_Texture,
      u32 pp_UseAlpha,
      u32 pp_IgnoreTexA,
      u32 pp_ShadInstr,
      u32 pp_Offset,
      u32 pp_FogCtrl)
{
	u32 rv=0;

	rv|=pp_ClipTestMode;
	rv<<=1; rv|=cp_AlphaTest;
	rv<<=1; rv|=pp_Texture;
	rv<<=1; rv|=pp_UseAlpha;
	rv<<=1; rv|=pp_IgnoreTexA;
	rv<<=2; rv|=pp_ShadInstr;
	rv<<=1; rv|=pp_Offset;
	rv<<=2; rv|=pp_FogCtrl;

	return rv;
}

bool CompilePipelineShader(void *data)
{
	char pshader[8192];
   PipelineShader *s = (PipelineShader*)data;

	sprintf(pshader,PixelPipelineShader,
                s->cp_AlphaTest,s->pp_ClipTestMode,s->pp_UseAlpha,
                s->pp_Texture,s->pp_IgnoreTexA,s->pp_ShadInstr,s->pp_Offset,s->pp_FogCtrl);

	s->program=gl_CompileAndLink(VertexShaderSource,pshader);


	//setup texture 0 as the input for the shader
	GLuint gu=glGetUniformLocation(s->program, "tex");
	if (s->pp_Texture==1)
		glUniform1i(gu,0);

	//get the uniform locations
	s->scale	            = glGetUniformLocation(s->program, "scale");
	s->depth_scale      = glGetUniformLocation(s->program, "depth_scale");


	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if (s->pp_FogCtrl==1 && s->pp_Texture==1)
		s->sp_FOG_COL_VERT=glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT=-1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
		s->sp_LOG_FOG_COEFS=glGetUniformLocation(s->program, "sp_LOG_FOG_COEFS");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
		s->sp_LOG_FOG_COEFS=-1;
	}


	ShaderUniforms.Set(s);

	return glIsProgram(s->program)==GL_TRUE;
}

static bool gl_create_resources(void)
{
#ifdef CORE
	//create vao
	//This is really not "proper", vaos are suposed to be defined once
	//i keep updating the same one to make the es2 code work in 3.1 context
	glGenVertexArrays(1, &vbo.vao);
#endif

	//create vbos
	glGenBuffers(1, &vbo.geometry);
	glGenBuffers(1, &vbo.modvols);
	glGenBuffers(1, &vbo.idxs);
	glGenBuffers(1, &vbo.idxs2);

	memset(program_table,0,sizeof(program_table));

	PipelineShader* dshader=0;
	u32 compile=0;
#define forl(name,max) for(u32 name=0;name<=max;name++)
	forl(cp_AlphaTest,1)
	{
		forl(pp_ClipTestMode,2)
		{
			forl(pp_UseAlpha,1)
			{
				forl(pp_Texture,1)
				{
					forl(pp_FogCtrl,3)
					{
						forl(pp_IgnoreTexA,1)
						{
							forl(pp_ShadInstr,3)
							{
								forl(pp_Offset,1)
								{
									dshader=&program_table[GetProgramID(cp_AlphaTest,pp_ClipTestMode,pp_Texture,pp_UseAlpha,pp_IgnoreTexA,
															pp_ShadInstr,pp_Offset,pp_FogCtrl)];

									dshader->cp_AlphaTest = cp_AlphaTest;
									dshader->pp_ClipTestMode = pp_ClipTestMode-1;
									dshader->pp_Texture = pp_Texture;
									dshader->pp_UseAlpha = pp_UseAlpha;
									dshader->pp_IgnoreTexA = pp_IgnoreTexA;
									dshader->pp_ShadInstr = pp_ShadInstr;
									dshader->pp_Offset = pp_Offset;
									dshader->pp_FogCtrl = pp_FogCtrl;
									dshader->program = -1;
								}
							}
						}
					}
				}
			}
		}
	}



	modvol_shader.program=gl_CompileAndLink(VertexShaderSource,ModifierVolumeShader);
	modvol_shader.scale          = glGetUniformLocation(modvol_shader.program, "scale");
	modvol_shader.sp_ShaderColor = glGetUniformLocation(modvol_shader.program, "sp_ShaderColor");
	modvol_shader.depth_scale    = glGetUniformLocation(modvol_shader.program, "depth_scale");

	//#define PRECOMPILE_SHADERS
	#ifdef PRECOMPILE_SHADERS
	for (u32 i=0;i<sizeof(program_table)/sizeof(program_table[0]);i++)
	{
		if (!CompilePipelineShader(	&program_table[i] ))
			return false;
	}
	#endif

	return true;
}

GLuint gl_CompileShader(const char* shader,GLuint type);

//setup

static bool gles_init(void)
{
   if (!gl_create_resources())
      return false;

   gl_state.framebuf = hw_render.get_current_framebuffer();
   gl_state.cullmode = GL_BACK;

   return true;
}

static void tryfit(float* x,float* y)
{
	//y=B*ln(x)+A

	double sylnx=0,sy=0,slnx=0,slnx2=0;

	u32 cnt=0;

	for (int i=0;i<128;i++)
	{
		int rep=1;

		//discard values clipped to 0 or 1
		if (i<128 && y[i]==1 && y[i+1]==1)
			continue;

		if (i>0 && y[i]==0 && y[i-1]==0)
			continue;

		//Add many samples for first and last value (fog-in, fog-out -> important)
		if (i>0 && y[i]!=1 && y[i-1]==1)
			rep=10000;

		if (i<128 && y[i]!=0 && y[i+1]==0)
			rep=10000;

		for (int j=0;j<rep;j++)
		{
			cnt++;
			sylnx+=y[i]*log((double)x[i]);
			sy+=y[i];
			slnx+=log((double)x[i]);
			slnx2+=log((double)x[i])*log((double)x[i]);
		}
	}

	double a,b;
	b=(cnt*sylnx-sy*slnx)/(cnt*slnx2-slnx*slnx);
	a=(sy-b*slnx)/(cnt);


	//We use log2 and not ln on calculations	//B*log(x)+A
	//log2(x)=log(x)/log(2)
	//log(x)=log2(x)*log(2)
	//B*log(2)*log(x)+A
	b*=logf(2.0);

	float maxdev=0;
	for (int i=0;i<128;i++)
	{
		float diff=min(max(b*logf(x[i])/logf(2.0)+a,(double)0),(double)1)-y[i];
		maxdev=max((float)fabs((float)diff),(float)maxdev);
	}
	printf("FOG TABLE Curve match: maxdev: %.02f cents\n",maxdev*100);
	fog_coefs[0]=a;
	fog_coefs[1]=b;
	//printf("%f\n",B*log(maxdev)/log(2.0)+A);
}

static void ClearBG(void)
{

}

static bool ProcessFrame(TA_context* ctx)
{
   //disable RTTs for now ..
   if (!enable_rtt && ctx->rend.isRTT)
      return false;

#ifndef TARGET_NO_THREADS
   slock_lock(ctx->rend_inuse);
#endif
   ctx->MarkRend();

   if (KillTex)
   {
      void killtex();
      killtex();
      printf("Texture cache cleared\n");
   }

   if (!ta_parse_vdrc(ctx))
      return false;

   CollectCleanup();

   return true;
}

static bool RenderFrame(void)
{
	bool is_rtt=pvrrc.isRTT;

	//if (FrameCount&7) return;

	//Setup the matrix

	//TODO: Make this dynamic
	float vtx_min_fZ=0.f;	//pvrrc.fZ_min;
	float vtx_max_fZ=pvrrc.fZ_max;

	//sanitise the values, now with NaN detection (for omap)
	//0x49800000 is 1024*1024. Using integer math to avoid issues w/ infs and nans
	if ((s32&)vtx_max_fZ<0 || (u32&)vtx_max_fZ>0x49800000)
		vtx_max_fZ=10*1024;


	//add some extra range to avoid clipping border cases
	vtx_min_fZ*=0.98f;
	vtx_max_fZ*=1.001f;

	//calculate a projection so that it matches the pvr x,y setup, and
	//a) Z is linearly scaled between 0 ... 1
	//b) W is passed though for proper perspective calculations

	/*
	PowerVR coords:
	fx, fy (pixel coordinates)
	fz=1/w

	(as a note, fx=x*fz;fy=y*fz)

	Clip space
	-Wc .. Wc, xyz
	x: left-right, y: bottom-top
	NDC space
	-1 .. 1, xyz
	Window space:
	translated NDC (viewport, glDepth)

	Attributes:
	//this needs to be cleared up, been some time since I wrote my rasteriser and i'm starting
	//to forget/mixup stuff
	vaX         -> VS output
	iaX=vaX*W   -> value to be interpolated
	iaX',W'     -> interpolated values
	paX=iaX'/W' -> Per pixel interpolated value for attribute


	Proper mappings:
	Output from shader:
	W=1/fz
	x=fx*W -> maps to fx after perspective divide
	y=fy*W ->         fy   -//-
	z=-W for min, W for max. Needs to be linear.



	umodified W, perfect mapping:
	Z mapping:
	pz=z/W
	pz=z/(1/fz)
	pz=z*fz
	z=zt_s+zt_o
	pz=(zt_s+zt_o)*fz
	pz=zt_s*fz+zt_o*fz
	zt_s=scale
	zt_s=2/(max_fz-min_fz)
	zt_o*fz=-min_fz-1
	zt_o=(-min_fz-1)/fz == (-min_fz-1)*W


	x=fx/(fx_range/2)-1		//0 to max -> -1 to 1
	y=fy/(-fy_range/2)+1	//0 to max -> 1 to -1
	z=-min_fz*W + (zt_s-1)  //0 to +inf -> -1 to 1

	o=a*z+c
	1=a*z_max+c
	-1=a*z_min+c

	c=-a*z_min-1
	1=a*z_max-a*z_min-1
	2=a*(z_max-z_min)
	a=2/(z_max-z_min)
	*/

	//float B=2/(min_invW-max_invW);
	//float A=-B*max_invW+vnear;

	//these should be adjusted based on the current PVR scaling etc params
	float dc_width=640;
	float dc_height=480;

	if (!is_rtt)
	{
		gcflip=0;
		dc_width=640;
		dc_height=480;
	}
	else
	{
		gcflip=1;

		//For some reason this produces wrong results
		//so for now its hacked based like on the d3d code
		/*
		dc_width=FB_X_CLIP.max-FB_X_CLIP.min+1;
		dc_height=FB_Y_CLIP.max-FB_Y_CLIP.min+1;
		u32 pvr_stride=(FB_W_LINESTRIDE.stride)*8;
		*/

		dc_width=640;
		dc_height=480;
	}

	float scale_x=1, scale_y=1;

	float scissoring_scale_x = 1;

	if (!is_rtt)
	{
		scale_x=fb_scale_x;
		scale_y=fb_scale_y;

		//work out scaling parameters !
		//Pixel doubling is on VO, so it does not affect any pixel operations
		//A second scaling is used here for scissoring
		if (VO_CONTROL.pixel_double)
		{
			scissoring_scale_x = 0.5f;
			scale_x *= 0.5f;
		}
	}

	if (SCALER_CTL.hscale)
	{
		scale_x*=2;
	}

   if (is_rtt)
   {
      switch (gles_screen_width)
      {
         case 640:
            scale_x = 1;
            scale_y = 1;
            break;
         case 1280:
            scale_x = 2;
            scale_y = 2;
            break;
         case 1920:
            scale_x = 3;
            scale_y = 3;
            break;
         case 2560:
            scale_x = 4;
            scale_y = 4;
            break;
         case 3200:
            scale_x = 5;
            scale_y = 5;
            break;
         case 3840:
            scale_x = 6;
            scale_y = 6;
            break;
         case 4480:
            scale_x = 7;
            scale_y = 7;
            break;
      }
   }

	dc_width  *= scale_x;
	dc_height *= scale_y;

   gl_state.program = modvol_shader.program;
	glUseProgram(gl_state.program);

	/*

	float vnear=0;
	float vfar =1;

	float max_invW=1/vtx_min_fZ;
	float min_invW=1/vtx_max_fZ;

	float B=vfar/(min_invW-max_invW);
	float A=-B*max_invW+vnear;


	GLfloat dmatrix[16] =
	{
		(2.f/dc_width)  ,0                ,-(640/dc_width)              ,0  ,
		0               ,-(2.f/dc_height) ,(480/dc_height)              ,0  ,
		0               ,0                ,A                            ,B  ,
		0               ,0                ,1                            ,0
	};

	glUniformMatrix4fv(matrix, 1, GL_FALSE, dmatrix);

	*/

	/*
		Handle Dc to screen scaling
	*/
	float dc2s_scale_h = gles_screen_height/480.0f;
	float ds2s_offs_x  = (gles_screen_width-dc2s_scale_h*640)/2;

	//-1 -> too much to left
	ShaderUniforms.scale_coefs[0]=2.0f/(gles_screen_width/dc2s_scale_h*scale_x);
	ShaderUniforms.scale_coefs[1]=(is_rtt?2:-2)/dc_height;
	ShaderUniforms.scale_coefs[2]=1-2*ds2s_offs_x/(gles_screen_width);
	ShaderUniforms.scale_coefs[3]=(is_rtt?1:-1);


	ShaderUniforms.depth_coefs[0]=2/(vtx_max_fZ-vtx_min_fZ);
	ShaderUniforms.depth_coefs[1]=-vtx_min_fZ-1;
	ShaderUniforms.depth_coefs[2]=0;
	ShaderUniforms.depth_coefs[3]=0;

	//printf("scale: %f, %f, %f, %f\n",scale_coefs[0],scale_coefs[1],scale_coefs[2],scale_coefs[3]);


	//VERT and RAM fog color constants
	u8* fog_colvert_bgra=(u8*)&FOG_COL_VERT;
	u8* fog_colram_bgra=(u8*)&FOG_COL_RAM;
	ShaderUniforms.ps_FOG_COL_VERT[0]=fog_colvert_bgra[2]/255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[1]=fog_colvert_bgra[1]/255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[2]=fog_colvert_bgra[0]/255.0f;

	ShaderUniforms.ps_FOG_COL_RAM[0]=fog_colram_bgra [2]/255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[1]=fog_colram_bgra [1]/255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[2]=fog_colram_bgra [0]/255.0f;

	//Fog density constant
	u8* fog_density=(u8*)&FOG_DENSITY;
	float fog_den_mant=fog_density[1]/128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	s32 fog_den_exp=(s8)fog_density[0];
	ShaderUniforms.fog_den_float=fog_den_mant*powf(2.0f,fog_den_exp);


	if (fog_needs_update)
	{
		fog_needs_update=false;
		//Get the coefs for the fog curve
		u8* fog_table=(u8*)FOG_TABLE;
		float xvals[128];
		float yvals[128];
		for (int i=0;i<128;i++)
		{
			xvals[i]=powf(2.0f,i>>4)*(1+(i&15)/16.f);
			yvals[i]=fog_table[i*4+1]/255.0f;
		}

		tryfit(xvals,yvals);
	}

   gl_state.program = modvol_shader.program;
	glUseProgram(gl_state.program);

	glUniform4fv(modvol_shader.scale, 1, ShaderUniforms.scale_coefs);
	glUniform4fv(modvol_shader.depth_scale, 1, ShaderUniforms.depth_coefs);


	GLfloat td[4]={0.5,0,0,0};

	ShaderUniforms.PT_ALPHA=(PT_ALPHA_REF&0xFF)/255.0f;

	for (u32 i=0;i<sizeof(program_table)/sizeof(program_table[0]);i++)
	{
		PipelineShader* s=&program_table[i];
		if (s->program == -1)
			continue;

      gl_state.program = s->program;
		glUseProgram(gl_state.program);

		ShaderUniforms.Set(s);
	}
	//setup render target first
	if (is_rtt)
	{
		GLuint channels,format;
		switch(FB_W_CTRL.fb_packmode)
		{
		case 0: //0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_5_5_5_1;
			break;

		case 1: //0x1   565 RGB 16 bit
			channels=GL_RGB;
			format=GL_UNSIGNED_SHORT_5_6_5;
			break;

		case 2: //0x2   4444 ARGB 16 bit
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_5_5_5_1;
			break;

		case 3://0x3    1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold.
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_5_5_5_1;
			break;

		case 4: //0x4   888 RGB 24 bit packed
			channels=GL_RGB;
			format=GL_UNSIGNED_SHORT_5_6_5;
			break;

		case 5: //0x5   0888 KRGB 32 bit    K is the value of fk_kval.
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_4_4_4_4;
			break;

		case 6: //0x6   8888 ARGB 32 bit
			channels=GL_RGBA;
			format=GL_UNSIGNED_SHORT_4_4_4_4;
			break;

		case 7: //7     invalid
			die("7 is not valid");
			break;
		}
		BindRTT(FB_W_SOF1&VRAM_MASK,FB_X_CLIP.max-FB_X_CLIP.min+1,FB_Y_CLIP.max-FB_Y_CLIP.min+1,channels,format);
	}

   gl_state.clear_color.r = 0;
   gl_state.clear_color.g = 0;
   gl_state.clear_color.b = 0;
   gl_state.clear_color.a = 1.0f;

	if (settings.rend.WideScreen)
   {
      gl_state.clear_color.r = pvrrc.verts.head()->col[2]/255.0f;
      gl_state.clear_color.g = pvrrc.verts.head()->col[1]/255.0f;
      gl_state.clear_color.b = pvrrc.verts.head()->col[0]/255.0f;
   }

   glClearColor(gl_state.clear_color.r, gl_state.clear_color.g, gl_state.clear_color.b, gl_state.clear_color.a);
#ifdef GLES
	glClearDepthf(0.f);
#else
   glClearDepth(0.f);
#endif

   gl_state.viewport.x = 0;
   gl_state.viewport.y = 0;
   gl_state.viewport.w = gles_screen_width;
   gl_state.viewport.h = gles_screen_height;

   glViewport(gl_state.viewport.x, gl_state.viewport.y, gl_state.viewport.w, gl_state.viewport.h);
	glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	if (UsingAutoSort())
		GenSorted();

	//move vertex to gpu

	//Main VBO
	glBindBuffer(GL_ARRAY_BUFFER, vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo.idxs);

	glBufferData(GL_ARRAY_BUFFER,pvrrc.verts.bytes(),pvrrc.verts.head(),GL_STREAM_DRAW);

	glBufferData(GL_ELEMENT_ARRAY_BUFFER,pvrrc.idx.bytes(),pvrrc.idx.head(),GL_STREAM_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	//Modvol VBO
	if (pvrrc.modtrig.used())
	{
		glBindBuffer(GL_ARRAY_BUFFER, vbo.modvols);
		glBufferData(GL_ARRAY_BUFFER,pvrrc.modtrig.bytes(),pvrrc.modtrig.head(),GL_STREAM_DRAW);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	int offs_x=ds2s_offs_x+0.5f;
	//this needs to be scaled

	//not all scaling affects pixel operations, scale to adjust for that
	scale_x *= scissoring_scale_x;

	#if 0
		//handy to debug really stupid render-not-working issues ...
		printf("SS: %dx%d\n", gles_screen_width, gles_screen_height);
		printf("SCI: %d, %f\n", pvrrc.fb_X_CLIP.max, dc2s_scale_h);
		printf("SCI: %f, %f, %f, %f\n", offs_x+pvrrc.fb_X_CLIP.min/scale_x,(pvrrc.fb_Y_CLIP.min/scale_y)*dc2s_scale_h,(pvrrc.fb_X_CLIP.max-pvrrc.fb_X_CLIP.min+1)/scale_x*dc2s_scale_h,(pvrrc.fb_Y_CLIP.max-pvrrc.fb_Y_CLIP.min+1)/scale_y*dc2s_scale_h);
	#endif

   gl_state.scissor.x = offs_x+pvrrc.fb_X_CLIP.min/scale_x;
   gl_state.scissor.y = (pvrrc.fb_Y_CLIP.min/scale_y)*dc2s_scale_h;
   gl_state.scissor.w = (pvrrc.fb_X_CLIP.max-pvrrc.fb_X_CLIP.min+1)/scale_x*dc2s_scale_h;
   gl_state.scissor.h = (pvrrc.fb_Y_CLIP.max-pvrrc.fb_Y_CLIP.min+1)/scale_y*dc2s_scale_h;

   glScissor(gl_state.scissor.x, gl_state.scissor.y, gl_state.scissor.w, gl_state.scissor.h);

	if (settings.rend.WideScreen && pvrrc.fb_X_CLIP.min==0 && ((pvrrc.fb_X_CLIP.max+1)/scale_x==640) && (pvrrc.fb_Y_CLIP.min==0) && ((pvrrc.fb_Y_CLIP.max+1)/scale_y==480 ) )
	{
		glDisable(GL_SCISSOR_TEST);
      gl_state.cap_state[6] = 0;
	}
	else
   {
		glEnable(GL_SCISSOR_TEST);
      gl_state.cap_state[6] = 1;
   }

	//restore scale_x
	scale_x /= scissoring_scale_x;

	SetupMainVBO();
	//Draw the strips !

	//initial state
	glDisable(GL_BLEND);
   gl_state.cap_state[1] = 0;
	glEnable(GL_DEPTH_TEST);
   gl_state.cap_state[0] = 1;

	//We use sampler 0
	glActiveTexture(GL_TEXTURE0);

	//Opaque
	//Nothing extra needs to be setup here
	/*if (!GetAsyncKeyState(VK_F1))*/
	DrawList<ListType_Opaque,false>(pvrrc.global_param_op);

#if 0
	DrawModVols();
#endif

	//Alpha tested
	//setup alpha test state
	/*if (!GetAsyncKeyState(VK_F2))*/
	DrawList<ListType_Punch_Through,false>(pvrrc.global_param_pt);

	//Alpha blended
	//Setup blending
	glEnable(GL_BLEND);
   gl_state.cap_state[1] = 1;
   gl_state.blendfunc.sfactor = GL_SRC_ALPHA;
   gl_state.blendfunc.dfactor = GL_ONE_MINUS_SRC_ALPHA;
   glBlendFunc(gl_state.blendfunc.sfactor, gl_state.blendfunc.sfactor);
	/*if (!GetAsyncKeyState(VK_F3))*/
	{
		/*
		if (UsingAutoSort())
			SortRendPolyParamList(pvrrc.global_param_tr);
		else
			*/
#if TRIG_SORT
		if (pvrrc.isAutoSort)
			DrawSorted();
		else
			DrawList<ListType_Translucent,false>(pvrrc.global_param_tr);
#else
		if (pvrrc.isAutoSort)
			SortPParams();
		DrawList<ListType_Translucent,true>(pvrrc.global_param_tr);
#endif
	}

   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	KillTex=false;

	return !is_rtt;
}

/*
static bool rend_single_frame(void)
{
	//wait render start only if no frame pending
	_pvrrc = DequeueRender();

	while (!_pvrrc)
	{
		rs.Wait();
		_pvrrc = DequeueRender();
	}

	bool do_swp=false;
}
*/

void rend_set_fb_scale(float x,float y)
{
	fb_scale_x=x;
	fb_scale_y=y;
}

void co_dc_yield(void);

struct glesrend : Renderer
{
	bool Init()
   {
      libCore_vramlock_Init();
      return gles_init();
   }
	void Resize(int w, int h) { gles_screen_width=w; gles_screen_height=h; }
	void Term() { libCore_vramlock_Free(); }

	bool Process(TA_context* ctx) { return ProcessFrame(ctx); }
	bool Render()
   {
      unsigned i;

      glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
      glBlendFunc(gl_state.blendfunc.sfactor, gl_state.blendfunc.dfactor);
      glClearColor(gl_state.clear_color.r, gl_state.clear_color.g, gl_state.clear_color.b, gl_state.clear_color.a);
      glCullFace(gl_state.cullmode);
      glScissor(gl_state.scissor.x, gl_state.scissor.y, gl_state.scissor.w, gl_state.scissor.h);
      glUseProgram(gl_state.program);
      glViewport(gl_state.viewport.x, gl_state.viewport.y, gl_state.viewport.w, gl_state.viewport.h);
#ifdef CORE
      glBindVertexArray(vbo.vao);
#endif
      for(i = 0; i < SGL_CAP_MAX; i ++)
      {
         if (gl_state.cap_state[i])
            glEnable(gl_state.cap_translate[i]);
         else
            glDisable(gl_state.cap_translate[i]);
      }
      bool ret = RenderFrame();
   }

	void Present()
   {
      is_dupe = false;
      co_dc_yield();
   }

	virtual u32 GetTexture(TSP tsp, TCW tcw) {
		return gl_GetTexture(tsp, tcw);
	}
};

Renderer* rend_GLES2() { return new glesrend(); }
