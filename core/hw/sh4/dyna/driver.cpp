#include "types.h"
#include <unordered_set>

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/aica/aica_if.h"
#include "hw/gdrom/gdrom_if.h"

#include <time.h>
#include <float.h>

#include "blockmanager.h"
#include "ngen.h"
#include "decoder.h"

#if FEAT_SHREC != DYNAREC_NONE

u8 SH4_TCB[CODE_SIZE + TEMP_CODE_SIZE + 4096]
#if defined(_WIN32) || FEAT_SHREC != DYNAREC_JIT
	;
#elif defined(__linux__) || defined(__HAIKU__) || \
      defined(__FreeBSD__) || defined(__DragonFly__)
	__attribute__((section(".text")));
#elif defined(__MACH__)
	__attribute__((section("__TEXT,.text")));
#else
	#error SH4_TCB ALLOC
#endif

u8* CodeCache;
u8* TempCodeCache;
uintptr_t cc_rx_offset;

u32 LastAddr;
u32 LastAddr_min;
u32 TempLastAddr;
u32* emit_ptr=0;
u32* emit_ptr_limit;

std::unordered_set<u32> smc_hotspots;

void* emit_GetCCPtr(void)
{
   if (emit_ptr)
      return (void*)emit_ptr;
   return (void*)&CodeCache[LastAddr];
}
void emit_WriteCodeCache() {}

void emit_SetBaseAddr(void)
{
   LastAddr_min = LastAddr;
}

void RASDASD()
{
	LastAddr=LastAddr_min;
	memset(emit_GetCCPtr(),0xCC,emit_FreeSpace());
}

void clear_temp_cache(bool full)
{
	//printf("recSh4:Temp Code Cache clear at %08X\n", curr_pc);
	TempLastAddr = 0;
	bm_ResetTempCache(full);
}

static void recSh4_ClearCache(void)
{
#ifndef NDEBUG
	printf("recSh4:Dynarec Cache clear at %08X\n",curr_pc);
#endif
	LastAddr=LastAddr_min;
	bm_Reset();
	smc_hotspots.clear();
	clear_temp_cache(true);
}

static void recSh4_Run(void)
{
	sh4_int_bCpuRun=true;

	sh4_dyna_rcb=(u8*)&Sh4cntx + sizeof(Sh4cntx);
	//printf("cntx // fpcb offset: %td // pc offset: %td // pc %08X\n",(u8*)&sh4rcb.fpcb - sh4_dyna_rcb, (u8*)&sh4rcb.cntx.pc - sh4_dyna_rcb,sh4rcb.cntx.pc);
	//verify(rcb_noffs(&next_pc)==-184);
	ngen_mainloop(sh4_dyna_rcb);

	sh4_int_bCpuRun=false;
}

void emit_Write32(u32 data)
{
	if (emit_ptr)
	{
		*emit_ptr=data;
		emit_ptr++;
	}
	else
	{
		*(u32*)&CodeCache[LastAddr]=data;
		LastAddr+=4;
	}
}

void emit_Skip(u32 sz)
{
	if (emit_ptr)
		emit_ptr = (u32*)((u8*)emit_ptr + sz);
	else
		LastAddr+=sz;
}
u32 emit_FreeSpace()
{
	if (emit_ptr)
		return (emit_ptr_limit - emit_ptr) * sizeof(u32);
	else
		return CODE_SIZE-LastAddr;
}

// pc must be a physical address
bool DoCheck(u32 pc)
{
	if (IsOnRam(pc))
	{
		if (!settings.dynarec.unstable_opt)
			return true;

		pc&=0xFFFFFF;
		switch(pc)
		{
			//DOA2LE
			case 0x3DAFC6:
			case 0x3C83F8:

			//Shenmue 2
			case 0x348000:
				
			//Shenmue
			case 0x41860e:
			

				return true;

			default:
            break;
		}
	}
	return false;
}

void AnalyseBlock(RuntimeBlockInfo* blk);

char block_hash[1024];

#include "deps/crypto/sha1.h"

const char* RuntimeBlockInfo::hash(bool full, bool relocable)
{
	sha1_ctx ctx;
	sha1_init(&ctx);

	u8* ptr = GetMemPtr(this->addr,this->guest_opcodes*2);

	if (ptr)
	{
		if (relocable)
		{
			for (u32 i=0; i<this->guest_opcodes; i++)
			{
				u16 data=ptr[i];
				//Do not count PC relative loads (relocated code)
				if ((ptr[i]>>12)==0xD)
					data=0xD000;

				sha1_update(&ctx,2,(u8*)&data);
			}
		}
		else
		{
			sha1_update(&ctx,this->guest_opcodes*2,ptr);
		}
	}

	sha1_final(&ctx);

	if (full)
		sprintf(block_hash,">:%d:%08X:%02X:%08X:%08X:%08X:%08X:%08X",relocable,this->addr,this->guest_opcodes,ctx.digest[0],ctx.digest[1],ctx.digest[2],ctx.digest[3],ctx.digest[4]);
	else
		sprintf(block_hash,">:%d:%02X:%08X:%08X:%08X:%08X:%08X",relocable,this->guest_opcodes,ctx.digest[0],ctx.digest[1],ctx.digest[2],ctx.digest[3],ctx.digest[4]);

	//return ctx
	return block_hash;
}

bool RuntimeBlockInfo::Setup(u32 rpc,fpscr_t rfpu_cfg)
{
	staging_runs=addr=lookups=runs=host_code_size=0;
	guest_cycles=guest_opcodes=host_opcodes=0;
	pBranchBlock=pNextBlock=0;
	code=0;
	has_jcond=false;
	BranchBlock=NextBlock=csc_RetCache=0xFFFFFFFF;
	BlockType=BET_SCL_Intr;
	has_fpu_op = false;
	temp_block = false;
	
	vaddr=rpc;
	if (mmu_enabled())
	{
		u32 rv = mmu_instruction_translation(vaddr, addr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(vaddr, rv, MMU_TT_IREAD);
			return false;
		}
	}
	else
		addr = vaddr;
	fpu_cfg=rfpu_cfg;
	
	oplist.clear();

	if (!dec_DecodeBlock(this,SH4_TIMESLICE/2))
		return false;

	AnalyseBlock(this);

	return true;
}

DynarecCodeEntryPtr rdv_CompilePC(u32 blockcheck_failures)
{
	u32 pc=next_pc;

	if (emit_FreeSpace()<16*1024 || pc==0x8c0000e0 || pc==0xac010000 || pc==0xac008300)
		recSh4_ClearCache();

	RuntimeBlockInfo* rbi = ngen_AllocateBlock();

	if (!rbi->Setup(pc,fpscr))
	{
		delete rbi;
		return NULL;
	}
	rbi->blockcheck_failures = blockcheck_failures;
	if (smc_hotspots.find(rbi->addr) != smc_hotspots.end())
	{
		if (TEMP_CODE_SIZE - TempLastAddr < 16 * 1024)
			clear_temp_cache(false);
		emit_ptr = (u32 *)(TempCodeCache + TempLastAddr);
		emit_ptr_limit = (u32 *)(TempCodeCache + TEMP_CODE_SIZE);
		rbi->temp_block = true;
	}
		bool do_opts = !rbi->temp_block;
		rbi->staging_runs=do_opts?100:-100;
		ngen_Compile(rbi,DoCheck(rbi->addr),(pc&0xFFFFFF)==0x08300 || (pc&0xFFFFFF)==0x10000,false,do_opts);
		verify(rbi->code!=0);

		bm_AddBlock(rbi);

	if (emit_ptr != NULL)
	{
		TempLastAddr = (u8*)emit_ptr - TempCodeCache;
		emit_ptr = NULL;
		emit_ptr_limit = NULL;
	}

	return rbi->code;
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock_pc()
{
	return rdv_FailedToFindBlock(next_pc);
}

DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc)
{
	//printf("rdv_FailedToFindBlock ~ %08X\n",pc);
	next_pc=pc;
	DynarecCodeEntryPtr code = rdv_CompilePC(0);
	if (code == NULL)
		code = bm_GetCodeByVAddr(next_pc);
	else
		code = (DynarecCodeEntryPtr)CC_RW2RX(code);
	return code;
}

u32 DYNACALL rdv_DoInterrupts_pc(u32 pc) {
	next_pc = pc;
	UpdateINTC();

	return next_pc;
}

u32 DYNACALL rdv_DoInterrupts(void* block_cpde)
{
	RuntimeBlockInfo* rbi = bm_GetBlock2(block_cpde);
	return rdv_DoInterrupts_pc(rbi->vaddr);
}

// addr must be the physical address of the start of the block
DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr)
{
	u32 blockcheck_failures = 0;
	if (mmu_enabled())
	{
		RuntimeBlockInfo *block = bm_GetBlock(addr);
		blockcheck_failures = block->blockcheck_failures + 1;
		if (blockcheck_failures > 5)
		{
			bool inserted = smc_hotspots.insert(addr).second;
			//if (inserted)
			//	printf("rdv_BlockCheckFail SMC hotspot @ %08x fails %d\n", addr, blockcheck_failures);
		}
		bm_RemoveBlock(block);
	}
	else
	{
		next_pc = addr;
		recSh4_ClearCache();
	}
	return (DynarecCodeEntryPtr)CC_RW2RX(rdv_CompilePC(blockcheck_failures));
}

DynarecCodeEntryPtr rdv_FindOrCompile()
{
	DynarecCodeEntryPtr rv = bm_GetCodeByVAddr(next_pc);  // Returns exec addr
	if (rv == ngen_FailedToFindBlock)
		rv = (DynarecCodeEntryPtr)CC_RW2RX(rdv_CompilePC(0)); // Returns rw addr
	
	return rv;
}

void* DYNACALL rdv_LinkBlock(u8* code,u32 dpc)
{
	// code is the RX addr to return after, however bm_GetBlock returns RW
	RuntimeBlockInfo* rbi=bm_GetBlock2((void*)code);

	if (!rbi)
	{
#ifndef NDEBUG
		printf("Stale block ..");
#endif
		rbi=bm_GetStaleBlock(code);
	}
	
	verify(rbi != NULL);

	u32 bcls=BET_GET_CLS(rbi->BlockType);

	if (bcls==BET_CLS_Static)
	{
		next_pc=rbi->BranchBlock;
	}
	else if (bcls==BET_CLS_Dynamic)
	{
		next_pc=dpc;
	}
	else if (bcls==BET_CLS_COND)
	{
		if (dpc)
			next_pc=rbi->BranchBlock;
		else
			next_pc=rbi->NextBlock;
	}

	DynarecCodeEntryPtr rv = rdv_FindOrCompile();  // Returns rx ptr

	bool do_link = !mmu_enabled() && bm_GetBlock2((void*)code) == rbi;

	if (do_link)
	{
		if (bcls==BET_CLS_Dynamic)
		{
			verify(rbi->relink_data==0 || rbi->pBranchBlock==0);

			if (rbi->pBranchBlock!=0)
			{
				rbi->pBranchBlock->RemRef(rbi);
				rbi->pBranchBlock=0;
				rbi->relink_data=1;
			}
			else if (rbi->relink_data==0)
			{
				rbi->pBranchBlock=bm_GetBlock(next_pc);
				rbi->pBranchBlock->AddRef(rbi);
			}
		}
		else
		{
			RuntimeBlockInfo* nxt=bm_GetBlock(next_pc);

			if (rbi->BranchBlock==next_pc)
				rbi->pBranchBlock=nxt;
			if (rbi->NextBlock==next_pc)
				rbi->pNextBlock=nxt;

			nxt->AddRef(rbi);
		}
		u32 ncs=rbi->relink_offset+rbi->Relink();
		verify(rbi->host_code_size>=ncs);
		rbi->host_code_size=ncs;
	}
#ifndef NDEBUG
	else
	{
		printf(" .. null RBI: %08X -- unlinked stale block\n",next_pc);
	}
#endif
	
	return (void*)rv;
}

static void recSh4_Stop(void)
{
	Sh4_int_Stop();
}
static void recSh4_Start(void)
{
	Sh4_int_Start();
}

static void recSh4_Step(void)
{
	Sh4_int_Step();
}

static void recSh4_Skip(void)
{
	Sh4_int_Skip();
}

static void recSh4_Reset(bool Manual)
{
	Sh4_int_Reset(Manual);
	recSh4_ClearCache();
}

static void recSh4_Init(void)
{
	printf("recSh4 Init\n");
	Sh4_int_Init();
	bm_Init();
	bm_Reset();

#if 0
	verify(rcb_noffs(p_sh4rcb->fpcb) == FPCB_OFFSET);
#endif

	verify(rcb_noffs(p_sh4rcb->sq_buffer) == -512);

	verify(rcb_noffs(&p_sh4rcb->cntx.sh4_sched_next) == -152);
	verify(rcb_noffs(&p_sh4rcb->cntx.interrupt_pend) == -148);
	
	if (_nvmem_enabled())
	{
		if (!_nvmem_4gb_space())
		{
			verify(mem_b.data==((u8*)p_sh4rcb->sq_buffer+512+0x0C000000));
		}
		else
		{
			verify(mem_b.data==((u8*)p_sh4rcb->sq_buffer+512+0x8C000000));
		}
	}
	
	// Prepare some pointer to the pre-allocated code cache:
	void *candidate_ptr = (void*)(((unat)SH4_TCB + 4095) & ~4095);

	// Call the platform-specific magic to make the pages RWX
	CodeCache = NULL;
	#ifdef FEAT_NO_RWX_PAGES
	verify(vmem_platform_prepare_jit_block(candidate_ptr, CODE_SIZE + TEMP_CODE_SIZE, (void**)&CodeCache, &cc_rx_offset));
	#else
	verify(vmem_platform_prepare_jit_block(candidate_ptr, CODE_SIZE + TEMP_CODE_SIZE, (void**)&CodeCache));
	#endif
	// Ensure the pointer returned is non-null
	verify(CodeCache != NULL);

	memset(CodeCache, 0xFF, CODE_SIZE + TEMP_CODE_SIZE);
	TempCodeCache = CodeCache + CODE_SIZE;
	ngen_init();
	bm_Reset();
}

static void recSh4_Term(void)
{
	printf("recSh4 Term\n");
	bm_Term();
	Sh4_int_Term();
}

static bool recSh4_IsCpuRunning(void)
{
	return Sh4_int_IsCpuRunning();
}

void Get_Sh4Recompiler(sh4_if* rv)
{
	rv->Run = recSh4_Run;
	rv->Stop = recSh4_Stop;
	rv->Start = recSh4_Start;
	rv->Step = recSh4_Step;
	rv->Skip = recSh4_Skip;
	rv->Reset = recSh4_Reset;
	rv->Init = recSh4_Init;
	rv->Term = recSh4_Term;
	rv->IsCpuRunning = recSh4_IsCpuRunning;
	rv->ResetCache = recSh4_ClearCache;
}
#endif  // FEAT_SHREC != DYNAREC_NONE
