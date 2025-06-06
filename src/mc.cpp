#include "mc.h"
#include "line_placement.h"
#include "page_placement.h"
#include "os_placement.h"
#include "mem_ctrls.h"
#include "dramsim_mem_ctrl.h"
#include "ddr_mem.h"
#include "zsim.h"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <random>


MemoryController::MemoryController(g_string &name, uint32_t frequency, uint32_t domain, Config &config)
	: _name(name)
{
	// Trace Related
	_collect_trace = config.get<bool>("sys.mem.enableTrace", false);
	if (_collect_trace && _name == "mem-0")
	{
		_cur_trace_len = 0;
		_max_trace_len = 10000;
		_trace_dir = config.get<const char *>("sys.mem.traceDir", "./");
		// FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "wb");
		FILE *f = fopen((_trace_dir + _name + g_string("trace.txt")).c_str(), "w"); // 新增
		// uint32_t num = 0;
		fprintf(f, "cycle, address, type\n"); // 新增
		// fwrite(&num, sizeof(uint32_t), 1, f);
		fclose(f);
		futex_init(&_lock);
	}
	// 默认为false，cfg文件里也都未指定
	_sram_tag = config.get<bool>("sys.mem.sram_tag", false);
	_llc_latency = config.get<uint32_t>("sys.caches.l3.latency",4); // llc-latency = 4ns without l3
	double timing_scale = config.get<double>("sys.mem.dram_timing_scale", 1);
	g_string scheme = config.get<const char *>("sys.mem.cache_scheme", "NoCache");
	_ext_type = config.get<const char *>("sys.mem.ext_dram.type", "Simple");

	// following is revised by RL
	// global_memory_size = config.get<uint32_t>("sim.gmMBytes") * 1024 * 1024;
	// 这边无法获取到sim.gmMBytes的值

	if (scheme != "NoCache" && scheme != "Hybrid2" && scheme != "Bumblebee" && scheme !="DirectFlat" && scheme != "BATMAN")
	{
		_granularity = config.get<uint32_t>("sys.mem.mcdram.cache_granularity");
		_num_ways = config.get<uint32_t>("sys.mem.mcdram.num_ways");
		_mcdram_type = config.get<const char *>("sys.mem.mcdram.type", "Simple");
		_cache_size = config.get<uint32_t>("sys.mem.mcdram.size", 128) * 1024 * 1024;
	}
	if (scheme == "AlloyCache")
	{
		_scheme = AlloyCache;
		assert(_granularity == 64);
		assert(_num_ways == 1);
	}
	else if (scheme == "CacheMode")
	{
		_scheme = CacheMode;
	}
	
	else if (scheme == "UnisonCache")
	{
		assert(_granularity == 4096);
		_scheme = UnisonCache;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	}
	else if (scheme == "HMA")
	{
		assert(_granularity == 4096);
		assert(_num_ways == _cache_size / _granularity);
		_scheme = HMA;
	}
	else if (scheme == "HybridCache")
	{
		// 4KB page or 2MB page
		assert(_granularity == 4096 || _granularity == 4096 * 512);
		_scheme = HybridCache;
	}
	else if (scheme == "NoCache")
		_scheme = NoCache;
	else if (scheme == "CacheOnly")
		_scheme = CacheOnly;
	else if (scheme == "Tagless")
	{
		_scheme = Tagless;
		_next_evict_idx = 0;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	}
	else if (scheme == "Hybrid2")
	{ 
		_scheme = Hybrid2;
	}
	else if(scheme == "Bumblebee")
	{
		_scheme = Bumblebee;
	}
	else if(scheme == "DirectFlat")
	{
		_scheme = DirectFlat;
	} 
	else if (scheme == "BATMAN")
	{
		_scheme = BATMAN;
	}
	else
	{
		printf("scheme=%s\n", scheme.c_str());
		assert(false);
	}

	g_string placement_scheme = config.get<const char *>("sys.mem.mcdram.placementPolicy", "LRU");
	_bw_balance = config.get<bool>("sys.mem.bwBalance", false);
	_ds_index = 0;
	if (_bw_balance)
		assert(_scheme == AlloyCache || _scheme == HybridCache);

	// Configure the external Dram
	g_string ext_dram_name = _name + g_string("-ext");
	if (_ext_type == "Simple")
	{
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		_ext_dram = (SimpleMemory *)gm_malloc(sizeof(SimpleMemory));
		new (_ext_dram) SimpleMemory(latency, ext_dram_name, config);
	}
	else if (_ext_type == "DDR")
		_ext_dram = BuildDDRMemory(config, frequency, domain, ext_dram_name, "sys.mem.ext_dram.", 4, 1.0);
	else if (_ext_type == "MD1")
	{
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		uint32_t bandwidth = config.get<uint32_t>("sys.mem.ext_dram.bandwidth", 6400);
		_ext_dram = (MD1Memory *)gm_malloc(sizeof(MD1Memory));
		new (_ext_dram) MD1Memory(64, frequency, bandwidth, latency, ext_dram_name);
	}
	else if (_ext_type == "DRAMSim")
	{
		uint64_t cpuFreqHz = 1000000 * frequency;
		uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
		string dramTechIni = config.get<const char *>("sys.mem.techIni");
		string dramSystemIni = config.get<const char *>("sys.mem.systemIni");
		string outputDir = config.get<const char *>("sys.mem.outputDir");
		string traceName = config.get<const char *>("sys.mem.traceName", "dramsim");
		traceName += "_ext";
		_ext_dram = (DRAMSimMemory *)gm_malloc(sizeof(DRAMSimMemory));
		uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		new (_ext_dram) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
	}
	else
		panic("Invalid memory controller type %s", _ext_type.c_str());

	// following is revised by RL
	if (_scheme != NoCache && _scheme != Hybrid2 && _scheme != Bumblebee && _scheme != DirectFlat  && _scheme != BATMAN)
	{
		// Configure the MC-Dram (Timing Model)
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		//_mcdram = new MemObject * [_mcdram_per_mc];
		_mcdram = (MemObject **)gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			// g_string mcdram_name(ss.str().c_str());
			if (_mcdram_type == "Simple")
			{
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				_mcdram[i] = (SimpleMemory *)gm_malloc(sizeof(SimpleMemory));
				new (_mcdram[i]) SimpleMemory(latency, mcdram_name, config);
				//_mcdram[i] = new SimpleMemory(latency, mcdram_name, config);
			}
			else if (_mcdram_type == "DDR")
			{
				// XXX HACK tBL for mcdram is 1, so for data access, should multiply by 2, for tad access, should multiply by 3.
				_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
			}
			else if (_mcdram_type == "MD1")
			{
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				uint32_t bandwidth = config.get<uint32_t>("sys.mem.mcdram.bandwidth", 12800);
				_mcdram[i] = (MD1Memory *)gm_malloc(sizeof(MD1Memory));
				new (_mcdram[i]) MD1Memory(64, frequency, bandwidth, latency, mcdram_name);
			}
			else if (_mcdram_type == "DRAMSim")
			{
				uint64_t cpuFreqHz = 1000000 * frequency;
				uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
				string dramTechIni = config.get<const char *>("sys.mem.techIni");
				string dramSystemIni = config.get<const char *>("sys.mem.systemIni");
				string outputDir = config.get<const char *>("sys.mem.outputDir");
				string traceName = config.get<const char *>("sys.mem.traceName");
				traceName += "_mc";
				traceName += to_string(i);
				_mcdram[i] = (DRAMSimMemory *)gm_malloc(sizeof(DRAMSimMemory));
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				new (_mcdram[i]) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
			}
			else
				panic("Invalid memory controller type %s", _mcdram_type.c_str());
		}
		// Configure MC-Dram Functional Model
		// 默认的ways为1
		_num_sets = _cache_size / _num_ways / _granularity;
		if (_scheme == Tagless)
			assert(_num_sets == 1);
		_cache = (Set *)gm_malloc(sizeof(Set) * _num_sets);
		for (uint64_t i = 0; i < _num_sets; i++)
		{
			_cache[i].ways = (Way *)gm_malloc(sizeof(Way) * _num_ways);
			_cache[i].num_ways = _num_ways;
			for (uint32_t j = 0; j < _num_ways; j++)
				_cache[i].ways[j].valid = false;
		}
		// std::cout << "Allocating is allow!" << std::endl;
		if (_scheme == AlloyCache || _scheme == CacheMode)
		{
			_line_placement_policy = (LinePlacementPolicy *)gm_malloc(sizeof(LinePlacementPolicy));
			new (_line_placement_policy) LinePlacementPolicy();
			_line_placement_policy->initialize(config);
		}
		else if (_scheme == HMA)
		{
			_os_placement_policy = (OSPlacementPolicy *)gm_malloc(sizeof(OSPlacementPolicy));
			new (_os_placement_policy) OSPlacementPolicy(this);
		}
		else if (_scheme == UnisonCache || _scheme == HybridCache)
		{
			_page_placement_policy = (PagePlacementPolicy *)gm_malloc(sizeof(PagePlacementPolicy));
			new (_page_placement_policy) PagePlacementPolicy(this);
			_page_placement_policy->initialize(config);
		}
	}
	if (_scheme == HybridCache)
	{
		_tag_buffer = (TagBuffer *)gm_malloc(sizeof(TagBuffer));
		new (_tag_buffer) TagBuffer(config);
	}
	if (_scheme == Hybrid2)
	{ 
		// futex_init(&_AsynQueuelock);
		// 【newAddition】 新增Hybrid2。此处代码接收2种类型的参数
		// 这样的设计就只有通道没有伪通道的概念
		// HBM通道数设置，按照道理来说应该是需要保持一致的
		_cache_hbm_per_mc = config.get<uint32_t>("sys.mem.cachehbm.cacheHBMPerMC", 4);
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		// 用作cache的HBM和用作memory的HBM设置
		// _cachehbm = (MemObject **) gm_malloc(sizeof(MemObject *) * _cache_hbm_per_mc);
		// _memhbm = (MemObject **) gm_malloc(sizeof(MemObject *) * _mem_hbm_per_mc);
		// 把大小也传进来,主要是传进来memhbm大小，这样可以根据lineAddr判断在哪一个内存介质
		_cache_hbm_size = config.get<uint32_t>("sys.mem.cachehbm.size", 64) * 1024 * 1024; // Default:64MB
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size", 1024) * 1024 * 1024;   // Default:1GB
		_cache_hbm_type = config.get<const char *>("sys.mem.cachehbm.type", "DDR");
		_mem_hbm_type = config.get<const char *>("sys.mem.memhbm.type", "DDR");

		// 所有_memhbm被替换为_mcdram 以避免未知错误
		// 请注意将_mcdram和_mchbm的配置文件进行统一，以避免不会暴露的bug
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **)gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}

		// // 目前假定这里的hbm的type都是ddr类型,循环创建memHBM
		// for (uint32_t i = 0; i < _mem_hbm_per_mc ; i++){
		// 	g_string memhbm_name = _name + g_string("-memhbm-") + g_string(to_string(i).c_str());
		// 	_memhbm[i] = BuildDDRMemory(config, frequency, domain, memhbm_name, "sys.mem.memdram.", 4, timing_scale);

		// }

		// assert(_memhbm[0] != nullptr);
		// std::cout << "_memhbm[0] =========" << _memhbm[0] << std::endl;

		// 这里使用std::vector存储XTAEntry
		// XTAEntries的数量为set的数量
		// set的数量计算公式为_cache_hbm_size / (set_assoc_num * _hybrid2_page_size)
		// 即以page为粒度管理
		// 问题求解逻辑为 CacheHBM大小 一个set 可以映射 set_assoc_num 个 page-size大小的page
		// 问你需要多少个set
		_hybrid2_page_size = config.get<uint32_t>("sys.mem.pagesize", 4) * 1024; // in Bytes
		_hybrid2_blk_size = config.get<uint32_t>("sys.mem.blksize", 64);		 // in Bytes
		assert(_hybrid2_blk_size != 0);
		hybrid2_blk_per_page = _hybrid2_page_size / _hybrid2_blk_size;		// Default = 64
		set_assoc_num = config.get<uint32_t>("sys.mem.cachehbm.setnum", 8); // Default:8
		assert(set_assoc_num * _hybrid2_page_size != 0);
		hbm_set_num = _cache_hbm_size / (set_assoc_num * _hybrid2_page_size);
		hbm_pages_per_set = _mem_hbm_size / _hybrid2_page_size / set_assoc_num;
		// hybrid2_blk_per_page = _hybrid2_page_size / _hybrid2_blk_size;

		// 推荐不在config里修改，在这里修改即可
		phy_mem_size = config.get<uint64_t>("sys.mem.totalSize",9)*1024*1024*1024;
		
		num_pages = phy_mem_size / _hybrid2_page_size;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}

		// 循环创建XTAEntry
		assert(hbm_set_num > 0);
		for (uint64_t i = 0; i < hbm_set_num; ++i)
		{
			// 初始化一个set对应的XTAEntries,共有hbm_set_num个
			std::vector<XTAEntry> entries; // xxx??? 

			// 循环初始化XTAEntry,一个set固定set_assoc_num个page
			for (uint64_t j = 0; j < set_assoc_num; j++)
			{
				XTAEntry tmp_entry;
				tmp_entry._hybrid2_tag = 0;
				tmp_entry._hbm_tag = 0;
				tmp_entry._dram_tag = 0;
				tmp_entry._hybrid2_LRU = 0; // LRU的逻辑应该有其它的设置方式，目前暂时先不考虑
				tmp_entry._hybrid2_counter = 0;
				// 还剩下2个vector需要设置，先对指针数组初始化
				// tmp_entry.bit_vector = (uint64_t*)malloc(hybrid2_blk_per_page * sizeof(uint64_t));
				// tmp_entry.dirty_vector = (uint64_t*)malloc(hybrid2_blk_per_page * sizeof(uint64_t));
				// tmp_entry.bit_vector = new uint64_t[hybrid2_blk_per_page];
				// tmp_entry.dirty_vector = new uint64_t[hybrid2_blk_per_page];
				for (uint64_t k = 0; k < (uint64_t)hybrid2_blk_per_page; k++)
				{
					tmp_entry.bit_vector[k] = 0;
					tmp_entry.dirty_vector[k] = 0;
				}
				// 假设组相联数为8 这里会将8个页面对应的XTAEntry加入XTAEntries(SetEntries更准确一点)
				entries.push_back(tmp_entry);
			}
			// SetEntries加入XTA
			XTA.push_back(entries);
		}

		// 循环初始化DRAM HBM内存占用情况 [暂时不想用这个]
		// for(uint64_t i=0; i < hbm_set_num; i++)
		// {
		// 	std::vector<int> SETEntries_occupied;
		// 	memory_occupied.push_back(SETEntries_occupied);
		// 	// 按理来说是还有dram—_pages_per_set的 但是这里假设了DRAM空间无限大，怎么写需要考虑，TODO
		// 	// 暂时设置一个DRAM大小比例吧，后续可调
		// 	int x = 8;
		// 	for(uint64_t j = 0; j < (1+x)*hbm_pages_per_set ; j++)
		// 	{
		// 		memory_occupied[i].push_back(0);
		// 	}
		// }
	}
	

	// if (_scheme == Chameleon){
	// 	_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
	// 	_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size",1024)*1024*1024;// Default:1GB
	// 	_mem_hbm_type = config.get<const char*>("sys.mem.memhbm.type","DDR");
	// 	_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
	// 	_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
	// 	for (uint32_t i = 0; i < _mcdram_per_mc; i++)
	// 	{
	// 		g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
	// 		_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
	// 	}
	// 	phy_mem_size = config.get<uint32_t>("sys.mem.totalSize",9)*1024*1024*1024;
	// 	_chameleon_blk_size=config.get<uint64_t>("sys.mem.mcdram.blksize",64);

	// 	int ddrRatio = (int)(phy_mem_size - _mem_hbm_size) / _mem_hbm_size;
	// 	// 要多少segment
	// 	// 估算元数据开销：segGrpEntry一个接近4B 1GB/64B*4B = 64MB
	// 	uint32_t _segment_number =	_mem_hbm_size / _chameleon_blk_size;
	// 	// 初始化
	// 	for(uint32_t i = 0; i<_segment_number ; i++)
	// 	{
	// 		segGrpEntry tmpEntry(ddrRatio);
	// 		segGrps.push_back(tmpEntry);
	// 	}

	// }

	if(_scheme == Bumblebee){
		// std::cout << "Start Initialization!\n" << std::endl;
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size",1024)*1024*1024;// Default:1GB
		_mem_hbm_type = config.get<const char*>("sys.mem.memhbm.type","DDR");
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}
		phy_mem_size = config.get<uint32_t>("sys.mem.totalSize",9)*1024*1024*1024;
		_bumblebee_blk_size = config.get<uint32_t>("sys.mem.bumblebee.blksize", 64);
		_bumblebee_page_size =  config.get<uint32_t>("sys.mem.bumblebee.pagesize", 4)*1024;


		uint32_t set_nums = _mem_hbm_size / bumblebee_n / _bumblebee_page_size;
		for(uint32_t i = 0 ; i < set_nums;i++)
		{
			MetaGrpEntry tmpEntry;
			MetaGrp.push_back(tmpEntry);
			HotnenssTracker hotTracker;
			HotnessTable.push_back(hotTracker);
		}

		num_pages = phy_mem_size / _bumblebee_page_size;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}

		// futex_init(&_AsynQueuelock);
	}

	if(_scheme == DirectFlat)
	{
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size",1024)*1024*1024;// Default:1GB
		_mem_hbm_type = config.get<const char*>("sys.mem.memhbm.type","DDR");
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}
		phy_mem_size = config.get<uint32_t>("sys.mem.totalSize",9)*1024*1024*1024;
		_bumblebee_page_size = config.get<uint32_t>("sys.mem.bumblebee.pagesize", 4)*1024;
		num_pages = phy_mem_size / _bumblebee_page_size;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}
		// 测试访问分布,9G，分为9个区域，每个区域1G
		_flat_access_cntr.resize(9, 0);
	}

	if(_scheme == BATMAN)
	{
		_mem_hbm_per_mc = config.get<uint32_t>("sys.mem.memhbm.memHBMPerMC", 4);
		_mem_hbm_size = config.get<uint32_t>("sys.mem.memhbm.size",1024)*1024*1024;// Default:1GB
		_mem_hbm_type = config.get<const char*>("sys.mem.memhbm.type","DDR");
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		{
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
			_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 1, timing_scale);
		}
		phy_mem_size = config.get<uint32_t>("sys.mem.totalSize",9)*1024*1024*1024;

		_batman_blk_size = config.get<uint32_t>("sys.mem.batman.blksize", 64);
		_batman_page_size =  config.get<uint32_t>("sys.mem.batman.pagesize", 4)*1024;

		batman_set_nums = _mem_hbm_size / _batman_page_size;
		for(int i = 0;i < batman_set_nums;i++)
		{
			batman_set b_set;
			b_sets.push_back(b_set);
		}

		num_pages = phy_mem_size / _batman_page_size;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}

		lst_md_cycle = 0;
	}

	if(_scheme == NoCache || _scheme == CacheOnly)
	{
		num_pages =  9*1024*1024 / 4;
		for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
		{
			fixedMapping[vpgnum] = vpgnum % num_pages;
		}
	}
	
	num_pages =  9*1024*1024 / 4; 
	for (uint64_t vpgnum = 0; vpgnum < num_pages; ++vpgnum)
	{
		fixedMapping[vpgnum] = vpgnum % num_pages;
	}

	// Stats
	_num_hit_per_step = 0;
	_num_miss_per_step = 0;
	_mc_bw_per_step = 0;
	_ext_bw_per_step = 0;
	for (uint32_t i = 0; i < MAX_STEPS; i++)
		_miss_rate_trace[i] = 0;
	_num_requests = 0;
	// std::cout << "Init is done!\n" << std::endl;
}

uint64_t
MemoryController::access(MemReq &req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	if (req.type == PUTS)
		return req.cycle;
	futex_lock(&_lock);
	// ignore clean LLC eviction
	if (_collect_trace && _name == "mem-0")
	{
		_address_trace[_cur_trace_len] = req.lineAddr;
		_cycle_trace[_cur_trace_len] = req.cycle;
		_type_trace[_cur_trace_len] = (req.type == PUTX) ? 1 : 0;
		_cur_trace_len++;
		assert(_cur_trace_len <= _max_trace_len);
		if (_cur_trace_len == _max_trace_len)
		{
			// FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "ab");
			FILE *f = fopen((_trace_dir + _name + g_string("trace.txt")).c_str(), "a"); // 使用 "a" 以追加模式打开文件
			for (size_t i = 0; i < _max_trace_len; i++)
			{
				fprintf(f, "%lu, %lx, %u\n", _cycle_trace[i], _address_trace[i], _type_trace[i]);
			}
			// fwrite(_address_trace, sizeof(Address), _max_trace_len, f);
			// fwrite(_type_trace, sizeof(uint32_t), _max_trace_len, f);
			fclose(f);
			_cur_trace_len = 0;
		}
	}

	_num_requests++;
	
	if (_scheme == NoCache)
	{
		///////   load from external dram
		Address tmp_addr = req.lineAddr;
		req.lineAddr = vaddr_to_paddr(req);
		req.cycle = _ext_dram->access(req, 0, 4);
		req.lineAddr = tmp_addr;
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
		////////////////////////////////////
	}
	if (_scheme == Hybrid2)
	{
		// 请勿在同一个调用链里调用处理req的请求两次！
		// std::cout << "Still ret req.cycle  00000  = " << hbm_hybrid2_access(req) <<std::endl;
		req.cycle = hybrid2_access(req);
		// std::cout << "Still ret req.cycle    11111 = " << req.cycle <<std::endl;
		// futex_unlock(&_lock); // 此处重复释放锁，涉及到锁的一致性问题。
		return req.cycle;
	}

	if (_scheme == Bumblebee)
	{
		req.cycle = bumblebee_access(req);
		return req.cycle;
	}

	if(_scheme == DirectFlat)
	{
		req.cycle = direct_flat_access(req);
		return req.cycle;
	}

	if(_scheme == BATMAN)
	{
		req.cycle = batman_access(req);
		return req.cycle;
	}

	// if(_scheme==Chameleon)
	// {
	// 	// 请勿在同一个调用链里调用处理req的请求两次！
	// 	// std::cout << "Still ret req.cycle  00000  = " << hbm_hybrid2_access(req) <<std::endl;
	// 	req.cycle = chameleon_access(req);
	// 	// std::cout << "Still ret req.cycle    11111 = " << req.cycle <<std::endl;
	// 	// futex_unlock(&_lock); // 此处重复释放锁，涉及到锁的一致性问题。
	// 	return req.cycle;
	// }

	/////////////////////////////
	// TODO For UnisonCache
	// should correctly model way accesses
	/////////////////////////////


	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	Address initial_req_addr = req.lineAddr;
	Address address = vaddr_to_paddr(req);
	// Address address = req.lineAddr;
	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
	Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64);
	// printf("address=%ld, _mcdram_per_mc=%d, mc_address=%ld\n", address, _mcdram_per_mc, mc_address);
	Address tag = address / (_granularity / 64);
	uint64_t set_num = tag % _num_sets;
	uint32_t hit_way = _num_ways;
	// uint64_t orig_cycle = req.cycle;
	uint64_t data_ready_cycle = req.cycle;
	MESIState state;
	
	if (_scheme == CacheOnly)
	{
		///////   load from mcdram
		// std::cout << "Channel Select = " << mcdram_select << "  |||  CacheOnly req.lineAddr = " << req.lineAddr << std::endl;
		Address tmp_address = vaddr_to_paddr(req);
		uint32_t mcdram_select = (tmp_address / 64) % _mcdram_per_mc;
		Address mc_address = (tmp_address / 64 / _mcdram_per_mc * 64) | (tmp_address % 64);
		// mc_address = (address  / _mcdram_per_mc) | address;
		// mcdram_select = address  % _mcdram_per_mc;
		req.lineAddr = mc_address;
		req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
		req.lineAddr = initial_req_addr;
		_numLoadHit.inc();
		futex_unlock(&_lock);
		// std::cout << "CacheOnly is working"<<std::endl;
		return req.cycle;
		
		////////////////////////////////////
	}

	req.lineAddr = address;

	// cacheonly模式下，HBM大于2GB时需要以下命令清除缓冲区，否则会报错
	// std::cout << std::endl;
	// ????
	uint64_t step_length = _cache_size / 64 / 10;

	// whether needs to probe tag for HybridCache.
	// need to do so for LLC dirty eviction and if the page is not in TB
	bool hybrid_tag_probe = false;
	if (_granularity >= 4096)
	{
		if (_tlb.find(tag) == _tlb.end())
			_tlb[tag] = TLBEntry{tag, _num_ways, 0, 0, 0};
		if (_tlb[tag].way != _num_ways)
		{
			hit_way = _tlb[tag].way;
			assert(_cache[set_num].ways[hit_way].valid && _cache[set_num].ways[hit_way].tag == tag);
		}
		else if (_scheme != Tagless)
		{
			// for Tagless, this assertion takes too much time.
			for (uint32_t i = 0; i < _num_ways; i++)
				assert(_cache[set_num].ways[i].tag != tag || !_cache[set_num].ways[i].valid);
		}

		if (_scheme == UnisonCache)
		{
			//// Tag and data access. For simplicity, use a single access.
			if (type == LOAD)
			{
				req.lineAddr = mc_address; // transMCAddressPage(set_num, 0); //mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
				// req.lineAddr = initial_req_addr; // add by jhlu 
			}
			else
			{
				assert(type == STORE);
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
			}
			///////////////////////////////
		}
		if (_scheme == HybridCache && type == STORE)
		{
			if (_tag_buffer->existInTB(tag) == _tag_buffer->getNumWays() && set_num >= _ds_index)
			{
				_numTBDirtyMiss.inc();
				if (!_sram_tag)
					hybrid_tag_probe = true;
			}
			else
				_numTBDirtyHit.inc();
		}
		if (_scheme == HybridCache && _sram_tag)
			req.cycle += _llc_latency;
	}
	else if (_scheme == AlloyCache)
	{ // 这里从else 变为 else if
		// assert(_scheme == AlloyCache);
		if (_cache[set_num].ways[0].valid && _cache[set_num].ways[0].tag == tag && set_num >= _ds_index)
			hit_way = 0;
		if (type == LOAD && set_num >= _ds_index)
		{
			///// mcdram TAD access
			// Modeling TAD as 2 cachelines
			if (_sram_tag)
			{
				req.cycle += _llc_latency;
				/*				if (hit_way == 0) {
									req.lineAddr = mc_address;
									req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
									_mc_bw_per_step += 4;
									_numTagLoad.inc();
									req.lineAddr = address;
								}
				*/
			}
			else
			{
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
				// req.lineAddr = initial_req_addr; // add by jhlu
			}
			///////////////////////////////
		}
	}
	else if(_scheme == CacheMode)
	{
		if (_cache[set_num].ways[0].valid && _cache[set_num].ways[0].tag == tag && set_num >= _ds_index)
			hit_way = 0;
		if (set_num >= _ds_index)
		{
		//判断是否有sram tag，有的话则访问llc
			assert(type == LOAD || type == STORE);
			if (_sram_tag)
			{
				req.cycle += _llc_latency;
			}
			else
			{
				// 朴素cache需要先访问HBM获取tag
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
				
				// req.lineAddr = mc_address;
				// req.cycle = _mcdram[mcdram_select]->access(req, 1, 4);
				// _mc_bw_per_step += 4;
				// _numTagLoad.inc();
				// req.lineAddr = address;
			}
		}
	}
	else
	{
		// 这里目前是只能是hybrid2,暂时先assert
		// 按理来说这段代码应该解耦出去，但是不知道有哪些地方调用了这个access，改动代价较高
		// 但是也可以尝试
		// 解锁操作移动到hybrid2_access
		// assert(_scheme == Hybrid2);
		// // 重新写一个访问函数
		// return hybrid2_access(req);
	}
	bool cache_hit = hit_way != _num_ways;

	// orig_cycle = req.cycle;
	//  dram cache logic. Here, I'm assuming the 4 mcdram channels are
	//  organized centrally
	bool counter_access = false;
	// use the following state for requests, so that req.state is not changed
	if (!cache_hit)
	{
		uint64_t cur_cycle = req.cycle;
		_num_miss_per_step++;
		if (type == LOAD)
			_numLoadMiss.inc();
		else
			_numStoreMiss.inc();

		uint32_t replace_way = _num_ways;
		if (_scheme == AlloyCache || _scheme == CacheMode)
		{
			bool place = false;
			if (set_num >= _ds_index)
				place = _line_placement_policy->handleCacheMiss(&_cache[set_num].ways[0]);
			replace_way = place ? 0 : 1;
		}
		else if (_scheme == HMA)
			_os_placement_policy->handleCacheAccess(tag, type);
		else if (_scheme == Tagless)
		{
			replace_way = _next_evict_idx;
			_next_evict_idx = (_next_evict_idx + 1) % _num_ways;
		}
		else
		{
			if (set_num >= _ds_index)
				replace_way = _page_placement_policy->handleCacheMiss(tag, type, set_num, &_cache[set_num], counter_access);
		}

		/////// load from external dram
		if (_scheme == AlloyCache)
		{
			if (type == LOAD)
			{
				if (!_sram_tag && set_num >= _ds_index)
					req.cycle = _ext_dram->access(req, 1, 4);
				else
					req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else if (type == STORE && replace_way >= _num_ways)
			{
				// no replacement
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else if (type == STORE)
			{ // && replace_way < _num_ways)
			
				MemReq load_req = {address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _ext_dram->access(load_req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == CacheMode)
		{
			if (type == STORE && replace_way < _num_ways)
			{
				//replacement
				//把要修改的读到cache中
				MemReq load_req = {address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _ext_dram->access(load_req, 1, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else
			{ // not replace
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		}
		
		else if (_scheme == HMA)
		{
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		}
		else if (_scheme == UnisonCache)
		{
			if (type == LOAD)
			{
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			}
			else if (type == STORE && replace_way >= _num_ways)
			{
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			}
			data_ready_cycle = req.cycle;
		}
		else if (_scheme == HybridCache)
		{
			if (hybrid_tag_probe)
			{
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
				_numTagLoad.inc();
				data_ready_cycle = req.cycle;
			}
			else
			{
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == Tagless)
		{
			assert(_ext_dram);
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		}
		////////////////////////////////////

		if (replace_way < _num_ways)
		{
			///// mcdram replacement
			// TODO update the address
			if (_scheme == AlloyCache || _scheme == CacheMode)
			{
				//写到HBM
				MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				//判断tag是不是在sram上，不在则需要把tag和数据一起写入到HBM，burst+2
				// 因为粒度为64B 不需要再从dram load到cache再写，cache目前是有数据的
				uint32_t size = _sram_tag ? 4 : 6;
				_mcdram[mcdram_select]->access(insert_req, 2, size);
				_mc_bw_per_step += size;
				_numTagStore.inc();
			}
			else if (_scheme == UnisonCache || _scheme == HybridCache || _scheme == Tagless)
			{
				uint32_t access_size = (_scheme == UnisonCache || _scheme == Tagless) ? _footprint_size : (_granularity / 64);
				// load page from ext dram
				MemReq load_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_ext_dram->access(load_req, 2, access_size * 4);
				_ext_bw_per_step += access_size * 4;
				// store the page to mcdram
				MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_mcdram[mcdram_select]->access(insert_req, 2, access_size * 4);
				_mc_bw_per_step += access_size * 4;
				if (_scheme == Tagless)
				{
					MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					_ext_dram->access(load_gipt_req, 2, 2);	 // update GIPT
					_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
					_ext_bw_per_step += 4;
				}
				else if (!_sram_tag)
				{
					_mcdram[mcdram_select]->access(insert_req, 2, 2); // store tag
					_mc_bw_per_step += 2;
				}
				_numTagStore.inc();
			}

			///////////////////////////////
			_numPlacement.inc();
			if (_cache[set_num].ways[replace_way].valid)
			{
				Address replaced_tag = _cache[set_num].ways[replace_way].tag;
				// Note that tag_buffer is not updated if placed into an invalid entry.
				// this is like ignoring the initialization cost
				if (_scheme == HybridCache)
				{
					// Update TagBuffer
					// if (!_tag_buffer->canInsert(tag, replaced_tag)) {
					//	printf("!!!!!!Occupancy = %f\n", _tag_buffer->getOccupancy());
					//	_tag_buffer->clearTagBuffer();
					//	_numTagBufferFlush.inc();
					//}
					// assert (_tag_buffer->canInsert(tag, replaced_tag));
					assert(_tag_buffer->canInsert(tag, replaced_tag));
					{
						_tag_buffer->insert(tag, true);
						_tag_buffer->insert(replaced_tag, true);
					}
					// else {
					//	goto end;
					// }
				}

				_tlb[replaced_tag].way = _num_ways;
				// only used for UnisonCache
				uint32_t unison_dirty_lines = __builtin_popcountll(_tlb[replaced_tag].dirty_bitvec) * 4;
				uint32_t unison_touch_lines = __builtin_popcountll(_tlb[replaced_tag].touch_bitvec) * 4;
				if (_scheme == UnisonCache || _scheme == Tagless)
				{
					assert(unison_touch_lines > 0);
					assert(unison_touch_lines <= 64);
					assert(unison_dirty_lines <= 64);
					_numTouchedLines.inc(unison_touch_lines);
					_numEvictedLines.inc(unison_dirty_lines);
				}

				if (_cache[set_num].ways[replace_way].dirty)
				{
					_numDirtyEviction.inc();
					///////   store dirty line back to external dram
					// Store starts after TAD is loaded.
					// request not on critical path.
					if (_scheme == AlloyCache)
					{
						if (type == STORE)
						{
							if (_sram_tag)
							{
								MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
								req.cycle = _mcdram[mcdram_select]->access(load_req, 2, 4);
								_mc_bw_per_step += 4;
								//_numTagLoad.inc();
							}
						}
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, 4);
						_ext_bw_per_step += 4;
					}
					else if (_scheme == CacheMode)
					{
						// 读取HBM目前的数据
						MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						req.cycle = _mcdram[mcdram_select]->access(load_req, 2, 4);
						_mc_bw_per_step += 4;
						//写回DRAM中
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, 4);
						_ext_bw_per_step += 4;
					}
					
					else if (_scheme == HybridCache)
					{
						// load page from mcdram
						MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, (_granularity / 64) * 4);
						_mc_bw_per_step += (_granularity / 64) * 4;
						// store page to ext dram
						// TODO. this event should be appended under the one above.
						// but they are parallel right now.
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, (_granularity / 64) * 4);
						_ext_bw_per_step += (_granularity / 64) * 4;
					}
					else if (_scheme == UnisonCache || _scheme == Tagless)
					{
						assert(unison_dirty_lines > 0);
						// load page from mcdram
						assert(unison_dirty_lines <= 64);
						MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, unison_dirty_lines * 4);
						_mc_bw_per_step += unison_dirty_lines * 4;
						// store page to ext dram
						// TODO. this event should be appended under the one above.
						// but they are parallel right now.
						MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, unison_dirty_lines * 4);
						_ext_bw_per_step += unison_dirty_lines * 4;
						if (_scheme == Tagless)
						{
							MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(load_gipt_req, 2, 2);	 // update GIPT
							_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
							_ext_bw_per_step += 4;
						}
					}

					/////////////////////////////
				}
				else
				{
					_numCleanEviction.inc();
					if (_scheme == UnisonCache || _scheme == Tagless)
						assert(unison_dirty_lines == 0);
				}
			}
			_cache[set_num].ways[replace_way].valid = true;
			_cache[set_num].ways[replace_way].tag = tag;
			_cache[set_num].ways[replace_way].dirty = (req.type == PUTX);
			_tlb[tag].way = replace_way;
			if (_scheme == UnisonCache || _scheme == Tagless)
			{
				uint64_t bit = (address - tag * 64) / 4;
				assert(bit < 16 && bit >= 0);
				bit = ((uint64_t)1UL) << bit;
				_tlb[tag].touch_bitvec = 0;
				_tlb[tag].dirty_bitvec = 0;
				_tlb[tag].touch_bitvec |= bit;
				if (type == STORE)
					_tlb[tag].dirty_bitvec |= bit;
			}
		}
		else
		{
			// Miss but no replacement
			if (_scheme == HybridCache)
				if (type == LOAD && _tag_buffer->canInsert(tag))
					_tag_buffer->insert(tag, false);
			assert(_scheme != Tagless)
		}
	}
	else
	{ // cache_hit == true
		assert(set_num >= _ds_index);
		if (_scheme == AlloyCache)
		{
			if (type == LOAD && _sram_tag)
			{
				MemReq read_req = {mc_address, GETX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(read_req, 0, 4);
				_mc_bw_per_step += 4;
			}
			if (type == STORE)
			{
				// LLC dirty eviction hit
				MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(write_req, 0, 4);
				_mc_bw_per_step += 4;
			}
		}
		else if (_scheme == CacheMode){
			// 朴素cache需要先读取tag，再进行访问，因此这边的请求需要在tag读取后进行
			if (type == LOAD)
			{
				MemReq read_req = {mc_address, GETX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(read_req, 1, 4);
				_mc_bw_per_step += 4;
			}
			if (type == STORE)
			{
				// LLC dirty eviction hit
				MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(write_req, 1, 4);
				_mc_bw_per_step += 4;
			}
		}
		else if (_scheme == UnisonCache && type == STORE)
		{
			// LLC dirty eviction hit
			MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			req.cycle = _mcdram[mcdram_select]->access(write_req, 1, 4);
			_mc_bw_per_step += 4;
		}
		if (_scheme == AlloyCache || _scheme == UnisonCache || _scheme == CacheMode)
			data_ready_cycle = req.cycle;
		_num_hit_per_step++;
		if (_scheme == HMA)
			_os_placement_policy->handleCacheAccess(tag, type);
		else if (_scheme == HybridCache || _scheme == UnisonCache)
		{
			_page_placement_policy->handleCacheHit(tag, type, set_num, &_cache[set_num], counter_access, hit_way);
		}

		if (req.type == PUTX)
		{
			_numStoreHit.inc();
			_cache[set_num].ways[hit_way].dirty = true;
		}
		else
			_numLoadHit.inc();

		if (_scheme == HybridCache)
		{
			if (!hybrid_tag_probe)
			{
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address;
				// req.lineAddr = initial_req_addr; // add by jhlu
				data_ready_cycle = req.cycle;
				if (type == LOAD && _tag_buffer->canInsert(tag))
					_tag_buffer->insert(tag, false);
			}
			else
			{
				assert(!_sram_tag);
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
				req.lineAddr = mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 1, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address; 
				// req.lineAddr = initial_req_addr; // add by jhlu
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == Tagless)
		{
			req.lineAddr = mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			// req.lineAddr = initial_req_addr; // add by jhlu
			data_ready_cycle = req.cycle;

			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}

		//// data access
		if (_scheme == HMA)
		{
			req.lineAddr = mc_address; // transMCAddressPage(set_num, hit_way); //mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			// req.lineAddr = initial_req_addr; // add by jhlu
			data_ready_cycle = req.cycle;
		}
		if (_scheme == UnisonCache)
		{
			// Update LRU information for UnisonCache
			MemReq tag_update_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			_mcdram[mcdram_select]->access(tag_update_req, 2, 2);
			_mc_bw_per_step += 2;
			_numTagStore.inc();
			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}
		///////////////////////////////
	}
	// end:
	//  TODO. make this part work again.
	if (counter_access && !_sram_tag)
	{
		// TODO may not need the counter load if we can store freq info inside TAD
		/////// model counter access in mcdram
		// One counter read and one coutner write
		assert(set_num >= _ds_index);
		_numCounterAccess.inc();
		MemReq counter_req = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		counter_req.type = PUTX;
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		_mc_bw_per_step += 4;
		//////////////////////////////////////
	}
	if (_scheme == HybridCache && _tag_buffer->getOccupancy() > 0.7)
	{
		printf("[Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
		_tag_buffer->clearTagBuffer();
		_tag_buffer->setClearTime(req.cycle);
		_numTagBufferFlush.inc();
	}

	// TODO. Make the timing info here correct.
	// TODO. should model system level stall
	if (_scheme == HMA && _num_requests % _os_quantum == 0)
	{
		uint64_t num_replace = _os_placement_policy->remapPages();
		_numPlacement.inc(num_replace * 2);
	}
	
	if (_num_requests % step_length == 0)
	{
		// std::cout << "step_length is not fail" << std::endl;
		_num_hit_per_step /= 2;
		_num_miss_per_step /= 2;
		_mc_bw_per_step /= 2;
		_ext_bw_per_step /= 2;
		//默认不开启
		if (_bw_balance && _mc_bw_per_step + _ext_bw_per_step > 0)
		{
			// adjust _ds_index	based on mc vs. ext dram bandwidth.
			double ratio = 1.0 * _mc_bw_per_step / (_mc_bw_per_step + _ext_bw_per_step);
			double target_ratio = 0.8; // because mc_bw = 4 * ext_bw

			// the larger the gap between ratios, the more _ds_index changes.
			// _ds_index changes in the granualrity of 1/1000 dram cache capacity.
			// 1% in the ratio difference leads to 1/1000 _ds_index change.
			// 300 is arbitrarily chosen.
			// XXX XXX XXX
			// 1000 is only used for graph500 and pagerank.
			// uint64_t index_step = _num_sets / 300; // in terms of the number of sets
			uint64_t index_step = _num_sets / 1000; // in terms of the number of sets
			int64_t delta_index = (ratio - target_ratio > -0.02 && ratio - target_ratio < 0.02) ? 0 : index_step * (ratio - target_ratio) / 0.01;
			printf("ratio = %f\n", ratio);
			if (delta_index > 0)
			{
				// _ds_index will increase. All dirty data between _ds_index and _ds_index + delta_index
				// should be written back to external dram.
				// For Alloy cache, this is relatively easy.
				// For Hybrid, we need to update tag buffer as well...
				for (uint32_t mc = 0; mc < _mcdram_per_mc; mc++)
				{
					for (uint64_t set = _ds_index; set < (uint64_t)(_ds_index + delta_index); set++)
					{
						if (set >= _num_sets)
							break;
						for (uint32_t way = 0; way < _num_ways; way++)
						{
							Way &meta = _cache[set].ways[way];
							if (meta.valid && meta.dirty)
							{
								// should write back to external dram.
								MemReq load_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[mc]->access(load_req, 2, (_granularity / 64) * 4);
								MemReq wb_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(wb_req, 2, (_granularity / 64) * 4);
								_ext_bw_per_step += (_granularity / 64) * 4;
								_mc_bw_per_step += (_granularity / 64) * 4;
							}
							if (_scheme == HybridCache && meta.valid)
							{
								_tlb[meta.tag].way = _num_ways;
								// for Hybrid cache, should insert to tag buffer as well.
								if (!_tag_buffer->canInsert(meta.tag))
								{
									printf("Rebalance. [Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
									_tag_buffer->clearTagBuffer();
									_tag_buffer->setClearTime(req.cycle);
									_numTagBufferFlush.inc();
								}
								assert(_tag_buffer->canInsert(meta.tag));
								_tag_buffer->insert(meta.tag, true);
							}
							meta.valid = false;
							meta.dirty = false;
						}
						if (_scheme == HybridCache)
							_page_placement_policy->flushChunk(set);
					}
				}
			}
			_ds_index = ((int64_t)_ds_index + delta_index <= 0) ? 0 : _ds_index + delta_index;
			printf("_ds_index = %ld/%ld\n", _ds_index, _num_sets);
		}
	}
	req.lineAddr = initial_req_addr;
	futex_unlock(&_lock);
	// uint64_t latency = req.cycle - orig_cycle;
	// req.cycle = orig_cycle;
	return data_ready_cycle; // req.cycle + latency;
}

/**
 * @brief HPCA'2020 Hybrid2 Memory Controller
 * @cite  @INPROCEEDINGS{9065506,
			author={Vasilakis, Evangelos and Papaefstathiou, Vassilis and Trancoso, Pedro and Sourdis, Ioannis},
			booktitle={2020 IEEE International Symposium on High Performance Computer Architecture (HPCA)}, 
			title={Hybrid2: Combining Caching and Migration in Hybrid Memory Systems}, 
			year={2020},
			pages={649-662},
			keywords={Random access memory;Bandwidth;Frequency modulation;Metadata;Three-dimensional displays;System-on-chip;Hardware;DRAM Cache;Data Migration;Hybrid Memory System;3D stacked DRAM;Memory},
			doi={10.1109/HPCA47549.2020.00059}}
 */
uint64_t
MemoryController::hybrid2_access(MemReq &req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}

	if (req.type == PUTS)
	{
		return req.cycle;
	}
	// futex_unlock(&_lock); return之前的某个时机需要释放这把锁
	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	Address address = req.lineAddr;
	address = address;

	// address = address / 64 * 64;;
	MESIState state;
	// HBM在这里需要自己考虑分到哪一个通道
	uint32_t mem_hbm_select = (address / 64) % _cache_hbm_per_mc;
	// uint32_t mem_hbm_select = address % _cache_hbm_per_mc;
	Address mem_hbm_address = (address / 64 / _cache_hbm_per_mc * 64) | (address % 64);
	// Address mem_hbm_address = (address / _cache_hbm_per_mc ) | address;

	// address在哪一个page，在page第几个block
	// 保证内存对齐
	assert(0 != _hybrid2_blk_size);
	// uint64_t page_addr = (address / _hybrid2_page_size) * _hybrid2_page_size;
	uint64_t page_addr = get_page_id(address);
	// uint64_t blk_offset = (address - page_addr*_hybrid2_page_size) / _hybrid2_blk_size;
	// 先计算页内偏移再按block对齐
	// uint64_t blk_addr = ((address % _hybrid2_page_size) / _hybrid2_blk_size) * _hybrid2_blk_size;
	uint64_t blk_offset = (address % _hybrid2_page_size) / _hybrid2_blk_size;
	// std::cout << "blk_offset ==" << blk_offset << std::endl;

	// 根据程序的执行流，先访问XTA
	// 根据XTA的两层结构，应该先找到set，再找到Page
	// 所以需要先封装一个获取set的函数以降低耦合度
	uint64_t set_id = get_set_id(address);
	g_vector<XTAEntry> &SETEntries = find_XTA_set(set_id);
	// 遍历 这个SET
	bool if_XTA_hit = false;
	// bool is_dram = address >= _mem_hbm_size;

	// 为HBMTable服务，在迁移或逐出阶段，对于可能在HBM或remap到DRAM的数据使用
	uint64_t avg_temp = 0;
	uint64_t low_temp = 100000;
	
	// metadata is in hbm (modified 2025.02.18)
	uint64_t look_up_XTA_rd_latency = _mcdram[0]->rd_dram_tag_latency(req,2);
	uint64_t look_up_XTA_wt_latency = _mcdram[0]->wt_dram_tag_latency(req,2);; 

	uint64_t total_latency = 0;
	total_latency += look_up_XTA_rd_latency + look_up_XTA_wt_latency;// must read , each req will (over)write XTA at least once

	// 在SETEntries里找，看看能不能找到那个page,找到了就是XTAHit，否则就是XTAMiss
	// 找的逻辑是根据地址去找，匹配_hybrid2_tag
	for (uint64_t i = 0; i < set_assoc_num; i++)
	{
		// 不那么重要的设计
		if (SETEntries[i]._hybrid2_counter > 0)
		{
			low_temp = low_temp > SETEntries[i]._hybrid2_counter ? SETEntries[i]._hybrid2_counter : low_temp;
		}
		avg_temp += SETEntries[i]._hybrid2_counter;
	
		if (page_addr == SETEntries[i]._hybrid2_tag)
		{
			// Indicates XTA Hit
			if_XTA_hit = true;
			// std::cout << "[XTA Hit]" <<std::endl;
			// XTA Hit 意味着 Page也hit了，page hit 但是cacheline 不一定hit
			// 首先把LRU的值先改了,本Page LRU置为0，其余计数器+1
			for (uint64_t j = 0; j < set_assoc_num; j++)
			{
				SETEntries[j]._hybrid2_LRU++;
			}
			SETEntries[i]._hybrid2_LRU = 0;
			SETEntries[i]._hybrid2_counter += 1;

			int exist = SETEntries[i].bit_vector[blk_offset]; // 0 代表cacheline miss 1 代表 cacheline hit
			if (exist)
			{
				// std::cout << "XHCH" << std::endl;
				// 访问HBM,TODO
				if (type == STORE) 
				{
					// Type = store需要标记为脏 update 2024/12/30
					SETEntries[i].dirty_vector[blk_offset] = 1; // if evict, should writeback !
					req.lineAddr = mem_hbm_address;
					req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);
					req.lineAddr = tmpAddr;
					total_latency += req.cycle;  // Look Up XTA Latency should be considered !
					SETEntries[i]._hybrid2_counter += 1;
					futex_unlock(&_lock);
					return total_latency;
				}
				else if (type == LOAD)
				{
					req.lineAddr = mem_hbm_address;
					req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);
					req.lineAddr = tmpAddr;
					total_latency += req.cycle;
					SETEntries[i]._hybrid2_counter += 1;
					futex_unlock(&_lock);
					return total_latency;
				}
			}
			else // cacheline miss
			{
				// std::cout << "XHCM" << std::endl;
				// 这里也有两种情况。
				// Case1:有可能在DRAM里；Case2：有可能在HBM里 || 两种情况都有可能出现remap的情况
				// 有可能是dram,有可能remap到hbm
				if (address >= _mem_hbm_size) //XTAHit, Cacheline Miss, after loading data, vaild-bit is set to 1 !
				{
					// 检查DRAMTable有没有存映射
					auto it = DRAMTable.find(page_addr);
					if (it == DRAMTable.end())
					{
						// 访问DRAM
						// (access dram, load from dram), store to hbm(asyn);
						// critical path latency = access(dram);

						// access dram
						req.cycle = _ext_dram->access(req, 0, 4);
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;

						// load from dram (when we access a cacheline, we actually have executed load operation. Thus load_req is a extra meaningless latency)
						
						// store to hbm 
						uint64_t tmp_hbm_tag = SETEntries[i]._hbm_tag;
						Address dest_hbm_addr = 0;
						if(tmp_hbm_tag != static_cast<uint64_t>(0))
							dest_hbm_addr = tmp_hbm_tag * _hybrid2_page_size + blk_offset*64;
						else
							dest_hbm_addr = tmpAddr % _mem_hbm_size;
						
						uint64_t dest_hbm_mc_address = (dest_hbm_addr / 64 / _mem_hbm_per_mc * 64 ) |(dest_hbm_addr % 64);
						uint64_t dest_hbm_select = (dest_hbm_addr / 64) % _mem_hbm_per_mc;
						MemReq store_req = {dest_hbm_mc_address, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[dest_hbm_select]->access(store_req, 2, 4); // notice : this is a cacheline, so data_size = 4 (*16) 
						SETEntries[i].bit_vector[blk_offset] = 1;
						SETEntries[i]._hybrid2_counter += 1;
						futex_unlock(&_lock);
						return total_latency;
					}
					else
					{
						uint64_t dest_address = it->second;
						uint64_t dest_hbm_mc_address = (dest_address / 64 / _mem_hbm_per_mc * 64 ) | (dest_address % 64);
						uint64_t dest_hbm_select = (dest_address / 64)  % _mem_hbm_per_mc;
						req.lineAddr = dest_hbm_mc_address;
						req.cycle = _mcdram[dest_hbm_select]->access(req, 0, 4);
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						SETEntries[i].bit_vector[blk_offset] = 1;
						SETEntries[i]._hybrid2_counter += 1;
						futex_unlock(&_lock);
						return total_latency;
					}
				}
				else
				{ // 否则有可能是HBM,但也有可能是remap到DRAM
					auto it = HBMTable.find(page_addr);
					if (it == HBMTable.end())
					{
						// 访问HBM
						req.lineAddr = mem_hbm_address;
						req.cycle = _mcdram[mem_hbm_select]->access(req, 0, 4);
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						SETEntries[i].bit_vector[blk_offset] = 1;
						SETEntries[i]._hybrid2_counter += 1;
						futex_unlock(&_lock);
						return total_latency;
					}
					else
					{
						// under what circumstances can it happen?
						// a cacheline that was evicted to dram ?
						uint64_t dest_address = it->second *_hybrid2_page_size + blk_offset*_hybrid2_blk_size;
						req.lineAddr = dest_address;
						req.cycle = _ext_dram->access(req, 0, 4);
						total_latency += req.cycle;
						req.lineAddr = tmpAddr;

						// load from dram (when we access a cacheline, we actually have executed load operation. Thus load_req is a meaningless additional latency)

						// store to hbm
						uint64_t tmp_hbm_tag = SETEntries[i]._hbm_tag;
						Address dest_hbm_addr = 0;
						if(tmp_hbm_tag != static_cast<uint64_t>(0))
							dest_hbm_addr = tmp_hbm_tag * _hybrid2_page_size + blk_offset*64;
						else
							dest_hbm_addr = dest_address % _mem_hbm_size;

						// uint64_t dest_hbm_mc_address = (dest_hbm_addr / 64 / _mem_hbm_per_mc * 64) | (dest_hbm_addr % 64);
						// uint64_t dest_hbm_select = (dest_hbm_addr / 64) % _mem_hbm_per_mc;					
						uint64_t dest_hbm_mc_address = (dest_hbm_addr / 64 / _mem_hbm_per_mc *64 ) | (dest_hbm_addr%64) ;
						uint64_t dest_hbm_select = (dest_hbm_addr / 64)  % _mem_hbm_per_mc;
						MemReq store_req = {dest_hbm_mc_address, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[dest_hbm_select]->access(store_req, 2, 4); 		
						
						SETEntries[i].bit_vector[blk_offset] = 1;
						SETEntries[i]._hybrid2_counter += 1;
						futex_unlock(&_lock);
						return total_latency;
					}
				}
			}
		}
	} // ending looking up XTA
	// std::cout << "[XTA Miss]" <<std::endl;
	assert(0 != set_assoc_num);
	avg_temp = avg_temp / set_assoc_num;
	// XTA Miss 
	if (!if_XTA_hit)
	{
		// std::cout << "XM" << std::endl;
		chbm_miss_cntr += 1;
		uint64_t current_cycle = req.cycle;
		// std::cout<< "current_cycle = " << current_cycle <<std::endl;
		if(current_cycle - cntr_last_cycle > time_intv)
		{
			chbm_miss_cntr = 0;
			cntr_last_cycle = current_cycle;			
		}

		// 根究 address 找到set 把set里的page 根据LRU值淘汰一个
		// 这个set 已经由之前的引用类型获得,这里的address都有可能在remaptable里
		int empty_idx = check_set_full(SETEntries);
		uint64_t lru_idx = ret_lru_page(SETEntries);
		int empty_occupy = check_set_occupy(SETEntries);

		// 表示没有空的，那就LRU干掉一个,这就有空的了
		// 被LRU干掉的数据根据迁移代价计算公式迁移到对应的内存介质
		if (-1 == empty_idx)
		{
			uint64_t cache_blk_num = 0;
			uint64_t dirty_blk_num = 0;

			for (uint32_t k = 0; k < hybrid2_blk_per_page; k++)
			{
				if (SETEntries[lru_idx].bit_vector[k])
					cache_blk_num++;
				if (SETEntries[lru_idx].dirty_vector[k])
					dirty_blk_num++;
			}
			uint64_t migrate_cost = 2 * hybrid2_blk_per_page - cache_blk_num + 1;
			uint64_t evict_cost = dirty_blk_num;
			uint64_t net_cost = migrate_cost - evict_cost;

			uint64_t tmp_hybrid2_tag =  SETEntries[lru_idx]._hybrid2_tag;
			uint64_t tmp_hbm_tag = SETEntries[lru_idx]._hbm_tag;
			uint64_t tmp_dram_tag = SETEntries[lru_idx]._dram_tag;
			uint64_t heat_counter = SETEntries[lru_idx]._hybrid2_counter;

			bool migrate_init_dram = false;
			bool migrate_init_hbm = false;
			bool migrate_final_hbm = false;
			bool migrate_final_dram = false;

			// 是否迁移或逐出
			// Case DDR:
			// Eviction：（1）cHBM->mHBM (2) dirty Cacheline writeback
			// Migration：（1）cHBM->mHBM (2) load (bit=0) cacheline , store to hbm    (a) add(K_page,V_page)->DRAMTable

			// Case HBM:
			// Eviction: (1) cHBM->mHBM
			// Migration: (1) cHBM->mHBM (2) load(bit=1) cacheline, store to dram   (a) add(K_page,V_page)->HBMTable

			// 这一段是可能原来就在DRAM，或者被remap进HBM的部分
			// remap进HBM的部分，要是这部分数据不太热就踢出去
			if (address >= _mem_hbm_size)
			{
				migrate_init_dram = true;
				uint64_t hbm_page_addr = 0;
				auto it = DRAMTable.find(page_addr);
				if (it != DRAMTable.end()) // Indicates remap to hbm
				{
					migrate_init_dram = false;
					migrate_final_hbm = true;
					hbm_page_addr = it->second;
				} // 当前页面！！！

				// Page就在DDR上面，chbm_miss_cntr比开销大（positive），移到HBM（移动vaild_bit为0的cacheline）
				// 迁移需要新增映射
				if (migrate_init_dram && chbm_miss_cntr > net_cost)
				{
					// 对应页面处理流程
					// S1:取出LRU淘汰页面的有效数据
					// S2:取出DDR页面对应的cacheline （返回给CPU）
					// S3：取出的cacheline store到各自的位置
					if(tmp_hbm_tag != static_cast<uint64_t>(0))
					{
						DRAMTable[page_addr] = tmp_hbm_tag;
						// 再次优化逻辑：
						// 既然我load DRAM数据的时候就已经完成了access的操作，那access cacheline完全可以先做
						req.lineAddr = tmpAddr;
						req.cycle = _ext_dram->access(req,0,4);

						// Load from cHBM
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(SETEntries[lru_idx].bit_vector[i] == 1) 
							{
								// load from hbm
								Address lru_addr = SETEntries[lru_idx]._hybrid2_tag*_hybrid2_page_size + i*_hybrid2_blk_size;
								uint64_t lru_hbm_addr = (lru_addr / 64 / _mem_hbm_per_mc * 64)| (lru_addr % 64) ;
								uint64_t lru_hbm_select = (lru_hbm_addr / 64 ) % _mem_hbm_per_mc;
								MemReq load_req = {lru_hbm_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								// _mcdram[mem_hbm_select]->access(load_req, 2, 4);  // load 完才能 access
								_mcdram[lru_hbm_select]->access(load_req, 2, 4);

							}
						}

						// store cacheline
						Address mem_addr = tmp_hbm_tag*_hybrid2_page_size + blk_offset*_hybrid2_blk_size;
						uint64_t mem_hbm_addr = (mem_addr/64/ _mem_hbm_per_mc * 64 )| (mem_addr % 64) ;
						uint64_t mem_select = (mem_hbm_addr / 64) % _mem_hbm_per_mc;
						MemReq store_req = {mem_hbm_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mem_select]->access(store_req, 2, 4);

						// Store to DDR
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(SETEntries[lru_idx].bit_vector[i] == 1)
							{
								Address dest_addr = tmpAddr + i*blk_offset;
								MemReq store_req = {dest_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(store_req, 2, 4);
							}
						}

						// metadata update
						SETEntries[lru_idx]._hbm_tag = tmp_hbm_tag;
						SETEntries[lru_idx]._hybrid2_tag = page_addr;
						SETEntries[lru_idx]._dram_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_counter = 1;

						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(i == blk_offset)SETEntries[lru_idx].bit_vector[i] = 1;
							else SETEntries[lru_idx].bit_vector[i] = 0;
						}
						SETEntries[lru_idx]._hybrid2_LRU = 0;
						total_latency += req.cycle;
						futex_unlock(&_lock);
						return total_latency;

					}
					else
					{ // 否则按照地址均匀的方式，按地址%mem_hbm_size 映射
						DRAMTable[page_addr] = page_addr % (_mem_hbm_size / _hybrid2_page_size);
						Address remap_addr = page_addr % (_mem_hbm_size / _hybrid2_page_size);
						req.lineAddr = tmpAddr;
						req.cycle = _ext_dram->access(req,0,4);
						
						// load cHBM
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(SETEntries[lru_idx].bit_vector[i] == 1)
							{
								// load from hbm
								Address lru_addr = remap_addr*_hybrid2_page_size + i*_hybrid2_blk_size;
								uint64_t lru_hbm_addr = (lru_addr / 64 / _mem_hbm_per_mc * 64)| (lru_addr%64) ;
								uint64_t lru_hbm_select = (lru_addr / 64)  % _mem_hbm_per_mc;
								MemReq load_req = {lru_hbm_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[lru_hbm_select]->access(load_req, 2, 4);

							}
						}

						Address lru_addr = SETEntries[lru_idx]._hybrid2_tag + blk_offset*_hybrid2_blk_size;
						uint64_t lru_hbm_addr = (lru_addr / 64 /_mem_hbm_per_mc * 64)| (lru_addr%64);
						uint64_t lru_hbm_select = (lru_addr / 64)  % _mem_hbm_per_mc;
						MemReq store_req = {lru_hbm_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[lru_hbm_select]->access(store_req,2,4);

						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(SETEntries[lru_idx].bit_vector[i] == 1)
							{
								uint64_t dest_hbm_mc_address = (page_addr % (_mem_hbm_size / _hybrid2_page_size) * _hybrid2_page_size + i * 64) / 64 / _mem_hbm_per_mc * 64  | ((tmp_hbm_tag * _hybrid2_page_size + i * 64) % 64);
								uint64_t dest_hbm_select = (page_addr % (_mem_hbm_size / _hybrid2_page_size) * _hybrid2_page_size + i * 64) / 64  % _mem_hbm_per_mc;
								MemReq store_req = {dest_hbm_mc_address, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[dest_hbm_select]->access(store_req, 2, 4);	
							}
						}

						// metadata update
						SETEntries[lru_idx]._hbm_tag = page_addr % (_mem_hbm_size / _hybrid2_page_size);
						SETEntries[lru_idx]._hybrid2_tag = page_addr;
						SETEntries[lru_idx]._dram_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_counter = 1;
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(i != blk_offset)SETEntries[lru_idx].bit_vector[i] = 0;
							else SETEntries[lru_idx].bit_vector[i] = 1;
						}
						SETEntries[lru_idx]._hybrid2_LRU = 0;
						total_latency += req.cycle;
						futex_unlock(&_lock);
						return total_latency;
					}
				}

				// 否则就驱逐(evict cacheline 为 dirty的)
				// 驱逐在HBM，则逻辑驱逐；驱逐在DDR，则dirty cacheline驱逐
				if (chbm_miss_cntr <= net_cost)
				{				
					bool is_logic = tmp_dram_tag == static_cast<uint64_t>(0) ? false:true; // 有DRAMTag 就得驱逐回去
					if(migrate_final_hbm) // 是HBM就是，逻辑驱逐,连load,store都不用
					{
						Address dest_addr = hbm_page_addr + blk_offset*_bumblebee_blk_size;
						uint64_t lru_hbm_addr = (dest_addr / 64 / _mem_hbm_per_mc * 64)| (dest_addr % 64);
						uint64_t lru_hbm_select = (dest_addr / 64) % _mem_hbm_per_mc;
						req.lineAddr = lru_hbm_addr;
						req.cycle = _mcdram[lru_hbm_select]->access(req,0,4);
						req.lineAddr = tmpAddr;

						if(is_logic)
						{
							// 逻辑驱逐完，当前的加入cache
							SETEntries[lru_idx]._hbm_tag = tmp_hbm_tag;
							SETEntries[lru_idx]._hybrid2_tag = page_addr;
							SETEntries[lru_idx]._dram_tag = page_addr;
							SETEntries[lru_idx]._hybrid2_counter = 1;
							for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
							{
								if(i != blk_offset)SETEntries[lru_idx].bit_vector[i] = 0;
								else SETEntries[lru_idx].bit_vector[i] = 1;
							}
							SETEntries[lru_idx]._hybrid2_LRU = 0;
							total_latency += req.cycle;
							req.lineAddr = tmpAddr;
							futex_unlock(&_lock);
							return total_latency;
						}

						for(uint32_t i = 0;i<(_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							// only dirty cacheline should be writeback;
							if(SETEntries[lru_idx].dirty_vector[i] == 1)
							{
								// load from hbm
								uint64_t dest_hbm_mc_address = ((tmp_hybrid2_tag * _hybrid2_page_size + i * 64) / 64 / _mem_hbm_per_mc * 64) | ((tmp_hybrid2_tag * _hybrid2_page_size + i * 64) % 64);
								uint64_t dest_hbm_select = (tmp_hybrid2_tag * _hybrid2_page_size + i * 64) / 64  % _mem_hbm_per_mc;
								MemReq load_req = {dest_hbm_mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[dest_hbm_select]->access(load_req, 2, 4);

								// evict to ddr
								Address dest_addr = tmp_dram_tag + i*_hybrid2_blk_size;
								MemReq store_req = {dest_addr, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(store_req,2,4);
							}
						}

						// 置空
						SETEntries[lru_idx]._hbm_tag = tmp_hbm_tag;
						SETEntries[lru_idx]._hybrid2_tag = page_addr;
						SETEntries[lru_idx]._dram_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_counter = 1;
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(i != blk_offset)SETEntries[lru_idx].bit_vector[i] = 0;
							else SETEntries[lru_idx].bit_vector[i] = 1;
						}
						SETEntries[lru_idx]._hybrid2_LRU = 0;
						total_latency += req.cycle;
						req.lineAddr = tmpAddr;
						futex_unlock(&_lock);
						return total_latency;
					}
					else // 是DRAM，
					{
						req.lineAddr = tmpAddr;
						req.cycle = _ext_dram->access(req,0,4);

						if(is_logic)
						{
							// 逻辑驱逐完，当前请求加入cache
							SETEntries[lru_idx]._hbm_tag = tmp_hbm_tag;
							SETEntries[lru_idx]._hybrid2_tag = page_addr;
							SETEntries[lru_idx]._dram_tag = page_addr;
							SETEntries[lru_idx]._hybrid2_counter = 1;
							for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
							{
								if(i != blk_offset)SETEntries[lru_idx].bit_vector[i] = 0;
								else SETEntries[lru_idx].bit_vector[i] = 1;
							}
							SETEntries[lru_idx]._hybrid2_LRU = 0;
							total_latency += req.cycle;
							req.lineAddr = tmpAddr;
							futex_unlock(&_lock);
							return total_latency;					
						}

						for(uint32_t i = 0;i<(_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							// only dirty cacheline should be writeback;
							if(SETEntries[lru_idx].dirty_vector[i] == 1)
							{
								// load from hbm
								uint64_t dest_hbm_mc_address = ((tmp_hybrid2_tag * _hybrid2_page_size + i * 64) / 64 / _mem_hbm_per_mc * 64) | ((tmp_hybrid2_tag * _hybrid2_page_size + i * 64) % 64);
								uint64_t dest_hbm_select = (tmp_hybrid2_tag * _hybrid2_page_size + i * 64) / 64 % _mem_hbm_per_mc;
								MemReq load_req = {dest_hbm_mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[dest_hbm_select]->access(load_req, 2, 4);

								// evict to ddr
								Address dest_addr = tmp_dram_tag + i*_hybrid2_blk_size;
								MemReq store_req = {dest_addr, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(store_req,2,4);
							}
						}

						// 置空
						SETEntries[lru_idx]._hbm_tag = tmp_hbm_tag;
						SETEntries[lru_idx]._hybrid2_tag = page_addr;
						SETEntries[lru_idx]._dram_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_counter = 1;
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							if(i != blk_offset)SETEntries[lru_idx].bit_vector[i] = 0;
							else SETEntries[lru_idx].bit_vector[i] = 1;
						}
						SETEntries[lru_idx]._hybrid2_LRU = 0;
						total_latency += req.cycle;
						req.lineAddr = tmpAddr;
						futex_unlock(&_lock);
						return total_latency;
					}
				}
			}

			// 这一段是原来可能在HBM的部分，但是也有可能被remap进DRAM
			// 在HBM的话，只要数据比其它页面都冷就remap到DRAM（相当于踢掉）
			// 被remap进DRAM的话，只要数据比页面平均温度更热，就erase这个映射，相当于保持在HBM里
			if (address < _mem_hbm_size)
			{
				migrate_init_hbm = true;
				auto it = HBMTable.find(page_addr);
				if (it != HBMTable.end())
				{
					migrate_init_hbm = false;
					migrate_final_dram = true;
				}

				// 优化了逻辑：
				// （1）非必要不主动迁移驱逐，只有对应set处于“高占用”情况且对应页热度是最低的，才考虑 
				// （2）依然加入chbm_miss_cntr比较，已决定迁移驱逐
				bool is_migrate = chbm_miss_cntr > net_cost ? true:false;
				if (migrate_init_hbm && empty_occupy > (int)set_assoc_num - 2 && heat_counter < low_temp)
				{
					if(is_migrate)
					{
						// 本身就是HBM的页面，mHBM页面和cHBM页面迁移就是改标志位
						SETEntries[lru_idx]._hbm_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_tag = page_addr;
						SETEntries[lru_idx]._dram_tag = page_addr;
						SETEntries[lru_idx]._hybrid2_counter = 1;
						for(uint32_t i = 0; i< (_hybrid2_page_size / _hybrid2_blk_size);i++)
						{
							SETEntries[lru_idx].bit_vector[i] = 1; // 既然在HBM里逻辑无代价，全都set 1
						}

						req.lineAddr = ((tmp_hbm_tag*_hybrid2_page_size + blk_offset*_hybrid2_blk_size) / 64 / _mem_hbm_per_mc * 64 )|((tmp_hbm_tag*_hybrid2_page_size + blk_offset*_hybrid2_blk_size) % 64) ;
						mem_hbm_select = req.lineAddr / 64 % _mcdram_per_mc;
					    req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
						req.lineAddr = tmpAddr;
						total_latency += req.cycle;
						futex_unlock(&_lock);
						return total_latency;
					}
					else // evict
					{
						HBMTable.erase(page_addr);
						// 置空
						SETEntries[lru_idx]._hybrid2_tag = 0;
						SETEntries[lru_idx]._hbm_tag = 0;
						SETEntries[lru_idx]._dram_tag = 0;
						SETEntries[lru_idx]._hybrid2_LRU = 0;
						SETEntries[lru_idx]._hybrid2_counter = 0;
						for (uint32_t k = 0; k < hybrid2_blk_per_page; k++)
						{
							SETEntries[lru_idx].bit_vector[k] = 0;
							SETEntries[lru_idx].dirty_vector[k] = 0;
						}
						// access
						req.lineAddr = tmpAddr;
						req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
						futex_unlock(&_lock);
						total_latency += req.cycle;
						return total_latency;
					}
					
				}

				// 温热数据就留在HBM了,
				if (migrate_final_dram && heat_counter >= avg_temp)
				{
					HBMTable.erase(page_addr);
				}
			}

			// 置空
			SETEntries[lru_idx]._hybrid2_tag = 0;
			SETEntries[lru_idx]._hbm_tag = 0;
			SETEntries[lru_idx]._dram_tag = 0;
			SETEntries[lru_idx]._hybrid2_LRU = 0;
			SETEntries[lru_idx]._hybrid2_counter = 0;
			for (uint32_t k = 0; k < hybrid2_blk_per_page; k++)
			{
				SETEntries[lru_idx].bit_vector[k] = 0;
				SETEntries[lru_idx].dirty_vector[k] = 0;
			}

			// 更新索引
			empty_idx = lru_idx;
		}

		if (-1 != empty_idx) // 表示有空的，且一定不是-1：因为没有空的也会被我LRU干掉一个
		{
			SETEntries[empty_idx]._hybrid2_tag = get_page_id(address);
			SETEntries[empty_idx]._hybrid2_LRU = 0;
			SETEntries[empty_idx]._hybrid2_counter = 0;
			SETEntries[empty_idx]._hybrid2_counter += 1;

			uint64_t dest_blk_address = 0;
			bool is_dram = false;
			bool is_remapped = false;

			if (address > _mem_hbm_size)
			{ // 可能是dram，但也有可能是被remap的
				// std::cout << "workflow come here :(addr > memhbm_size)  !" << std::endl;
				SETEntries[empty_idx]._dram_tag = get_page_id(address);
				dest_blk_address = address;
				is_dram = true;
				// 看看有没有remap，有就更新，没有就不更新
				auto it = DRAMTable.find(get_page_id(address));
				if (it != DRAMTable.end()) // 就说明有对吧
				{
					SETEntries[empty_idx]._hbm_tag = it->second;
					dest_blk_address = it->second*_hybrid2_page_size + blk_offset*64;;
					is_dram = false;
					is_remapped = true;
				}
			}
			else // 否则有可能是HBM，有可能remap到dram
			{
				// std::cout << "workflow come here :(addr < memhbm_size)!" << std::endl;
				assert(address >= 0);
				SETEntries[empty_idx]._hbm_tag = get_page_id(address);
				dest_blk_address = address;
				auto it = HBMTable.find(get_page_id(address));
				if (it != HBMTable.end()) // 就说明有对吧
				{
					SETEntries[empty_idx]._dram_tag = it->second;
					dest_blk_address = it->second*_hybrid2_page_size + blk_offset*64;
					is_dram = true;
					is_remapped = true;
				}
			}

			// 这个时候基本的XTAEntry已经完成了，还差两个blk_vector
			// 看看dest_blk_address在哪里
			if (!is_dram)
			{ // 在HBM,只需访问HBM对应的blk
				// 访问HBM,TODO
				// assert(static_cast<uint64_t>(0) != dest_blk_address);  // 0 也是合理的

				// uint64_t dest_hbm_mc_address = (dest_blk_address / 64 / _mem_hbm_per_mc * 64) | (dest_blk_address % 64);
				// uint64_t dest_hbm_select = (dest_blk_address / 64) % _mem_hbm_per_mc;
				uint64_t dest_hbm_mc_address = (dest_blk_address / 64 / _mem_hbm_per_mc * 64) | (dest_blk_address % 64);
				uint64_t dest_hbm_select = (dest_blk_address / 64)  % _mem_hbm_per_mc;
				req.lineAddr = dest_hbm_mc_address;
				req.cycle = _mcdram[dest_hbm_select]->access(req, 0, 4);
				req.lineAddr = tmpAddr;
				total_latency += req.cycle;

				// 更新XTA
				SETEntries[empty_idx].bit_vector[static_cast<uint32_t>(blk_offset)] = 1;
				futex_unlock(&_lock);
				return total_latency;
			}
			else // 在DRAM
			{
				// 访问DRAM,TODO
				// assert(static_cast<uint64_t>(0) != dest_blk_address);
				req.lineAddr = dest_blk_address;
				req.cycle = _ext_dram->access(req, 0, 4);
				req.lineAddr = tmpAddr;
				total_latency += req.cycle;
				// 从DRAM写到HBM
				// 现在是Page有空的，然后原来的数据是在DRAM，所以DRAMTable需要更新进去这个remap,已经remap就无需管
				if (!is_remapped)
				{
					// 在ZSim中是否由于access只返回访问对应内存介质access的延迟，而不会产生实际的修改内存操作
					// 基于这样的设想，我是否只需要remap到HBM的对应set的任何一个有效位置即可呢？
					// 再更新XTA
					uint64_t dest_hbm_addr = address % _mem_hbm_size;
					DRAMTable[address] = dest_hbm_addr;
					SETEntries[empty_idx]._hybrid2_counter += 1;
					SETEntries[empty_idx].bit_vector[static_cast<uint32_t>(blk_offset)] = 1;
				}
				futex_unlock(&_lock);
				return total_latency;
			}
		}
	}
	futex_unlock(&_lock);
	return 0;
}

/**
 * @brief DAC'23 Bumblebee Memory Controller
 * @cite  @INPROCEEDINGS{10248000,
			author={Hua, Yifan and Zheng, Shengan and Yin, Ji and Chen, Weidong and Huang, Linpeng},
			booktitle={2023 60th ACM/IEEE Design Automation Conference (DAC)}, 
			title={Bumblebee: A MemCache Design for Die-stacked and Off-chip Heterogeneous Memory Systems}, 
			year={2023},
			pages={1-6},
			keywords={Energy consumption;Design automation;Costs;Memory management;Memory architecture;Random access memory;Switches;Heterogeneous memory;Die-stacked high-bandwidth memory;Caching and migration},
			doi={10.1109/DAC56929.2023.10248000}}
 * @attention 79% improvement comparing to pure DRAM in current Verison
 */
uint64_t
MemoryController::bumblebee_access(MemReq& req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	
	if (req.type == PUTS)
	{
		return req.cycle;
	}
	ReqType type = (req.type == GETS || req.type == GETX) ? LOAD : STORE;
	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	Address address = req.lineAddr;
	MESIState state;
	
	// get set and page offset in set; it's easy, so this function is not decoupled;
	uint64_t set_id = 99999;
	int page_offset = -1;
	int blk_offset = -1;
	// bool is_hbm = false;

	if(address  < _mem_hbm_size)
	{
		set_id = address  /  (_bumblebee_page_size * bumblebee_n);
		page_offset =  address  /  _bumblebee_page_size  % bumblebee_n;
		blk_offset = address  % _bumblebee_page_size / _bumblebee_blk_size;
	}
	else
	{
		set_id = (address - _mem_hbm_size) % _mem_hbm_size / ( bumblebee_n * _bumblebee_page_size);
		page_offset = (address - _mem_hbm_size) / _mem_hbm_size * bumblebee_n + (address - _mem_hbm_size) / _bumblebee_page_size % bumblebee_n;
		blk_offset = (address  - _mem_hbm_size)  % _bumblebee_page_size / _bumblebee_blk_size;
	}
	assert(99999 != set_id);
	assert(-1 != page_offset);

	PLEEntry& pleEntry =  MetaGrp[set_id]._pleEntry;
	g_vector<BLEEntry>& bleEntries =  MetaGrp[set_id]._bleEntries;
	HotnenssTracker& hotTracker = HotnessTable[set_id];
	uint64_t current_cycle = req.cycle;
	// should not trySwap Now

	hotTrackerDecrease(hotTracker,current_cycle);
	bool is_pop = shouldPop(hotTracker);
	int pop_pg_id = -1; 
	int pop_pg_idx = -1;
	if(is_pop)
	{
		QueuePage it = hotTracker.HBMQueue.back();
		pop_pg_id = it._page_id;
		for(int i = 0; i < (bumblebee_m + bumblebee_n);i++)
		{
			// find page
			if(pleEntry.PLE[i] == pop_pg_id)
			{
				pop_pg_idx = i;
				break;
			}
		}
	}
	
	// 记录bleEntries索引信息
	int ble_idx = -1;
	for(int i = 0;i<bumblebee_m+bumblebee_n;i++)
	{
		if(bleEntries[i].ple_idx == page_offset)
		{
			ble_idx = i;
			break;
		}
	}
	BLEEntry& bleEntry = bleEntries[ble_idx]; // 少写一个引用符号引发的血案！！

	bleEntry.cntr += 1;
	bleEntry.cntr -= (int)(current_cycle - bleEntry.l_cycle)/long_time;
	if(bleEntry.cntr <= 0) bleEntry.cntr = 0;
	bleEntry.l_cycle = current_cycle;


	BLEEntry popBleEntry;
	if(is_pop)
	{
		for(int i = 0;i<bumblebee_m+bumblebee_n;i++)
		{
			if(bleEntries[i].ple_idx == pop_pg_id)
			{
				popBleEntry = bleEntries[i];
				break;
			}
		}
	}

	int SL = hotTracker._na - hotTracker._nn - hotTracker._nc;
	bool hot_mem_flag = false;
	bool hot_cache_flag = false;
	int sl_state = 0;

	// 这里改来改去性能抖动爆炸了都
	if(SL > 0)
	{
		hot_mem_flag = true;
		sl_state = 1;
	}
	else if(SL <= 0) 
	{
		hot_cache_flag = true;
		sl_state = 2;
	}

	// search value(new PLE)
	int search_idx = -1;
	for(int i = 0; i < (bumblebee_m + bumblebee_n);i++)
	{
		// find page
		if(pleEntry.PLE[i] == page_offset)
		{
			search_idx = i;
			break;
		}
	}

	// PRT Miss   [2025/01/13] 调整了首次分配的逻辑
	if(-1 == search_idx)
	{
		// allocate ToDo：基于热度分配和空闲页面分配 可解耦一个函数
		// 如果最近分配的页面仍然驻留在热表队列中，并且有空闲的HBM空间可用，则该页面分配到HBM。否则，该页面应分配到片外DRAM。
		// 先根据空闲的HBM来吧
		int free_idx = -1;
		for(int i = 0;i < bumblebee_n ;i++)
		{
			if(pleEntry.Occupy[i]==0)
			{
				free_idx = i;
				break;		
			}
		}

		// 有空闲HBM
		if(-1 != free_idx)
		{
			if(page_offset < bumblebee_n && pleEntry.Occupy[page_offset]==0) free_idx = page_offset;
			pleEntry.PLE[free_idx] = page_offset;
			pleEntry.Occupy[free_idx] = 1;
			if(hot_mem_flag)pleEntry.Type[free_idx] = 1; // mHBM
			if(hot_cache_flag)pleEntry.Type[free_idx] = 2; // cHBM

			// now access
			Address dest_addr = set_id * bumblebee_n * _bumblebee_page_size + free_idx*_bumblebee_page_size + blk_offset * _bumblebee_blk_size;
			Address dest_hbm_address = (dest_addr / 64  /_mem_hbm_per_mc * 64) | (dest_addr % 64);
			uint32_t mem_hbm_select = (dest_addr / 64  ) % _mem_hbm_per_mc;
			req.lineAddr = dest_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;
			bleEntry.validVector[blk_offset] = 1;
			current_cycle = req.cycle;

			// 这个页表加入HBMQueue
			QueuePage _queuePage;
			_queuePage._page_id = page_offset; 
			_queuePage._counter += 1;
			_queuePage._last_mod_cycle = current_cycle;
			hotTracker.HBMQueue.push_front(_queuePage);
			// hotTracker.state has been decoupled

			// 看看是否要驱逐（支持的逻辑是我只有往HBMQueue新增Page，才有可能使得HBM占用比之前高）
			// 驱逐逻辑是在HBM占用率较高的情况下，HBM LRU Table 的计数器长时间保持不变
			hotTracker._rh += 1;
			if(hotTracker._rh > rh_upper && hot_mem_flag)
			{
				tryEvict(pleEntry,hotTracker,current_cycle,bleEntries,set_id,req,sl_state);
			}

			// 确认一下hotTracker的参数
			hotTrackerState(hotTracker,pleEntry);
			futex_unlock(&_lock);
			return req.cycle;
		}
		else // 没有空闲HBM：2025/01/10 逻辑重构：根据is_pop，去判断要不要去替换掉cHBM,否则是分配到DDR里
		{
			bleEntry.validVector[blk_offset] = 1;
			// 原来是DDR
			if(page_offset >= bumblebee_n)
			{
				// 原来的未被占用
				if(pleEntry.Occupy[page_offset]==0)
				{
					pleEntry.PLE[page_offset] = page_offset;
					pleEntry.Occupy[page_offset] = 1;
					pleEntry.Type[page_offset] = 0;

					// now access
					req.lineAddr = address;
					req.cycle = _ext_dram->access(req,0,4);
					req.lineAddr = tmpAddr;
					current_cycle = req.cycle;

					QueuePage _ddr_page;
					_ddr_page._page_id=page_offset;
					_ddr_page._counter=1;
					_ddr_page._last_mod_cycle=current_cycle;
					hotTracker.DRAMQueue.push_front(_ddr_page);

					bleEntry.validVector[blk_offset] = 1;
					// 确认一下hotTracker的参数
					hotTrackerState(hotTracker,pleEntry);
					futex_unlock(&_lock);
					return req.cycle;
				}
				else // 原来的被占用  状态位修改：DRAMQueue
				{ 
					int free_ddr = -1;
					for(int i = bumblebee_n; i <bumblebee_m + bumblebee_n;i++)
					{
						if(pleEntry.Occupy[i] == 0)
						{
							free_ddr = i;
							break;
						}
					}
					if(free_ddr != -1)
					{
						/* 这里好像之前写错了，修改【2025/03/03】*/
						// 改完性能确实好很多。 
						// pleEntry.PLE[free_idx] = page_offset;
						// pleEntry.Occupy[free_idx] = 1;
						// pleEntry.Type[free_idx] = 0;
						pleEntry.PLE[free_ddr] = page_offset;
						pleEntry.Occupy[free_ddr] = 1;
						pleEntry.Type[free_ddr] = 0;
						
						// Address dest_addr = _mem_hbm_size+(free_idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(free_idx%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
						Address dest_addr = _mem_hbm_size+(free_ddr-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(free_ddr%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
						req.lineAddr = dest_addr;
						req.cycle = _ext_dram->access(req,0,4);
						req.lineAddr = tmpAddr;
						current_cycle = req.cycle;
					}
					else
					{
						if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req); // ?
						pleEntry.PLE[page_offset] = page_offset;
						pleEntry.Occupy[page_offset] = 1;
						pleEntry.Type[page_offset] = 0;	

						Address dest_addr = _mem_hbm_size+(page_offset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(page_offset%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
						req.lineAddr = dest_addr;
						req.cycle = _ext_dram->access(req,0,4);
						req.lineAddr = tmpAddr;
						current_cycle = req.cycle;
					}
					QueuePage _ddr_page;
					_ddr_page._page_id=page_offset;
					_ddr_page._counter=1;
					_ddr_page._last_mod_cycle=current_cycle;
					hotTracker.DRAMQueue.push_front(_ddr_page);
					bleEntry.validVector[blk_offset] = 1;
					// 确认一下hotTracker的参数
					hotTrackerState(hotTracker,pleEntry);
					futex_unlock(&_lock);
					return req.cycle;
				}
			}
			else // 当函数进入这里，HBM本身就没有什么空间了
			{
				bleEntry.validVector[blk_offset] = 1;
				// bool should_find_ddr = false;
				
				if(is_pop) // 只有我可能pop出去(可以先pop对应cacheline)，我才有可能直接写在HBM里，否则直接分配DDR
				{
					// is_pop 代表了两种可能性
					// case 1: 原来的HBMType是Memory模式就切换为Cache模式，此时依然分配在DDR上
					// case 2: HBMType是Cache模式，此时需要写回valid数据（为什么不是脏数据呢？因为如果首次分配即在此处，LOAD的数据也是需要处理的）；那如果不是首次，大约的确是需要写回脏数据的；这个在实现上需要增加什么样的数据结构，待考虑；

					// alloc & access
					
					if(pleEntry.Type[pop_pg_idx]==2) // case 2
					{
						// check cacheline
						Address access_address = set_id * bumblebee_n * _bumblebee_page_size + pop_pg_idx*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
						Address ld_hbm_address = (access_address / 64 / _mem_hbm_per_mc * 64) | (access_address % 64);
						uint32_t mem_hbm_select = (access_address / 64)  % _mem_hbm_per_mc;
						req.lineAddr = ld_hbm_address;
						req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
						req.lineAddr = tmpAddr;
						// load
						for(int i = 0; i < blk_per_page ; i++)
						{
							if(popBleEntry.validVector[i] == 1) // 多了一次cacheline 浪费
							{
								Address ld_address = set_id * bumblebee_n * _bumblebee_page_size + pop_pg_idx*_bumblebee_page_size + i*_bumblebee_blk_size;
								Address ld_hbm_address =  (ld_address / 64 /_mem_hbm_per_mc * 64 ) | (ld_address % 64);
								uint32_t mem_hbm_select = (ld_address / 64)  % _mem_hbm_per_mc;
								MemReq load_req = {ld_hbm_address, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};;		
								_mcdram[mem_hbm_select]->access(load_req,2,4);
							}
						}
				
						pleEntry.PLE[pop_pg_idx] = page_offset;
						pleEntry.Occupy[pop_pg_idx] = 1;
						if(hot_mem_flag)pleEntry.Type[pop_pg_idx] = 1; 
						if(hot_cache_flag)pleEntry.Type[pop_pg_idx] = 2; 

						// 队列入队出队
						QueuePage _queuePage = hotTracker.HBMQueue.back();
						QueuePage _pushDramPage;
						_pushDramPage._page_id = _queuePage._page_id;
						_pushDramPage._counter = _queuePage._counter; // 0 or _cntr ?
						_pushDramPage._last_mod_cycle = current_cycle;
						hotTracker.HBMQueue.pop_back();
						hotTracker.HBMQueue.push_front(_pushDramPage);
						// asyn store
						// 首先需要有一个对应的DDR，需要找到一个空的DDR
						int get_dest_idx = -1;
						for(int fd_ddr = bumblebee_n;fd_ddr < bumblebee_m+bumblebee_n;++fd_ddr)
						{
							if(pleEntry.Occupy[fd_ddr]==0)
							{
								get_dest_idx = fd_ddr;
								break;
							}
						}
						assert(-1 != get_dest_idx);
						if(-1 != get_dest_idx)
						{
							for(int i = 0; i < blk_per_page ; i++)
							{
								if(popBleEntry.validVector[i] == 1)
								{
									Address dest_addr = _mem_hbm_size+(get_dest_idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(get_dest_idx%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
									MemReq store_req = {dest_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
									_ext_dram->access(store_req,2,4);
								}
							}

							pleEntry.PLE[get_dest_idx] = pop_pg_id;
							pleEntry.Occupy[get_dest_idx] = 1;
							pleEntry.Type[get_dest_idx] = 0; // trivial code
						}
											
						// 确认一下hotTracker的参数
						hotTrackerState(hotTracker,pleEntry);
						futex_unlock(&_lock);
						return req.cycle;
					}
					else if(pleEntry.Type[pop_pg_idx]==1)
					{
						// turn
						pleEntry.Type[pop_pg_idx] = 2;
						pleEntry.Occupy[pop_pg_idx] = 1; // trivial code
						// pleEntry.PLE[pop_pg_idx] = pop_pg_id; // trivial code
						// alloc ddr
						int get_dest_idx = -1;
						for(int fd_ddr = bumblebee_n;fd_ddr < bumblebee_m+bumblebee_n;++fd_ddr)
						{
							if(pleEntry.Occupy[fd_ddr]==0)
							{
								get_dest_idx = fd_ddr;
								break;
							}
						}
						
						// access
						if(-1 != get_dest_idx)
						{
							Address dest_ddr = _mem_hbm_size+(get_dest_idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(get_dest_idx%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
							MemReq alloc_req =  {dest_ddr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							req.cycle = _ext_dram->access(alloc_req,0,4);

							pleEntry.PLE[get_dest_idx] = page_offset;
							pleEntry.Occupy[get_dest_idx] = 1;
							pleEntry.Type[get_dest_idx] = 0; //trivial code

							// add to DRAMQueue
							QueuePage _push_dram_page;
							_push_dram_page._counter = 1;
							_push_dram_page._page_id = page_offset;
							_push_dram_page._last_mod_cycle = current_cycle;
							hotTracker.DRAMQueue.push_front(_push_dram_page);

						}
						else
						{
							assert(-1 != get_dest_idx);
						}

						hotTrackerState(hotTracker,pleEntry);
						futex_unlock(&_lock);
						return req.cycle;
					}
					
				}
				else // !pop,分配到空DDR上
				{
					int get_dest_idx = -1;
					for(int fd_ddr = bumblebee_n;fd_ddr < bumblebee_m+bumblebee_n;++fd_ddr)
					{
						if(pleEntry.Occupy[fd_ddr]==0)
						{
							get_dest_idx = fd_ddr;
							break;
						}
					}

					assert(-1 != get_dest_idx);

					pleEntry.PLE[get_dest_idx] = page_offset;
					pleEntry.Occupy[get_dest_idx] = 1;
					pleEntry.Type[get_dest_idx] = 0; //trivial code

					QueuePage _push_dram_page;
					_push_dram_page._counter = 1;
					_push_dram_page._page_id = page_offset;
					_push_dram_page._last_mod_cycle = current_cycle;
					hotTracker.DRAMQueue.push_front(_push_dram_page);

					Address acc_addr = _mem_hbm_size+(get_dest_idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(get_dest_idx%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
					req.lineAddr = acc_addr;
					req.cycle = _ext_dram->access(req,0,4);
					req.lineAddr = tmpAddr;

					hotTrackerState(hotTracker,pleEntry);
					futex_unlock(&_lock);
					return req.cycle;
				}
			}
		}
	}

	// PRT Hit
	int dest_mem_idx = search_idx;
	bool is_cache = pleEntry.Type[dest_mem_idx] == 2 ? true:false;
	bool block_hit = bleEntry.validVector[blk_offset] ? true:false;
			
	// 只有是cache模式，我才需要考虑是不是block_hit;才需要考虑需不需要设置dirtybit
	if(!is_cache)
	{
		if(dest_mem_idx < bumblebee_n)
		{
			Address dest_addr = set_id*bumblebee_n*_bumblebee_page_size + dest_mem_idx*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
			uint32_t mem_hbm_select = dest_addr / 64  % _mem_hbm_per_mc;
			Address mem_hbm_address = (dest_addr/ 64 / _mem_hbm_per_mc * 64 ) | (dest_addr % 64);
			req.lineAddr = mem_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;
			current_cycle = req.cycle;
			if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req); // 函数内置触发逻辑，直接调用
			hotTrackerState(hotTracker,pleEntry);
			bleEntry.validVector[blk_offset] = 1; // memory模式依然需要以防万一，因为随时可以切换cache模式
		}
		else
		{
			Address dest_addr = _mem_hbm_size+(page_offset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(page_offset%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
			req.lineAddr = dest_addr;
			req.cycle = _ext_dram->access(req,0,4);
			req.lineAddr = tmpAddr;
		    current_cycle = req.cycle;
			if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req);
			hotTrackerState(hotTracker,pleEntry);
			bleEntry.validVector[blk_offset] = 1; // memory模式依然需要以防万一，因为随时可以切换cache模式
		}
	}
	else // 是cache模式
	{
		// PRT Hit -> isCache -> Cacheline Hit 此时我才需要考虑dirtybit的设置。
		// case in HBM;
		if(block_hit)
		{
			if(dest_mem_idx < bumblebee_n)
			{
				Address dest_addr = set_id*bumblebee_n*_bumblebee_page_size + dest_mem_idx*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
				uint32_t mem_hbm_select = (dest_addr / 64)  % _mem_hbm_per_mc;
				Address mem_hbm_address = (dest_addr/64/ _mem_hbm_per_mc * 64 ) | (dest_addr % 64);
				req.lineAddr = mem_hbm_address;
				req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
				req.lineAddr = tmpAddr;
				current_cycle = req.cycle;
				if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req);
				hotTrackerState(hotTracker,pleEntry);
				if(type==STORE)bleEntry.dirtyVector[blk_offset] = 1;
				bleEntry.validVector[blk_offset] = 1; //以防万一
			}
			else
			{
				Address dest_addr = _mem_hbm_size+(page_offset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(page_offset%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
				req.lineAddr = dest_addr;
				req.cycle = _ext_dram->access(req,0,4);
				req.lineAddr = tmpAddr;
				current_cycle = req.cycle;
				if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req);
				hotTrackerState(hotTracker,pleEntry);
				if(type==STORE)bleEntry.dirtyVector[blk_offset] = 1;
				bleEntry.validVector[blk_offset] = 1; //以防万一
			}
		}
		else
		{
			// PRT Hit -> isCache -> Cacheline Miss 此时我才需要考虑是否需要load/store，writeback
			if(page_offset >= bumblebee_n) // cache DDR
			{
				Address dest_addr = _mem_hbm_size+(page_offset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(page_offset%bumblebee_n)*_bumblebee_page_size+blk_offset*_bumblebee_blk_size;
				// load & access
				req.lineAddr = dest_addr;
				req.cycle = _ext_dram->access(req,0,4);
				req.lineAddr = tmpAddr;
				current_cycle = req.cycle;
				
				// metadata upd
				bleEntry.validVector[blk_offset] = 1;
				
				// store
				Address sd_addr = set_id*bumblebee_n*_bumblebee_page_size + dest_mem_idx*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
				uint32_t mem_hbm_select = sd_addr / 64  % _mem_hbm_per_mc;
				Address mem_hbm_address = (sd_addr/ 64/ _mem_hbm_per_mc * 64 ) | (sd_addr % 64) ;
				MemReq store_req = {mem_hbm_address,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_mcdram[mem_hbm_select]->access(store_req,2,4);			
			}
			else // cache HBM: Only mHBM => cHBM
			{
				Address sd_addr = set_id*bumblebee_n*_bumblebee_page_size + dest_mem_idx*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
				uint32_t mem_hbm_select = (sd_addr / 64) % _mem_hbm_per_mc;
				Address mem_hbm_address = (sd_addr/64/ _mem_hbm_per_mc * 64 ) | (sd_addr % 64) ;
				req.lineAddr = mem_hbm_address;
				req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);		
				req.lineAddr = tmpAddr;
				current_cycle = req.cycle;

				// metadata upd
				bleEntry.validVector[blk_offset] = 1;
			}
			if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req);
			hotTrackerState(hotTracker,pleEntry);
		}
	}
	
	// 先更新QueuePage的counter
	// 两个Queue可以一起遍历，都是常数且一个数量级的开销

	//Notes: g_list继承std::list 迭代器指向元素本身；g_list为手动实现的list时，请使用引用类型 ！
	bool is_push = false;
	for(auto it = hotTracker.HBMQueue.begin();it != hotTracker.HBMQueue.end();++it)
	{
		if(it->_page_id == page_offset)
		{
			it->_counter += 1;
			it->_last_mod_cycle = current_cycle;
			break;
		}
		if(it == hotTracker.HBMQueue.end())
		{
			QueuePage _hbm_page;
			_hbm_page._counter = 1;
			_hbm_page._last_mod_cycle = current_cycle;
			_hbm_page._page_id = page_offset;
			hotTracker.HBMQueue.push_front(_hbm_page);
			is_push = true;
		}
	}
	// 每次push都有可能触发驱逐
	hotTracker._rh += 1;
	if(is_push && hotTracker._rh > rh_upper && hotTracker._nn < 2 && hot_mem_flag)
	{
		tryEvict(pleEntry,hotTracker,current_cycle,bleEntries,set_id,req,sl_state);
	}
	for(auto it = hotTracker.DRAMQueue.begin();it != hotTracker.DRAMQueue.end();++it)
	{
		if(it->_page_id == page_offset)
		{
			it->_counter += 1;
			it->_last_mod_cycle = current_cycle;
			break;
		}
		if(it == hotTracker.DRAMQueue.end())
		{
			QueuePage _dram_page;
			_dram_page._counter = 1;
			_dram_page._last_mod_cycle = current_cycle;
			_dram_page._page_id = page_offset;
			hotTracker.DRAMQueue.push_front(_dram_page);
		}
	}
	if(SL > 0)trySwap(pleEntry,hotTracker,MetaGrp[set_id],current_cycle,set_id,req);
	futex_unlock(&_lock);
	return req.cycle;
}


/**
 * @brief 根据当前cycle和上一次修改的cycle进行热度衰减
 */
void
MemoryController::hotTrackerDecrease(HotnenssTracker& hotTracker,uint64_t current_cycle)
{
	uint64_t cntr_decrease = (current_cycle - hotTracker._last_mod_cycle) / long_time;
	if(cntr_decrease > 0)
	{
		for(auto it = hotTracker.HBMQueue.begin();it != hotTracker.HBMQueue.end();++it)
		{
			it->_counter -= cntr_decrease;
			if(it->_counter < 0) it->_counter = 0;
		}
		
		for(auto it = hotTracker.DRAMQueue.begin();it != hotTracker.DRAMQueue.end();++it)
		{
			it->_counter -= cntr_decrease;
			if(it->_counter < 0) it->_counter = 0;
		}
	
		hotTracker._last_mod_cycle = current_cycle;
	}
}

/**
 * @brief: 根据当前hotTracker的end所指的page的热度（是否小于等于0，判断是否需要踢出）
 */
bool
MemoryController::shouldPop(HotnenssTracker& hotTracker)
{
	bool should_be_pop = false;
	QueuePage it = hotTracker.HBMQueue.back();
	if(it._counter <= 0) should_be_pop = true;
	return should_be_pop;
}

/**
 * @brief 返回当前Set已分配HBM内存页面情况
 */
int
MemoryController::ret_hbm_occupy(MetaGrpEntry& set)
{
	int occ = 0;

	for(int i = 0; i< bumblebee_n;i++)
	{
		if(set._pleEntry.Occupy[i]==1);
		occ += 1;
	}

	return occ;
}

/**
 * @brief 返回HBMQueue最冷数据的page_id
 */
std::pair<int,uint64_t>
MemoryController::find_coldest(HotnenssTracker& hotTracker)
{
	std::pair<int,uint64_t> p0;
	p0 = std::make_pair(0,0);
	if(hotTracker.HBMQueue.size()<= 0)return p0;
	uint64_t cold_cntr = 100000; // 这个值会设置过低吗？
	int ret_pg_id = -1;
	for(auto it = hotTracker.HBMQueue.begin();it != hotTracker.HBMQueue.end();++it)
	{
		if(it->_counter < cold_cntr)
		{
			ret_pg_id = it->_page_id;
			cold_cntr = it->_counter;
		}
	}
	// assert(-1 != ret_pg_id); // 
	std::pair<int,uint64_t> p1 = std::make_pair(ret_pg_id,cold_cntr);
	return p1;
}

/**
 * @brief 返回DRAMQueue最热数据的page_id
 */
std::pair<int,uint64_t>
MemoryController::find_hottest(HotnenssTracker& hotTracker)
{
	std::pair<int,uint64_t> p0;
	p0 = std::make_pair(0,0);
	if(hotTracker.DRAMQueue.size()<= 0)return p0;
	uint64_t hot_cntr = 0;
	int ret_pg_id = -1;
	for(auto it = hotTracker.HBMQueue.begin();it != hotTracker.HBMQueue.end();++it)
	{
		if(it->_counter > hot_cntr)
		{
			ret_pg_id = it->_page_id;
			hot_cntr = it->_counter;
		}
	}
	// assert(-1 != ret_pg_id); //
	std::pair<int,uint64_t> p1 = std::make_pair(ret_pg_id,hot_cntr);
	return p1;
}

/**
 * @brief 尝试交换Queue最热最冷页面，触发条件：nc=0;ret_hbm_occupy=N
 */
void
MemoryController::trySwap(PLEEntry& pleEntry,HotnenssTracker& hotTracker,MetaGrpEntry& set,uint64_t current_cycle,uint64_t set_id,MemReq& req)
{
	if(ret_hbm_occupy(set) < bumblebee_n) return;
	if(hotTracker._nc > 0) return;
	int cold_pg_id = find_coldest(hotTracker).first;
	uint64_t cold_cntr = find_coldest(hotTracker).second;
	int hot_pg_id = find_hottest(hotTracker).first;
	uint64_t hot_cntr = find_hottest(hotTracker).second;
	if(0 >= cold_pg_id * hot_pg_id) return;
	if(hot_cntr < cold_cntr + 10)return;

	QueuePage _push_dram_page;
	_push_dram_page._page_id = -1;
	QueuePage _push_hbm_page;
	_push_hbm_page._page_id = -1;

	for(auto it = hotTracker.HBMQueue.begin();it != hotTracker.HBMQueue.end();++it)
	{
		if(it->_page_id == cold_pg_id)
		{
			_push_dram_page._page_id = cold_pg_id;
			_push_dram_page._last_mod_cycle = current_cycle;
			_push_dram_page._counter = it->_counter;
			it = hotTracker.HBMQueue.erase(it);
			break;
		}
	}

	if(-1 == _push_dram_page._page_id) return;

	for(auto it = hotTracker.DRAMQueue.begin();it != hotTracker.DRAMQueue.end();++it)
	{
		if(it->_page_id == hot_pg_id)
		{
			_push_hbm_page._page_id = hot_pg_id;
			_push_hbm_page._last_mod_cycle = current_cycle;
			_push_hbm_page._counter = it->_counter;
			it = hotTracker.DRAMQueue.erase(it);
			break;
		}
	}

	if(-1 == _push_hbm_page._page_id)return;

	hotTracker.DRAMQueue.push_front(_push_dram_page);
	hotTracker.HBMQueue.push_front(_push_hbm_page);

	int p1_idx = -1;
	int p2_idx = -1;

	for(int i = 0; i < bumblebee_n + bumblebee_m;i++)
	{
		if(pleEntry.PLE[i]==cold_pg_id)p1_idx = i;
		if(pleEntry.PLE[i]==hot_pg_id)p2_idx = i;
		if(p1_idx != -1 && p2_idx != -1)break;
	}

	assert(-1 != p1_idx);
	assert(-1 != p2_idx);

	pleEntry.PLE[p1_idx] = hot_pg_id;
	pleEntry.Occupy[p1_idx] = 1;
	pleEntry.Type[p1_idx] = 1;

	pleEntry.PLE[p2_idx] = cold_pg_id;
	pleEntry.Occupy[p2_idx] = 1;
	pleEntry.Type[p2_idx]= 0; // trivial code

	
	Address hbm_pg_addr = set_id * bumblebee_n * _bumblebee_page_size + p1_idx*_bumblebee_page_size;
	// 非局部性组织
	// Address ddr_pg_addr = _mem_hbm_size + set_id * bumblebee_m * _bumblebee_page_size + (p2_idx - bumblebee_n)*_bumblebee_page_size;
	// 局部性组织
	Address ddr_pg_addr = _mem_hbm_size+(p2_idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(p2_idx%bumblebee_n)*_bumblebee_page_size;
	MESIState state;
	// load d/h
	for(int i = 0 ;i < blk_per_page ; i++)
	{
		Address ddr_blk_addr = ddr_pg_addr + i * _bumblebee_blk_size;
		MemReq load_ddr_req = {ddr_blk_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_ext_dram->access(load_ddr_req,2,4);

		Address hbm_blk_addr = hbm_pg_addr + i *_bumblebee_blk_size;
		Address hbm_addr =(hbm_blk_addr / 64 / _mcdram_per_mc * 64) | (hbm_blk_addr % 64);
		uint32_t mem_hbm_select = hbm_blk_addr / 64 % _mcdram_per_mc;
		MemReq load_hbm_req = {hbm_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_mcdram[mem_hbm_select]->access(load_hbm_req,2,4);

		MemReq store_hbm_req = {hbm_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_mcdram[mem_hbm_select]->access(store_hbm_req,2,4);

		MemReq store_ddr_req = {ddr_blk_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_ext_dram->access(store_ddr_req,2,4);
	}

	return;
}

/**
 * @brief 返回当前Set已分配内存页面情况
 */
int
MemoryController::ret_set_alloc_state(MetaGrpEntry& set)
{
	set.set_alloc_page = 0;
	for(int i = 0; i< bumblebee_n+bumblebee_m;i++)
	{
		if(set._pleEntry.Occupy[i]==1)
		set.set_alloc_page += 1;
	}
	return set.set_alloc_page;
}


/**
 * @brief BATMAN in Flat Mode with a pro/demotion function similar to Chameleon
 * @attention 确实做到了基本上80%的访问是HBM的,20%的访问是DRAM的,但是性能上不如预期
 */
uint64_t
MemoryController::batman_access(MemReq& req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	
	if (req.type == PUTS)
	{
		return req.cycle;
	}

	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	Address address = req.lineAddr;
	uint64_t current_cycle = req.cycle;
	MESIState state;
	int tag_size = 2; // indicates 2*16
	uint64_t stable_longtime = 1000;

	// 衰减，怎么衰减? 还是定期置空？
	// V1 等比例缩放 ； V2 FINE TUNE ； V3 置空
	if(current_cycle - lst_md_cycle >= stable_longtime * long_time)
	{
		lst_md_cycle = current_cycle;

		// Version 1 
		// nm_access = nm_access / 2;
		// total_access = total_access / 2;

		// Version 2 deepsjeng 0.5667
		if(current_tar < TAR - guard_band) // 当前tar小于TAR,上一个period DRAM访问多，current_tar向0置空
		{
			nm_access = 0;
			total_access = 0;
			current_tar = 0;
		}
		else if(current_tar > TAR + guard_band) // 当前tar大于TAR，上一个period HBM访问多，分子分母等比例缩放，这样继续访问HBM，就会使得tar增长速度更快，避免大基数调节的滞后
		{
			nm_access = nm_access / 8;
			total_access = total_access / 8;
			current_tar = (float)nm_access/total_access;
		}

		// Version 3
		// nm_access = 0;
		// total_access = 0;
	}

	bool swap_banned = false;
	if(current_tar >= TAR - guard_band && current_tar <= TAR + guard_band)swap_banned = true;
	int set_id = -1;
	int page_offset = -1;
	int blk_offset = -1;
	uint64_t total_latency = 0;
	// metadata can be read/write parellel in two pasedo channle
	uint64_t look_up_XTA_wt_latency =  _mcdram[0]->wt_dram_tag_latency(req,tag_size/2);
	uint64_t look_up_XTA_rd_latency =  _mcdram[0]->rd_dram_tag_latency(req,tag_size/2);
	bool look_up_mem_metadata = false;

	if(address < _mem_hbm_size)
	{
		set_id = address / _batman_page_size;
		page_offset = 8; // 0 => 8 
		blk_offset = address % _batman_page_size / _batman_blk_size;
	}
	else
	{
		// 局部性更好的计算方式  （更换计算方式需要记得修改生成的req的lineaddr的计算方式）
		set_id = (address - _mem_hbm_size) % _mem_hbm_size / _batman_page_size;
		page_offset = (address - _mem_hbm_size) / _mem_hbm_size;
		blk_offset = (address  - _mem_hbm_size)  %  _batman_page_size / _batman_blk_size;
	}


	assert(-1 != set_id);


	if(address < _mem_hbm_size)
	{
		// HBM未使用，或已使用且是其本身
		if(b_sets[set_id].occupy == 0 || (b_sets[set_id].occupy == 1 && b_sets[set_id].remap_idx == batman_ddr_ratio))
		{
			// access & alloc HBM
			b_sets[set_id].occupy = 1;
			Address dest_addr = address;
			Address dest_hbm_address = (dest_addr / 64  /_mem_hbm_per_mc * 64) | (dest_addr % 64);
			uint32_t mem_hbm_select = (dest_addr / 64  ) % _mem_hbm_per_mc;
			req.lineAddr = dest_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;
			total_latency += req.cycle;
			total_latency += look_up_XTA_rd_latency; // occupy and r_idx
			
			// std::cout << "Access HBM" << std::endl;

			nm_access += 1;
			total_access += 1;
			current_tar = (float)nm_access/total_access;

			b_sets[set_id].cntr += 1;
			b_sets[set_id].init_hbm_cntr += 1; // 启动8idx的时候 这个还有意义吗?
			b_sets[set_id].dram_pages_cntr[batman_ddr_ratio] = b_sets[set_id].init_hbm_cntr;


			// 当前validbit更新
			b_sets[set_id].validBitMap[batman_ddr_ratio][blk_offset]=1;

			// 由于nm access了，可能导致潜在的tar超出阈值，基于BATMAN的带宽分配，考虑分散HBM热度
			if(b_sets[set_id].occupy == 1 && current_tar > TAR + guard_band &&  b_sets[set_id].cntr <= bt_hot) // 过热数据不驱逐
			{
				// compare cntr to decide swapping
				// 找页面
				int max_optimal_idx = -1;
				int max_cntr = 0;
				for(int i = 0 ;i < batman_ddr_ratio ;i++)
				{
					// 比HBM Page冷的最热的页面
					if(b_sets[set_id].cntr > b_sets[set_id].dram_pages_cntr[i] && b_sets[set_id].dram_pages_cntr[i] > static_cast<uint64_t>(max_cntr))
					{
						max_optimal_idx = i; // 实际页面
						max_cntr = b_sets[set_id].dram_pages_cntr[i];
					}
				}

				int exact_idx = -1; //实际索引
				for(int i = 0;i < batman_ddr_ratio;i++)
				{
					if(b_sets[set_id].bat_set_idx[i]==max_optimal_idx)
					{
						exact_idx = i; 
						break;
					}
				}

				// 存在就交换
				if(-1 != exact_idx && !swap_banned) // 只有存在这种页面，就交换以减少NM访问
				{
					// if(max_optimal_idx == -1)max_optimal_idx = batman_ddr_ratio; // 
					look_up_mem_metadata = true;
					/**	
					 * e.g.
					 * HBM[Page[6]] <- swap -> DRAM[4]->Page[3]
					 * ==> HBM[Page[3]] DRAM[4]->Page[6]
					 */
					int temp = b_sets[set_id].bat_set_idx[exact_idx];
					b_sets[set_id].bat_set_idx[exact_idx] = b_sets[set_id].remap_idx; // DRAM[4]->Page[3] => DRAM[4]->Page[6]
					b_sets[set_id].remap_idx = temp; // HBM[Page[6]]
					b_sets[set_id].cntr = b_sets[set_id].dram_pages_cntr[max_optimal_idx]; // 更换热度
					

					for(int i = 0;i < blk_per_page ;i++)
					{
						Address hbm_addr = set_id * _batman_page_size + i*_batman_blk_size;
						Address hbm_address = (hbm_addr / 64  /_mem_hbm_per_mc * 64) | (hbm_addr % 64);
						uint32_t hbm_select = (hbm_addr / 64  ) % _mem_hbm_per_mc;
						if(b_sets[set_id].validBitMap[batman_ddr_ratio][i]==1)
						{
							// load from hbm
							MemReq load_req = {hbm_address, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_mcdram[hbm_select]->access(load_req,2,4);

							// store to dram
							// Address store_addr = _mem_hbm_size   + (set_id * 8 + exact_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							Address store_addr = _mem_hbm_size + exact_idx*_mem_hbm_size + set_id * _batman_page_size + i*_batman_blk_size; // 局部性写法
							MemReq store_req = {store_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_ext_dram->access(store_req,2,4);
						}
						assert(-1 != max_optimal_idx);
						if(b_sets[set_id].validBitMap[max_optimal_idx][i]==1)
						{
							// load from dram
							// Address load_ddr = _mem_hbm_size   + (set_id * 8 + exact_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							Address load_ddr = _mem_hbm_size + exact_idx*_mem_hbm_size + set_id * _batman_page_size + i*_batman_blk_size; // 局部性写法
							MemReq load_ddr_req = {load_ddr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_ext_dram->access(load_ddr_req,2,4);

							// store to hbm
							MemReq store_hbm_req = {hbm_address, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_mcdram[hbm_select]->access(store_hbm_req,2,4);
						}		
					}
				}
			}
			if(look_up_mem_metadata)total_latency += look_up_XTA_wt_latency;
			futex_unlock(&_lock);
			return total_latency;
		}
		else // 访问的初始地址是HBM，现在HBM存的不是这个地址的页面；比较热度时即比较初始HBM页热度和当前HBM页热度
		{
			assert(b_sets[set_id].occupy == 1 && b_sets[set_id].remap_idx != batman_ddr_ratio);
			int get_remap_idx = -1; // HBMPage 实际索引位置
			for(int i = 0 ; i <= batman_ddr_ratio ;i++) // ?? <=
			{
				if(b_sets[set_id].bat_set_idx[i] == batman_ddr_ratio)
				{
					get_remap_idx = i;
					break;
				}
			}

			assert(-1 != get_remap_idx);
			if(get_remap_idx == batman_ddr_ratio) get_remap_idx = 0;
			// 当前时会触发assertion failed
			// 在HBM Page未分配 DDR Page 可以迁移到HBM上，此处需要加上逻辑

			// Address dest_addr = _mem_hbm_size + (set_id * 8 + get_remap_idx) * _batman_page_size + blk_offset*_batman_blk_size; // 非局部性写法
			Address dest_addr = _mem_hbm_size + get_remap_idx * _mem_hbm_size + set_id * _batman_page_size + blk_offset*_batman_blk_size; // 局部性写法
			req.lineAddr = dest_addr;
			req.cycle = _ext_dram->access(req,0,4); 
			req.lineAddr = tmpAddr;
			total_latency += req.cycle;
			total_latency += look_up_XTA_rd_latency;

			// std::cout << "Access DRAM" << std::endl;
			total_access += 1;
			current_tar = (float)nm_access/total_access;
			b_sets[set_id].init_hbm_cntr += 1;


			// 当前validbit更新
			b_sets[set_id].validBitMap[batman_ddr_ratio][blk_offset]=1;

			// NM热度不够，才考虑当前页面是不是需要移到HBM
			if(current_tar < TAR - guard_band && !swap_banned)
			{
				bool is_swap = b_sets[set_id].init_hbm_cntr > b_sets[set_id].cntr ? true:false;
				if(is_swap)
				{		
					// load from dram,hbm
					for(int i = 0;i < blk_per_page;i++)
					{
						if(b_sets[set_id].validBitMap[batman_ddr_ratio][i]==1) // dram(init-hbm) blk valid
						{
							Address load_addr = _mem_hbm_size + get_remap_idx*_mem_hbm_size + set_id*_batman_page_size + i*_batman_blk_size; // 局部性写法
							// Address ld_addr = _mem_hbm_size + (set_id * 8 + get_remap_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							MemReq load_req = {load_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(load_req,2,4);
						}

						if(b_sets[set_id].validBitMap[b_sets[set_id].remap_idx][i]==1) // hbm(init-dram) blk valid
						{
							Address ld_addr = set_id * _batman_page_size + i*_batman_blk_size;
							ld_addr = (ld_addr / 64  /_mem_hbm_per_mc * 64) | (ld_addr % 64);
							uint32_t ld_hbm_select = (ld_addr / 64  ) % _mem_hbm_per_mc;
							MemReq ld_req = {ld_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_mcdram[ld_hbm_select]->access(ld_req,2,4);
						}
					}

					// store to hbm dram
					for(int i = 0;i < blk_per_page;i++)
					{
						if(b_sets[set_id].validBitMap[batman_ddr_ratio][i]==1) // dram(init-hbm) blk valid
						{
							Address store_addr = set_id * _batman_page_size + i*_batman_blk_size;
							store_addr = (store_addr / 64  /_mem_hbm_per_mc * 64) | (store_addr % 64);
							uint32_t sd_hbm_select = (store_addr / 64  ) % _mem_hbm_per_mc;
							MemReq store_req = {store_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_mcdram[sd_hbm_select]->access(store_req,2,4);
						}

						if(b_sets[set_id].validBitMap[b_sets[set_id].remap_idx][i]==1) // hbm(init-dram) blk valid
						{
							Address sd_addr = _mem_hbm_size + get_remap_idx*_mem_hbm_size + set_id*_batman_page_size + i*_batman_blk_size; // 局部性写法
							// Address ld_addr = _mem_hbm_size + (set_id * 8 + get_remap_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							MemReq sd_req = {sd_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(sd_req,2,4);
						}
					}

					// state
					b_sets[set_id].dram_pages_cntr[b_sets[set_id].remap_idx] = b_sets[set_id].cntr; // 这个似乎没有必要
					b_sets[set_id].cntr = b_sets[set_id].init_hbm_cntr; // 热度变回原来的page热度
					b_sets[set_id].dram_pages_cntr[batman_ddr_ratio] = b_sets[set_id].init_hbm_cntr; // 一起修改
					b_sets[set_id].bat_set_idx[get_remap_idx] = b_sets[set_id].remap_idx; // HBM所在位置索引指向新的页面
					b_sets[set_id].remap_idx = batman_ddr_ratio; // HBM指向自己
					total_latency += look_up_XTA_wt_latency;
				}
			}
			futex_unlock(&_lock);
			return total_latency;
		}
	}
	else // address >= _mem_hbm_size
	{
		if(b_sets[set_id].remap_idx == page_offset)
		{
			// access HBM
			Address dest_addr = set_id * _batman_page_size + blk_offset * _batman_blk_size;
			Address dest_hbm_address = (dest_addr / 64  /_mem_hbm_per_mc * 64) | (dest_addr % 64);
			uint32_t mem_hbm_select = (dest_addr / 64  ) % _mem_hbm_per_mc;
			req.lineAddr = dest_hbm_address;
			req.cycle = _mcdram[mem_hbm_select]->access(req,0,4);
			req.lineAddr = tmpAddr;
			total_latency += req.cycle;
			total_latency += look_up_XTA_rd_latency;

			// std::cout << "Access HBM" << std::endl;

			nm_access += 1;
			total_access += 1;
			current_tar = (float)nm_access/total_access;
			b_sets[set_id].dram_pages_cntr[page_offset] += 1;
			b_sets[set_id].validBitMap[page_offset][blk_offset] = 1;

			if(current_tar > TAR + guard_band && b_sets[set_id].cntr <= bt_hot && !swap_banned)
			{
				// compare cntr to decide swap
				int max_optimal_idx = -1;
				int max_cntr = 0;
				for(int i = 0 ;i <= batman_ddr_ratio ;i++)
				{
					if(i == page_offset)continue;
					// 比HBM Page冷的最热的页面
					if(b_sets[set_id].cntr > b_sets[set_id].dram_pages_cntr[i] && b_sets[set_id].dram_pages_cntr[i] >static_cast<uint64_t>(max_cntr))
					{
						max_optimal_idx = i; // 实际页面
						max_cntr = b_sets[set_id].dram_pages_cntr[i];
					}
				}

				int exact_idx = -1; //实际索引

				for(int i = 0;i <= batman_ddr_ratio;i++)
				{
					if(b_sets[set_id].bat_set_idx[i]==max_optimal_idx)
					{
						exact_idx = i; 
						break;
					}
				}

				if(-1 != exact_idx && max_optimal_idx != -1)
				{
					assert(-1 != max_optimal_idx); // 逻辑更新后,只要exact_idx存在,这就必定不可能是-1
					// if(max_optimal_idx == -1)max_optimal_idx = batman_ddr_ratio; // 
					look_up_mem_metadata = true;
					b_sets[set_id].bat_set_idx[exact_idx] = b_sets[set_id].remap_idx; // DRAM[4]->Page[3] => DRAM[4]->Page[6]
					b_sets[set_id].remap_idx = max_optimal_idx; // HBM[Page[6]]
					b_sets[set_id].cntr = b_sets[set_id].dram_pages_cntr[max_optimal_idx]; // 更换热度

					for(int i = 0;i < blk_per_page ;i++)
					{
						Address hbm_addr = set_id * _batman_page_size + i*_batman_blk_size;
						Address hbm_address = (hbm_addr / 64  /_mem_hbm_per_mc * 64) | (hbm_addr % 64);
						uint32_t hbm_select = (hbm_addr / 64  ) % _mem_hbm_per_mc;
						if(b_sets[set_id].validBitMap[max_optimal_idx][i]==1)
						{
							// load from hbm
							MemReq load_req = {hbm_address, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_mcdram[hbm_select]->access(load_req,2,4);

							// store to dram
							// Address store_addr = _mem_hbm_size   + (set_id * 8 + exact_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							Address store_addr = _mem_hbm_size + exact_idx*_mem_hbm_size + set_id * _batman_page_size + i*_batman_blk_size; // 局部性写法
							MemReq store_req = {store_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_ext_dram->access(store_req,2,4);
						}

						if(b_sets[set_id].validBitMap[max_optimal_idx][i]==1)
						{
							// load from dram
							// Address load_ddr = _mem_hbm_size   + (set_id * 8 + exact_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							Address load_ddr = _mem_hbm_size + exact_idx*_mem_hbm_size + set_id * _batman_page_size + i*_batman_blk_size; // 局部性写法
							MemReq load_ddr_req = {load_ddr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_ext_dram->access(load_ddr_req,2,4);

							// store to hbm
							MemReq store_hbm_req = {hbm_address, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_mcdram[hbm_select]->access(store_hbm_req,2,4);
						}		
					}
					
				}
			}
			if(look_up_mem_metadata)total_latency += look_up_XTA_wt_latency;
			futex_unlock(&_lock);
			return total_latency;
		}
		else
		{
			// access DRAM
			req.lineAddr = address;
			req.cycle = _ext_dram->access(req,0,4);
			req.lineAddr = tmpAddr;
			total_latency += req.cycle;
			total_latency += look_up_XTA_rd_latency;
			// std::cout << "Access DRAM" << std::endl;

			total_access += 1;
			current_tar = (float)nm_access/total_access;
			b_sets[set_id].dram_pages_cntr[page_offset] += 1;
			b_sets[set_id].validBitMap[page_offset][blk_offset] += 1;

			int dram_idx = -1;
			for(int i = 0; i <= batman_ddr_ratio; i++)
			{
				if(b_sets[set_id].bat_set_idx[i] == page_offset)
				{
					dram_idx = i;
					break;
				}
			}

			assert(-1 != dram_idx); // Failed Assertion [fixed]

		
			if(current_tar < TAR - guard_band && !swap_banned)
			{
				// compare to decide swapping , migrating 
				if(b_sets[set_id].occupy == 0) // migrate
				{
					for(int i = 0; i < blk_per_page; i++)
					{
						if(b_sets[set_id].validBitMap[page_offset][i]==1)
						{
							Address load_addr = _mem_hbm_size + dram_idx*_mem_hbm_size + set_id*_batman_page_size + i*_batman_blk_size; // 局部性写法
							// Address load_addr = _mem_hbm_size + ( set_id * 8 + dram_idx ) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
							MemReq load_ddr_req = {load_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(load_ddr_req,2,4);

							Address store_addr = set_id * _batman_page_size + i*_batman_blk_size;
							store_addr = (store_addr / 64 / _mem_hbm_per_mc * 64) | (store_addr % 64);
							uint32_t hbm_select = (store_addr / 64) % _mem_hbm_per_mc;
							MemReq store_hbm_req = {store_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};	
							_mcdram[hbm_select]->access(store_hbm_req,2,4); 	
						}
					}
					look_up_mem_metadata = true;
					b_sets[set_id].cntr = b_sets[set_id].dram_pages_cntr[page_offset];
					b_sets[set_id].remap_idx = page_offset;
					b_sets[set_id].occupy = 1;
					b_sets[set_id].bat_set_idx[dram_idx] = batman_ddr_ratio;
					b_sets[set_id].init_hbm_cntr = 0;
				}
				else
				{
					// compare to decide whether to swap or not ????
					// 当前访问的是DDR地址 DDR未被HBM存 判断是否需要与HBMPage 交换
					bool is_swap = b_sets[set_id].dram_pages_cntr[page_offset] > b_sets[set_id].cntr ? true : false;
					// bool is_swap = b_sets[set_id].init_hbm_cntr > b_sets[set_id].cntr ? true:false;  // Error writing
					if(is_swap)
					{		
						int get_remap_idx = dram_idx;
						// load from dram,hbm
						for(int i = 0; i < blk_per_page; i++)
						{
							if(b_sets[set_id].validBitMap[page_offset][i]==1) // 
							{
								Address load_addr = _mem_hbm_size + get_remap_idx*_mem_hbm_size + set_id*_batman_page_size + i*_batman_blk_size; // 局部性写法
								// Address ld_addr = _mem_hbm_size + (set_id * 8 + get_remap_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
								MemReq load_req = {load_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(load_req,2,4);
							}

							if(b_sets[set_id].validBitMap[b_sets[set_id].remap_idx][i]==1) // 
							{
								Address ld_addr = set_id * _batman_page_size + i*_batman_blk_size;
								ld_addr = (ld_addr / 64  /_mem_hbm_per_mc * 64) | (ld_addr % 64);
								uint32_t ld_hbm_select = (ld_addr / 64  ) % _mem_hbm_per_mc;
								MemReq ld_req = {ld_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[ld_hbm_select]->access(ld_req,2,4);
							}
						}

						// store to hbm dram
						for(int i = 0;i < blk_per_page;i++)
						{
							if(b_sets[set_id].validBitMap[page_offset][i]==1) // dram(init-hbm) blk valid
							{
								Address store_addr = set_id * _batman_page_size + i*_batman_blk_size;
								store_addr = (store_addr / 64  /_mem_hbm_per_mc * 64) | (store_addr % 64);
								uint32_t sd_hbm_select = (store_addr / 64  ) % _mem_hbm_per_mc;
								MemReq store_req = {store_addr, PUTX, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[sd_hbm_select]->access(store_req,2,4);
							}

							if(b_sets[set_id].validBitMap[b_sets[set_id].remap_idx][i]==1) // hbm(init-dram) blk valid
							{
								Address sd_addr = _mem_hbm_size + get_remap_idx*_mem_hbm_size + set_id*_batman_page_size + i*_batman_blk_size; // 局部性写法
								// Address ld_addr = _mem_hbm_size + (set_id * 8 + get_remap_idx) * _batman_page_size + i*_batman_blk_size; // 非局部性写法
								MemReq sd_req = {sd_addr, GETS, req.childId,&state,req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(sd_req,2,4);
							}
						}

						// state
						// b_sets[set_id].dram_pages_cntr[page_offset] = b_sets[set_id].cntr; // 这个似乎没有必要
						b_sets[set_id].cntr = b_sets[set_id].dram_pages_cntr[page_offset]; // 热度变回原来的page热度
						b_sets[set_id].bat_set_idx[get_remap_idx] = b_sets[set_id].remap_idx; // HBM所在位置索引指向新的页面
						b_sets[set_id].remap_idx = page_offset; // HBM指向自己 ..xx
						look_up_mem_metadata = true;
					}
				}
			}
			if(look_up_mem_metadata)total_latency += look_up_XTA_wt_latency;
			futex_unlock(&_lock);
			return total_latency;
		}
	}
}

uint64_t 
MemoryController::direct_flat_access(MemReq& req)
{
	switch (req.type)
	{
	case PUTS:
	case PUTX:
		*req.state = I;
		break;
	case GETS:
		*req.state = req.is(MemReq::NOEXCL) ? S : E;
		break;
	case GETX:
		*req.state = M;
		break;
	default:
		panic("!?");
	}
	
	if (req.type == PUTS)
	{
		return req.cycle;
	}

	Address tmpAddr = req.lineAddr;
	req.lineAddr = vaddr_to_paddr(req);
	Address address = req.lineAddr;

	_flat_access_cntr[address / _mem_hbm_size] += 1;
	_flat_access_print_time += 1;
	if(_flat_access_print_time % 10000 == 0)
	{
		_flat_access_print_time = 0;
		std::cout<<"Flat access counter:"<<_mem_hbm_size<<std::endl;
		for(int i = 0; i < 9; i++) std::cout<<_flat_access_cntr[i]<<" ";
		std::cout<<std::endl;
	}
	

	if(address > phy_mem_size - _mem_hbm_size)
	{
		Address mc_address = (address / 64) % (_mem_hbm_size / _mcdram_per_mc);
		//(address  / 64  / _mcdram_per_mc * 64) | (address % 64);
		uint64_t mcdram_select = address / 64 % _mcdram_per_mc;
		req.lineAddr = mc_address;
		req.cycle = _mcdram[mcdram_select]->access(req,0,4);
		req.lineAddr = tmpAddr;
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
	}
	else
	{
		req.lineAddr = tmpAddr;
		req.cycle =_ext_dram->access(req,0,4);
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
	}

}

/**
 * @brief bumblebee结构中解耦的获取地址的计算方式。
 * @param idx: 当前实际存的索引
 * @param page_offset:地址计算的偏移
 */
Address
MemoryController::getDestAddress(uint64_t set_id,int idx,int page_offset,int blk_offset)
{
	Address dest_address = 1;
	if(idx >= bumblebee_n) // indicates dram
	{
		// 非局部性组织
		// dest_address = _mem_hbm_size + set_id * bumblebee_m * _bumblebee_page_size + (idx - bumblebee_n)*_bumblebee_page_size + blk_offset*_bumblebee_blk_size;
		// 局部性组织
		dest_address = _mem_hbm_size+(idx-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(idx%bumblebee_n)*_bumblebee_page_size+ blk_offset*_bumblebee_blk_size;
	}
	else // indicates HBM
	{
		dest_address = set_id * bumblebee_n * _bumblebee_page_size + idx*_bumblebee_page_size  + blk_offset * _bumblebee_blk_size;
	}
	return dest_address;
}

/**
 * @brief 更新hotTracker对应的参数状态
 */
void 
MemoryController::hotTrackerState(HotnenssTracker& hotTracker,PLEEntry& pleEntry)
{
	// 确认一下hotTracker的参数
	hotTracker._rh = 0;
	// hotTracker._nn = bumblebee_n;
	hotTracker._na = 0;
	hotTracker._nc = 0;
	for(int i = 0;i < bumblebee_n; i++)
	{
		if(pleEntry.Type[i]==2) // indicates cHBM
		{
			hotTracker._nc += 1;
			if(pleEntry.Occupy[i]==1)hotTracker._rh += 1;
		}				
		else if(pleEntry.Type[i]==1 && pleEntry.Occupy[i]==1)
		{
			hotTracker._na += 1;
			hotTracker._rh += 1;
		}
		else if(pleEntry.Type[i]==1 && pleEntry.Occupy[i]==0)
		{
			hotTracker._rh += 1;
		}
	}

	hotTracker._nn = bumblebee_n - hotTracker._na - hotTracker._nc;
	return;
}


void
MemoryController::execAsynReq()
{
	futex_lock(&_AsynQueuelock);
	while(!AsynReqQueue.empty())
	{
		AsynReq asynReq = AsynReqQueue.front();
		if(asynReq.type == 0) // indicates HBM
 		{
			_mcdram[asynReq.channel_select]->access(asynReq._asynReq,asynReq.access_type,4);
		}
		else // indicates DRAM
		{
			_ext_dram->access(asynReq._asynReq,asynReq.access_type,4);
		}
		AsynReqQueue.pop_front();
	}
	futex_unlock(&_AsynQueuelock);
	return;
}

/**
 * @brief 尝试驱逐操作
 */
void
MemoryController::tryEvict(PLEEntry& pleEntry,HotnenssTracker& hotTracker,uint64_t current_cycle,g_vector<BLEEntry>& bleEntries,uint64_t set_id,MemReq& req,int sl_state)
{
	// int type = sl_state;
	QueuePage endPage = hotTracker.HBMQueue.back();
	int endPageOffset = endPage._page_id;
	// 根据value 找到 idx
	int endPageIdx = -1;
	MESIState state;
	for(int i = 0;i < bumblebee_m + bumblebee_n;i++)
	{
		if(endPageOffset == pleEntry.PLE[i])
		{
			endPageIdx = i;
			break;
		}
	}

	assert(-1 != endPageIdx);

	// 根据Value找到bleEntry
	int ble_idx = -1;
	for(int i =0;i<(bumblebee_m+bumblebee_n);i++)
	{
		if(bleEntries[i].ple_idx == endPageOffset)
		{
			ble_idx = i;
			break;
		}
	}
	BLEEntry& bleEntry = bleEntries[ble_idx];

	if(endPage._last_mod_cycle - current_cycle > long_time) //hyperparameter:zombie page
	{
		if(pleEntry.Type[endPageIdx]==2)
		{
			if(endPageOffset >= bumblebee_n) // DDR Yes
			{
				bool is_alloc_ddr = pleEntry.Occupy[endPageOffset] == 1 ? true:false;
				if(is_alloc_ddr) // 原来有被分配（or remap）
				{
					bool is_self = pleEntry.Occupy[endPageOffset] == endPageOffset ? true:false;
					if(is_self) // 如果原来就是自己，写回dirty即可
					{
						for(int i = 0;i < blk_per_page; i++)
						{
							if(bleEntry.dirtyVector[i]==1)
							{
								// load from hbm
								Address ld_hbm_addr = set_id*bumblebee_n*_bumblebee_page_size + endPageIdx*_bumblebee_page_size + i*_bumblebee_blk_size;
								Address mem_hbm_addr = (ld_hbm_addr / 64 / _mcdram_per_mc * 64) | (ld_hbm_addr % 64);
								uint32_t mem_hbm_select = ld_hbm_addr / 64 % _mcdram_per_mc;
								MemReq load_req = {mem_hbm_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[mem_hbm_select]->access(load_req,2,4);

								// store to dram
								// 非局部性组织
								// Address sd_dram_addr = _mem_hbm_size + set_id*bumblebee_m*_bumblebee_page_size + (endPageOffset-bumblebee_n)*_bumblebee_page_size + i*_bumblebee_blk_size;
								// 局部性组织
								Address sd_dram_addr = _mem_hbm_size+(endPageOffset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(endPageOffset%bumblebee_n)*_bumblebee_page_size+ i*_bumblebee_blk_size;
								MemReq store_req = {sd_dram_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(store_req,2,4);

								bleEntry.dirtyVector[i] = 0; //避免再被换入时的错误状态
							}
						}
						pleEntry.PLE[endPageIdx] = -1;//置空
						pleEntry.Occupy[endPageIdx] = 0;
						if(sl_state==1)pleEntry.Type[endPageIdx] = 1; // 1 or 2 ?
						if(sl_state==2)pleEntry.Type[endPageIdx] = 2; // 1 or 2 ?

						pleEntry.PLE[endPageOffset] = endPageOffset; // trivial code
						pleEntry.Occupy[endPageOffset] = 1;//trivial code
						pleEntry.Type[endPageOffset] = 0; // trivial code
					}
					else // 否则就是找空DDR
					{
						int free_ddr = -1;
						for(int i = bumblebee_n;i < bumblebee_m + bumblebee_n;i++)
						{
							if(pleEntry.Occupy[i]==0)
							{
								free_ddr = i;
								break;
							}
						}

						assert(-1 != free_ddr);

						for(int i = 0; i < blk_per_page; i++)
						{
							if(bleEntry.validVector[i]==1)
							{
								// load from hbm
								Address ld_hbm_addr = set_id*bumblebee_n*_bumblebee_page_size + endPageIdx*_bumblebee_page_size + i*_bumblebee_blk_size;
								Address mem_hbm_addr = (ld_hbm_addr / 64 / _mcdram_per_mc * 64) | (ld_hbm_addr % 64);
								uint32_t mem_hbm_select = ld_hbm_addr / 64 % _mcdram_per_mc;
								MemReq load_req = {mem_hbm_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[mem_hbm_select]->access(load_req,2,4);

								// store to dram
								// 非局部性组织
								// Address sd_dram_addr = _mem_hbm_size + set_id*bumblebee_m*_bumblebee_page_size + (free_ddr-bumblebee_n)*_bumblebee_page_size + i*_bumblebee_blk_size;
								// 局部性组织
								Address sd_dram_addr = _mem_hbm_size+(free_ddr-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(free_ddr%bumblebee_n)*_bumblebee_page_size+ i*_bumblebee_blk_size;
								MemReq store_req = {sd_dram_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(store_req,2,4);
							}
						}

						pleEntry.PLE[endPageIdx] = -1;//置空
						pleEntry.Occupy[endPageIdx] = 0;
						if(sl_state==1)pleEntry.Type[endPageIdx] = 1; // 1 or 2 ?
						if(sl_state==2)pleEntry.Type[endPageIdx] = 2; // 1 or 2 ?

						pleEntry.PLE[free_ddr] = endPageOffset; 
						pleEntry.Occupy[free_ddr] = 1;
						pleEntry.Type[free_ddr] = 0; 
					}
				}
				else // 原来没有被分配，valid写回
				{
					pleEntry.PLE[endPageIdx] = -1;
					pleEntry.Occupy[endPageIdx] = 0;
					if(sl_state==1)pleEntry.Type[endPageIdx] = 1; // 1 or 2 ?
					if(sl_state==2)pleEntry.Type[endPageIdx] = 2; // 1 or 2 ?
					pleEntry.PLE[endPageOffset] = endPageOffset;
					pleEntry.Occupy[endPageOffset] = 1;
					pleEntry.Type[endPageOffset] = 0;

					for(int i = 0; i < blk_per_page; i++)
					{
						if(bleEntry.validVector[i]==1)
						{
							// load from hbm
							Address ld_hbm_addr = set_id*bumblebee_n*_bumblebee_page_size + endPageIdx*_bumblebee_page_size + i*_bumblebee_blk_size;
							Address mem_hbm_addr = (ld_hbm_addr / 64 / _mcdram_per_mc * 64) | (ld_hbm_addr % 64);
							uint32_t mem_hbm_select = ld_hbm_addr / 64 % _mcdram_per_mc;
							MemReq load_req = {mem_hbm_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_mcdram[mem_hbm_select]->access(load_req,2,4);
							// store to dram
							// 非局部性组织
							// Address sd_dram_addr = _mem_hbm_size + set_id*bumblebee_m*_bumblebee_page_size + (endPageOffset-bumblebee_n)*_bumblebee_page_size + i*_bumblebee_blk_size;
							// 局部性组织
							Address sd_dram_addr = _mem_hbm_size+(endPageOffset-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(endPageOffset%bumblebee_n)*_bumblebee_page_size+ i*_bumblebee_blk_size;
							MemReq store_req = {sd_dram_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(store_req,2,4);
						}
					}
				}
			}
			else // 找空DDR Evict
			{
				int free_ddr = -1;
				for(int i = bumblebee_n;i < bumblebee_m + bumblebee_n;i++)
				{
					if(pleEntry.Occupy[i]==0)
					{
						free_ddr = i;
						break;
					}
				}
				assert(-1 != free_ddr);

				for(int i = 0; i < blk_per_page; i++)
				{
					if(bleEntry.validVector[i]==1)
					{
						// load from hbm
						Address ld_hbm_addr = set_id*bumblebee_n*_bumblebee_page_size + endPageIdx*_bumblebee_page_size + i*_bumblebee_blk_size;
						Address mem_hbm_addr = (ld_hbm_addr / 64 / _mcdram_per_mc * 64) | (ld_hbm_addr % 64);
						uint32_t mem_hbm_select = ld_hbm_addr / 64 % _mcdram_per_mc;
						MemReq load_req = {mem_hbm_addr,GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mem_hbm_select]->access(load_req,2,4);
						// store to dram
						// 非局部性组织
						// Address sd_dram_addr = _mem_hbm_size + set_id*bumblebee_m*_bumblebee_page_size + (free_ddr-bumblebee_n)*_bumblebee_page_size + i*_bumblebee_blk_size;
						// 局部性组织
						Address sd_dram_addr = _mem_hbm_size+(free_ddr-bumblebee_n)/bumblebee_n*_mem_hbm_size+set_id*bumblebee_n*_bumblebee_page_size+(free_ddr%bumblebee_n)*_bumblebee_page_size+ i*_bumblebee_blk_size;
						MemReq store_req = {sd_dram_addr,PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(store_req,2,4);
					}
				}

				pleEntry.PLE[endPageIdx] = -1;//置空
				pleEntry.Occupy[endPageIdx] = 0;
				if(sl_state==1)pleEntry.Type[endPageIdx] = 1; // 1 or 2 ?
				if(sl_state==2)pleEntry.Type[endPageIdx] = 2; // 1 or 2 ?
				pleEntry.PLE[free_ddr] = endPageOffset; 
				pleEntry.Occupy[free_ddr] = 1;
				pleEntry.Type[free_ddr] = 0; 
			}
		}
		else if(pleEntry.Type[endPageIdx]==1) // turn to cache
		{
			pleEntry.Type[endPageIdx] = 2;
			pleEntry.Occupy[endPageIdx] = 1;
		}
	}
	
	return;
}

/**
 * @brief Hybrid2's function: get specific set_id
 * @attention parameter `addr` should be restored to the byte address
 */
uint64_t
MemoryController::get_set_id(uint64_t addr)
{
	if (addr  < _mem_hbm_size)
	{
		uint64_t pg_id = get_page_id(addr);
		return pg_id % hbm_set_num;
	}
	else // 按照内存无限的假定，可以有模拟器设置global memory
	{
		assert(addr >= _mem_hbm_size);
		uint64_t pg_id = get_page_id(addr);
		return pg_id % hbm_set_num;
	}
}

/**
 * @brief Hybrid2's function: get specific page_id
 * @attention parameter `addr` should be restored to the byte address
 */
uint64_t
MemoryController::get_page_id(uint64_t addr)
{
	return addr  / _hybrid2_page_size;
}

/**
 * @brief Hybrid2's function: get &set
 */
g_vector<MemoryController::XTAEntry> &
MemoryController::find_XTA_set(uint64_t set_id)
{
	assert(set_id < XTA.size() && set_id >= 0);
	return XTA[set_id];
}

/**
 * @brief Hybrid2's function: get the page index in the set with the biggest lru
 */
uint64_t
MemoryController::ret_lru_page(g_vector<XTAEntry> SETEntries)
{
	if (SETEntries.empty())
	{
		return -1;
	}
	uint64_t uint_max = SETEntries.size() + 114514;
	int max_lru = -1;
	uint64_t max_idx = uint_max;
	for (auto i = 0u; i < SETEntries.size(); i++)
	{
		if (max_lru < (int)SETEntries[i]._hybrid2_LRU)
		{
			max_lru = (int)SETEntries[i]._hybrid2_LRU;
			max_idx = (uint64_t)i;
		}
	}
	assert(max_idx != uint_max);
	return max_idx;
}
/**
 * @brief Hybrid2's function: check whether the set is full
 */
int 
MemoryController::check_set_full(g_vector<XTAEntry> SETEntries)
{
	int empty_idx = -1;
	for (auto i = 0u; i < SETEntries.size(); i++)
	{
		if (static_cast<uint64_t>(0) == SETEntries[i]._hybrid2_tag)
		{
			empty_idx = i;
			break;
		}
	}
	return empty_idx;
}
/**
 * @brief Hybrid2's function: return number of total empty pages in one set
 */
int 
MemoryController::check_set_occupy(g_vector<XTAEntry> SETEntries)
{
	int empty_cntr = 0;
	for (auto i = 0u; i < SETEntries.size(); i++)
	{
		if (static_cast<uint64_t>(0) == SETEntries[i]._hybrid2_tag)
		{
			empty_cntr += 1;
		}
	}
	return empty_cntr;
}

/**
 * @brief restoring the virtual cacheline address to physical byte address 
 * @attention the phsical memory range should be declared at construction function
 */
Address
MemoryController::vaddr_to_paddr(MemReq req)
{
	Address vddr = req.lineAddr * 64;
	Address paddr = vddr % (9LL * 1024 * 1024 * 1024);
	return paddr;
};


// req.lineAddr * 64 % (9*1024*1024*1024) / (1024*1024*1024)

/**
 * @brief restoring the physical cacheline address to virtual cacheline address 
 * @attention not used now
 */
Address
MemoryController::paddr_to_vaddr(Address pLineAddr)
{
	uint64_t page_bits = std::log2(_hybrid2_page_size);
	uint64_t page_offset = pLineAddr & (_hybrid2_page_size - 1);
	uint64_t ppgnum = pLineAddr >> page_bits;

	uint64_t vpgnum = 0;
	for (uint64_t i = 0; i < num_pages; ++i)
	{
		if (fixedMapping[i] == ppgnum)
		{
			vpgnum = i; // 找到对应的虚拟页号
			break;
		}
	}

	Address vLineAddr = (vpgnum << page_bits) | page_offset;
	return vLineAddr;
}

bool MemoryController::is_hbm(MemReq req)
{
	bool is_hbm = true;
	Address pLineAddr = vaddr_to_paddr(req);
	if (pLineAddr >= _mem_hbm_size)
	{
		is_hbm = false;
	}
	return is_hbm;
}



// In test , bug occured without calling this function
// Following function has been removed from above code !
Address
MemoryController::handle_low_address(Address addr)
{
	if (addr >= 0 && addr < 1024 * 1024)
	{
		addr = addr + 1024 * 1024;
	}
	return addr;
}

DDRMemory *
MemoryController::BuildDDRMemory(Config &config, uint32_t frequency,
								 uint32_t domain, g_string name, const string &prefix, uint32_t tBL, double timing_scale)
{
	uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 4);
	uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);					 // DDR3 std is 8
	uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 8 * 1024);					 // 1Kb cols, x4 devices
	const char *tech = config.get<const char *>(prefix + "tech", "DDR3-1333-CL10");				 // see cpp file for other techs
	const char *addrMapping = config.get<const char *>(prefix + "addrMapping", "rank:col:bank"); // address splitter interleaves channels; row always on top

	// If set, writes are deferred and bursted out to reduce WTR overheads
	bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
	bool closedPage = config.get<bool>(prefix + "closedPage", true);

	// Max row hits before we stop prioritizing further row hits to this bank.
	// Balances throughput and fairness; 0 -> FCFS / high (e.g., -1) -> pure FR-FCFS
	uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", 4);

	// Request queues
	uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
	uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 10); // in system cycles

	auto mem = (DDRMemory *)gm_malloc(sizeof(DDRMemory));
	new (mem) DDRMemory(zinfo->lineSize, pageSize, ranksPerChannel, banksPerRank, frequency, tech, addrMapping, controllerLatency, queueDepth, maxRowHits, deferWrites, closedPage, domain, name, tBL, timing_scale);
	printf("GET MEM INFO : %d %d", zinfo->lineSize, pageSize);
	return mem;
}

void MemoryController::initStats(AggregateStat *parentStat)
{
	AggregateStat *memStats = new AggregateStat();
	memStats->init(_name.c_str(), "Memory controller stats");

	_numPlacement.init("placement", "Number of Placement");
	memStats->append(&_numPlacement);
	_numCleanEviction.init("cleanEvict", "Clean Eviction");
	memStats->append(&_numCleanEviction);
	_numDirtyEviction.init("dirtyEvict", "Dirty Eviction");
	memStats->append(&_numDirtyEviction);
	_numLoadHit.init("loadHit", "Load Hit");
	memStats->append(&_numLoadHit);
	_numLoadMiss.init("loadMiss", "Load Miss");
	memStats->append(&_numLoadMiss);
	_numStoreHit.init("storeHit", "Store Hit");
	memStats->append(&_numStoreHit);
	_numStoreMiss.init("storeMiss", "Store Miss");
	memStats->append(&_numStoreMiss);
	_numCounterAccess.init("counterAccess", "Counter Access");
	memStats->append(&_numCounterAccess);

	_numTagLoad.init("tagLoad", "Number of tag loads");
	memStats->append(&_numTagLoad);
	_numTagStore.init("tagStore", "Number of tag stores");
	memStats->append(&_numTagStore);
	_numTagBufferFlush.init("tagBufferFlush", "Number of tag buffer flushes");
	memStats->append(&_numTagBufferFlush);

	_numTBDirtyHit.init("TBDirtyHit", "Tag buffer hits (LLC dirty evict)");
	memStats->append(&_numTBDirtyHit);
	_numTBDirtyMiss.init("TBDirtyMiss", "Tag buffer misses (LLC dirty evict)");
	memStats->append(&_numTBDirtyMiss);

	_numTouchedLines.init("totalTouchLines", "total # of touched lines in UnisonCache");
	memStats->append(&_numTouchedLines);
	_numEvictedLines.init("totalEvictLines", "total # of evicted lines in UnisonCache");
	memStats->append(&_numEvictedLines);

	_ext_dram->initStats(memStats);
	for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		_mcdram[i]->initStats(memStats);

	parentStat->append(memStats);
}

Address
MemoryController::transMCAddress(Address mc_addr)
{
	// 28 lines per DRAM row (2048 KB row)
	uint64_t num_lines_per_mc = 128 * 1024 * 1024 / 2048 * 28;
	uint64_t set = mc_addr % num_lines_per_mc;
	return set / 28 * 32 + set % 28;
}

Address
MemoryController::transMCAddressPage(uint64_t set_num, uint32_t way_num)
{
	return (_num_ways * set_num + way_num) * _granularity;
}

TagBuffer::TagBuffer(Config &config)
{
	uint32_t tb_size = config.get<uint32_t>("sys.mem.mcdram.tag_buffer_size", 1024);
	_num_ways = 8;
	_num_sets = tb_size / _num_ways;
	_entry_occupied = 0;
	_tag_buffer = (TagBufferEntry **)gm_malloc(sizeof(TagBufferEntry *) * _num_sets);
	//_tag_buffer = new TagBufferEntry * [_num_sets];
	for (uint32_t i = 0; i < _num_sets; i++)
	{
		_tag_buffer[i] = (TagBufferEntry *)gm_malloc(sizeof(TagBufferEntry) * _num_ways);
		//_tag_buffer[i] = new TagBufferEntry [_num_ways];
		for (uint32_t j = 0; j < _num_ways; j++)
		{
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}

uint32_t
TagBuffer::existInTB(Address tag)
{
	uint32_t set_num = tag % _num_sets;
	for (uint32_t i = 0; i < _num_ways; i++)
		if (_tag_buffer[set_num][i].tag == tag)
		{
			// printf("existInTB\n");
			return i;
		}
	return _num_ways;
}

bool TagBuffer::canInsert(Address tag)
{
#if 1
	uint32_t num = 0;
	for (uint32_t i = 0; i < _num_sets; i++)
		for (uint32_t j = 0; j < _num_ways; j++)
			if (_tag_buffer[i][j].remap)
				num++;
	assert(num == _entry_occupied);
#endif

	uint32_t set_num = tag % _num_sets;
	// printf("tag_buffer=%#lx, set_num=%d, tag_buffer[set_num]=%#lx, num_ways=%d\n",
	//	(uint64_t)_tag_buffer, set_num, (uint64_t)_tag_buffer[set_num], _num_ways);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap || _tag_buffer[set_num][i].tag == tag)
			return true;
	return false;
}

bool TagBuffer::canInsert(Address tag1, Address tag2)
{
	uint32_t set_num1 = tag1 % _num_sets;
	uint32_t set_num2 = tag2 % _num_sets;
	if (set_num1 != set_num2)
		return canInsert(tag1) && canInsert(tag2);
	else
	{
		uint32_t num = 0;
		for (uint32_t i = 0; i < _num_ways; i++)
			if (!_tag_buffer[set_num1][i].remap || _tag_buffer[set_num1][i].tag == tag1 || _tag_buffer[set_num1][i].tag == tag2)
				num++;
		return num >= 2;
	}
}

void TagBuffer::insert(Address tag, bool remap)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
#if 1
	for (uint32_t i = 0; i < _num_ways; i++)
		for (uint32_t j = i + 1; j < _num_ways; j++)
		{
			// if (_tag_buffer[set_num][i].tag != 0 && _tag_buffer[set_num][i].tag == _tag_buffer[set_num][j].tag) {
			//	for (uint32_t k = 0; k < _num_ways; k++)
			//		printf("_tag_buffer[%d][%d]: tag=%ld, remap=%d\n",
			//			set_num, k, _tag_buffer[set_num][k].tag, _tag_buffer[set_num][k].remap);
			// }
			assert(_tag_buffer[set_num][i].tag != _tag_buffer[set_num][j].tag || _tag_buffer[set_num][i].tag == 0);
		}
#endif
	if (exist_way < _num_ways)
	{
		// the tag already exists in the Tag Buffer
		assert(tag == _tag_buffer[set_num][exist_way].tag);
		if (remap)
		{
			if (!_tag_buffer[set_num][exist_way].remap)
				_entry_occupied++;
			_tag_buffer[set_num][exist_way].remap = true;
		}
		else if (!_tag_buffer[set_num][exist_way].remap)
			updateLRU(set_num, exist_way);
		return;
	}

	uint32_t max_lru = 0;
	uint32_t replace_way = _num_ways;
	for (uint32_t i = 0; i < _num_ways; i++)
	{
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru >= max_lru)
		{
			max_lru = _tag_buffer[set_num][i].lru;
			replace_way = i;
		}
	}
	assert(replace_way != _num_ways);
	_tag_buffer[set_num][replace_way].tag = tag;
	_tag_buffer[set_num][replace_way].remap = remap;
	if (!remap)
	{
		// printf("\tset=%d way=%d, insert. no remap\n", set_num, replace_way);
		updateLRU(set_num, replace_way);
	}
	else
	{
		// printf("set=%d way=%d, insert\n", set_num, replace_way);
		_entry_occupied++;
	}
}

void TagBuffer::updateLRU(uint32_t set_num, uint32_t way)
{
	assert(!_tag_buffer[set_num][way].remap);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru < _tag_buffer[set_num][way].lru)
			_tag_buffer[set_num][i].lru++;
	_tag_buffer[set_num][way].lru = 0;
}

void TagBuffer::clearTagBuffer()
{
	_entry_occupied = 0;
	for (uint32_t i = 0; i < _num_sets; i++)
	{
		for (uint32_t j = 0; j < _num_ways; j++)
		{
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}
