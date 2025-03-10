// See LICENSE for license details.

#include "sim.h"
#include "mmu.h"
#include "dts.h"
#include "remote_bitbang.h"
#include "byteorder.h"
#include "platform.h"
#include "libfdt.h"
#include <fstream>
#include <map>
#include <iostream>
#include <sstream>
#include <climits>
#include <cstdlib>
#include <cassert>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

volatile bool ctrlc_pressed = false;
static void handle_signal(int sig)
{
  if (ctrlc_pressed)
    exit(-1);
  ctrlc_pressed = true;
  signal(sig, &handle_signal);
}
/*struct driver_info{
  uint64_t pds_base=0xa0000000;
  uint64_t lds_base=0xa8000000;
  uint64_t knl=0;
  uint64_t wgid=0;
  uint64_t gidx=0;
  uint64_t gidy=0;
  uint64_t gidz=0;
  uint64_t pds_size=(w.thread_number*w.warp_number)<<10;
  uint64_t lds_size=(10*w.thread_number*w.warp_number)<<10;
}*/

sim_t::sim_t(const cfg_t *cfg, bool halted,
             std::vector<std::pair<reg_t, mem_t*>> mems,
             std::vector<std::pair<reg_t, abstract_device_t*>> plugin_devices,
             const std::vector<std::string>& args,
             const debug_module_config_t &dm_config,
             const char *log_path,
             bool dtb_enabled, const char *dtb_file,
#ifdef HAVE_BOOST_ASIO
             boost::asio::io_service *io_service_ptr, boost::asio::ip::tcp::acceptor *acceptor_ptr, // option -s
#endif
             FILE *cmd_file) // needed for command line option --cmd
  : htif_t(args),
    isa(cfg->isa(), cfg->priv()),
    cfg(cfg),
    mems(mems),
    plugin_devices(plugin_devices),
    procs(std::max(cfg->nprocs(), size_t(1))),
    reach_end(std::max(cfg->nprocs(),size_t(1))),
    workgroups(NULL),
    dtb_file(dtb_file ? dtb_file : ""),
    dtb_enabled(dtb_enabled),
    log_file(log_path),
    cmd_file(cmd_file),
#ifdef HAVE_BOOST_ASIO
    io_service_ptr(io_service_ptr), // socket interface
    acceptor_ptr(acceptor_ptr),
#endif
    sout_(nullptr),
    current_step(0),
    current_proc(0),
    debug(false),
    histogram_enabled(false),
    log(false),
    remote_bitbang(NULL),
    debug_module(this, dm_config)
{
  signal(SIGINT, &handle_signal);

  sout_.rdbuf(std::cerr.rdbuf()); // debug output goes to stderr by default

  for (auto& x : mems)
    bus.add_device(x.first, x.second);

  for (auto& x : plugin_devices)
    bus.add_device(x.first, x.second);

  debug_module.add_device(&bus);

  debug_mmu = new mmu_t(this, NULL);

  // initialize warp schedule
  
  w.init_warp(cfg->gpgpuarch());
  uint64_t pds_base=w.pds_base;
  uint64_t lds_base=w.lds_base;
  uint64_t knl_base=w.knl_base;
  //uint64_t wgid=0;
  uint64_t gidx=0;
  uint64_t gidy=0;
  uint64_t gidz=0;
  uint64_t clprintf=0x8fffe000;
  uint64_t pds_size=w.pds_size;
  uint64_t lds_size=w.lds_size;

  uint64_t pds = pds_base;
  uint64_t lds = lds_base;

  w.workgroup_number = 1;

  uint64_t spike_curr_wgid = w.curr_wgid;

  assert(spike_curr_wgid < w.workgroup_size_x * w.workgroup_size_y * w.workgroup_size_z);
  gidz = spike_curr_wgid / (w.workgroup_size_x * w.workgroup_size_y);
  gidy = (spike_curr_wgid % (w.workgroup_size_x * w.workgroup_size_y)) / w.workgroup_size_x;
  gidx = (spike_curr_wgid % (w.workgroup_size_x * w.workgroup_size_y)) % w.workgroup_size_x;
  //printf("simt() current_wgid is %ld, gidx %ld, gidy %ld, gidz %ld\n", spike_curr_wgid, gidx, gidy, gidz);

  workgroups = new warp_schedule_t[w.workgroup_number];
  for (size_t i=0;i<w.workgroup_number;i++) {
    assert(w.warp_number>0 & w.workgroup_number>0 & w.thread_number>0);
    assert(w.warp_number * w.workgroup_number == cfg->nprocs());
    workgroups[i].set_warp_schedule(w.warp_number,w.thread_number,w.workgroup_number, spike_curr_wgid);

    for (size_t j = 0; j < w.warp_number; j++) {
      reach_end[i*w.warp_number+j] = i*w.warp_number+j;
      procs[i*w.warp_number+j] = new processor_t(&isa, cfg->varch(), this, cfg->hartids()[i*w.warp_number+j], halted,
                               log_file.get(), sout_);

      procs[i*w.warp_number+j]->gpgpu_unit.set_warp(&workgroups[i]);//workgroups[i]);
      //现在一个warp就是一个core
      procs[i*w.warp_number+j]->gpgpu_unit.init_warp(w.warp_number, w.thread_number,
              j * w.thread_number, spike_curr_wgid, j, pds, lds, knl_base, gidx, gidy, gidz, clprintf);
      assert(w.thread_number == (procs[i]->VU.get_vlen() / procs[i]->VU.get_elen()));
    }
    
#if 0
    gidx = gidx+1;
    if(gidx == w.workgroup_size_x) {
        gidx = 0;
        gidy = gidy + 1;

        if(gidy == w.workgroup_size_y) {
            gidy = 0;
            gidz = gidz+1;

            if(gidz == w.workgroup_size_z) {
                gidz = 0;
            }
        }
    }
#endif
    pds = pds + pds_size;
    lds = lds + lds_size;
  }

  
#if 0
  make_dtb();

  void *fdt = (void *)dtb.c_str();

  // Only make a CLINT (Core-Local INTerrupt controller) if one is specified in
  // the device tree configuration.
  //
  // This isn't *quite* as general as we could get (because you might have one
  // that's not bus-accessible), but it should handle the normal use cases. In
  // particular, the default device tree configuration that you get without
  // setting the dtb_file argument has one.
  reg_t clint_base;
  if (fdt_parse_clint(fdt, &clint_base, "riscv,clint0") == 0) {
    clint.reset(new clint_t(procs, CPU_HZ / INSNS_PER_RTC_TICK, cfg->real_time_clint()));
    bus.add_device(clint_base, clint.get());
  }

  //per core attribute
  int cpu_offset = 0, rc;
  size_t cpu_idx = 0;
  cpu_offset = fdt_get_offset(fdt, "/cpus");
  if (cpu_offset < 0)
    return;

  for (cpu_offset = fdt_get_first_subnode(fdt, cpu_offset); cpu_offset >= 0;
       cpu_offset = fdt_get_next_subnode(fdt, cpu_offset)) {

    if (cpu_idx >= nprocs())
      break;

    //handle pmp
    reg_t pmp_num = 0, pmp_granularity = 0;
    if (fdt_parse_pmp_num(fdt, cpu_offset, &pmp_num) == 0) {
      if (pmp_num <= 64) {
        procs[cpu_idx]->set_pmp_num(pmp_num);
      } else {
        std::cerr << "core ("
                  << cpu_idx
                  << ") doesn't have valid 'riscv,pmpregions'"
                  << pmp_num << ").\n";
        exit(1);
      }
    } else {
      procs[cpu_idx]->set_pmp_num(0);
    }

    if (fdt_parse_pmp_alignment(fdt, cpu_offset, &pmp_granularity) == 0) {
      procs[cpu_idx]->set_pmp_granularity(pmp_granularity);
    }

    //handle mmu-type
    const char *mmu_type;
    rc = fdt_parse_mmu_type(fdt, cpu_offset, &mmu_type);
    if (rc == 0) {
      procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SBARE);
      if (strncmp(mmu_type, "riscv,sv32", strlen("riscv,sv32")) == 0) {
        procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SV32);
      } else if (strncmp(mmu_type, "riscv,sv39", strlen("riscv,sv39")) == 0) {
        procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SV39);
      } else if (strncmp(mmu_type, "riscv,sv48", strlen("riscv,sv48")) == 0) {
        procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SV48);
      } else if (strncmp(mmu_type, "riscv,sv57", strlen("riscv,sv57")) == 0) {
        procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SV57);
      } else if (strncmp(mmu_type, "riscv,sbare", strlen("riscv,sbare")) == 0) {
        //has been set in the beginning
      } else {
        std::cerr << "core ("
                  << cpu_idx
                  << ") has an invalid 'mmu-type': "
                  << mmu_type << ").\n";
        exit(1);
      }
    } else {
      procs[cpu_idx]->set_mmu_capability(IMPL_MMU_SBARE);
    }

    cpu_idx++;
  }

  if (cpu_idx != nprocs()) {
      std::cerr << "core number in dts ("
                <<  cpu_idx
                << ") doesn't match it in command line ("
                << nprocs() << ").\n";
      exit(1);
  }
#endif

}

sim_t::~sim_t()
{
  delete[] workgroups;
  for (size_t i = 0; i < procs.size(); i++)
    delete procs[i];
  delete debug_mmu;
}

void sim_thread_main(void* arg)
{
  ((sim_t*)arg)->main();
}

void sim_t::main()
{
  if (!debug && log)
    set_procs_debug(true);

  while (!done())
  {
    if (debug || ctrlc_pressed)
      interactive();
    else
      step(INTERLEAVE);
    if (remote_bitbang) {
      remote_bitbang->tick();
    }
  }
}

int sim_t::run()
{
  host = context_t::current();
  target.init(sim_thread_main, this);
  return htif_t::run();
}

void sim_t::step(size_t n)
{
  for (size_t i = 0, steps = 0; i < n; i += steps)
  {
    steps = std::min(n - i, INTERLEAVE - current_step);
    procs[reach_end[current_proc]]->step(steps);
    current_step += steps;
    if (current_step == INTERLEAVE)
    {
      current_step = 0;
      procs[reach_end[current_proc]]->get_mmu()->yield_load_reservation();
      current_proc++;
      if (current_proc == reach_end.size()) {
        current_proc = 0;
        if (clint) clint->increment(INTERLEAVE / INSNS_PER_RTC_TICK);
      }

      host->switch_to();
    }
  }
}

void sim_t::set_debug(bool value)
{
  debug = value;
}

void sim_t::set_histogram(bool value)
{
  histogram_enabled = value;
  for (size_t i = 0; i < procs.size(); i++) {
    procs[i]->set_histogram(histogram_enabled);
  }
}

void sim_t::configure_log(bool enable_log, bool enable_commitlog)
{
  log = enable_log;

  if (!enable_commitlog)
    return;

#ifndef RISCV_ENABLE_COMMITLOG
  fputs("Commit logging support has not been properly enabled; "
        "please re-build the riscv-isa-sim project using "
        "\"configure --enable-commitlog\".\n",
        stderr);
  abort();
#else
  for (processor_t *proc : procs) {
    proc->enable_log_commits();
  }
#endif
}

void sim_t::set_procs_debug(bool value)
{
  for (size_t i=0; i< procs.size(); i++)
    procs[i]->set_debug(value);
}

static bool paddr_ok(reg_t addr)
{
  return (addr >> MAX_PADDR_BITS) == 0;
}

bool sim_t::mmio_load(reg_t addr, size_t len, uint8_t* bytes)
{
  if (addr + len < addr || !paddr_ok(addr + len - 1))
    return false;
  return bus.load(addr, len, bytes);
}

bool sim_t::mmio_store(reg_t addr, size_t len, const uint8_t* bytes)
{
  if (addr + len < addr || !paddr_ok(addr + len - 1))
    return false;
  return bus.store(addr, len, bytes);
}

void sim_t::make_dtb()
{
  if (!dtb_file.empty()) {
    std::ifstream fin(dtb_file.c_str(), std::ios::binary);
    if (!fin.good()) {
      std::cerr << "can't find dtb file: " << dtb_file << std::endl;
      exit(-1);
    }

    std::stringstream strstream;
    strstream << fin.rdbuf();

    dtb = strstream.str();
  } else {
    std::pair<reg_t, reg_t> initrd_bounds = cfg->initrd_bounds();
    dts = make_dts(INSNS_PER_RTC_TICK, CPU_HZ,
                   initrd_bounds.first, initrd_bounds.second,
                   cfg->bootargs(), procs, mems);
    dtb = dts_compile(dts);
  }

  int fdt_code = fdt_check_header(dtb.c_str());
  if (fdt_code) {
    std::cerr << "Failed to read DTB from ";
    if (dtb_file.empty()) {
      std::cerr << "auto-generated DTS string";
    } else {
      std::cerr << "`" << dtb_file << "'";
    }
    std::cerr << ": " << fdt_strerror(fdt_code) << ".\n";
    exit(-1);
  }
}

void sim_t::set_rom()
{
  const int reset_vec_size = 8;

  reg_t start_pc = cfg->start_pc.value_or(get_entry_point());

  uint32_t reset_vec[reset_vec_size] = {
    0x297,                                      // auipc  t0,0x0
    0x28593 + (reset_vec_size * 4 << 20),       // addi   a1, t0, &dtb
    0xf1402573,                                 // csrr   a0, mhartid
    get_core(0)->get_xlen() == 32 ?
      0x0182a283u :                             // lw     t0,24(t0)
      0x0182b283u,                              // ld     t0,24(t0)
    0x28067,                                    // jr     t0
    0,
    (uint32_t) (start_pc & 0xffffffff),
    (uint32_t) (start_pc >> 32)
  };
  if (get_target_endianness() == memif_endianness_big) {
    int i;
    // Instuctions are little endian
    for (i = 0; reset_vec[i] != 0; i++)
      reset_vec[i] = to_le(reset_vec[i]);
    // Data is big endian
    for (; i < reset_vec_size; i++)
      reset_vec[i] = to_be(reset_vec[i]);

    // Correct the high/low order of 64-bit start PC
    if (get_core(0)->get_xlen() != 32)
      std::swap(reset_vec[reset_vec_size-2], reset_vec[reset_vec_size-1]);
  } else {
    for (int i = 0; i < reset_vec_size; i++)
      reset_vec[i] = to_le(reset_vec[i]);
  }

  std::vector<char> rom((char*)reset_vec, (char*)reset_vec + sizeof(reset_vec));

  rom.insert(rom.end(), dtb.begin(), dtb.end());
  const int align = 0x1000;
  rom.resize((rom.size() + align - 1) / align * align);

  boot_rom.reset(new rom_device_t(rom));
  bus.add_device(DEFAULT_RSTVEC, boot_rom.get());
}

char* sim_t::addr_to_mem(reg_t addr) {
  if (!paddr_ok(addr))
    return NULL;
  auto desc = bus.find_device(addr);
  if (auto mem = dynamic_cast<mem_t*>(desc.second))
    if (addr - desc.first < mem->size())
      return mem->contents(addr - desc.first);
  return NULL;
}

const char* sim_t::get_symbol(uint64_t addr)
{
  return htif_t::get_symbol(addr);
}

// htif

void sim_t::reset()
{
  if (dtb_enabled)
    set_rom();
}

void sim_t::idle()
{
  target.switch_to();
}

void sim_t::read_chunk(addr_t taddr, size_t len, void* dst)
{
  assert(len == 8);
  auto data = debug_mmu->to_target(debug_mmu->load_uint64(taddr));
  memcpy(dst, &data, sizeof data);
}

void sim_t::write_chunk(addr_t taddr, size_t len, const void* src)
{
  assert(len == 8);
  target_endian<uint64_t> data;
  memcpy(&data, src, sizeof data);
  debug_mmu->store_uint64(taddr, debug_mmu->from_target(data));
}

void sim_t::set_target_endianness(memif_endianness_t endianness)
{
#ifdef RISCV_ENABLE_DUAL_ENDIAN
  assert(endianness == memif_endianness_little || endianness == memif_endianness_big);

  bool enable = endianness == memif_endianness_big;
  debug_mmu->set_target_big_endian(enable);
  for (size_t i = 0; i < procs.size(); i++) {
    procs[i]->get_mmu()->set_target_big_endian(enable);
    procs[i]->reset();
  }
#else
  assert(endianness == memif_endianness_little);
#endif
}

memif_endianness_t sim_t::get_target_endianness() const
{
#ifdef RISCV_ENABLE_DUAL_ENDIAN
  return debug_mmu->is_target_big_endian()? memif_endianness_big : memif_endianness_little;
#else
  return memif_endianness_little;
#endif
}

void sim_t::proc_reset(unsigned id)
{
  debug_module.proc_reset(id);
}
