#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cinttypes>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "wasm.hh"
#include "wasm-v8.hh"


static const char data_file[] = "mmap.data";

int mem_count = 0;

auto open_mem_file(int pages) -> int {
  // Create memory file.
  std::cout << "opening memory file..." << std::endl;
  auto size = pages * wasm::Memory::page_size;

  auto fd = open(data_file, O_RDWR | O_CREAT, 0600);
  if (fd == -1) {
    std::cout << "> Error opening memory file! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }
  if (ftruncate(fd, size) == -1) {
    close(fd);
    std::cout << "> Error initialising memory file! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  return fd;
}

struct mem_info {
  void* base;
  byte_t* data;
  size_t alloc_size;
  int fd;
};

auto make_mem(size_t size, int fd) -> std::unique_ptr<mem_info> {
  std::cout << "> Making memory "
    << "(size = " << size << ", fd = " << fd << ")..." << std::endl;
  auto offset = wasm::v8::Memory::redzone_size_lo(size);
  auto alloc_size = size + wasm::v8::Memory::redzone_size_hi(size);

  void* base = nullptr;
  if (offset > 0) {
    base = mmap(nullptr, offset, PROT_NONE, MAP_ANON | MAP_PRIVATE, 0, 0);
    if (base == MAP_FAILED) {
      std::cout << "> Error reserving lo redzone! errno = " << errno
        << " (" << strerror(errno) << ")" << std::endl;
      exit(1);
    }
  }

  auto data = static_cast<byte_t*>(base) + offset;
  auto result = mmap(
    data, alloc_size, PROT_NONE, MAP_FILE | MAP_FIXED | MAP_SHARED, fd, 0);
  if (result == MAP_FAILED) {
    std::cout << "> Error reserving memory! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  if (mprotect(data, size, PROT_READ | PROT_WRITE) != 0) {
    std::cout << "> Error allocating memory! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  ++mem_count;
  return std::unique_ptr<mem_info>(new mem_info{base, data, alloc_size, fd});
}

void free_mem(void* extra, byte_t* data, size_t size) {
  std::cout << "> Freeing memory in callback "
    << "(size = " << size << ")..." << std::endl;
  auto info = static_cast<mem_info*>(extra);

  close(info->fd);
  auto offset = info->data - static_cast<byte_t*>(info->base);
  if (offset != 0 && munmap(info->base, offset) == -1) {
    std::cout << "> Error freeing lo redzone! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  } 

  if (munmap(info->data, info->alloc_size) == -1) {
    std::cout << "> Error freeing memory! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  delete info;
  --mem_count;
}

auto grow_mem(void* extra, byte_t* data, size_t old_size, size_t new_size) -> byte_t* {
  std::cout << "> Growing memory in callback "
    << "(old size = " << old_size << ", new size = " << new_size << ")..."
    << std::endl;
  auto info = static_cast<mem_info*>(extra);

  if (ftruncate(info->fd, new_size) == -1) {
    close(info->fd);
    std::cout << "> Error growing memory file! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  if (mprotect(data + old_size, new_size, PROT_READ | PROT_WRITE) != 0) {
    close(info->fd);
    std::cout << "> Error resizing memory! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  return data;
}


auto get_export_func(const wasm::ownvec<wasm::Extern>& exports, size_t i) -> const wasm::Func* {
  if (exports.size() <= i || !exports[i]->func()) {
    std::cout << "> Error accessing function export " << i << "!" << std::endl;
    exit(1);
  }
  return exports[i]->func();
}

template<class T, class U>
void check(T actual, U expected) {
  if (actual != expected) {
    std::cout << "> Error on result, expected " << expected << ", got " << actual << std::endl;
    exit(1);
  }
}

template<class T>
void check(byte_t actual, T expected) {
  if (actual != expected) {
    std::cout << "> Error on result, expected " << expected << ", got " << int(actual) << std::endl;
    exit(1);
  }
}

template<class... Args>
void check_ok(const wasm::Func* func, Args... xs) {
  wasm::Val args[] = {wasm::Val::i32(xs)...};
  if (func->call(args)) {
    std::cout << "> Error on result, expected return" << std::endl;
    exit(1);
  }
}

template<class... Args>
void check_trap(const wasm::Func* func, Args... xs) {
  wasm::Val args[] = {wasm::Val::i32(xs)...};
  if (! func->call(args)) {
    std::cout << "> Error on result, expected trap" << std::endl;
    exit(1);
  }
}

template<class... Args>
auto call(const wasm::Func* func, Args... xs) -> int32_t {
  wasm::Val args[] = {wasm::Val::i32(xs)...};
  wasm::Val results[1];
  if (func->call(args, results)) {
    std::cout << "> Error on result, expected return" << std::endl;
    exit(1);
  }
  return results[0].i32();
}


auto compile(wasm::Engine* engine) -> wasm::own<wasm::Shared<wasm::Module>> {
  // Load binary.
  std::cout << "Loading binary..." << std::endl;
  std::ifstream file("mmap.wasm");
  file.seekg(0, std::ios_base::end);
  auto file_size = file.tellg();
  file.seekg(0);
  auto binary = wasm::vec<byte_t>::make_uninitialized(file_size);
  file.read(binary.get(), file_size);
  file.close();
  if (file.fail()) {
    std::cout << "> Error loading module!" << std::endl;
    exit(1);
  }

  // Compile.
  auto store_ = wasm::Store::make(engine);
  auto store = store_.get();

  std::cout << "Compiling module..." << std::endl;
  auto module = wasm::Module::make(store, binary);
  if (!module) {
    std::cout << "> Error compiling module!" << std::endl;
    exit(1);
  }

  return module->share();
}


void execute(
  wasm::Engine* engine, wasm::Shared<wasm::Module>* shared_module, int pages,
  int run, const std::function<void(
    wasm::Memory*, const wasm::ownvec<wasm::Extern>&)>& fn
) {
  std::cout << "Starting run " << run << "..." << std::endl;
  auto store_ = wasm::Store::make(engine);
  auto store = store_.get();

  // Allocate memory.
  std::cout << "Allocating memory..." << std::endl;
  auto size = pages * wasm::Memory::page_size;
  auto info = make_mem(size, open_mem_file(pages));

  // Create memory.
  std::cout << "Creating memory..." << std::endl;
  auto memory_type = wasm::MemoryType::make(wasm::Limits(pages));

  auto memory = wasm::v8::Memory::make_external(
    store, memory_type.get(), info->data, info.get(), &grow_mem, &free_mem);
  if (!memory) {
    std::cout << "> Error creating memory!" << std::endl;
    exit(1);
  }
  info.release();

  // Instantiate.
  std::cout << "Instantiating module..." << std::endl;
  auto module = wasm::Module::obtain(store, shared_module);
  wasm::Extern* imports[] = {memory.get()};
  auto instance = wasm::Instance::make(store, module.get(), imports);
  if (!instance) {
    std::cout << "> Error instantiating module!" << std::endl;
    exit(1);
  }

  // Extract export.
  std::cout << "Extracting exports..." << std::endl;
  auto exports = instance->exports();
  fn(memory.get(), exports);

  // Done.
  std::cout << "Ending run " << run << "..." << std::endl;
}


void run() {
  // Initialize.
  std::cout << "Initializing..." << std::endl;
  auto engine = wasm::Engine::make();
  auto shared_module = compile(engine.get());

  truncate(data_file, 0);  // in case it still exists

  // Run 1.
  auto pages = 2;
  auto run1 = [&](
    wasm::Memory* memory, const wasm::ownvec<wasm::Extern>& exports
  ) {
    // Extract export.
    size_t i = 0;
    auto size_func = get_export_func(exports, i++);
    auto load_func = get_export_func(exports, i++);
    auto store_func = get_export_func(exports, i++);
    auto grow_func = get_export_func(exports, i++);

    // Try cloning.
    assert(memory->copy()->same(memory));

    // Check initial memory.
    std::cout << "Checking memory..." << std::endl;
    check(memory->size(), 2u);
    check(memory->data_size(), 0x20000u);
    check(memory->data()[0], 0);
    check(memory->data()[0x1000], 0);
    check(memory->data()[0x1003], 0);

    check(call(size_func), 2);
    check(call(load_func, 0), 0);
    check(call(load_func, 0x1000), 0);
    check(call(load_func, 0x1003), 0);
    check(call(load_func, 0x1ffff), 0);
    check_trap(load_func, 0x20000);

    // Mutate memory.
    std::cout << "Mutating memory..." << std::endl;
    memory->data()[0x1003] = 5;
    check_ok(store_func, 0x1002, 6);
    check_trap(store_func, 0x20000, 0);

    check(memory->data()[0x1002], 6);
    check(memory->data()[0x1003], 5);
    check(call(load_func, 0x1002), 6);
    check(call(load_func, 0x1003), 5);

    // Grow memory.
    std::cout << "Growing memory..." << std::endl;
    check(memory->grow(1), true);
    check(memory->size(), 3u);
    check(memory->data_size(), 0x30000u);

    check_ok(store_func, 0x20000, 7);
    memory->data()[0x20001] = 8;
    check(call(load_func, 0x20000), 7);
    check(call(load_func, 0x20001), 8);

    check_trap(load_func, 0x30000);
    check_trap(store_func, 0x30000, 0);

    check(memory->grow(0), true);

    check(call(grow_func, 2), 3);
    check(memory->size(), 5u);

    check_ok(store_func, 0x40000, 10);
    memory->data()[0x40001] = 11;
    check(call(load_func, 0x40000), 10);
    check(call(load_func, 0x40001), 11);

    check_trap(load_func, 0x50000);
    check_trap(store_func, 0x50000, 0);
  };
  execute(engine.get(), shared_module.get(), pages, 1, run1);

  // Run 2.
  pages = 5;
  auto run2 = [&](
    wasm::Memory* memory, const wasm::ownvec<wasm::Extern>& exports
  ) {
    size_t i = 0;
    auto size_func = get_export_func(exports, i++);
    auto load_func = get_export_func(exports, i++);
    auto store_func = get_export_func(exports, i++);

    // Check persisted memory.
    std::cout << "Checking memory..." << std::endl;
    check(memory->size(), 5u);
    check(memory->data_size(), 0x50000u);
    check(call(size_func), 5);

    check(memory->data()[0], 0);
    check(memory->data()[0x1002], 6);
    check(memory->data()[0x1003], 5);
    check(call(load_func, 0x1002), 6);
    check(call(load_func, 0x1003), 5);
    check(call(load_func, 0x20000), 7);
    check(call(load_func, 0x20001), 8);
    check(call(load_func, 0x40000), 10);
    check(call(load_func, 0x40001), 11);

    check_ok(store_func, 0x40002, 12);
    memory->data()[0x40003] = 13;
    check(call(load_func, 0x40002), 12);
    check(call(load_func, 0x40003), 13);

    check_trap(load_func, 0x50000);
    check_trap(store_func, 0x50000, 0);
  };
  execute(engine.get(), shared_module.get(), pages, 2, run2);

  // Cleaning up.
  if (remove(data_file) == -1) {
    std::cout << "> Error removing memory file! errno = " << errno
      << " (" << strerror(errno) << ")" << std::endl;
    exit(1);
  }

  // Shut down.
  std::cout << "Shutting down..." << std::endl;
}


int main(int argc, const char* argv[]) {
  run();
  assert(mem_count == 0);
  std::cout << "Done." << std::endl;
  return 0;
}
