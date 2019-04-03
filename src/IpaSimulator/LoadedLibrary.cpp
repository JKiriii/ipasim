// LoadedLibrary.cpp

#include "ipasim/LoadedLibrary.hpp"

#include "ipasim/Common.hpp"
#include "ipasim/DynamicLoader.hpp"

using namespace ipasim;
using namespace std;

namespace {

struct method_t {
  const char *name;
  const char *types;
  void *imp;
};

struct method_list_t {
  uint32_t entrysize;
  uint32_t count;
  method_t methods[0];
};

#define FAST_DATA_MASK 0xfffffffcUL
#define RW_REALIZED (1 << 31)

struct class_ro_t {
  uint32_t flags;
  uint32_t instanceStart;
  uint32_t instanceSize;

  const uint8_t *ivarLayout;

  const char *name;
  method_list_t *baseMethodList;
  void *baseProtocols;
  const void *ivars;

  const uint8_t *weakIvarLayout;
  void *baseProperties;
};

template <typename Element, typename List> class list_array_tt {
  struct array_t {
    uint32_t count;
    List *lists[0];
  };

private:
  union {
    List *list;
    uintptr_t arrayAndFlag;
  };

  bool hasArray() const { return arrayAndFlag & 1; }
  array_t *array() { return (array_t *)(arrayAndFlag & ~1); }

public:
  List **beginLists() {
    if (hasArray()) {
      return array()->lists;
    } else {
      return &list;
    }
  }

  List **endLists() {
    if (hasArray()) {
      return array()->lists + array()->count;
    } else if (list) {
      return &list + 1;
    } else {
      return &list;
    }
  }
};

using method_array_t = list_array_tt<method_t, method_list_t>;

struct class_rw_t {
  uint32_t flags;
  uint32_t version;

  const class_ro_t *ro;

  method_array_t methods;
  /*
  property_array_t properties;
  protocol_array_t protocols;

  Class firstSubclass;
  Class nextSiblingClass;

  char *demangledName;
  */
};

struct objc_class {
  objc_class *isa;
  void *superclass;
  void *cache;
  void *vtable;
  class_ro_t *info;

  class_rw_t *data() {
    return (class_rw_t *)((uintptr_t)info & FAST_DATA_MASK);
  }
  bool isRealized() { return data()->flags & RW_REALIZED; }
  const class_ro_t *getInfo() { return isRealized() ? data()->ro : info; }
};

struct category_t {
  const char *name;
  objc_class *cls;
  method_list_t *instanceMethods;
  method_list_t *classMethods;

  /*
  struct protocol_list_t *protocols;
  struct property_list_t *instanceProperties;
  // Fields below this point are not always present on disk.
  struct property_list_t *_classProperties;
  */
};

} // namespace

static const char *findMethod(method_list_t *Methods, uint64_t Addr) {
  if (!Methods)
    return nullptr;
  for (size_t J = 0; J != Methods->count; ++J) {
    method_t &Method = Methods->methods[J];
    if (reinterpret_cast<uint64_t>(Method.imp) == Addr)
      return Method.types;
  }
  return nullptr;
}

static const char *findMethod(objc_class *Class, uint64_t Addr) {
  // TODO: Isn't this first part redundant for realized classes?
  if (const char *T = findMethod(Class->getInfo()->baseMethodList, Addr))
    return T;
  if (Class->isRealized())
    for (auto *L = Class->data()->methods.beginLists(),
              *End = Class->data()->methods.endLists();
         L != End; ++L)
      if (const char *T = findMethod(*L, Addr))
        return T;
  return nullptr;
}

const char *LoadedLibrary::getClassOfMethod(uint64_t Addr) {
  // Enumerate classes in the image.
  uint64_t SecSize;
  uint64_t SecAddr = getSection("__objc_classlist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return Class->getInfo()->name;
      if (const char *T = findMethod(Class->isa, Addr))
        return Class->isa->getInfo()->name;
    }
  }

  // Try also non-lazy classes.
  SecAddr = getSection("__objc_nlclslist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return Class->getInfo()->name;
      if (const char *T = findMethod(Class->isa, Addr))
        return Class->isa->getInfo()->name;
    }
  }

  return nullptr;
}

const char *LoadedLibrary::getMethodType(uint64_t Addr) {
  // Enumerate classes in the image.
  uint64_t SecSize;
  uint64_t SecAddr = getSection("__objc_classlist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return T;
      if (const char *T = findMethod(Class->isa, Addr))
        return T;
    }
  }

  // Try also categories.
  SecAddr = getSection("__objc_catlist", &SecSize);
  if (SecAddr) {
    auto *Categories = reinterpret_cast<category_t **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every category.
      category_t *Category = Categories[I];
      if (const char *T = findMethod(Category->classMethods, Addr))
        return T;
      if (const char *T = findMethod(Category->instanceMethods, Addr))
        return T;
    }
  }

  return nullptr;
}

bool LoadedLibrary::isInRange(uint64_t Addr) {
  return StartAddress <= Addr && Addr < StartAddress + Size;
}

void LoadedLibrary::checkInRange(uint64_t Addr) {
  // TODO: Do more flexible error reporting here.
  if (!isInRange(Addr))
    throw "address out of range";
}

uint64_t LoadedDylib::findSymbol(DynamicLoader &DL, const string &Name) {
  using namespace LIEF::MachO;

  if (!Bin.has_symbol(Name)) {
    // Try also re-exported libraries.
    for (DylibCommand &Lib : Bin.libraries()) {
      if (Lib.command() != LOAD_COMMAND_TYPES::LC_REEXPORT_DYLIB)
        continue;

      LoadedLibrary *LL = DL.load(Lib.name());
      if (!LL)
        continue;

      // If the target library is DLL, it doesn't have underscore prefixes, so
      // we need to remove it.
      uint64_t SymAddr;
      if (!LL->hasUnderscorePrefix() && Name[0] == '_')
        SymAddr = LL->findSymbol(DL, Name.substr(1));
      else
        SymAddr = LL->findSymbol(DL, Name);

      if (SymAddr)
        return SymAddr;
    }
    return 0;
  }
  return StartAddress + Bin.get_symbol(Name).value();
}

uint64_t LoadedDylib::getSection(const std::string &Name, uint64_t *Size) {
  using namespace LIEF::MachO;

  if (!Bin.has_section(Name))
    return 0;

  Section &Sect = Bin.get_section(Name);
  if (Size)
    *Size = Sect.size();
  return Sect.address() + StartAddress;
}

uint64_t LoadedDll::getSection(const std::string &Name, uint64_t *Size) {
  using namespace LIEF::MachO;

  // We only support DLLs with `_mh_dylib_header`.
  if (!MachOPoser)
    return 0;

  // Enumerate segments.
  uint64_t Slide, Addr;
  bool HasSlide = false, HasAddr = false;
  auto Header = reinterpret_cast<const mach_header *>(StartAddress);
  auto Cmd = reinterpret_cast<const load_command *>(Header + 1);
  for (size_t I = 0; I != Header->ncmds; ++I) {
    if (Cmd->cmd == (uint32_t)LOAD_COMMAND_TYPES::LC_SEGMENT) {
      auto Seg = reinterpret_cast<const segment_command_32 *>(Cmd);

      // Look for segment `__TEXT` to compute slide.
      if (!HasSlide && !strncmp(Seg->segname, "__TEXT", 16)) {
        Slide = StartAddress - Seg->vmaddr;
        HasSlide = true;
        if (HasAddr)
          break;
      }

      // Enumerate segment's sections.
      if (!HasAddr) {
        auto Sect = reinterpret_cast<const section_32 *>(
            bytes(Cmd) + sizeof(segment_command_32));
        for (size_t J = 0; J != Seg->nsects; ++J) {
          // Note that `Sect->sectname` is not necessarily null-terminated!
          if (!strncmp(Sect->sectname, Name.c_str(), 16)) {
            // We have found it.
            if (Size)
              *Size = Sect->size;
            Addr = Sect->addr;
            HasAddr = true;
            if (HasSlide)
              break;
          }

          // Move to the next `section`.
          Sect = reinterpret_cast<const section_32 *>(bytes(Sect) +
                                                      sizeof(section_32));
        }
      }
    }

    // Move to the next `load_command`.
    Cmd = reinterpret_cast<const load_command *>(bytes(Cmd) + Cmd->cmdsize);
  }

  if (HasSlide && HasAddr)
    return Addr + Slide;
  return 0;
}

uint64_t LoadedDll::findSymbol(DynamicLoader &DL, const string &Name) {
  return (uint64_t)GetProcAddress(Ptr, Name.c_str());
}